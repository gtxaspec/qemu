/*
 * QTest testcase for the ATBM6441 "Hera" DMA controller
 *
 * Memory to memory with the address steps and widths, the status and
 * interrupt (chip number 7), the masks, abort, chained descriptors, and
 * the handshake transfers the flash code does through SPI1: a read of the
 * flash into memory on the receive request and a page program from memory
 * on the transmit request, with the DMA started before the SPI command.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define CFG         0x10
#define INTST       0x30
#define CHEN        0x34
#define CHABORT     0x40
#define CH(n, r)    (0x44 + 0x14 * (n) + (r))
#define CTRL        0x0
#define SRC         0x4
#define DST         0x8
#define SIZE        0xc
#define LLP         0x10

#define EN          BIT(0)
#define MASK_TC     BIT(1)
#define SRC_HS      BIT(17)
#define DST_HS      BIT(16)
#define W32         (2 << 20 | 2 << 18)
#define W16         (1 << 20 | 1 << 18)
#define W8          0
#define SRC_DEC     (1 << 14)
#define SRC_FIX     (2 << 14)
#define DST_FIX     (2 << 12)
#define SRCREQ(n)   ((n) << 8)
#define DSTREQ(n)   ((n) << 4)

#define TC(n)       BIT(16 + (n))
#define ABT(n)      BIT(8 + (n))

/* SPI1, for the handshake transfers */
#define SPI_IFSET   0x10
#define SPI_TRANSCTRL 0x20
#define SPI_CMD     0x24
#define SPI_ADDR    0x28
#define SPI_DATA    0x2c
#define SPI_CTRL    0x30
#define SPI_ST      0x34
#define TC_CMD      BIT(30)
#define TC_ADDR     BIT(29)
#define TC_MODE(m)  ((m) << 24)

static uint32_t rd(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, HERA_DMA + reg);
}

static void wr(QTestState *qts, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, HERA_DMA + reg, v);
}

static void channel(QTestState *qts, int n, uint32_t src, uint32_t dst,
                    uint32_t size, uint32_t ctrl)
{
    wr(qts, CH(n, SRC), src);
    wr(qts, CH(n, DST), dst);
    wr(qts, CH(n, SIZE), size);
    wr(qts, CH(n, LLP), 0);
    wr(qts, CH(n, CTRL), ctrl | EN);
}

static void fill(QTestState *qts, uint32_t addr, int n)
{
    for (int i = 0; i < n; i++) {
        qtest_writeb(qts, addr + i, 0x40 + i);
    }
}

static void test_reset(void)
{
    QTestState *qts = hera_init("");

    g_assert_cmphex(rd(qts, CFG), ==, 0x80000008);
    g_assert_cmphex(rd(qts, INTST), ==, 0);
    g_assert_cmphex(rd(qts, CHEN), ==, 0);
    g_assert_cmphex(rd(qts, CH(0, CTRL)), ==, 0);
    qtest_quit(qts);
}

/* the way AtbmDma_Submit starts one: src, dst, size in words, then enable */
static void test_copy(void)
{
    QTestState *qts = hera_init("");
    uint8_t in[64], out[64];

    fill(qts, HERA_ILM, 64);
    channel(qts, 0, HERA_ILM, HERA_DTCM, 16, W32);
    g_assert_cmphex(rd(qts, INTST), ==, TC(0));
    g_assert_true(hera_irq(qts, HERA_IRQ_DMA));
    g_assert_cmphex(rd(qts, CHEN), ==, 0);
    g_assert_cmphex(rd(qts, CH(0, CTRL)) & EN, ==, 0);
    qtest_memread(qts, HERA_ILM, in, 64);
    qtest_memread(qts, HERA_DTCM, out, 64);
    g_assert_cmpmem(in, 64, out, 64);
    g_assert_cmphex(qtest_readb(qts, HERA_DTCM + 64), ==, 0);
    g_assert_cmphex(rd(qts, CH(0, SRC)), ==, HERA_ILM + 64);
    g_assert_cmphex(rd(qts, CH(0, DST)), ==, HERA_DTCM + 64);

    /* the ISR reads the status and writes it back */
    wr(qts, INTST, rd(qts, INTST));
    g_assert_cmphex(rd(qts, INTST), ==, 0);
    g_assert_false(hera_irq(qts, HERA_IRQ_DMA));
    qtest_quit(qts);
}

