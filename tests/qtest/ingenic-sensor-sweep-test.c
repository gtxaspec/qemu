/*
 * QTest testcase for the image sensors' exposure: for every model that
 * follows its exposure, the brightness of a T20's frame for the scene's
 * white bar rises with the integration time the vendor driver writes, from
 * a line to the exposure that shows the scene as rendered, and is
 * proportional to it (the luma is gamma-encoded, 2.2: the linear light is
 * (luma - 16) / 164 to that power), and rises with each step of the gain
 * the driver's table has, by that step's factor.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "libqtest.h"

#define ISP             0x13300000
#define CPM_CLKGR0      0x10000020
#define I2C0            0x10050000
#define T20_CORE        (ISP + 0x80000)
#define T20_WR          (T20_CORE + 0xb00)
#define T20_WR_UV       (T20_WR + 0x80)
#define BUF             0x00400000
#define W               64
#define H               64
#define FRAME_NS        40000000        /* fps=25 */
#define SETTLE          10              /* frames: the slowest apply delay */

typedef struct Reg {
    uint16_t reg;
    uint8_t val;
} Reg;

/* a gain the driver's table has: its registers and the factor over 1x */
typedef struct Step {
    Reg w[4];
    double f;
} Step;

typedef struct Row {
    const char *model;
    uint8_t addr;
    int reg_bytes;
    Reg stream;                 /* what starts its frames, if anything */
    Reg pre[6];                 /* the page, 1x gain */
    Reg post;                   /* the update that lets a page's writes land */
    /* the integration time's registers, as the bus has them */
    struct {
        uint16_t reg;
        uint8_t mask;
        uint8_t shift;
    } it[3];
    int frac_bits;
    /* lines are the frame length less the field: the length, its registers */
    uint16_t vts;
    uint16_t vts_reg[2];
    uint32_t ref;               /* lines at 1x that show the scene as is */
    Step gain[30];
} Row;

#define SOI_IT          .it = { { 0x02, 0xff, 8 }, { 0x01, 0xff, 0 } }
#define SM_IT           .it = { { 0x3e00, 0x0f, 16 }, { 0x3e01, 0xff, 8 }, \
                                { 0x3e02, 0xf0, 0 } }
#define OS_IT           .it = { { 0x03, 0xff, 8 }, { 0x04, 0xff, 0 } }
#define SC_STREAM       { 0x0100, 1 }

/* the SOI's code 0x00: a power of two in bits 6:4 times 1 + bits 3:0 / 16 */
#define SOI_GAIN \
    .gain = { { { { 0x00, 0x00 } }, 1 }, { { { 0x00, 0x08 } }, 1.5 }, \
              { { { 0x00, 0x10 } }, 2 }, { { { 0x00, 0x20 } }, 4 }, \
              { { { 0x00, 0x30 } }, 8 }, { { { 0x00, 0x40 } }, 16 } }

/* the SC2336's coarse codes at reg, the fine gain 0x80 at 0x3e07 */
#define SC_COARSE(reg) \
    .pre = { { reg, 0x00 }, { 0x3e07, 0x80 } }, \
    .gain = { { { { reg, 0x00 } }, 1 }, { { { reg, 0x08 } }, 2 }, \
              { { { reg, 0x09 } }, 4 }, { { { reg, 0x0b } }, 8 }, \
              { { { reg, 0x0f } }, 16 } }

/*
 * SmartSens' coarse 0x3e08 bits 4:2, ones from the bottom, with fine at 0x3e09
 */
#define SC_FINE(one) \
    .pre = { { 0x3e08, 0x03 }, { 0x3e09, one } }, \
    .gain = { { { { 0x3e08, 0x03 } }, 1 }, { { { 0x3e08, 0x07 } }, 2 }, \
              { { { 0x3e08, 0x0b } }, 3 }, { { { 0x3e08, 0x0f } }, 4 }, \
              { { { 0x3e08, 0x1f } }, 8 }, \
              { { { 0x3e08, 0x03 }, { 0x3e09, 2 * one } }, 2 } }

/* the integration time: octaves up to the exposure of the reference */
#define IT_STEPS        16

