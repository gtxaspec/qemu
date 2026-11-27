/*
 * QTest testcase for the pads a PWM channel drives
 *
 * A pad in its PWM function reads (PXPIN) the channel's level as it is at
 * that moment, so sampling PXPIN over a period measures the duty cycle, as
 * it does on a T41NQ and a T40XP: a channel held low or high, disabled
 * (its INITL or idle level) or running reads so. A pad in any other
 * function, or a GPIO, reads what outside holds on it, high by default.
 * The timer unit's channels carry the pads of the T23, T31 and T40, the
 * PWM block's those of the T32, T33, T41 and A1.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define GPIO            0x10010000
#define PXINTC          0x18
#define PXMSKC          0x28
#define PXMSKS          0x24
#define PXPAT1S         0x34
#define PXPAT1C         0x38
#define PXPAT0S         0x44
#define PXPAT0C         0x48

#define CPM             0x10000000
#define TCU             0x10002000
#define TCU_TESR        0x14
#define TCU_TECR        0x18
#define TCU_TFCR        0x28
#define TCU_TDFR(n)     (0x40 + (n) * 0x10)
#define TCU_TDHR(n)     (0x44 + (n) * 0x10)
#define TCU_TCNT(n)     (0x48 + (n) * 0x10)
#define TCU_TCSR(n)     (0x4c + (n) * 0x10)
#define CSR_SLOW        (1u << 1 | 5u << 3)
#define CSR_PWM_EN      (1u << 7)

#define PWM_CCFG0       0x00
#define PWM_CCFG1       0x04
#define PWM_ENS         0x10
#define PWM_ENC         0x14
#define PWM_LVL         0x30
#define PWM_WCFG(n)     (0xb0 + (n) * 4)

/* A pad and the channel it carries in function @func */
typedef struct {
    unsigned pin;               /* port * 32 + bit */
    unsigned func;
    unsigned chan;
} Pad;

typedef struct {
    const char *machine;
    const char *args;
    const Pad *pads;
    unsigned npads;
    unsigned stride;            /* of the GPIO ports, 0 for 0x1000 */
    bool block;                 /* the PWM block, not the timer unit */
    uint32_t base;              /* the block's */
    uint32_t gate;              /* CLKGR1 */
    unsigned gate_bit;
    uint32_t cdr;               /* PWMCDR */
    uint32_t clocks[3];         /* CPCCR, CPAPCR, CPMPCR as U-Boot sets them */
} Case;

#define PA(n) (n)
#define PB(n) (32 + (n))
#define PC(n) (64 + (n))
#define PD(n) (96 + (n))

static const Pad legacy_pads[] = {
    { PB(17), 0, 0 }, { PB(18), 0, 1 }, { PC(8), 0, 2 }, { PC(9), 0, 3 },
};
static const Pad t21_pads[] = {
    { PB(17), 0, 0 }, { PC(8), 0, 2 }, { PC(17), 0, 0 }, { PC(18), 0, 1 },
    { PC(11), 1, 2 }, { PC(14), 1, 5 }, { PC(15), 0, 6 }, { PC(16), 1, 7 },
};
static const Pad t30_pads[] = {
    { PB(18), 0, 1 }, { PC(9), 0, 3 }, { PC(11), 1, 0 }, { PC(18), 1, 7 },
    { PC(25), 0, 4 }, { PC(28), 0, 7 },
};
static const Pad t31_pads[] = {
    { PA(14), 2, 0 }, { PA(22), 2, 1 }, { PB(17), 0, 0 }, { PB(27), 1, 2 },
    { PB(28), 1, 3 }, { PC(17), 0, 0 }, { PC(20), 0, 3 },
};
static const Pad t23_pads[] = {
    { PA(14), 2, 0 }, { PB(17), 0, 0 }, { PB(18), 0, 1 },
    { PB(27), 1, 2 }, { PB(28), 1, 3 },
};
static const Pad t32_pads[] = {
    { PA(17), 3, 0 }, { PB(26), 1, 1 }, { PB(27), 1, 2 }, { PB(28), 1, 3 },
    { PB(30), 2, 0 }, { PB(31), 0, 3 }, { PC(9), 2, 2 },
};
static const Pad t33_pads[] = {
    { PA(17), 3, 0 }, { PB(26), 1, 1 }, { PB(27), 1, 2 }, { PB(28), 0, 3 },
    { PB(30), 0, 0 }, { PB(31), 0, 3 }, { PC(9), 2, 2 },
};
static const Pad t40_pads[] = {
    { PC(2), 2, 0 }, { PC(3), 2, 1 }, { PD(22), 2, 6 }, { PC(27), 1, 0 },
    { PB(30), 0, 2 }, { PB(7), 1, 7 },
};
static const Pad t41_pads[] = {
    { PB(17), 0, 0 }, { PB(18), 0, 1 }, { PB(8), 2, 2 }, { PC(16), 2, 1 },
    { PC(18), 2, 3 }, { PD(12), 2, 7 },
};
static const Pad a1_pads[] = {
    { PB(28), 0, 0 }, { PB(31), 0, 3 }, { PC(14), 2, 0 }, { PC(17), 2, 3 },
};