static void test_widths_and_steps(void)
{
    QTestState *qts = hera_init("");

    fill(qts, HERA_ILM, 16);
    /* bytes, 5 of them */
    channel(qts, 1, HERA_ILM, HERA_DTCM, 5, W8);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM), ==, 0x43424140);
    g_assert_cmphex(qtest_readb(qts, HERA_DTCM + 4), ==, 0x44);
    g_assert_cmphex(qtest_readb(qts, HERA_DTCM + 5), ==, 0);
    g_assert_cmphex(rd(qts, INTST), ==, TC(1));
    wr(qts, INTST, TC(1));

    /* halfwords, the source stepping down */
    channel(qts, 2, HERA_ILM + 6, HERA_DTCM + 0x100, 4, W16 | SRC_DEC);
    g_assert_cmphex(qtest_readw(qts, HERA_DTCM + 0x100), ==, 0x4746);
    g_assert_cmphex(qtest_readw(qts, HERA_DTCM + 0x102), ==, 0x4544);
    g_assert_cmphex(qtest_readw(qts, HERA_DTCM + 0x104), ==, 0x4342);
    g_assert_cmphex(qtest_readw(qts, HERA_DTCM + 0x106), ==, 0x4140);
    wr(qts, INTST, TC(2));

    /* a fixed destination ends with the last word written to it */
    channel(qts, 3, HERA_ILM, HERA_DTCM + 0x200, 3, W32 | DST_FIX);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM + 0x200), ==, 0x4b4a4948);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM + 0x204), ==, 0);
    g_assert_cmphex(rd(qts, CH(3, DST)), ==, HERA_DTCM + 0x200);
    wr(qts, INTST, TC(3));
    qtest_quit(qts);
}

static void test_mask_and_abort(void)
{
    QTestState *qts = hera_init("");

    /* a masked complete interrupt leaves no status */
    channel(qts, 0, HERA_ILM, HERA_DTCM, 1, W32 | MASK_TC);
    g_assert_cmphex(rd(qts, INTST), ==, 0);
    g_assert_false(hera_irq(qts, HERA_IRQ_DMA));

    /* abort: a handshake channel on a request that is not coming */
    qtest_writel(qts, HERA_SPI1 + SPI_CTRL, BIT(3));
    channel(qts, 4, HERA_SPI1 + SPI_DATA, HERA_DTCM, 4, W32 | SRC_FIX |
            SRC_HS | SRCREQ(7));
    g_assert_cmphex(rd(qts, CHEN), ==, BIT(4));
    wr(qts, CHABORT, BIT(4));
    g_assert_cmphex(rd(qts, CHEN), ==, 0);
    g_assert_cmphex(rd(qts, INTST), ==, ABT(4));
    g_assert_true(hera_irq(qts, HERA_IRQ_DMA));
    wr(qts, INTST, ABT(4));
    g_assert_false(hera_irq(qts, HERA_IRQ_DMA));

    /* a transfer to a place nothing is at */
    channel(qts, 5, HERA_ILM, 0x50000000, 1, W32);
    g_assert_cmphex(rd(qts, INTST), ==, BIT(5));
    g_assert_cmphex(rd(qts, CHEN), ==, 0);
    qtest_quit(qts);
}

static void test_chain(void)
{
    QTestState *qts = hera_init("");
    uint32_t desc = HERA_SRAM + 0x1000;

    fill(qts, HERA_ILM, 32);
    /* the second descriptor: 4 bytes from ILM+16 to DTCM+0x40, the end */
    qtest_writel(qts, desc + 0x14 + 0x00, W8 | EN);
    qtest_writel(qts, desc + 0x14 + 0x04, HERA_ILM + 16);
    qtest_writel(qts, desc + 0x14 + 0x08, HERA_DTCM + 0x40);
    qtest_writel(qts, desc + 0x14 + 0x0c, 4);
    qtest_writel(qts, desc + 0x14 + 0x10, 0);
    /* the first: 8 bytes to DTCM, then the second */
    qtest_writel(qts, desc + 0x00, W8 | EN);
    qtest_writel(qts, desc + 0x04, HERA_ILM);
    qtest_writel(qts, desc + 0x08, HERA_DTCM);
    qtest_writel(qts, desc + 0x0c, 8);
    qtest_writel(qts, desc + 0x10, desc + 0x14);

    wr(qts, CH(0, SRC), HERA_ILM + 24);
    wr(qts, CH(0, DST), HERA_DTCM + 0x80);
    wr(qts, CH(0, SIZE), 1);
    wr(qts, CH(0, LLP), desc);
    wr(qts, CH(0, CTRL), W8 | EN);
    /* the channel's own transfer, then both descriptors */
    g_assert_cmphex(qtest_readb(qts, HERA_DTCM + 0x80), ==, 0x58);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM), ==, 0x43424140);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM + 4), ==, 0x47464544);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM + 0x40), ==, 0x53525150);
    g_assert_cmphex(rd(qts, CHEN), ==, 0);
    g_assert_cmphex(rd(qts, INTST), ==, TC(0));
    qtest_quit(qts);
}

