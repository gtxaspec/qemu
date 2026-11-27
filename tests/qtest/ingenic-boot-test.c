/*
 * QTest testcase for an Ingenic direct Linux boot (-kernel uImage)
 *
 * The argv the kernel finds through a1, at the top of the low RAM: the
 * command line from -append, from the flash's U-Boot environment, or
 * thingino's defaults for the part and RAM size; and an -initrd at the top
 * of the kernel's memory with rd_start and rd_size on the command line,
 * or at the rd_start the command line already carries.
 * The default lines are what thingino's build gives each part. UART1 is
 * left as U-Boot leaves it, through every reset, and the clock gates as a
 * T10L's, T21N's, T23DL's, T31X's, T32LQ's, T40XP's and T41LQ's U-Boot
 * leaves them, UART1's open (the A1's as its vendor U-Boot does).
 *
 * A flash boot starts its SPL with the chip as the part's own bootrom
 * leaves it after reading the SPL from NOR: the SFC as that read left it,
 * TRAN_LEN the SPL's size, the boot select pins PC0 and PC1 inputs, and on
 * the T20, T31 and T41 the SFC's pins PA23 to PA28 in their function.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define ARGV_AREA       0x2000
#define ENV_OFFSET      0x50000
#define ENV_SIZE        0x10000

static uint32_t crc32_le(const uint8_t *p, size_t n)
{
    uint32_t crc = ~0u;

    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = crc >> 1 ^ (crc & 1 ? 0xedb88320 : 0);
        }
    }
    return ~crc;
}

/* A Linux kernel uImage of 4 KiB loaded at 0x80010000 */
static char *make_uimage(void)
{
    uint8_t img[64 + 4096] = { 0 };
    char *path;
    int fd = g_file_open_tmp("ingenic-boot-kernel-XXXXXX", &path, NULL);

    stl_be_p(img, 0x27051956);
    stl_be_p(img + 12, 4096);
    stl_be_p(img + 16, 0x80010000);
    stl_be_p(img + 20, 0x80010000);
    img[28] = 5;                /* Linux */
    img[29] = 5;                /* MIPS */
    img[30] = 2;                /* kernel */
    g_assert(write(fd, img, sizeof(img)) == sizeof(img));
    close(fd);
    return path;
}

/* A 16 MiB flash holding a U-Boot environment of NUL-separated pairs */
static char *make_flash(const char *const *vars)
{
    g_autofree uint8_t *env = g_malloc0(ENV_SIZE);
    char *path;
    int fd = g_file_open_tmp("ingenic-boot-flash-XXXXXX", &path, NULL);
    size_t off = 4;

    for (int i = 0; vars[i]; i++) {
        size_t len = strlen(vars[i]) + 1;

        memcpy(env + off, vars[i], len);
        off += len;
    }
    stl_le_p(env, crc32_le(env + 4, ENV_SIZE - 4));
    g_assert(ftruncate(fd, 16 * 1024 * 1024) == 0);
    g_assert(pwrite(fd, env, ENV_SIZE, ENV_OFFSET) == ENV_SIZE);
    close(fd);
    return path;
}

static char *read_cmdline(QTestState *qts, uint64_t ram)
{
    uint32_t argv = MIN(ram, 256 * 1024 * 1024) - ARGV_AREA;
    char buf[ARGV_AREA - 16];

    g_assert_cmphex(qtest_readl(qts, argv), ==, (argv + 8) | 0x80000000);
    g_assert_cmphex(qtest_readl(qts, argv + 4), ==, (argv + 16) | 0x80000000);
    qtest_memread(qts, argv + 8, buf, 6);
    g_assert_cmpstr(buf, ==, "linux");
    qtest_memread(qts, argv + 16, buf, sizeof(buf));
    buf[sizeof(buf) - 1] = 0;
    return g_strdup(buf);
}

typedef struct {
    const char *machine;
    unsigned ram_mib;
    const char *cmdline;
} DefaultCase;

static const DefaultCase defaults[] = {
    { "ingenic-t31", 128, "mem=99M@0x0 rmem=29M@0x6300000" },
    { "ingenic-t31", 64, "mem=41M@0x0 rmem=23M@0x2900000" },
    { "ingenic-t23", 32, "mem=22M@0x0 rmem=10M@0x1600000" },
    { "ingenic-t20", 128,
      "mem=91M@0x0 rmem=29M@0x6300000 ispmem=8M@0x5b00000" },
    { "ingenic-t10", 64,
      "mem=33M@0x0 rmem=23M@0x2900000 ispmem=8M@0x2100000" },
    { "ingenic-t41", 256, "mem=192M@0x0 rmem=64M@0xc000000" },
    { "ingenic-a1", 256, "mem=256M@0x0 rmem=0M@0x10000000" },
};

