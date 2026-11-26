/*
 * QTest testcase for the ATBM6441 "Hera" watchdog
 *
 * Write protection through WEN, the restart key, the interrupt stage as
 * hal_wdt_start() sets it up and the reset at the end of the last stage.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define IDREV   0x00
#define CTRL    0x10
#define RESTART 0x14
#define WEN     0x18
#define ST      0x1c
#define UNIT    0x20
#define IVB     0x24
#define RSTST   0x28

#define WEN_KEY     0x5aa5
#define RESTART_KEY 0xcafe

#define CTRL_EN     BIT(0)
#define CTRL_PCLK   BIT(1)
#define CTRL_INTEN  BIT(2)
#define CTRL_ALLRST BIT(4)
#define CTRL_CPURST BIT(6)
#define ST_INTZERO  BIT(0)
#define ST_CPURST   BIT(1)

#define MSEC        1000000LL

static uint32_t rd(QTestState *qts, uint32_t base, uint32_t reg)
{
    return qtest_readl(qts, base + reg);
}

/* a write the way the SDK does it: WEN first */
static void wr(QTestState *qts, uint32_t base, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, base + WEN, WEN_KEY);
    qtest_writel(qts, base + reg, v);
}

static void test_reset(void)
{
    QTestState *qts = hera_init("");
    uint32_t base[] = { HERA_WDT, HERA_WDT2 };

    for (int i = 0; i < 2; i++) {
        g_assert_cmphex(rd(qts, base[i], IDREV), ==, 0x03002000);
        g_assert_cmphex(rd(qts, base[i], CTRL), ==, 0);
        g_assert_cmphex(rd(qts, base[i], ST), ==, 0);
        g_assert_cmphex(rd(qts, base[i], UNIT), ==, 0);
    }
    qtest_quit(qts);
}

static void test_write_protect(void)
{
    QTestState *qts = hera_init("");

    /* without WEN nothing sticks */
    qtest_writel(qts, HERA_WDT + CTRL, CTRL_EN);
    qtest_writel(qts, HERA_WDT + UNIT, 5);
    g_assert_cmphex(rd(qts, HERA_WDT, CTRL), ==, 0);
    g_assert_cmphex(rd(qts, HERA_WDT, UNIT), ==, 0);

    /* WEN opens one write; a wrong key does not open it */
    wr(qts, HERA_WDT, UNIT, 0x80000000 | (3 << 20) | (2 << 10) | 0x40);
    g_assert_cmphex(rd(qts, HERA_WDT, UNIT), ==, 0x80000000 | (3 << 20) |
                    (2 << 10) | 0x40);
    qtest_writel(qts, HERA_WDT + UNIT, 7);
    g_assert_cmphex(rd(qts, HERA_WDT, UNIT) & 0x3ff, ==, 0x40);
    qtest_writel(qts, HERA_WDT + WEN, 0x1234);
    qtest_writel(qts, HERA_WDT + UNIT, 7);
    g_assert_cmphex(rd(qts, HERA_WDT, UNIT) & 0x3ff, ==, 0x40);

    wr(qts, HERA_WDT, IVB, 0x00030000 | 0x20 << 8 | 0xf);
    g_assert_cmphex(rd(qts, HERA_WDT, IVB), ==, 0x0003200f);
    qtest_writel(qts, HERA_WDT2 + WEN, WEN_KEY);
    g_assert_cmphex(rd(qts, HERA_WDT, CTRL), ==, 0);
    qtest_quit(qts);
}

/*
 * hal_wdt_start(): the interrupt stage 1024 clocks of the 32.768 kHz EXTCLK
 * and a unit count of 0x40: two seconds
 */
