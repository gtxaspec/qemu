/*
 * ATBM6441 "Hera" RTC, at 0x16700000 and a second at 0x17100000
 *
 * The chip's sleep timer. Nothing in the SDK uses it but the light sleep
 * (pwr_ARESA.o: PWR_StartRTC arms it, PWR_GetSleepedTime reads how long
 * the chip slept, PWR_RTC_ISR quiets it), and what follows is how those
 * use it; the names are pwr.h's:
 *   0x04 MS_CNT      [25:16] the "ms" count, [31] set once the RTC has
 *                    counted since MS_CNT was last written (the firmware
 *                    waits for it after a wake); a write sets the count's
 *                    ms to its [25:16]
 *   0x08 MS_CNT_B    [12:0] kept (the firmware clears them before
 *                    arming: a prescaler or a mode, not known)
 *   0x0c MS_NUM_INT  the alarm's ms
 *   0x10 CNTR        the count: sec [6:0], min [12:7], hour [17:13],
 *                    day [21:18] (as PWR_GetSleepedTime takes it apart);
 *                    a write sets it, the ms to 0
 *   0x14 ALARM       sec [5:0], min [11:6], hour [16:12] (as PWR_StartRTC
 *                    builds it: one bit narrower than CNTR's fields)
 *   0x18 EN          [0] run, [2] the alarm and its interrupt, [1] the
 *                    alarm waits for MS_NUM_INT too
 *   0x1c ST          [2] the alarm, [1] its ms (write 1 to clear); [16]
 *                    reads 1, the writes to the slow clock's domain having
 *                    completed (PWR_RTC_SYNC polls it)
 * The "ms" counts the 32.768 kHz clock and carries into the seconds at
 * 1000: PWR_StartRTC turns a sleep of t us into t * 32768 / 1000000 of
 * them and PWR_GetSleepedTime the count back with * 1000000 / 32768. So
 * this RTC's second is 1000 / 32768 s.
 * Interrupt 0 (chip numbers 25 and 15, the period) stays low; 1 (26 and
 * 16) is the alarm, which also goes to the power controller as output
 * "wake".
 *
 * Not modelled: the periodic interrupts of the Andes ATCRTC100 this RTC
 * looks like (EN[3..7]: the SDK never enables them), the long sleep's
 * slower counting (PWR_StartLongRTC), the ID register (reads 0).
 * Inferred, not documented: everything above but the register names.
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

#define TYPE_ATBM_HERA_RTC "atbm-hera-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraRtcState, ATBM_HERA_RTC)

#define RTC_IDREV       0x00
#define RTC_MS_CNT      0x04
#define RTC_MS_CNT_B    0x08
#define RTC_MS_NUM_INT  0x0c
#define RTC_CNTR        0x10
#define RTC_ALARM       0x14
#define RTC_EN          0x18
#define RTC_ST          0x1c
#define RTC_SIZE        0x100

#define EN_RUN          BIT(0)
#define EN_MS           BIT(1)
#define EN_ALARM        BIT(2)
#define ST_MS           BIT(1)
#define ST_ALARM        BIT(2)
#define ST_SYNC         BIT(16)
#define ST_W1C          0xff
#define EN_WMASK        0xff
#define MS_VALID        BIT(31)

#define TICK_HZ         32768
#define TICKS_PER_DAY   (86400ULL * 1000)

struct AtbmHeraRtcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq period_irq;
    qemu_irq alarm_irq;
    qemu_irq wake;
    QEMUTimer *alarm_timer;

    uint32_t en;
    uint32_t st;
    uint32_t alarm;
    uint32_t ms_num;
    uint32_t cfg;
    uint64_t base;          /* the count at base_ns, or while stopped */
    int64_t base_ns;
    uint64_t written;       /* the count at the last MS_CNT write */
};

static int64_t rtc_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint64_t rtc_count(AtbmHeraRtcState *s)
{
    if (!(s->en & EN_RUN)) {
        return s->base;
    }
    return s->base + muldiv64(rtc_now() - s->base_ns, TICK_HZ,
                              NANOSECONDS_PER_SECOND);
}

static void rtc_set_count(AtbmHeraRtcState *s, uint64_t count)
{
    s->base = count;
    s->base_ns = rtc_now();
}

/* The alarm as a count within a day */
static uint64_t rtc_alarm_count(AtbmHeraRtcState *s)
{
    uint64_t secs = extract32(s->alarm, 12, 5) * 3600 +
                    extract32(s->alarm, 6, 6) * 60 + extract32(s->alarm, 0, 6);

    return secs * 1000 + ((s->en & EN_MS) ? s->ms_num % 1000 : 0);
}

static void rtc_update_irq(AtbmHeraRtcState *s)
{
    bool alarm = s->st & s->en & ST_ALARM;

    qemu_set_irq(s->period_irq, 0);
    qemu_set_irq(s->alarm_irq, alarm);
    qemu_set_irq(s->wake, alarm);
}