static void test_default(const void *data)
{
    const DefaultCase *c = data;
    g_autofree char *kernel = make_uimage();
    g_autofree char *want = g_strdup_printf(
        "%s console=ttyS1,115200n8 panic=2", c->cmdline);
    QTestState *qts = qtest_initf("-machine %s -m %u -kernel %s",
                                  c->machine, c->ram_mib, kernel);
    g_autofree char *got = read_cmdline(qts, c->ram_mib * 1024 * 1024ULL);

    g_assert_cmpstr(got, ==, want);
    qtest_quit(qts);
    unlink(kernel);
}

static void test_append(void)
{
    g_autofree char *kernel = make_uimage();
    QTestState *qts = qtest_initf("-machine ingenic-t31 -m 64 -kernel %s "
                                  "-append 'console=ttyS1 root=/dev/ram0'",
                                  kernel);
    g_autofree char *got = read_cmdline(qts, 64 * 1024 * 1024);

    g_assert_cmpstr(got, ==, "console=ttyS1 root=/dev/ram0");
    qtest_quit(qts);
    unlink(kernel);
}

/* bootcmd as thingino writes it, and the bootargs a T31X camera boots with */
static void test_flash_env(void)
{
    static const char *const vars[] = {
        "baudrate=115200",
        "bootcmd=sf probe;setenv bootargs mem=${osmem} rmem=${rmem}  "
        "console=${serialport},${baudrate}n8 panic=${panic_timeout} "
        "root=${root} rootfstype=${rootfstype} init=${init} "
        "mtdparts=${mtdparts};sf read ${loadaddr} ${kern_addr} "
        "${kern_size};bootm ${loadaddr}",
        "init=/init",
        "mtdparts=jz_sfc:320k(boot),64k(env),64k(backup),1600k(kernel),"
        "4032k(rootfs),10304k(data),16384k@0(all)",
        "osmem=64M@0x0",
        "panic_timeout=2",
        "rmem=64M@0x4000000",
        "root=/dev/mtdblock4",
        "rootfstype=squashfs",
        "serialport=ttyS1",
        NULL,
    };
    g_autofree char *kernel = make_uimage();
    g_autofree char *flash = make_flash(vars);
    QTestState *qts = qtest_initf("-machine ingenic-t31 -m 128 -kernel %s "
                                  "-drive if=none,format=raw,file=%s",
                                  kernel, flash);
    g_autofree char *got = read_cmdline(qts, 128 * 1024 * 1024);

    g_assert_cmpstr(got, ==, "mem=64M@0x0 rmem=64M@0x4000000 "
                    "console=ttyS1,115200n8 panic=2 root=/dev/mtdblock4 "
                    "rootfstype=squashfs init=/init mtdparts=jz_sfc:320k(boot),"
                    "64k(env),64k(backup),1600k(kernel),4032k(rootfs),"
                    "10304k(data),16384k@0(all)");
    qtest_quit(qts);
    unlink(kernel);
    unlink(flash);
}

/* 100 KiB below mem=99M, on a 64 KiB boundary */
static void test_initrd(void)
{
    g_autofree char *kernel = make_uimage();
    g_autofree uint8_t *data = g_malloc(0x19000);
    g_autofree uint8_t *back = g_malloc(0x19000);
    g_autofree char *initrd = NULL;
    int fd = g_file_open_tmp("ingenic-boot-initrd-XXXXXX", &initrd, NULL);
    QTestState *qts;
    g_autofree char *got = NULL;

    for (int i = 0; i < 0x19000; i++) {
        data[i] = i * 7;
    }
    g_assert(write(fd, data, 0x19000) == 0x19000);
    close(fd);
    qts = qtest_initf("-machine ingenic-t31 -m 128 -kernel %s -initrd %s",
                      kernel, initrd);
    got = read_cmdline(qts, 128 * 1024 * 1024);
    g_assert_cmpstr(got, ==, "mem=99M@0x0 rmem=29M@0x6300000 "
                    "console=ttyS1,115200n8 panic=2 "
                    "rd_start=0x862e0000 rd_size=0x19000");
    qtest_memread(qts, 0x62e0000, back, 0x19000);
    g_assert_cmpmem(back, 0x19000, data, 0x19000);
    qtest_quit(qts);
    unlink(kernel);
    unlink(initrd);
}

static char *make_initrd(size_t size, uint8_t *data)
{
    char *path;
    int fd = g_file_open_tmp("ingenic-boot-initrd-XXXXXX", &path, NULL);

    for (size_t i = 0; i < size; i++) {
        data[i] = i * 7;
    }
    g_assert(write(fd, data, size) == size);
    close(fd);
    return path;
}

