/*
 * ATBM6441 on a camera SoC's SDIO bus, the chip in another QEMU
 * (protocol and roles: include/hw/misc/atbm-hera-link.h)
 *
 * This is the SDIO card the camera's QEMU plugs on its host (-device
 * atbm-hera-link-sdio,bus=msc1,chardev=hera). Function 1 and the vendor
 * area of function 0 are the chip's: each access is forwarded to the chip's
 * QEMU, which runs the firmware, and the vCPU waits for the answer. The
 * rest of function 0 (the CCCR and the FBRs) the card keeps, and tells the
 * chip of every write to it. The SDIO interrupt is the chip's.
 *
 * The SoC's GPIOs: "power-pin" and "reset-pin" (the SoC's pin numbers,
 * 32 * port + bit) are what the SoC powers the chip and resets it with,
 * high on "power-pin" powers it and low on "reset-pin" holds it in reset;
 * "from-chip" ("pad:pin,...") drives SoC pins from chip pads (a wake line),
 * "to-chip" ("pin:pad,...") the chip's pads from SoC pins (the PIR, the
 * charger). The same lines are named gpios of the card for a test: input
 * "power", "reset" and "pad-in" 0..27, output "pad-out" 0..27.
 *
 * "on-chip-reset" is what the card does when the chip says that it has reset
 * (the board's watchdog or reset request): "ignore" does nothing; "soc-reset"
 * resets the whole SoC, as the board does when the chip's watchdog cuts the
 * SoC's supply; "card-reset" takes the card off the bus and puts it back
 * after "reinsert-ms" of virtual time (the host's card-detect line moves,
 * and "cd-pin" is a SoC pin that follows it, low while the card is in), as
 * a socket does, so that the kernel enumerates it again. The chip's first
 * message after it starts is not acted on in any mode.
 *
 * "host-power-pad" is a chip pad that switches the SoC's power, as the
 * SDK's SDIO demo can use its wakeup_host_gpio (pad 25): low stops the SoC
 * (its vCPUs and clocks, a supply cut), high cold-boots it. The chip drives
 * it low at its boot until something wakes it (its wake pad 24: "to-chip"
 * or "pad24"), so with it the camera only runs while the chip wants it.
 *
 * -device atbm6441 is the model of the chip's behaviour that needs no
 * second QEMU; this card never changes what that one does.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qapi/visitor.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/gpio/ingenic-gpio.h"
#include "hw/misc/atbm-hera-link.h"
#include "hw/sd/sdio.h"
#include "system/runstate.h"
#include "migration/blocker.h"
#include "trace.h"

#define TYPE_ATBM_HERA_LINK_SDIO "atbm-hera-link-sdio"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraLinkSdioState, ATBM_HERA_LINK_SDIO)

enum {
    CHIP_RESET_IGNORE,
    CHIP_RESET_CARD,
    CHIP_RESET_SOC,
};

struct AtbmHeraLinkSdioState {
    SDIOCard parent_obj;

    CharFrontend chr;
    Error *migration_blocker;
    DeviceState *gpio;
    int32_t power_pin;
    int32_t reset_pin;
    char *from_chip;
    char *to_chip;
    char *on_chip_reset;
    int32_t cd_pin;
    int32_t host_power_pad;
    bool soc_off;               /* the chip's power pad has the SoC off */
    uint32_t reinsert_ms;
    int chip_reset;             /* CHIP_RESET_* */
    QEMUTimer *reinsert;
    QEMUBH *remove_bh;
    bool absent;                /* taken off the bus by a chip reset */
    bool chip_irq;
    qemu_irq card_detect;

    bool up;                    /* the chip has said hello */
    AtbmHeraLinkMsg rx;
    uint32_t rx_len;
    bool power;
    bool power_set;
    bool reset;
    bool reset_set;
    uint32_t pad_driven;
    uint32_t pad_level;
    qemu_irq pad_out[ATBM_HERA_LINK_PADS];
};

static void hl_send(AtbmHeraLinkSdioState *s, uint32_t type, uint32_t a,
                    uint32_t b, uint32_t c, const void *payload,
                    uint32_t len)
{
    AtbmHeraLinkMsg m = {
        .type = cpu_to_le32(type), .a = cpu_to_le32(a),
        .b = cpu_to_le32(b), .c = cpu_to_le32(c),
    };

    if (!s->up) {
        return;
    }
    trace_atbm_hera_link_sdio_tx(type, a, b, c);
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)&m, sizeof(m));
    if (len) {
        qemu_chr_fe_write_all(&s->chr, payload, len);
    }
}

