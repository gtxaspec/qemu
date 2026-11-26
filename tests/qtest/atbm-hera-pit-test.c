/*
 * QTest testcase for the ATBM6441 "Hera" PIT
 *
 * The channel timers in the 32-, 16- and 8-bit modes, the PWM, and the
 * microsecond counter with its four compare values, the way pwm.c and
 * the SDK's timer code program them.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define CFG     0x10
#define INTEN   0x14
#define INTST   0x18
#define CHNEN   0x1c
#define CTRL(n) (0x20 + 0x10 * (n))
#define LOAD(n) (0x24 + 0x10 * (n))
#define CNTR(n) (0x28 + 0x10 * (n))
#define US_NUM  0x60
#define US_LOAD 0x64
#define US_CLKS 0x68
#define US_CNT  0x6c
#define US_CMP1 0x70
#define US_CMP2 0x74
#define US_CMP3 0x78

#define MODE_32  1
#define MODE_16  2
#define MODE_8   3
#define MODE_PWM 4
#define PCLK     0x8
#define PARK     0x10

#define US_EN    0x10000

#define USEC     1000LL

static uint32_t rd(QTestState *qts, uint32_t base, uint32_t reg)
{
    return qtest_readl(qts, base + reg);
}

static void wr(QTestState *qts, uint32_t base, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, base + reg, v);
}

static void test_reset(void)
{
    QTestState *qts = hera_init("");
    uint32_t base[] = { HERA_PIT1, HERA_PIT2, HERA_PIT3 };

    for (int i = 0; i < 3; i++) {
        g_assert_cmphex(rd(qts, base[i], CFG), ==, 4);
        g_assert_cmphex(rd(qts, base[i], INTEN), ==, 0);
        g_assert_cmphex(rd(qts, base[i], INTST), ==, 0);
        g_assert_cmphex(rd(qts, base[i], CHNEN), ==, US_EN);
        g_assert_cmphex(rd(qts, base[i], US_NUM), ==, 0x27);
        for (int c = 0; c < 4; c++) {
            g_assert_cmphex(rd(qts, base[i], CTRL(c)), ==, 0);
            g_assert_cmphex(rd(qts, base[i], LOAD(c)), ==, 0);
        }
    }
    qtest_quit(qts);
}

/* the free-running us count HAL_TIME_US reads, 1 per microsecond */
static void test_us_count(void)
{
    QTestState *qts = hera_init("");
    uint32_t a = rd(qts, HERA_PIT1, US_CNT);

    qtest_clock_step(qts, 1000 * USEC);
    g_assert_cmpuint(rd(qts, HERA_PIT1, US_CNT) - a, ==, 1000);

    /* stopped it holds; started again it restarts from 0 */
    wr(qts, HERA_PIT1, CHNEN, 0);
    a = rd(qts, HERA_PIT1, US_CNT);
    qtest_clock_step(qts, 50 * USEC);
    g_assert_cmpuint(rd(qts, HERA_PIT1, US_CNT), ==, a);
    wr(qts, HERA_PIT1, CHNEN, US_EN);
    g_assert_cmpuint(rd(qts, HERA_PIT1, US_CNT), ==, 0);
    qtest_clock_step(qts, 25 * USEC);
    g_assert_cmpuint(rd(qts, HERA_PIT1, US_CNT), ==, 25);

    /* a different clocks-per-us count slows it */
    wr(qts, HERA_PIT1, CHNEN, 0);
    wr(qts, HERA_PIT1, US_NUM, 79);
    wr(qts, HERA_PIT1, CHNEN, US_EN);
    qtest_clock_step(qts, 100 * USEC);
    g_assert_cmpuint(rd(qts, HERA_PIT1, US_CNT), ==, 50);
    qtest_quit(qts);
}

