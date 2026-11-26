/*
 * QTest testcase for the ATBM6441 "Hera" HIF (the SDIO slave block)
 *
 * The chip's view (the descriptor rings and the interrupt registers the
 * SDK's SDIO demo programs) against the host's (function 1: CONFIG,
 * CONTROL, the message queue and the memory windows), the host played
 * through the machine's test port: a message in, a message out with its
 * length trailer, the errors, the windows and the interrupts both ways.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define PORT            0x0ae00000
#define P_ADDR          0x00
#define P_LEN           0x04
#define P_INCR          0x08
#define P_DATA          0x10
#define P_IRQ           0x14

#define H_IN(i)         (0x00 + 4 * (i))
#define H_OUT_A(i)      (0x100 + 8 * (i))
#define H_OUT_B(i)      (0x104 + 8 * (i))
#define H_INTEN         0x120
#define H_RAW           0x124
#define H_CLEAR         0x128
#define H_CONFIG        0x134
#define H_CONTROL       0x138
#define H_SOFT_RESET    0x16c

#define INT_PRIO        BIT(0)
#define INT_OUT         BIT(1)
#define INT_IN          BIT(2)
#define INT_NODATA      BIT(6)
#define INT_NOBUF       BIT(11)

#define ARMED           BIT(17)

/* function 1 addresses */
#define F_REG(r)        ((r) << 2)
#define F_CONFIG        F_REG(0)
#define F_CONTROL       F_REG(1)
#define F_QUEUE(buf)    (F_REG(2) | ((buf) << 6))
#define F_AHB           F_REG(3)
#define F_BASE          F_REG(4)
#define F_SRAM          F_REG(5)

#define CFG_CPU_RESET   BIT(14)
#define CFG_IRQ         (BIT(16) | BIT(17))
#define CFG_PFETCH      BIT(11)
#define CTL_WUP         BIT(12)
#define CTL_RDY         BIT(13)

#define MSG_ADDR        0x100

static uint32_t rd(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, HERA_HIF + reg);
}

static void wr(QTestState *qts, uint32_t reg, uint32_t v)
{
    qtest_writel(qts, HERA_HIF + reg, v);
}

static QTestState *start(const char *extra)
{
    g_autofree char *args = g_strdup_printf("-machine atbm-hera,"
                                            "hif-host-port=0x%x %s", PORT,
                                            extra);
    QTestState *qts = qtest_initf("-cpu " HERA_CPU " %s", args);

    qtest_irq_intercept_in(qts, "/machine/intsink");
    return qts;
}

/* the host's side of a function 1 transfer through the test port */
static void host_write(QTestState *qts, uint32_t addr, const void *buf,
                       uint32_t len)
{
    const uint8_t *p = buf;

    qtest_writel(qts, PORT + P_ADDR, addr);
    qtest_writel(qts, PORT + P_LEN, len);
    qtest_writel(qts, PORT + P_INCR, 1);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = 0;

        for (uint32_t j = 0; j < 4 && i + j < len; j++) {
            w |= (uint32_t)p[i + j] << (8 * j);
        }
        qtest_writel(qts, PORT + P_DATA, w);
    }
}

static void host_read(QTestState *qts, uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *p = buf;

    qtest_writel(qts, PORT + P_ADDR, addr);
    qtest_writel(qts, PORT + P_LEN, len);
    qtest_writel(qts, PORT + P_INCR, 1);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = qtest_readl(qts, PORT + P_DATA);

        for (uint32_t j = 0; j < 4 && i + j < len; j++) {
            p[i + j] = w >> (8 * j);
        }
    }
}

static uint32_t host_reg(QTestState *qts, uint32_t addr)
{
    uint32_t v;

    host_read(qts, addr, &v, 4);
    return v;
}

static void host_set(QTestState *qts, uint32_t addr, uint32_t v)
{
    host_write(qts, addr, &v, 4);
}

