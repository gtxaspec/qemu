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
#define PADS_24_25      0x17400034      /* the pad functions of pins 24, 25 */

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

#define WRTC_CTRL       (SMU + 0x10f4)
#define WRTC_LOAD       (SMU + 0x10f8)
#define WRTC_COUNT      (SMU + 0x1100)
#define WRTC_KEY        0xa5a50000

static bool yield_pending(QTestState *qts)
{
    return qtest_readl(qts, PLIC_PENDING) & (1u << 27);
}

/*
 * The writes system_reboot() and Hardware_Reboot() make before the last
 * one: the reset flag, the interrupt mask and the pads of pins 24 and 25
 * (which a stock chip already has at function 8). None resets the chip.
 */
static void test_reboot_not_pads(void)
{
    g_autofree char *flash = make_flash();
    QTestState *qts = start(flash, "-serial null -serial null");

    qtest_writel(qts, SMU_YIELD, 1);
    qtest_writel(qts, SMU + 0x1030, qtest_readl(qts, SMU + 0x1030) | 8);
    qtest_writel(qts, SMU + 0x08, 0xffffffff);
    qtest_writel(qts, PADS_24_25, 0x26082608);
    qtest_clock_step(qts, 100000000);
    g_assert_true(yield_pending(qts));
    g_assert_cmphex(qtest_readl(qts, PADS_24_25), ==, 0x26082608);
    qtest_quit(qts);
    unlink(flash);
}

/*
 * Hardware_Reboot()'s last call, "wdt_rtc_rebo set": stop and clear the
 * watchdog, 100 counts at 100 kHz, start it. 1 ms later the chip resets.
 */
static void test_reboot_wdt_rtc(void)
{
    g_autofree char *flash = make_flash();
    QTestState *qts = start(flash, "-serial null -serial null");

    qtest_writel(qts, SMU_YIELD, 1);
    qtest_writel(qts, WRTC_CTRL, WRTC_KEY | 2);
    qtest_writel(qts, WRTC_LOAD, 100);
    qtest_writel(qts, WRTC_CTRL, WRTC_KEY | 1);
    qtest_writel(qts, WRTC_CTRL, WRTC_KEY | 0xa501);
    qtest_clock_step(qts, 500000);
    g_assert_cmpuint(qtest_readl(qts, WRTC_COUNT), ==, 50);
    qtest_clock_step(qts, 490000);
    g_assert_true(yield_pending(qts));
    qtest_clock_step(qts, 10000);
    qtest_qmp_eventwait(qts, "WATCHDOG");
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_false(yield_pending(qts));
    /* the reset stops it */
    g_assert_cmpuint(qtest_readl(qts, WRTC_COUNT), ==, 0);
    qtest_quit(qts);
    unlink(flash);
}

/* Fed with bit 1, it does not expire; a write without the key is dropped */
static void test_wdt_rtc_feed(void)
{
    g_autofree char *flash = make_flash();
    QTestState *qts = start(flash, "-serial null -serial null");

    qtest_writel(qts, SMU_YIELD, 1);
    qtest_writel(qts, WRTC_LOAD, 1000);
    qtest_writel(qts, WRTC_CTRL, WRTC_KEY | 1);
    for (int i = 0; i < 5; i++) {
        qtest_clock_step(qts, 8000000);
        g_assert_cmpuint(qtest_readl(qts, WRTC_COUNT), ==, 800);
        qtest_writel(qts, WRTC_CTRL, WRTC_KEY | 3);
    }
    /* stopped, it keeps its count */
    qtest_clock_step(qts, 5000000);
    qtest_writel(qts, WRTC_CTRL, WRTC_KEY);
    qtest_clock_step(qts, 50000000);
    g_assert_cmpuint(qtest_readl(qts, WRTC_COUNT), ==, 500);
    qtest_writel(qts, WRTC_CTRL, 1);
    qtest_clock_step(qts, 50000000);
    g_assert_cmpuint(qtest_readl(qts, WRTC_COUNT), ==, 500);
    g_assert_true(yield_pending(qts));
    qtest_quit(qts);
    unlink(flash);
}

/* The chip version register: 0xb3 on a stock 6461 */
static void test_chip_version(void)
{
    g_autofree char *flash = make_flash();
    QTestState *qts = start(flash, "-serial null -serial null");

    g_assert_cmphex(qtest_readl(qts, 0x0acc017c), ==, 0xb3);
    qtest_quit(qts);
    unlink(flash);
}

/* A reset starts from a cold boot's state: memories, registers, the HIF */
static void test_reset_state(void)
{
    g_autofree char *flash = make_flash();
    QTestState *qts = start(flash, "-serial null -serial null");

    qtest_writel(qts, 0x09000100, 0x12345678);          /* SRAM */
    qtest_writel(qts, 0x00800040, 0x9abcdef0);          /* DTCM */
    qtest_writel(qts, 0x18100438, 0x0a0b0c0d);          /* MAC descriptors */
    qtest_writel(qts, 0x0a500b00, 0x55aa55aa);          /* a MAC register */
    qtest_writel(qts, 0x0ab00134, 0x00030000);          /* HIF CONFIG */

    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(qtest_readl(qts, 0x09000100), ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x00800040), ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x18100438), ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x0a500b00), ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x0ab00134), ==, 0);
    qtest_quit(qts);
    unlink(flash);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm6461/smu/yield", test_yield);
    qtest_add_func("/atbm6461/smu/uart-rx-bits", test_rx_bits);
    qtest_add_func("/atbm6461/reboot/not-pads", test_reboot_not_pads);
    qtest_add_func("/atbm6461/reboot/wdt-rtc", test_reboot_wdt_rtc);
    qtest_add_func("/atbm6461/wdt-rtc/feed", test_wdt_rtc_feed);
    qtest_add_func("/atbm6461/chip-version", test_chip_version);
    qtest_add_func("/atbm6461/reset/state", test_reset_state);

    return g_test_run();
}
