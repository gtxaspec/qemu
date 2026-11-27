/*
 * QTest testcase for the Broadcom BCM43438, BCM43436 and BCM43362 SDIO Wi-Fi
 *
 * The card sits on a T41's SDHCI, which the test drives as a driver does:
 * the SDIO enumeration, the chip's identity and RAM behind function 1's
 * backplane window, the firmware's start, the two ways the chip asks for
 * service (the SDIO card interrupt, and the host-wake pin CCCR 0xf2 turns
 * on), and the firmware commands brcmfmac sends over function 2.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"

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
#define SD_INT_CARD     BIT(8)

#define XFER_READ       0x10

#define CCCR_IOE        0x02
#define CCCR_IEN        0x04
#define CCCR_SEPINT     0xf2

/* function 1: the window registers, then the backplane through the window */
#define F1_SBADDRLOW    0x1000a
#define F1_SBADDRMID    0x1000b
#define F1_SBADDRHIGH   0x1000c

#define CORE_CC         0x18000000
#define CORE_SDIOD      0x18002000
#define CORE_SOCRAM     0x18004000
#define WRAP_ARM        0x18103000
#define SDIOD_INTSTATUS 0x20
#define SDIOD_HOSTINTMASK 0x24
#define WRAP_RESETCTRL  0x800
#define I_HMB_FRAME_IND BIT(6)
#define I_HMB_HOST_INT  BIT(7)

#define SDPCM_HDR_LEN   12
#define CDC_HDR_LEN     16
#define CDCF_IOC_ERROR  BIT(0)
#define CDCF_IOC_SET    BIT(1)
#define WLC_SET_FAKEFRAG 219
#define WLC_GET_VAR     262
#define WLC_SET_VAR     263

#define HOST_WAKE       "host-wake"

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

/* CMD53, byte mode, through the buffer data port a word at a time */
static void sd_io_extended(QTestState *qts, bool write, unsigned fn,
                           unsigned addr, bool incr, uint8_t *buf, size_t len)
{
    uint32_t rdy = write ? SD_INT_RDY_WR : SD_INT_RDY_RD;
    uint32_t arg = (uint32_t)write << 31 | fn << 28 | (uint32_t)incr << 26 |
                   addr << 9 | len;

    g_assert_cmpuint(len, <=, 512);
    g_assert_cmpuint(len & 3, ==, 0);
    qtest_writew(qts, SDHCI + SD_BLKSIZE, len);
    qtest_writew(qts, SDHCI + SD_BLKCNT, 1);
    qtest_writew(qts, SDHCI + SD_XFER, write ? 0 : XFER_READ);
    qtest_writel(qts, SDHCI + SD_INT, 0xffffffff);
    qtest_writel(qts, SDHCI + SD_ARG, arg);
    qtest_writew(qts, SDHCI + SD_CMD, 53 << 8 | 0x22);
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & rdy, ==, rdy);
    for (size_t i = 0; i < len; i += 4) {
        if (write) {
            qtest_writel(qts, SDHCI + SD_DATA, ldl_le_p(buf + i));
        } else {
            stl_le_p(buf + i, qtest_readl(qts, SDHCI + SD_DATA));
        }
    }
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_XFER, ==,
                    SD_INT_XFER);
}

/* Aim function 1's 32 KiB window at the 32 KiB holding @base */
static void window(QTestState *qts, uint32_t base)
{
    sd_io_direct(qts, true, 1, F1_SBADDRLOW, (base >> 8) & 0x80);
    sd_io_direct(qts, true, 1, F1_SBADDRMID, base >> 16);
    sd_io_direct(qts, true, 1, F1_SBADDRHIGH, base >> 24);
}

static uint32_t bp_read(QTestState *qts, uint32_t addr)
{
    uint8_t b[4];

    window(qts, addr);
    sd_io_extended(qts, false, 1, addr & 0x7fff, true, b, 4);
    return ldl_le_p(b);
}

static void bp_write(QTestState *qts, uint32_t addr, uint32_t val)
{
    uint8_t b[4];

    stl_le_p(b, val);
    window(qts, addr);
    sd_io_extended(qts, true, 1, addr & 0x7fff, true, b, 4);
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
    sd_io_direct(qts, true, 0, CCCR_IOE, 0x06);
}

