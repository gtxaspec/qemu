/*
 * QTest testcases for the Ingenic A1 XGMAC's DMA
 *
 * Receive: a frame that arrives while the driver has the receive DMA
 * stopped, as stmmac does while it resets the adapter and the ring may be
 * freed, must not be written to the ring; it is delivered once the DMA is
 * started.
 *
 * Transmit: stmmac stops the DMA and frees the ring while it resets the
 * adapter, and the ring it allocates next lives elsewhere. Neither a
 * kick of the tail pointer with the DMA stopped nor one after a new
 * ring was programmed may touch the old ring, which holds other data by
 * then.
 *
 * Registers: VERSION and the hardware feature registers read as an XGMAC
 * 3.1 core, as the A1N does, and the PTP system time commands self-clear.
 *
 * Transmit offload: checksum insertion and TCP segmentation.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/sockets.h"

#define GMAC            0x130b0000
#define VERSION         (GMAC + 0x0110)
#define HW_FEATURE0     (GMAC + 0x011c)
#define HW_FEATURE1     (GMAC + 0x0120)
#define HW_FEATURE2     (GMAC + 0x0124)
#define HW_FEATURE3     (GMAC + 0x0128)
#define HW_FEATURE4     (GMAC + 0x012c)
#define PTP_TCR         (GMAC + 0x0d00)
#define PTP_STSR        (GMAC + 0x0d08)
#define PTP_STSUR       (GMAC + 0x0d10)
#define PTP_STNSUR      (GMAC + 0x0d14)
#define RX_CONFIG       (GMAC + 0x0004)
#define RX_CONTROL      (GMAC + 0x3108)
#define RXDESC_LADDR    (GMAC + 0x311c)
#define RXDESC_RING     (GMAC + 0x3134)
#define TX_CONTROL      (GMAC + 0x3104)
#define TXDESC_LADDR    (GMAC + 0x3114)
#define TXDESC_TAIL     (GMAC + 0x3124)
#define TXDESC_RING     (GMAC + 0x3130)
#define CH_STATUS       (GMAC + 0x3160)
#define RAM             0x01000000
#define RING            (RAM + 0x1000)
#define BUF             (RAM + 0x2000)

static void test_rx_gated(void)
{
    struct sockaddr_in me = { .sin_family = AF_INET,
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    struct sockaddr_in qemu = me;
    socklen_t len = sizeof(me);
    uint8_t frame[64] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x52, 0x54,
                          0x00, 0x12, 0x34, 0x56, 0x08, 0x00, 0xaa, 0x55 };
    QTestState *qts;
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    int qp;
    uint32_t des3;

    g_assert_cmpint(sk, >=, 0);
    g_assert_cmpint(bind(sk, (struct sockaddr *)&me, sizeof(me)), ==, 0);
    getsockname(sk, (struct sockaddr *)&me, &len);
    {
        int probe = socket(AF_INET, SOCK_DGRAM, 0);

        bind(probe, (struct sockaddr *)&qemu, sizeof(qemu));
        len = sizeof(qemu);
        getsockname(probe, (struct sockaddr *)&qemu, &len);
        qp = ntohs(qemu.sin_port);
        close(probe);
    }
    qts = qtest_initf("-machine ingenic-a1 -nic socket,udp=127.0.0.1:%d,"
                      "localaddr=127.0.0.1:%d", ntohs(me.sin_port), qp);

    /* one owned descriptor: buffer address, buffer 2 address, length, OWN */
    qtest_writel(qts, RING, BUF);
    qtest_writel(qts, RING + 4, 0);
    qtest_writel(qts, RING + 8, 0);
    qtest_writel(qts, RING + 12, 0x80000000);
    qtest_writel(qts, BUF, 0);
    qtest_writel(qts, RXDESC_LADDR, RING);
    qtest_writel(qts, RXDESC_RING, 0);

    me.sin_port = htons(qp);
    g_assert_cmpint(sendto(sk, frame, sizeof(frame), 0,
                           (struct sockaddr *)&me, sizeof(me)), ==,
                    sizeof(frame));
    qtest_clock_step(qts, 1000000);
    qtest_readl(qts, CH_STATUS);

    g_assert_cmphex(qtest_readl(qts, BUF), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RING + 12), ==, 0x80000000);

    qtest_writel(qts, RX_CONTROL, 1);
    qtest_writel(qts, RX_CONFIG, 1);
    for (int i = 0; i < 50; i++) {
        qtest_clock_step(qts, 1000000);
        des3 = qtest_readl(qts, RING + 12);
        if (!(des3 & 0x80000000)) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmphex(des3 & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, BUF + 12) >> 16, ==, 0x55aa);

    close(sk);
    qtest_quit(qts);
}