static QTestState *start(const Row *r)
{
    QTestState *qts = qtest_initf("-machine ingenic-t20 -device image-sensor,"
                                  "mclk-gate=off,"
                                  "bus=i2c0,fps=25,motion=off,model=%s",
                                  r->model);

    qtest_writel(qts, CPM_CLKGR0, 0);
    qtest_writel(qts, T20_CORE + 0x10, W);
    qtest_writel(qts, T20_CORE + 0x14, H);
    qtest_writel(qts, T20_WR, 13 | 1 << 9);             /* NV12, on */
    qtest_writel(qts, T20_WR + 4, H << 16 | W);
    qtest_writel(qts, T20_WR + 8, BUF);
    qtest_writel(qts, T20_WR + 0x20, W);
    qtest_writel(qts, T20_WR + 0x24, 2);
    qtest_writel(qts, T20_WR_UV, 1 << 9);
    qtest_writel(qts, T20_WR_UV + 4, H / 2 << 16 | W);
    qtest_writel(qts, T20_WR_UV + 8, BUF + W * H);
    qtest_writel(qts, T20_WR_UV + 0x20, W);
    qtest_writel(qts, T20_WR_UV + 0x24, 2);
    qtest_writel(qts, ISP, 1);                          /* VIC start */
    qtest_clock_step(qts, FRAME_NS * 2);
    return qts;
}

static void write_regs(QTestState *qts, const Row *r, const Reg *w, int n)
{
    qtest_writel(qts, I2C0 + 0x6c, 0);
    qtest_writel(qts, I2C0 + 0x04, r->addr);
    qtest_writel(qts, I2C0 + 0x6c, 1);
    for (int i = 0; i < n && (w[i].reg || w[i].val); i++) {
        if (r->reg_bytes == 2) {
            qtest_writel(qts, I2C0 + 0x10, w[i].reg >> 8);
        }
        qtest_writel(qts, I2C0 + 0x10, w[i].reg & 0xff);
        qtest_writel(qts, I2C0 + 0x10, 0x200 | w[i].val);
    }
    qtest_clock_step(qts, FRAME_NS * SETTLE);
}

/* the white bar's luma in the next frame */
static int white(QTestState *qts)
{
    uint8_t y;

    qtest_clock_step(qts, FRAME_NS);
    qtest_memread(qts, BUF + 10 * W + 4, &y, 1);
    return y;
}

/* the linear light a luma stands for, 1 at the white bar as rendered */
static double linear(int y)
{
    return pow(MAX(y - 16, 0) / 164.0, 2.2);
}

static void set_lines(QTestState *qts, const Row *r, uint32_t lines)
{
    uint64_t field = r->vts ? r->vts - lines : (uint64_t)lines << r->frac_bits;
    Reg w[5];
    int n = 0;

    for (int i = 0; i < 3 && r->it[i].mask; i++) {
        w[n].reg = r->it[i].reg;
        w[n++].val = (field >> r->it[i].shift) & r->it[i].mask;
    }
    if (r->post.reg) {
        w[n++] = r->post;
    }
    write_regs(qts, r, w, n);
}

static QTestState *setup(const Row *r)
{
    QTestState *qts = start(r);
    Reg v[2];

    write_regs(qts, r, &r->stream, 1);
    write_regs(qts, r, r->pre, ARRAY_SIZE(r->pre));
    if (r->vts_reg[0]) {
        v[0] = (Reg){ r->vts_reg[0], r->vts >> 8 };
        v[1] = (Reg){ r->vts_reg[1], r->vts & 0xff };
        write_regs(qts, r, v, 2);
    }
    return qts;
}

static void test_it(const void *data)
{
    const Row *r = data;
    QTestState *qts = setup(r);
    int prev = 0;
    uint32_t prev_lines = 0;
    GString *log = g_string_new(r->model);

    g_string_append(log, ":");
    for (uint32_t lines = 1; lines <= r->ref; lines *= 2) {
        int y;

        set_lines(qts, r, lines);
        y = white(qts);
        g_string_append_printf(log, " %u:%d", lines, y);
        if (prev && prev < 225) {
            g_assert_cmpint(y, >, prev);
            if (prev >= 30 && y <= 220) {
                double ratio = linear(y) / linear(prev) *
                               prev_lines / lines;

                g_assert_cmpfloat(ratio, >, 0.75);
                g_assert_cmpfloat(ratio, <, 1.35);
            }
        }
        prev = y;
        prev_lines = lines;
    }
    g_test_message("%s", log->str);
    g_string_free(log, true);
    qtest_quit(qts);
}

