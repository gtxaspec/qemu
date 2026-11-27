/*
 * QTest testcase for the CPM of a direct Linux boot (-kernel uImage)
 *
 * The dividers and clock sources as thingino's U-Boot leaves them, read
 * back at its prompt on each part (a T31X's: CPU 1392 MHz, L2 696, AHB0 and
 * AHB2 200, PCLK 100, DDR 600, MAC and SFC 50, MSC0 24). A flash boot
 * leaves the CPM to the firmware: its CPCCR stays as the bootrom has it.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define CPM             0x10000000
#define CPCCR           0x00
#define CPAPCR          0x10
#define CPMPCR          0x14
#define DDRCDR          0x2c
#define MACCDR          0x54
#define MSC0CDR         0x68
#define SSICDR          0x74
#define CPVPCR          0xe0

typedef struct {
    const char *machine;
    const char *variant;
    uint32_t cpccr, ddrcdr, maccdr, msc0cdr, ssicdr;
    /* in MHz, 0: the PLLs are not the MNOD form the decode below reads */
    unsigned cpu, l2, ahb0, ahb2, pclk, ddr;
    unsigned mpll;          /* in MHz, 0: not checked */
    bool nodiv;             /* only the PLLs, CPCCR and DDRCDR are set */
} Case;

static const Case cases[] = {
    { "ingenic-t10", "t10l", 0x9a7b5510, 0xa0000002, 0xa0000017, 0x2000000e,
      0xe0000017 },
    { "ingenic-t10", "t10n", 0x9a7b5510, 0xa0000002, 0xa0000017, 0x20000011,
      0xe0000017 },
    { "ingenic-t20", "t20l", 0x9a794410, 0xa0000001, 0x60000013, 0x2000000e,
      0xa000000e },
    { "ingenic-t20", "t20n", 0x9a794410, 0xa0000001, 0x60000013, 0x20000011,
      0xa0000011 },
    { "ingenic-t21", "t21n", 0x9a773310, 0xa0000001, 0x60000023, 0x20000007,
      0x50000010 },
    { "ingenic-t20", "t20x", 0x9a794410, 0xa0000001, 0x60000013, 0x80000014,
      0xa0000013, 0, 0, 0, 0, 0, 0, 1000 },
    { "ingenic-t23", "t23n", 0x9a7b5510, 0xa0000001, 0x60000000, 0x60000031,
      0x50000017, 1188, 594, 200, 200, 100, 600 },
    { "ingenic-t23", "t23dl", 0x9a7b5510, 0xa0000001, 0x60000017, 0x60000031,
      0x50000017, 1188, 594, 200, 200, 100, 600 },
    { "ingenic-t23", "t23zn", 0x9a7b5510, 0xa0000002, 0x60000000, 0x60000031,
      0x50000017, 1400, 700, 200, 200, 100, 400 },
    { "ingenic-t23", "t23dn", 0x9a7b5510, 0xa0000001, 0x60000000, 0x60000031,
      0x50000017, 1400, 700, 200, 200, 100, 600 },
    { "ingenic-t23", "t23zx", 0x9a7b5510, 0xa0000002, 0x60000000, 0x60000031,
      0x50000017, 1400, 700, 200, 200, 100, 400 },
    { "ingenic-t30", "t30n", 0x9a773310, 0xa0000001, 0x60000013, 0x20000012,
      0x50000011 },
    { "ingenic-t30", "t30a", 0x9a7b5510, 0xa0000001, 0x60000017, 0x20000018,
      0x50000017 },
    { "ingenic-t31", "t31l", 0x9a773310, 0xa0000001, 0x60000013, 0x20000014,
      0x50000013, 1008, 504, 250, 250, 125, 500 },
    { "ingenic-t31", "t31n", 0x9a773310, 0xa0000001, 0x60000013, 0x2000001c,
      0x50000013, 1404, 702, 250, 250, 125, 500 },
    { "ingenic-t31", "t31zl", 0x9a773310, 0xa0000001, 0x60000013, 0x2000001c,
      0x50000013, 1404, 702, 250, 250, 125, 500 },
    { "ingenic-t31", "t31x", 0x9a7b5510, 0xa0000001, 0x60000017, 0x2000001c,
      0x50000017, 1392, 696, 200, 200, 100, 600 },
    { "ingenic-t31", "t31zc", 0x9a7b5510, 0xa0000001, 0x60000017, 0x2000001c,
      0x50000017, 1392, 696, 200, 200, 100, 600 },
    { "ingenic-t31", "t31a", 0x9a7b5510, 0xa0000001, 0x6000001d, 0x2000001e,
      0x5000001d, 1512, 756, 250, 250, 125, 750 },
    { "ingenic-t31", "t31lc", 0x9a773310, 0xa0000001, 0x60000013, 0x20000016,
      0x50000013, 1104, 552, 250, 250, 125, 500 },
    { "ingenic-t32", "t32lq", 0x9a7b5510, 0xa0000003, 0x20000000, 0x6000c02f,
      0, 900, 450, 200, 200, 100, 300, 1200 },
    { "ingenic-t32", "t32nq", 0x9a052210, 0xa0000001, 0x20000000, 0x6000c023,
      0, 1200, 600, 300, 300, 150, 450, 900 },
    { "ingenic-t32", "t32vn", 0x9a094410, 0xa0000003, 0x20000000, 0x6000c037,
      0, 900, 450, 280, 280, 140, 350, 1400 },
    { "ingenic-t33", "t33n", 0x9a052210, 0xa0000001, 0, 0, 0,
      950, 475, 300, 300, 150, 450, 900, true },
    { "ingenic-t33", "t33l", 0x9a094410, 0xa0000003, 0, 0, 0,
      950, 475, 260, 260, 130, 325, 1300, true },
    { "ingenic-t33", "t33zl", 0x9a073310, 0xa0000003, 0, 0, 0,
      891, 445, 275, 275, 137, 275, 1100, true },
    { "ingenic-t33", "t33zn", 0x9a094410, 0xa0000001, 0, 0, 0,
      891, 445, 140, 140, 70, 350, 700, true },
    { "ingenic-t31", "t31zx", 0x9a7b5510, 0xa0000001, 0x60000017, 0x2000001c,
      0x50000017, 1392, 696, 200, 200, 100, 600 },
};

