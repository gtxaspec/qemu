/*
 * Awinic AW87559 class-K speaker amplifier (the AW87549, AW87559, AW87569,
 * AW87579 and AW81509 share its ID, 0x5a)
 *
 * An I2C slave at 0x58 behind byte registers: ID (0x00, read only), the
 * system, boost and PA settings (0x01 to 0x10), the boost output and the
 * status and interrupt registers (0x57 to 0x59) and the trim registers
 * (0x60 to 0x77), all reset to the values the Awinic aw87xxx driver
 * (v2.0.0.10) lists in aw87xxx_pid_5a_reg.h. A write of 0xaa to the ID
 * register is the soft reset the driver uses. A multi-byte transfer steps
 * the register pointer.
 *
 * The chip's HWEN/reset pin, a SoC GPIO ("reset-pin", PB31 on the Wyze Cam
 * v3 Pro), holds it in reset while low: the address does not ACK and the
 * registers go back to their reset values, as read on a Wyze Cam v3 Pro
 * where the driver's probe fails with -EIO until it raises the pin. The
 * driver raises it, waits 2 ms and reads the ID. The speaker profile (an
 * ACF firmware the driver loads from /lib/firmware) is the driver's
 * register writes, which the registers keep as written: after the
 * driver's Voice profile a Wyze Cam v3 Pro reads 0x01 = 0x78 (boost, PA
 * and switch on), as the vendor profile writes it.
 *
 * The status registers (0x58 and 0x59) and the boost output (0x57) are
 * read only to the driver's register map, and writes to them are dropped.
 * Registers the map does not name read what was written (0 from reset):
 * the Wyze Cam v3 Pro's 0x6a to 0x6f read 0. Nothing the driver does makes
 * the audio path observable, so the output stage is not modelled.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/notify.h"
#include "system/system.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/gpio/ingenic-gpio.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_AW87559 "aw87559"
OBJECT_DECLARE_SIMPLE_TYPE(Aw87559State, AW87559)

#define AW_REGS         0x80
#define AW_ID           0x00
#define AW_ID_VALUE     0x5a
#define AW_SOFT_RESET   0xaa
#define AW_BOOST_DET    0x57
#define AW_SYSST        0x58
#define AW_SYSINT       0x59

struct Aw87559State {
    I2CSlave parent_obj;

    Notifier done;
    DeviceState *gpio;
    int32_t reset_pin;
    uint8_t regs[AW_REGS];
    uint8_t pointer;
    uint8_t len;                /* bytes of this write so far */
    bool wired;                 /* the reset pin is a SoC GPIO */
    bool held;                  /* the reset pin is low */
};

/* aw87xxx_pid_5a_reg.h's default values, by register */
static const uint8_t aw87559_reset[AW_REGS] = {
    [0x00] = AW_ID_VALUE, [0x01] = 0x38, [0x02] = 0x09, [0x03] = 0x0c,
    [0x04] = 0x00, [0x05] = 0x08, [0x06] = 0x10, [0x07] = 0x43,
    [0x08] = 0x4e, [0x09] = 0x03, [0x0a] = 0x08, [0x0b] = 0x4a,
    [0x0c] = 0x03, [0x0d] = 0xdd, [0x0e] = 0x7a, [0x0f] = 0x23,
    [0x10] = 0x08, [0x57] = 0x0c, [0x58] = 0xff, [0x59] = 0xff,
    [0x60] = 0x66, [0x61] = 0x18, [0x62] = 0x02, [0x63] = 0x08,
    [0x64] = 0x45, [0x65] = 0x53, [0x66] = 0x70, [0x67] = 0x08,
    [0x68] = 0x21, [0x69] = 0xa4, [0x70] = 0x1c, [0x71] = 0x10,
    [0x72] = 0xa0, [0x73] = 0x54, [0x74] = 0x00,
};

static void aw87559_chip_reset(Aw87559State *s)
{
    memcpy(s->regs, aw87559_reset, sizeof(s->regs));
    s->pointer = 0;
    s->len = 0;
}

static bool aw87559_read_only(uint8_t reg)
{
    return reg == AW_ID || (reg >= AW_BOOST_DET && reg <= AW_SYSINT);
}

