/*
 * QTest testcase for the ATBM6441 "Hera" cipher and hash engine
 *
 * One padded block of "abc" through SHA-1 and SHA-256 the way the SDK's
 * driver runs it (state in the CPU's word order, the start value, the done
 * bit), and AES-128 ECB and CBC both ways, against the standards' vectors.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "atbm-hera-test.h"

#define HASH            0x17200000
#define H_CTRL          (HASH + 0x18)
#define H_LEN           (HASH + 0x1c)
#define H_SRC           (HASH + 0x20)
#define H_STATE_IN      (HASH + 0x24)
#define H_STATUS        (HASH + 0x28)
#define H_STATE_OUT     (HASH + 0x2c)

#define C_CTRL          (HASH + 0x00)
#define C_SIZE          (HASH + 0x04)
#define C_IV            (HASH + 0x08)
#define C_KEY           (HASH + 0x0c)
#define C_IN            (HASH + 0x10)
#define C_OUT           (HASH + 0x14)
#define C_DONE          (HASH + 0x134)

/* ATBM_set_hardware's control word: AES128 (3) << 4, mode << 1, start */
#define C_AES128(mode, enc) (0x1801 | 3 << 4 | (mode) << 1 | ((enc) ? 0x80 : 0))

#define BLOCK           (HERA_SRAM + 0x100)
#define KEY             (HERA_SRAM + 0x300)
#define IV              (HERA_SRAM + 0x340)
#define OUT             (HERA_SRAM + 0x380)
#define STATE           (HERA_SRAM + 0x200)

static void run(QTestState *qts, uint32_t start, uint32_t clear,
                const uint32_t *iv, unsigned words)
{
    uint8_t block[64] = { 'a', 'b', 'c', 0x80 };

    block[63] = 24;     /* the message length in bits */
    qtest_memwrite(qts, BLOCK, block, sizeof(block));
    for (unsigned i = 0; i < words; i++) {
        qtest_writel(qts, STATE + 4 * i, iv[i]);
    }
    qtest_writel(qts, H_SRC, BLOCK);
    qtest_writel(qts, H_LEN, 64);
    qtest_writel(qts, H_STATE_OUT, STATE);
    qtest_writel(qts, H_STATE_IN, STATE);
    qtest_writel(qts, H_CTRL, start);
    g_assert_cmphex(qtest_readl(qts, H_STATUS) & 1, ==, 1);
    qtest_writel(qts, H_CTRL, clear);
}

static void test_sha1(void)
{
    static const uint32_t iv[5] = {
        0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0,
    };
    static const uint32_t digest[5] = {
        0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d,
    };
    QTestState *qts = hera_init(NULL);

    run(qts, 0x751, 0x140, iv, 5);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmphex(qtest_readl(qts, STATE + 4 * i), ==, digest[i]);
    }
    qtest_quit(qts);
}

static void test_sha256(void)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    static const uint32_t digest[8] = {
        0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
        0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad,
    };
    QTestState *qts = hera_init(NULL);

    run(qts, 0x753, 0x142, iv, 8);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(qts, STATE + 4 * i), ==, digest[i]);
    }
    qtest_quit(qts);
}

static void cipher(QTestState *qts, uint32_t ctrl, const uint8_t *in,
                   uint8_t *out, size_t len)
{
    qtest_memwrite(qts, BLOCK, in, len);
    qtest_writel(qts, C_KEY, KEY);
    /* as the SDK's ECB callers do, no IV unless chaining */
    qtest_writel(qts, C_IV, ctrl & 0xe ? IV : 0);
    qtest_writel(qts, C_IN, BLOCK);
    qtest_writel(qts, C_OUT, OUT);
    qtest_writel(qts, C_SIZE, len);
    qtest_writel(qts, C_CTRL, ctrl);
    g_assert_cmphex(qtest_readl(qts, C_DONE) & 1, ==, 1);
    qtest_writel(qts, C_CTRL, 0);
    qtest_memread(qts, OUT, out, len);
}

/* FIPS-197 appendix C.1, then CBC of two blocks back again */
static void test_aes(void)
{
    static const uint8_t pt[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    static const uint8_t ct[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
    };
    uint8_t key[16], iv[16] = {}, out[32], two[32], back[32];
    QTestState *qts = hera_init(NULL);

    for (int i = 0; i < 16; i++) {
        key[i] = i;
    }
    qtest_memwrite(qts, KEY, key, sizeof(key));
    qtest_memwrite(qts, IV, iv, sizeof(iv));

    cipher(qts, C_AES128(0, true), pt, out, 16);
    g_assert_cmpmem(out, 16, ct, 16);
    cipher(qts, C_AES128(0, false), ct, out, 16);
    g_assert_cmpmem(out, 16, pt, 16);

    /* CBC with a zero IV: the first block is ECB's, the second chains */
    memcpy(two, pt, 16);
    memcpy(two + 16, pt, 16);
    cipher(qts, C_AES128(1, true), two, out, 32);
    g_assert_cmpmem(out, 16, ct, 16);
    g_assert_true(memcmp(out + 16, ct, 16) != 0);
    cipher(qts, C_AES128(1, false), out, back, 32);
    g_assert_cmpmem(back, 32, two, 32);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/atbm-hera/hash/sha1", test_sha1);
    qtest_add_func("/atbm-hera/hash/sha256", test_sha256);
    qtest_add_func("/atbm-hera/cipher/aes", test_aes);

    return g_test_run();
}
