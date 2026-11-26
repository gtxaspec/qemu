/*
 * ATBM6441 "Hera" RTC, at 0x16700000 and a second at 0x17100000
 *
 * The chip's sleep timebase, on the 32.768 kHz clock. The registers
 * (pwr.h, and what the SDK's PWR_RTC_* functions do with them):
 *   0x04 MS_CNT      milliseconds since the RTC was enabled, bit 31 clear
 *   0x08 MS_CNT_B    the same count, read back-to-back with MS_CNT
 *   0x0c MS_NUM_INT  a period in ms: every MS_NUM_INT ms ST[1] is set
 *   0x10 CNTR        seconds, written to set the time
 *   0x14 ALARM       when CNTR reaches it, ST[2] is set
 *   0x18 EN          [0] run, [2] alarm interrupt, [3..7] the day, hour,
 *                    minute, second and half-second period interrupts,
 *                    [1] the ms period interrupt
 *   0x1c ST          the flags EN's bits enable (write 1 to clear), and
 *                    [16], 1 as the writes to the slow clock's domain
 *                    complete: PWR_RTC_SYNC polls it, so it reads 1
 * Two interrupts: 0 the period (chip numbers 25 and 15), 1 the alarm
 * (26 and 16).
 *
 * Assumptions (only pwr.h's names and the SDK's accesses are known): the
 * bit layout of EN and ST is Andes' ATCRTC100 as far as it was
 * recognised in PWR_RTC_ISR (which writes ST=4 and EN=0 to quiet the
 * alarm); CNTR counts plain seconds, not days/hours/minutes; MS_CNT_B
 * equals MS_CNT; the ID register at 0x00 reads 0.
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
#define ST_DAY          BIT(3)
#define ST_HOUR         BIT(4)
#define ST_MIN          BIT(5)
#define ST_SEC          BIT(6)
#define ST_HSEC         BIT(7)
#define ST_PERIOD       (ST_MS | ST_DAY | ST_HOUR | ST_MIN | ST_SEC | ST_HSEC)
#define ST_SYNC         BIT(16)
#define ST_W1C          0xff
#define EN_WMASK        0xff

#define HALF_SEC_NS     (NANOSECONDS_PER_SECOND / 2)
#define MS_NS           1000000LL

struct AtbmHeraRtcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq period_irq;
    qemu_irq alarm_irq;
    QEMUTimer *tick;
    QEMUTimer *ms_timer;

    uint32_t en;
    uint32_t st;
    uint32_t alarm;
    uint32_t ms_num;
    uint32_t cntr_base;     /* CNTR as of base_ns */
    int64_t base_ns;        /* when it was running from cntr_base: 0 s */
    int64_t run_ns;         /* when EN[0] last went up; the ms count's 0 */
    uint32_t held_ms;
    int64_t ms_next;
};

static int64_t rtc_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint32_t rtc_cntr(AtbmHeraRtcState *s)
{
    if (!(s->en & EN_RUN)) {
        return s->cntr_base;
    }
    return s->cntr_base +
           (uint32_t)((rtc_now() - s->base_ns) / NANOSECONDS_PER_SECOND);
}

static uint32_t rtc_ms(AtbmHeraRtcState *s)
{
    if (!(s->en & EN_RUN)) {
        return s->held_ms;
    }
    return ((rtc_now() - s->run_ns) / MS_NS) & 0x7fffffff;
}

static void rtc_update_irq(AtbmHeraRtcState *s)
{
    qemu_set_irq(s->period_irq, !!(s->st & s->en & ST_PERIOD));
    qemu_set_irq(s->alarm_irq, !!(s->st & s->en & ST_ALARM));
}

