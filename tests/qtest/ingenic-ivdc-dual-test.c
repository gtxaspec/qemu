/*
 * QTest testcase for one IVDC frame feeding both encoders
 *
 * On the T32 the H.264 encoder (VPU0) and the JPEG engine (VPU1) of one
 * group both take their frames over the IVDC. A frame the ISP sends goes
 * to whichever of them starts while it is live, or to both: the second
 * takes it where the first did, as the raptor team's T32LQ runs did (the
 * H.264 main and its JPEG snapshot, both IVDC, 29 fps with snapshots
 * flowing). Each ends the frame in its own bit (18 and 19 of 0x44), the
 * frame done (16) comes once, and a start after the frame has gone waits
 * for the next one without disturbing the other.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define IVDC_PATH       "/machine/soc/ivdc"
#define IVDC            0x13100000
#define CPM_CLKGR0      0x10000020
#define CPM_CLKGR1      0x10000028
#define MS              1000000
#define LINES           1080

#define ISP_SIZE        (IVDC + 0x10)
#define INT_EN          (IVDC + 0x40)
#define INT_STA         (IVDC + 0x44)
#define INT_CLR         (IVDC + 0x54)
#define VPU0_START      (IVDC + 0x70)
#define VPU0_STAT       (IVDC + 0x7c)
#define VPU1_START      (IVDC + 0x88)
#define VPU1_STAT       (IVDC + 0x94)

#define INT_DDR_UV_OVF  (1u << 1)
#define INT_FRM_DONE    (1u << 16)
#define INT_FRM_START   (1u << 17)
#define INT_VPU_END(v)  (1u << (18 + (v)))
#define INT_VPU_ACK_TO  (1u << 24)

static QTestState *ivdc_init(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t32");

    qtest_writel(qts, CPM_CLKGR0, 0);
    qtest_writel(qts, CPM_CLKGR1, 0);
    qtest_writel(qts, ISP_SIZE, LINES << 16);
    qtest_writel(qts, INT_EN, 0xffffffff);
    qtest_irq_intercept_out_named(qts, IVDC_PATH, "taken");
    return qts;
}

static void frame(QTestState *qts)
{
    qtest_set_irq_in(qts, IVDC_PATH, "frame", 0, 1);
    qtest_set_irq_in(qts, IVDC_PATH, "frame", 0, 0);
}

static void start(QTestState *qts, unsigned v)
{
    qtest_writel(qts, v ? VPU1_START : VPU0_START, 1);
}

static uint32_t take_status(QTestState *qts)
{
    uint32_t sta = qtest_readl(qts, INT_STA);

    qtest_writel(qts, INT_CLR, sta);
    return sta;
}

/* Whether VPU @v took sensor 0's frame: its "taken" line 4v is high */
static bool took(QTestState *qts, unsigned v)
{
    return qtest_get_irq(qts, v * 4);
}

static void test_both_at_start(void)
{
    QTestState *qts = ivdc_init();

    /* both start before the frame: both take it as it comes */
    start(qts, 0);
    start(qts, 1);
    frame(qts);
    g_assert_true(took(qts, 0));
    g_assert_true(took(qts, 1));
    g_assert_cmphex(qtest_readl(qts, VPU0_STAT) & 0x70, ==, 0);
    g_assert_cmphex(qtest_readl(qts, VPU1_STAT) & 0x70, ==, 0);
    g_assert_cmphex(take_status(qts) & INT_FRM_START, ==, INT_FRM_START);

    /* neither's end before the frame's lines have gone */
    qtest_clock_step(qts, 20 * MS);
    g_assert_cmphex(take_status(qts) & (INT_VPU_END(0) | INT_VPU_END(1) |
                                        INT_FRM_DONE), ==, 0);
    qtest_clock_step(qts, 15 * MS);
    g_assert_cmphex(take_status(qts) & (INT_VPU_END(0) | INT_VPU_END(1) |
                                        INT_FRM_DONE | INT_DDR_UV_OVF),
                    ==, INT_VPU_END(0) | INT_VPU_END(1) | INT_FRM_DONE);

    /* the frame is gone: it does not come again, nor does it overflow */
    qtest_clock_step(qts, 100 * MS);
    g_assert_cmphex(take_status(qts), ==, 0);
    qtest_quit(qts);
}

static void test_second_joins(void)
{
    QTestState *qts = ivdc_init();

    /* the JPEG engine starts a few lines in, on the frame the H.264 has */
    frame(qts);
    start(qts, 0);
    g_assert_true(took(qts, 0));
    g_assert_false(took(qts, 1));
    qtest_clock_step(qts, 10 * MS);
    start(qts, 1);
    g_assert_true(took(qts, 1));
    take_status(qts);
    qtest_clock_step(qts, 25 * MS);
    g_assert_cmphex(take_status(qts) & (INT_VPU_END(0) | INT_VPU_END(1) |
                                        INT_FRM_DONE | INT_DDR_UV_OVF),
                    ==, INT_VPU_END(0) | INT_VPU_END(1) | INT_FRM_DONE);
    qtest_quit(qts);
}

static void test_second_waits(void)
{
    QTestState *qts = ivdc_init();

    /* the JPEG engine starts when the H.264's frame has gone: the next */
    frame(qts);
    start(qts, 0);
    qtest_clock_step(qts, 35 * MS);
    g_assert_cmphex(take_status(qts) & INT_VPU_END(0), ==, INT_VPU_END(0));
    start(qts, 1);
    qtest_clock_step(qts, 100 * MS);
    g_assert_cmphex(take_status(qts) & (INT_VPU_END(1) | INT_DDR_UV_OVF), ==,
                    0);
    frame(qts);
    g_assert_true(took(qts, 1));
    qtest_clock_step(qts, 120 * MS);    /* 3/4 of the 135 ms since the last */
    g_assert_cmphex(take_status(qts) & (INT_VPU_END(0) | INT_VPU_END(1) |
                                        INT_FRM_DONE | INT_VPU_ACK_TO),
                    ==, INT_VPU_END(1) | INT_FRM_DONE);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ingenic-ivdc-dual/t32/both-at-start", test_both_at_start);
    qtest_add_func("/ingenic-ivdc-dual/t32/second-joins", test_second_joins);
    qtest_add_func("/ingenic-ivdc-dual/t32/second-waits", test_second_waits);
    return g_test_run();
}