/* An rd_start already on the command line: the image goes there */
static void test_rd_start(void)
{
    static const struct {
        const char *cmdline, *want;
        uint64_t phys;
    } cases[] = {
        { "console=ttyS1 rd_start=0x80800000",
          "console=ttyS1 rd_start=0x80800000 rd_size=0x19000", 0x800000 },
        { "rd_start=0xa0800000 rd_size=0x19000 mem=64M@0x0",
          "rd_start=0xa0800000 rd_size=0x19000 mem=64M@0x0", 0x800000 },
    };
    g_autofree uint8_t *data = g_malloc(0x19000);
    g_autofree uint8_t *back = g_malloc(0x19000);
    g_autofree char *kernel = make_uimage();
    g_autofree char *initrd = make_initrd(0x19000, data);

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *qts = qtest_initf("-machine ingenic-t31 -m 128 -kernel %s "
                                      "-initrd %s -append '%s'",
                                      kernel, initrd, cases[i].cmdline);
        g_autofree char *got = read_cmdline(qts, 128 * 1024 * 1024);

        g_assert_cmpstr(got, ==, cases[i].want);
        qtest_memread(qts, cases[i].phys, back, 0x19000);
        g_assert_cmpmem(back, 0x19000, data, 0x19000);
        qtest_quit(qts);
    }
    unlink(kernel);
    unlink(initrd);
}

/* A mismatched rd_size is kept, as the kernel uses it, with a warning */
static void test_rd_size_differs(void)
{
    g_autofree uint8_t *data = g_malloc(0x19000);
    g_autofree char *kernel = make_uimage();
    g_autofree char *initrd = make_initrd(0x19000, data);
    QTestState *qts = qtest_initf("-machine ingenic-t31 -m 128 -kernel %s "
                                  "-initrd %s -append "
                                  "'rd_start=0x80800000 rd_size=0x20000'",
                                  kernel, initrd);
    g_autofree char *got = read_cmdline(qts, 128 * 1024 * 1024);

    g_assert_cmpstr(got, ==, "rd_start=0x80800000 rd_size=0x20000");
    qtest_quit(qts);
    unlink(kernel);
    unlink(initrd);
}

/* QEMU must exit 1 with the message on stderr */
static void rd_start_fails(int ram_mib, const char *cmdline, const char *msg)
{
    g_autofree uint8_t *data = g_malloc(0x19000);
    g_autofree char *kernel = make_uimage();
    g_autofree char *initrd = make_initrd(0x19000, data);
    g_autofree char *ram = g_strdup_printf("%d", ram_mib);
    const char *bin = qtest_qemu_binary(NULL);
    char *argv[] = { (char *)bin, (char *)"-machine", (char *)"ingenic-t31",
                     (char *)"-display", (char *)"none", (char *)"-m", ram,
                     (char *)"-kernel", kernel, (char *)"-initrd", initrd,
                     (char *)"-append", (char *)cmdline, NULL };
    g_autofree char *err = NULL;
    int status = 0;

    g_assert(g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL,
                          NULL, NULL, NULL, &err, &status, NULL));
    g_assert(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 1);
    g_assert_nonnull(strstr(err, msg));
    unlink(kernel);
    unlink(initrd);
}

static void test_rd_start_errors(void)
{
    rd_start_fails(64, "mem=64M@0x0 rd_start=0x84000000",
                   "is outside the RAM, which ends at 0x4000000");
    rd_start_fails(128, "mem=32M@0x0 rd_start=0x81ff0000",
                   "does not fit below the end of the kernel's memory at "
                   "0x2000000");
    rd_start_fails(128, "rd_start=0x80010000",
                   "overlaps the kernel at 0x10000 to 0x11000");
    rd_start_fails(128, "rd_start=0x800000",
                   "is not a page aligned KSEG0 or KSEG1 address");
}

static void check_uart1(QTestState *qts)
{
    const uint32_t uart1 = 0x10031000;

    g_assert_cmphex(qtest_readl(qts, uart1 + 0x0c), ==, 0x03);  /* 8N1 */
    g_assert_cmphex(qtest_readl(qts, uart1 + 0x08), ==, 0xc1);  /* FIFOs */
    qtest_writel(qts, uart1 + 0x0c, 0x83);
    g_assert_cmphex(qtest_readl(qts, uart1 + 0x00), ==, 13);    /* 115200 */
    g_assert_cmphex(qtest_readl(qts, uart1 + 0x04), ==, 0);
    qtest_writel(qts, uart1 + 0x0c, 0x03);
}

