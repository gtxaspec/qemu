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
 *   0x1030             bit 0 is kept (the firmware's "state retained"
 *                      flag, set before a light sleep and tested after
 *                      the wake); the rest reads as 0: bit 3 clear makes
 *                      the firmware start its watchdog itself
 *   0x084              [24:5] reads the calibration result as 0x08c does
 *                      (PWR_ConfigSleepClkCalib takes it from here)
 *   0x1400 .. 0x1418   AUXADC: with bit 0 of 0x1400 set 0x1418[9:0] holds
 *                      the "adc-code" property (the SDK reads mV as
 *                      code * 1000 / 423)
 *   0x1800 ..          192 words of deep-sleep retention RAM
 *   0x1084             wake enables [15:0] and levels [31:16], 0x1a0 reads
 *                      the wake pins whose condition holds (input lines
 *                      "wake" 0..15, in the data sheet's wake-id order)
 *
 * The power controller (what pwr_ARESA.o's sleep entry and the SDK's
 * sleep document say; the registers' names are powersave.h's):
 *   0x1000 CPU_CTRL    bit 0 is the sleep request. When the core enters
 *                      standby (input "standby") with it set the chip
 *                      sleeps: output "sleep" goes high (the board holds
 *                      the core) until a wake
 *   0x10fc PWR_SMU_BD  [31:29] == 4 light sleep (CPU_RETENTION_FUNC sleeps
 *                      only then), anything else deep sleep
 *                      (PWR_GotoDeepSleep leaves the register alone);
 *                      light sleep wake enables: bit 1 RTC1, bit 2 RTC2
 *   light sleep        RAM, the peripherals and the Wi-Fi state are kept
 *                      (the CPU and peripheral power domains are off on
 *                      silicon: the firmware re-initialises them). Wakes
 *                      on: an enabled RTC's alarm (inputs "rtc-wake" 0
 *                      and 1), a GPIO wake pin (0x1084), or the
 *                      paravirtual Wi-Fi port's "wlan-wake". A source
 *                      already active as the chip goes to sleep wakes it
 *                      at once. On the wake "sleep" goes low and the
 *                      board restarts the core where 0x102c says
 *   0x102c BOOT_RST    where a restarted core boots: [31:16] an address,
 *                      the ROM's (0x6) through the boot ROM, another
 *                      straight there (the firmware puts its ILM there
 *                      before a light sleep, ORing in bits 1:0, and writes
 *                      the ROM back after the wake, (x & 0xffff) |
 *                      0x60001: the core resumes through _start and
 *                      LightSleep_Wakeup_check); the low bits are kept
 *   deep sleep         a machine reset on the wake: a cold boot. Wakes on
 *                      its timer: 0x10a8 bit 10 enables it, 0x10ac and
 *                      0x10b0[15:0] hold V = (ms * cal / div) >> 5, cal
 *                      the calibration result (0x084) and div = 1 <<
 *                      0x10e4[22:20], as PWR_GotoDeepSleep computes it;
 *                      the model turns V back into those ms
 * Everything else is kept and read back, and an access to a register
 * nothing is known about is logged. The window stays 8 KiB: 0x0000 is
 * the clock and reset block, 0x1000 the power one.
 *
 * Not modelled: PLLs and DCXO (they are ready at once), the sleep
 * sequence's timing (the 3 ms the silicon takes to stop and restart its
 * clocks: the firmware subtracts it from the sleep), the wake by GPIO 0/1
 * from deep sleep, the long sleep (PWR_GotoLongSleep) and the interrupt
 * lines (28, power control, and 30, PMU wake: the firmware's handler for
 * 30 is never registered, a woken core restarts instead): the "power" and
 * "pmu" outputs stay low.
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
#include "qemu/timer.h"
#include "system/runstate.h"
#include "trace.h"

#define TYPE_ATBM_HERA_SMU "atbm-hera-smu"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraSmuState, ATBM_HERA_SMU)

#define SMU_SIZE        0x2000
#define SMU_REGS        (SMU_SIZE / 4)

#define R_CALIB_RES     0x084
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
#define R_BOOT_RST      0x102c
#define R_DS_CFG        0x10a8
#define R_DS_CNT_LO     0x10ac
#define R_DS_CNT_HI     0x10b0
#define R_CLK_DIV       0x10e4
#define R_PWR_BD        0x10fc
#define R_ADC_CTRL      0x1400
#define R_ADC_DATA      0x1418

#define CLKS_PER_32K    1221
#define NWAKE           16

#define CPU_SLEEP_REQ   BIT(0)
#define STRAP_RETAINED  BIT(0)
#define BD_RTC1_WAKE    BIT(1)
#define BD_RTC2_WAKE    BIT(2)
#define BD_LIGHT_MODE   4           /* [31:29] */
#define DS_TIMER_EN     BIT(10)

enum { AWAKE, LIGHT_SLEEP, DEEP_SLEEP };

struct AtbmHeraSmuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq power_irq;
    qemu_irq pmu_irq;
    qemu_irq cpu_hold;
    qemu_irq sleep;
    QEMUTimer *deep_timer;

    uint32_t adc_code;
    uint32_t reg[SMU_REGS];
    uint16_t wake_pins;
    uint8_t rtc_wake;
    bool wlan_wake;
    uint8_t asleep;
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