static char *make_uimage(void)
{
    uint8_t img[64 + 4096] = { 0 };
    char *path;
    int fd = g_file_open_tmp("ingenic-uboot-clocks-XXXXXX", &path, NULL);

    stl_be_p(img, 0x27051956);
    stl_be_p(img + 12, 4096);
    stl_be_p(img + 16, 0x80010000);
    stl_be_p(img + 20, 0x80010000);
    img[28] = 5;
    img[29] = 5;
    img[30] = 2;
    g_assert(write(fd, img, sizeof(img)) == sizeof(img));
    close(fd);
    return path;
}

/* EXTCLK 24 MHz * M / N / OD1 / OD0, in MHz */
static unsigned pll_mhz(uint32_t reg)
{
    unsigned m = reg >> 20 & 0xfff, n = reg >> 14 & 0x3f;
    unsigned od1 = reg >> 11 & 7, od0 = reg >> 8 & 7;

    g_assert(reg & 1);
    return 24 * m / (n * od1 * od0);
}

static void test_direct(const void *data)
{
    const Case *c = data;
    g_autofree char *kernel = make_uimage();
    QTestState *qts = qtest_initf("-machine %s "
                                  "-global ingenic-t31.soc-variant=%s "
                                  "-kernel %s", c->machine, c->variant,
                                  kernel);
    uint32_t cc = qtest_readl(qts, CPM + CPCCR);

    g_assert_cmphex(cc, ==, c->cpccr);
    g_assert_cmphex(qtest_readl(qts, CPM + DDRCDR), ==, c->ddrcdr);
    if (!c->nodiv) {
        g_assert_cmphex(qtest_readl(qts, CPM + MACCDR), ==, c->maccdr);
        g_assert_cmphex(qtest_readl(qts, CPM + MSC0CDR), ==, c->msc0cdr);
        g_assert_cmphex(qtest_readl(qts, CPM + SSICDR), ==, c->ssicdr);
    }
    if (c->mpll) {
        g_assert_cmpuint(pll_mhz(qtest_readl(qts, CPM + CPMPCR)), ==, c->mpll);
    }
    if (c->cpu) {
        unsigned apll = pll_mhz(qtest_readl(qts, CPM + CPAPCR));
        unsigned mpll = pll_mhz(qtest_readl(qts, CPM + CPMPCR));
        unsigned ddr = mpll / ((qtest_readl(qts, CPM + DDRCDR) & 0xf) + 1);
        unsigned pdiv = (cc >> 16 & 0xf) + 1;

        g_assert_cmpuint(apll / ((cc & 0xf) + 1), ==, c->cpu);
        g_assert_cmpuint(apll / ((cc >> 4 & 0xf) + 1), ==, c->l2);
        /* SEL_H0PLL and SEL_H2PLL 2: the MPLL */
        g_assert_cmpuint(cc >> 26 & 3, ==, 2);
        g_assert_cmpuint(cc >> 24 & 3, ==, 2);
        g_assert_cmpuint(mpll / ((cc >> 8 & 0xf) + 1), ==, c->ahb0);
        g_assert_cmpuint(mpll / ((cc >> 12 & 0xf) + 1), ==, c->ahb2);
        g_assert_cmpuint(mpll / pdiv, ==, c->pclk);
        g_assert_cmpuint(ddr, ==, c->ddr);
    }
    qtest_quit(qts);
    unlink(kernel);
}

