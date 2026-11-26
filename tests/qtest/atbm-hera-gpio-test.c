/*
 * QTest testcase for the ATBM6441 "Hera" GPIO and its pad registers
 *
 * Direction, the set/clear registers, pulls through the pad registers,
 * the five interrupt modes with the level ones staying pending, and the
 * pin and output lines a SoC wires to.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define CFG       0x10
#define DIN       0x20
#define DOUT      0x24
#define DIR0      0x28
#define DO0       0x2c
#define DO1       0x30
#define DIR1      0x34
#define INTEN     0x50
#define INTMODE(n) (0x54 + 4 * (n))
#define INTST     0x64

#define MODE_HIGH 2
#define MODE_LOW  3
#define MODE_FALL 5
#define MODE_RISE 6
#define MODE_BOTH 7

#define PAD(n)    (HERA_PINMUX + 4 + 4 * ((n) / 2))
#define PULL_UP   1
#define PULL_DOWN 2

static uint32_t rd(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, HERA_GPIO + reg);
}

static void wr(QTestState *qts, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, HERA_GPIO + reg, v);
}

static void drive(QTestState *qts, int pin, int level)
{
    qtest_set_irq_in(qts, "/machine/gpio", "pin", pin, level);
}

static void test_reset(void)
{
    QTestState *qts = hera_init("");

    g_assert_cmpuint(rd(qts, CFG), ==, 28);
    g_assert_cmphex(rd(qts, DIN), ==, 0);
    g_assert_cmphex(rd(qts, DOUT), ==, 0);
    g_assert_cmphex(rd(qts, DIR0), ==, 0);
    g_assert_cmphex(rd(qts, DIR1), ==, 0x0fffffff);
    g_assert_cmphex(rd(qts, INTEN), ==, 0);
    g_assert_cmphex(rd(qts, INTST), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HERA_PINMUX + 4), ==, 0);
    qtest_quit(qts);
}

/* gpio_output_config/gpio_output_value as gpio.c does them */
static void test_output(void)
{
    QTestState *qts = hera_init("");

    wr(qts, DIR1, rd(qts, DIR1) & ~(1u << 4));
    wr(qts, DIR0, rd(qts, DIR0) | (1u << 4));
    g_assert_cmphex(rd(qts, DIR0), ==, 1u << 4);
    g_assert_cmphex(rd(qts, DIN) & (1u << 4), ==, 0);
    wr(qts, DOUT, rd(qts, DOUT) | (1u << 4));
    g_assert_cmphex(rd(qts, DIN) & (1u << 4), ==, 1u << 4);
    g_assert_cmphex(rd(qts, DOUT), ==, 1u << 4);

    /* set and clear registers, and the toggle */
    wr(qts, DO0, 1u << 4);
    g_assert_cmphex(rd(qts, DOUT), ==, 0);
    wr(qts, DO1, 1u << 4 | 1u << 9);
    g_assert_cmphex(rd(qts, DOUT), ==, 1u << 4 | 1u << 9);
    wr(qts, DOUT, rd(qts, DOUT) ^ (1u << 4));
    g_assert_cmphex(rd(qts, DOUT), ==, 1u << 9);

    /* a level written to an input channel does not reach its pad */
    g_assert_cmphex(rd(qts, DIN) & (1u << 9), ==, 0);
    wr(qts, DIR0, rd(qts, DIR0) | (1u << 9));
    g_assert_cmphex(rd(qts, DIN) & (1u << 9), ==, 1u << 9);
    qtest_quit(qts);
}

static void test_pull_and_pin(void)
{
    QTestState *qts = hera_init("");

    /* an odd pin's pull is in bits 28:27, an even pin's in 12:11 */
    qtest_writel(qts, PAD(5), PULL_UP << 27);
    qtest_writel(qts, PAD(6), PULL_UP << 11 | PULL_DOWN << 27);
    g_assert_cmphex(rd(qts, DIN), ==, 1u << 5 | 1u << 6);
    g_assert_cmphex(qtest_readl(qts, PAD(5)), ==, PULL_UP << 27);

    /* somebody driving it wins over a pull */
    drive(qts, 5, 0);
    drive(qts, 7, 1);
    g_assert_cmphex(rd(qts, DIN), ==, 1u << 6 | 1u << 7);
    drive(qts, 5, 1);
    g_assert_cmphex(rd(qts, DIN), ==, 1u << 5 | 1u << 6 | 1u << 7);
    qtest_quit(qts);
}

