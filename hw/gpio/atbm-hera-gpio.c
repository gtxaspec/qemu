/*
 * ATBM6441 "Hera" GPIO (an Andes ATCGPIO100 at 0x16800000) and the pad
 * configuration registers beside it (0x17400000)
 *
 * 28 channels, the chip's internal pin numbers (gpio.h maps the data
 * sheet's GPIO0..20 onto them). Registers, as gpio.c uses them:
 *   0x10 CFG       the channel count, 28
 *   0x20 DIN       the pad levels
 *   0x24 DOUT      the level driven where a channel is an output
 *   0x28 DIR0      1: output
 *   0x2c, 0x30     writing 1 clears (DO0) or sets (DO1) DOUT bits
 *   0x34 DIR1      the inverse direction (the driver sets a bit here to
 *                  make a channel an input); kept, DIR0 decides
 *   0x40 PULLEN, 0x44 PULLMODE, 0x6c DBEN, 0x70 DBCTRL   kept
 *   0x50 INTEN     0x54..0x60 INTMODE0..3: 4 bits a channel, 8 a register:
 *                  2 high level, 3 low level, 5 falling, 6 rising, 7 both
 *   0x64 INTST     the pending channels, write 1 to clear; a level
 *                  channel stays pending while its level holds
 * The pad registers are pin n's at 0x17400000 + 4 + 4 * (n / 2): an even
 * pin in bits 3:0 (function), 10:9 (drive), 12:11 (pull: 1 up, 2 down), an
 * odd one 16 bits up. They are kept, and the pull decides the level of an
 * input nobody drives.
 *
 * Lines (for a SoC wired to the chip): input "pin" n drives pad n; output
 * "out" n is the level the chip drives or, with the channel an input, its
 * pull; output "oe" n says whether the chip drives it; output "level" n is
 * the pad's level as DIN reads it (for the power controller's wake
 * pins). One interrupt, which is the chip's number 23.
 *
 * Not modelled: the function selection (a pad given to a UART, SPI or PWM
 * still reads as a GPIO), the debounce.
 * Assumptions: DIR1 and DIR0 reset to all inputs (DIR1 = all ones), the
 * pad registers reset to 0 (no pull) and ID/revision reads 0.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define TYPE_ATBM_HERA_GPIO "atbm-hera-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraGpioState, ATBM_HERA_GPIO)

#define NGPIO           28
#define GPIO_MASK       ((1u << NGPIO) - 1)

#define G_IDREV         0x00
#define G_CFG           0x10
#define G_DIN           0x20
#define G_DOUT          0x24
#define G_DIR0          0x28
#define G_DO0           0x2c
#define G_DO1           0x30
#define G_DIR1          0x34
#define G_PULLEN        0x40
#define G_PULLMODE      0x44
#define G_INTEN         0x50
#define G_INTMODE0      0x54
#define G_INTST         0x64
#define G_DBEN          0x6c
#define G_DBCTRL        0x70
#define GPIO_SIZE       0x100

#define PAD_SIZE        0x100
#define PAD_REGS        (PAD_SIZE / 4)

#define MODE_HIGH       2
#define MODE_LOW        3
#define MODE_FALL       5
#define MODE_RISE       6
#define MODE_BOTH       7

struct AtbmHeraGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion pad_iomem;
    qemu_irq irq;
    qemu_irq out[NGPIO];
    qemu_irq oe[NGPIO];
    qemu_irq level[NGPIO];

    uint32_t dout;
    uint32_t dir0;
    uint32_t dir1;
    uint32_t pullen;
    uint32_t pullmode;
    uint32_t inten;
    uint32_t intmode[4];
    uint32_t intst;
    uint32_t dben;
    uint32_t dbctrl;
    uint32_t ext;           /* the levels driven from outside */
    uint32_t ext_driven;    /* which pads somebody drives */
    uint32_t pad[PAD_REGS];

    uint32_t pins;          /* the pad levels as last worked out */
    uint32_t last_out;
    uint32_t last_oe;
};

static unsigned gpio_pull(AtbmHeraGpioState *s, int n)
{
    uint32_t reg = s->pad[1 + n / 2];

    return extract32(reg, (n & 1) ? 27 : 11, 2);
}

