/*
 * ATBM6441 "Hera" Wi-Fi block stand-in: MAC (PAC), PHY and their friends
 *
 * These blocks have no documentation and are out of scope for the machine
 * (the plan is a shim at the WSM boundary), but the closed LMAC code in the
 * SDK's libwifi polls them during its init and the console only comes up
 * when it gets what it waits for. This device is a register file that
 * reads back what was written, plus the few reads that are known to
 * need an answer. Each offset's first access is logged (LOG_UNIMP), so
 * what the firmware waits on is on record.
 *
 * What is answered, each found by watching the firmware stall:
 *   PAC   0x0a500a20 bit 31 set: PAS_Init() writes its tables and then
 *         spins on this bit (the PAC sequencer is up)
 *   PHY   0x0acc017c reads 0x64: the chip version BSP_Init() stores
 *   PHY   0x0ac380f0 bit 4 set: PHY_RF_ComputeIQOffset() polls it (an RX
 *         calibration step is done)
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_ATBM_HERA_WIFI_STUB "atbm-hera-wifi-stub"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraWifiStubState, ATBM_HERA_WIFI_STUB)

#define MAX_WORDS       (0x100000 / 4)

typedef struct {
    uint32_t addr;
    uint32_t set;       /* bits forced to 1 */
    uint32_t clear;     /* bits forced to 0 */
} StubRead;

static const StubRead stub_reads[] = {
    { 0x0a500a20, 0x80000000, 0 },
    { 0x0acc017c, 0x00000064, 0xffffffff },
    { 0x0ac380f0, 0x00000010, 0 },
};

struct AtbmHeraWifiStubState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t base;
    uint32_t size;
    uint32_t *regs;
    unsigned long *logged;
};

static void stub_log(AtbmHeraWifiStubState *s, hwaddr addr, bool is_write,
                     uint64_t val)
{
    unsigned long *w = &s->logged[BIT_WORD(addr / 4)];
    unsigned long bit = BIT_MASK(addr / 4);

    if (*w & bit) {
        return;
    }
    *w |= bit;
    qemu_log_mask(LOG_UNIMP, "atbm-hera-wifi-stub: %s at 0x%08" PRIx64
                  "%s\n", is_write ? "write" : "read", s->base + addr,
                  is_write ? "" : " (reads back what was written)");
}

static uint64_t stub_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraWifiStubState *s = opaque;
    uint32_t v = s->regs[addr / 4];

    stub_log(s, addr, false, 0);
    for (int i = 0; i < ARRAY_SIZE(stub_reads); i++) {
        if (stub_reads[i].addr == s->base + (addr & ~3)) {
            v = (v & ~stub_reads[i].clear) | stub_reads[i].set;
        }
    }
    return v >> ((addr & 3) * 8);
}

static void stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraWifiStubState *s = opaque;
    int sh = (addr & 3) * 8;
    uint32_t mask = (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1) << sh;

    stub_log(s, addr, true, val);
    s->regs[addr / 4] = (s->regs[addr / 4] & ~mask) | ((val << sh) & mask);
}

static const MemoryRegionOps stub_ops = {
    .read = stub_read,
    .write = stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void stub_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraWifiStubState *s = ATBM_HERA_WIFI_STUB(dev);

    if (!s->size || s->size > MAX_WORDS * 4) {
        error_setg(errp, "size must be 1 to 0x100000");
        return;
    }
    s->regs = g_new0(uint32_t, s->size / 4);
    s->logged = bitmap_new(s->size / 4);
    memory_region_init_io(&s->iomem, OBJECT(dev), &stub_ops, s,
                          "atbm-hera-wifi-stub", s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property stub_props[] = {
    DEFINE_PROP_UINT32("base", AtbmHeraWifiStubState, base, 0),
    DEFINE_PROP_UINT32("size", AtbmHeraWifiStubState, size, 0x20000),
};

static void stub_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stub_realize;
    device_class_set_props(dc, stub_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo stub_info = {
    .name = TYPE_ATBM_HERA_WIFI_STUB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraWifiStubState),
    .class_init = stub_class_init,
};

static void stub_register_types(void)
{
    type_register_static(&stub_info);
}

type_init(stub_register_types)
