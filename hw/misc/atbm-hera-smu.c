/*
 * ATBM6441 "Hera" SMU/PMU at 0x16100000: clocks, resets, boot and power
 *
 * Only a part of it is in the SDK (pwr.h and what the drivers do with it);
 * the rest was read out of what the firmware accesses. The window is a
 * register file that reads back what was written, with these behaviours:
 *   0x080 CALIB_CTRL   bit 0 starts a sleep-clock calibration: 0x098[0]
 *                      (CALIB_ST) is then 1, as when it is done at once,
 *                      and 0x08c (the count) and 0x090 (the result) hold
 *                      NUM (0x088[9:0]) times 1221, the 40 MHz clocks in
 *                      a 32.768 kHz one; bit 0 clear clears the status
 *   0x188  PLL_CTRL    bit 1 reads 1 once bit 31 has been written 1: the
 *                      bootloader sets 31 and polls 1 (the lock, assumed
 *                      to be a PLL's); the bootloader's use is all that
 *                      is known of it
 *   0x1000 CPU_CTRL    bit 8 holds the CPU: the "cpu-hold" line
 *   0x1028 BOOT_STATE  kept like any register: neither the bootloader
 *                      nor the firmware's boot reads or writes it (it
 *                      was preset to 5, "what the bootrom leaves", before
 *                      the real bootloader was run and proved it unread)
 *   0x1030             read as 0: bit 3 clear makes the firmware start
 *                      its watchdog itself
 *   0x1400 .. 0x1418   AUXADC: with bit 0 of 0x1400 set 0x1418[9:0] holds
 *                      the "adc-code" property (the SDK reads mV as
 *                      code * 1000 / 423)
 *   0x1800 ..          192 words of deep-sleep retention RAM
 *   0x1084             wake enables [15:0] and levels [31:16], 0x1a0 reads
 *                      the wake pins whose condition holds (input lines
 *                      "wake" 0..15, in the data sheet's wake-id order)
 * Everything else is kept and read back, and an access to a register
 * nothing is known about is logged. The window stays 8 KiB: 0x0000 is
 * the clock and reset block, 0x1000 the power one.
 *
 * Not modelled: PLLs and DCXO (they are ready at once), the sleep and
 * wake sequence and the interrupt lines (28, power control, and 30, PMU
 * wake): the "power" and "pmu" outputs stay low until Phase 4.
 * Assumptions: all of the above where it says "reads"; the calibration
 * is instant; reset values are 0.
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
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_ATBM_HERA_SMU "atbm-hera-smu"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraSmuState, ATBM_HERA_SMU)

#define SMU_SIZE        0x2000
#define SMU_REGS        (SMU_SIZE / 4)

#define R_CALIB_CTRL    0x080
#define R_CALIB_NUM     0x088
#define R_CALIB_CNT     0x08c
#define R_PLL_CTRL      0x188
#define R_CALIB_SW      0x090
#define R_CALIB_ST      0x098
#define R_WAKE_CFG      0x1084
#define R_WAKE_ST       0x1a0
#define R_CPU_CTRL      0x1000
#define R_STRAP         0x1030
#define R_ADC_CTRL      0x1400
#define R_ADC_DATA      0x1418

#define CLKS_PER_32K    1221
#define NWAKE           16

struct AtbmHeraSmuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq power_irq;
    qemu_irq pmu_irq;
    qemu_irq cpu_hold;

    uint32_t adc_code;
    uint32_t reg[SMU_REGS];
    uint16_t wake_pins;
    uint32_t logged[SMU_REGS / 32];
};

/*
 * The offsets the SDK's code and the firmware touch (pwr.h and the
 * accesses in the shipped firmware); anything else is logged once
 */
static const struct { uint16_t lo, hi; } smu_known[] = {
    { 0x008, 0x008 }, { 0x010, 0x024 }, { 0x038, 0x044 }, { 0x060, 0x064 },
    { 0x074, 0x098 }, { 0x0a0, 0x0d4 }, { 0x0e0, 0x0e8 }, { 0x13c, 0x140 },
    { 0x188, 0x188 }, { 0x1a0, 0x1a0 }, { 0x1c0, 0x1c0 }, { 0x1d8, 0x1d8 },
    { 0x1ec, 0x1ec },
    { 0x258, 0x258 }, { 0x400, 0x400 }, { 0x1000, 0x1034 },
    { 0x105c, 0x1060 }, { 0x1070, 0x1070 }, { 0x1084, 0x1094 },
    { 0x10a4, 0x10b4 }, { 0x10bc, 0x10bc }, { 0x10d0, 0x10d0 },
    { 0x10e0, 0x10e8 }, { 0x10fc, 0x1104 }, { 0x1400, 0x1418 },
    { 0x140c, 0x140c }, { 0x1800, 0x1aff },
};

