/*
 * ATBM6441 "Hera" PIT: three Andes ATCPIT100 timers (0x16400000,
 * 0x16500000 and the PWM-only one at 0x17000000) with the vendor's
 * microsecond counter added
 *
 * Four channels, each four bytes of registers from +0x20 (CTRL, RELOAD,
 * CNTR), a 4-bit group of CHNEN/INTEN/INTST bits per channel:
 *   CTRL[2:0] 1: one 32-bit timer, 2: two 16-bit, 3: four 8-bit,
 *             4: PWM; CTRL[3] the clock: 1 PCLK (40 MHz), 0 the external
 *             32.768 kHz; CTRL[4] the PWM's level while it is stopped
 *   A timer counts RELOAD+1 clocks (each of the 16- and 8-bit timers its
 *   own field of RELOAD) and then sets its INTST bit: bit 4n+j for timer
 *   j of channel n. CHNEN bit 4n+j starts it; bit 4n+3 starts the PWM.
 *   The PWM is high for RELOAD[31:16]+1 clocks and low for
 *   RELOAD[15:0]+1, as pwm.c's duty arithmetic has it, on the "pwm"
 *   output n; stopped, the output sits at CTRL[4].
 * The microsecond counter (all from pwm.c and the lib's timer code):
 *   0x60 clocks per us, minus one (0x27 for the 40 MHz clock)
 *   0x6c the count, in us, running while CHNEN bit 16 is set
 *   0x64, 0x70, 0x74, 0x78  four compare values: when the count reaches
 *        one, INTST bit 16+k is set, once; INTEN bit 16+k arms it, a
 *        compare written while it is armed re-arms it
 *   0x68 the clocks counted so far in the current us
 * The interrupt is the OR of INTST and INTEN.
 *
 * Assumptions (nothing in the SDK says): the us counter runs from reset,
 * as the bootrom leaves it; ID/revision reads 0 and CFG reads 4 (the
 * channels); a timer interrupts at the end of its RELOAD+1'th clock.
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

#define TYPE_ATBM_HERA_PIT "atbm-hera-pit"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraPitState, ATBM_HERA_PIT)

#define PIT_IDREV       0x00
#define PIT_CFG         0x10
#define PIT_INTEN       0x14
#define PIT_INTST       0x18
#define PIT_CHNEN       0x1c
#define PIT_CH(n)       (0x20 + 0x10 * (n))
#define PIT_US_NUM      0x60
#define PIT_US_LOAD     0x64
#define PIT_US_CLKS     0x68
#define PIT_US_CNT      0x6c
#define PIT_US_CMP1     0x70
#define PIT_US_CMP2     0x74
#define PIT_US_CMP3     0x78
#define PIT_SIZE        0x100

#define NCH             4
#define NTMR            (NCH * 4)
#define NUS             4
#define US_BIT          16
#define CHNEN_US        BIT(16)

#define CTRL_MODE       0x7
#define CTRL_PCLK       BIT(3)
#define CTRL_PARK       BIT(4)
#define MODE_32         1
#define MODE_16         2
#define MODE_8          3
#define MODE_PWM        4

#define PCLK_HZ         40000000
#define EXTCLK_HZ       32768

typedef struct PitTimer {
    int64_t start_ns;       /* when this period began; -1 stopped */
    uint32_t reload;        /* its reload value, as the period began */
    int64_t period_ns;
} PitTimer;

struct AtbmHeraPitState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq pwm_out[NCH];
    QEMUTimer *timer;

    uint32_t inten;
    uint32_t intst;
    uint32_t chnen;
    uint32_t ctrl[NCH];
    uint32_t load[NCH];
    PitTimer tmr[NTMR];

    bool pwm_level[NCH];
    int64_t pwm_next[NCH];  /* the next edge, or -1 */

    uint32_t us_num;
    uint32_t us_cmp[NUS];
    int64_t us_base_ns;     /* when the us count was 0 */
    int64_t us_next[NUS];   /* the compare's match, or -1 */
    uint32_t us_held;       /* the count while it is stopped */
};

static uint64_t pit_clk_hz(uint32_t ctrl)
{
    return ctrl & CTRL_PCLK ? PCLK_HZ : EXTCLK_HZ;
}

static int pit_width(uint32_t mode)
{
    return mode == MODE_32 ? 32 : mode == MODE_16 ? 16 : 8;
}

static int pit_ntmr(uint32_t mode)
{
    return mode == MODE_32 ? 1 : mode == MODE_16 ? 2 : 4;
}