static void rtc_schedule(AtbmHeraRtcState *s)
{
    uint64_t now, at;

    if ((s->en & (EN_RUN | EN_ALARM)) != (EN_RUN | EN_ALARM)) {
        timer_del(s->alarm_timer);
        return;
    }
    now = rtc_count(s);
    at = now - now % TICKS_PER_DAY + rtc_alarm_count(s);
    if (at <= now) {
        at += TICKS_PER_DAY;
    }
    timer_mod(s->alarm_timer, s->base_ns +
              muldiv64(at - s->base, NANOSECONDS_PER_SECOND, TICK_HZ) + 1);
}

static void rtc_alarm_expire(void *opaque)
{
    AtbmHeraRtcState *s = opaque;

    s->st |= ST_ALARM | ((s->en & EN_MS) ? ST_MS : 0);
    rtc_update_irq(s);
    rtc_schedule(s);
}

static uint32_t rtc_cntr(AtbmHeraRtcState *s)
{
    uint64_t secs = rtc_count(s) / 1000;

    return (secs % 60) | (secs / 60 % 60) << 7 | (secs / 3600 % 24) << 13 |
           (secs / 86400 % 16) << 18;
}

static uint64_t rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraRtcState *s = opaque;
    uint64_t count;

    switch (addr) {
    case RTC_IDREV:
        return 0;
    case RTC_MS_CNT:
        count = rtc_count(s);
        return (count % 1000) << 16 | (count != s->written ? MS_VALID : 0);
    case RTC_MS_CNT_B:
        return s->cfg;
    case RTC_MS_NUM_INT:
        return s->ms_num;
    case RTC_CNTR:
        return rtc_cntr(s);
    case RTC_ALARM:
        return s->alarm;
    case RTC_EN:
        return s->en;
    case RTC_ST:
        return s->st | ST_SYNC;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-rtc: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void rtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraRtcState *s = opaque;
    uint64_t count = rtc_count(s);

    switch (addr) {
    case RTC_MS_CNT:
        rtc_set_count(s, count - count % 1000 + extract32(val, 16, 10) % 1000);
        s->written = s->base;
        break;
    case RTC_MS_CNT_B:
        s->cfg = val & 0x1fff;
        return;
    case RTC_MS_NUM_INT:
        s->ms_num = val;
        break;
    case RTC_CNTR:
        rtc_set_count(s, ((uint64_t)extract32(val, 18, 4) * 86400 +
                          extract32(val, 13, 5) * 3600 +
                          extract32(val, 7, 6) * 60 +
                          extract32(val, 0, 7)) * 1000);
        break;
    case RTC_ALARM:
        s->alarm = val;
        break;
    case RTC_EN:
        if ((s->en ^ val) & EN_RUN) {
            rtc_set_count(s, count);
        }
        s->en = val & EN_WMASK;
        break;
    case RTC_ST:
        s->st &= ~(val & ST_W1C);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-rtc: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
    rtc_schedule(s);
    rtc_update_irq(s);
}

static const MemoryRegionOps rtc_ops = {
    .read = rtc_read,
    .write = rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rtc_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraRtcState *s = ATBM_HERA_RTC(obj);

    s->en = 0;
    s->st = 0;
    s->alarm = 0;
    s->ms_num = 0;
    s->cfg = 0;
    s->base = 0;
    s->base_ns = 0;
    s->written = 0;
    timer_del(s->alarm_timer);
    rtc_update_irq(s);
}

static void rtc_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraRtcState *s = ATBM_HERA_RTC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rtc_ops, s,
                          TYPE_ATBM_HERA_RTC, RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->alarm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rtc_alarm_expire, s);
}

static void rtc_init(Object *obj)
{
    AtbmHeraRtcState *s = ATBM_HERA_RTC(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->period_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->alarm_irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->wake, "wake", 1);
}

static const VMStateDescription vmstate_rtc = {
    .name = TYPE_ATBM_HERA_RTC,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(en, AtbmHeraRtcState),
        VMSTATE_UINT32(st, AtbmHeraRtcState),
        VMSTATE_UINT32(alarm, AtbmHeraRtcState),
        VMSTATE_UINT32(ms_num, AtbmHeraRtcState),
        VMSTATE_UINT32(cfg, AtbmHeraRtcState),
        VMSTATE_UINT64(base, AtbmHeraRtcState),
        VMSTATE_INT64(base_ns, AtbmHeraRtcState),
        VMSTATE_UINT64(written, AtbmHeraRtcState),
        VMSTATE_TIMER_PTR(alarm_timer, AtbmHeraRtcState),
        VMSTATE_END_OF_LIST()
    }
};

static void rtc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rtc_realize;
    dc->vmsd = &vmstate_rtc;
    rc->phases.hold = rtc_reset_hold;
}

static const TypeInfo rtc_info = {
    .name = TYPE_ATBM_HERA_RTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraRtcState),
    .instance_init = rtc_init,
    .class_init = rtc_class_init,
};

static void rtc_register_types(void)
{
    type_register_static(&rtc_info);
}

type_init(rtc_register_types)
