/*
 * QTest testcase for the Awinic AW87559 speaker amplifier
 *
 * The test is the guest's aw87xxx driver (v2.0.0.10): SMBus byte reads and
 * writes through an I2C controller's DesignWare registers. Held in reset
 * by its HWEN pin, low as at power-on, the address NAKs (the probe's
 * -EIO on a Wyze Cam v3 Pro); once the pin is high it answers with ID
 * 0x5a and the register values aw87xxx_pid_5a_reg.h lists, takes the
 * profile's writes and the 0xaa soft reset, drops writes to the read only
 * registers, and loses its registers when the pin goes low again.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define TAR             0x04
#define DC              0x10
#define ENB             0x6c
#define RXFLR           0x78
#define TXABRT          0x80
#define CTXABRT         0x54
#define DC_READ         0x100
#define DC_STOP         0x200

#define GPIO            0x10010000
#define RESET_PIN       40              /* PB8 */
#define AMP             0x58

typedef struct {
    QTestState *qts;
    uint32_t base;
} Bus;

static void put(Bus *b, uint8_t reg, const uint8_t *d, int n)
{
    qtest_writel(b->qts, b->base + TAR, AMP);
    qtest_writel(b->qts, b->base + DC, reg | (n ? 0 : DC_STOP));
    for (int i = 0; i < n; i++) {
        qtest_writel(b->qts, b->base + DC, d[i] | (i == n - 1 ? DC_STOP : 0));
    }
}

static void get(Bus *b, uint8_t reg, uint8_t *d, int n)
{
    qtest_writel(b->qts, b->base + TAR, AMP);
    qtest_writel(b->qts, b->base + DC, reg);
    for (int i = 0; i < n; i++) {
        qtest_writel(b->qts, b->base + DC,
                     DC_READ | (i == n - 1 ? DC_STOP : 0));
    }
    g_assert_cmpuint(qtest_readl(b->qts, b->base + RXFLR), ==, n);
    for (int i = 0; i < n; i++) {
        d[i] = qtest_readl(b->qts, b->base + DC);
    }
}

static uint8_t rd(Bus *b, uint8_t reg)
{
    uint8_t v;

    get(b, reg, &v, 1);
    return v;
}

static void wr(Bus *b, uint8_t reg, uint8_t v)
{
    put(b, reg, &v, 1);
}

static bool nacked(Bus *b)
{
    bool r = qtest_readl(b->qts, b->base + TXABRT) & 1;

    qtest_readl(b->qts, b->base + CTXABRT);
    return r;
}

static void pin(Bus *b, int level)
{
    uint32_t base = GPIO + (RESET_PIN / 32) * 0x1000;
    uint32_t bit = 1u << (RESET_PIN % 32);

    /* PXMSK 0x20, PXPAT1 0x30, PXPAT0 0x40, with set at +4 and clear at +8 */
    qtest_writel(b->qts, base + 0x24, bit);
    qtest_writel(b->qts, base + 0x38, bit);
    qtest_writel(b->qts, base + 0x40 + (level ? 4 : 8), bit);
}

static void probe_nak(Bus *b)
{
    qtest_writel(b->qts, b->base + TAR, AMP);
    qtest_writel(b->qts, b->base + DC, 0x00);
    qtest_writel(b->qts, b->base + DC, DC_READ | DC_STOP);
    g_assert_true(nacked(b));
}

static void test_reset_pin(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t31 "
                                 "-global ingenic-t31.soc-variant=t31x "
                                 "-device aw87559,bus=i2c1,reset-pin=40");
    Bus b = { qts, 0x10051000 };
    uint8_t blk[2];

    qtest_writel(qts, b.base + ENB, 1);

    /* Held in reset until the driver raises the pin */
    probe_nak(&b);

    pin(&b, 1);
    g_assert_cmphex(rd(&b, 0x00), ==, 0x5a);
    g_assert_false(nacked(&b));
    g_assert_cmphex(rd(&b, 0x01), ==, 0x38);
    g_assert_cmphex(rd(&b, 0x02), ==, 0x09);
    g_assert_cmphex(rd(&b, 0x62), ==, 0x02);
    g_assert_cmphex(rd(&b, 0x58), ==, 0xff);
    g_assert_cmphex(rd(&b, 0x6a), ==, 0x00);

    /* A profile's writes stay; a block write steps the pointer */
    wr(&b, 0x01, 0x78);
    g_assert_cmphex(rd(&b, 0x01), ==, 0x78);
    put(&b, 0x02, (const uint8_t[]){ 0x49, 0x0a }, 2);
    get(&b, 0x02, blk, 2);
    g_assert_cmphex(blk[0], ==, 0x49);
    g_assert_cmphex(blk[1], ==, 0x0a);

    /* The ID and the status registers are read only */
    wr(&b, 0x00, 0x11);
    wr(&b, 0x58, 0x00);
    g_assert_cmphex(rd(&b, 0x00), ==, 0x5a);
    g_assert_cmphex(rd(&b, 0x58), ==, 0xff);

    /* Soft reset */
    wr(&b, 0x00, 0xaa);
    g_assert_cmphex(rd(&b, 0x01), ==, 0x38);
    g_assert_cmphex(rd(&b, 0x02), ==, 0x09);

    /* Pin low: NAK, and the registers are gone */
    wr(&b, 0x01, 0x78);
    pin(&b, 0);
    probe_nak(&b);
    pin(&b, 1);
    g_assert_cmphex(rd(&b, 0x01), ==, 0x38);

    qtest_quit(qts);
}

static void test_unwired(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t40 "
                                 "-device aw87559,bus=i2c3,reset-pin=-1");
    Bus b = { qts, 0x10053000 };

    qtest_writel(qts, b.base + ENB, 1);
    g_assert_cmphex(rd(&b, 0x00), ==, 0x5a);
    g_assert_false(nacked(&b));
    qtest_quit(qts);
}

static void test_t40_held(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t40 "
                                 "-device aw87559,bus=i2c3");
    Bus b = { qts, 0x10053000 };

    /* PB31 is the default reset pin: the Wyze Cam v3 Pro's probe -EIO */
    qtest_writel(qts, b.base + ENB, 1);
    probe_nak(&b);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("ingenic-aw87559/reset-pin", test_reset_pin);
    qtest_add_func("ingenic-aw87559/unwired", test_unwired);
    qtest_add_func("ingenic-aw87559/t40-held", test_t40_held);
    return g_test_run();
}