/* Both functions' interrupts, then the host's interrupt mask */
static void irq_enable(QTestState *qts)
{
    sd_io_direct(qts, true, 0, CCCR_IEN, 0x07);
    bp_write(qts, CORE_SDIOD + SDIOD_HOSTINTMASK,
             I_HMB_HOST_INT | I_HMB_FRAME_IND);
}

/* A firmware in RAM, the Cortex-M3 let out of reset */
static void fw_boot(QTestState *qts)
{
    bp_write(qts, 0, 0x20000100);
    bp_write(qts, 4, 0x00000101);
    bp_write(qts, WRAP_ARM + WRAP_RESETCTRL, 0);
}

static QTestState *card_init(const char *card, const char *extra)
{
    return qtest_initf("-machine ingenic-t41 -device %s,id=w,bus=msc0%s",
                       card, extra);
}

typedef struct Ident {
    const char *card;
    uint32_t chipid;
    uint32_t ram;
} Ident;

static void test_ids(const void *data)
{
    const Ident *id = data;
    QTestState *qts = card_init(id->card, "");
    uint32_t info, banks, bank;

    sdio_enumerate(qts);
    g_assert_cmphex(bp_read(qts, CORE_CC), ==, id->chipid);
    info = bp_read(qts, CORE_SOCRAM);
    banks = info >> 4 & 0xf;
    bank = (bp_read(qts, CORE_SOCRAM + 0x40) & 0x3f) + 1;
    g_assert_cmpuint(banks * bank * 8192, ==, id->ram);
    qtest_quit(qts);
}

static void test_inband(const void *data)
{
    const Ident *id = data;
    QTestState *qts = card_init(id->card, "");

    sdio_enumerate(qts);
    irq_enable(qts);
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_CARD, ==, 0);
    fw_boot(qts);

    /* the firmware is up: a pending interrupt rides the SDIO bus */
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_CARD, ==,
                    SD_INT_CARD);
    g_assert_cmphex(bp_read(qts, CORE_SDIOD + SDIOD_INTSTATUS), ==,
                    I_HMB_HOST_INT);
    /* and stays until the host clears what the chip has pending */
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_CARD, ==,
                    SD_INT_CARD);
    bp_write(qts, CORE_SDIOD + SDIOD_INTSTATUS, I_HMB_HOST_INT);
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_CARD, ==, 0);

    /* the host's mask gates it, and so does the bus' own enable */
    bp_write(qts, CORE_SDIOD + SDIOD_HOSTINTMASK, 0);
    bp_write(qts, CORE_SDIOD + SDIOD_INTSTATUS, I_HMB_HOST_INT);
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_CARD, ==, 0);
    qtest_quit(qts);
}

/* With CCCR 0xf2 set the chip uses its pin and leaves the bus alone */
static void test_oob(const void *data)
{
    const Ident *id = data;
    QTestState *qts = card_init(id->card, "");

    qtest_irq_intercept_out_named(qts, "/machine/peripheral/w", HOST_WAKE);
    sdio_enumerate(qts);
    irq_enable(qts);
    sd_io_direct(qts, true, 0, CCCR_SEPINT, 0x07);
    g_assert_false(qtest_get_irq(qts, 0));
    fw_boot(qts);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readl(qts, SDHCI + SD_INT) & SD_INT_CARD, ==, 0);
    bp_write(qts, CORE_SDIOD + SDIOD_INTSTATUS, I_HMB_HOST_INT);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

/* An ioctl through function 2: the SDPCM frame, then the BCDC message */
static uint32_t ioctl(QTestState *qts, uint32_t cmd, bool set,
                      const void *buf, size_t blen, uint8_t *out)
{
    uint8_t f[256] = {};
    uint8_t r[256] = {};
    size_t total = ROUND_UP(SDPCM_HDR_LEN + CDC_HDR_LEN + blen, 4);
    size_t rlen;

    stw_le_p(f, total);
    stw_le_p(f + 2, ~total);
    f[7] = SDPCM_HDR_LEN;
    stl_le_p(f + SDPCM_HDR_LEN, cmd);
    stl_le_p(f + SDPCM_HDR_LEN + 4, blen);
    stl_le_p(f + SDPCM_HDR_LEN + 8, set ? CDCF_IOC_SET : 0);
    memcpy(f + SDPCM_HDR_LEN + CDC_HDR_LEN, buf, blen);
    sd_io_extended(qts, true, 2, 0, false, f, total);

    sd_io_extended(qts, false, 2, 0, false, r, 4);
    rlen = lduw_le_p(r);
    g_assert_cmpuint(rlen, ==, SDPCM_HDR_LEN + CDC_HDR_LEN + blen);
    sd_io_extended(qts, false, 2, 0, false, r + 4, ROUND_UP(rlen, 4) - 4);
    if (out) {
        memcpy(out, r + SDPCM_HDR_LEN + CDC_HDR_LEN, blen);
    }
    return ldl_le_p(r + SDPCM_HDR_LEN + 8);
}