static void test_reset(void)
{
    QTestState *qts = start("");

    g_assert_cmphex(rd(qts, H_INTEN), ==, 0);
    g_assert_cmphex(rd(qts, H_RAW), ==, 0);
    g_assert_cmphex(rd(qts, H_CONFIG), ==, 0);
    g_assert_cmphex(rd(qts, H_IN(0)), ==, 0);
    g_assert_cmphex(rd(qts, H_OUT_B(0)), ==, 0);
    g_assert_cmphex(host_reg(qts, F_CONFIG), ==, 0);
    g_assert_cmphex(host_reg(qts, F_CONTROL), ==, 0);
    g_assert_false(hera_irq(qts, HERA_IRQ_HIF));
    g_assert_cmphex(qtest_readl(qts, PORT + P_IRQ), ==, 0);
    qtest_quit(qts);
}

/* the host sends a message into the buffer the firmware armed for it */
static void test_input(void)
{
    QTestState *qts = start("");
    uint8_t msg[40], got[40];

    for (int i = 0; i < 40; i++) {
        msg[i] = 0x30 + i;
    }
    wr(qts, H_INTEN, INT_IN);
    /* HiSdio_ProgramChannel: the buffer's SRAM offset and the armed bit */
    wr(qts, H_IN(5), MSG_ADDR | ARMED);
    host_write(qts, F_QUEUE(5), msg, 40);

    g_assert_true(hera_irq(qts, HERA_IRQ_HIF));
    g_assert_cmphex(rd(qts, H_RAW), ==, INT_IN);
    /* disarmed, with the length in 16-bit words in [29:18] */
    g_assert_cmphex(rd(qts, H_IN(5)), ==, MSG_ADDR | (20 << 18));
    qtest_memread(qts, HERA_SRAM + MSG_ADDR, got, 40);
    g_assert_cmpmem(got, 40, msg, 40);

    /* HiSdio_IrqHandler: read the raw status, clear what was read */
    wr(qts, H_CLEAR, rd(qts, H_RAW));
    g_assert_cmphex(rd(qts, H_RAW), ==, 0);
    g_assert_false(hera_irq(qts, HERA_IRQ_HIF));

    /* the buffer is spent: a second write to it is an error */
    host_write(qts, F_QUEUE(5), msg, 40);
    g_assert_cmphex(rd(qts, H_RAW), ==, INT_NOBUF);
    g_assert_cmphex(rd(qts, H_IN(5)), ==, MSG_ADDR | (20 << 18));

    /* masked, the cause is there and the line is not */
    wr(qts, H_CLEAR, INT_NOBUF);
    wr(qts, H_INTEN, 0);
    wr(qts, H_IN(6), 0x400 | ARMED);
    host_write(qts, F_QUEUE(6), msg, 6);
    g_assert_cmphex(rd(qts, H_RAW), ==, INT_IN);
    g_assert_false(hera_irq(qts, HERA_IRQ_HIF));
    g_assert_cmphex(rd(qts, H_IN(6)), ==, 0x400 | (3 << 18));

    /* the buffer numbers wrap over the 64 descriptors */
    wr(qts, H_IN(2), 0x500 | ARMED);
    host_write(qts, F_QUEUE(64 + 2), msg, 8);
    g_assert_cmphex(rd(qts, H_IN(2)), ==, 0x500 | (4 << 18));
    qtest_quit(qts);
}

/* a build with 32 input descriptors, every other word */
static void test_input32(void)
{
    QTestState *qts = start("-global atbm-hera-hif.input64=false");
    uint8_t msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    wr(qts, H_IN(2 * 3), MSG_ADDR | ARMED);
    host_write(qts, F_QUEUE(3), msg, 8);
    g_assert_cmphex(rd(qts, H_IN(2 * 3)), ==, MSG_ADDR | (4 << 18));
    g_assert_cmphex(qtest_readl(qts, HERA_SRAM + MSG_ADDR), ==, 0x04030201);
    /* descriptor 35 is 3 again */
    wr(qts, H_IN(2 * 3), MSG_ADDR | ARMED);
    host_write(qts, F_QUEUE(35), msg, 8);
    g_assert_cmphex(rd(qts, H_IN(2 * 3)), ==, MSG_ADDR | (4 << 18));
    qtest_quit(qts);
}

