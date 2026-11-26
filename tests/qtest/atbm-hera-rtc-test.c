/*
 * QTest testcase for the ATBM6441 "Hera" RTC
 *
 * The seconds and millisecond counters, the sync flag PWR_RTC_SYNC polls,
 * the alarm and the period interrupts on chip numbers 26/25 (the first
 * RTC) and 16/15 (the second).
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
#define EN_SEC      BIT(6)
#define EN_HSEC     BIT(7)
#define ST_MS       BIT(1)
#define ST_ALARM    BIT(2)
#define ST_SEC      BIT(6)
#define ST_HSEC     BIT(7)
#define ST_SYNC     BIT(16)

#define MSEC        1000000LL

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

static void test_count(void)
{
    QTestState *qts = hera_init("");

    /* stopped it does not count */
    qtest_clock_step(qts, 1500 * MSEC);
    g_assert_cmpuint(rd(qts, HERA_RTC, CNTR), ==, 0);

    wr(qts, HERA_RTC, CNTR, 1000);
    wr(qts, HERA_RTC, EN, EN_RUN);
    qtest_clock_step(qts, 2300 * MSEC);
    g_assert_cmpuint(rd(qts, HERA_RTC, CNTR), ==, 1002);
    g_assert_cmpuint(rd(qts, HERA_RTC, MS_CNT), ==, 2300);
    g_assert_cmpuint(rd(qts, HERA_RTC, MS_CNT_B), ==, 2300);
    g_assert_cmphex(rd(qts, HERA_RTC, ST) & ST_SYNC, ==, ST_SYNC);
    g_assert_cmpuint(rd(qts, HERA_RTC2, CNTR), ==, 0);

    /* stopped again it holds both counts */
    wr(qts, HERA_RTC, EN, 0);
    qtest_clock_step(qts, 5000 * MSEC);
    g_assert_cmpuint(rd(qts, HERA_RTC, CNTR), ==, 1002);
    g_assert_cmpuint(rd(qts, HERA_RTC, MS_CNT), ==, 2300);
    qtest_quit(qts);
}

/* PWR_RTC_ISR: ST=4 clears the alarm, EN=0 stops it */
static void test_alarm(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_RTC, CNTR, 10);
    wr(qts, HERA_RTC, ALARM, 12);
    wr(qts, HERA_RTC, EN, EN_RUN | EN_ALARM);
    qtest_clock_step(qts, 1900 * MSEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_A));
    qtest_clock_step(qts, 200 * MSEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_RTC_A));
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_P));
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC2_A));
    g_assert_cmphex(rd(qts, HERA_RTC, ST) & ST_ALARM, ==, ST_ALARM);

    wr(qts, HERA_RTC, ST, ST_ALARM);
    wr(qts, HERA_RTC, EN, 0);
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_A));
    g_assert_cmphex(rd(qts, HERA_RTC, ST) & ST_ALARM, ==, 0);
    qtest_quit(qts);
}

static void test_period(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_RTC2, MS_NUM_INT, 100);
    wr(qts, HERA_RTC2, EN, EN_RUN | EN_MS);
    qtest_clock_step(qts, 90 * MSEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC2_P));
    qtest_clock_step(qts, 20 * MSEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_RTC2_P));
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_P));
    g_assert_cmphex(rd(qts, HERA_RTC2, ST) & ST_MS, ==, ST_MS);
    wr(qts, HERA_RTC2, ST, ST_MS);
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC2_P));
    qtest_clock_step(qts, 100 * MSEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_RTC2_P));
    wr(qts, HERA_RTC2, EN, 0);
    wr(qts, HERA_RTC2, ST, ST_MS);

    /* half second and second ticks, on the first RTC */
    wr(qts, HERA_RTC, EN, EN_RUN | EN_HSEC | EN_SEC);
    qtest_clock_step(qts, 600 * MSEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_RTC_P));
    g_assert_cmphex(rd(qts, HERA_RTC, ST) & 0xff, ==, ST_HSEC);
    wr(qts, HERA_RTC, ST, ST_HSEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_RTC_P));
    qtest_clock_step(qts, 500 * MSEC);
    g_assert_cmphex(rd(qts, HERA_RTC, ST) & 0xff, ==, ST_HSEC | ST_SEC);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/rtc/reset", test_reset);
    qtest_add_func("/atbm-hera/rtc/count", test_count);
    qtest_add_func("/atbm-hera/rtc/alarm", test_alarm);
    qtest_add_func("/atbm-hera/rtc/period", test_period);
    return g_test_run();
}