static void test_gain(const void *data)
{
    const Row *r = data;
    QTestState *qts = setup(r);
    double top = 1, base = 0;
    int first = 0, n = 0;
    GString *log = g_string_new(r->model);

    for (n = 0; n < ARRAY_SIZE(r->gain) && r->gain[n].f; n++) {
        top = MAX(top, r->gain[n].f);
    }
    /* the top gain shows 80% of the exposure as rendered */
    set_lines(qts, r, MAX(lround(0.8 * r->ref / top), 1));
    g_string_append(log, ":");
    for (int i = 0; i < n; i++) {
        const Step *s = &r->gain[i];
        int y;

        write_regs(qts, r, s->w, ARRAY_SIZE(s->w));
        if (r->post.reg) {
            write_regs(qts, r, &r->post, 1);
        }
        y = white(qts);
        g_string_append_printf(log, " x%g:%d", s->f, y);
        if (i == 0) {
            first = y;
            base = linear(y) / s->f;
            g_assert_cmpint(first, >=, 30);
            continue;
        }
        g_assert_cmpfloat(linear(y) / s->f / base, >, 0.8);
        g_assert_cmpfloat(linear(y) / s->f / base, <, 1.25);
    }
    g_test_message("%s", log->str);
    g_string_free(log, true);
    qtest_quit(qts);
}

/*
 * the T41 driver's table of the GC4023: registers 0x0614, 0x0615, 0xb8, 0xb9
 */
#define GC4023_ROW(a, b, c, d, g) \
    { { { 0x0614, a }, { 0x0615, b }, { 0x00b8, c }, { 0x00b9, d } }, \
      g / 1024.0 }

