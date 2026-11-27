/*
 * QTest testcase for the size, the JEDEC ID and the address wrap of the
 * SPI NOR behind the T31's and the T40's SFC
 *
 * An 8 MiB image is an 8 MiB W25Q64 (EF 40 17), a 16 MiB one a W25Q128 and
 * a 32 MiB one a GD25Q256. The part ignores the address bits above its
 * capacity, so a read, a program and an erase beyond the end land at the
 * same offset modulo the size, and a read that runs off the end continues
 * at 0.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define SFC             0x13440000
#define SFC_GLB         0x00
#define SFC_TRAN_CONF0  0x14
#define SFC_TRAN_LEN    0x2c
#define SFC_DEV_ADDR0   0x30
#define SFC_TRIG        0x64
#define SFC_SR          0x68
#define SFC_SCR         0x6c
#define SFC_DR          0x1000

#define GLB_TRAN_DIR    (1 << 13)
#define TRAN_DATEEN     (1 << 16)
#define TRIG_START      1
#define SR_END          (1 << 4)

#define MiB             (1024 * 1024)
#define SPL_MAGIC       0x03040506
#define MARK            0xcafef00d

static uint32_t cmd(QTestState *qts, uint8_t op, uint32_t addr, uint32_t len,
                    bool out, uint32_t data)
{
    uint32_t v = 0;

    qtest_writel(qts, SFC + SFC_SCR, 0x1f);
    qtest_writel(qts, SFC + SFC_GLB, out ? GLB_TRAN_DIR : 0);
    qtest_writel(qts, SFC + SFC_TRAN_CONF0, op | (len ? TRAN_DATEEN : 0));
    qtest_writel(qts, SFC + SFC_TRAN_LEN, len);
    qtest_writel(qts, SFC + SFC_DEV_ADDR0, addr);
    qtest_writel(qts, SFC + SFC_TRIG, TRIG_START);
    if (len && out) {
        qtest_writel(qts, SFC + SFC_DR, data);
    } else if (len) {
        v = qtest_readl(qts, SFC + SFC_DR);
    }
    g_assert_cmphex(qtest_readl(qts, SFC + SFC_SR) & SR_END, ==, SR_END);
    return v;
}

static void wren(QTestState *qts)
{
    cmd(qts, 0x06, 0, 0, false, 0);
}

static QTestState *start(const char *machine, unsigned mib, char **img)
{
    g_autofree uint8_t *flash = g_malloc(mib * MiB);
    int fd = g_file_open_tmp("ingenic-sfc-nor-XXXXXX", img, NULL);

    g_assert_cmpint(fd, >=, 0);
    memset(flash, 0xff, mib * MiB);
    stl_le_p(flash, SPL_MAGIC);
    stl_le_p(flash + 12, 0x100);
    stl_le_p(flash + 0x1000, MARK);
    g_assert_cmpint(write(fd, flash, mib * MiB), ==, mib * MiB);
    close(fd);
    return qtest_initf("-machine %s -drive if=none,format=raw,file=%s",
                       machine, *img);
}

static void test_id(const char *machine)
{
    static const struct {
        unsigned mib;
        uint32_t id;
    } parts[] = {
        { 8, 0x1740ef }, { 16, 0x1840ef }, { 32, 0x1940c8 },
    };

    for (int i = 0; i < ARRAY_SIZE(parts); i++) {
        g_autofree char *img = NULL;
        QTestState *qts = start(machine, parts[i].mib, &img);

        g_assert_cmphex(cmd(qts, 0x9f, 0, 3, false, 0) & 0xffffff, ==,
                        parts[i].id);
        qtest_quit(qts);
        unlink(img);
    }
}

static void test_wrap(const char *machine)
{
    g_autofree char *img = NULL;
    QTestState *qts = start(machine, 8, &img);
    const uint32_t end = 8 * MiB;

    g_assert_cmphex(cmd(qts, 0x03, 0x1000, 4, false, 0), ==, MARK);
    g_assert_cmphex(cmd(qts, 0x03, end + 0x1000, 4, false, 0), ==, MARK);
    g_assert_cmphex(cmd(qts, 0x03, 3 * end + 0x1000, 4, false, 0), ==, MARK);
    g_assert_cmphex(cmd(qts, 0x03, end, 4, false, 0), ==, SPL_MAGIC);

    /* a program and an erase beyond the end hit the low addresses */
    wren(qts);
    cmd(qts, 0x02, end + 0x2000, 4, true, 0x12345678);
    g_assert_cmphex(cmd(qts, 0x03, 0x2000, 4, false, 0), ==, 0x12345678);
    wren(qts);
    cmd(qts, 0x20, end + 0x1000, 0, false, 0);
    g_assert_cmphex(cmd(qts, 0x03, 0x1000, 4, false, 0), ==, 0xffffffff);

    qtest_quit(qts);
    unlink(img);
}

