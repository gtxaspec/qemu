/*
 * QTest testcase for the ATBM6441 "Hera" SPI controller and its flash
 *
 * SPI1 driven the way spi_exe_cmd() in the SDK's flash code does it:
 * wait for idle, reset the FIFOs, set TRANSCTRL, write the address and
 * the command; then the flash commands of flash.h (ID, status, write
 * enable, page program, the three erases, reads, deep power down) and the
 * execute-in-place windows that show the same array.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define IFSET       0x10
#define TRANSCTRL   0x20
#define CMD         0x24
#define ADDR        0x28
#define DATA        0x2c
#define CTRL        0x30
#define ST          0x34
#define INTEN       0x38
#define INTST       0x3c
#define HWCFG       0x7c

#define MODE_WR         1
#define MODE_RD         2
#define MODE_WR_RD      3
#define MODE_NONE       7
#define MODE_DMY_RD     9

#define TC_CMD          BIT(30)
#define TC_ADDR         BIT(29)
#define TC_MODE(m)      ((m) << 24)
#define TC_WCNT(n)      (((n) - 1) << 12)
#define TC_DCNT(n)      (((n) - 1) << 9)
#define TC_RCNT(n)      ((n) - 1)

#define ST_BUSY         BIT(0)
#define ST_RXEM         BIT(14)
#define ST_TXEM         BIT(22)

#define CTRL_RST_FIFOS  0x6
#define INT_END         BIT(4)

#define MSEC            1000000LL

static uint32_t rd(QTestState *qts, uint32_t base, uint32_t reg)
{
    return qtest_readl(qts, base + reg);
}

static void wr(QTestState *qts, uint32_t base, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, base + reg, v);
}

/* the start of spi_exe_cmd(): idle, then both FIFOs reset */
static void begin(QTestState *qts)
{
    g_assert_cmphex(rd(qts, HERA_SPI1, ST) & ST_BUSY, ==, 0);
    wr(qts, HERA_SPI1, CTRL, CTRL_RST_FIFOS);
}

/* a command with no data, as flash_write_enable() sends it */
static void cmd_only(QTestState *qts, uint8_t cmd)
{
    begin(qts);
    wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_MODE(MODE_NONE));
    wr(qts, HERA_SPI1, CMD, cmd);
    g_assert_cmphex(rd(qts, HERA_SPI1, ST) & ST_BUSY, ==, 0);
}

/* a command with n bytes read back, no address */
static uint32_t cmd_read(QTestState *qts, uint8_t cmd, int n)
{
    begin(qts);
    wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_MODE(MODE_RD) | TC_RCNT(n));
    wr(qts, HERA_SPI1, CMD, cmd);
    return rd(qts, HERA_SPI1, DATA);
}

static uint8_t status(QTestState *qts)
{
    return cmd_read(qts, 0x05, 1) & 0xff;
}

static void wait_idle(QTestState *qts)
{
    for (int i = 0; i < 1000 && (status(qts) & 1); i++) {
        qtest_clock_step(qts, 1 * MSEC);
    }
    g_assert_cmphex(status(qts) & 1, ==, 0);
}

/* a command, a 24-bit address and a read of n bytes into words */
static void read_flash(QTestState *qts, uint8_t cmd, uint32_t addr,
                       uint8_t *buf, int n)
{
    begin(qts);
    wr(qts, HERA_SPI1, ADDR, addr);
    wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_ADDR | TC_MODE(MODE_RD) |
       TC_RCNT(n));
    wr(qts, HERA_SPI1, CMD, cmd);
    for (int i = 0; i < n; i += 4) {
        uint32_t w = rd(qts, HERA_SPI1, DATA);

        for (int j = 0; j < 4 && i + j < n; j++) {
            buf[i + j] = w >> (8 * j);
        }
    }
}

