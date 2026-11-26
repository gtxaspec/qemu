/*
 * QTest testcase for the ATBM6441 "Hera" paravirtual LMAC port
 *
 * The port as a firmware's stand-in LMAC drives it, with no firmware: the
 * startup indication after RUN, a request answered in place, a scan that
 * hears the simulated access point's beacon, a transmit without a confirm,
 * and a message too long for the buffer given.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/net/atbm-hera-lmac.h"
#include "atbm-hera-test.h"

#define L(r)            (ATBM_HERA_LMAC_BASE + (r))

#define REQ_BUF         (HERA_SRAM + 0x1000)
#define IND_BUF         (HERA_SRAM + 0x2000)
#define BUF_SIZE        1632

#define WSM_STARTUP_IND         0x0801
#define WSM_RECEIVE_IND         0x0804
#define WSM_SCAN_COMPLETE_IND   0x0806
#define WSM_CONFIGURATION_REQ   0x0009
#define WSM_START_SCAN_REQ      0x0007
#define WSM_TX_REQ              0x0004

#define AP_CHANNEL      6

static QTestState *lmac_init(void)
{
    QTestState *qts = hera_init("-global atbm-hera-lmac.channel=6");

    qtest_writel(qts, L(ATBM_HERA_LMAC_IND_ADDR), IND_BUF);
    qtest_writel(qts, L(ATBM_HERA_LMAC_IND_SIZE), BUF_SIZE);
    return qts;
}

/* RUN, and the startup indication shortly after */
static void lmac_run(QTestState *qts)
{
    qtest_writel(qts, L(ATBM_HERA_LMAC_IRQ_EN), 1);
    qtest_writel(qts, L(ATBM_HERA_LMAC_CTRL), ATBM_HERA_LMAC_CTRL_RUN);
    qtest_clock_step(qts, 50 * 1000 * 1000);
}

static uint16_t ind_id(QTestState *qts)
{
    return qtest_readw(qts, IND_BUF + 2) & 0x1fff;
}

/* Put a request in the request buffer and hand it over */
static uint32_t lmac_req(QTestState *qts, uint16_t id, const uint8_t *body,
                         size_t len)
{
    qtest_writew(qts, REQ_BUF, 4 + len);
    qtest_writew(qts, REQ_BUF + 2, id);
    qtest_memwrite(qts, REQ_BUF + 4, body, len);
    qtest_writel(qts, L(ATBM_HERA_LMAC_REQ_SIZE), BUF_SIZE);
    qtest_writel(qts, L(ATBM_HERA_LMAC_REQ), REQ_BUF);
    return qtest_readl(qts, L(ATBM_HERA_LMAC_REQ_RESULT));
}

static void test_id(void)
{
    QTestState *qts = lmac_init();

    g_assert_cmphex(qtest_readl(qts, L(ATBM_HERA_LMAC_ID)), ==,
                    ATBM_HERA_LMAC_MAGIC);
    g_assert_cmpuint(qtest_readl(qts, L(ATBM_HERA_LMAC_VERSION)), ==,
                     ATBM_HERA_LMAC_ABI);
    /* stopped: nothing for the guest, no interrupt */
    g_assert_cmphex(qtest_readl(qts, L(ATBM_HERA_LMAC_STATUS)), ==, 0);
    g_assert_false(hera_irq(qts, ATBM_HERA_LMAC_IRQ));
    qtest_quit(qts);
}

static void test_startup(void)
{
    QTestState *qts = lmac_init();
    uint32_t st;

    lmac_run(qts);
    st = qtest_readl(qts, L(ATBM_HERA_LMAC_STATUS));
    g_assert_cmphex(st & ATBM_HERA_LMAC_STATUS_PEND, ==,
                    ATBM_HERA_LMAC_STATUS_PEND);
    g_assert_true(hera_irq(qts, ATBM_HERA_LMAC_IRQ));
    g_assert_cmpuint(qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP)), ==,
                     st >> 16);
    g_assert_cmphex(ind_id(qts), ==, WSM_STARTUP_IND);
    /* input buffers: as many as the firmware model says, of 1632 bytes */
    g_assert_cmpuint(qtest_readw(qts, IND_BUF + 4), >, 0);
    g_assert_cmpuint(qtest_readw(qts, IND_BUF + 6), ==, BUF_SIZE);
    /* drained: the line drops, and a further pop gives nothing */
    g_assert_false(hera_irq(qts, ATBM_HERA_LMAC_IRQ));
    g_assert_cmpuint(qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP)), ==, 0);

    /* stopping drops the model's state; running again starts it again */
    qtest_writel(qts, L(ATBM_HERA_LMAC_CTRL), 0);
    lmac_run(qts);
    g_assert_cmpuint(qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP)), >, 0);
    g_assert_cmphex(ind_id(qts), ==, WSM_STARTUP_IND);
    qtest_quit(qts);
}