/* RDSFDP: the address, then 8 dummy clocks the controller ignores */
static void read_sfdp(QTestState *qts, uint32_t addr, uint8_t *buf,
                      unsigned len)
{
    qtest_writel(qts, SFC + SFC_SCR, 0x1f);
    qtest_writel(qts, SFC + SFC_GLB, 0);
    qtest_writel(qts, SFC + SFC_TRAN_CONF0, 0x5a | TRAN_DATEEN);
    qtest_writel(qts, SFC + SFC_TRAN_LEN, len);
    qtest_writel(qts, SFC + SFC_DEV_ADDR0, addr);
    qtest_writel(qts, SFC + SFC_TRIG, TRIG_START);
    for (unsigned i = 0; i < len; i += 4) {
        stl_le_p(buf + i, qtest_readl(qts, SFC + SFC_DR));
    }
    g_assert_cmphex(qtest_readl(qts, SFC + SFC_SR) & SR_END, ==, SR_END);
}

/* What mainline spi-nor parses of it: the sizes, erases, reads, 4 bytes */
static void check_sfdp(QTestState *qts, uint64_t size)
{
    uint8_t t[0x50], again[8];
    uint32_t bfpt[16], dens;
    unsigned ptp;

    read_sfdp(qts, 0, t, sizeof(t));
    g_assert_cmphex((uint32_t)ldl_le_p(t), ==, 0x50444653);       /* "SFDP" */
    g_assert_cmpuint(t[5], ==, 1);                      /* JESD216 major */
    g_assert_cmpuint(t[4], >=, 6);                      /* rev B: 16 DWORDs */
    g_assert_cmpuint(t[6] + 1, ==, 1);                  /* NPH */
    g_assert_cmpuint(t[8], ==, 0);                      /* BFPT */
    g_assert_cmpuint(t[11], >=, 16);
    ptp = ldl_le_p(t + 12) & 0xffffff;
    g_assert_cmpuint(ptp + 64, <=, sizeof(t));
    for (int i = 0; i < 16; i++) {
        bfpt[i] = ldl_le_p(t + ptp + 4 * i);
    }
    /* 4 KiB erase opcode 0x20, dual and quad reads, 3 or 4 byte address */
    g_assert_cmpuint(bfpt[0] & 3, ==, 1);
    g_assert_cmphex(bfpt[0] >> 8 & 0xff, ==, 0x20);
    g_assert(bfpt[0] & 1u << 16 && bfpt[0] & 1u << 21 && bfpt[0] & 1u << 22);
    g_assert_cmpuint(bfpt[0] >> 17 & 3, ==, size > 16 * MiB ? 1 : 0);
    dens = bfpt[1];
    g_assert_cmpuint(dens & 0x80000000 ? 1ull << (dens & 0x7fffffff) :
                     (uint64_t)dens + 1, ==, size * 8);
    g_assert_cmphex(bfpt[2], ==, 0x6b08eb44);
    g_assert_cmphex(bfpt[3] & 0xff00ff00, ==, 0xbb003b00);
    /* erase types: 4 KiB 20h, 32 KiB 52h, 64 KiB D8h */
    g_assert_cmphex(bfpt[7], ==, 0x520f200c);
    g_assert_cmphex(bfpt[8] & 0xffff, ==, 0xd810);
    g_assert_cmpuint(bfpt[10] >> 4 & 15, ==, 8);        /* 256 byte pages */
    /* B7h and E9h for 4-byte addressing */
    g_assert(bfpt[15] & 1u << 24 && bfpt[15] & 1u << 14);

    /* an offset reads on from it, and past the table all ones */
    read_sfdp(qts, 0x10, again, 8);
    g_assert_cmphex((uint32_t)ldl_le_p(again), ==, bfpt[0]);
    read_sfdp(qts, 0x60, again, 8);
    g_assert_cmphex((uint32_t)ldl_le_p(again), ==, 0xffffffff);
}

static void test_sfdp(const char *machine)
{
    for (unsigned mib = 8; mib <= 64; mib *= 2) {
        g_autofree char *img = NULL;
        QTestState *qts = start(machine, mib, &img);

        check_sfdp(qts, (uint64_t)mib * MiB);
        qtest_quit(qts);
        unlink(img);
    }
}

static void t31_sfdp(void)
{
    test_sfdp("ingenic-t31");
}

static void t40_sfdp(void)
{
    test_sfdp("ingenic-t40");
}

static void t41_sfdp(void)
{
    test_sfdp("ingenic-t41");
}

static void a1_sfdp(void)
{
    QTestState *qts = qtest_init("-machine ingenic-a1");

    check_sfdp(qts, 16 * MiB);
    qtest_quit(qts);
}

static void t31_id(void)
{
    test_id("ingenic-t31");
}

static void t31_wrap(void)
{
    test_wrap("ingenic-t31");
}

static void t40_id(void)
{
    test_id("ingenic-t40");
}

static void t40_wrap(void)
{
    test_wrap("ingenic-t40");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("ingenic/sfc-nor/t31/id", t31_id);
    qtest_add_func("ingenic/sfc-nor/t31/wrap", t31_wrap);
    qtest_add_func("ingenic/sfc-nor/t40/id", t40_id);
    qtest_add_func("ingenic/sfc-nor/t40/wrap", t40_wrap);
    qtest_add_func("ingenic/sfc-nor/t31/sfdp", t31_sfdp);
    qtest_add_func("ingenic/sfc-nor/t40/sfdp", t40_sfdp);
    qtest_add_func("ingenic/sfc-nor/t41/sfdp", t41_sfdp);
    qtest_add_func("ingenic/sfc-nor/a1/sfdp", a1_sfdp);
    return g_test_run();
}