/* the firmware sends messages: the host sees the length, reads them */
static void test_output(void)
{
    QTestState *qts = start("");
    uint8_t got[24];
    uint32_t v;

    wr(qts, H_INTEN, INT_OUT);
    for (int i = 0; i < 16; i++) {
        qtest_writeb(qts, HERA_SRAM + 0x600 + i, 0xa0 + i);
    }
    for (int i = 0; i < 24; i++) {
        qtest_writeb(qts, HERA_SRAM + 0x700 + i, 0xc0 + i);
    }
    /* HiSdio_ProgramOutput: the offset, then the length (words) and busy */
    wr(qts, H_OUT_A(0), 0x600);
    wr(qts, H_OUT_B(0), (8 << 1) | 1);
    wr(qts, H_OUT_A(1), 0x700);
    wr(qts, H_OUT_B(1), (12 << 1) | 1);

    /* with the interrupt not enabled the host is not told */
    g_assert_cmphex(qtest_readl(qts, PORT + P_IRQ), ==, 0);
    host_set(qts, F_CONFIG, CFG_IRQ);
    g_assert_cmphex(qtest_readl(qts, PORT + P_IRQ), ==, 1);
    g_assert_cmphex(host_reg(qts, F_CONTROL) & 0xcfff, ==, 8);

    /* the first message; its last 16 bits are the next one's length */
    host_read(qts, F_QUEUE(1), got, 16);
    for (int i = 0; i < 14; i++) {
        g_assert_cmphex(got[i], ==, 0xa0 + i);
    }
    g_assert_cmphex(got[14] | got[15] << 8, ==, 12);
    g_assert_cmphex(rd(qts, H_OUT_B(0)) & 1, ==, 0);
    g_assert_cmphex(rd(qts, H_RAW), ==, INT_OUT);
    g_assert_true(hera_irq(qts, HERA_IRQ_HIF));
    g_assert_cmphex(qtest_readl(qts, PORT + P_IRQ), ==, 1);
    wr(qts, H_CLEAR, INT_OUT);

    /* the second, and then nothing left */
    host_read(qts, F_QUEUE(2), got, 24);
    for (int i = 0; i < 22; i++) {
        g_assert_cmphex(got[i], ==, 0xc0 + i);
    }
    g_assert_cmphex(got[22] | got[23] << 8, ==, 0);
    g_assert_cmphex(rd(qts, H_OUT_B(1)) & 1, ==, 0);
    g_assert_cmphex(qtest_readl(qts, PORT + P_IRQ), ==, 0);
    g_assert_cmphex(host_reg(qts, F_CONTROL) & 0xcfff, ==, 0);

    /* a read with nothing to send: zeros and an error */
    wr(qts, H_CLEAR, INT_OUT);
    memset(got, 0xee, sizeof(got));
    host_read(qts, F_QUEUE(3), got, 8);
    g_assert_cmphex(got[0], ==, 0);
    g_assert_cmphex(rd(qts, H_RAW), ==, INT_NODATA);

    /* a length past 12 bits goes to the upper bits of CONTROL */
    wr(qts, H_OUT_A(2), 0x800);
    wr(qts, H_OUT_B(2), (0x1234 << 1) | 1);
    v = host_reg(qts, F_CONTROL);
    g_assert_cmphex(v & 0xfff, ==, 0x234);
    g_assert_cmphex((v >> 14) & 3, ==, 1);
    qtest_quit(qts);
}

