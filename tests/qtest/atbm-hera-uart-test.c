/*
 * QTest testcase for the ATBM6441 "Hera" UART
 *
 * The register file as the SDK's UART driver uses it: byte accesses at
 * 4-byte strides from +0x20, the prescaler behind LCR.DLAB, the FIFOs
 * and their trigger levels, and the interrupt it raises into the chip's
 * interrupt number 19 (UART1) and 20 (UART2).
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/sockets.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define RBR     0x20
#define THR     0x20
#define DLL     0x20
#define IER     0x24
#define DLM     0x24
#define IIR     0x28
#define FCR     0x28
#define PSR     0x28
#define LCR     0x2c
#define MCR     0x30
#define LSR     0x34
#define MSR     0x38
#define SPR     0x3c
#define DIV     0x14
#define MDR     0x40
#define ACR     0x44

#define LCR_DLAB        0x80
#define LCR_8N1         0x03
#define LSR_DR          0x01
#define LSR_THRE        0x20
#define LSR_TEMT        0x40
#define IER_RDR         0x01
#define IER_THRE        0x02
#define IIR_NONE        0x01
#define IIR_RDR         0x04
#define IIR_THRE        0x02
#define FCR_FIFO        0x01
#define FCR_RXRST       0x02
#define FCR_TXRST       0x04
#define FCR_RX14        0xc0

static uint8_t rd(QTestState *qts, uint32_t base, uint32_t reg)
{
    return qtest_readb(qts, base + reg);
}

static void wr(QTestState *qts, uint32_t base, uint32_t reg, uint8_t v)
{
    qtest_writeb(qts, base + reg, v);
}

static void test_reset(void)
{
    QTestState *qts = hera_init("-serial null -serial null");

    g_assert_cmphex(rd(qts, HERA_UART1, LSR), ==, LSR_THRE | LSR_TEMT);
    g_assert_cmphex(rd(qts, HERA_UART1, IIR), ==, IIR_NONE);
    g_assert_cmphex(rd(qts, HERA_UART1, IER), ==, 0);
    g_assert_cmphex(rd(qts, HERA_UART1, LCR), ==, 0);
    g_assert_cmphex(rd(qts, HERA_UART2, LSR), ==, LSR_THRE | LSR_TEMT);
    g_assert_cmphex(rd(qts, HERA_UART1, DIV), ==, 0);
    g_assert_cmphex(rd(qts, HERA_UART1, MDR), ==, 0);
    g_assert_cmphex(rd(qts, HERA_UART1, ACR), ==, 0);
    qtest_quit(qts);
}

/* the way the SDK programs a baud rate, then reads it back */
static void test_divisor(void)
{
    QTestState *qts = hera_init("-serial null -serial null");

    wr(qts, HERA_UART1, DIV, 0x10);
    wr(qts, HERA_UART1, LCR, LCR_DLAB | LCR_8N1);
    wr(qts, HERA_UART1, DLM, 0x01);
    wr(qts, HERA_UART1, DLL, 0x2b);
    wr(qts, HERA_UART1, PSR, 0x01);
    g_assert_cmphex(rd(qts, HERA_UART1, DLM), ==, 0x01);
    g_assert_cmphex(rd(qts, HERA_UART1, DLL), ==, 0x2b);
    g_assert_cmphex(rd(qts, HERA_UART1, PSR), ==, 0x01);
    wr(qts, HERA_UART1, LCR, LCR_8N1);
    g_assert_cmphex(rd(qts, HERA_UART1, DIV), ==, 0x10);
    g_assert_cmphex(rd(qts, HERA_UART1, LCR), ==, LCR_8N1);

    /* PSR is not FCR: it did not turn the FIFOs on */
    g_assert_cmphex(rd(qts, HERA_UART1, IIR), ==, IIR_NONE);
    wr(qts, HERA_UART1, FCR, FCR_FIFO);
    g_assert_cmphex(rd(qts, HERA_UART1, IIR) & 0xc0, ==, 0xc0);
    qtest_quit(qts);
}