/* The level of every pad: the chip's output, an outside driver, or a pull */
static uint32_t gpio_levels(AtbmHeraGpioState *s)
{
    uint32_t v = 0;

    for (int n = 0; n < NGPIO; n++) {
        bool level;

        if (s->dir0 & BIT(n)) {
            level = s->dout & BIT(n);
        } else if (s->ext_driven & BIT(n)) {
            level = s->ext & BIT(n);
        } else {
            level = gpio_pull(s, n) == 1;
        }
        v |= (uint32_t)level << n;
    }
    return v;
}

static void gpio_update_irq(AtbmHeraGpioState *s)
{
    qemu_set_irq(s->irq, !!(s->intst & s->inten));
}

static void gpio_refresh(AtbmHeraGpioState *s)
{
    uint32_t now = gpio_levels(s);
    uint32_t changed = now ^ s->pins;
    uint32_t out_now = 0;

    for (int n = 0; n < NGPIO; n++) {
        unsigned mode = extract32(s->intmode[n / 8], (n % 8) * 4, 4) & 7;
        bool lvl = now & BIT(n);

        if (!(s->inten & BIT(n))) {
            continue;
        }
        if ((mode == MODE_HIGH && lvl) || (mode == MODE_LOW && !lvl) ||
            (mode == MODE_FALL && (changed & BIT(n)) && !lvl) ||
            (mode == MODE_RISE && (changed & BIT(n)) && lvl) ||
            (mode == MODE_BOTH && (changed & BIT(n)))) {
            s->intst |= BIT(n);
        }
    }
    for (int n = 0; n < NGPIO; n++) {
        if (changed & BIT(n)) {
            qemu_set_irq(s->level[n], !!(now & BIT(n)));
        }
    }
    s->pins = now;
    gpio_update_irq(s);

    /* the output lines: the driven level, or the pull of a released pad */
    for (int n = 0; n < NGPIO; n++) {
        bool oe = s->dir0 & BIT(n);
        bool lvl = oe ? (s->dout & BIT(n)) : gpio_pull(s, n) == 1;

        out_now |= (uint32_t)lvl << n;
        if ((out_now ^ s->last_out) & BIT(n)) {
            qemu_set_irq(s->out[n], lvl);
        }
        if ((s->dir0 ^ s->last_oe) & BIT(n)) {
            qemu_set_irq(s->oe[n], oe);
        }
    }
    s->last_out = out_now;
    s->last_oe = s->dir0;
}

static void gpio_pin_in(void *opaque, int n, int level)
{
    AtbmHeraGpioState *s = opaque;

    s->ext_driven |= BIT(n);
    s->ext = deposit32(s->ext, n, 1, !!level);
    gpio_refresh(s);
}

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraGpioState *s = opaque;

    switch (addr) {
    case G_IDREV:
        return 0;
    case G_CFG:
        return NGPIO;
    case G_DIN:
        return gpio_levels(s);
    case G_DOUT:
        return s->dout;
    case G_DIR0:
        return s->dir0;
    case G_DO0:
    case G_DO1:
        return 0;
    case G_DIR1:
        return s->dir1;
    case G_PULLEN:
        return s->pullen;
    case G_PULLMODE:
        return s->pullmode;
    case G_INTEN:
        return s->inten;
    case G_INTMODE0 ... G_INTMODE0 + 12:
        return s->intmode[(addr - G_INTMODE0) / 4];
    case G_INTST:
        return s->intst;
    case G_DBEN:
        return s->dben;
    case G_DBCTRL:
        return s->dbctrl;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-gpio: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraGpioState *s = opaque;

    switch (addr) {
    case G_DOUT:
        s->dout = val & GPIO_MASK;
        break;
    case G_DIR0:
        s->dir0 = val & GPIO_MASK;
        break;
    case G_DO0:
        s->dout &= ~val;
        break;
    case G_DO1:
        s->dout |= val & GPIO_MASK;
        break;
    case G_DIR1:
        s->dir1 = val & GPIO_MASK;
        break;
    case G_PULLEN:
        s->pullen = val;
        break;
    case G_PULLMODE:
        s->pullmode = val;
        break;
    case G_INTEN:
        s->inten = val & GPIO_MASK;
        break;
    case G_INTMODE0 ... G_INTMODE0 + 12:
        s->intmode[(addr - G_INTMODE0) / 4] = val;
        break;
    case G_INTST:
        /* a level channel whose level still holds is set again below */
        s->intst &= ~val;
        break;
    case G_DBEN:
        s->dben = val;
        break;
    case G_DBCTRL:
        s->dbctrl = val;
        break;
    case G_IDREV:
    case G_CFG:
    case G_DIN:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-gpio: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
    gpio_refresh(s);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t pad_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraGpioState *s = opaque;

    return s->pad[addr / 4];
}

static void pad_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraGpioState *s = opaque;

    s->pad[addr / 4] = val;
    gpio_refresh(s);
}

