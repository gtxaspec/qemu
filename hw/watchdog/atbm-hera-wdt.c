/*
 * ATBM6441 "Hera" watchdog: Andes ATCWDT200, at 0x16600000 and a second
 * at 0x17500000
 *
 * Registers (wdt.c, wdt.h): 0x00 ID/revision, 0x10 CTRL, 0x14 RESTART,
 * 0x18 WEN, 0x1c ST, 0x20 UNIT, 0x24 reset IVB, 0x28 CPU reset status.
 * CTRL, RESTART, UNIT and the IVB register take a write only after WEN
 * was written 0x5aa5, once: the SDK writes WEN before each of them.
 * RESTART takes 0xcafe.
 *
 * The count is in three stages from a restart, on the EXTCLK (32.768 kHz)
 * or on PCLK (40 MHz) as CTRL[1] says: the interrupt stage, whose end
 * sets ST[0] and raises the "nmi" line if CTRL[2] allows (the SDK's
 * hal_wdt_handler is an exception handler: the chip's WDT interrupt is
 * not among dev_int.h's numbers), then the CPU reset stage and the reset
 * of everything. A restart goes back to the first. At the end of the CPU
 * reset stage, with CTRL[6] set, ST[1] is set and the "cpu-reset" line
 * pulses; at the end of the last, with CTRL[4] set, the machine is reset
 * the way -watchdog-action says. The stage lengths are
 *   interrupt  CTRL[9:7] -> 64 256 1024 2048 4096 8192 16384 32768 clocks
 *   CPU reset  CTRL[13:11], all reset CTRL[17:15] -> 16 64 256 1024 2048
 *              4096 8192 16384 clocks
 * each multiplied by its UNIT count (UNIT[9:0] the interrupt stage,
 * [19:10] the CPU reset, [29:20] the all reset; 0 counts as 1).
 *
 * Assumption (the SDK gives the enum and "64 = 2 ms" but not the unit
 * counter's role): the stage length is the clocks times the UNIT count.
 * ST[3:2], the IVB register and the reset status only keep what is written.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "system/watchdog.h"

#define TYPE_ATBM_HERA_WDT "atbm-hera-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraWdtState, ATBM_HERA_WDT)

#define WDT_IDREV       0x00
#define WDT_CTRL        0x10
#define WDT_RESTART     0x14
#define WDT_WEN         0x18
#define WDT_ST          0x1c
#define WDT_UNIT        0x20
#define WDT_IVB         0x24
#define WDT_CPURST_ST   0x28
#define WDT_SIZE        0x100

#define WDT_ID_VALUE    0x03002000
#define WEN_KEY         0x5aa5
#define RESTART_KEY     0xcafe

#define CTRL_EN         BIT(0)
#define CTRL_PCLK       BIT(1)
#define CTRL_INTEN      BIT(2)
#define CTRL_ALLRST     BIT(4)
#define CTRL_CPURST     BIT(6)
#define CTRL_WMASK      0xfffff
#define ST_INTZERO      BIT(0)
#define ST_CPURST       BIT(1)

#define PCLK_HZ         40000000
#define EXTCLK_HZ       32768

enum { STAGE_INT, STAGE_CPU, STAGE_ALL };

struct AtbmHeraWdtState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq nmi;
    qemu_irq cpu_reset;
    QEMUTimer *timer;

    uint32_t ctrl;
    uint32_t st;
    uint32_t unit;
    uint32_t ivb;
    uint32_t cpurst_st;
    bool unlocked;
    int stage;
};

static const uint16_t int_clocks[] = {
    64, 256, 1024, 2048, 4096, 8192, 16384, 32768
};
static const uint16_t rst_clocks[] = {
    16, 64, 256, 1024, 2048, 4096, 8192, 16384
};

static int64_t wdt_stage_ns(AtbmHeraWdtState *s, int stage)
{
    uint64_t clocks, count;
    uint64_t hz = s->ctrl & CTRL_PCLK ? PCLK_HZ : EXTCLK_HZ;

    switch (stage) {
    case STAGE_INT:
        clocks = int_clocks[extract32(s->ctrl, 7, 3)];
        count = extract32(s->unit, 0, 10);
        break;
    case STAGE_CPU:
        clocks = rst_clocks[extract32(s->ctrl, 11, 3)];
        count = extract32(s->unit, 10, 10);
        break;
    default:
        clocks = rst_clocks[extract32(s->ctrl, 15, 3)];
        count = extract32(s->unit, 20, 10);
        break;
    }
    return muldiv64(clocks * MAX(count, 1), NANOSECONDS_PER_SECOND, hz);
}

static void wdt_update_irq(AtbmHeraWdtState *s)
{
    qemu_set_irq(s->nmi, (s->st & ST_INTZERO) && (s->ctrl & CTRL_INTEN));
}

static void wdt_restart(AtbmHeraWdtState *s)
{
    s->stage = STAGE_INT;
    if (s->ctrl & CTRL_EN) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            wdt_stage_ns(s, STAGE_INT));
    } else {
        timer_del(s->timer);
    }
}

static void wdt_expire(void *opaque)
{
    AtbmHeraWdtState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (s->stage) {
    case STAGE_INT:
        s->st |= ST_INTZERO;
        wdt_update_irq(s);
        s->stage = STAGE_CPU;
        timer_mod(s->timer, now + wdt_stage_ns(s, STAGE_CPU));
        break;
    case STAGE_CPU:
        if (s->ctrl & CTRL_CPURST) {
            s->st |= ST_CPURST;
            qemu_irq_pulse(s->cpu_reset);
        }
        s->stage = STAGE_ALL;
        timer_mod(s->timer, now + wdt_stage_ns(s, STAGE_ALL));
        break;
    default:
        if (s->ctrl & CTRL_ALLRST) {
            watchdog_perform_action();
        }
        break;
    }
}

static uint64_t wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraWdtState *s = opaque;

    switch (addr) {
    case WDT_IDREV:
        return WDT_ID_VALUE;
    case WDT_CTRL:
        return s->ctrl;
    case WDT_ST:
        return s->st;
    case WDT_UNIT:
        return s->unit;
    case WDT_IVB:
        return s->ivb;
    case WDT_CPURST_ST:
        return s->cpurst_st;
    case WDT_RESTART:
    case WDT_WEN:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-wdt: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void wdt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraWdtState *s = opaque;
    bool unlocked = s->unlocked;

    if (addr == WDT_WEN) {
        s->unlocked = (val & 0xffff) == WEN_KEY;
        return;
    }
    if (addr == WDT_ST) {
        s->st &= ~(val & (ST_INTZERO | ST_CPURST));
        wdt_update_irq(s);
        return;
    }
    if (addr == WDT_CPURST_ST) {
        s->cpurst_st = val;
        return;
    }
    switch (addr) {
    case WDT_CTRL:
    case WDT_RESTART:
    case WDT_UNIT:
    case WDT_IVB:
        s->unlocked = false;
        if (!unlocked) {
            qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-wdt: write at 0x%"
                          HWADDR_PRIx " without WEN\n", addr);
            return;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-wdt: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
    switch (addr) {
    case WDT_CTRL:
        s->ctrl = val & CTRL_WMASK;
        wdt_restart(s);
        wdt_update_irq(s);
        break;
    case WDT_RESTART:
        if ((val & 0xffff) == RESTART_KEY) {
            wdt_restart(s);
        }
        break;
    case WDT_UNIT:
        s->unit = val & 0xbfffffff;
        break;
    default:
        s->ivb = val;
        break;
    }
}

static const MemoryRegionOps wdt_ops = {
    .read = wdt_read,
    .write = wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void wdt_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraWdtState *s = ATBM_HERA_WDT(obj);

    s->ctrl = 0;
    s->st = 0;
    s->unit = 0;
    s->ivb = 0;
    s->cpurst_st = 0;
    s->unlocked = false;
    s->stage = STAGE_INT;
    timer_del(s->timer);
    wdt_update_irq(s);
}

static void wdt_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraWdtState *s = ATBM_HERA_WDT(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &wdt_ops, s,
                          TYPE_ATBM_HERA_WDT, WDT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_expire, s);
}

static void wdt_init(Object *obj)
{
    AtbmHeraWdtState *s = ATBM_HERA_WDT(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->nmi);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cpu_reset, "cpu-reset", 1);
}

static const VMStateDescription vmstate_wdt = {
    .name = TYPE_ATBM_HERA_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, AtbmHeraWdtState),
        VMSTATE_UINT32(st, AtbmHeraWdtState),
        VMSTATE_UINT32(unit, AtbmHeraWdtState),
        VMSTATE_UINT32(ivb, AtbmHeraWdtState),
        VMSTATE_UINT32(cpurst_st, AtbmHeraWdtState),
        VMSTATE_BOOL(unlocked, AtbmHeraWdtState),
        VMSTATE_INT32(stage, AtbmHeraWdtState),
        VMSTATE_TIMER_PTR(timer, AtbmHeraWdtState),
        VMSTATE_END_OF_LIST()
    }
};

static void wdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = wdt_realize;
    dc->vmsd = &vmstate_wdt;
    rc->phases.hold = wdt_reset_hold;
}

static const TypeInfo wdt_info = {
    .name = TYPE_ATBM_HERA_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraWdtState),
    .instance_init = wdt_init,
    .class_init = wdt_class_init,
};

static void wdt_register_types(void)
{
    type_register_static(&wdt_info);
}

type_init(wdt_register_types)
