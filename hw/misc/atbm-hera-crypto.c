/*
 * ATBM6441 "Hera" cipher and hash engine
 *
 * The block at 0x17200000 that the SDK's libdev hands AES (cipher.o) and
 * SHA-1/SHA-256 block processing (atbm_hash.o) to. The Wi-Fi supplicant's
 * key handling runs on it: PBKDF2 for the PMK, the PRF for the PTK, the
 * EAPOL MICs, the AES key unwrap of the group key; mbedTLS's AES and SHA
 * as well. Undocumented beyond the SDK's cipher.h (the AES register
 * addresses and the driver's arguments); the rest is the drivers' use.
 *
 * Cipher (cipher.h: ATBM_set_hardware):
 *   0x00 CTRL      bit 0 starts; algorithm (DES, 3DES128, 3DES192,
 *                  AES128, AES192, AES256) in bits 6:4, mode (ECB, CBC,
 *                  CFB, OFB, CTR) in bits 3:1, CFB width in bits 10:8,
 *                  bit 7 encrypts; the driver also sets bits 12:11
 *   0x04 SIZE      bytes, bits 19:0
 *   0x08 IV        0x0c KEY, 0x10 INPUT, 0x14 OUTPUT: buffer addresses
 *   0x134 DONE     bit 0, which the driver polls; it writes 0 to CTRL after
 * Hash (atbm_hash.o):
 *   0x18 CTRL      bit 0 starts; 0x751 SHA-1, 0x753 SHA-256, 0x655 MD5
 *                  (bit 1 picks SHA-256). The driver then writes 0x140,
 *                  0x142 or 0x44 (bit 0 clear)
 *   0x1c LEN       bytes to process, whole 64-byte blocks
 *   0x20 SRC       the data
 *   0x24 STATE_IN  the chaining state, as the CPU keeps it: 32-bit words
 *                  in its own (little endian) order
 *   0x28 STATUS    bit 0: done, which the driver polls after a start
 *   0x2c STATE_OUT where the new state goes (the driver passes the same
 *                  address as STATE_IN)
 *
 * AES runs ECB and CBC (what the supplicant and mbedTLS use); DES, the
 * other modes and MD5 (the driver does MD5 in software) are logged, not
 * run. Keys, IVs and data are byte strings in memory. Both engines finish
 * at once.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "crypto/aes.h"
#include "trace.h"

#define TYPE_ATBM_HERA_CRYPTO "atbm-hera-crypto"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraCryptoState, ATBM_HERA_CRYPTO)

#define R_CCTRL         (0x00 / 4)
#define R_CSIZE         (0x04 / 4)
#define R_CIV           (0x08 / 4)
#define R_CKEY          (0x0c / 4)
#define R_CIN           (0x10 / 4)
#define R_COUT          (0x14 / 4)
#define R_CDONE         (0x134 / 4)
#define R_CTRL          (0x18 / 4)
#define R_LEN           (0x1c / 4)
#define R_SRC           (0x20 / 4)
#define R_STATE_IN      (0x24 / 4)
#define R_STATUS        (0x28 / 4)
#define R_STATE_OUT     (0x2c / 4)
#define NREGS           (0x140 / 4)

#define CTRL_START      BIT(0)
#define CTRL_SHA256     BIT(1)
#define CTRL_ALG_MASK   0x700
#define CTRL_ALG_MD5    0x600
#define STATUS_DONE     BIT(0)

#define CCTRL_START     BIT(0)
#define CCTRL_MODE(v)   extract32(v, 1, 3)
#define CCTRL_ENCRYPT   BIT(7)
#define CCTRL_ALG(v)    extract32(v, 4, 3)
#define CCTRL_SIZE_MASK 0xfffff
#define MODE_ECB        0
#define MODE_CBC        1
#define ALG_AES128      3

/* one process call: the driver gives a block, its finish a few */
#define LEN_MAX         4096
/* the largest cipher job taken; mbedTLS gives up to a TLS record */
#define CSIZE_MAX       (64 * 1024)

struct AtbmHeraCryptoState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[NREGS];
};

static void sha1_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, f, k, t;

    for (int i = 0; i < 16; i++) {
        w[i] = ldl_be_p(p + 4 * i);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol32(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64], v[8], t1, t2;

    for (int i = 0; i < 16; i++) {
        w[i] = ldl_be_p(p + 4 * i);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof(v));
    for (int i = 0; i < 64; i++) {
        t1 = v[7] + (ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25)) +
             ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha256_k[i] + w[i];
        t2 = (ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22)) +
             ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(&v[1], &v[0], 7 * sizeof(v[0]));
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) {
        h[i] += v[i];
    }
}

static void atbm_hera_crypto_hash(AtbmHeraCryptoState *s, uint32_t ctrl)
{
    bool sha256 = ctrl & CTRL_SHA256;
    unsigned words = sha256 ? 8 : 5;
    uint32_t len = s->regs[R_LEN];
    uint8_t state[32], block[64];
    uint32_t h[8];
    MemTxResult r;

    trace_atbm_hera_hash_run(ctrl, s->regs[R_SRC], len);
    if ((ctrl & CTRL_ALG_MASK) == CTRL_ALG_MD5) {
        qemu_log_mask(LOG_UNIMP, "atbm-hera-crypto: MD5 not modelled\n");
        return;
    }
    if (len % 64 || len > LEN_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-crypto: length %u\n", len);
        return;
    }
    r = address_space_read(&address_space_memory, s->regs[R_STATE_IN],
                           MEMTXATTRS_UNSPECIFIED, state, 4 * words);
    for (unsigned i = 0; i < words; i++) {
        h[i] = ldl_le_p(state + 4 * i);
    }
    for (uint32_t off = 0; off < len && r == MEMTX_OK; off += 64) {
        r = address_space_read(&address_space_memory, s->regs[R_SRC] + off,
                               MEMTXATTRS_UNSPECIFIED, block, sizeof(block));
        if (sha256) {
            sha256_block(h, block);
        } else {
            sha1_block(h, block);
        }
    }
    if (r != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-crypto: bad address\n");
        return;
    }
    for (unsigned i = 0; i < words; i++) {
        stl_le_p(state + 4 * i, h[i]);
    }
    address_space_write(&address_space_memory, s->regs[R_STATE_OUT],
                        MEMTXATTRS_UNSPECIFIED, state, 4 * words);
}