/* From a bottom half: the message comes in the middle of a transfer */
static void hl_card_remove(void *opaque)
{
    AtbmHeraLinkSdioState *s = opaque;
    SDBus *bus = SD_BUS(qdev_get_parent_bus(DEVICE(s)));

    /* the card starts over, and the chip is told of the bus reset */
    device_cold_reset(DEVICE(s));
    sdbus_set_inserted(bus, false);
    qemu_set_irq(s->card_detect, 0);
    timer_mod(s->reinsert, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                           s->reinsert_ms);
}

static void hl_card_reinsert(void *opaque)
{
    AtbmHeraLinkSdioState *s = opaque;
    SDBus *bus = SD_BUS(qdev_get_parent_bus(DEVICE(s)));

    s->absent = false;
    sdbus_set_inserted(bus, true);
    qemu_set_irq(s->card_detect, 1);
    sdio_card_set_irq(SDIO_CARD(s), 1, s->chip_irq);
}

/*
 * The chip pad that switches the SoC's supply (the SDIO demo's
 * wakeup_host_gpio, which AN9406 allows to be the host's power switch):
 * low stops the SoC, as if unpowered (its clocks stop with it), high
 * cold-boots it
 */
static void hl_host_power(AtbmHeraLinkSdioState *s, bool on)
{
    if (on == !s->soc_off) {
        return;
    }
    trace_atbm_hera_link_sdio_host_power(on);
    s->soc_off = !on;
    if (on) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        vm_start();
    } else {
        vm_stop(RUN_STATE_PAUSED);
    }
}

static void hl_chip_reset(AtbmHeraLinkSdioState *s, uint32_t reason)
{
    trace_atbm_hera_link_sdio_chip_reset(reason, s->chip_reset);
    if (reason == AHL_CHIP_RESET_COLD) {
        return;
    }
    switch (s->chip_reset) {
    case CHIP_RESET_SOC:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
    case CHIP_RESET_CARD:
        s->absent = true;
        qemu_bh_schedule(s->remove_bh);
        break;
    default:
        break;
    }
}

/* The chip's messages that need no answer */
static void hl_message(AtbmHeraLinkSdioState *s, const AtbmHeraLinkMsg *m)
{
    uint32_t a = le32_to_cpu(m->a), c = le32_to_cpu(m->c);
    uint32_t n;

    trace_atbm_hera_link_sdio_rx(le32_to_cpu(m->type), a,
                                 le32_to_cpu(m->b), c);
    switch (le32_to_cpu(m->type)) {
    case AHL_HELLO:
        if (c != ATBM_HERA_LINK_VERSION) {
            error_report("atbm-hera-link-sdio: the chip speaks link version "
                         "%u, not %u", c, ATBM_HERA_LINK_VERSION);
            qemu_chr_fe_disconnect(&s->chr);
            return;
        }
        s->up = true;
        hl_send(s, AHL_HELLO, ATBM_HERA_LINK_CAP_CHIP_RESET, 0,
                ATBM_HERA_LINK_VERSION, NULL, 0);
        if (s->power_set) {
            hl_send(s, AHL_POWER, 0, 0, s->power, NULL, 0);
        }
        if (s->reset_set) {
            hl_send(s, AHL_RESET, 0, 0, s->reset, NULL, 0);
        }
        for (n = 0; n < ATBM_HERA_LINK_PADS; n++) {
            if (s->pad_driven & BIT(n)) {
                hl_send(s, AHL_PAD, n, 0, extract32(s->pad_level, n, 1),
                        NULL, 0);
            }
        }
        break;
    case AHL_IRQ:
        s->chip_irq = !!c;
        if (!s->absent) {
            sdio_card_set_irq(SDIO_CARD(s), 1, !!c);
        }
        break;
    case AHL_CHIP_RESET:
        hl_chip_reset(s, a);
        break;
    case AHL_PAD:
        if (a < ATBM_HERA_LINK_PADS) {
            qemu_set_irq(s->pad_out[a], !!c);
        }
        if ((int)a == s->host_power_pad) {
            hl_host_power(s, c);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-link-sdio: unexpected "
                      "message %u from the chip\n", le32_to_cpu(m->type));
        break;
    }
}

static void hl_lost(AtbmHeraLinkSdioState *s)
{
    s->up = false;
    s->rx_len = 0;
    sdio_card_set_irq(SDIO_CARD(s), 1, false);
}

static bool hl_read_all(AtbmHeraLinkSdioState *s, void *buf, uint32_t len)
{
    if (qemu_chr_fe_read_all(&s->chr, buf, len) != len) {
        hl_lost(s);
        return false;
    }
    return true;
}