static void test_lines(void)
{
    QTestState *qts = qtest_init("-machine atbm-hera -cpu " HERA_CPU "");

    qtest_irq_intercept_out_named(qts, "/machine/gpio", "out");
    g_assert_false(qtest_get_irq(qts, 3));
    wr(qts, DIR0, 1u << 3);
    wr(qts, DOUT, 1u << 3);
    g_assert_true(qtest_get_irq(qts, 3));
    wr(qts, DO0, 1u << 3);
    g_assert_false(qtest_get_irq(qts, 3));

    /* released, it is its pull */
    wr(qts, DO1, 1u << 3);
    wr(qts, DIR0, 0);
    g_assert_false(qtest_get_irq(qts, 3));
    qtest_writel(qts, PAD(3), PULL_UP << 27);
    g_assert_true(qtest_get_irq(qts, 3));
    qtest_quit(qts);
}

static void test_level_irq(void)
{
    QTestState *qts = hera_init("");

    /* what gpio_interrupt_config does: the mode, then the enable */
    wr(qts, INTMODE(0), MODE_HIGH << (2 * 4));
    wr(qts, INTEN, 1u << 2);
    g_assert_false(hera_irq(qts, HERA_IRQ_GPIO));
    drive(qts, 2, 1);
    g_assert_true(hera_irq(qts, HERA_IRQ_GPIO));
    g_assert_cmphex(rd(qts, INTST), ==, 1u << 2);

    /* it stays pending while the level holds */
    wr(qts, INTST, 1u << 2);
    g_assert_true(hera_irq(qts, HERA_IRQ_GPIO));
    drive(qts, 2, 0);
    wr(qts, INTST, 1u << 2);
    g_assert_false(hera_irq(qts, HERA_IRQ_GPIO));
    g_assert_cmphex(rd(qts, INTST), ==, 0);

    /* low level, and masked */
    wr(qts, INTMODE(1), MODE_LOW << (1 * 4));
    drive(qts, 9, 1);
    wr(qts, INTEN, 1u << 9);
    g_assert_false(hera_irq(qts, HERA_IRQ_GPIO));
    drive(qts, 9, 0);
    g_assert_true(hera_irq(qts, HERA_IRQ_GPIO));
    wr(qts, INTEN, 0);
    g_assert_false(hera_irq(qts, HERA_IRQ_GPIO));
    qtest_quit(qts);
}

static void test_edge_irq(void)
{
    QTestState *qts = hera_init("");

    wr(qts, INTMODE(0), MODE_FALL << (0 * 4) | MODE_RISE << (1 * 4) |
       MODE_BOTH << (2 * 4));
    wr(qts, INTEN, 7);

    /* the falling edge channel: a rise is nothing, a fall latches */
    drive(qts, 0, 1);
    g_assert_cmphex(rd(qts, INTST), ==, 0);
    drive(qts, 0, 0);
    g_assert_cmphex(rd(qts, INTST), ==, 1);
    g_assert_true(hera_irq(qts, HERA_IRQ_GPIO));
    wr(qts, INTST, 1);
    g_assert_false(hera_irq(qts, HERA_IRQ_GPIO));

    /* the rising edge channel */
    drive(qts, 1, 0);
    drive(qts, 1, 1);
    g_assert_cmphex(rd(qts, INTST), ==, 2);
    wr(qts, INTST, 2);
    drive(qts, 1, 0);
    g_assert_cmphex(rd(qts, INTST), ==, 0);

    /* both */
    drive(qts, 2, 1);
    g_assert_cmphex(rd(qts, INTST), ==, 4);
    wr(qts, INTST, 4);
    drive(qts, 2, 0);
    g_assert_cmphex(rd(qts, INTST), ==, 4);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/gpio/reset", test_reset);
    qtest_add_func("/atbm-hera/gpio/output", test_output);
    qtest_add_func("/atbm-hera/gpio/pull-and-pin", test_pull_and_pin);
    qtest_add_func("/atbm-hera/gpio/lines", test_lines);
    qtest_add_func("/atbm-hera/gpio/level-irq", test_level_irq);
    qtest_add_func("/atbm-hera/gpio/edge-irq", test_edge_irq);
    return g_test_run();
}