static const Row rows[] = {
    { .model = "jxh42", .addr = 0x30, .reg_bytes = 1, SOI_IT, SOI_GAIN,
      .ref = 405 },
    { .model = "jxh62", .addr = 0x30, .reg_bytes = 1, SOI_IT, SOI_GAIN,
      .ref = 405 },
    { .model = "jxf22", .addr = 0x40, .reg_bytes = 1, SOI_IT, SOI_GAIN,
      .ref = 600 },
    { .model = "jxf23", .addr = 0x40, .reg_bytes = 1, SOI_IT, SOI_GAIN,
      .ref = 600 },
    { .model = "jxf37", .addr = 0x40, .reg_bytes = 1, SOI_IT, SOI_GAIN,
      .ref = 600 },
    { .model = "jxq03", .addr = 0x40, .reg_bytes = 1, SOI_IT, SOI_GAIN,
      .ref = 720 },
    { .model = "jxq03p", .addr = 0x40, .reg_bytes = 1, SOI_IT, SOI_GAIN,
      .ref = 720 },
    { .model = "jxh63p", .addr = 0x40, .reg_bytes = 1, SOI_IT, SOI_GAIN,
      .ref = 1350 },
    /*
     * GC2053 and GC2063: lines at 0x03:0x04, the pre-gain 0xb1:0xb2, the table
     * by 0xb4:0xb3
     */
    { .model = "gc2053", .addr = 0x37, .reg_bytes = 1,
      .pre = { { 0xb1, 0x01 } }, .it = { { 0x03, 0x3f, 8 }, { 0x04, 0xff, 0 } },
      .ref = 600,
      .gain = { { { { 0xb3, 0x00 }, { 0xb4, 0x00 } }, 1 },
                { { { 0xb3, 0x10 }, { 0xb4, 0x00 } }, 1184 / 1024.0 },
                { { { 0xb3, 0x40 }, { 0xb4, 0x00 } }, 2032 / 1024.0 },
                { { { 0xb3, 0x80 }, { 0xb4, 0x00 } }, 4160 / 1024.0 },
                { { { 0xb3, 0xc0 }, { 0xb4, 0x00 } }, 8064 / 1024.0 } } },
    { .model = "gc2063", .addr = 0x37, .reg_bytes = 1,
      .pre = { { 0xb1, 0x01 } }, .it = { { 0x03, 0x3f, 8 }, { 0x04, 0xff, 0 } },
      .ref = 600,
      .gain = { { { { 0xb3, 0x00 }, { 0xb4, 0x00 } }, 1 },
                { { { 0xb3, 0x40 }, { 0xb4, 0x00 } }, 2032 / 1024.0 },
                { { { 0xb3, 0x80 }, { 0xb4, 0x00 } }, 4160 / 1024.0 } } },
    /* GC2083: lines at 0x0d03:0x0d04, the table by 0xd0, 0xb8, 0xb9 */
    { .model = "gc2083", .addr = 0x37, .reg_bytes = 2,
      .stream = { 0x023e, 0x99 },
      .pre = { { 0xd0, 0x00 }, { 0xb8, 0x01 }, { 0xb9, 0x00 } },
      .it = { { 0x0d03, 0xff, 8 }, { 0x0d04, 0xff, 0 } }, .ref = 600,
      .gain = { { { { 0xd0, 0x00 }, { 0xb8, 0x01 }, { 0xb9, 0x00 } }, 1 },
                { { { 0xd0, 0x01 }, { 0xb8, 0x01 }, { 0xb9, 0x1a } },
                  1440 / 1024.0 },
                { { { 0xd0, 0x02 }, { 0xb8, 0x02 }, { 0xb9, 0x00 } }, 2 },
                { { { 0xd0, 0x04 }, { 0xb8, 0x04 }, { 0xb9, 0x00 } }, 4 },
                { { { 0xd0, 0x44 }, { 0xb8, 0x08 }, { 0xb9, 0x00 } }, 8 },
                { { { 0xd0, 0x04 }, { 0xb8, 0x10 }, { 0xb9, 0x00 } }, 16 },
                { { { 0xd0, 0x44 }, { 0xb8, 0x20 }, { 0xb9, 0x00 } }, 32 } } },
    /* GC4653 and GC4663: the column gain 0x02b8 in whole steps, ref 1100 */
    { .model = "gc4653", .addr = 0x29, .reg_bytes = 2,
      .pre = { { 0x02b8, 0x01 } },
      .it = { { 0x0202, 0x3f, 8 }, { 0x0203, 0xff, 0 } }, .ref = 1100,
      .gain = { { { { 0x02b8, 0x01 } }, 1 }, { { { 0x02b8, 0x02 } }, 2 },
                { { { 0x02b8, 0x04 } }, 4 }, { { { 0x02b8, 0x08 } }, 8 },
                { { { 0x02b8, 0x10 } }, 16 } } },
    { .model = "gc4663", .addr = 0x29, .reg_bytes = 2,
      .pre = { { 0x02b8, 0x01 } },
      .it = { { 0x0202, 0x3f, 8 }, { 0x0203, 0xff, 0 } }, .ref = 1100,
      .gain = { { { { 0x02b8, 0x01 } }, 1 }, { { { 0x02b8, 0x04 } }, 4 } } },
    /* GC4023: the same lines, the T41 driver's gain table, ref 675 */
    { .model = "gc4023", .addr = 0x29, .reg_bytes = 2,
      .pre = { { 0x00b8, 0x01 } },
      .it = { { 0x0202, 0x3f, 8 }, { 0x0203, 0xff, 0 } }, .ref = 675,
      .gain = {
        GC4023_ROW(0x00, 0x00, 0x01, 0x00, 1024),
        GC4023_ROW(0x80, 0x02, 0x01, 0x0b, 1215),
        GC4023_ROW(0x01, 0x00, 0x01, 0x19, 1440),
        GC4023_ROW(0x81, 0x02, 0x01, 0x2a, 1696),
        GC4023_ROW(0x02, 0x00, 0x02, 0x00, 2048),
        GC4023_ROW(0x82, 0x02, 0x02, 0x17, 2432),
        GC4023_ROW(0x03, 0x00, 0x02, 0x33, 2863),
        GC4023_ROW(0x83, 0x02, 0x03, 0x14, 3391),
        GC4023_ROW(0x04, 0x00, 0x04, 0x00, 4096),
        GC4023_ROW(0x80, 0x02, 0x04, 0x2f, 4848),
        GC4023_ROW(0x01, 0x00, 0x05, 0x26, 5727),
        GC4023_ROW(0x81, 0x02, 0x06, 0x28, 6799),
        GC4023_ROW(0x02, 0x00, 0x08, 0x00, 8192),
        GC4023_ROW(0x82, 0x02, 0x09, 0x1e, 9712),
        GC4023_ROW(0x03, 0x00, 0x0b, 0x0c, 11472),
        GC4023_ROW(0x83, 0x02, 0x0d, 0x11, 13583),
        GC4023_ROW(0x04, 0x00, 0x10, 0x00, 16384),
        GC4023_ROW(0x84, 0x02, 0x12, 0x3d, 19408),
        GC4023_ROW(0x05, 0x00, 0x16, 0x19, 22944),
        GC4023_ROW(0x85, 0x02, 0x1a, 0x22, 27183),
        GC4023_ROW(0xb5, 0x04, 0x20, 0x00, 32768),
      } },
    /*
     * IMX307 and IMX327: the shutter 0x3022:0x3021:0x3020 less VMAX, 0.3 dB
     * steps
     */
    { .model = "imx307", .addr = 0x1a, .reg_bytes = 2, .stream = { 0x3000, 0 },
      .pre = { { 0x3014, 0x00 } },
      .it = { { 0x3020, 0xff, 0 }, { 0x3021, 0xff, 8 }, { 0x3022, 0x03, 16 } },
      .vts = 1125, .vts_reg = { 0x3019, 0x3018 }, .ref = 500,
      .gain = { { { { 0x3014, 0x00 } }, 1 },
                { { { 0x3014, 0x0a } }, 1.4125 },
                { { { 0x3014, 0x14 } }, 1.9953 },
                { { { 0x3014, 0x28 } }, 3.9811 },
                { { { 0x3014, 0x3c } }, 7.9433 } } },
    { .model = "imx327", .addr = 0x1a, .reg_bytes = 2, .stream = { 0x3000, 0 },
      .pre = { { 0x3014, 0x00 } },
      .it = { { 0x3020, 0xff, 0 }, { 0x3021, 0xff, 8 }, { 0x3022, 0x03, 16 } },
      .vts = 1125, .vts_reg = { 0x3019, 0x3018 }, .ref = 500,
      .gain = { { { { 0x3014, 0x00 } }, 1 },
                { { { 0x3014, 0x14 } }, 1.9953 },
                { { { 0x3014, 0x3c } }, 7.9433 } } },
    /*
     * CV2003 and CV2005: the shutter 0x304a:0x3049:0x3048 less the frame
     * length, 256 / (256 - code)
     */
    { .model = "cv2003", .addr = 0x35, .reg_bytes = 2, .stream = { 0x3000, 0 },
      .pre = { { 0x3154, 0x00 } },
      .it = { { 0x304a, 0xff, 16 }, { 0x3049, 0xff, 8 }, { 0x3048, 0xff, 0 } },
      .vts = 5161, .vts_reg = { 0x3021, 0x3020 }, .ref = 2325,
      .gain = { { { { 0x3154, 0x00 } }, 1 }, { { { 0x3154, 0x80 } }, 2 },
                { { { 0x3154, 0xc0 } }, 4 }, { { { 0x3154, 0xe0 } }, 8 } } },
    { .model = "cv2005", .addr = 0x35, .reg_bytes = 2, .stream = { 0x3000, 0 },
      .pre = { { 0x3118, 0x00 } },
      .it = { { 0x304a, 0xff, 16 }, { 0x3049, 0xff, 8 }, { 0x3048, 0xff, 0 } },
      .vts = 3624, .vts_reg = { 0x301d, 0x301c }, .ref = 1500,
      .gain = { { { { 0x3118, 0x00 } }, 1 }, { { { 0x3118, 0x80 } }, 2 },
                { { { 0x3118, 0xc0 } }, 4 }, { { { 0x3118, 0xe0 } }, 8 } } },
    /*
     * OmniVision's page 1: lines 0x03:0x04, the gain 0x24 in 1/16ths, an
     * update at 0x01
     */
#define OV_ROW(m, r) \
    { .model = m, .addr = 0x3c, .reg_bytes = 1, \
      .pre = { { 0xfd, 0x01 }, { 0x24, 0x10 } }, .post = { 0x01, 0x01 }, \
      OS_IT, .ref = r, \
      .gain = { { { { 0x24, 0x10 } }, 1 }, { { { 0x24, 0x20 } }, 2 }, \
                { { { 0x24, 0x40 } }, 4 }, { { { 0x24, 0x80 } }, 8 } } }
    OV_ROW("os02b10", 610),
    OV_ROW("os02g10", 600),
    OV_ROW("os03b10", 715),
    OV_ROW("ov2735b", 600),
    /* OS08C10: lines 0x3501:0x3502, the gain 0x3508:0x3509 in 1/128ths */
    { .model = "os08c10", .addr = 0x36, .reg_bytes = 2, .stream = SC_STREAM,
      .pre = { { 0x3508, 0x00 }, { 0x3509, 0x80 } },
      .it = { { 0x3501, 0xff, 8 }, { 0x3502, 0xff, 0 } }, .ref = 1250,
      .gain = { { { { 0x3508, 0x00 }, { 0x3509, 0x80 } }, 1 },
                { { { 0x3508, 0x01 }, { 0x3509, 0x00 } }, 2 },
                { { { 0x3508, 0x02 }, { 0x3509, 0x00 } }, 4 },
                { { { 0x3508, 0x04 }, { 0x3509, 0x00 } }, 8 } } },
    /* MIS20C1: lines 0x3100:0x3101, the gain code 0x3102:0x3103 */
    { .model = "mis20c1", .addr = 0x30, .reg_bytes = 2,
      .pre = { { 0x3102, 0x00 }, { 0x3103, 0x00 } }, .post = { 0x3008, 0x01 },
      .it = { { 0x3100, 0xff, 8 }, { 0x3101, 0xff, 0 } }, .ref = 505,
      .gain = { { { { 0x3102, 0x00 }, { 0x3103, 0x00 } }, 1 },
                { { { 0x3102, 0x02 }, { 0x3103, 0x00 } }, 2 },
                { { { 0x3102, 0x03 }, { 0x3103, 0x00 } }, 4 },
                { { { 0x3102, 0x03 }, { 0x3103, 0x80 } }, 8 } } },
    /*
     * PS5250 and PS5260: page 1, lines the frame length less 0x0c:0x0d, the
     * gain 0x83
     */
    { .model = "ps5250", .addr = 0x48, .reg_bytes = 1,
      .pre = { { 0xef, 0x01 }, { 0x83, 0x00 } }, .post = { 0x09, 0x01 },
      .it = { { 0x0c, 0xff, 8 }, { 0x0d, 0xff, 0 } }, .vts = 1349, .ref = 600,
      .gain = { { { { 0x83, 0x00 } }, 1 }, { { { 0x83, 0x10 } }, 2 },
                { { { 0x83, 0x20 } }, 4 }, { { { 0x83, 0x30 } }, 8 } } },
    { .model = "ps5260", .addr = 0x48, .reg_bytes = 1,
      .pre = { { 0xef, 0x01 }, { 0x18, 0x01 }, { 0x83, 0x00 } },
      .post = { 0x09, 0x01 },
      .it = { { 0x0c, 0xff, 8 }, { 0x0d, 0xff, 0 } }, .vts = 1124, .ref = 505,
      .gain = { { { { 0x83, 0x00 } }, 1 }, { { { 0x83, 0x10 } }, 2 },
                { { { 0x83, 0x20 } }, 4 }, { { { 0x83, 0x30 } }, 8 } } },
    /* SmartSens, half lines: SC2336's coarse and fine gain at 0x3e09 */
    { .model = "sc2336", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 5, .ref = 700, SC_COARSE(0x3e09) },
    { .model = "sc2336p", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 5, .ref = 700, SC_COARSE(0x3e09) },
    { .model = "sc2331", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 5, .ref = 510, SC_COARSE(0x3e08) },
    { .model = "sc4236", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 5, .ref = 1000,
      .pre = { { 0x3e08, 0x00 }, { 0x3e09, 0x10 }, { 0x3e07, 0x80 } },
      .gain = { { { { 0x3e08, 0x00 } }, 1 }, { { { 0x3e08, 0x04 } }, 2 },
                { { { 0x3e08, 0x0c } }, 4 }, { { { 0x3e08, 0x1c } }, 8 } } },
    { .model = "sc401ai", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 5, .ref = 810,
      .pre = { { 0x3e08, 0x03 }, { 0x3e09, 0x40 } },
      .gain = { { { { 0x3e08, 0x03 } }, 1 },
                { { { 0x3e08, 0x23 } }, 1504 / 1024.0 },
                { { { 0x3e08, 0x27 } }, 3008 / 1024.0 },
                { { { 0x3e08, 0x2f } }, 6016 / 1024.0 } } },
    { .model = "sc223a", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 5, .ref = 650,
      .pre = { { 0x3e09, 0x00 }, { 0x3e07, 0x80 } },
      .gain = { { { { 0x3e09, 0x00 } }, 1 },
                { { { 0x3e09, 0x40 } }, 1853 / 1024.0 },
                { { { 0x3e09, 0x48 } }, 3706 / 1024.0 },
                { { { 0x3e09, 0x49 } }, 7412 / 1024.0 } } },
    /* the sixteenths of a line */
    { .model = "sc4336", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 675, SC_COARSE(0x3e09) },
    { .model = "sc4336p", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 675, SC_COARSE(0x3e09) },
    { .model = "sc1a4t", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 1350, SC_COARSE(0x3e09) },
    { .model = "sc3332", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 1225, SC_COARSE(0x3e09) },
    { .model = "sc5336", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 810, SC_COARSE(0x3e09) },
    { .model = "sc3336", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 735,
      .pre = { { 0x3e09, 0x00 }, { 0x3e07, 0x80 } },
      .gain = { { { { 0x3e09, 0x00 } }, 1 },
                { { { 0x3e09, 0x40 } }, 1556 / 1024.0 },
                { { { 0x3e09, 0x48 } }, 3112 / 1024.0 },
                { { { 0x3e09, 0x49 } }, 6225 / 1024.0 },
                { { { 0x3e09, 0x4b } }, 12450 / 1024.0 } } },
    { .model = "sc3338", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 735,
      .pre = { { 0x3e09, 0x00 }, { 0x3e07, 0x80 } },
      .gain = { { { { 0x3e09, 0x00 } }, 1 },
                { { { 0x3e09, 0x48 } }, 3112 / 1024.0 } } },
    { .model = "sc301iot", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 865,
      .pre = { { 0x3e09, 0x00 }, { 0x3e07, 0x80 } },
      .gain = { { { { 0x3e09, 0x00 } }, 1 },
                { { { 0x3e09, 0x40 } }, 1606 / 1024.0 },
                { { { 0x3e09, 0x48 } }, 3212 / 1024.0 },
                { { { 0x3e09, 0x49 } }, 6425 / 1024.0 },
                { { { 0x3e09, 0x4b } }, 12850 / 1024.0 } } },
    { .model = "sc2300", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 1800,
      .pre = { { 0x3e08, 0x03 }, { 0x3e09, 0x20 } },
      .gain = { { { { 0x3e08, 0x03 } }, 1 }, { { { 0x3e08, 0x07 } }, 2 },
                { { { 0x3e08, 0x23 } }, 2458 / 1024.0 },
                { { { 0x3e08, 0x27 } }, 4915 / 1024.0 },
                { { { 0x3e08, 0x2f } }, 9830 / 1024.0 } } },
    { .model = "sc1235", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 540,
      .pre = { { 0x3e08, 0x00 }, { 0x3e09, 0x10 } },
      .gain = { { { { 0x3e09, 0x10 } }, 1 }, { { { 0x3e09, 0x20 } }, 2 },
                { { { 0x3e09, 0x40 } }, 4 }, { { { 0x3e09, 0x80 } }, 8 } } },
    { .model = "sc2235", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 650, SC_FINE(0x10) },
    { .model = "sc2332", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 700, SC_FINE(0x20) },
    { .model = "sc3235", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 700, SC_FINE(0x20) },
    { .model = "sc3335", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 4, .ref = 735, SC_FINE(0x40) },
    { .model = "sc5235", .addr = 0x30, .reg_bytes = 2, .stream = SC_STREAM,
      SM_IT, .frac_bits = 5, .ref = 1200, SC_FINE(0x20) },
};

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    for (int i = 0; i < ARRAY_SIZE(rows); i++) {
        g_autofree char *it = g_strdup_printf("/ingenic-sensor-sweep/%s-it",
                                              rows[i].model);
        g_autofree char *gain = g_strdup_printf("/ingenic-sensor-sweep/%s-gain",
                                                rows[i].model);

        qtest_add_data_func(it, &rows[i], test_it);
        if (rows[i].gain[0].f) {
            qtest_add_data_func(gain, &rows[i], test_gain);
        }
    }
    return g_test_run();
}