static void test_interrupt(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_WDT, UNIT, 0x40 | 8 << 10 | 8 << 20);
    wr(qts, HERA_WDT, CTRL, 2 << 7 | 4 << 11 | 4 << 15 | CTRL_ALLRST |
       CTRL_INTEN | CTRL_EN);
    qtest_clock_step(qts, 1900 * MSEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_NMI));
    g_assert_cmphex(rd(qts, HERA_WDT, ST), ==, 0);

    /* fed in time, it starts the stage over */
    wr(qts, HERA_WDT, RESTART, RESTART_KEY);
    qtest_clock_step(qts, 1900 * MSEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_NMI));
    wr(qts, HERA_WDT, RESTART, 0xbeef);
    qtest_clock_step(qts, 200 * MSEC);
    g_assert_true(hera_irq(qts, HERA_IRQ_NMI));
    g_assert_cmphex(rd(qts, HERA_WDT, ST), ==, ST_INTZERO);
    g_assert_false(hera_irq(qts, HERA_IRQ_PIT));

    /* the handler restarts it and clears the status */
    wr(qts, HERA_WDT, RESTART, RESTART_KEY);
    g_assert_true(hera_irq(qts, HERA_IRQ_NMI));
    wr(qts, HERA_WDT, ST, ST_INTZERO);
    g_assert_false(hera_irq(qts, HERA_IRQ_NMI));
    g_assert_cmphex(rd(qts, HERA_WDT, ST), ==, 0);

    /* disabled it never comes */
    wr(qts, HERA_WDT, CTRL, 2 << 7 | CTRL_INTEN);
    qtest_clock_step(qts, 10000 * MSEC);
    g_assert_false(hera_irq(qts, HERA_IRQ_NMI));

    /* interrupt not enabled: ST sets, the line stays low */
    wr(qts, HERA_WDT, CTRL, 2 << 7 | CTRL_EN);
    qtest_clock_step(qts, 2100 * MSEC);
    g_assert_cmphex(rd(qts, HERA_WDT, ST), ==, ST_INTZERO);
    g_assert_false(hera_irq(qts, HERA_IRQ_NMI));
    qtest_quit(qts);
}

/* the other watchdog shares the NMI line */
static void test_second(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_WDT2, UNIT, 1);
    wr(qts, HERA_WDT2, CTRL, CTRL_INTEN | CTRL_EN | CTRL_PCLK);
    /* 64 clocks of 40 MHz: 1.6 us */
    qtest_clock_step(qts, 2000);
    g_assert_true(hera_irq(qts, HERA_IRQ_NMI));
    g_assert_cmphex(rd(qts, HERA_WDT2, ST), ==, ST_INTZERO);
    g_assert_cmphex(rd(qts, HERA_WDT, ST), ==, 0);
    qtest_quit(qts);
}

/* the stages after the interrupt: the CPU reset's status bit, then all */
static void test_reset_stages(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_WDT, UNIT, 1 | 1 << 10 | 1 << 20);
    wr(qts, HERA_WDT, CTRL, CTRL_CPURST | CTRL_ALLRST | CTRL_EN | CTRL_PCLK |
       CTRL_INTEN);
    /* 64 clocks, then 16, then 16: 1.6 us, 0.4 us and 0.4 us */
    qtest_clock_step(qts, 1700);
    g_assert_cmphex(rd(qts, HERA_WDT, ST), ==, ST_INTZERO);
    qtest_clock_step(qts, 400);
    g_assert_cmphex(rd(qts, HERA_WDT, ST), ==, ST_INTZERO | ST_CPURST);
    g_assert_cmphex(rd(qts, HERA_WDT, CTRL), !=, 0);

    /* the last stage resets the machine: the registers are back */
    qtest_clock_step(qts, 500);
    g_assert_cmphex(rd(qts, HERA_WDT, CTRL), ==, 0);
    g_assert_cmphex(rd(qts, HERA_WDT, ST), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/wdt/reset", test_reset);
    qtest_add_func("/atbm-hera/wdt/write-protect", test_write_protect);
    qtest_add_func("/atbm-hera/wdt/interrupt", test_interrupt);
    qtest_add_func("/atbm-hera/wdt/second", test_second);
    qtest_add_func("/atbm-hera/wdt/reset-stages", test_reset_stages);
    return g_test_run();
}