static bool pit_active(AtbmHeraPitState *s, int i)
{
    int ch = i / 4, j = i % 4;
    uint32_t mode = s->ctrl[ch] & CTRL_MODE;

    if (mode != MODE_32 && mode != MODE_16 && mode != MODE_8) {
        return false;
    }
    return j < pit_ntmr(mode) && extract32(s->chnen, i, 1);
}

static uint32_t pit_field(AtbmHeraPitState *s, int i)
{
    int ch = i / 4, j = i % 4;
    int w = pit_width(s->ctrl[ch] & CTRL_MODE);

    return w == 32 ? s->load[ch] : extract32(s->load[ch], j * w, w);
}

static int64_t pit_period_ns(AtbmHeraPitState *s, int ch, uint64_t clocks)
{
    return MAX(1, muldiv64(clocks, NANOSECONDS_PER_SECOND,
                           pit_clk_hz(s->ctrl[ch])));
}

static uint32_t pit_us_count(AtbmHeraPitState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t us_ns = (int64_t)(s->us_num + 1) * 1000000000 / PCLK_HZ;

    if (!(s->chnen & CHNEN_US)) {
        return s->us_held;
    }
    return (uint32_t)((now - s->us_base_ns) / us_ns);
}

static void pit_update_irq(AtbmHeraPitState *s)
{
    qemu_set_irq(s->irq, !!(s->intst & s->inten));
}

static void pit_set_pwm(AtbmHeraPitState *s, int ch, bool level)
{
    if (s->pwm_level[ch] != level) {
        s->pwm_level[ch] = level;
        qemu_set_irq(s->pwm_out[ch], level);
    }
}

static void pit_arm_us(AtbmHeraPitState *s, int k)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t us_ns = (int64_t)(s->us_num + 1) * 1000000000 / PCLK_HZ;
    uint32_t delta = s->us_cmp[k] - pit_us_count(s);

    if (!(s->chnen & CHNEN_US) || !extract32(s->inten, US_BIT + k, 1)) {
        s->us_next[k] = -1;
        return;
    }
    s->us_next[k] = now + (int64_t)delta * us_ns;
}

static void pit_schedule(AtbmHeraPitState *s)
{
    int64_t next = INT64_MAX;

    for (int i = 0; i < NTMR; i++) {
        if (s->tmr[i].start_ns >= 0) {
            next = MIN(next, s->tmr[i].start_ns + s->tmr[i].period_ns);
        }
    }
    for (int i = 0; i < NCH; i++) {
        if (s->pwm_next[i] >= 0) {
            next = MIN(next, s->pwm_next[i]);
        }
    }
    for (int k = 0; k < NUS; k++) {
        if (s->us_next[k] >= 0) {
            next = MIN(next, s->us_next[k]);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, next);
    }
}

static void pit_start_timer(AtbmHeraPitState *s, int i)
{
    int ch = i / 4;

    s->tmr[i].start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tmr[i].reload = pit_field(s, i);
    s->tmr[i].period_ns = pit_period_ns(s, ch, (uint64_t)s->tmr[i].reload + 1);
}

static void pit_pwm_edge(AtbmHeraPitState *s, int ch, int64_t at)
{
    uint64_t clocks = (s->pwm_level[ch] ? extract32(s->load[ch], 16, 16)
                                        : extract32(s->load[ch], 0, 16)) + 1;

    s->pwm_next[ch] = at + pit_period_ns(s, ch, clocks);
}

/* Bring every timer in line with CTRL, RELOAD and CHNEN */
static void pit_sync(AtbmHeraPitState *s)
{
    for (int i = 0; i < NTMR; i++) {
        bool on = pit_active(s, i);

        if (on && s->tmr[i].start_ns < 0) {
            pit_start_timer(s, i);
        } else if (!on) {
            s->tmr[i].start_ns = -1;
        }
    }
    for (int ch = 0; ch < NCH; ch++) {
        bool on = (s->ctrl[ch] & CTRL_MODE) == MODE_PWM &&
                  extract32(s->chnen, ch * 4 + 3, 1);

        if (on && s->pwm_next[ch] < 0) {
            pit_set_pwm(s, ch, true);
            pit_pwm_edge(s, ch, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        } else if (!on && s->pwm_next[ch] >= 0) {
            s->pwm_next[ch] = -1;
            pit_set_pwm(s, ch, !!(s->ctrl[ch] & CTRL_PARK));
        } else if (!on) {
            pit_set_pwm(s, ch, !!(s->ctrl[ch] & CTRL_PARK));
        }
    }
    for (int k = 0; k < NUS; k++) {
        if (!(s->chnen & CHNEN_US)) {
            s->us_next[k] = -1;
        }
    }
    pit_schedule(s);
}

static void pit_expire(void *opaque)
{
    AtbmHeraPitState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int i = 0; i < NTMR; i++) {
        PitTimer *t = &s->tmr[i];

        if (t->start_ns < 0 || t->start_ns + t->period_ns > now) {
            continue;
        }
        s->intst |= BIT(i);
        t->start_ns += t->period_ns;
        t->reload = pit_field(s, i);
        t->period_ns = pit_period_ns(s, i / 4, (uint64_t)t->reload + 1);
        if (now - t->start_ns > 1000 * t->period_ns) {
            t->start_ns = now - (now - t->start_ns) % t->period_ns;
        }
    }
    for (int ch = 0; ch < NCH; ch++) {
        if (s->pwm_next[ch] >= 0 && s->pwm_next[ch] <= now) {
            int64_t at = s->pwm_next[ch];

            pit_set_pwm(s, ch, !s->pwm_level[ch]);
            pit_pwm_edge(s, ch, at);
            if (s->pwm_next[ch] < now) {
                s->pwm_next[ch] = now + 1;
            }
        }
    }
    for (int k = 0; k < NUS; k++) {
        if (s->us_next[k] >= 0 && s->us_next[k] <= now) {
            s->intst |= BIT(US_BIT + k);
            s->us_next[k] = -1;
        }
    }
    pit_update_irq(s);
    pit_schedule(s);
}