/*
 * Wait for the answer to the request just sent: DATA fills buf, ACK needs
 * nothing. The messages before it are the chip's own, taken as they come.
 */
static bool hl_answer(AtbmHeraLinkSdioState *s, uint32_t want, uint8_t *buf,
                      uint32_t len)
{
    for (;;) {
        AtbmHeraLinkMsg m;
        uint32_t type;

        if (!hl_read_all(s, &m, sizeof(m))) {
            return false;
        }
        type = le32_to_cpu(m.type);
        if (type != want) {
            hl_message(s, &m);
            continue;
        }
        if (type == AHL_DATA) {
            if (le32_to_cpu(m.c) != len) {
                error_report("atbm-hera-link-sdio: the chip answered %u "
                             "bytes of %u", le32_to_cpu(m.c), len);
                hl_lost(s);
                return false;
            }
            return !len || hl_read_all(s, buf, len);
        }
        return true;
    }
}

static void hl_read(SDIOCard *card, unsigned fn, uint32_t addr, bool incr,
                    uint8_t *buf, size_t len)
{
    AtbmHeraLinkSdioState *s = ATBM_HERA_LINK_SDIO(card);

    memset(buf, 0, len);
    if (!s->up || s->absent || len > ATBM_HERA_LINK_MAX_XFER) {
        return;
    }
    hl_send(s, AHL_SDIO_READ, fn, addr,
            len | (incr ? ATBM_HERA_LINK_INCR : 0), NULL, 0);
    hl_answer(s, AHL_DATA, buf, len);
}

static void hl_write(SDIOCard *card, unsigned fn, uint32_t addr, bool incr,
                     const uint8_t *buf, size_t len)
{
    AtbmHeraLinkSdioState *s = ATBM_HERA_LINK_SDIO(card);

    if (!s->up || s->absent || len > ATBM_HERA_LINK_MAX_XFER) {
        return;
    }
    hl_send(s, AHL_SDIO_WRITE, fn, addr,
            len | (incr ? ATBM_HERA_LINK_INCR : 0), buf, len);
    hl_answer(s, AHL_ACK, NULL, 0);
}

static uint8_t hl_vendor_read(SDIOCard *card, uint32_t addr)
{
    uint8_t v;

    hl_read(card, 0, addr, false, &v, 1);
    return v;
}

static void hl_vendor_write(SDIOCard *card, uint32_t addr, uint8_t val)
{
    hl_write(card, 0, addr, false, &val, 1);
}

static void hl_f0_write(SDIOCard *card, uint32_t addr, uint8_t val)
{
    AtbmHeraLinkSdioState *s = ATBM_HERA_LINK_SDIO(card);

    if (!s->absent) {
        hl_send(s, AHL_SDIO_CFG, addr, 0, val, NULL, 0);
    }
}

static void hl_io_reset(SDIOCard *card)
{
    hl_send(ATBM_HERA_LINK_SDIO(card), AHL_SDIO_RESET, 0, 0, 0, NULL, 0);
}

static int hl_can_read(void *opaque)
{
    AtbmHeraLinkSdioState *s = opaque;

    return sizeof(s->rx) - s->rx_len;
}

static void hl_receive(void *opaque, const uint8_t *buf, int size)
{
    AtbmHeraLinkSdioState *s = opaque;

    memcpy((uint8_t *)&s->rx + s->rx_len, buf, size);
    s->rx_len += size;
    if (s->rx_len == sizeof(s->rx)) {
        s->rx_len = 0;
        hl_message(s, &s->rx);
    }
}

static void hl_event(void *opaque, QEMUChrEvent event)
{
    AtbmHeraLinkSdioState *s = opaque;

    if (event == CHR_EVENT_CLOSED) {
        hl_lost(s);
    }
}

static void hl_power_in(void *opaque, int n, int level)
{
    AtbmHeraLinkSdioState *s = opaque;

    s->power = !!level;
    s->power_set = true;
    hl_send(s, AHL_POWER, 0, 0, s->power, NULL, 0);
}

/* The pin is low while the chip is held in reset */
static void hl_reset_in(void *opaque, int n, int level)
{
    AtbmHeraLinkSdioState *s = opaque;

    s->reset = !level;
    s->reset_set = true;
    hl_send(s, AHL_RESET, 0, 0, s->reset, NULL, 0);
}

static void hl_pad_get(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    AtbmHeraLinkSdioState *s = ATBM_HERA_LINK_SDIO(obj);
    unsigned n = GPOINTER_TO_UINT(opaque);
    int64_t val = -1;

    if (s->pad_driven & BIT(n)) {
        val = extract32(s->pad_level, n, 1);
    }
    visit_type_int(v, name, &val, errp);
}