static void smu_check_known(AtbmHeraSmuState *s, hwaddr addr, bool is_write)
{
    if (s->logged[addr / 4 / 32] & BIT((addr / 4) % 32)) {
        return;
    }
    s->logged[addr / 4 / 32] |= BIT((addr / 4) % 32);
    for (int i = 0; i < ARRAY_SIZE(smu_known); i++) {
        if (addr >= smu_known[i].lo && addr <= smu_known[i].hi) {
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "atbm-hera-smu: %s at 0x%" HWADDR_PRIx
                  " (nothing is known of it; it reads back what was "
                  "written)\n", is_write ? "write" : "read", addr);
}

static uint16_t smu_wake_status(AtbmHeraSmuState *s)
{
    uint32_t cfg = s->reg[R_WAKE_CFG / 4];
    uint16_t on = cfg & 0xffff;
    uint16_t low = cfg >> 16;

    /* a pin wakes when its level is the one asked for: high unless low */
    return on & ~(s->wake_pins ^ ~low);
}

static uint64_t smu_read_word(AtbmHeraSmuState *s, hwaddr addr)
{
    switch (addr) {
    case R_STRAP:
        return 0;
    case R_PLL_CTRL:
        return s->reg[R_PLL_CTRL / 4] | ((s->reg[R_PLL_CTRL / 4] >> 30) & 2);
    case R_WAKE_ST:
        return smu_wake_status(s);
    case R_ADC_DATA:
        return (s->reg[R_ADC_CTRL / 4] & 1) ? s->adc_code & 0x3ff : 0;
    default:
        return s->reg[addr / 4];
    }
}

static void smu_write_word(AtbmHeraSmuState *s, hwaddr addr, uint32_t val)
{
    switch (addr) {
    case R_WAKE_ST:
        return;
    case R_CALIB_CTRL:
        s->reg[R_CALIB_CTRL / 4] = val;
        if (val & 1) {
            uint32_t n = s->reg[R_CALIB_NUM / 4] & 0x3ff;

            s->reg[R_CALIB_ST / 4] |= 1;
            s->reg[R_CALIB_CNT / 4] = n * CLKS_PER_32K;
            s->reg[R_CALIB_SW / 4] = n * CLKS_PER_32K;
        } else {
            s->reg[R_CALIB_ST / 4] &= ~1;
        }
        return;
    case R_CALIB_ST:
        return;
    case R_CPU_CTRL:
        s->reg[R_CPU_CTRL / 4] = val;
        qemu_set_irq(s->cpu_hold, !!(val & BIT(8)));
        return;
    default:
        s->reg[addr / 4] = val;
        return;
    }
}

/* the firmware also reads and writes bytes and halves of the registers */
static uint64_t smu_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t val;

    smu_check_known(opaque, addr & ~3, false);
    val = smu_read_word(opaque, addr & ~3);

    return (val >> ((addr & 3) * 8)) & MAKE_64BIT_MASK(0, size * 8);
}

static void smu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraSmuState *s = opaque;
    unsigned shift = (addr & 3) * 8;
    uint32_t mask = MAKE_64BIT_MASK(shift, size * 8);

    smu_check_known(s, addr & ~3, true);
    if (size < 4) {
        val = (smu_read_word(s, addr & ~3) & ~mask) | ((val << shift) & mask);
    }
    smu_write_word(s, addr & ~3, val);
}

static const MemoryRegionOps smu_ops = {
    .read = smu_read,
    .write = smu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void smu_wake_in(void *opaque, int n, int level)
{
    AtbmHeraSmuState *s = opaque;

    s->wake_pins = deposit32(s->wake_pins, n, 1, !!level);
}

static void smu_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraSmuState *s = ATBM_HERA_SMU(obj);

    memset(s->reg, 0, sizeof(s->reg));
    qemu_set_irq(s->cpu_hold, 0);
    qemu_set_irq(s->power_irq, 0);
    qemu_set_irq(s->pmu_irq, 0);
}

static void smu_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraSmuState *s = ATBM_HERA_SMU(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &smu_ops, s,
                          TYPE_ATBM_HERA_SMU, SMU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void smu_init(Object *obj)
{
    AtbmHeraSmuState *s = ATBM_HERA_SMU(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->power_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->pmu_irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cpu_hold, "cpu-hold", 1);
    qdev_init_gpio_in_named(DEVICE(obj), smu_wake_in, "wake", NWAKE);
}

static const VMStateDescription vmstate_smu = {
    .name = TYPE_ATBM_HERA_SMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, AtbmHeraSmuState, SMU_REGS),
        VMSTATE_UINT16(wake_pins, AtbmHeraSmuState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property smu_props[] = {
    DEFINE_PROP_UINT32("adc-code", AtbmHeraSmuState, adc_code, 0x200),
};

static void smu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = smu_realize;
    dc->vmsd = &vmstate_smu;
    device_class_set_props(dc, smu_props);
    rc->phases.hold = smu_reset_hold;
}

static const TypeInfo smu_info = {
    .name = TYPE_ATBM_HERA_SMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraSmuState),
    .instance_init = smu_init,
    .class_init = smu_class_init,
};

static void smu_register_types(void)
{
    type_register_static(&smu_info);
}

type_init(smu_register_types)
