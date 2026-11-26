/*
 * QTest testcase for the ATBM6441 "Hera" board
 *
 * The memory map, the boot ROM that only -bios fills, the ELF loader
 * (a segment in a RAM goes there, one in the flash window goes into the
 * flash, and each is also in the flash image at its load address plus
 * 0x10000) and a flash image kept across the machine and the SPI commands.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define HERA_ROM 0x00060000

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

/*
 * A little ELF32: segments laid out as the SDK's iot.out has them (the
 * ILM image at load address 0, the code in the flash window, data for the
 * DTCM and the shared SRAM, and one that is nowhere)
 */
static char *make_elf(void)
{
    static const struct { uint32_t va, pa, len; } seg[] = {
        { 0x00030000, 0x00000000, 8 },
        { 0x000a0000, 0x00010000, 8 },
        { 0x00800000, 0x0008d630, 4 },
        { 0x09000000, 0x08f70000, 4 },
        { 0x70000000, 0x00000000, 4 },
    };
    uint8_t elf[52 + 5 * 32 + 64] = { 0 };
    uint32_t off = 52 + 5 * 32;
    char *path = NULL;
    int fd = g_file_open_tmp("atbm-hera-elf-XXXXXX", &path, NULL);

    memcpy(elf, "\177ELF\1\1\1", 7);
    put16(elf + 16, 2);
    put16(elf + 18, 167);
    put32(elf + 20, 1);
    put32(elf + 24, 0x302b0);
    put32(elf + 28, 52);
    put16(elf + 40, 52);
    put16(elf + 42, 32);
    put16(elf + 44, 5);
    for (int i = 0; i < 5; i++) {
        uint8_t *ph = elf + 52 + 32 * i;

        put32(ph + 0, 1);
        put32(ph + 4, off);
        put32(ph + 8, seg[i].va);
        put32(ph + 12, seg[i].pa);
        put32(ph + 16, seg[i].len);
        put32(ph + 20, seg[i].len);
        for (uint32_t j = 0; j < seg[i].len; j++) {
            elf[off + j] = 0x10 * (i + 1) + j;
        }
        off += seg[i].len + 4;
    }
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, elf, sizeof(elf)), ==, sizeof(elf));
    close(fd);
    return path;
}

static void test_memory_map(void)
{
    QTestState *qts = hera_init("");

    /* RAMs, with nothing in the gaps */
    qtest_writel(qts, HERA_ILM, 0x11111111);
    qtest_writel(qts, HERA_ILM + 0xfffc, 0x22222222);
    qtest_writel(qts, HERA_DTCM + 0x3fffc, 0x33333333);
    qtest_writel(qts, HERA_SRAM + 0x3fffc, 0x44444444);
    g_assert_cmphex(qtest_readl(qts, HERA_ILM), ==, 0x11111111);
    g_assert_cmphex(qtest_readl(qts, HERA_ILM + 0xfffc), ==, 0x22222222);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM + 0x3fffc), ==, 0x33333333);
    g_assert_cmphex(qtest_readl(qts, HERA_SRAM + 0x3fffc), ==, 0x44444444);
    g_assert_cmphex(qtest_readl(qts, HERA_ILM + 0x10000), ==, 0);

    /* the boot ROM is not in the SDK: empty, and not writable */
    g_assert_cmphex(qtest_readl(qts, HERA_ROM), ==, 0);
    qtest_writel(qts, HERA_ROM, 0x55555555);
    g_assert_cmphex(qtest_readl(qts, HERA_ROM), ==, 0);

    /* an erased flash in both windows; the MAC and PHY read 0 */
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH_NC + 0x1ffffc), ==,
                    0xffffffff);
    g_assert_cmphex(qtest_readl(qts, 0x0a500000), ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x0acc017c), ==, 0);
    qtest_quit(qts);
}

/* the third UART (irq 31) and the second SPI exist */
static void test_third_uart_and_spi2(void)
{
    QTestState *qts = hera_init("-serial null -serial null -serial null");

    g_assert_cmphex(qtest_readb(qts, 0x17600034), ==, 0x60);
    g_assert_cmphex(qtest_readl(qts, HERA_SPI2 + 0x7c), ==, 0x00005711);
    g_assert_cmphex(qtest_readl(qts, HERA_SPI3 + 0x7c), ==, 0x00001711);
    qtest_quit(qts);
}