/* flash_read() with DMA: the receive request paces the transfer */
static void test_flash_read_dma(void)
{
    QTestState *qts = hera_init("");
    uint32_t w;

    /* erased flash reads 0xff; put a pattern at 0x1000 by PIO first */
    qtest_writel(qts, HERA_SPI1 + SPI_IFSET, (2 << 16) | (7 << 8) | BIT(7));
    qtest_writel(qts, HERA_SPI1 + SPI_CTRL, 0x6);
    qtest_writel(qts, HERA_SPI1 + SPI_TRANSCTRL, TC_CMD | TC_MODE(7));
    qtest_writel(qts, HERA_SPI1 + SPI_CMD, 0x06);
    qtest_writel(qts, HERA_SPI1 + SPI_CTRL, 0x6);
    qtest_writel(qts, HERA_SPI1 + SPI_ADDR, 0x1000);
    qtest_writel(qts, HERA_SPI1 + SPI_TRANSCTRL, TC_CMD | TC_ADDR |
                 TC_MODE(1) | (31 << 12));
    qtest_writel(qts, HERA_SPI1 + SPI_CMD, 0x02);
    for (int i = 0; i < 8; i++) {
        qtest_writel(qts, HERA_SPI1 + SPI_DATA, 0x01010101 * (i + 1));
    }
    qtest_clock_step(qts, 2000000);

    /* the DMA first: it waits for the SPI's request */
    qtest_writel(qts, HERA_SPI1 + SPI_CTRL, 0x6);
    qtest_writel(qts, HERA_SPI1 + SPI_CTRL, BIT(3));
    channel(qts, 1, HERA_SPI1 + SPI_DATA, HERA_DTCM, 8, W32 | SRC_FIX |
            SRC_HS | SRCREQ(7));
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM), ==, 0);
    g_assert_cmphex(rd(qts, CHEN), ==, BIT(1));

    /* the command: 32 bytes of read */
    qtest_writel(qts, HERA_SPI1 + SPI_ADDR, 0x1000);
    qtest_writel(qts, HERA_SPI1 + SPI_TRANSCTRL, TC_CMD | TC_ADDR |
                 TC_MODE(2) | 31);
    qtest_writel(qts, HERA_SPI1 + SPI_CMD, 0x03);
    for (int i = 0; i < 8; i++) {
        w = qtest_readl(qts, HERA_DTCM + 4 * i);
        g_assert_cmphex(w, ==, 0x01010101 * (i + 1));
    }
    g_assert_cmphex(rd(qts, CHEN), ==, 0);
    g_assert_cmphex(rd(qts, INTST), ==, TC(1));
    g_assert_cmphex(qtest_readl(qts, HERA_SPI1 + SPI_ST) & 1, ==, 0);
    qtest_quit(qts);
}

/* flash_page_program() with DMA, the DMA enabled before the command */
static void test_flash_program_dma(void)
{
    QTestState *qts = hera_init("");

    qtest_writel(qts, HERA_SPI1 + SPI_IFSET, (2 << 16) | (7 << 8) | BIT(7));
    for (int i = 0; i < 8; i++) {
        qtest_writel(qts, HERA_SRAM + 4 * i, 0xa0b0c000 + i);
    }
    qtest_writel(qts, HERA_SPI1 + SPI_CTRL, 0x6);
    qtest_writel(qts, HERA_SPI1 + SPI_TRANSCTRL, TC_CMD | TC_MODE(7));
    qtest_writel(qts, HERA_SPI1 + SPI_CMD, 0x06);

    qtest_writel(qts, HERA_SPI1 + SPI_CTRL, 0x6);
    qtest_writel(qts, HERA_SPI1 + SPI_CTRL, BIT(4));
    channel(qts, 1, HERA_SRAM, HERA_SPI1 + SPI_DATA, 8, W32 | DST_FIX |
            DST_HS | DSTREQ(6));
    /* the FIFO takes what it holds, the rest waits for the command */
    g_assert_cmphex(rd(qts, CHEN), ==, BIT(1));
    g_assert_cmpuint(rd(qts, CH(1, SIZE)), ==, 4);

    qtest_writel(qts, HERA_SPI1 + SPI_ADDR, 0x2000);
    qtest_writel(qts, HERA_SPI1 + SPI_TRANSCTRL, TC_CMD | TC_ADDR |
                 TC_MODE(1) | (31 << 12));
    qtest_writel(qts, HERA_SPI1 + SPI_CMD, 0x02);
    g_assert_cmphex(rd(qts, CHEN), ==, 0);
    g_assert_cmphex(rd(qts, INTST), ==, TC(1));
    qtest_clock_step(qts, 2000000);
    for (int i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x2000 + 4 * i), ==,
                        0xa0b0c000 + i);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/dmac/reset", test_reset);
    qtest_add_func("/atbm-hera/dmac/copy", test_copy);
    qtest_add_func("/atbm-hera/dmac/widths-and-steps", test_widths_and_steps);
    qtest_add_func("/atbm-hera/dmac/mask-and-abort", test_mask_and_abort);
    qtest_add_func("/atbm-hera/dmac/chain", test_chain);
    qtest_add_func("/atbm-hera/dmac/flash-read", test_flash_read_dma);
    qtest_add_func("/atbm-hera/dmac/flash-program", test_flash_program_dma);
    return g_test_run();
}