/* The XBurst2 parts: the registers U-Boot sets, and what the PLLs decode to */
typedef struct {
    const char *machine;
    const char *prefix;
    const char *variant;
    bool pllreg;            /* the T41's PLL word, not the MNOD form */
    unsigned ddroff;        /* DDRCDR's offset */
    uint32_t cpccr;
    uint32_t regs[16];      /* offset and value, 0 ends */
    unsigned apll, mpll, ahb0, ddr;
} Case2;

static const Case2 cases2[] = {
    { "ingenic-t40", "ingenic-t40", "t40n", false, 0x2c, 0x9a0b5510,
      { 0x2c, 0xa0000001, 0x10, 0x04c0510d, 0x14, 0x07d0590d,
        0x54, 0x60000013, 0x58, 0x010049c0, 0x60, 0x60000013 },
      912, 1000, 166, 500 },
    { "ingenic-t40", "ingenic-t40", "t40nn", false, 0x2c, 0x9a0b5510,
      { 0x68, 0x20108008, 0xe0, 0x010049c0 }, 912, 1000, 166, 500 },
    { "ingenic-t40", "ingenic-t40", "t40xp", false, 0x2c, 0x9a0b5510,
      { 0x2c, 0xa0000001, 0x10, 0x0540510d, 0x14, 0x0640510d,
        0x54, 0x60000017, 0x58, 0x010049c0, 0x60, 0x60000017,
        0x68, 0x20108009 },
      1008, 1200, 200, 600 },
    { "ingenic-t40", "ingenic-t40", "t40a", false, 0x2c, 0x9a0b5510,
      { 0x68, 0x20108009, 0x54, 0x60000017 }, 1008, 1200, 200, 600 },
    { "ingenic-t41", "ingenic-t40", "t41nq", true, 0x2c, 0x9a0b5520,
      { 0x2c, 0xa0000001, 0x10, 0x05b008bd, 0x14, 0x15d0889d,
        0x54, 0x6000001b, 0x60, 0x6000011b, 0x68, 0x2010800a,
        0xe0, 0x059008bd },
      1104, 1400, 233, 700 },
    { "ingenic-t41", "ingenic-t40", "t41lq", true, 0x2c, 0x9a0b5520,
      { 0x2c, 0xa0000001, 0x10, 0x05b008bd, 0x14, 0x063008bd,
        0x54, 0x60000017, 0x60, 0x60000117, 0x68, 0x2010800a },
      1104, 1200, 200, 600 },
    /* thingino builds these with the NQ's and the LQ's U-Boot */
    { "ingenic-t41", "ingenic-t40", "t41zn", true, 0x2c, 0x9a0b5520,
      { 0x14, 0x15d0889d, 0x54, 0x6000001b }, 1104, 1400, 233, 700 },
    { "ingenic-t41", "ingenic-t40", "t41a", true, 0x2c, 0x9a0b5520,
      { 0x10, 0x05b008bd, 0x14, 0x15d0889d, 0x60, 0x6000011b },
      1104, 1400, 233, 700 },
    { "ingenic-t41", "ingenic-t40", "t41zl", true, 0x2c, 0x9a0b5520,
      { 0x14, 0x063008bd, 0x54, 0x60000017 }, 1104, 1200, 200, 600 },
    /* a 2013.07 U-Boot's, on an A1N */
    { "ingenic-a1", "ingenic-a1", "a1n", false, 0x3c, 0x9a7f7520,
      { 0x3c, 0xa0000001, 0x10, 0x05c0510d, 0x14, 0x0860510d,
        0x18, 0x07d0510d, 0x1c, 0x0640510d, 0x90, 0x60000007,
        0x98, 0xe030800e },
      1104, 1608, 268, 804 },
    { "ingenic-a1", "ingenic-a1", "a1x", false, 0x3c, 0x9a7f7520,
      { 0x14, 0x0860510d }, 1104, 1608, 268, 804 },
};