/* a page program: command, address, then the words of the data */
static void program(QTestState *qts, uint32_t addr, const uint8_t *buf, int n)
{
    cmd_only(qts, 0x06);
    begin(qts);
    wr(qts, HERA_SPI1, ADDR, addr);
    wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_ADDR | TC_MODE(MODE_WR) |
       TC_WCNT(n));
    wr(qts, HERA_SPI1, CMD, 0x02);
    for (int i = 0; i < n; i += 4) {
        uint32_t w = 0;

        for (int j = 0; j < 4 && i + j < n; j++) {
            w |= (uint32_t)buf[i + j] << (8 * j);
        }
        wr(qts, HERA_SPI1, DATA, w);
    }
    g_assert_cmphex(rd(qts, HERA_SPI1, ST) & ST_BUSY, ==, 0);
    wait_idle(qts);
}

static void erase(QTestState *qts, uint8_t cmd, uint32_t addr)
{
    cmd_only(qts, 0x06);
    begin(qts);
    wr(qts, HERA_SPI1, ADDR, addr);
    wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_ADDR | TC_MODE(MODE_NONE));
    wr(qts, HERA_SPI1, CMD, cmd);
    wait_idle(qts);
}

static void test_reset(void)
{
    QTestState *qts = hera_init("");

    g_assert_cmphex(rd(qts, HERA_SPI1, HWCFG), ==, 0x00001711);
    g_assert_cmphex(rd(qts, HERA_SPI1, ST), ==, ST_RXEM | ST_TXEM);
    g_assert_cmphex(rd(qts, HERA_SPI1, IFSET) & 0x3ff80, ==,
                    (2 << 16) | (7 << 8) | BIT(7));
    g_assert_cmphex(rd(qts, HERA_SPI1, INTEN), ==, 0);
    g_assert_cmphex(rd(qts, HERA_SPI3, HWCFG), ==, 0x00001711);
    qtest_quit(qts);
}

static void test_id_and_status(void)
{
    QTestState *qts = hera_init("");
    uint8_t buf[8];

    /* RDID: c8 40 15 on the wire, first byte lowest in the merged word */
    g_assert_cmphex(cmd_read(qts, 0x9f, 3), ==, 0x001540c8);
    g_assert_cmphex(cmd_read(qts, 0x9f, 3), ==, 0x001540c8);

    /* 90: the address is 0: manufacturer, then device */
    read_flash(qts, 0x90, 0, buf, 2);
    g_assert_cmphex(buf[0], ==, 0xc8);
    g_assert_cmphex(buf[1], ==, 0x14);

    g_assert_cmphex(status(qts), ==, 0);
    cmd_only(qts, 0x06);
    g_assert_cmphex(status(qts), ==, 0x02);
    cmd_only(qts, 0x04);
    g_assert_cmphex(status(qts), ==, 0);
    g_assert_cmphex(cmd_read(qts, 0x35, 1) & 0xff, ==, 0);

    /* write status 1 and 2 in one command: only the writable bits of 1 */
    cmd_only(qts, 0x06);
    begin(qts);
    wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_MODE(MODE_WR) | TC_WCNT(2));
    wr(qts, HERA_SPI1, CMD, 0x01);
    wr(qts, HERA_SPI1, DATA, 0x0203);
    wait_idle(qts);
    g_assert_cmphex(status(qts), ==, 0x00);
    g_assert_cmphex(cmd_read(qts, 0x35, 1) & 0xff, ==, 0x02);
    qtest_quit(qts);
}