static void test_control_and_soft_reset(void)
{
    QTestState *qts = start("");

    /* a wake request is answered at once */
    host_set(qts, F_CONTROL, CTL_WUP);
    g_assert_cmphex(host_reg(qts, F_CONTROL) & (CTL_WUP | CTL_RDY), ==,
                    CTL_WUP | CTL_RDY);
    g_assert_cmphex(rd(qts, H_CONTROL), ==, CTL_WUP | CTL_RDY);
    host_set(qts, F_CONTROL, 0);
    g_assert_cmphex(host_reg(qts, F_CONTROL) & CTL_WUP, ==, 0);

    /* the pointer of the output ring starts again at the soft reset */
    wr(qts, H_OUT_A(0), 0x600);
    wr(qts, H_OUT_B(0), (4 << 1) | 1);
    wr(qts, H_OUT_A(1), 0x700);
    wr(qts, H_OUT_B(1), (6 << 1) | 1);
    uint8_t buf[8];
    host_read(qts, F_QUEUE(1), buf, 8);
    g_assert_cmphex(host_reg(qts, F_CONTROL) & 0xfff, ==, 6);
    wr(qts, H_SOFT_RESET, 1);
    wr(qts, H_SOFT_RESET, 0);
    g_assert_cmphex(host_reg(qts, F_CONTROL) & 0xfff, ==, 0);
    wr(qts, H_OUT_B(0), (4 << 1) | 1);
    g_assert_cmphex(host_reg(qts, F_CONTROL) & 0xfff, ==, 4);
    qtest_quit(qts);
}

/* the windows onto the chip's memory, by the base register */
static void test_windows(void)
{
    QTestState *qts = start("");
    uint32_t w;

    host_set(qts, F_BASE, HERA_DTCM + 0x40);
    g_assert_cmphex(host_reg(qts, F_BASE), ==, HERA_DTCM + 0x40);
    host_set(qts, F_AHB, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM + 0x40), ==, 0x11223344);
    g_assert_cmphex(host_reg(qts, F_AHB), ==, 0x11223344);

    host_set(qts, F_BASE, HERA_ILM + 8);
    host_set(qts, F_SRAM, 0xcafef00d);
    g_assert_cmphex(qtest_readl(qts, HERA_ILM + 8), ==, 0xcafef00d);
    g_assert_cmphex(host_reg(qts, F_SRAM), ==, 0xcafef00d);

    /* a prefetch request is not a state: it clears itself */
    host_set(qts, F_CONFIG, CFG_PFETCH | BIT(10));
    g_assert_cmphex(host_reg(qts, F_CONFIG), ==, BIT(10));

    /* chip registers are there too: an SMU register keeps what is written */
    host_set(qts, F_BASE, HERA_SMU + 0x1028);
    host_set(qts, F_AHB, 5);
    host_set(qts, F_BASE, HERA_SMU + 0x1028);
    w = host_reg(qts, F_AHB);
    g_assert_cmphex(w, ==, 5);
    qtest_quit(qts);
}

static void test_cpu_reset(void)
{
    QTestState *qts = qtest_init("-machine atbm-hera,hif-host-port=0xae00000 "
                                 "-cpu " HERA_CPU "");

    qtest_irq_intercept_out_named(qts, "/machine/hif", "cpu-reset");
    g_assert_false(qtest_get_irq(qts, 0));
    host_set(qts, F_CONFIG, CFG_CPU_RESET);
    g_assert_true(qtest_get_irq(qts, 0));
    host_set(qts, F_CONFIG, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/hif/reset", test_reset);
    qtest_add_func("/atbm-hera/hif/input", test_input);
    qtest_add_func("/atbm-hera/hif/input32", test_input32);
    qtest_add_func("/atbm-hera/hif/output", test_output);
    qtest_add_func("/atbm-hera/hif/control-and-soft-reset",
                   test_control_and_soft_reset);
    qtest_add_func("/atbm-hera/hif/windows", test_windows);
    qtest_add_func("/atbm-hera/hif/cpu-reset", test_cpu_reset);
    return g_test_run();
}