/* the next half second and the next ms period, if anything is running */
static void rtc_schedule(AtbmHeraRtcState *s)
{
    int64_t now = rtc_now();

    if (!(s->en & EN_RUN)) {
        timer_del(s->tick);
        timer_del(s->ms_timer);
        return;
    }
    timer_mod(s->tick, s->base_ns +
              ((now - s->base_ns) / HALF_SEC_NS + 1) * HALF_SEC_NS);
    if (s->ms_num) {
        s->ms_next = s->run_ns +
                     ((now - s->run_ns) / (s->ms_num * MS_NS) + 1) *
                     (int64_t)s->ms_num * MS_NS;
        timer_mod(s->ms_timer, s->ms_next);
    } else {
        timer_del(s->ms_timer);
    }
}

static void rtc_tick(void *opaque)
{
    AtbmHeraRtcState *s = opaque;
    int64_t half = (rtc_now() - s->base_ns) / HALF_SEC_NS;
    uint32_t secs = s->cntr_base + half / 2;

    s->st |= ST_HSEC;
    if (!(half & 1)) {
        s->st |= ST_SEC;
        if (secs % 60 == 0) {
            s->st |= ST_MIN;
        }
        if (secs % 3600 == 0) {
            s->st |= ST_HOUR;
        }
        if (secs % 86400 == 0) {
            s->st |= ST_DAY;
        }
        if (secs == s->alarm) {
            s->st |= ST_ALARM;
        }
    }
    rtc_update_irq(s);
    rtc_schedule(s);
}

static void rtc_ms_expire(void *opaque)
{
    AtbmHeraRtcState *s = opaque;

    s->st |= ST_MS;
    rtc_update_irq(s);
    rtc_schedule(s);
}

static uint64_t rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraRtcState *s = opaque;

    switch (addr) {
    case RTC_IDREV:
        return 0;
    case RTC_MS_CNT:
    case RTC_MS_CNT_B:
        return rtc_ms(s);
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

    switch (addr) {
    case RTC_MS_NUM_INT:
        s->ms_num = val;
        rtc_schedule(s);
        break;
    case RTC_CNTR:
        s->cntr_base = val;
        s->base_ns = rtc_now();
        rtc_schedule(s);
        break;
    case RTC_ALARM:
        s->alarm = val;
        break;
    case RTC_EN: {
        uint32_t old = s->en;

        if (!(old & EN_RUN) && (val & EN_RUN)) {
            s->base_ns = rtc_now();
            s->run_ns = s->base_ns;
        } else if ((old & EN_RUN) && !(val & EN_RUN)) {
            s->held_ms = rtc_ms(s);
            s->cntr_base = rtc_cntr(s);
        }
        s->en = val & EN_WMASK;
        rtc_schedule(s);
        rtc_update_irq(s);
        break;
    }
    case RTC_ST:
        s->st &= ~(val & ST_W1C);
        rtc_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-rtc: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    }
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
    s->cntr_base = 0;
    s->held_ms = 0;
    s->base_ns = 0;
    s->run_ns = 0;
    timer_del(s->tick);
    timer_del(s->ms_timer);
    rtc_update_irq(s);
}

static void rtc_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraRtcState *s = ATBM_HERA_RTC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rtc_ops, s,
                          TYPE_ATBM_HERA_RTC, RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, rtc_tick, s);
    s->ms_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rtc_ms_expire, s);
}

static void rtc_init(Object *obj)
{
    AtbmHeraRtcState *s = ATBM_HERA_RTC(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->period_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->alarm_irq);
}

static const VMStateDescription vmstate_rtc = {
    .name = TYPE_ATBM_HERA_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(en, AtbmHeraRtcState),
        VMSTATE_UINT32(st, AtbmHeraRtcState),
        VMSTATE_UINT32(alarm, AtbmHeraRtcState),
        VMSTATE_UINT32(ms_num, AtbmHeraRtcState),
        VMSTATE_UINT32(cntr_base, AtbmHeraRtcState),
        VMSTATE_INT64(base_ns, AtbmHeraRtcState),
        VMSTATE_INT64(run_ns, AtbmHeraRtcState),
        VMSTATE_UINT32(held_ms, AtbmHeraRtcState),
        VMSTATE_TIMER_PTR(tick, AtbmHeraRtcState),
        VMSTATE_TIMER_PTR(ms_timer, AtbmHeraRtcState),
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