static void test_program_erase(void)
{
    QTestState *qts = hera_init("");
    uint8_t out[16], in[16];

    for (int i = 0; i < 16; i++) {
        out[i] = 0x10 + i;
    }
    read_flash(qts, 0x03, 0x1000, in, 8);
    g_assert_cmphex(in[0], ==, 0xff);

    /* programming without write enable does nothing */
    begin(qts);
    wr(qts, HERA_SPI1, ADDR, 0x1000);
    wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_ADDR | TC_MODE(MODE_WR) |
       TC_WCNT(4));
    wr(qts, HERA_SPI1, CMD, 0x02);
    wr(qts, HERA_SPI1, DATA, 0);
    read_flash(qts, 0x03, 0x1000, in, 4);
    g_assert_cmphex(in[0], ==, 0xff);

    program(qts, 0x1000, out, 16);
    read_flash(qts, 0x03, 0x1000, in, 16);
    g_assert_cmpmem(in, 16, out, 16);

    /* busy for a while, and write enable is spent */
    cmd_only(qts, 0x06);
    begin(qts);
    wr(qts, HERA_SPI1, ADDR, 0x1000);
    wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_ADDR | TC_MODE(MODE_WR) |
       TC_WCNT(4));
    wr(qts, HERA_SPI1, CMD, 0x02);
    wr(qts, HERA_SPI1, DATA, 0x0f0f0f0f);
    g_assert_cmphex(status(qts), ==, 0x01);
    wait_idle(qts);
    g_assert_cmphex(status(qts), ==, 0);

    /* a program only clears bits */
    read_flash(qts, 0x03, 0x1000, in, 4);
    g_assert_cmphex(in[0], ==, 0x00);
    g_assert_cmphex(in[1], ==, 0x01);
    g_assert_cmphex(in[3], ==, 0x03);

    /* 4 KiB sector erase: the whole sector, not the next */
    program(qts, 0x1ffc, out, 4);
    program(qts, 0x2000, out, 4);
    erase(qts, 0x20, 0x1234);
    read_flash(qts, 0x03, 0x1000, in, 4);
    g_assert_cmphex(in[0], ==, 0xff);
    read_flash(qts, 0x03, 0x1ffc, in, 4);
    g_assert_cmphex(in[0], ==, 0xff);
    read_flash(qts, 0x03, 0x2000, in, 4);
    g_assert_cmphex(in[0], ==, 0x10);
    qtest_quit(qts);
}

/* a program wraps inside its 256-byte page */
static void test_page_wrap(void)
{
    QTestState *qts = hera_init("");
    uint8_t out[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, in[8];

    program(qts, 0x30fc, out, 8);
    read_flash(qts, 0x03, 0x30fc, in, 4);
    g_assert_cmpmem(in, 4, out, 4);
    read_flash(qts, 0x03, 0x3000, in, 4);
    g_assert_cmpmem(in, 4, out + 4, 4);
    read_flash(qts, 0x03, 0x3100, in, 4);
    g_assert_cmphex(in[0], ==, 0xff);
    qtest_quit(qts);
}

static void test_block_erase(void)
{
    QTestState *qts = hera_init("");
    uint8_t out[4] = { 0xaa, 0xaa, 0xaa, 0xaa }, in[4];
    static const uint32_t at[] = { 0x8000, 0xffff0, 0x10000, 0x20000,
                                   0x28000, 0x30000 };

    for (int i = 0; i < 6; i++) {
        program(qts, at[i], out, 4);
    }
    /* 32 KiB: 0x8000..0xffff (not 0x10000), the address inside the block */
    erase(qts, 0x52, 0x9000);
    read_flash(qts, 0x03, 0x8000, in, 4);
    g_assert_cmphex(in[0], ==, 0xff);
    read_flash(qts, 0x03, 0xffff0, in, 4);
    g_assert_cmphex(in[0], ==, 0xaa);
    /* 64 KiB: 0x20000..0x2ffff */
    erase(qts, 0xd8, 0x2fff0);
    read_flash(qts, 0x03, 0x20000, in, 4);
    g_assert_cmphex(in[0], ==, 0xff);
    read_flash(qts, 0x03, 0x28000, in, 4);
    g_assert_cmphex(in[0], ==, 0xff);
    read_flash(qts, 0x03, 0x30000, in, 4);
    g_assert_cmphex(in[0], ==, 0xaa);
    read_flash(qts, 0x03, 0x10000, in, 4);
    g_assert_cmphex(in[0], ==, 0xaa);
    /* chip erase */
    cmd_only(qts, 0x06);
    cmd_only(qts, 0xc7);
    wait_idle(qts);
    read_flash(qts, 0x03, 0x10000, in, 4);
    g_assert_cmphex(in[0], ==, 0xff);
    qtest_quit(qts);
}

/* the fast reads: the dummy bytes are the controller's to clock */
static void test_fast_reads(void)
{
    QTestState *qts = hera_init("");
    uint8_t out[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    uint32_t w;

    program(qts, 0x4000, out, 8);
    static const struct { uint8_t cmd; int dummy; } c[] = {
        { 0x0b, 1 }, { 0x3b, 1 }, { 0x6b, 1 }, { 0xbb, 1 }, { 0xeb, 3 },
    };

    for (int i = 0; i < 5; i++) {
        begin(qts);
        wr(qts, HERA_SPI1, ADDR, 0x4000);
        /* dual/quad bytes are the same bytes on this bus */
        wr(qts, HERA_SPI1, TRANSCTRL, TC_CMD | TC_ADDR |
           TC_MODE(MODE_DMY_RD) | TC_DCNT(c[i].dummy) | TC_RCNT(8));
        wr(qts, HERA_SPI1, CMD, c[i].cmd);
        w = rd(qts, HERA_SPI1, DATA);
        g_assert_cmphex(w, ==, 0x44332211);
        w = rd(qts, HERA_SPI1, DATA);
        g_assert_cmphex(w, ==, 0x88776655);
    }
    qtest_quit(qts);
}

static void test_xip(void)
{
    QTestState *qts = hera_init("");
    uint8_t out[4] = { 0xde, 0xad, 0xbe, 0xef };

    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x1000), ==, 0xffffffff);
    program(qts, 0x1000, out, 4);

    /* cached and uncached windows are the same flash */
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x1000), ==, 0xefbeadde);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH_NC + 0x1000), ==, 0xefbeadde);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x1004), ==, 0xffffffff);

    /* not writable by the CPU */
    qtest_writel(qts, HERA_FLASH + 0x1004, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x1004), ==, 0xffffffff);

    erase(qts, 0x20, 0x1000);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH_NC + 0x1000), ==, 0xffffffff);
    qtest_quit(qts);
}