/* A kernel pointer where a descriptor's OWN bit would be: bit 31 is set */
#define PTR             0x8aff2000

static void tx_ring(QTestState *qts, uint32_t ring, uint32_t last)
{
    /* two descriptors, the last word of each OWN (or a pointer) */
    qtest_writel(qts, ring, BUF);
    qtest_writel(qts, ring + 4, 0);
    qtest_writel(qts, ring + 8, 0);
    qtest_writel(qts, ring + 12, 0x80000000 | (1u << 29) | (1u << 28));
    qtest_writel(qts, ring + 16, BUF);
    qtest_writel(qts, ring + 20, 0);
    qtest_writel(qts, ring + 24, 0);
    qtest_writel(qts, ring + 28, last);
    qtest_writel(qts, TXDESC_LADDR, ring);
    qtest_writel(qts, TXDESC_RING, 1);
}

static void test_tx_stopped(void)
{
    QTestState *qts = qtest_init("-machine ingenic-a1 -nic none");

    tx_ring(qts, RING, PTR);
    qtest_writel(qts, TXDESC_TAIL, RING + 32);
    g_assert_cmphex(qtest_readl(qts, RING + 12), ==,
                    0x80000000 | (1u << 29) | (1u << 28));

    qtest_writel(qts, TX_CONTROL, 1);
    qtest_writel(qts, TXDESC_TAIL, RING + 16);
    g_assert_cmphex(qtest_readl(qts, RING + 12) & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RING + 28), ==, PTR);
    qtest_quit(qts);
}

static void test_tx_new_ring(void)
{
    QTestState *qts = qtest_init("-machine ingenic-a1 -nic none");
    uint32_t ring2 = RING + 0x1000;

    tx_ring(qts, RING, 0);
    qtest_writel(qts, TX_CONTROL, 1);
    qtest_writel(qts, TXDESC_TAIL, RING + 16);
    g_assert_cmphex(qtest_readl(qts, RING + 12) & 0x80000000, ==, 0);

    /* the old ring is freed and reused: its next slot holds a pointer */
    qtest_writel(qts, TX_CONTROL, 0);
    qtest_writel(qts, RING + 28, PTR);
    tx_ring(qts, ring2, 0);
    qtest_writel(qts, TX_CONTROL, 1);
    qtest_writel(qts, TXDESC_TAIL, ring2 + 16);

    g_assert_cmphex(qtest_readl(qts, RING + 28), ==, PTR);
    g_assert_cmphex(qtest_readl(qts, ring2 + 12) & 0x80000000, ==, 0);
    qtest_quit(qts);
}

static void test_version(void)
{
    QTestState *qts = qtest_init("-machine ingenic-a1 -nic none");
    uint32_t f0, f1;

    /* user ID 0x76, Synopsys ID 0x31, as read from an A1N */
    g_assert_cmphex(qtest_readl(qts, VERSION), ==, 0x00207631);

    f0 = qtest_readl(qts, HW_FEATURE0);
    g_assert_cmpuint((f0 >> 23) & 3, ==, 2);        /* active PHY interface */
    g_assert_true(f0 & (1u << 16));                 /* RX checksum */
    g_assert_true(f0 & (1u << 14));                 /* TX checksum */
    g_assert_true(f0 & (1u << 12));                 /* timestamping */
    g_assert_true(f0 & (1u << 1));                  /* GMII: 10/100 too */

    f1 = qtest_readl(qts, HW_FEATURE1);
    g_assert_cmpuint((f1 >> 27) & 15, ==, 8);       /* 8 L3/L4 filters */
    g_assert_true(f1 & (1u << 18));                 /* TSO */
    g_assert_true(f1 & (1u << 17));                 /* split header */
    g_assert_cmpuint((f1 >> 14) & 3, ==, 0);        /* 32-bit DMA */

    g_assert_cmphex(qtest_readl(qts, HW_FEATURE2), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HW_FEATURE3), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HW_FEATURE4), ==, 0);
    qtest_quit(qts);
}