static const Case cases[] = {
    { "ingenic-t10", "", legacy_pads, ARRAY_SIZE(legacy_pads), 0x100 },
    { "ingenic-t20", "", legacy_pads, ARRAY_SIZE(legacy_pads), 0x100 },
    { "ingenic-t21", "", t21_pads, ARRAY_SIZE(t21_pads) },
    { "ingenic-t30", "", t30_pads, ARRAY_SIZE(t30_pads) },
    { "ingenic-t23", "", t23_pads, ARRAY_SIZE(t23_pads) },
    { "ingenic-t31", "", t31_pads, ARRAY_SIZE(t31_pads) },
    { "ingenic-t40", "", t40_pads, ARRAY_SIZE(t40_pads) },
    { "ingenic-t32", "", t32_pads, ARRAY_SIZE(t32_pads), 0, true,
      0x13450000, 0x28, 9, 0x5c,
      { 0x9a7b5510, 0x04b0510d, 0x0640510d } },
    { "ingenic-t33", "", t33_pads, ARRAY_SIZE(t33_pads), 0, true,
      0x13450000, 0x28, 9, 0x5c,
      { 0x9a7b5510, 0x04b0510d, 0x0640510d } },
    { "ingenic-t41", "", t41_pads, ARRAY_SIZE(t41_pads), 0, true,
      0x13460000, 0x28, 7, 0xfc,
      { 0x9a0b5520, 0x05b008bd, 0x15d0889d } },
    { "ingenic-a1", "", a1_pads, ARRAY_SIZE(a1_pads), 0, true,
      0x13460000, 0x38, 15, 0xfc,
      { 0x9a094410, 0x05c0510d, 0x0860510d } },
};

/* A channel's period is 16 ticks, high for 3 of them after 13 low */
#define PERIOD          16
#define HIGH            3

static uint32_t port_reg(const Case *c, unsigned pin, uint32_t reg)
{
    return GPIO + pin / 32 * (c->stride ?: 0x1000) + reg;
}

static unsigned pin_level(QTestState *qts, const Case *c, unsigned pin)
{
    return qtest_readl(qts, port_reg(c, pin, 0)) >> (pin % 32) & 1;
}

/* The pad as a GPIO input, or in function @func */
static void mux(QTestState *qts, const Case *c, unsigned pin, int func)
{
    uint32_t bit = 1u << (pin % 32);

    qtest_writel(qts, port_reg(c, pin, PXINTC), bit);
    if (func < 0) {
        qtest_writel(qts, port_reg(c, pin, PXMSKS), bit);
        qtest_writel(qts, port_reg(c, pin, PXPAT1S), bit);
        return;
    }
    qtest_writel(qts, port_reg(c, pin, PXMSKC), bit);
    qtest_writel(qts, port_reg(c, pin, func & 2 ? PXPAT1S : PXPAT1C), bit);
    qtest_writel(qts, port_reg(c, pin, func & 1 ? PXPAT0S : PXPAT0C), bit);
}

static uint64_t qom_uint(QTestState *qts, const char *path, const char *prop)
{
    QDict *r = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                         "{ 'path': %s, 'property': %s } }", path, prop);
    uint64_t v;

    g_assert(qdict_haskey(r, "return"));
    v = qdict_get_int(r, "return");
    qobject_unref(r);
    return v;
}

static uint64_t chan_out(QTestState *qts, const Case *c, unsigned n,
                         const char *what)
{
    g_autofree char *prop = g_strdup_printf("pwm%u-%s", n, what);

    return qom_uint(qts, c->block ? "/machine/soc/pwm" : "/machine/soc/tcu",
                    prop);
}

static QTestState *start(const Case *c)
{
    QTestState *qts = qtest_initf("-machine %s %s", c->machine, c->args);

    if (c->block) {
        uint32_t gr = qtest_readl(qts, CPM + c->gate);

        qtest_writel(qts, CPM + 0x10, c->clocks[1]);
        qtest_writel(qts, CPM + 0x14, c->clocks[2]);
        qtest_writel(qts, CPM, c->clocks[0]);
        qtest_writel(qts, CPM + c->gate, gr & ~(1u << c->gate_bit));
        qtest_writel(qts, CPM + c->cdr, 1u << 29 | 0xff);
    }
    return qts;
}