static void test_scratch_mode(void)
{
    QTestState *qts = hera_init("-serial null -serial null");

    wr(qts, HERA_UART2, SPR, 0xa5);
    g_assert_cmphex(rd(qts, HERA_UART2, SPR), ==, 0xa5);
    g_assert_cmphex(rd(qts, HERA_UART1, SPR), ==, 0);
    qtest_writel(qts, HERA_UART2 + MDR, 0x1);
    qtest_writel(qts, HERA_UART2 + ACR, 0x3);
    g_assert_cmphex(qtest_readl(qts, HERA_UART2 + MDR), ==, 0x1);
    g_assert_cmphex(qtest_readl(qts, HERA_UART2 + ACR), ==, 0x3);
    qtest_quit(qts);
}

static QTestState *with_socket(char **path, int *fd)
{
    QTestState *qts;

    *path = g_strdup_printf("%s/atbm-hera-uart-%d.sock", g_get_tmp_dir(),
                            getpid());
    qts = hera_init(g_strdup_printf(
        "-chardev socket,id=s0,path=%s,server=on,wait=off "
        "-serial chardev:s0 -serial null", *path));
    *fd = unix_connect(*path, &error_abort);
    return qts;
}

static void test_tx_rx(void)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    char c;
    int fd;

    qts = with_socket(&path, &fd);
    wr(qts, HERA_UART1, LCR, LCR_8N1);
    wr(qts, HERA_UART1, FCR, FCR_FIFO | FCR_RXRST | FCR_TXRST);

    wr(qts, HERA_UART1, THR, 'h');
    g_assert_cmpint(read(fd, &c, 1), ==, 1);
    g_assert_cmpint(c, ==, 'h');
    g_assert_cmphex(rd(qts, HERA_UART1, LSR) & (LSR_THRE | LSR_TEMT), ==,
                    LSR_THRE | LSR_TEMT);

    /* a received byte: DR, then the RBR read empties it */
    g_assert_cmphex(rd(qts, HERA_UART1, LSR) & LSR_DR, ==, 0);
    c = 'Q';
    g_assert_cmpint(write(fd, &c, 1), ==, 1);
    qtest_clock_step(qts, 100000000);
    while (!(rd(qts, HERA_UART1, LSR) & LSR_DR)) {
        qtest_clock_step(qts, 10000000);
    }
    g_assert_cmphex(rd(qts, HERA_UART1, RBR), ==, 'Q');
    g_assert_cmphex(rd(qts, HERA_UART1, LSR) & LSR_DR, ==, 0);
    close(fd);
    unlink(path);
    qtest_quit(qts);
}

/* the interrupt: RDR from the first byte, THRE when the FIFO is empty */
static void test_irq(void)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    char c = 'x';
    int fd;

    qts = with_socket(&path, &fd);
    wr(qts, HERA_UART1, FCR, FCR_FIFO | FCR_RXRST | FCR_TXRST);
    g_assert_false(hera_irq(qts, HERA_IRQ_UART1));
    wr(qts, HERA_UART1, IER, IER_RDR);
    g_assert_false(hera_irq(qts, HERA_IRQ_UART1));

    g_assert_cmpint(write(fd, &c, 1), ==, 1);
    while (!hera_irq(qts, HERA_IRQ_UART1)) {
        qtest_clock_step(qts, 10000000);
    }
    /* the data ready cause, or the receive timeout if the test was slow */
    g_assert_true((rd(qts, HERA_UART1, IIR) & 0x07) == IIR_RDR);
    g_assert_cmphex(rd(qts, HERA_UART1, RBR), ==, 'x');
    g_assert_false(hera_irq(qts, HERA_IRQ_UART1));
    g_assert_false(hera_irq(qts, HERA_IRQ_UART2));

    /* THRE interrupt: enabling it with an empty transmitter fires it */
    wr(qts, HERA_UART1, IER, IER_THRE);
    g_assert_true(hera_irq(qts, HERA_IRQ_UART1));
    g_assert_cmphex(rd(qts, HERA_UART1, IIR) & 0x0f, ==, IIR_THRE);
    wr(qts, HERA_UART1, IER, 0);
    g_assert_false(hera_irq(qts, HERA_IRQ_UART1));
    close(fd);
    unlink(path);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/uart/reset", test_reset);
    qtest_add_func("/atbm-hera/uart/divisor", test_divisor);
    qtest_add_func("/atbm-hera/uart/scratch-mode", test_scratch_mode);
    qtest_add_func("/atbm-hera/uart/tx-rx", test_tx_rx);
    qtest_add_func("/atbm-hera/uart/irq", test_irq);
    return g_test_run();
}
