/*
 * QTest testcase for the link between a camera SoC's QEMU and an ATBM6441
 * "Hera" chip's
 *
 * Two kinds of test, on a T41's SDHCI (the host the ingenic-atbm test
 * drives as well). The wire tests make the test the chip: a thread
 * accepts the SoC's connection, says hello and answers each request, and
 * the SoC's messages are checked as they come (CCCR writes, the vendor
 * area of function 0, function 1's data, the power and reset pins and the
 * pad lines). The pair tests run both QEMUs, the SoC's and the chip's
 * (QTEST_QEMU_BINARY_NDS32), with the test playing the chip's firmware
 * through the chip's qtest: SDIO accesses reach the HIF, its interrupt
 * reaches the SDIO card, the GPIO pads and the UART cross, and the chip
 * is held and restarted by the SoC.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include <poll.h>
#include <sys/un.h>
#include "libqtest.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "hw/misc/atbm-hera-link.h"

#define NDS32_VAR       "QTEST_QEMU_BINARY_NDS32"

#define SDHCI           0x13060000
#define SD_BLKSIZE      0x04
#define SD_BLKCNT       0x06
#define SD_ARG          0x08
#define SD_XFER         0x0c
#define SD_CMD          0x0e
#define SD_RESP         0x10
#define SD_DATA         0x20
#define SD_POWER        0x29
#define SD_CLOCK        0x2c
#define SD_INT          0x30
#define SD_INT_EN       0x34
#define SD_INT_CMD      BIT(0)
#define SD_INT_XFER     BIT(1)
#define SD_INT_RDY_RD   BIT(5)
#define SD_INT_RDY_WR   BIT(4)
#define XFER_READ       0x10

#define CCCR_IOE        0x02
#define CCCR_IEN        0x04
#define CCCR_INT        0x05

/* function 1's registers, at four bytes each */
#define F_CONFIG        0x00
#define F_BASE          0x10
#define F_AHB           0x0c
#define CONFIG_IRQ      (BIT(16) | BIT(17))

/* the chip's side */
#define HERA_SRAM       0x09000000
#define HERA_HIF        0x0ab00000
#define H_CONFIG        0x134
#define H_FN1_BLKSZ     0x148
#define H_HOST_IRQ      0x14c
#define HERA_GPIO       0x16800000
#define G_DIN           0x20
#define G_DIR0          0x28
#define G_DOUT          0x24
#define G_DO0           0x2c
#define HERA_WDT        0x16600000
#define W_CTRL          0x10
#define W_WEN           0x18
#define W_UNIT          0x20
#define HERA_UART1      0x16200000
#define U_THR           0x20

/* the T41's UART0 and the CPM gate that clocks it */
#define UART0           0x10030000
#define U_RBR           0x00
#define U_FCR           0x08
#define U_LSR           0x14
#define LSR_DR          0x01
#define FCR_FME         0x01
#define FCR_UME         0x10
#define CPM_CLKGR0      0x10000020

static uint32_t ld32(const void *p)
{
    return (uint32_t)ldl_le_p(p);
}

static uint32_t sd_cmd(QTestState *qts, unsigned cmd, uint32_t arg,
                       unsigned flags)
{
    qtest_writel(qts, SDHCI + SD_INT, 0xffffffff);
    qtest_writel(qts, SDHCI + SD_ARG, arg);
    qtest_writew(qts, SDHCI + SD_CMD, cmd << 8 | flags);
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_CMD, ==,
                    SD_INT_CMD);
    return qtest_readl(qts, SDHCI + SD_RESP);
}

static uint8_t sd_io_direct(QTestState *qts, bool write, unsigned fn,
                            unsigned addr, uint8_t val)
{
    return sd_cmd(qts, 52, (uint32_t)write << 31 | fn << 28 | addr << 9 | val,
                  0x02) & 0xff;
}

