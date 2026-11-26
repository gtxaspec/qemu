/*
 * ATBM6441 "Hera": the chip's end of the link to a camera SoC's QEMU
 * (protocol and roles: include/hw/misc/atbm-hera-link.h)
 *
 * The SoC's SDIO card forwards each function 1 access, and this serves it
 * from the HIF block through its two calls; the HIF's interrupt to the host
 * goes back as IRQ. The chip's GPIO block shares its pads with the SoC
 * ("pad-out" in from the GPIO's out lines, "pad-in" out to its pin
 * inputs), and the SoC powers the chip and holds its reset: "hold" is high
 * while the chip is off or in reset, and "reset" pulses on power-up, when
 * the board resets every device as a power cycle does.
 *
 * Unlinked (no "chardev"), the device does nothing. A message that is not
 * for the chip is logged and dropped. While the chip is powered off the
 * SDIO function answers zeros.
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
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/atbm-hera-hif.h"
#include "hw/misc/atbm-hera-link.h"
#include "migration/blocker.h"
#include "trace.h"

/* function 1's block size in its FBR, two bytes */
#define FBR1_BLKSIZE    0x110

#define TYPE_ATBM_HERA_LINK "atbm-hera-link"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraLinkState, ATBM_HERA_LINK)

struct AtbmHeraLinkState {
    DeviceState parent_obj;

    CharFrontend chr;
    AtbmHeraHifState *hif;
    Error *migration_blocker;

    bool hello;                 /* the SoC has said hello */
    bool powered;
    bool reset_held;
    AtbmHeraLinkMsg rx;
    uint8_t *payload;           /* of the SDIO_WRITE being received */
    uint32_t pay_len;
    uint32_t rx_len;            /* bytes of the header or payload so far */
    uint32_t pad_seen;
    uint32_t pad_level;         /* what the chip drives on each pad */
    uint16_t blksz;             /* function 1's, as the SoC set it */

    qemu_irq pad_in[ATBM_HERA_LINK_PADS];
    qemu_irq hold;
    qemu_irq reset;
};

static bool link_up(AtbmHeraLinkState *s)
{
    return s->hello && qemu_chr_fe_backend_connected(&s->chr);
}

static void link_send(AtbmHeraLinkState *s, uint32_t type, uint32_t a,
                      uint32_t b, uint32_t c, const void *payload,
                      uint32_t len)
{
    AtbmHeraLinkMsg m = {
        .type = cpu_to_le32(type), .a = cpu_to_le32(a),
        .b = cpu_to_le32(b), .c = cpu_to_le32(c),
    };

    trace_atbm_hera_link_tx(type, a, b, c);
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)&m, sizeof(m));
    if (len) {
        qemu_chr_fe_write_all(&s->chr, payload, len);
    }
}

static void link_host_irq(void *opaque, int n, int level)
{
    AtbmHeraLinkState *s = opaque;

    if (link_up(s)) {
        link_send(s, AHL_IRQ, 0, 0, level, NULL, 0);
    }
}

static void link_pad_out(void *opaque, int n, int level)
{
    AtbmHeraLinkState *s = opaque;

    s->pad_seen |= BIT(n);
    s->pad_level = deposit32(s->pad_level, n, 1, !!level);
    if (link_up(s)) {
        link_send(s, AHL_PAD, n, 0, !!level, NULL, 0);
    }
}

static void link_set_hold(AtbmHeraLinkState *s)
{
    qemu_set_irq(s->hold, !s->powered || s->reset_held);
}

static void link_hello(AtbmHeraLinkState *s)
{
    if (le32_to_cpu(s->rx.c) != ATBM_HERA_LINK_VERSION) {
        error_report("atbm-hera-link: the SoC speaks link version %u, "
                     "not %u", le32_to_cpu(s->rx.c),
                     ATBM_HERA_LINK_VERSION);
        qemu_chr_fe_disconnect(&s->chr);
        return;
    }
    s->hello = true;
    link_send(s, AHL_IRQ, 0, 0, atbm_hera_hif_host_irq(s->hif), NULL, 0);
    for (int n = 0; n < ATBM_HERA_LINK_PADS; n++) {
        if (s->pad_seen & BIT(n)) {
            link_send(s, AHL_PAD, n, 0, extract32(s->pad_level, n, 1),
                      NULL, 0);
        }
    }
}

static void link_sdio(AtbmHeraLinkState *s, const AtbmHeraLinkMsg *m,
                      const uint8_t *data)
{
    uint32_t fn = le32_to_cpu(m->a), addr = le32_to_cpu(m->b);
    uint32_t c = le32_to_cpu(m->c);
    uint32_t len = c & ~ATBM_HERA_LINK_INCR;
    bool incr = c & ATBM_HERA_LINK_INCR;
    g_autofree uint8_t *buf = NULL;

    if (le32_to_cpu(m->type) == AHL_SDIO_WRITE) {
        if (s->powered && fn == 1) {
            atbm_hera_hif_host_write(s->hif, addr, incr, data, len);
        } else if (fn != 1) {
            qemu_log_mask(LOG_UNIMP, "atbm-hera-link: write to function "
                          "%u address 0x%x dropped\n", fn, addr);
        }
        link_send(s, AHL_ACK, 0, 0, 0, NULL, 0);
        return;
    }
    buf = g_malloc0(len ?: 1);
    if (s->powered && fn == 1) {
        atbm_hera_hif_host_read(s->hif, addr, incr, buf, len);
    } else if (fn != 1) {
        qemu_log_mask(LOG_UNIMP, "atbm-hera-link: read of function %u "
                      "address 0x%x gives 0\n", fn, addr);
    }
    link_send(s, AHL_DATA, 0, 0, len, buf, len);
}

