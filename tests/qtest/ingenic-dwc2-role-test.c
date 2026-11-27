/*
 * QTest testcase for the DWC2 role and the root port
 *
 * The role follows GUSBCFG's ForceHstMode and ForceDevMode over the ID
 * pin, as the mainline driver switches it, with no write to the PHY's ID
 * override: GINTSTS.CurMod follows, and the D+ pull-up reaches a host
 * cabled to the device side. Leaving host mode drops the root port, and a
 * device that stayed plugged in is found again on the way back, with a
 * connect interrupt. A full-speed device straight on the root port
 * reports its speed and takes a control transfer.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "qobject/qdict.h"
#include "hw/usb/dwc2-regs.h"

#define OTG         0x13500000
#define BUF         0x01000000

static uint32_t rd(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, OTG + off);
}

static void wr(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, OTG + off, val);
}

static bool host_mode(QTestState *qts)
{
    return rd(qts, GINTSTS) & GINTSTS_CURMODE_HOST;
}

static void test_role(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t41");

    g_assert_true(host_mode(qts));

    wr(qts, GUSBCFG, GUSBCFG_FORCEDEVMODE);
    g_assert_false(host_mode(qts));
    wr(qts, GUSBCFG, GUSBCFG_FORCEHOSTMODE);
    g_assert_true(host_mode(qts));
    wr(qts, GUSBCFG, 0);
    g_assert_true(host_mode(qts));

    /* the ID pin decides when nothing is forced, and the force wins */
    wr(qts, GOTGCTL, GOTGCTL_CONID_B);
    g_assert_false(host_mode(qts));
    wr(qts, GUSBCFG, GUSBCFG_FORCEHOSTMODE);
    g_assert_true(host_mode(qts));
    wr(qts, GUSBCFG, 0);
    g_assert_false(host_mode(qts));
    wr(qts, GOTGCTL, 0);
    g_assert_true(host_mode(qts));

    qtest_quit(qts);
}

static char *ncm_state(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                           "{'path': '/machine/peripheral/h', "
                           "'property': 'state'}}");
    char *s = g_strdup(qdict_get_str(rsp, "return"));

    qobject_unref(rsp);
    return s;
}

static void test_pullup(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t41 -netdev user,id=u "
                                 "-device dwc2-ncm-host,netdev=u,id=h");
    g_autofree char *st = ncm_state(qts);

    g_assert_cmpstr(st, ==, "idle");

    /* the driver forces device mode, then lets the pull-up out */
    wr(qts, GUSBCFG, GUSBCFG_FORCEDEVMODE);
    wr(qts, DCTL, 0);
    g_free(st);
    st = ncm_state(qts);
    g_assert_cmpstr(st, ==, "debounce");

    wr(qts, GUSBCFG, GUSBCFG_FORCEHOSTMODE);
    g_free(st);
    st = ncm_state(qts);
    g_assert_cmpstr(st, ==, "idle");

    qtest_quit(qts);
}

static void test_redetect(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t41 -device usb-kbd");
    uint32_t hprt = rd(qts, HPRT0);

    g_assert_true(hprt & HPRT0_CONNSTS);
    g_assert_cmpuint((hprt & HPRT0_SPD_MASK) >> HPRT0_SPD_SHIFT, ==,
                     HPRT0_SPD_FULL_SPEED);
    wr(qts, HPRT0, HPRT0_PWR | HPRT0_CONNDET);
    wr(qts, GINTSTS, GINTSTS_PRTINT);

    /* a session as a device */
    wr(qts, GUSBCFG, GUSBCFG_FORCEDEVMODE);
    g_assert_false(host_mode(qts));
    g_assert_false(rd(qts, HPRT0) & HPRT0_CONNSTS);

    /* back to host: the keyboard is still there */
    wr(qts, GUSBCFG, GUSBCFG_FORCEHOSTMODE);
    g_assert_true(host_mode(qts));
    hprt = rd(qts, HPRT0);
    g_assert_true(hprt & HPRT0_CONNSTS);
    g_assert_true(hprt & HPRT0_CONNDET);
    g_assert_cmpuint((hprt & HPRT0_SPD_MASK) >> HPRT0_SPD_SHIFT, ==,
                     HPRT0_SPD_FULL_SPEED);
    g_assert_true(rd(qts, GINTSTS) & GINTSTS_PRTINT);

    qtest_quit(qts);
}

static void channel(QTestState *qts, uint32_t chr, uint32_t pid, uint32_t len)
{
    wr(qts, HCINT(0), 0xffffffff);
    wr(qts, HCTSIZ(0), len | (1 << 19) | (pid << 29));
    wr(qts, HCDMA(0), BUF);
    wr(qts, HCCHAR(0), chr | HCCHAR_CHENA);
    for (int i = 0; i < 100; i++) {
        qtest_clock_step(qts, 1000000);
        if (rd(qts, HCINT(0)) & 1) {
            return;
        }
    }
    g_test_fail();
}

static void test_fs_root(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t41 -device usb-kbd");
    uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 8, 0 };
    uint8_t desc[8];
    uint32_t chr = 8;       /* MPS 8, EP0, control, address 0, OUT */

    wr(qts, HPRT0, HPRT0_PWR | HPRT0_CONNDET);
    wr(qts, HPRT0, HPRT0_PWR | HPRT0_RST);
    qtest_clock_step(qts, 50000000);
    wr(qts, HPRT0, HPRT0_PWR);
    g_assert_true(rd(qts, HPRT0) & HPRT0_ENA);

    qtest_memwrite(qts, BUF, setup, 8);
    channel(qts, chr, 3, 8);                    /* SETUP */
    channel(qts, chr | HCCHAR_EPDIR, 2, 8);     /* DATA1 IN */
    qtest_memread(qts, BUF, desc, 8);
    g_assert_cmpuint(desc[0], ==, 18);
    g_assert_cmpuint(desc[1], ==, 1);
    g_assert_cmpuint(desc[7], ==, 8);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("ingenic/dwc2-role/force", test_role);
    qtest_add_func("ingenic/dwc2-role/pullup", test_pullup);
    qtest_add_func("ingenic/dwc2-role/redetect", test_redetect);
    qtest_add_func("ingenic/dwc2-role/fs-root", test_fs_root);
    return g_test_run();
}