static void test_bios(void)
{
    g_autofree char *path = NULL;
    uint8_t rom[16] = { 0xde, 0xad, 0xbe, 0xef };
    int fd = g_file_open_tmp("atbm-hera-rom-XXXXXX", &path, NULL);
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, rom, sizeof(rom)), ==, sizeof(rom));
    close(fd);
    qts = hera_init(g_strdup_printf("-bios %s", path));
    g_assert_cmphex(qtest_readl(qts, HERA_ROM), ==, 0xefbeadde);
    g_assert_cmphex(qtest_readl(qts, HERA_ROM + 16), ==, 0);
    qtest_quit(qts);
    unlink(path);
}

static void test_elf(void)
{
    g_autofree char *path = make_elf();
    QTestState *qts = hera_init(g_strdup_printf("-kernel %s", path));

    /* a segment in a RAM is there at its address */
    g_assert_cmphex(qtest_readl(qts, HERA_ILM), ==, 0x13121110);
    g_assert_cmphex(qtest_readl(qts, HERA_ILM + 4), ==, 0x17161514);
    g_assert_cmphex(qtest_readl(qts, HERA_DTCM), ==, 0x33323130);
    g_assert_cmphex(qtest_readl(qts, HERA_SRAM), ==, 0x43424140);

    /* one in the flash window is in the flash: 0xa0000 */
    g_assert_cmphex(qtest_readl(qts, 0x000a0000), ==, 0x23222120);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH_NC + 0x20000), ==, 0x23222120);

    /*
     * and the ILM image is also at its load address in the flash image,
     * after the boot loader's 64 KiB: flash 0x10000, window 0x90000
     */
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x10000), ==, 0x13121110);
    /* so is the DTCM's .data, for the startup code to copy */
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x10000 + 0x8d630), ==,
                    0x33323130);
    /* a segment that is nowhere is dropped; the erased flash is around */
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x10008), ==, 0xffffffff);
    qtest_quit(qts);
    unlink(path);
}

/* an image on disk is the flash; the SPI commands change the file */
static void test_flash_image(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    uint8_t img[0x20000];
    QTestState *qts;
    uint8_t out[4];
    int fd = g_file_open_tmp("atbm-hera-flash-XXXXXX", &path, NULL);

    memset(img, 0xff, sizeof(img));
    memcpy(img + 0x100, "ABCD", 4);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, img, sizeof(img)), ==, sizeof(img));
    close(fd);

    args = g_strdup_printf("-drive if=mtd,format=raw,file=%s", path);
    qts = hera_init(args);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x100), ==, 0x44434241);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x1ff00), ==, 0xffffffff);

    /* erase the sector of "ABCD" through SPI1: write enable, 0x20 */
    qtest_writel(qts, HERA_SPI1 + 0x30, 6);
    qtest_writel(qts, HERA_SPI1 + 0x20, BIT(30) | (7 << 24));
    qtest_writel(qts, HERA_SPI1 + 0x24, 0x06);
    qtest_writel(qts, HERA_SPI1 + 0x30, 6);
    qtest_writel(qts, HERA_SPI1 + 0x28, 0x100);
    qtest_writel(qts, HERA_SPI1 + 0x20, BIT(30) | BIT(29) | (7 << 24));
    qtest_writel(qts, HERA_SPI1 + 0x24, 0x20);
    g_assert_cmphex(qtest_readl(qts, HERA_FLASH + 0x100), ==, 0xffffffff);
    qtest_quit(qts);

    fd = open(path, O_RDONLY);
    g_assert_cmpint(read(fd, img, sizeof(img)), ==, sizeof(img));
    close(fd);
    memcpy(out, img + 0x100, 4);
    g_assert_cmphex(out[0], ==, 0xff);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/board/memory-map", test_memory_map);
    qtest_add_func("/atbm-hera/board/third-uart-and-spi2",
                   test_third_uart_and_spi2);
    qtest_add_func("/atbm-hera/board/bios", test_bios);
    qtest_add_func("/atbm-hera/board/elf", test_elf);
    qtest_add_func("/atbm-hera/board/flash-image", test_flash_image);
    return g_test_run();
}