static uint32_t pit_counter(AtbmHeraPitState *s, int ch)
{
    uint32_t mode = s->ctrl[ch] & CTRL_MODE;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t val = 0;

    if (mode != MODE_32 && mode != MODE_16 && mode != MODE_8) {
        return 0;
    }
    for (int j = 0; j < pit_ntmr(mode); j++) {
        PitTimer *t = &s->tmr[ch * 4 + j];
        int w = pit_width(mode);
        uint64_t left = 0;

        if (t->start_ns >= 0) {
            uint64_t clocks = muldiv64(now - t->start_ns,
                                       pit_clk_hz(s->ctrl[ch]),
                                       NANOSECONDS_PER_SECOND);

            left = clocks > t->reload ? 0 : t->reload - clocks;
        } else {
            left = pit_field(s, ch * 4 + j);
        }
        val = w == 32 ? left : deposit32(val, j * w, w, left);
    }
    return val;
}

static uint64_t pit_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraPitState *s = opaque;
    int ch = (addr - 0x20) / 0x10;

    if (addr >= 0x20 && addr < PIT_CH(NCH)) {
        switch (addr & 0xf) {
        case 0x0:
            return s->ctrl[ch];
        case 0x4:
            return s->load[ch];
        case 0x8:
            return pit_counter(s, ch);
        default:
            return 0;
        }
    }
    switch (addr) {
    case PIT_IDREV:
        return 0;
    case PIT_CFG:
        return NCH;
    case PIT_INTEN:
        return s->inten;
    case PIT_INTST:
        return s->intst;
    case PIT_CHNEN:
        return s->chnen;
    case PIT_US_NUM:
        return s->us_num;
    case PIT_US_LOAD:
        return s->us_cmp[0];
    case PIT_US_CMP1:
        return s->us_cmp[1];
    case PIT_US_CMP2:
        return s->us_cmp[2];
    case PIT_US_CMP3:
        return s->us_cmp[3];
    case PIT_US_CLKS: {
        int64_t us_ns = (int64_t)(s->us_num + 1) * 1000000000 / PCLK_HZ;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        return (s->chnen & CHNEN_US) ?
            ((now - s->us_base_ns) % us_ns) * PCLK_HZ / 1000000000 : 0;
    }
    case PIT_US_CNT:
        return pit_us_count(s);
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-pit: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void pit_write_us(AtbmHeraPitState *s, int k, uint32_t val)
{
    s->us_cmp[k] = val;
    pit_arm_us(s, k);
    pit_schedule(s);
}

static void pit_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraPitState *s = opaque;
    int ch = (addr - 0x20) / 0x10;

    if (addr >= 0x20 && addr < PIT_CH(NCH)) {
        switch (addr & 0xf) {
        case 0x0:
            s->ctrl[ch] = val & (CTRL_MODE | CTRL_PCLK | CTRL_PARK);
            for (int j = 0; j < 4; j++) {
                s->tmr[ch * 4 + j].start_ns = -1;
            }
            s->pwm_next[ch] = -1;
            pit_sync(s);
            break;
        case 0x4:
            s->load[ch] = val;
            break;
        default:
            break;
        }
        return;
    }
    switch (addr) {
    case PIT_INTEN:
        s->inten = val;
        for (int k = 0; k < NUS; k++) {
            if (extract32(val, US_BIT + k, 1)) {
                if (s->us_next[k] < 0 && !extract32(s->intst, US_BIT + k, 1)) {
                    pit_arm_us(s, k);
                }
            } else {
                s->us_next[k] = -1;
            }
        }
        pit_update_irq(s);
        pit_schedule(s);
        break;
    case PIT_INTST:
        s->intst &= ~val;
        pit_update_irq(s);
        break;
    case PIT_CHNEN: {
        uint32_t old = s->chnen;

        if (!(old & CHNEN_US) && (val & CHNEN_US)) {
            s->us_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->us_held = 0;
        } else if ((old & CHNEN_US) && !(val & CHNEN_US)) {
            s->us_held = pit_us_count(s);
        }
        s->chnen = val & (CHNEN_US | 0xffff);
        pit_sync(s);
        for (int k = 0; k < NUS; k++) {
            if (s->chnen & CHNEN_US && s->us_next[k] < 0) {
                pit_arm_us(s, k);
            }
        }
        pit_schedule(s);
        break;
    }
    case PIT_US_NUM:
        s->us_num = val & 0xff;
        break;
    case PIT_US_LOAD:
        pit_write_us(s, 0, val);
        break;
    case PIT_US_CMP1:
        pit_write_us(s, 1, val);
        break;
    case PIT_US_CMP2:
        pit_write_us(s, 2, val);
        break;
    case PIT_US_CMP3:
        pit_write_us(s, 3, val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-pit: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps pit_ops = {
    .read = pit_read,
    .write = pit_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pit_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraPitState *s = ATBM_HERA_PIT(obj);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->inten = 0;
    s->intst = 0;
    s->chnen = CHNEN_US;
    memset(s->ctrl, 0, sizeof(s->ctrl));
    memset(s->load, 0, sizeof(s->load));
    s->us_num = 0x27;
    memset(s->us_cmp, 0, sizeof(s->us_cmp));
    s->us_base_ns = now;
    s->us_held = 0;
    for (int i = 0; i < NTMR; i++) {
        s->tmr[i].start_ns = -1;
    }
    for (int i = 0; i < NCH; i++) {
        s->pwm_next[i] = -1;
        s->pwm_level[i] = false;
    }
    for (int k = 0; k < NUS; k++) {
        s->us_next[k] = -1;
    }
    timer_del(s->timer);
    pit_update_irq(s);
}

static void pit_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraPitState *s = ATBM_HERA_PIT(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pit_ops, s,
                          TYPE_ATBM_HERA_PIT, PIT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pit_expire, s);
}

static void pit_init(Object *obj)
{
    AtbmHeraPitState *s = ATBM_HERA_PIT(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), s->pwm_out, "pwm", NCH);
}

static const VMStateDescription vmstate_pit_timer = {
    .name = TYPE_ATBM_HERA_PIT "/timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(start_ns, PitTimer),
        VMSTATE_UINT32(reload, PitTimer),
        VMSTATE_INT64(period_ns, PitTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pit = {
    .name = TYPE_ATBM_HERA_PIT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(inten, AtbmHeraPitState),
        VMSTATE_UINT32(intst, AtbmHeraPitState),
        VMSTATE_UINT32(chnen, AtbmHeraPitState),
        VMSTATE_UINT32_ARRAY(ctrl, AtbmHeraPitState, NCH),
        VMSTATE_UINT32_ARRAY(load, AtbmHeraPitState, NCH),
        VMSTATE_STRUCT_ARRAY(tmr, AtbmHeraPitState, NTMR, 1,
                             vmstate_pit_timer, PitTimer),
        VMSTATE_BOOL_ARRAY(pwm_level, AtbmHeraPitState, NCH),
        VMSTATE_INT64_ARRAY(pwm_next, AtbmHeraPitState, NCH),
        VMSTATE_UINT32(us_num, AtbmHeraPitState),
        VMSTATE_UINT32_ARRAY(us_cmp, AtbmHeraPitState, NUS),
        VMSTATE_INT64(us_base_ns, AtbmHeraPitState),
        VMSTATE_INT64_ARRAY(us_next, AtbmHeraPitState, NUS),
        VMSTATE_UINT32(us_held, AtbmHeraPitState),
        VMSTATE_TIMER_PTR(timer, AtbmHeraPitState),
        VMSTATE_END_OF_LIST()
    }
};

static void pit_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = pit_realize;
    dc->vmsd = &vmstate_pit;
    rc->phases.hold = pit_reset_hold;
}

static const TypeInfo pit_info = {
    .name = TYPE_ATBM_HERA_PIT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraPitState),
    .instance_init = pit_init,
    .class_init = pit_class_init,
};

static void pit_register_types(void)
{
    type_register_static(&pit_info);
}

type_init(pit_register_types)
