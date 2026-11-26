/*
 * ATBM6441 "Hera" eFuse: 1024 bits of OTP at 0x16b00000
 *
 * The chip id, the calibration trims and the MAC address live in it. The
 * firmware reaches it through three registers (the SDK's
 * atbmwifi_efuse_read_byte and atbmwifi_efuse_write_bit):
 *   0x00 CMD    a read: the byte number in bits 30:8. A write: the byte
 *               number in bits 14:8 and the bit in 17:15
 *   0x04 DATA   a read: the byte CMD names (0 past the 128 bytes). A
 *               write of 1: burn the bit CMD names, if 0x08 has been
 *               written 0xef555555 since the last burn
 *   0x08 KEY    the burn's unlock, written twice before it
 *   0x28 .. 0x3c  the unlock sequence of the secure key area: kept
 * A burn only sets bits: an OTP bit is never cleared. The content is the
 * "drive" (a raw image of 128 bytes, written back as bits are burnt), or all
 * zeros without one.
 *
 * Not modelled: the burn's time, the secure key area's unlock (reads of
 * it return what the plain bytes hold), the charge pump's supply.
 * Assumptions: the burn wants the key once per burn, and a bit in a
 * burn's CMD above the byte number is the bit's place, as the SDK builds
 * it; nothing is known of what DATA reads while a burn runs.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"

#define TYPE_ATBM_HERA_EFUSE "atbm-hera-efuse"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraEfuseState, ATBM_HERA_EFUSE)

#define EF_CMD          0x00
#define EF_DATA         0x04
#define EF_KEY          0x08
#define EF_SECURE       0x28
#define EF_SECURE_END   0x40   /* six words */
#define EF_SIZE         0x100
#define EF_BYTES        128
#define EF_BURN_KEY     0xef555555

struct AtbmHeraEfuseState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    BlockBackend *blk;

    uint32_t cmd;
    uint32_t key;
    bool key_ok;
    uint32_t secure[(EF_SECURE_END - EF_SECURE) / 4];
    uint8_t data[EF_BYTES];
};

static void efuse_sync(AtbmHeraEfuseState *s, unsigned byte)
{
    if (s->blk && blk_pwrite(s->blk, byte, 1, &s->data[byte], 0) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "atbm-hera-efuse: write-back failed at byte %u\n", byte);
    }
}

static uint64_t efuse_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraEfuseState *s = opaque;
    uint32_t idx = (s->cmd >> 8) & 0x7fffff;

    switch (addr) {
    case EF_CMD:
        return s->cmd;
    case EF_DATA:
        return idx < EF_BYTES ? s->data[idx] : 0;
    case EF_KEY:
        return s->key;
    case EF_SECURE ... EF_SECURE_END - 1:
        return s->secure[(addr - EF_SECURE) / 4];
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-efuse: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void efuse_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    AtbmHeraEfuseState *s = opaque;

    switch (addr) {
    case EF_CMD:
        s->cmd = val;
        break;
    case EF_KEY:
        s->key = val;
        s->key_ok = val == EF_BURN_KEY;
        break;
    case EF_DATA:
        if (val & 1) {
            unsigned byte = extract32(s->cmd, 8, 7);
            unsigned bit = extract32(s->cmd, 15, 3);

            if (!s->key_ok) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "atbm-hera-efuse: burn without the key\n");
                break;
            }
            s->key_ok = false;
            s->data[byte] |= BIT(bit);
            efuse_sync(s, byte);
        }
        break;
    case EF_SECURE ... EF_SECURE_END - 1:
        s->secure[(addr - EF_SECURE) / 4] = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-efuse: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps efuse_ops = {
    .read = efuse_read,
    .write = efuse_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void efuse_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraEfuseState *s = ATBM_HERA_EFUSE(obj);

    s->cmd = 0;
    s->key = 0;
    s->key_ok = false;
    memset(s->secure, 0, sizeof(s->secure));
}

static void efuse_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraEfuseState *s = ATBM_HERA_EFUSE(dev);

    if (s->blk) {
        int64_t len = blk_getlength(s->blk);

        /* the block layer rounds a file up to a 512-byte sector */
        if (len < EF_BYTES) {
            error_setg(errp, "the eFuse image must be %d bytes, not %" PRId64,
                       EF_BYTES, len);
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (blk_pread(s->blk, 0, EF_BYTES, s->data, 0) < 0) {
            error_setg(errp, "cannot read the eFuse image");
            return;
        }
    }
    memory_region_init_io(&s->iomem, OBJECT(dev), &efuse_ops, s,
                          TYPE_ATBM_HERA_EFUSE, EF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_efuse = {
    .name = TYPE_ATBM_HERA_EFUSE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmd, AtbmHeraEfuseState),
        VMSTATE_UINT32(key, AtbmHeraEfuseState),
        VMSTATE_BOOL(key_ok, AtbmHeraEfuseState),
        VMSTATE_UINT32_ARRAY(secure, AtbmHeraEfuseState, 6),
        VMSTATE_UINT8_ARRAY(data, AtbmHeraEfuseState, EF_BYTES),
        VMSTATE_END_OF_LIST()
    }
};

static const Property efuse_props[] = {
    DEFINE_PROP_DRIVE("drive", AtbmHeraEfuseState, blk),
};

static void efuse_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = efuse_realize;
    dc->vmsd = &vmstate_efuse;
    device_class_set_props(dc, efuse_props);
    rc->phases.hold = efuse_reset_hold;
}

static const TypeInfo efuse_info = {
    .name = TYPE_ATBM_HERA_EFUSE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraEfuseState),
    .class_init = efuse_class_init,
};

static void efuse_register_types(void)
{
    type_register_static(&efuse_info);
}

type_init(efuse_register_types)
