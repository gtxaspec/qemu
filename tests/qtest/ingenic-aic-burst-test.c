/*
 * QTest testcase for the T10 and T20 AIC receive FIFO read by PDMA bursts
 *
 * As measured on a T10L and a T20X with the vendor oss2 driver recording,
 * the first word of every RX DMA burst reaches memory doubled (the sample
 * plus itself): at 32-byte and 16-byte bursts alike, never with single
 * 2-byte transfers. A T30 with the same driver is clean. The mic is a slow
 * tone, so a neighbouring sample is within a few hundred of a clean one
 * and a doubled one is far from it.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define PDMA            0x13420000
#define DMAC            (PDMA + 0x1000)
#define CH0(r)          (PDMA + (r))
#define DSA             0x00
#define DTA             0x04
#define DTC             0x08
#define DRT             0x0c
#define DCS             0x10
#define DCM             0x14
#define DCS_GO          0x80000001
#define DCS_TT          0x00000008
#define DRT_AIC_RX      7

#define AIC             0x10020000
#define AICFR           0x00
#define AICCR           0x04
#define AICDR           0x34
#define DST             0x00600000
#define WORDS           128
#define CODEC_CACR2     0x10021008      /* bit 0: mono ADC */

typedef struct {
    const char *machine;
    unsigned tsz;               /* DCM.TSZ: 2 = 2 bytes, 3 = 16, 4 = 32 */
    unsigned burst;             /* bytes */
    bool doubled;
    bool stock;                 /* the vendor driver's setup, mono ADC */
} Case;

static const Case cases[] = {
    { "ingenic-t10", 4, 32, true, false },
    { "ingenic-t20", 4, 32, true, false },
    { "ingenic-t10", 3, 16, true, false },
    { "ingenic-t20", 3, 16, true, false },
    { "ingenic-t10", 2, 2, false, false },
    { "ingenic-t20", 2, 2, false, false },
    { "ingenic-t30", 4, 32, false, false },
    { "ingenic-t30", 3, 16, false, false },
    { "ingenic-t10", 4, 32, true, true },
    { "ingenic-t20", 4, 32, true, true },
    { "ingenic-t10", 3, 16, true, true },
    { "ingenic-t20", 3, 16, true, true },
    { "ingenic-t10", 2, 2, false, true },
    { "ingenic-t20", 2, 2, false, true },
};

static int16_t word(QTestState *qts, int i)
{
    return (int16_t)qtest_readw(qts, DST + 2 * i);
}

static void test_burst(const void *data)
{
    const Case *c = data;
    QTestState *qts = qtest_initf("-machine %s -audio none "
                                  "-global ingenic-aic.mic=tone:100:-6",
                                  c->machine);
    unsigned per = c->burst / 2;
    unsigned step = c->stock ? 2 : 1;

    /* RFTH 7 (16 entries), record, RX DMA, 16 bits: one entry a frame */
    qtest_writel(qts, AIC + AICFR, 1 | 7u << 24);
    if (c->stock) {
        /* the vendor driver's AICCR, two entries a frame, and a mono ADC */
        qtest_writel(qts, CODEC_CACR2, 1);
        qtest_writel(qts, AIC + AICCR, 0x01218001);
    } else {
        qtest_writel(qts, AIC + AICCR, 1 | 1 << 12 | 1 << 15 | 1 << 16);
    }
    qtest_writel(qts, DMAC, 1);
    qtest_writel(qts, CH0(DCS), 0);
    qtest_writel(qts, CH0(DSA), AIC + AICDR);
    qtest_writel(qts, CH0(DTA), DST);
    qtest_writel(qts, CH0(DTC), WORDS * 2 / c->burst);
    qtest_writel(qts, CH0(DRT), DRT_AIC_RX);
    qtest_writel(qts, CH0(DCM), 1u << 22 | 2 << 14 | 2 << 12 | c->tsz << 8);
    qtest_writel(qts, CH0(DCS), DCS_GO);
    for (int i = 0; i < 100 && !(qtest_readl(qts, CH0(DCS)) & DCS_TT); i++) {
        qtest_clock_step(qts, 100000);
    }
    g_assert_cmphex(qtest_readl(qts, CH0(DCS)) & DCS_TT, ==, DCS_TT);

    /*
     * Word 32 starts a burst whatever the size, the next sample (one or
     * two words on) never does. A stock frame is L then R: the mic is in L
     * and R reads 0.
     */
    for (int i = 32; i < WORDS - 2 * step; i += MAX(per, step)) {
        int a = word(qts, i), b = word(qts, i + step);

        g_assert_cmpint(b, >, 1000);
        g_assert_cmpint(abs((c->doubled ? 2 * b : b) - a), <, 500);
        g_assert_cmpint(abs(word(qts, i + 2 * step) - b), <, 500);
        if (c->stock) {
            g_assert_cmpint(word(qts, i + 1), ==, 0);
        }
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        char *name = g_strdup_printf("ingenic-aic-burst/%s/%u",
                                     cases[i].machine + 8, cases[i].burst);
        if (cases[i].stock) {
            g_free(name);
            name = g_strdup_printf("ingenic-aic-burst/%s/%u/stock",
                                   cases[i].machine + 8, cases[i].burst);
        }

        qtest_add_data_func(name, &cases[i], test_burst);
        g_free(name);
    }
    return g_test_run();
}