/* name, NUL, value */
static size_t iovar(uint8_t *buf, const char *name, uint32_t val)
{
    size_t n = strlen(name) + 1;

    memcpy(buf, name, n);
    stl_le_p(buf + n, val);
    return n + 4;
}

static void test_commands(const void *data)
{
    const Ident *id = data;
    QTestState *qts = card_init(id->card, "");
    uint8_t buf[64] = {}, out[64];
    uint32_t zero = 0;
    size_t n;

    sdio_enumerate(qts);
    fw_boot(qts);

    /* frameburst */
    g_assert_cmphex(ioctl(qts, WLC_SET_FAKEFRAG, true, &(uint32_t){ 1 }, 4,
                          NULL) & CDCF_IOC_ERROR, ==, 0);
    /* join_pref: two RSSI preferences, four bytes each */
    n = iovar(buf, "join_pref", 0x00020000 | 0x100);
    g_assert_cmphex(ioctl(qts, WLC_SET_VAR, true, buf, n + 4, NULL) &
                    CDCF_IOC_ERROR, ==, 0);
    /* the transmit power, a quarter dBm */
    n = iovar(buf, "qtxpower", 0);
    g_assert_cmphex(ioctl(qts, WLC_GET_VAR, false, buf, 32, out) &
                    CDCF_IOC_ERROR, ==, 0);
    g_assert_cmpuint(ldl_le_p(out), ==, 17 * 4);
    /* the CLM blob goes in a chunk, then the status reads clean */
    n = iovar(buf, "clmload", 0);
    g_assert_cmphex(ioctl(qts, WLC_SET_VAR, true, buf, n + 16, NULL) &
                    CDCF_IOC_ERROR, ==, 0);
    memcpy(buf, "clmload_status", 15);
    memcpy(out, &zero, 4);
    g_assert_cmphex(ioctl(qts, WLC_GET_VAR, false, buf, 32, out) &
                    CDCF_IOC_ERROR, ==, 0);
    g_assert_cmpuint(ldl_le_p(out), ==, 0);
    /* the power save timeout, and the access categories after a join */
    n = iovar(buf, "pm2_sleep_ret", 200);
    g_assert_cmphex(ioctl(qts, WLC_SET_VAR, true, buf, n, NULL) &
                    CDCF_IOC_ERROR, ==, 0);
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "wme_ac_sta", 11);
    g_assert_cmphex(ioctl(qts, WLC_GET_VAR, false, buf, 64, out) &
                    CDCF_IOC_ERROR, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const Ident chips[] = {
        { "bcm43438", 0x1541a9a6, 512 * 1024 },
        { "bcm43436", 0x1542a9a6, 512 * 1024 },
        { "bcm43362", 0x1501a962, 240 * 1024 },
    };

    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_machine("ingenic-t41")) {
        return g_test_run();
    }
    for (int i = 0; i < ARRAY_SIZE(chips); i++) {
        static const struct {
            const char *name;
            void (*fn)(const void *);
        } cases[] = {
            { "ids", test_ids }, { "inband", test_inband },
            { "oob", test_oob }, { "commands", test_commands },
        };

        if (!qtest_has_device(chips[i].card)) {
            continue;
        }
        for (int j = 0; j < ARRAY_SIZE(cases); j++) {
            g_autofree char *name = g_strdup_printf("bcm/%s/%s",
                                                    chips[i].card,
                                                    cases[j].name);

            qtest_add_data_func(name, &chips[i], cases[j].fn);
        }
    }
    return g_test_run();
}