/* CMD53 in byte mode, up to 512 bytes, through the buffer data port */
static void sd_io_extended(QTestState *qts, bool write, unsigned addr,
                           uint8_t *buf, size_t len)
{
    uint32_t rdy = write ? SD_INT_RDY_WR : SD_INT_RDY_RD;
    uint32_t arg = (uint32_t)write << 31 | 1 << 28 | BIT(26) | addr << 9 |
                   (len & 0x1ff);

    g_assert_cmpuint(len, <=, 512);
    qtest_writew(qts, SDHCI + SD_BLKSIZE, len);
    qtest_writew(qts, SDHCI + SD_BLKCNT, 1);
    qtest_writew(qts, SDHCI + SD_XFER, write ? 0 : XFER_READ);
    qtest_writel(qts, SDHCI + SD_INT, 0xffffffff);
    qtest_writel(qts, SDHCI + SD_ARG, arg);
    qtest_writew(qts, SDHCI + SD_CMD, 53 << 8 | 0x22);
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & rdy, ==, rdy);
    for (size_t i = 0; i < len; i += 4) {
        if (write) {
            qtest_writel(qts, SDHCI + SD_DATA, ld32(buf + i));
        } else {
            stl_le_p(buf + i, qtest_readl(qts, SDHCI + SD_DATA));
        }
    }
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_XFER, ==,
                    SD_INT_XFER);
}

static uint32_t hif_read(QTestState *qts, unsigned reg)
{
    uint8_t b[4];

    sd_io_extended(qts, false, reg, b, 4);
    return ld32(b);
}

static void hif_write(QTestState *qts, unsigned reg, uint32_t val)
{
    uint8_t b[4];

    stl_le_p(b, val);
    sd_io_extended(qts, true, reg, b, 4);
}

static void sdio_enumerate(QTestState *qts)
{
    uint32_t r;

    qtest_writeb(qts, SDHCI + SD_POWER, 0x0f);
    qtest_writel(qts, SDHCI + SD_INT_EN, 0xffffffff);
    qtest_writew(qts, SDHCI + SD_CLOCK, 0x0001);
    g_assert_cmphex(qtest_readw(qts, SDHCI + SD_CLOCK) & 0x2, ==, 0x2);
    qtest_writew(qts, SDHCI + SD_CLOCK, 0x0007);
    sd_cmd(qts, 0, 0, 0);
    sd_cmd(qts, 5, 0, 0x02);
    r = sd_cmd(qts, 5, 0x200000, 0x02);
    g_assert_cmphex(r & BIT(31), ==, BIT(31));
    r = sd_cmd(qts, 3, 0, 0x02);
    sd_cmd(qts, 7, r & 0xffff0000, 0x03);
}

static char *link_dir;

static char *sock_path(const char *name)
{
    return g_build_filename(link_dir, name, NULL);
}

/* The test as the chip */

typedef struct FakeChip {
    int listen_fd;
    int fd;
    GThread *thread;
    GAsyncQueue *seen;          /* AtbmHeraLinkMsg, the payload folded in */
    gboolean quit;
} FakeChip;

typedef struct Seen {
    AtbmHeraLinkMsg m;
    uint8_t data[64];
    uint32_t len;
} Seen;

static bool read_all(int fd, void *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, (uint8_t *)buf + done, len - done);

        if (n <= 0) {
            return false;
        }
        done += n;
    }
    return true;
}

static void chip_send(FakeChip *c, uint32_t type, uint32_t a, uint32_t b,
                      uint32_t cc, const void *payload, uint32_t len)
{
    AtbmHeraLinkMsg m;

    stl_le_p(&m.type, type);
    stl_le_p(&m.a, a);
    stl_le_p(&m.b, b);
    stl_le_p(&m.c, cc);
    g_assert_cmpint(write(c->fd, &m, sizeof(m)), ==, sizeof(m));
    if (len) {
        g_assert_cmpint(write(c->fd, payload, len), ==, len);
    }
}