static void hl_pad_in(void *opaque, int n, int level);

static void hl_pad_set(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    AtbmHeraLinkSdioState *s = ATBM_HERA_LINK_SDIO(obj);
    unsigned n = GPOINTER_TO_UINT(opaque);
    int64_t val;

    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    if (val < 0 || val > 1) {
        error_setg(errp, "a pad is driven to 0 or 1; it cannot be let go");
        return;
    }
    hl_pad_in(s, n, val);
}

static void hl_pad_in(void *opaque, int n, int level)
{
    AtbmHeraLinkSdioState *s = opaque;

    s->pad_driven |= BIT(n);
    s->pad_level = deposit32(s->pad_level, n, 1, !!level);
    hl_send(s, AHL_PAD, n, 0, !!level, NULL, 0);
}

/* "a:b,a:b": the pairs of numbers of a "from-chip" or "to-chip" property */
static bool hl_wire_list(AtbmHeraLinkSdioState *s, const char *list,
                         bool from_chip, Error **errp)
{
    g_auto(GStrv) pairs = g_strsplit(list ?: "", ",", -1);

    for (char **p = pairs; *p && **p; p++) {
        g_auto(GStrv) v = g_strsplit(*p, ":", 2);
        int first, second, chip_pad, soc_pin;

        if (!v[0] || !v[1] || qemu_strtoi(v[0], NULL, 0, &first) ||
            qemu_strtoi(v[1], NULL, 0, &second)) {
            error_setg(errp, "\"%s\" is not a pair of numbers", *p);
            return false;
        }
        chip_pad = from_chip ? first : second;
        soc_pin = from_chip ? second : first;
        if (chip_pad < 0 || chip_pad >= ATBM_HERA_LINK_PADS || soc_pin < 0 ||
            soc_pin >= INGENIC_GPIO_MAX_PORTS * 32) {
            error_setg(errp, "pad %d or pin %d is out of range", chip_pad,
                       soc_pin);
            return false;
        }
        if (from_chip) {
            qdev_connect_gpio_out_named(DEVICE(s), "pad-out", chip_pad,
                qdev_get_gpio_in_named(s->gpio, "pull", soc_pin));
        } else {
            qdev_connect_gpio_out_named(s->gpio, "pad", soc_pin,
                qdev_get_gpio_in_named(DEVICE(s), "pad-in", chip_pad));
        }
    }
    return true;
}

static bool hl_wire(AtbmHeraLinkSdioState *s, Error **errp)
{
    bool ambiguous;
    Object *o;

    if (s->power_pin < 0 && s->reset_pin < 0 && s->cd_pin < 0 &&
        !s->from_chip && !s->to_chip) {
        return true;
    }
    o = object_resolve_path_type("", TYPE_INGENIC_GPIO, &ambiguous);
    if (!o || ambiguous) {
        error_setg(errp, "the SoC has no single GPIO block to wire the "
                   "chip's pins to");
        return false;
    }
    s->gpio = DEVICE(o);
    if (s->power_pin >= 0) {
        qdev_connect_gpio_out_named(s->gpio, "pad", s->power_pin,
            qdev_get_gpio_in_named(DEVICE(s), "power", 0));
    }
    if (s->reset_pin >= 0) {
        qdev_connect_gpio_out_named(s->gpio, "pad", s->reset_pin,
            qdev_get_gpio_in_named(DEVICE(s), "reset", 0));
    }
    if (s->cd_pin >= 0) {
        qdev_connect_gpio_out_named(DEVICE(s), "card-detect", 0,
            qemu_irq_invert(qdev_get_gpio_in_named(s->gpio, "pull",
                                                   s->cd_pin)));
    }
    return hl_wire_list(s, s->from_chip, true, errp) &&
           hl_wire_list(s, s->to_chip, false, errp);
}