/* Channel @n low for 13 ticks, then high for 3, initial and idle level 0 */
static void run(QTestState *qts, const Case *c, unsigned n)
{
    if (c->block) {
        uint32_t lvl = qtest_readl(qts, c->base + PWM_LVL) &
                       ~(0x10001u << n);

        qtest_writel(qts, c->base + PWM_ENC, 1u << n);
        qtest_writel(qts, c->base + (n < 8 ? PWM_CCFG0 : PWM_CCFG1),
                     7u << (n % 8 * 4));
        qtest_writel(qts, c->base + PWM_LVL, lvl);
        qtest_writel(qts, c->base + PWM_WCFG(n),
                     (uint32_t)HIGH << 16 | (PERIOD - HIGH));
        qtest_writel(qts, c->base + PWM_ENS, 1u << n);
        return;
    }
    qtest_writel(qts, TCU + TCU_TCSR(n), CSR_SLOW);
    qtest_writel(qts, TCU + TCU_TDFR(n), PERIOD - 1);
    qtest_writel(qts, TCU + TCU_TDHR(n), PERIOD - HIGH - 1);
    qtest_writel(qts, TCU + TCU_TCNT(n), 0);
    qtest_writel(qts, TCU + TCU_TFCR, 0x10001u << n);
    qtest_writel(qts, TCU + TCU_TCSR(n), CSR_SLOW | CSR_PWM_EN);
    qtest_writel(qts, TCU + TCU_TESR, 1u << n);
}

static void stop(QTestState *qts, const Case *c, unsigned n)
{
    if (c->block) {
        qtest_writel(qts, c->base + PWM_ENC, 1u << n);
    } else {
        qtest_writel(qts, TCU + TCU_TCSR(n), CSR_SLOW);
        qtest_writel(qts, TCU + TCU_TECR, 1u << n);
    }
}

static void test_pads(const void *data)
{
    const Case *c = data;
    QTestState *qts = start(c);

    for (unsigned i = 0; i < c->npads; i++) {
        const Pad *p = &c->pads[i];
        int64_t tick;
        unsigned high = 0;

        /* a GPIO input, and another function, read what outside holds */
        mux(qts, c, p->pin, -1);
        g_assert_cmpuint(pin_level(qts, c, p->pin), ==, 1);
        run(qts, c, p->chan);
        mux(qts, c, p->pin, (p->func + 1) & 3);
        g_assert_cmpuint(pin_level(qts, c, p->pin), ==, 1);
        stop(qts, c, p->chan);

        /* in its PWM function the pad shows the channel's level */
        mux(qts, c, p->pin, p->func);
        g_assert_cmpuint(pin_level(qts, c, p->pin), ==, 0);
        run(qts, c, p->chan);
        g_assert_cmpuint(chan_out(qts, c, p->chan, "period-ns"), !=, 0);
        tick = chan_out(qts, c, p->chan, "period-ns") / PERIOD;

        /* two periods, sampled in the middle of each tick */
        qtest_clock_step(qts, tick / 2);
        for (unsigned k = 0; k < 2 * PERIOD; k++) {
            unsigned lvl = pin_level(qts, c, p->pin);

            g_assert_cmpuint(lvl, ==, chan_out(qts, c, p->chan, "level"));
            g_assert_cmpuint(lvl, ==, k % PERIOD >= PERIOD - HIGH);
            high += lvl;
            qtest_clock_step(qts, tick);
        }
        g_assert_cmpuint(high, ==, 2 * HIGH);

        /* another pad in function 0..3 other than this one's stays high */
        mux(qts, c, p->pin, (p->func + 2) & 3);
        for (unsigned k = 0; k < PERIOD; k++) {
            g_assert_cmpuint(pin_level(qts, c, p->pin), ==, 1);
            qtest_clock_step(qts, tick);
        }
        mux(qts, c, p->pin, p->func);

        /* disabled, it rests at its initial (idle) level, low */
        stop(qts, c, p->chan);
        qtest_clock_step(qts, 2 * PERIOD * tick);
        g_assert_cmpuint(pin_level(qts, c, p->pin), ==, 0);
        mux(qts, c, p->pin, -1);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *path = g_strdup_printf("ingenic-pwm-pad/%s",
                                                cases[i].machine + 8);

        qtest_add_data_func(path, &cases[i], test_pads);
    }
    return g_test_run();
}
