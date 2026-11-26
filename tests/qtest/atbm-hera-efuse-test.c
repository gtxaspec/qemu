/*
 * QTest testcase for the ATBM6441 "Hera" eFuse
 *
 * The read the SDK's atbmwifi_efuse_read_byte() does, the burn of
 * atbmwifi_efuse_write_bit() with its key, OTP bits that only go to 1,
 * and a backing image that keeps the burnt bits.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define CMD     0x00
#define DATA    0x04
#define KEY     0x08
#define BURN_KEY 0xef555555

static uint8_t efuse_byte(QTestState *qts, uint32_t idx)
{
    qtest_writel(qts, HERA_EFUSE + CMD, idx << 8);
    return qtest_readl(qts, HERA_EFUSE + DATA);
}

/* atbmwifi_efuse_write_bit(): the bit number gives byte and bit */
static void efuse_burn(QTestState *qts, uint32_t bit)
{
    qtest_writel(qts, HERA_EFUSE + CMD, (bit << 5 & 0x7f00) |
                 (bit & 7) << 15);
    qtest_writel(qts, HERA_EFUSE + KEY, BURN_KEY);
    qtest_writel(qts, HERA_EFUSE + KEY, BURN_KEY);
    qtest_writel(qts, HERA_EFUSE + DATA, 1);
}

static void test_blank(void)
{
    QTestState *qts = hera_init("");

    for (int i = 0; i < 128; i++) {
        g_assert_cmphex(efuse_byte(qts, i), ==, 0);
    }
    g_assert_cmphex(efuse_byte(qts, 128), ==, 0);
    g_assert_cmphex(efuse_byte(qts, 0x10 + 3), ==, 0);
    qtest_quit(qts);
}

static void test_burn(void)
{
    QTestState *qts = hera_init("");

    efuse_burn(qts, 8 * 5 + 3);
    g_assert_cmphex(efuse_byte(qts, 5), ==, 0x08);
    efuse_burn(qts, 8 * 5 + 7);
    g_assert_cmphex(efuse_byte(qts, 5), ==, 0x88);
    g_assert_cmphex(efuse_byte(qts, 4), ==, 0);
    g_assert_cmphex(efuse_byte(qts, 6), ==, 0);

    /* the last bit of the last byte */
    efuse_burn(qts, 1023);
    g_assert_cmphex(efuse_byte(qts, 127), ==, 0x80);

    /* a burn without the key, or one already used, does nothing */
    qtest_writel(qts, HERA_EFUSE + CMD, 9 << 8 | 0 << 15);
    qtest_writel(qts, HERA_EFUSE + DATA, 1);
    g_assert_cmphex(efuse_byte(qts, 9), ==, 0);
    qtest_writel(qts, HERA_EFUSE + CMD, 9 << 8 | 1 << 15);
    qtest_writel(qts, HERA_EFUSE + KEY, 0x12345678);
    qtest_writel(qts, HERA_EFUSE + DATA, 1);
    g_assert_cmphex(efuse_byte(qts, 9), ==, 0);
    qtest_writel(qts, HERA_EFUSE + CMD, 9 << 8 | 1 << 15);
    qtest_writel(qts, HERA_EFUSE + KEY, BURN_KEY);
    qtest_writel(qts, HERA_EFUSE + DATA, 1);
    qtest_writel(qts, HERA_EFUSE + DATA, 1);
    g_assert_cmphex(efuse_byte(qts, 9), ==, 0x02);

    /* a read of the data register does not burn */
    qtest_writel(qts, HERA_EFUSE + CMD, 9 << 8);
    g_assert_cmphex(qtest_readl(qts, HERA_EFUSE + DATA), ==, 0x02);
    g_assert_cmphex(efuse_byte(qts, 9), ==, 0x02);
    qtest_quit(qts);
}

/* a 128-byte image: its content is the eFuse, and burns reach the file */
static void test_image(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    uint8_t img[128] = { 0 }, out[128];
    QTestState *qts;
    GError *err = NULL;
    int fd = g_file_open_tmp("atbm-hera-efuse-XXXXXX", &path, &err);

    g_assert_cmpint(fd, >=, 0);
    img[0x10] = 0xa5;
    img[0x11] = 0x5a;
    g_assert_cmpint(write(fd, img, sizeof(img)), ==, sizeof(img));
    close(fd);

    args = g_strdup_printf("-drive if=none,id=ef,format=raw,file=%s "
                           "-global atbm-hera-efuse.drive=ef", path);
    qts = hera_init(args);
    g_assert_cmphex(efuse_byte(qts, 0x10), ==, 0xa5);
    g_assert_cmphex(efuse_byte(qts, 0x11), ==, 0x5a);
    efuse_burn(qts, 8 * 0x10 + 1);
    g_assert_cmphex(efuse_byte(qts, 0x10), ==, 0xa7);

    /* a bit that is 1 stays 1 */
    efuse_burn(qts, 8 * 0x11);
    g_assert_cmphex(efuse_byte(qts, 0x11), ==, 0x5b);
    qtest_quit(qts);

    fd = open(path, O_RDONLY);
    g_assert_cmpint(read(fd, out, sizeof(out)), ==, sizeof(out));
    close(fd);
    g_assert_cmphex(out[0x10], ==, 0xa7);
    g_assert_cmphex(out[0x11], ==, 0x5b);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/efuse/blank", test_blank);
    qtest_add_func("/atbm-hera/efuse/burn", test_burn);
    qtest_add_func("/atbm-hera/efuse/image", test_image);
    return g_test_run();
}