typedef struct {
    const char *machine;
    uint32_t clkgr, gates;
} UartCase;

static const UartCase uart_cases[] = {
    { "ingenic-t10", 0x10000020, 0x1fef7f80 },
    { "ingenic-t21", 0x10000020, 0x1fef7f80 },
    { "ingenic-t23", 0x10000020, 0x1bef7f80 },
    { "ingenic-t31", 0x10000020, 0x1fef7f80 },
    { "ingenic-t32", 0x10000020, 0xf1b9ef60 },
    { "ingenic-t40", 0x10000020, 0x1fdf7f80 },
    { "ingenic-t41", 0x10000020, 0x1f9f7fc4 },
    { "ingenic-a1", 0x10000030, 0xe0b00500 },
};

static void test_uart(const void *data)
{
    const UartCase *c = data;
    g_autofree char *kernel = make_uimage();
    QTestState *qts = qtest_initf("-machine %s -kernel %s", c->machine,
                                  kernel);

    g_assert_cmphex(qtest_readl(qts, c->clkgr), ==, c->gates);
    check_uart1(qts);
    qtest_writel(qts, 0x10031000 + 0x0c, 0);
    qtest_system_reset(qts);
    check_uart1(qts);
    qtest_quit(qts);
    unlink(kernel);
}

typedef struct {
    const char *machine;
    uint32_t stride;            /* between GPIO ports */
    uint32_t pa_pat0;
} RomCase;

static const RomCase rom_cases[] = {
    { "ingenic-t20", 0x100, 0x1f800000 },
    { "ingenic-t31", 0x1000, 0x1f800000 },
    { "ingenic-t41", 0x1000, 0x1f800000 },
    { "ingenic-a1", 0x1000, 0 },
};

static void test_rom_left(const void *data)
{
    const RomCase *c = data;
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("ingenic-boot-XXXXXX", &path, NULL);
    uint8_t *flash = g_malloc0(64 * 1024);
    QTestState *qts;

    stl_le_p(flash, 0x03040506);        /* the SPL header's magic */
    stl_le_p(flash + 12, 0x2a20);       /* and its size */
    g_assert(fd >= 0);
    g_assert(write(fd, flash, 64 * 1024) == 64 * 1024);
    close(fd);
    g_free(flash);

    qts = qtest_initf("-machine %s -drive file=%s,format=raw,if=none,"
                      "id=flash0", c->machine, path);
    g_assert_cmphex(qtest_readl(qts, 0x13440000), ==, 0x00000f8f);
    g_assert_cmphex(qtest_readl(qts, 0x13440004), ==, 0x00000007);
    g_assert_cmphex(qtest_readl(qts, 0x13440014), ==, 0x19010003);
    g_assert_cmphex(qtest_readl(qts, 0x1344002c), ==, 0x2a20);
    g_assert_cmphex(qtest_readl(qts, 0x13440078), ==, 0);
    g_assert_cmphex(qtest_readl(qts, 0x10010000 + 2 * c->stride + 0x20) & 3,
                    ==, 3);
    if (c->pa_pat0) {
        g_assert_cmphex(qtest_readl(qts, 0x10010040), ==, c->pa_pat0);
    }
    qtest_quit(qts);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    for (int i = 0; i < ARRAY_SIZE(defaults); i++) {
        g_autofree char *path = g_strdup_printf("ingenic-boot/default/%s-%u",
                                                defaults[i].machine,
                                                defaults[i].ram_mib);
        qtest_add_data_func(path, &defaults[i], test_default);
    }
    qtest_add_func("ingenic-boot/append", test_append);
    qtest_add_func("ingenic-boot/flash-env", test_flash_env);
    qtest_add_func("ingenic-boot/initrd", test_initrd);
    qtest_add_func("ingenic-boot/initrd-rd-start", test_rd_start);
    qtest_add_func("ingenic-boot/initrd-rd-size-differs",
                   test_rd_size_differs);
    qtest_add_func("ingenic-boot/initrd-rd-start-errors",
                   test_rd_start_errors);
    for (int i = 0; i < ARRAY_SIZE(uart_cases); i++) {
        g_autofree char *path = g_strdup_printf("ingenic-boot/uart/%s",
                                                uart_cases[i].machine);
        qtest_add_data_func(path, &uart_cases[i], test_uart);
    }
    for (int i = 0; i < ARRAY_SIZE(rom_cases); i++) {
        g_autofree char *path = g_strdup_printf("ingenic-boot/rom-left/%s",
                                                rom_cases[i].machine);
        qtest_add_data_func(path, &rom_cases[i], test_rom_left);
    }

    return g_test_run();
}