/* HAL_Start_usTimer: disarm, clear, compare = now + delay, arm */
static void us_start(QTestState *qts, uint32_t base, int k, uint32_t delay)
{
    static const uint32_t cmp[] = { US_LOAD, US_CMP1, US_CMP2, US_CMP3 };
    uint32_t inten = rd(qts, base, INTEN);

    wr(qts, base, INTEN, inten & ~(1u << (16 + k)));
    wr(qts, base, INTST, 1u << (16 + k));
    wr(qts, base, cmp[k], rd(qts, base, US_CNT) + delay);
    wr(qts, base, INTEN, inten | (1u << (16 + k)));
}

static void test_us_compare(void)
{
    QTestState *qts = hera_init("");

    for (int k = 0; k < 4; k++) {
        us_start(qts, HERA_PIT1, k, 500);
        qtest_clock_step(qts, 499 * USEC);
        g_assert_false(hera_irq(qts, HERA_IRQ_PIT));
        g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 0);
        qtest_clock_step(qts, 2 * USEC);
        g_assert_true(hera_irq(qts, HERA_IRQ_PIT));
        g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 1u << (16 + k));

        /* once; a write of 1 clears it */
        qtest_clock_step(qts, 5000 * USEC);
        g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 1u << (16 + k));
        wr(qts, HERA_PIT1, INTST, 1u << (16 + k));
        g_assert_false(hera_irq(qts, HERA_IRQ_PIT));
        wr(qts, HERA_PIT1, INTEN, 0);
    }

    /* unmasked, not armed: no interrupt; the other PITs do not mix in */
    us_start(qts, HERA_PIT2, 2, 100);
    qtest_clock_step(qts, 101 * USEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_PIT_US));
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT));
    wr(qts, HERA_PIT2, INTST, 1u << 18);
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT_US));

    /* PIT3, the tick timer: its interrupt joins 17 */
    us_start(qts, HERA_PIT3, 0, 1000);
    qtest_clock_step(qts, 1001 * USEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_PIT_US));
    g_assert_cmphex(rd(qts, HERA_PIT3, INTST), ==, 1u << 16);
    wr(qts, HERA_PIT3, INTST, 1u << 16);
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT_US));
    qtest_quit(qts);
}

static void test_timer32(void)
{
    QTestState *qts = hera_init("");

    /* 4000 clocks of 40 MHz: 100 us */
    wr(qts, HERA_PIT1, CTRL(0), MODE_32 | PCLK);
    wr(qts, HERA_PIT1, LOAD(0), 3999);
    wr(qts, HERA_PIT1, INTEN, 1);
    wr(qts, HERA_PIT1, CHNEN, US_EN | 1);

    qtest_clock_step(qts, 50 * USEC);
    g_assert_cmpuint(rd(qts, HERA_PIT1, CNTR(0)), ==, 3999 - 2000);
    qtest_clock_step(qts, 49 * USEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT));
    qtest_clock_step(qts, 2 * USEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_PIT));
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 1);

    /* clear it and it comes again a period later */
    wr(qts, HERA_PIT1, INTST, 1);
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT));
    qtest_clock_step(qts, 100 * USEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_PIT));
    wr(qts, HERA_PIT1, INTST, 1);

    /* masked it is pending but silent; stopped it stays at the reload */
    wr(qts, HERA_PIT1, INTEN, 0);
    qtest_clock_step(qts, 100 * USEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT));
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 1);
    wr(qts, HERA_PIT1, INTST, 1);
    wr(qts, HERA_PIT1, CHNEN, US_EN);
    qtest_clock_step(qts, 300 * USEC);
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 0);
    g_assert_cmpuint(rd(qts, HERA_PIT1, CNTR(0)), ==, 3999);
    qtest_quit(qts);
}

/* the external 32.768 kHz clock: 8 clocks is 244140 ns */
static void test_ext_clock(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_PIT2, CTRL(2), MODE_32);
    wr(qts, HERA_PIT2, LOAD(2), 7);
    wr(qts, HERA_PIT2, INTEN, 1 << 8);
    wr(qts, HERA_PIT2, CHNEN, US_EN | 1 << 8);
    qtest_clock_step(qts, 240 * USEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT_US));
    qtest_clock_step(qts, 10 * USEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_PIT_US));
    g_assert_cmphex(rd(qts, HERA_PIT2, INTST), ==, 1 << 8);
    qtest_quit(qts);
}