static void atbm_hera_crypto_cipher(AtbmHeraCryptoState *s, uint32_t ctrl)
{
    unsigned alg = CCTRL_ALG(ctrl), mode = CCTRL_MODE(ctrl);
    bool enc = ctrl & CCTRL_ENCRYPT;
    uint32_t size = s->regs[R_CSIZE] & CCTRL_SIZE_MASK;
    unsigned bits = 128 + 64 * (alg - ALG_AES128);
    g_autofree uint8_t *buf = NULL;
    uint8_t key[32], iv[16], prev[16];
    AES_KEY k;

    trace_atbm_hera_cipher_run(ctrl, s->regs[R_CIN], s->regs[R_COUT], size);
    if (alg < ALG_AES128 || alg > ALG_AES128 + 2 ||
        (mode != MODE_ECB && mode != MODE_CBC)) {
        qemu_log_mask(LOG_UNIMP, "atbm-hera-crypto: algorithm %u mode %u "
                      "not modelled\n", alg, mode);
        return;
    }
    if (size % 16 || size > CSIZE_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-crypto: size %u\n", size);
        return;
    }
    buf = g_malloc(size);
    /* ECB callers pass no IV (0) */
    memset(iv, 0, sizeof(iv));
    if (address_space_read(&address_space_memory, s->regs[R_CKEY],
                           MEMTXATTRS_UNSPECIFIED, key, bits / 8) ||
        (mode == MODE_CBC &&
         address_space_read(&address_space_memory, s->regs[R_CIV],
                            MEMTXATTRS_UNSPECIFIED, iv, sizeof(iv))) ||
        address_space_read(&address_space_memory, s->regs[R_CIN],
                           MEMTXATTRS_UNSPECIFIED, buf, size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-crypto: bad address\n");
        return;
    }
    if (enc) {
        AES_set_encrypt_key(key, bits, &k);
    } else {
        AES_set_decrypt_key(key, bits, &k);
    }
    for (uint32_t off = 0; off < size; off += 16) {
        uint8_t *b = buf + off;

        if (enc) {
            if (mode == MODE_CBC) {
                for (int i = 0; i < 16; i++) {
                    b[i] ^= iv[i];
                }
            }
            AES_encrypt(b, b, &k);
            memcpy(iv, b, 16);
        } else {
            memcpy(prev, b, 16);
            AES_decrypt(b, b, &k);
            if (mode == MODE_CBC) {
                for (int i = 0; i < 16; i++) {
                    b[i] ^= iv[i];
                }
            }
            memcpy(iv, prev, 16);
        }
    }
    address_space_write(&address_space_memory, s->regs[R_COUT],
                        MEMTXATTRS_UNSPECIFIED, buf, size);
}

static uint64_t atbm_hera_crypto_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraCryptoState *s = opaque;

    return s->regs[addr / 4];
}

static void atbm_hera_crypto_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    AtbmHeraCryptoState *s = opaque;
    unsigned r = addr / 4;

    if (r == R_STATUS || r == R_CDONE) {
        return;
    }
    s->regs[r] = val;
    if (r == R_CCTRL && (val & CCTRL_START)) {
        atbm_hera_crypto_cipher(s, val);
        s->regs[R_CDONE] |= STATUS_DONE;
    }
    if (r == R_CTRL && (val & CTRL_START)) {
        atbm_hera_crypto_hash(s, val);
        s->regs[R_STATUS] |= STATUS_DONE;
    }
}

static const MemoryRegionOps atbm_hera_crypto_ops = {
    .read = atbm_hera_crypto_read,
    .write = atbm_hera_crypto_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void atbm_hera_crypto_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraCryptoState *s = ATBM_HERA_CRYPTO(obj);

    memset(s->regs, 0, sizeof(s->regs));
    /* idle reads as done: the driver only polls after a start */
    s->regs[R_STATUS] = STATUS_DONE;
    s->regs[R_CDONE] = STATUS_DONE;
}

static void atbm_hera_crypto_init(Object *obj)
{
    AtbmHeraCryptoState *s = ATBM_HERA_CRYPTO(obj);

    memory_region_init_io(&s->iomem, obj, &atbm_hera_crypto_ops, s,
                          TYPE_ATBM_HERA_CRYPTO, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_atbm_hera_crypto = {
    .name = TYPE_ATBM_HERA_CRYPTO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AtbmHeraCryptoState, NREGS),
        VMSTATE_END_OF_LIST()
    },
};

static void atbm_hera_crypto_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "ATBM6441 cipher and hash engine";
    dc->vmsd = &vmstate_atbm_hera_crypto;
    rc->phases.hold = atbm_hera_crypto_reset_hold;
}

static const TypeInfo atbm_hera_crypto_info = {
    .name = TYPE_ATBM_HERA_CRYPTO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraCryptoState),
    .instance_init = atbm_hera_crypto_init,
    .class_init = atbm_hera_crypto_class_init,
};

static void atbm_hera_crypto_register_types(void)
{
    type_register_static(&atbm_hera_crypto_info);
}

type_init(atbm_hera_crypto_register_types)