static void test_ptp_init(void)
{
    QTestState *qts = qtest_init("-machine ingenic-a1 -nic none");

    qtest_writel(qts, PTP_STSUR, 1000);
    qtest_writel(qts, PTP_STNSUR, 0);
    qtest_writel(qts, PTP_TCR, 0x1 | 0x4 | 0x200);
    g_assert_cmphex(qtest_readl(qts, PTP_TCR) & 0x4, ==, 0);
    g_assert_cmpuint(qtest_readl(qts, PTP_STSR), >=, 1000);
    g_assert_cmpuint(qtest_readl(qts, PTP_STSR), <, 1002);
    qtest_quit(qts);
}

/* A UDP socket QEMU's NIC sends to, and the NIC's address for it */
static int tx_socket(char *args, size_t n)
{
    struct sockaddr_in me = { .sin_family = AF_INET,
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    struct sockaddr_in qemu = me;
    socklen_t len = sizeof(me);
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { .tv_sec = 10 };

    g_assert_cmpint(sk, >=, 0);
    bind(sk, (struct sockaddr *)&me, sizeof(me));
    getsockname(sk, (struct sockaddr *)&me, &len);
    bind(probe, (struct sockaddr *)&qemu, sizeof(qemu));
    len = sizeof(qemu);
    getsockname(probe, (struct sockaddr *)&qemu, &len);
    close(probe);
    setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    snprintf(args, n, "-machine ingenic-a1 -nic socket,udp=127.0.0.1:%d,"
             "localaddr=127.0.0.1:%d", ntohs(me.sin_port),
             ntohs(qemu.sin_port));
    return sk;
}

static uint32_t sum16(const uint8_t *p, size_t n, uint32_t sum)
{
    for (; n > 1; n -= 2, p += 2) {
        sum += (p[0] << 8) | p[1];
    }
    if (n) {
        sum += p[0] << 8;
    }
    return sum;
}

static bool ok16(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return sum == 0xffff;
}

static void ip_headers(uint8_t *f, unsigned proto, unsigned l4len,
                       unsigned paylen)
{
    memset(f, 0, 14 + 20 + l4len + paylen);
    memcpy(f, "\x52\x55\x0a\x00\x02\x02\x52\x54\x00\x12\x34\x56", 12);
    f[12] = 0x08;
    f[14] = 0x45;
    f[14 + 2] = (20 + l4len + paylen) >> 8;
    f[14 + 3] = (20 + l4len + paylen) & 0xff;
    f[14 + 8] = 64;
    f[14 + 9] = proto;
    f[14 + 12] = 10; f[14 + 15] = 15;
    f[14 + 16] = 10; f[14 + 19] = 2;
}

static void test_tx_csum(void)
{
    char args[200];
    int sk = tx_socket(args, sizeof(args));
    QTestState *qts = qtest_init(args);
    uint8_t f[14 + 20 + 8 + 12], r[128];
    uint32_t pseudo;
    ssize_t n;

    ip_headers(f, 17, 8, 12);
    f[14 + 20 + 4] = 0;
    f[14 + 20 + 5] = 20;                    /* UDP length */
    f[14 + 20 + 6] = 0x12;                  /* a nonzero partial sum */
    memset(f + 14 + 20 + 8, 0x5a, 12);
    qtest_memwrite(qts, BUF, f, sizeof(f));

    qtest_writel(qts, RING, BUF);
    qtest_writel(qts, RING + 4, 0);
    qtest_writel(qts, RING + 8, sizeof(f));
    /* FD, LD, full checksum insertion */
    qtest_writel(qts, RING + 12, 0x80000000 | (1u << 29) | (1u << 28) |
                 (3u << 16));
    qtest_writel(qts, TXDESC_LADDR, RING);
    qtest_writel(qts, TXDESC_RING, 0);
    qtest_writel(qts, TX_CONTROL, 1);
    qtest_writel(qts, TXDESC_TAIL, RING + 16);

    n = recv(sk, r, sizeof(r), 0);
    g_assert_cmpint(n, ==, sizeof(f));
    g_assert_true(ok16(sum16(r + 14, 20, 0)));
    pseudo = sum16(r + 14 + 12, 8, 17 + 20);
    g_assert_true(ok16(sum16(r + 14 + 20, 20, pseudo)));

    close(sk);
    qtest_quit(qts);
}

static void test_tx_tso(void)
{
    char args[200];
    int sk = tx_socket(args, sizeof(args));
    QTestState *qts = qtest_init(args);
    enum { PAY = 2500, MSS = 1000, HDR = 14 + 20 + 20 };
    uint8_t f[HDR + PAY], r[2048];
    int segs[3] = { 1000, 1000, 500 };

    ip_headers(f, 6, 20, PAY);
    f[HDR - 20 + 4] = 0x00; f[HDR - 20 + 7] = 0x64;     /* seq 100 */
    f[HDR - 20 + 12] = 0x50;                           /* 20 byte header */
    f[HDR - 20 + 13] = 0x19;                           /* FIN, PSH, ACK */
    for (int i = 0; i < PAY; i++) {
        f[HDR + i] = i;
    }
    qtest_memwrite(qts, BUF, f, sizeof(f));

    /* context descriptor: MSS */
    qtest_writel(qts, RING, 0);
    qtest_writel(qts, RING + 4, 0);
    qtest_writel(qts, RING + 8, MSS);
    qtest_writel(qts, RING + 12, 0x80000000 | (1u << 30) | (1u << 26));
    /* TSE: headers in buffer 1, the payload in buffer 2 */
    qtest_writel(qts, RING + 16, BUF);
    qtest_writel(qts, RING + 20, BUF + HDR);
    qtest_writel(qts, RING + 24, HDR | (PAY << 16));
    qtest_writel(qts, RING + 28, 0x80000000 | (1u << 29) | (1u << 28) |
                 (1u << 18) | (5u << 19) | PAY);
    qtest_writel(qts, TXDESC_LADDR, RING);
    qtest_writel(qts, TXDESC_RING, 1);
    qtest_writel(qts, TX_CONTROL, 1);
    qtest_writel(qts, TXDESC_TAIL, RING + 32);

    for (int i = 0; i < 3; i++) {
        ssize_t n = recv(sk, r, sizeof(r), 0);
        uint32_t seq;

        g_assert_cmpint(n, ==, HDR + segs[i]);
        g_assert_cmpuint((r[16] << 8) | r[17], ==, 20 + 20 + segs[i]);
        g_assert_true(ok16(sum16(r + 14, 20, 0)));
        seq = (r[14 + 20 + 4] << 24) | (r[14 + 20 + 5] << 16) |
              (r[14 + 20 + 6] << 8) | r[14 + 20 + 7];
        g_assert_cmpuint(seq, ==, 100 + i * MSS);
        /* FIN and PSH on the last segment only */
        g_assert_cmphex(r[14 + 20 + 13] & 0x09, ==, i == 2 ? 0x09 : 0);
        g_assert_true(ok16(sum16(r + 14 + 20, 20 + segs[i],
                                 sum16(r + 14 + 12, 8, 6 + 20 + segs[i]))));
        g_assert_cmphex(r[HDR], ==, (i * MSS) & 0xff);
    }

    close(sk);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("ingenic-a1-xgmac/version", test_version);
    qtest_add_func("ingenic-a1-xgmac/ptp-init", test_ptp_init);
    qtest_add_func("ingenic-a1-xgmac/tx-csum", test_tx_csum);
    qtest_add_func("ingenic-a1-xgmac/tx-tso", test_tx_tso);
    qtest_add_func("ingenic-a1-xgmac/rx-gated", test_rx_gated);
    qtest_add_func("ingenic-a1-xgmac/tx-stopped", test_tx_stopped);
    qtest_add_func("ingenic-a1-xgmac/tx-new-ring", test_tx_new_ring);
    return g_test_run();
}
