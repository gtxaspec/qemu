/*
 * QTest testcase for the ATBM6461 machine's end of the link to a camera
 * SoC's QEMU (protocol: include/hw/misc/atbm-hera-link.h)
 *
 * The test is the camera SoC: it connects to the chip's link socket, answers
 * its hello and plays the SDIO card's requests. The chip runs under qtest
 * with a blank flash of the test's own, so no firmware is involved: the HIF
 * block answers, and its registers, the chip's memory windows and the
 * GPIO pads are reached the way the host driver reaches them.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/sockets.h"
#include "hw/misc/atbm-hera-link.h"

/* function 1's registers (4 bytes each) and the chip's HIF CONFIG */
#define F_CONFIG        0x00
#define F_BASE          0x10
#define F_AHB           0x0c
#define HIF             0x0ab00000
#define H_CONFIG        0x134
#define SRAM            0x09000000
#define GPIO            0x16800000
#define G_DIR0          0x28
#define G_DOUT          0x24
#define PADS_24_25      0x17400034
#define WDT1            0x16600000
#define WDT_CTRL        0x10
#define WDT_WEN         0x18
#define WDT_UNIT        0x20

static void put(int fd, uint32_t type, uint32_t a, uint32_t b, uint32_t c,
                const void *payload, size_t len)
{
    AtbmHeraLinkMsg m = {
        .type = cpu_to_le32(type), .a = cpu_to_le32(a),
        .b = cpu_to_le32(b), .c = cpu_to_le32(c),
    };

    g_assert_cmpint(write(fd, &m, sizeof(m)), ==, sizeof(m));
    if (len) {
        g_assert_cmpint(write(fd, payload, len), ==, len);
    }
}

static void get(int fd, AtbmHeraLinkMsg *m)
{
    AtbmHeraLinkMsg r;

    g_assert_cmpint(read(fd, &r, sizeof(r)), ==, sizeof(r));
    m->type = le32_to_cpu(r.type);
    m->a = le32_to_cpu(r.a);
    m->b = le32_to_cpu(r.b);
    m->c = le32_to_cpu(r.c);
}

/*
 * The answer to a request: IRQ and PAD messages the request caused come
 * first, in order, then the DATA (with its payload) or the ACK
 */
static void answer(int fd, AtbmHeraLinkMsg *m, void *payload, size_t len)
{
    do {
        get(fd, m);
    } while (m->type == AHL_IRQ || m->type == AHL_PAD);
    if (m->type == AHL_DATA) {
        g_assert_cmpuint(m->c, ==, len);
        for (size_t got = 0; got < len;) {
            ssize_t n = read(fd, (uint8_t *)payload + got, len - got);

            g_assert_cmpint(n, >, 0);
            got += n;
        }
    }
}

static uint32_t f1_read32(int fd, uint32_t reg)
{
    AtbmHeraLinkMsg m;
    uint8_t b[4];

    put(fd, AHL_SDIO_READ, 1, reg, 4 | ATBM_HERA_LINK_INCR, NULL, 0);
    answer(fd, &m, b, 4);
    g_assert_cmpint(m.type, ==, AHL_DATA);
    return ldl_le_p(b);
}

static void f1_write32(int fd, uint32_t reg, uint32_t val)
{
    AtbmHeraLinkMsg m;
    uint8_t b[4];

    stl_le_p(b, val);
    put(fd, AHL_SDIO_WRITE, 1, reg, 4 | ATBM_HERA_LINK_INCR, b, 4);
    answer(fd, &m, NULL, 0);
    g_assert_cmpint(m.type, ==, AHL_ACK);
}

static QTestState *chip(const char *flash, const char *sock)
{
    return qtest_initf("-machine atbm6461 -serial null -serial null "
                       "-drive if=mtd,format=raw,file=%s,snapshot=on "
                       "-chardev socket,id=hl,path=%s,server=on,wait=off "
                       "-global atbm-hera-link.chardev=hl", flash, sock);
}