static gpointer fake_chip(gpointer opaque)
{
    FakeChip *c = opaque;
    AtbmHeraLinkMsg m;

    c->fd = accept(c->listen_fd, NULL, NULL);
    g_assert(c->fd >= 0);
    chip_send(c, AHL_HELLO, 0, 0, ATBM_HERA_LINK_VERSION, NULL, 0);
    while (read_all(c->fd, &m, sizeof(m))) {
        Seen *s = g_new0(Seen, 1);
        uint32_t type = ld32(&m.type), len = ld32(&m.c) &
                                                 ~ATBM_HERA_LINK_INCR;

        s->m = m;
        if (type == AHL_SDIO_WRITE) {
            uint8_t sink[512];

            g_assert_cmpuint(len, <=, sizeof(sink));
            g_assert_true(read_all(c->fd, sink, len));
            s->len = MIN(len, sizeof(s->data));
            memcpy(s->data, sink, s->len);
        }
        g_async_queue_push(c->seen, s);
        if (type == AHL_SDIO_READ) {
            uint8_t buf[512];
            uint32_t addr = ld32(&m.b);

            for (uint32_t i = 0; i < len; i++) {
                buf[i] = (addr + i) ^ 0x5a;
            }
            chip_send(c, AHL_DATA, 0, 0, len, buf, len);
        } else if (type == AHL_SDIO_WRITE) {
            chip_send(c, AHL_ACK, 0, 0, 0, NULL, 0);
        }
    }
    return NULL;
}