static const MemoryRegionOps pad_ops = {
    .read = pad_read,
    .write = pad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void gpio_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraGpioState *s = ATBM_HERA_GPIO(obj);

    s->dout = 0;
    s->dir0 = 0;
    s->dir1 = GPIO_MASK;
    s->pullen = 0;
    s->pullmode = 0;
    s->inten = 0;
    memset(s->intmode, 0, sizeof(s->intmode));
    s->intst = 0;
    s->dben = 0;
    s->dbctrl = 0;
    memset(s->pad, 0, sizeof(s->pad));
    s->pins = gpio_levels(s);
    /* the pads go back to inputs, and what is wired to them hears of it */
    gpio_refresh(s);
}

static void gpio_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraGpioState *s = ATBM_HERA_GPIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &gpio_ops, s,
                          TYPE_ATBM_HERA_GPIO, GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    memory_region_init_io(&s->pad_iomem, OBJECT(dev), &pad_ops, s,
                          TYPE_ATBM_HERA_GPIO "-pad", PAD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->pad_iomem);
}

static void gpio_init(Object *obj)
{
    AtbmHeraGpioState *s = ATBM_HERA_GPIO(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), gpio_pin_in, "pin", NGPIO);
    qdev_init_gpio_out_named(DEVICE(obj), s->out, "out", NGPIO);
    qdev_init_gpio_out_named(DEVICE(obj), s->oe, "oe", NGPIO);
    qdev_init_gpio_out_named(DEVICE(obj), s->level, "level", NGPIO);
}

static const VMStateDescription vmstate_gpio = {
    .name = TYPE_ATBM_HERA_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dout, AtbmHeraGpioState),
        VMSTATE_UINT32(dir0, AtbmHeraGpioState),
        VMSTATE_UINT32(dir1, AtbmHeraGpioState),
        VMSTATE_UINT32(pullen, AtbmHeraGpioState),
        VMSTATE_UINT32(pullmode, AtbmHeraGpioState),
        VMSTATE_UINT32(inten, AtbmHeraGpioState),
        VMSTATE_UINT32_ARRAY(intmode, AtbmHeraGpioState, 4),
        VMSTATE_UINT32(intst, AtbmHeraGpioState),
        VMSTATE_UINT32(dben, AtbmHeraGpioState),
        VMSTATE_UINT32(dbctrl, AtbmHeraGpioState),
        VMSTATE_UINT32(ext, AtbmHeraGpioState),
        VMSTATE_UINT32(ext_driven, AtbmHeraGpioState),
        VMSTATE_UINT32_ARRAY(pad, AtbmHeraGpioState, PAD_REGS),
        VMSTATE_UINT32(pins, AtbmHeraGpioState),
        VMSTATE_UINT32(last_out, AtbmHeraGpioState),
        VMSTATE_UINT32(last_oe, AtbmHeraGpioState),
        VMSTATE_END_OF_LIST()
    }
};

static void gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = gpio_realize;
    dc->vmsd = &vmstate_gpio;
    rc->phases.hold = gpio_reset_hold;
}

static const TypeInfo gpio_info = {
    .name = TYPE_ATBM_HERA_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraGpioState),
    .instance_init = gpio_init,
    .class_init = gpio_class_init,
};

static void gpio_register_types(void)
{
    type_register_static(&gpio_info);
}

type_init(gpio_register_types)