/* Whether what the chip sleeps in has a reason to end */
static bool smu_wake_pending(AtbmHeraSmuState *s)
{
    uint32_t bd = s->reg[R_PWR_BD / 4];

    return ((bd & BD_RTC1_WAKE) && (s->rtc_wake & 1)) ||
           ((bd & BD_RTC2_WAKE) && (s->rtc_wake & 2)) ||
           smu_wake_status(s) || s->wlan_wake;
}

static void smu_wake(AtbmHeraSmuState *s)
{
    trace_atbm_hera_smu_wake(s->asleep, s->rtc_wake, smu_wake_status(s),
                             s->wlan_wake, s->reg[R_BOOT_RST / 4]);
    s->asleep = AWAKE;
    s->reg[R_CPU_CTRL / 4] &= ~CPU_SLEEP_REQ;
    qemu_set_irq(s->sleep, 0);
}

static void smu_check_wake(AtbmHeraSmuState *s)
{
    if (s->asleep == LIGHT_SLEEP && smu_wake_pending(s)) {
        smu_wake(s);
    }
}

/* The deep sleep timer's count back in nanoseconds, or -1 if it is off */
static int64_t smu_deep_ns(AtbmHeraSmuState *s)
{
    uint64_t v = s->reg[R_DS_CNT_LO / 4] |
                 (uint64_t)(s->reg[R_DS_CNT_HI / 4] & 0xffff) << 32;
    uint32_t cal = extract32(s->reg[R_CALIB_RES / 4], 5, 20);
    unsigned div = 1u << MIN(extract32(s->reg[R_CLK_DIV / 4], 20, 3), 4);

    if (!(s->reg[R_DS_CFG / 4] & DS_TIMER_EN)) {
        return -1;
    }
    if (!cal) {
        cal = CLKS_PER_32K;
    }
    return muldiv64(v * 32 * div, SCALE_MS, cal);
}

static void smu_deep_wake(void *opaque)
{
    AtbmHeraSmuState *s = opaque;

    trace_atbm_hera_smu_wake(s->asleep, 0, 0, 0, 0);
    s->asleep = AWAKE;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

/* The core stopped: the chip sleeps if the firmware asked for it */
static void smu_standby(void *opaque, int n, int level)
{
    AtbmHeraSmuState *s = opaque;
    uint32_t bd = s->reg[R_PWR_BD / 4];
    int64_t ns;

    if (!level || s->asleep || !(s->reg[R_CPU_CTRL / 4] & CPU_SLEEP_REQ)) {
        return;
    }
    if (extract32(bd, 29, 3) != BD_LIGHT_MODE) {
        s->asleep = DEEP_SLEEP;
        ns = smu_deep_ns(s);
        trace_atbm_hera_smu_sleep(s->asleep, bd, ns);
        qemu_set_irq(s->sleep, 1);
        if (ns >= 0) {
            timer_mod(s->deep_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
        }
    } else {
        s->asleep = LIGHT_SLEEP;
        trace_atbm_hera_smu_sleep(s->asleep, bd, -1);
        qemu_set_irq(s->sleep, 1);
        smu_check_wake(s);
    }
}

static void smu_rtc_wake_in(void *opaque, int n, int level)
{
    AtbmHeraSmuState *s = opaque;

    s->rtc_wake = deposit32(s->rtc_wake, n, 1, !!level);
    smu_check_wake(s);
}

static void smu_wlan_wake_in(void *opaque, int n, int level)
{
    AtbmHeraSmuState *s = opaque;

    s->wlan_wake = level;
    smu_check_wake(s);
}

static uint64_t smu_read_word(AtbmHeraSmuState *s, hwaddr addr)
{
    switch (addr) {
    case R_STRAP:
        return s->reg[R_STRAP / 4] & STRAP_RETAINED;
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
            s->reg[R_CALIB_RES / 4] = deposit32(s->reg[R_CALIB_RES / 4], 5, 20,
                                                n * CLKS_PER_32K);
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
    smu_check_wake(s);
}

static void smu_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraSmuState *s = ATBM_HERA_SMU(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->asleep = AWAKE;
    timer_del(s->deep_timer);
    qemu_set_irq(s->sleep, 0);
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
    s->deep_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, smu_deep_wake, s);
}

static void smu_init(Object *obj)
{
    AtbmHeraSmuState *s = ATBM_HERA_SMU(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->power_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->pmu_irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cpu_hold, "cpu-hold", 1);
    qdev_init_gpio_in_named(DEVICE(obj), smu_wake_in, "wake", NWAKE);
    qdev_init_gpio_in_named(DEVICE(obj), smu_standby, "standby", 1);
    qdev_init_gpio_in_named(DEVICE(obj), smu_rtc_wake_in, "rtc-wake", 2);
    qdev_init_gpio_in_named(DEVICE(obj), smu_wlan_wake_in, "wlan-wake", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->sleep, "sleep", 1);
}

static const VMStateDescription vmstate_smu = {
    .name = TYPE_ATBM_HERA_SMU,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, AtbmHeraSmuState, SMU_REGS),
        VMSTATE_UINT16(wake_pins, AtbmHeraSmuState),
        VMSTATE_UINT8(rtc_wake, AtbmHeraSmuState),
        VMSTATE_BOOL(wlan_wake, AtbmHeraSmuState),
        VMSTATE_UINT8(asleep, AtbmHeraSmuState),
        VMSTATE_TIMER_PTR(deep_timer, AtbmHeraSmuState),
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