static FakeChip *fake_chip_start(const char *path)
{
    FakeChip *c = g_new0(FakeChip, 1);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    c->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert(c->listen_fd >= 0);
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    g_assert(bind(c->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    g_assert(listen(c->listen_fd, 1) == 0);
    c->seen = g_async_queue_new();
    c->thread = g_thread_new("fake-chip", fake_chip, c);
    return c;
}

static Seen *chip_next(FakeChip *c)
{
    Seen *s = g_async_queue_timeout_pop(c->seen, 10 * G_USEC_PER_SEC);

    g_assert_nonnull(s);
    return s;
}

static void chip_expect(FakeChip *c, uint32_t type, uint32_t a, uint32_t b,
                        uint32_t cc)
{
    Seen *s = chip_next(c);

    /* a bus reset or a card reset says so, unasked, whenever it comes */
    while (type != AHL_SDIO_RESET &&
           ld32(&s->m.type) == AHL_SDIO_RESET) {
        g_free(s);
        s = chip_next(c);
    }

    g_assert_cmpuint(ld32(&s->m.type), ==, type);
    g_assert_cmphex(ld32(&s->m.a), ==, a);
    g_assert_cmphex(ld32(&s->m.b), ==, b);
    g_assert_cmphex(ld32(&s->m.c), ==, cc);
    g_free(s);
}

static void fake_chip_stop(FakeChip *c)
{
    shutdown(c->fd, SHUT_RDWR);
    g_thread_join(c->thread);
    close(c->fd);
    close(c->listen_fd);
    g_async_queue_unref(c->seen);
    g_free(c);
}

static void test_wire(void)
{
    g_autofree char *sock = sock_path("wire");
    FakeChip *c = fake_chip_start(sock);
    QTestState *qts = qtest_initf("-machine ingenic-t41 "
                                  "-chardev socket,id=hl,path=%s "
                                  "-device atbm-hera-link-sdio,id=h,bus=msc0,"
                                  "chardev=hl", sock);
    uint8_t b[8];
    Seen *s;

    /* the SoC answers the chip's hello, and the chip lists no pad */
    chip_expect(c, AHL_HELLO, ATBM_HERA_LINK_CAP_CHIP_RESET, 0,
                ATBM_HERA_LINK_VERSION);
    sdio_enumerate(qts);

    /* the CCCR the card keeps, and tells the chip of what is written */
    g_assert_cmphex(sd_io_direct(qts, false, 0, 0x00, 0), ==, 0x32);
    sd_io_direct(qts, true, 0, CCCR_IOE, 0x02);
    g_assert_cmphex(sd_io_direct(qts, false, 0, CCCR_IOE, 0), ==, 0x02);
    chip_expect(c, AHL_SDIO_CFG, CCCR_IOE, 0, 0x02);
    sd_io_direct(qts, true, 0, CCCR_IEN, 0x02);
    chip_expect(c, AHL_SDIO_CFG, CCCR_IEN, 0, 0x02);
    sd_io_direct(qts, true, 0, 0x110, 0x00);
    sd_io_direct(qts, true, 0, 0x111, 0x02);
    chip_expect(c, AHL_SDIO_CFG, 0x110, 0, 0x00);
    chip_expect(c, AHL_SDIO_CFG, 0x111, 0, 0x02);

    /* the vendor area of function 0 is the chip's */
    g_assert_cmphex(sd_io_direct(qts, false, 0, 0xf3, 0), ==, 0xf3 ^ 0x5a);
    chip_expect(c, AHL_SDIO_READ, 0, 0xf3, 1);
    sd_io_direct(qts, true, 0, 0xf4, 0x77);
    s = chip_next(c);
    g_assert_cmpuint(ld32(&s->m.type), ==, AHL_SDIO_WRITE);
    g_assert_cmphex(ld32(&s->m.b), ==, 0xf4);
    g_assert_cmphex(s->data[0], ==, 0x77);
    g_free(s);

    /* function 1 is the chip's: a CMD52, and a CMD53 that counts up */
    g_assert_cmphex(sd_io_direct(qts, false, 1, 0x21, 0), ==, 0x21 ^ 0x5a);
    chip_expect(c, AHL_SDIO_READ, 1, 0x21, 1);
    sd_io_extended(qts, false, 0x100, b, 8);
    for (int i = 0; i < 8; i++) {
        g_assert_cmphex(b[i], ==, (uint8_t)((0x100 + i) ^ 0x5a));
    }
    chip_expect(c, AHL_SDIO_READ, 1, 0x100, 8 | ATBM_HERA_LINK_INCR);
    hif_write(qts, F_CONFIG, 0xa5c3e1f0);
    s = chip_next(c);
    g_assert_cmpuint(ld32(&s->m.type), ==, AHL_SDIO_WRITE);
    g_assert_cmphex(ld32(&s->m.c), ==, 4 | ATBM_HERA_LINK_INCR);
    g_assert_cmphex(ld32(s->data), ==, 0xa5c3e1f0);
    g_free(s);

    /* the interrupt is the chip's, and reaches the card's CCCR INT */
    g_assert_cmphex(sd_io_direct(qts, false, 0, CCCR_INT, 0), ==, 0);
    chip_send(c, AHL_IRQ, 0, 0, 1, NULL, 0);
    /* the next request is answered after the chip's IRQ message */
    sd_io_direct(qts, false, 1, 0x00, 0);
    chip_expect(c, AHL_SDIO_READ, 1, 0x00, 1);
    g_assert_cmphex(sd_io_direct(qts, false, 0, CCCR_INT, 0), ==, 0x02);
    chip_send(c, AHL_IRQ, 0, 0, 0, NULL, 0);
    sd_io_direct(qts, false, 1, 0x00, 0);
    chip_expect(c, AHL_SDIO_READ, 1, 0x00, 1);
    g_assert_cmphex(sd_io_direct(qts, false, 0, CCCR_INT, 0), ==, 0);

    /* the pins: the SoC's lines in, the chip's pads out */
    qtest_set_irq_in(qts, "/machine/peripheral/h", "power", 0, 0);
    chip_expect(c, AHL_POWER, 0, 0, 0);
    qtest_set_irq_in(qts, "/machine/peripheral/h", "power", 0, 1);
    chip_expect(c, AHL_POWER, 0, 0, 1);
    qtest_set_irq_in(qts, "/machine/peripheral/h", "reset", 0, 0);
    chip_expect(c, AHL_RESET, 0, 0, 1);
    qtest_set_irq_in(qts, "/machine/peripheral/h", "reset", 0, 1);
    chip_expect(c, AHL_RESET, 0, 0, 0);
    qtest_set_irq_in(qts, "/machine/peripheral/h", "pad-in", 4, 1);
    chip_expect(c, AHL_PAD, 4, 0, 1);
    /* a pad's property drives it as well, and says what it drives */
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
        "{'path': '/machine/peripheral/h', 'property': 'pad24', "
        "'value': 1}}");
    chip_expect(c, AHL_PAD, 24, 0, 1);
    qtest_irq_intercept_out_named(qts, "/machine/peripheral/h", "pad-out");
    chip_send(c, AHL_PAD, 9, 0, 1, NULL, 0);
    sd_io_direct(qts, false, 1, 0x00, 0);
    chip_expect(c, AHL_SDIO_READ, 1, 0x00, 1);
    g_assert_true(qtest_get_irq(qts, 9));
    g_assert_false(qtest_get_irq(qts, 8));

    qtest_quit(qts);
    fake_chip_stop(c);
}

/* What the SoC's card does when the chip says it has reset */

static void wait_until(QTestState *qts, bool (*done)(QTestState *, int),
                       int arg)
{
    for (int tries = 0; !done(qts, arg); tries++) {
        g_assert_cmpint(tries, <, 5000);
        qtest_clock_step(qts, 1000000);
        g_usleep(1000);
    }
}

/* the card's card-detect line, which is the host's event in a socket */
static bool card_detect_is(QTestState *qts, int want)
{
    return qtest_get_irq(qts, 0) == want;
}

static bool power_is(QTestState *qts, int want)
{
    return qtest_readb(qts, SDHCI + SD_POWER) == want;
}

static QTestState *chip_reset_start(FakeChip **c, const char *sock,
                                    const char *mode)
{
    QTestState *qts;

    *c = fake_chip_start(sock);
    qts = qtest_initf("-machine ingenic-t41 "
                      "-chardev socket,id=hl,path=%s "
                      "-device atbm-hera-link-sdio,id=h,bus=msc0,chardev=hl%s",
                      sock, mode);
    chip_expect(*c, AHL_HELLO, ATBM_HERA_LINK_CAP_CHIP_RESET, 0,
                ATBM_HERA_LINK_VERSION);
    sdio_enumerate(qts);
    return qts;
}

/* A request is answered after what the chip sent before it */
static void chip_sync(QTestState *qts, FakeChip *c)
{
    sd_io_direct(qts, false, 1, 0x00, 0);
    chip_expect(c, AHL_SDIO_READ, 1, 0x00, 1);
}

static void test_chip_reset_ignore(void)
{
    g_autofree char *sock = sock_path("ignore");
    FakeChip *c;
    QTestState *qts = chip_reset_start(&c, sock, "");

    sd_io_direct(qts, true, 0, CCCR_IOE, 0x02);
    chip_expect(c, AHL_SDIO_CFG, CCCR_IOE, 0, 0x02);
    chip_send(c, AHL_CHIP_RESET, AHL_CHIP_RESET_REQUEST, 0, 0, NULL, 0);
    chip_sync(qts, c);
    chip_send(c, AHL_CHIP_RESET, AHL_CHIP_RESET_WDT, 0, 0, NULL, 0);
    chip_sync(qts, c);
    g_assert_cmphex(sd_io_direct(qts, false, 0, CCCR_IOE, 0), ==, 0x02);
    qtest_quit(qts);
    fake_chip_stop(c);
}

static void test_chip_reset_soc(void)
{
    g_autofree char *sock = sock_path("soc");
    FakeChip *c;
    QTestState *qts = chip_reset_start(&c, sock, ",on-chip-reset=soc-reset");

    /* the chip's first word after it starts says nothing to the SoC */
    qtest_writeb(qts, SDHCI + SD_POWER, 0x0b);
    chip_send(c, AHL_CHIP_RESET, AHL_CHIP_RESET_COLD, 0, 0, NULL, 0);
    chip_sync(qts, c);
    g_assert_cmphex(qtest_readb(qts, SDHCI + SD_POWER), ==, 0x0b);

    /* a reset it asked for does: the SoC starts over */
    chip_send(c, AHL_CHIP_RESET, AHL_CHIP_RESET_REQUEST, 0, 0, NULL, 0);
    wait_until(qts, power_is, 0);
    qtest_quit(qts);
    fake_chip_stop(c);
}

static void test_chip_reset_card(void)
{
    g_autofree char *sock = sock_path("card");
    FakeChip *c;
    QTestState *qts = chip_reset_start(&c, sock, ",on-chip-reset=card-reset,"
                                       "reinsert-ms=50");

    qtest_irq_intercept_out_named(qts, "/machine/peripheral/h",
                                  "card-detect");
    sd_io_direct(qts, true, 0, CCCR_IOE, 0x02);
    chip_expect(c, AHL_SDIO_CFG, CCCR_IOE, 0, 0x02);

    /* the chip's first word after it starts says nothing to the card */
    chip_send(c, AHL_CHIP_RESET, AHL_CHIP_RESET_COLD, 0, 0, NULL, 0);
    chip_sync(qts, c);
    g_assert_cmphex(sd_io_direct(qts, false, 0, CCCR_IOE, 0), ==, 0x02);

    /* the card leaves the bus, starts over and tells the chip of it */
    chip_send(c, AHL_CHIP_RESET, AHL_CHIP_RESET_WDT, 0, 0, NULL, 0);
    chip_expect(c, AHL_SDIO_RESET, 0, 0, 0);
    g_assert_false(card_detect_is(qts, 1));

    /* and returns after reinsert-ms, ready to be enumerated again */
    qtest_clock_step(qts, 60 * 1000000LL);
    wait_until(qts, card_detect_is, 1);
    sdio_enumerate(qts);
    g_assert_cmphex(sd_io_direct(qts, false, 0, CCCR_IOE, 0), ==, 0);
    qtest_quit(qts);
    fake_chip_stop(c);
}

/* Both QEMUs */

typedef struct Pair {
    QTestState *host;
    QTestState *chip;
} Pair;

static Pair pair_start(const char *card_args)
{
    g_autofree char *link = sock_path("link");
    g_autofree char *uart = sock_path("uart");
    g_autofree char *chip_args = g_strdup_printf(
        "-machine atbm-hera -cpu n9 "
        "-chardev socket,id=hl,path=%s,server=on,wait=off "
        "-global atbm-hera-link.chardev=hl "
        "-chardev socket,id=u1,path=%s,server=on,wait=off "
        "-serial chardev:u1 -serial null", link, uart);
    Pair p;

    p.chip = qtest_init_ext(NDS32_VAR, chip_args, NULL, true);
    p.host = qtest_initf("-machine ingenic-t41 "
                         "-chardev socket,id=hl,path=%s "
                         "-chardev socket,id=u0,path=%s -serial chardev:u0 "
                         "-device atbm-hera-link-sdio,id=h,bus=msc0,"
                         "chardev=hl%s", link, uart, card_args);
    return p;
}

static void pair_stop(Pair *p)
{
    qtest_quit(p->host);
    qtest_quit(p->chip);
}

static void test_pair_sdio(void)
{
    Pair p = pair_start("");
    uint8_t b[4];

    sdio_enumerate(p.host);

    /* function 1's CONFIG is the HIF's, seen from both ends */
    hif_write(p.host, F_CONFIG, CONFIG_IRQ);
    g_assert_cmphex(qtest_readl(p.chip, HERA_HIF + H_CONFIG), ==, CONFIG_IRQ);
    g_assert_cmphex(hif_read(p.host, F_CONFIG), ==, CONFIG_IRQ);

    /* function 1's block size, set in the card's FBR, reaches the HIF */
    sd_io_direct(p.host, true, 0, 0x110, 0x00);
    sd_io_direct(p.host, true, 0, 0x111, 0x01);
    hif_read(p.host, F_CONFIG);
    g_assert_cmphex(qtest_readl(p.chip, HERA_HIF + H_FN1_BLKSZ), ==, 256);

    /* the chip's SRAM through function 1's window, base and data port */
    qtest_writel(p.chip, HERA_SRAM + 0x40, 0x6441c0de);
    hif_write(p.host, F_BASE, HERA_SRAM + 0x40);
    g_assert_cmphex(hif_read(p.host, F_AHB), ==, 0x6441c0de);
    stl_le_p(b, 0x12345678);
    sd_io_extended(p.host, true, F_AHB, b, 4);
    g_assert_cmphex(qtest_readl(p.chip, HERA_SRAM + 0x40), ==, 0x12345678);

    /* the interrupt line: the chip raises it, the card's CCCR INT shows it */
    g_assert_cmphex(sd_io_direct(p.host, false, 0, CCCR_INT, 0), ==, 0);
    qtest_writel(p.chip, HERA_HIF + H_HOST_IRQ, 1);
    g_assert_cmphex(sd_io_direct(p.host, false, 0, CCCR_INT, 0), ==, 0x02);
    qtest_writel(p.chip, HERA_HIF + H_HOST_IRQ, 0);
    /* a request is where the SoC hears of it, as the chip's IRQ comes first */
    hif_read(p.host, F_CONFIG);
    g_assert_cmphex(sd_io_direct(p.host, false, 0, CCCR_INT, 0), ==, 0);
    pair_stop(&p);
}

static void test_pair_gpio(void)
{
    Pair p = pair_start("");

    qtest_irq_intercept_out_named(p.host, "/machine/peripheral/h", "pad-out");

    /* the chip drives pad 5, and the SoC sees it */
    qtest_writel(p.chip, HERA_GPIO + G_DIR0, BIT(5));
    qtest_writel(p.chip, HERA_GPIO + G_DOUT, BIT(5));
    sdio_enumerate(p.host);
    hif_read(p.host, F_CONFIG);
    g_assert_true(qtest_get_irq(p.host, 5));
    qtest_writel(p.chip, HERA_GPIO + G_DO0, BIT(5));
    hif_read(p.host, F_CONFIG);
    g_assert_false(qtest_get_irq(p.host, 5));

    /* the SoC drives pad 12, and the chip reads it */
    qtest_set_irq_in(p.host, "/machine/peripheral/h", "pad-in", 12, 1);
    sdio_enumerate(p.host);
    hif_read(p.host, F_CONFIG);
    g_assert_cmphex(qtest_readl(p.chip, HERA_GPIO + G_DIN) & BIT(12), ==,
                    BIT(12));
    qtest_set_irq_in(p.host, "/machine/peripheral/h", "pad-in", 12, 0);
    hif_read(p.host, F_CONFIG);
    g_assert_cmphex(qtest_readl(p.chip, HERA_GPIO + G_DIN) & BIT(12), ==, 0);
    pair_stop(&p);
}

static void test_pair_uart(void)
{
    static const uint8_t frame[] = { 0x7b, 0x04, 0x03, 0x83 };
    Pair p = pair_start("");

    /* the chip's UART1 is the SoC's UART0 */
    qtest_writel(p.host, CPM_CLKGR0, 0);
    /* 115200 8N1: divisor 13, 16 clocks a bit, then the FIFO and the unit */
    qtest_writel(p.host, UART0 + 0x0c, 0x80);
    qtest_writel(p.host, UART0 + U_RBR, 13);
    qtest_writel(p.host, UART0 + 0x04, 0);
    qtest_writel(p.host, UART0 + 0x0c, 0x03);
    qtest_writel(p.host, UART0 + 0x24, 16);
    qtest_writel(p.host, UART0 + U_FCR, FCR_UME | FCR_FME | 0x06);
    for (int i = 0; i < sizeof(frame); i++) {
        qtest_writel(p.chip, HERA_UART1 + U_THR, frame[i]);
        qtest_clock_step(p.chip, 1000000);
    }
    for (int i = 0; i < sizeof(frame); i++) {
        int tries = 0;

        while (!(qtest_readl(p.host, UART0 + U_LSR) & LSR_DR)) {
            g_assert_cmpint(++tries, <, 200);
            qtest_clock_step(p.host, 10000000);
            g_usleep(10000);
        }
        g_assert_cmphex(qtest_readl(p.host, UART0 + U_RBR) & 0xff, ==,
                        frame[i]);
    }
    pair_stop(&p);
}

/* The SoC powers the chip off and on, and holds its reset */
static void test_pair_power(void)
{
    Pair p = pair_start("");

    sdio_enumerate(p.host);
    hif_write(p.host, F_CONFIG, CONFIG_IRQ);
    g_assert_cmphex(hif_read(p.host, F_CONFIG), ==, CONFIG_IRQ);

    /* off, the chip's SDIO function answers zeros, and takes nothing */
    qtest_set_irq_in(p.host, "/machine/peripheral/h", "power", 0, 0);
    g_assert_cmphex(hif_read(p.host, F_CONFIG), ==, 0);
    hif_write(p.host, F_CONFIG, 0);

    /* powered again, it starts from the reset every device has */
    qtest_set_irq_in(p.host, "/machine/peripheral/h", "power", 0, 1);
    hif_read(p.host, F_CONFIG);
    qtest_qmp_assert_success(p.chip, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readl(p.chip, HERA_HIF + H_CONFIG), ==, 0);
    hif_write(p.host, F_CONFIG, CONFIG_IRQ);
    g_assert_cmphex(hif_read(p.host, F_CONFIG), ==, CONFIG_IRQ);
    pair_stop(&p);
}

/* The chip's watchdog resets it, and the SoC, set to follow, resets too */
static void test_pair_wdt(void)
{
    Pair p = pair_start(",on-chip-reset=soc-reset");

    qtest_writeb(p.host, SDHCI + SD_POWER, 0x0b);
    qtest_writel(p.chip, HERA_WDT + W_WEN, 0x5aa5);
    qtest_writel(p.chip, HERA_WDT + W_UNIT, 1 | 1 << 10 | 1 << 20);
    qtest_writel(p.chip, HERA_WDT + W_WEN, 0x5aa5);
    /* interrupt and all-reset enabled, on PCLK, 64 clocks then 16 and 16 */
    qtest_writel(p.chip, HERA_WDT + W_CTRL, BIT(0) | BIT(1) | BIT(2) |
                 BIT(4) | BIT(6));
    qtest_clock_step(p.chip, 3000);
    wait_until(p.host, power_is, 0);
    pair_stop(&p);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_machine("ingenic-t41") ||
        !qtest_has_device("atbm-hera-link-sdio")) {
        return g_test_run();
    }
    link_dir = g_dir_make_tmp("atbm-hera-link-XXXXXX", NULL);
    g_assert_nonnull(link_dir);
    qtest_add_func("atbm-hera-link/wire", test_wire);
    qtest_add_func("atbm-hera-link/chip-reset/ignore", test_chip_reset_ignore);
    qtest_add_func("atbm-hera-link/chip-reset/soc-reset", test_chip_reset_soc);
    qtest_add_func("atbm-hera-link/chip-reset/card-reset",
                   test_chip_reset_card);
    if (getenv(NDS32_VAR) &&
        qtest_has_machine_with_env(NDS32_VAR, "atbm-hera")) {
        qtest_add_func("atbm-hera-link/pair/sdio", test_pair_sdio);
        qtest_add_func("atbm-hera-link/pair/gpio", test_pair_gpio);
        qtest_add_func("atbm-hera-link/pair/uart", test_pair_uart);
        qtest_add_func("atbm-hera-link/pair/power", test_pair_power);
        qtest_add_func("atbm-hera-link/pair/wdt", test_pair_wdt);
    }
    ret = g_test_run();
    g_rmdir(link_dir);
    return ret;
}