static void link_message(AtbmHeraLinkState *s, const AtbmHeraLinkMsg *m,
                         const uint8_t *data)
{
    uint32_t a = le32_to_cpu(m->a), c = le32_to_cpu(m->c);

    trace_atbm_hera_link_rx(le32_to_cpu(m->type), a, le32_to_cpu(m->b), c);
    switch (le32_to_cpu(m->type)) {
    case AHL_HELLO:
        link_hello(s);
        break;
    case AHL_SDIO_READ:
    case AHL_SDIO_WRITE:
        link_sdio(s, m, data);
        break;
    case AHL_SDIO_CFG:
        /* of the CCCR and the FBR, the HIF goes by one setting */
        if (a == FBR1_BLKSIZE) {
            s->blksz = (s->blksz & 0xff00) | (c & 0xff);
        } else if (a == FBR1_BLKSIZE + 1) {
            s->blksz = (s->blksz & 0x00ff) | (c & 0xff) << 8;
        } else {
            break;
        }
        atbm_hera_hif_set_block_size(s->hif, s->blksz);
        break;
    case AHL_SDIO_RESET:
        s->blksz = 0;
        atbm_hera_hif_set_block_size(s->hif, 0);
        break;
    case AHL_PAD:
        if (a < ATBM_HERA_LINK_PADS) {
            qemu_set_irq(s->pad_in[a], !!c);
        }
        break;
    case AHL_POWER:
        if (!!c != s->powered) {
            s->powered = !!c;
            link_set_hold(s);
            if (s->powered) {
                qemu_set_irq(s->reset, 1);
                qemu_set_irq(s->reset, 0);
            }
        }
        break;
    case AHL_RESET:
        s->reset_held = !!c;
        link_set_hold(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-link: unexpected message "
                      "%u from the SoC\n", le32_to_cpu(m->type));
        break;
    }
}

static int link_can_read(void *opaque)
{
    AtbmHeraLinkState *s = opaque;

    if (s->pay_len) {
        return s->pay_len - s->rx_len;
    }
    return sizeof(s->rx) - s->rx_len;
}

static void link_read(void *opaque, const uint8_t *buf, int size)
{
    AtbmHeraLinkState *s = opaque;

    if (s->pay_len) {
        memcpy(s->payload + s->rx_len, buf, size);
        s->rx_len += size;
        if (s->rx_len == s->pay_len) {
            s->pay_len = s->rx_len = 0;
            link_message(s, &s->rx, s->payload);
        }
        return;
    }
    memcpy((uint8_t *)&s->rx + s->rx_len, buf, size);
    s->rx_len += size;
    if (s->rx_len < sizeof(s->rx)) {
        return;
    }
    s->rx_len = 0;
    if (le32_to_cpu(s->rx.type) == AHL_SDIO_WRITE) {
        uint32_t len = le32_to_cpu(s->rx.c) & ~ATBM_HERA_LINK_INCR;

        if (len > ATBM_HERA_LINK_MAX_XFER) {
            error_report("atbm-hera-link: a %u-byte write; the link is cut",
                         len);
            qemu_chr_fe_disconnect(&s->chr);
            return;
        }
        if (len) {
            g_free(s->payload);
            s->payload = g_malloc(len);
            s->pay_len = len;
            return;
        }
    }
    link_message(s, &s->rx, NULL);
}

static void link_event(void *opaque, QEMUChrEvent event)
{
    AtbmHeraLinkState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        s->rx_len = s->pay_len = 0;
        link_send(s, AHL_HELLO, 0, 0, ATBM_HERA_LINK_VERSION, NULL, 0);
        break;
    case CHR_EVENT_CLOSED:
        s->hello = false;
        s->rx_len = s->pay_len = 0;
        break;
    default:
        break;
    }
}

static void link_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraLinkState *s = ATBM_HERA_LINK(dev);

    s->powered = true;
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }
    if (!s->hif) {
        error_setg(errp, "the \"hif\" link must name the chip's HIF");
        return;
    }
    error_setg(&s->migration_blocker, "atbm-hera-link: the SoC runs in "
               "another QEMU, whose state is not saved with this one");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, link_can_read, link_read, link_event,
                             NULL, s, NULL, true);
}

static void link_unrealize(DeviceState *dev)
{
    AtbmHeraLinkState *s = ATBM_HERA_LINK(dev);

    qemu_chr_fe_deinit(&s->chr, false);
    g_free(s->payload);
    s->payload = NULL;
}

static void link_init(Object *obj)
{
    AtbmHeraLinkState *s = ATBM_HERA_LINK(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_in_named(dev, link_host_irq, "host-irq", 1);
    qdev_init_gpio_in_named(dev, link_pad_out, "pad-out",
                            ATBM_HERA_LINK_PADS);
    qdev_init_gpio_out_named(dev, s->pad_in, "pad-in", ATBM_HERA_LINK_PADS);
    qdev_init_gpio_out_named(dev, &s->hold, "hold", 1);
    qdev_init_gpio_out_named(dev, &s->reset, "reset", 1);
}

static const Property link_props[] = {
    DEFINE_PROP_CHR("chardev", AtbmHeraLinkState, chr),
    DEFINE_PROP_LINK("hif", AtbmHeraLinkState, hif, TYPE_ATBM_HERA_HIF,
                     AtbmHeraHifState *),
};

static void link_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = link_realize;
    dc->unrealize = link_unrealize;
    dc->desc = "ATBM6441 Hera link to the camera SoC's QEMU";
    device_class_set_props(dc, link_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo link_info = {
    .name = TYPE_ATBM_HERA_LINK,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(AtbmHeraLinkState),
    .instance_init = link_init,
    .class_init = link_class_init,
};

static void link_register_types(void)
{
    type_register_static(&link_info);
}

type_init(link_register_types)