/* two 16-bit timers in a channel, each with its own field and enable */
static void test_timer16(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_PIT1, CTRL(1), MODE_16 | PCLK);
    wr(qts, HERA_PIT1, LOAD(1), (1999 << 16) | 3999);
    wr(qts, HERA_PIT1, INTEN, 3 << 4);
    wr(qts, HERA_PIT1, CHNEN, US_EN | 3 << 4);
    qtest_clock_step(qts, 51 * USEC);
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 1 << 5);
    g_assert_true(hera_irq(qts, HERA_IRQ_PIT));
    qtest_clock_step(qts, 50 * USEC);
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST) & 0xff, ==, 3 << 4);
    wr(qts, HERA_PIT1, INTST, 3 << 4);
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT));

    /* only timer 1 enabled: timer 0 stays quiet */
    wr(qts, HERA_PIT1, CHNEN, US_EN | 2 << 4);
    qtest_clock_step(qts, 120 * USEC);
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST) & 0xff, ==, 1 << 5);
    wr(qts, HERA_PIT1, INTST, 1 << 5);

    /* four 8-bit timers */
    wr(qts, HERA_PIT1, CHNEN, US_EN);
    wr(qts, HERA_PIT1, CTRL(1), MODE_8 | PCLK);
    wr(qts, HERA_PIT1, LOAD(1), 0x7f3f1f0f);
    wr(qts, HERA_PIT1, CHNEN, US_EN | 0xf << 4);
    g_assert_cmphex(rd(qts, HERA_PIT1, CNTR(1)), ==, 0x7f3f1f0f);
    /* 16, 32, 64 and 128 clocks: 400, 800, 1600 and 3200 ns */
    qtest_clock_step(qts, 300);
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 0);
    qtest_clock_step(qts, 150);
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 1 << 4);
    qtest_clock_step(qts, 400);
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 3 << 4);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(rd(qts, HERA_PIT1, INTST), ==, 7 << 4);
    qtest_quit(qts);
}

/* PWM: a high phase of RELOAD[31:16]+1 clocks, a low of RELOAD[15:0]+1 */
static void test_pwm(void)
{
    /* qtest watches one device's lines at a time: the PIT's outputs */
    QTestState *qts = qtest_init("-machine atbm-hera -cpu " HERA_CPU "");

    qtest_irq_intercept_out_named(qts, "/machine/pit3", "pwm");
    g_assert_false(qtest_get_irq(qts, 1));

    /* a parked-high channel */
    wr(qts, HERA_PIT3, CTRL(1), MODE_PWM | PARK);
    g_assert_true(qtest_get_irq(qts, 1));
    wr(qts, HERA_PIT3, CTRL(1), MODE_PWM);
    g_assert_false(qtest_get_irq(qts, 1));

    /* 10 us high (400 clocks), 30 us low (1200 clocks) */
    wr(qts, HERA_PIT3, CTRL(1), MODE_PWM | PCLK);
    wr(qts, HERA_PIT3, LOAD(1), (399 << 16) | 1199);
    wr(qts, HERA_PIT3, CHNEN, US_EN | 1 << 7);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_clock_step(qts, 9 * USEC);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_clock_step(qts, 2 * USEC);
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_clock_step(qts, 28 * USEC);
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_clock_step(qts, 2 * USEC);
    g_assert_true(qtest_get_irq(qts, 1));

    /* stopped it returns to its park level */
    wr(qts, HERA_PIT3, CHNEN, US_EN);
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/pit/reset", test_reset);
    qtest_add_func("/atbm-hera/pit/us-count", test_us_count);
    qtest_add_func("/atbm-hera/pit/us-compare", test_us_compare);
    qtest_add_func("/atbm-hera/pit/timer32", test_timer32);
    qtest_add_func("/atbm-hera/pit/ext-clock", test_ext_clock);
    qtest_add_func("/atbm-hera/pit/timer16", test_timer16);
    qtest_add_func("/atbm-hera/pit/pwm", test_pwm);
    return g_test_run();
}