static void test_deep_power_down(void)
{
    QTestState *qts = hera_init("");

    cmd_only(qts, 0xb9);
    cmd_only(qts, 0x06);
    g_assert_cmphex(status(qts), ==, 0);
    cmd_only(qts, 0xab);
    cmd_only(qts, 0x06);
    g_assert_cmphex(status(qts), ==, 0x02);
    qtest_quit(qts);
}

static void test_end_interrupt(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_SPI1, INTEN, INT_END);
    g_assert_cmphex(rd(qts, HERA_SPI1, INTST) & INT_END, ==, 0);
    cmd_only(qts, 0x06);
    g_assert_cmphex(rd(qts, HERA_SPI1, INTST) & INT_END, ==, INT_END);
    wr(qts, HERA_SPI1, INTST, INT_END);
    g_assert_cmphex(rd(qts, HERA_SPI1, INTST) & INT_END, ==, 0);
    qtest_quit(qts);
}

/* nobody on SPI3's bus: reads return 0 and the transaction ends */
static void test_spi3_idle(void)
{
    QTestState *qts = hera_init("");

    wr(qts, HERA_SPI3, CTRL, CTRL_RST_FIFOS);
    wr(qts, HERA_SPI3, TRANSCTRL, TC_CMD | TC_MODE(MODE_RD) | TC_RCNT(4));
    wr(qts, HERA_SPI3, CMD, 0x9f);
    g_assert_cmphex(rd(qts, HERA_SPI3, DATA), ==, 0);
    g_assert_cmphex(rd(qts, HERA_SPI3, ST) & ST_BUSY, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/spi/reset", test_reset);
    qtest_add_func("/atbm-hera/spi/id-and-status", test_id_and_status);
    qtest_add_func("/atbm-hera/spi/program-erase", test_program_erase);
    qtest_add_func("/atbm-hera/spi/page-wrap", test_page_wrap);
    qtest_add_func("/atbm-hera/spi/block-erase", test_block_erase);
    qtest_add_func("/atbm-hera/spi/fast-reads", test_fast_reads);
    qtest_add_func("/atbm-hera/spi/xip", test_xip);
    qtest_add_func("/atbm-hera/spi/deep-power-down", test_deep_power_down);
    qtest_add_func("/atbm-hera/spi/end-interrupt", test_end_interrupt);
    qtest_add_func("/atbm-hera/spi/spi3-idle", test_spi3_idle);
    return g_test_run();
}