static unsigned pll2_mhz(uint32_t reg, bool pllreg)
{
    unsigned m = reg >> 20 & 0xfff, n = reg >> 14 & 0x3f;

    g_assert(reg & 1);
    if (pllreg) {
        unsigned od0 = reg >> 11 & 7, od1 = reg >> 7 & 15;

        return 24 * 2 * (m + 1) / ((n + 1) * (1u << od0) * (od1 + 1));
    }
    return pll_mhz(reg);
}

static void test_xb2(const void *data)
{
    const Case2 *c = data;
    g_autofree char *kernel = make_uimage();
    QTestState *qts = qtest_initf("-machine %s -global %s.soc-variant=%s "
                                  "-kernel %s", c->machine, c->prefix,
                                  c->variant, kernel);
    uint32_t cc = qtest_readl(qts, CPM + CPCCR);
    unsigned apll = pll2_mhz(qtest_readl(qts, CPM + CPAPCR), c->pllreg);
    unsigned mpll = pll2_mhz(qtest_readl(qts, CPM + CPMPCR), c->pllreg);

    g_assert_cmphex(cc, ==, c->cpccr);
    for (int i = 0; i < ARRAY_SIZE(c->regs) && c->regs[i]; i += 2) {
        g_assert_cmphex(qtest_readl(qts, CPM + c->regs[i]), ==,
                        c->regs[i + 1]);
    }
    g_assert_cmpuint(apll, ==, c->apll);
    g_assert_cmpuint(mpll, ==, c->mpll);
    g_assert_cmpuint(mpll / ((cc >> 8 & 0xf) + 1), ==, c->ahb0);
    g_assert_cmpuint(mpll / ((qtest_readl(qts, CPM + c->ddroff) & 0xf) + 1),
                     ==, c->ddr);
    qtest_quit(qts);
    unlink(kernel);
}

/* What a mainline kernel's clock reads show on silicon, per part */
static void test_silicon_reads(void)
{
    static const struct {
        const char *machine, *variant;
        uint32_t soc_id, vpll_hz_mhz;
    } p[] = {
        { "ingenic-t10", "t10l", 0x10005003, 0 },
        { "ingenic-t21", "t21n", 0x10021003, 1200 },
        { "ingenic-t23", "t23n", 0x10023003, 0 },
        { "ingenic-t30", "t30x", 0x10030005, 1200 },
    };

    for (int i = 0; i < ARRAY_SIZE(p); i++) {
        g_autofree char *kernel = make_uimage();
        QTestState *qts = qtest_initf("-machine %s "
                                      "-global ingenic-t31.soc-variant=%s "
                                      "-kernel %s", p[i].machine,
                                      p[i].variant, kernel);

        g_assert_cmphex(qtest_readl(qts, 0x1300002c), ==, p[i].soc_id);
        if (p[i].vpll_hz_mhz) {
            /* On: U-Boot starts the VPLL; the DFU loader's reads 0x03100860 */
            g_assert_cmpuint(pll2_mhz(qtest_readl(qts, CPM + CPVPCR), true),
                             ==, p[i].vpll_hz_mhz);
        }
        /* The T23 has no divider at 0x60 */
        if (!strcmp(p[i].variant, "t23n")) {
            g_assert_cmphex(qtest_readl(qts, CPM + 0x60), ==, 0);
        }
        /* The T30's EPLL is off, as the DFU loader's read shows */
        if (!strcmp(p[i].variant, "t30x")) {
            g_assert_cmphex(qtest_readl(qts, CPM + 0x58), ==, 0x06305040);
        }
        qtest_quit(qts);
        unlink(kernel);
    }
}

/* A flash boot leaves the CPCCR to the firmware, as the bootrom has it */
static void test_flash(void)
{
    QTestState *qts = qtest_init("-machine ingenic-t31");

    g_assert_cmphex(qtest_readl(qts, CPM + CPCCR), ==, 0x55800000);
    qtest_quit(qts);
}

