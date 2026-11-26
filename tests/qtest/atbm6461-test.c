/*
 * ATBM6461 machine qtests: what the firmware's console and its RTOS yield
 * need of the clock and power block, with a flash image of our own
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qemu/sockets.h"

#define SMU             0x16100000
#define SMU_YIELD       (SMU + 0x0c)    /* bit 0: the RTOS yield, PLIC 27 */
#define SMU_INTST       (SMU + 0x1e0)   /* bits 6 to 8: UART1 to 3 receive */
#define UART2           0x16300000
#define UART_RBR        (UART2 + 0x20)
#define UART_IER        (UART2 + 0x24)
#define UART_FCR        (UART2 + 0x28)
#define UART_LSR        (UART2 + 0x34)
#define PLIC_PENDING    0xe4001000

static char *make_flash(void)
{
    g_autofree char *tmpl = g_strdup_printf("%s/atbm6461-XXXXXX",
                                            g_get_tmp_dir());
    char *path = g_strdup(tmpl);
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 4 * 1024 * 1024), ==, 0);
    close(fd);
    return path;
}

static QTestState *start(const char *flash, const char *extra)
{
    return qtest_initf("-machine atbm6461 "
                       "-drive if=mtd,format=raw,file=%s,snapshot=on %s",
                       flash, extra);
}

static void test_yield(void)
{
    g_autofree char *flash = make_flash();
    QTestState *qts = start(flash, "-serial null -serial null");

    g_assert_cmphex(qtest_readl(qts, PLIC_PENDING) & (1u << 27), ==, 0);
    qtest_writel(qts, SMU_YIELD, 1);
    g_assert_cmphex(qtest_readl(qts, PLIC_PENDING) & (1u << 27), ==,
                    1u << 27);
    /* the handler clears the bit; the source stays latched until claimed */
    qtest_writel(qts, SMU_YIELD, 0);
    qtest_quit(qts);
    unlink(flash);
}

static void test_rx_bits(void)
{
    g_autofree char *flash = make_flash();
    g_autofree char *path = g_strdup_printf("%s/atbm6461-%d.sock",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *args = g_strdup_printf(
        "-serial null -chardev socket,id=s1,path=%s,server=on,wait=off "
        "-serial chardev:s1", path);
    QTestState *qts = start(flash, args);
    char c = 'A';
    int fd = unix_connect(path, &error_abort);

    qtest_writel(qts, UART_FCR, 0x07);
    /* a byte with the receive interrupt off shows nowhere */
    g_assert_cmphex(qtest_readl(qts, SMU_INTST) & (7 << 6), ==, 0);
    qtest_writel(qts, UART_IER, 1);
    g_assert_cmphex(qtest_readl(qts, SMU_INTST) & (7 << 6), ==, 0);

    g_assert_cmpint(write(fd, &c, 1), ==, 1);
    while (!(qtest_readl(qts, UART_LSR) & 1)) {
        qtest_clock_step(qts, 10000000);
    }
    /* UART2 is bit 7 */
    g_assert_cmphex(qtest_readl(qts, SMU_INTST) & (7 << 6), ==, 1 << 7);
    g_assert_cmphex(qtest_readl(qts, UART_RBR) & 0xff, ==, 'A');
    g_assert_cmphex(qtest_readl(qts, SMU_INTST) & (7 << 6), ==, 0);
    close(fd);
    unlink(path);
    qtest_quit(qts);
    unlink(flash);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm6461/smu/yield", test_yield);
    qtest_add_func("/atbm6461/smu/uart-rx-bits", test_rx_bits);

    return g_test_run();
}