static void aw87559_write(Aw87559State *s, uint8_t reg, uint8_t v)
{
    trace_aw87559_write(reg, v);
    if (reg == AW_ID && v == AW_SOFT_RESET) {
        aw87559_chip_reset(s);
        return;
    }
    if (reg >= AW_REGS || aw87559_read_only(reg)) {
        return;
    }
    s->regs[reg] = v;
}

static int aw87559_event(I2CSlave *i2c, enum i2c_event event)
{
    Aw87559State *s = AW87559(i2c);

    switch (event) {
    case I2C_START_SEND:
        if (s->held) {
            return 1;
        }
        s->len = 0;
        break;
    case I2C_START_RECV:
        if (s->held) {
            return 1;
        }
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t aw87559_recv(I2CSlave *i2c)
{
    Aw87559State *s = AW87559(i2c);
    uint8_t v = s->pointer < AW_REGS ? s->regs[s->pointer] : 0xff;

    trace_aw87559_read(s->pointer, v);
    s->pointer++;
    return v;
}

static int aw87559_send(I2CSlave *i2c, uint8_t data)
{
    Aw87559State *s = AW87559(i2c);

    if (s->len++ == 0) {
        s->pointer = data;
    } else {
        aw87559_write(s, s->pointer++, data);
    }
    return 0;
}

/* The HWEN pin: low holds the chip in reset, as the driver's power on/off */
static void aw87559_reset_pin(void *opaque, int n, int level)
{
    Aw87559State *s = opaque;
    bool held = !level;

    if (held == s->held) {
        return;
    }
    trace_aw87559_reset_pin(level);
    s->held = held;
    aw87559_chip_reset(s);
}

static void aw87559_done(Notifier *n, void *data)
{
    Aw87559State *s = container_of(n, Aw87559State, done);
    Object *o;
    bool ambiguous;

    if (s->reset_pin < 0) {
        return;
    }
    o = object_resolve_path_type("", TYPE_INGENIC_GPIO, &ambiguous);
    s->gpio = o && !ambiguous ? DEVICE(o) : NULL;
    if (!s->gpio) {
        return;
    }
    s->wired = true;
    s->held = true;
    qdev_connect_gpio_out_named(s->gpio, "pad", s->reset_pin,
                                qdev_get_gpio_in_named(DEVICE(s), "reset", 0));
}

static void aw87559_reset_hold(Object *obj, ResetType type)
{
    Aw87559State *s = AW87559(obj);

    aw87559_chip_reset(s);
    s->held = s->wired;
}

static void aw87559_realize(DeviceState *dev, Error **errp)
{
    Aw87559State *s = AW87559(dev);
    I2CSlave *i2c = I2C_SLAVE(dev);

    if (!i2c->address) {
        i2c->address = 0x58;
    }
    s->done.notify = aw87559_done;
    qemu_add_machine_init_done_notifier(&s->done);
}

static void aw87559_unrealize(DeviceState *dev)
{
    notifier_remove(&AW87559(dev)->done);
}

static void aw87559_init(Object *obj)
{
    qdev_init_gpio_in_named(DEVICE(obj), aw87559_reset_pin, "reset", 1);
}

static const Property aw87559_props[] = {
    DEFINE_PROP_INT32("reset-pin", Aw87559State, reset_pin, 63),
};

static const VMStateDescription vmstate_aw87559 = {
    .name = TYPE_AW87559,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Aw87559State),
        VMSTATE_UINT8_ARRAY(regs, Aw87559State, AW_REGS),
        VMSTATE_UINT8(pointer, Aw87559State),
        VMSTATE_UINT8(len, Aw87559State),
        VMSTATE_BOOL(wired, Aw87559State),
        VMSTATE_BOOL(held, Aw87559State),
        VMSTATE_END_OF_LIST()
    }
};

static void aw87559_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Awinic AW87559 speaker amplifier";
    dc->realize = aw87559_realize;
    dc->unrealize = aw87559_unrealize;
    dc->vmsd = &vmstate_aw87559;
    device_class_set_props(dc, aw87559_props);
    rc->phases.hold = aw87559_reset_hold;
    k->event = aw87559_event;
    k->recv = aw87559_recv;
    k->send = aw87559_send;
}

static const TypeInfo aw87559_info = {
    .name = TYPE_AW87559,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Aw87559State),
    .instance_init = aw87559_init,
    .class_init = aw87559_class_init,
};

static void aw87559_register_types(void)
{
    type_register_static(&aw87559_info);
}

type_init(aw87559_register_types)