static void hl_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraLinkSdioState *s = ATBM_HERA_LINK_SDIO(dev);
    AtbmHeraLinkMsg hello;

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp, "the \"chardev\" of the link to the chip's QEMU "
                   "is not set");
        return;
    }
    if (!s->on_chip_reset || !strcmp(s->on_chip_reset, "ignore")) {
        s->chip_reset = CHIP_RESET_IGNORE;
    } else if (!strcmp(s->on_chip_reset, "card-reset")) {
        s->chip_reset = CHIP_RESET_CARD;
    } else if (!strcmp(s->on_chip_reset, "soc-reset")) {
        s->chip_reset = CHIP_RESET_SOC;
    } else {
        error_setg(errp, "\"on-chip-reset\" is ignore, card-reset or "
                   "soc-reset, not \"%s\"", s->on_chip_reset);
        return;
    }
    s->reinsert = timer_new_ms(QEMU_CLOCK_VIRTUAL, hl_card_reinsert, s);
    s->remove_bh = qemu_bh_new(hl_card_remove, s);
    if (!hl_wire(s, errp)) {
        return;
    }
    error_setg(&s->migration_blocker, "atbm-hera-link-sdio: the chip runs "
               "in another QEMU, whose state is not saved with this one");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        return;
    }
    /* the chip speaks first, as it accepts the connection */
    if (qemu_chr_fe_read_all(&s->chr, (uint8_t *)&hello,
                             sizeof(hello)) != sizeof(hello) ||
        le32_to_cpu(hello.type) != AHL_HELLO) {
        error_setg(errp, "the chip's QEMU did not say hello (start it "
                   "first, with its link chardev a server)");
        return;
    }
    qemu_set_irq(s->card_detect, 1);
    hl_message(s, &hello);
    qemu_chr_fe_set_handlers(&s->chr, hl_can_read, hl_receive, hl_event,
                             NULL, s, NULL, true);
}

static void hl_unrealize(DeviceState *dev)
{
    AtbmHeraLinkSdioState *s = ATBM_HERA_LINK_SDIO(dev);

    qemu_chr_fe_deinit(&s->chr, false);
    timer_free(s->reinsert);
    qemu_bh_delete(s->remove_bh);
}

static void hl_init(Object *obj)
{
    AtbmHeraLinkSdioState *s = ATBM_HERA_LINK_SDIO(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_in_named(dev, hl_power_in, "power", 1);
    qdev_init_gpio_in_named(dev, hl_reset_in, "reset", 1);
    qdev_init_gpio_in_named(dev, hl_pad_in, "pad-in", ATBM_HERA_LINK_PADS);
    qdev_init_gpio_out_named(dev, s->pad_out, "pad-out", ATBM_HERA_LINK_PADS);
    qdev_init_gpio_out_named(dev, &s->card_detect, "card-detect", 1);
    for (unsigned n = 0; n < ATBM_HERA_LINK_PADS; n++) {
        g_autofree char *name = g_strdup_printf("pad%u", n);

        object_property_add(obj, name, "int", &int_type_info, hl_pad_get,
                            hl_pad_set, NULL,
                            GUINT_TO_POINTER(n));
    }
}

static const Property hl_props[] = {
    DEFINE_PROP_CHR("chardev", AtbmHeraLinkSdioState, chr),
    DEFINE_PROP_INT32("power-pin", AtbmHeraLinkSdioState, power_pin, -1),
    DEFINE_PROP_INT32("reset-pin", AtbmHeraLinkSdioState, reset_pin, -1),
    DEFINE_PROP_STRING("from-chip", AtbmHeraLinkSdioState, from_chip),
    DEFINE_PROP_STRING("to-chip", AtbmHeraLinkSdioState, to_chip),
    DEFINE_PROP_STRING("on-chip-reset", AtbmHeraLinkSdioState, on_chip_reset),
    DEFINE_PROP_INT32("cd-pin", AtbmHeraLinkSdioState, cd_pin, -1),
    DEFINE_PROP_INT32("host-power-pad", AtbmHeraLinkSdioState,
                      host_power_pad, -1),
    DEFINE_PROP_UINT32("reinsert-ms", AtbmHeraLinkSdioState, reinsert_ms, 200),
};

static void hl_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SDIOCardClass *sc = SDIO_CARD_CLASS(oc);

    dc->realize = hl_realize;
    dc->unrealize = hl_unrealize;
    dc->desc = "AltoBeam ATBM6441 SDIO slave, the chip in another QEMU";
    device_class_set_props(dc, hl_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);

    /* the IDs the vendor's driver binds to, as atbm-hera-sdio has them */
    sc->vendor = 0x007a;
    sc->device = 0x6011;
    sc->func_class = 0x07;
    sc->num_funcs = 1;
    sc->max_blksz = 512;
    sc->read = hl_read;
    sc->write = hl_write;
    sc->io_reset = hl_io_reset;
    sc->f0_write = hl_f0_write;
    sc->vendor_read = hl_vendor_read;
    sc->vendor_write = hl_vendor_write;
}

static const TypeInfo hl_info = {
    .name = TYPE_ATBM_HERA_LINK_SDIO,
    .parent = TYPE_SDIO_CARD,
    .instance_size = sizeof(AtbmHeraLinkSdioState),
    .instance_init = hl_init,
    .class_init = hl_class_init,
};

static void hl_register_types(void)
{
    type_register_static(&hl_info);
}

type_init(hl_register_types)