/* The MAC clock is 50 MHz and the SFC's 50 MHz off the same MPLL */
static void test_t31x_rates(void)
{
    g_autofree char *kernel = make_uimage();
    QTestState *qts = qtest_initf("-machine ingenic-t31 "
                                  "-global ingenic-t31.soc-variant=t31x "
                                  "-kernel %s", kernel);
    unsigned mpll = pll_mhz(qtest_readl(qts, CPM + CPMPCR));
    unsigned vpll = pll_mhz(qtest_readl(qts, CPM + CPVPCR));
    unsigned apll = pll_mhz(qtest_readl(qts, CPM + CPAPCR));
    uint32_t mac = qtest_readl(qts, CPM + MACCDR);
    uint32_t ssi = qtest_readl(qts, CPM + SSICDR);
    uint32_t msc = qtest_readl(qts, CPM + MSC0CDR);

    g_assert_cmpuint(apll, ==, 1392);
    g_assert_cmpuint(mpll, ==, 1200);
    g_assert_cmpuint(vpll, ==, 1200);
    g_assert_cmpuint(mpll / ((mac & 0xff) + 1), ==, 50);
    g_assert_cmpuint(mpll / ((ssi & 0xff) + 1), ==, 50);
    /* the MSC divides its source (SCLK_A) by twice its field plus one */
    g_assert_cmpuint(apll / (2 * ((msc & 0xff) + 1)), ==, 24);
    qtest_quit(qts);
    unlink(kernel);
}

/*
 * T32 Pro and CW080: the PRJ009 U-Boot's PLLs (the MPLL's dividers stored
 * less one), CPCCR and the divider rates the kernel's clock tree reads
 */
static void test_pro(const void *data)
{
    const char *variant = data;
    g_autofree char *kernel = make_uimage();
    QTestState *qts = qtest_initf("-machine ingenic-t32 "
                                  "-global ingenic-t31.soc-variant=%s "
                                  "-kernel %s", variant, kernel);
    uint32_t cc = qtest_readl(qts, CPM + CPCCR);
    uint32_t mp = qtest_readl(qts, CPM + CPMPCR);
    uint32_t vp = qtest_readl(qts, CPM + CPVPCR);
    unsigned apll = pll_mhz(qtest_readl(qts, CPM + CPAPCR));
    unsigned mpll = 24 * (mp >> 20 & 0xfff) /
                    ((mp >> 14 & 0x3f) * ((mp >> 11 & 7) + 1) *
                     ((mp >> 8 & 7) + 1));

    g_assert_cmphex(cc, ==, 0x9a0b5510);
    g_assert_cmpuint(apll, ==, 1200);
    g_assert_cmpuint(mpll, ==, 1066);
    g_assert_cmpuint(pll_mhz(vp), ==, 1080);
    g_assert_cmpuint(apll / ((cc & 0xf) + 1), ==, 1200);
    g_assert_cmpuint(mpll / ((cc >> 8 & 0xf) + 1), <=, 200);
    g_assert_cmpuint(mpll / ((cc >> 12 & 0xf) + 1), <=, 200);
    g_assert_cmpuint(mpll / ((cc >> 16 & 0xf) + 1), <=, 100);
    g_assert_cmphex(qtest_readl(qts, CPM + DDRCDR), ==, 0xa0000001);
    g_assert_cmphex(qtest_readl(qts, CPM + MSC0CDR), ==, 0x6000c02a);
    qtest_quit(qts);
    unlink(kernel);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *name = g_strdup_printf("ingenic-uboot-clocks/%s",
                                                cases[i].variant);

        qtest_add_data_func(name, &cases[i], test_direct);
    }
    for (int i = 0; i < ARRAY_SIZE(cases2); i++) {
        g_autofree char *name = g_strdup_printf("ingenic-uboot-clocks/%s",
                                                cases2[i].variant);

        qtest_add_data_func(name, &cases2[i], test_xb2);
    }
    qtest_add_data_func("ingenic-uboot-clocks/t32provn", "t32provn", test_pro);
    qtest_add_data_func("ingenic-uboot-clocks/t32prozl", "t32prozl", test_pro);
    qtest_add_data_func("ingenic-uboot-clocks/cw080snp", "cw080snp", test_pro);
    qtest_add_func("ingenic-uboot-clocks/silicon-reads", test_silicon_reads);
    qtest_add_func("ingenic-uboot-clocks/flash-boot", test_flash);
    qtest_add_func("ingenic-uboot-clocks/t31x-rates", test_t31x_rates);
    return g_test_run();
}