static void test_confirm(void)
{
    QTestState *qts = lmac_init();
    uint8_t body[16] = {};
    uint8_t mac[6];
    uint32_t r;

    lmac_run(qts);
    qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP));
    r = lmac_req(qts, WSM_CONFIGURATION_REQ, body, sizeof(body));
    g_assert_cmphex(r & ATBM_HERA_LMAC_RESULT_CNF, ==,
                    ATBM_HERA_LMAC_RESULT_CNF);
    /* the confirm over the request: its id, status 0, then the MAC */
    g_assert_cmpuint(qtest_readw(qts, REQ_BUF), ==, r >> 16);
    g_assert_cmphex(qtest_readw(qts, REQ_BUF + 2) & 0x1fff, ==,
                    WSM_CONFIGURATION_REQ | 0x400);
    g_assert_cmphex(qtest_readl(qts, REQ_BUF + 4), ==, 0);
    qtest_memread(qts, REQ_BUF + 8, mac, sizeof(mac));
    g_assert_cmphex(ldl_le_p(mac), ==,
                    qtest_readl(qts, L(ATBM_HERA_LMAC_MAC_LO)));
    g_assert_cmphex(lduw_le_p(mac + 4), ==,
                    qtest_readl(qts, L(ATBM_HERA_LMAC_MAC_HI)));
    /* the confirm was not also queued for the guest */
    g_assert_cmphex(qtest_readl(qts, L(ATBM_HERA_LMAC_STATUS)), ==, 0);
    qtest_quit(qts);
}

/* A scan of the access point's channel hears its beacon, then completes */
static void test_scan(void)
{
    QTestState *qts = lmac_init();
    uint8_t scan[12 + 16] = {};
    uint32_t len;

    lmac_run(qts);
    qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP));
    scan[9] = 1;
    stw_le_p(&scan[12], AP_CHANNEL);
    g_assert_cmphex(lmac_req(qts, WSM_START_SCAN_REQ, scan, sizeof(scan)) &
                    ATBM_HERA_LMAC_RESULT_CNF, ==, ATBM_HERA_LMAC_RESULT_CNF);
    g_assert_cmphex(qtest_readl(qts, REQ_BUF + 4), ==, 0);
    qtest_clock_step(qts, 100 * 1000 * 1000);
    g_assert_true(hera_irq(qts, ATBM_HERA_LMAC_IRQ));

    len = qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP));
    g_assert_cmpuint(len, >, 4 + 16 + 24);
    g_assert_cmphex(ind_id(qts), ==, WSM_RECEIVE_IND);
    /* a beacon, after the receive descriptor */
    g_assert_cmphex(qtest_readw(qts, IND_BUF + 4 + 16), ==, 0x0080);
    g_assert_cmpuint(qtest_readw(qts, IND_BUF + 4 + 4), ==, AP_CHANNEL);

    g_assert_cmpuint(qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP)), >, 0);
    g_assert_cmphex(ind_id(qts), ==, WSM_SCAN_COMPLETE_IND);
    g_assert_false(hera_irq(qts, ATBM_HERA_LMAC_IRQ));
    qtest_quit(qts);
}

/* A transmit that asks for no confirm leaves the buffer as it was */
static void test_tx_no_confirm(void)
{
    QTestState *qts = lmac_init();
    uint8_t tx[20 + 24] = {};

    lmac_run(qts);
    qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP));
    stl_le_p(tx, 0x1234);
    stw_le_p(&tx[20], 0x0048);          /* a null data frame */
    g_assert_cmphex(lmac_req(qts, WSM_TX_REQ, tx, sizeof(tx)), ==, 0);
    g_assert_cmphex(qtest_readw(qts, REQ_BUF + 2), ==, WSM_TX_REQ);
    qtest_quit(qts);
}

/* A message longer than the buffer is dropped and counted */
static void test_too_long(void)
{
    QTestState *qts = lmac_init();

    qtest_writel(qts, L(ATBM_HERA_LMAC_IND_SIZE), 16);
    lmac_run(qts);
    g_assert_cmpuint(qtest_readl(qts, L(ATBM_HERA_LMAC_IND_POP)), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, L(ATBM_HERA_LMAC_IND_DROPPED)), ==, 1);
    g_assert_cmphex(qtest_readl(qts, L(ATBM_HERA_LMAC_STATUS)), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/lmac/id", test_id);
    qtest_add_func("/atbm-hera/lmac/startup", test_startup);
    qtest_add_func("/atbm-hera/lmac/confirm", test_confirm);
    qtest_add_func("/atbm-hera/lmac/scan", test_scan);
    qtest_add_func("/atbm-hera/lmac/tx-no-confirm", test_tx_no_confirm);
    qtest_add_func("/atbm-hera/lmac/too-long", test_too_long);

    return g_test_run();
}
