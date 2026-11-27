/*
 * QTest testcase for the ATBM6441 "Hera" RTC
 *
 * The sleep timer as pwr_ARESA.o uses it: the count of the 32.768 kHz
 * "ms" with its carry into seconds at 1000, the layouts PWR_StartRTC and
 * PWR_GetSleepedTime use, the sync flag PWR_RTC_SYNC polls, the alarm on
 * chip numbers 26 (the first RTC) and 16 (the second).
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define MS_CNT      0x04
#define MS_CNT_B    0x08
#define MS_NUM_INT  0x0c
#define CNTR        0x10
#define ALARM       0x14
#define EN          0x18
#define ST          0x1c

#define EN_RUN      BIT(0)
#define EN_MS       BIT(1)
#define EN_ALARM    BIT(2)
#define ST_MS       BIT(1)
#define ST_ALARM    BIT(2)
#define ST_SYNC     BIT(16)
#define MS_VALID    BIT(31)

#define MSEC        1000000LL
/* the RTC's "ms" in ns: it counts the 32.768 kHz clock */
#define TICK_NS(n)  ((n) * 1000000000LL / 32768)

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
    uint32_t base[] = { HERA_RTC, HERA_RTC2 };

    for (int i = 0; i < 2; i++) {
        g_assert_cmphex(rd(qts, base[i], EN), ==, 0);
        g_assert_cmphex(rd(qts, base[i], ST), ==, ST_SYNC);
        g_assert_cmpuint(rd(qts, base[i], CNTR), ==, 0);
        g_assert_cmpuint(rd(qts, base[i], MS_CNT), ==, 0);
    }
    qtest_quit(qts);
}

/* PWR_GetSleepedTime's view: sec [6:0], min [12:7] ... and ms [25:16] */
static void test_count(void)
{
    QTestState *qts = hera_init("");

    /* stopped it does not count */
    qtest_clock_step(qts, 1500 * MSEC);
    g_assert_cmpuint(rd(qts, HERA_RTC, CNTR), ==, 0);

    wr(qts, HERA_RTC, MS_CNT, 0);
    wr(qts, HERA_RTC, CNTR, 0);
    wr(qts, HERA_RTC, EN, EN_RUN);
    g_assert_cmphex(rd(qts, HERA_RTC, MS_CNT), ==, 0);
    /* 61 "seconds" and 250 */
    qtest_clock_step(qts, TICK_NS(61250) + 1);
    g_assert_cmphex(rd(qts, HERA_RTC, CNTR), ==, 1 << 7 | 1);
    g_assert_cmphex(rd(qts, HERA_RTC, MS_CNT), ==, MS_VALID | 250 << 16);
    g_assert_cmphex(rd(qts, HERA_RTC, ST) & ST_SYNC, ==, ST_SYNC);
    g_assert_cmpuint(rd(qts, HERA_RTC2, CNTR), ==, 0);

    /* stopped again (PWR_RTC_ISR) it holds the count, still valid */
    wr(qts, HERA_RTC, EN, 0);
    qtest_clock_step(qts, 5000 * MSEC);
    g_assert_cmphex(rd(qts, HERA_RTC, CNTR), ==, 1 << 7 | 1);
    g_assert_cmphex(rd(qts, HERA_RTC, MS_CNT), ==, MS_VALID | 250 << 16);

    /* MS_CNT_B's low bits are kept */
    wr(qts, HERA_RTC, MS_CNT_B, 0xffffffff);
    g_assert_cmphex(rd(qts, HERA_RTC, MS_CNT_B), ==, 0x1fff);
    qtest_quit(qts);
}

/*
 * PWR_StartRTC: ALARM sec [5:0] min [11:6] hour [16:12], MS_NUM_INT the
 * ms, EN 7; PWR_RTC_ISR writes ST 4 and EN 0
 */
static void test_alarm(void)
{
    QTestState *qts = hera_init("");
    uint32_t t = 2 * 60000 + 5 * 1000 + 300;

    wr(qts, HERA_RTC, MS_CNT, 0);
    wr(qts, HERA_RTC, CNTR, 0);
    wr(qts, HERA_RTC, ALARM, 2 << 6 | 5);
    wr(qts, HERA_RTC, MS_NUM_INT, 300);
    wr(qts, HERA_RTC, EN, EN_RUN | EN_MS | EN_ALARM);
    qtest_clock_step(qts, TICK_NS(t) - 100000);
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_A));
    qtest_clock_step(qts, 200000);
    g_assert_true(hera_irq(qts, HERA_IRQ_RTC_A));
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_P));
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC2_A));
    g_assert_cmphex(rd(qts, HERA_RTC, ST) & (ST_ALARM | ST_MS), ==,
                    ST_ALARM | ST_MS);

    wr(qts, HERA_RTC, ST, ST_ALARM);
    wr(qts, HERA_RTC, EN, 0);
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_A));
    g_assert_cmphex(rd(qts, HERA_RTC, ST) & ST_ALARM, ==, 0);
    /* what it slept: the count stopped just past the alarm */
    g_assert_cmphex(rd(qts, HERA_RTC, CNTR), ==, 2 << 7 | 5);
    g_assert_cmphex(rd(qts, HERA_RTC, MS_CNT) & MS_VALID, ==, MS_VALID);
    g_assert_cmpuint(extract32(rd(qts, HERA_RTC, MS_CNT), 16, 10), >=, 300);
    g_assert_cmpuint(extract32(rd(qts, HERA_RTC, MS_CNT), 16, 10), <, 310);
    qtest_quit(qts);
}

/* without EN[1] the alarm is on the second, and RTC2 has its own line */
static void test_alarm2(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_RTC2, CNTR, 0);
    wr(qts, HERA_RTC2, ALARM, 3);
    wr(qts, HERA_RTC2, MS_NUM_INT, 999);
    wr(qts, HERA_RTC2, EN, EN_RUN | EN_ALARM);
    qtest_clock_step(qts, TICK_NS(2990));
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC2_A));
    qtest_clock_step(qts, TICK_NS(20));
    g_assert_true(hera_irq(qts, HERA_IRQ_RTC2_A));
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_A));
    g_assert_cmphex(rd(qts, HERA_RTC2, ST) & ST_MS, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/rtc/reset", test_reset);
    qtest_add_func("/atbm-hera/rtc/count", test_count);
    qtest_add_func("/atbm-hera/rtc/alarm", test_alarm);
    qtest_add_func("/atbm-hera/rtc/alarm2", test_alarm2);
    return g_test_run();
}