static void test_link(void)
{
    g_autofree char *flash = g_strdup_printf("%s/atbm6461-link-%d.bin",
                                             g_get_tmp_dir(), getpid());
    g_autofree char *sock = g_strdup_printf("%s/atbm6461-link-%d.sock",
                                            g_get_tmp_dir(), getpid());
    AtbmHeraLinkMsg m;
    QTestState *qts;
    int fd, flash_fd = open(flash, O_CREAT | O_RDWR, 0600);

    g_assert_cmpint(flash_fd, >=, 0);
    g_assert_cmpint(ftruncate(flash_fd, 4 * 1024 * 1024), ==, 0);
    close(flash_fd);
    qts = chip(flash, sock);
    fd = unix_connect(sock, &error_abort);

    /* the chip opens with HELLO, the SoC answers */
    get(fd, &m);
    g_assert_cmpint(m.type, ==, AHL_HELLO);
    g_assert_cmpint(m.c, ==, ATBM_HERA_LINK_VERSION);
    put(fd, AHL_HELLO, 0, 0, ATBM_HERA_LINK_VERSION, NULL, 0);

    /* function 1 reaches the HIF block's CONFIG, which the chip sees too */
    f1_write32(fd, F_CONFIG, 0x00030000);
    g_assert_cmphex(qtest_readl(qts, HIF + H_CONFIG) & 0x00030000, ==,
                    0x00030000);
    g_assert_cmphex(f1_read32(fd, F_CONFIG) & 0x00030000, ==, 0x00030000);

    /* the SRAM window: the SoC writes the chip's memory, the chip reads it */
    f1_write32(fd, F_BASE, SRAM + 0x100);
    f1_write32(fd, F_AHB, 0x6461a55a);
    g_assert_cmphex(qtest_readl(qts, SRAM + 0x100), ==, 0x6461a55a);

    /* a pad the chip drives reaches the SoC */
    qtest_writel(qts, GPIO + G_DIR0, 1u << 23);
    qtest_writel(qts, GPIO + G_DOUT, 1u << 23);
    do {
        get(fd, &m);
    } while (m.type != AHL_PAD || m.a != 23);
    g_assert_cmpint(m.c, ==, 1);

    close(fd);
    unlink(sock);
    unlink(flash);
    qtest_quit(qts);
}

/* The chip tells the SoC when it restarts, if the SoC can take it */
static void chip_up(const char *flash, const char *sock, uint32_t caps,
                    QTestState **qts, int *fd)
{
    AtbmHeraLinkMsg m;

    *qts = chip(flash, sock);
    *fd = unix_connect(sock, &error_abort);
    get(*fd, &m);
    g_assert_cmpint(m.type, ==, AHL_HELLO);
    put(*fd, AHL_HELLO, caps, 0, ATBM_HERA_LINK_VERSION, NULL, 0);
}

/* the CHIP_RESET that comes before the answer to a request, or 0 */
static uint32_t next_chip_reset(int fd)
{
    AtbmHeraLinkMsg m;
    uint32_t reason = 0;

    put(fd, AHL_SDIO_READ, 1, F_CONFIG, 4 | ATBM_HERA_LINK_INCR, NULL, 0);
    for (;;) {
        uint8_t b[4];

        get(fd, &m);
        if (m.type == AHL_CHIP_RESET) {
            g_assert_cmpuint(reason, ==, 0);
            reason = m.a;
        } else if (m.type == AHL_DATA) {
            g_assert_cmpint(read(fd, b, 4), ==, 4);
            return reason;
        }
    }
}

static void test_chip_reset(void)
{
    g_autofree char *flash = g_strdup_printf("%s/atbm6461-cr-%d.bin",
                                             g_get_tmp_dir(), getpid());
    g_autofree char *sock = g_strdup_printf("%s/atbm6461-cr-%d.sock",
                                            g_get_tmp_dir(), getpid());
    QTestState *qts;
    int fd, flash_fd = open(flash, O_CREAT | O_RDWR, 0600);

    g_assert_cmpint(flash_fd, >=, 0);
    g_assert_cmpint(ftruncate(flash_fd, 4 * 1024 * 1024), ==, 0);
    close(flash_fd);

    /* a SoC without the capability is told nothing */
    chip_up(flash, sock, 0, &qts, &fd);
    qtest_writel(qts, PADS_24_25, 0x26082608);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmpuint(next_chip_reset(fd), ==, 0);
    close(fd);
    qtest_quit(qts);

    chip_up(flash, sock, ATBM_HERA_LINK_CAP_CHIP_RESET, &qts, &fd);
    /* the first word is that it has started */
    g_assert_cmpuint(next_chip_reset(fd), ==, AHL_CHIP_RESET_COLD);

    /* Hardware_Reboot()'s pads */
    qtest_writel(qts, PADS_24_25, 0x26082608);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmpuint(next_chip_reset(fd), ==, AHL_CHIP_RESET_REQUEST);

    /* the last stage of a watchdog: 64 clocks, then 16 and 16 */
    qtest_writel(qts, WDT1 + WDT_WEN, 0x5aa5);
    qtest_writel(qts, WDT1 + WDT_UNIT, 1 | 1 << 10 | 1 << 20);
    qtest_writel(qts, WDT1 + WDT_WEN, 0x5aa5);
    qtest_writel(qts, WDT1 + WDT_CTRL, 0x1 | 0x2 | 0x10);
    qtest_clock_step(qts, 3000);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmpuint(next_chip_reset(fd), ==, AHL_CHIP_RESET_WDT);

    close(fd);
    unlink(sock);
    unlink(flash);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm6461-link/hif", test_link);
    qtest_add_func("/atbm6461-link/chip-reset", test_chip_reset);
    return g_test_run();
}
