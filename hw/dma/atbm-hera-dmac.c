/*
 * ATBM6441 "Hera" DMA controller: an Andes ATCDMAC100 at 0x14000000
 *
 * Eight channels (the SDK uses seven: dmad.h's HMAC, flash, SPI3 out, SPI2
 * in and out, I2S TX and RX). The registers, as dmad.c uses them:
 *   0x10 CFG        0x20 CTRL (kept)
 *   0x30 INTSTATUS  [7:0] error, [15:8] abort, [23:16] transfer complete,
 *                   write 1 to clear
 *   0x34 CHEN       the enabled channels, read-only
 *   0x40 CHABORT    write 1 to abort a channel
 *   0x44 + 0x14 * n the channel: CTRL, SRC, DST, SIZE, LLP
 * A channel's CTRL: [0] enable, [1] mask the complete interrupt, [2] mask
 * error, [3] mask abort, [7:4] and [11:8] the destination and source
 * request numbers, [13:12] and [15:14] the destination and source address
 * step (0 up, 1 down, 2 fixed), [16] and [17] destination and source
 * handshake, [19:18] and [21:20] the widths (0 8 bits, 1 16, 2 32),
 * [24:22] burst size. SIZE counts units of the source width. When LLP
 * is not 0 the next descriptor (CTRL, SRC, DST, SIZE, LLP) is read from
 * there when a transfer completes, as the controller's chaining does.
 *
 * A channel moves its units as soon as it is enabled; a handshake side
 * waits for its request line, one unit at a time, and the peripherals
 * (the SPIs through "dreq-tx" and "dreq-rx") drive the lines: the DMA
 * request numbers are the data sheet's (dmad.h's DMA_REQ_SEL: UART1 TX 0,
 * RX 1, UART2 2, 3, SPI2 TX 4, RX 5, SPI1 TX 6, RX 7, SPI3 TX 8, RX 9, SSP
 * 10..13, I2C 14). A request line nothing has driven reads as asserted:
 * the peripheral is always ready. The interrupt is chip number 7.
 *
 * Assumptions: the interrupt masks keep the status bit from being set;
 * the burst size has no effect on what is moved; a transfer whose source
 * and destination widths differ splits or gathers items (the SDK's flash
 * write programs a byte-wide SPI port from 32-bit words and counts words).
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

#define TYPE_ATBM_HERA_DMAC "atbm-hera-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraDmacState, ATBM_HERA_DMAC)

#define D_IDREV         0x00
#define D_CFG           0x10
#define D_CTRL          0x20
#define D_INTST         0x30
#define D_CHEN          0x34
#define D_CHABORT       0x40
#define D_CH_BASE       0x44
#define D_CH_STRIDE     0x14
#define DMAC_SIZE       0x100

#define NCH             8
#define NREQ            16
#define CFG_VALUE       0x80000008

#define C_EN            BIT(0)
#define C_MASK_TC       BIT(1)
#define C_MASK_ERR      BIT(2)
#define C_MASK_ABT      BIT(3)

#define ST_ERR(n)       BIT(n)
#define ST_ABT(n)       BIT(8 + (n))
#define ST_TC(n)        BIT(16 + (n))

typedef struct DmacChannel {
    uint32_t ctrl;
    uint32_t src;
    uint32_t dst;
    uint32_t size;
    uint32_t llp;
    uint32_t remaining;
} DmacChannel;

struct AtbmHeraDmacState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t ctrl;
    uint32_t intst;
    uint32_t chen;
    DmacChannel ch[NCH];

    uint32_t req_level;
    uint32_t req_driven;
    bool running;
    bool again;
};

static void dmac_update_irq(AtbmHeraDmacState *s)
{
    qemu_set_irq(s->irq, !!s->intst);
}

static bool dmac_req(AtbmHeraDmacState *s, uint32_t n)
{
    return n >= NREQ || !(s->req_driven & BIT(n)) || (s->req_level & BIT(n));
}

static void dmac_stop(AtbmHeraDmacState *s, int n)
{
    s->ch[n].ctrl &= ~C_EN;
    s->chen &= ~BIT(n);
}

static void dmac_status(AtbmHeraDmacState *s, int n, uint32_t bit,
                        uint32_t mask)
{
    if (!(s->ch[n].ctrl & mask)) {
        s->intst |= bit;
    }
}

static int dmac_step(uint32_t ctrl, uint32_t addr, int w)
{
    switch (extract32(ctrl, 0, 2)) {
    case 1:
        return -w;
    case 2:
        return 0;
    default:
        return w;
    }
}

/*
 * One unit: read a source item, write the destination. SIZE counts source
 * items: a wider source item goes out as several destination writes, low
 * byte first, and a wider destination gathers several source items. The
 * number of source items used is returned, 0 on a bus error.
 */
static uint32_t dmac_unit(AtbmHeraDmacState *s, int n)
{
    DmacChannel *c = &s->ch[n];
    int sw = 1 << extract32(c->ctrl, 20, 2);
    int dw = 1 << extract32(c->ctrl, 18, 2);
    uint8_t buf[4];
    uint32_t used = MAX(dw / sw, 1);

    used = MIN(used, c->remaining);
    for (uint32_t i = 0; i < used; i++) {
        if (address_space_read(&address_space_memory, c->src,
                               MEMTXATTRS_UNSPECIFIED, buf + i * sw, sw) !=
            MEMTX_OK) {
            return 0;
        }
        c->src += dmac_step(c->ctrl >> 14, c->src, sw);
    }
    for (int off = 0; off < MAX(sw, dw); off += dw) {
        if (address_space_write(&address_space_memory, c->dst,
                                MEMTXATTRS_UNSPECIFIED, buf + off, dw) !=
            MEMTX_OK) {
            return 0;
        }
        c->dst += dmac_step(c->ctrl >> 12, c->dst, dw);
    }
    return used;
}

/* A transfer is complete: its interrupt, then the next descriptor */
static void dmac_complete(AtbmHeraDmacState *s, int n)
{
    DmacChannel *c = &s->ch[n];

    dmac_status(s, n, ST_TC(n), C_MASK_TC);
    if (c->llp & ~3u) {
        uint32_t d[5];

        if (address_space_read(&address_space_memory, c->llp & ~3u,
                               MEMTXATTRS_UNSPECIFIED, d, sizeof(d)) !=
            MEMTX_OK) {
            dmac_status(s, n, ST_ERR(n), C_MASK_ERR);
            dmac_stop(s, n);
            return;
        }
        c->ctrl = le32_to_cpu(d[0]) | C_EN;
        c->src = le32_to_cpu(d[1]);
        c->dst = le32_to_cpu(d[2]);
        c->size = le32_to_cpu(d[3]) & 0x3fffff;
        c->llp = le32_to_cpu(d[4]);
        c->remaining = c->size;
    } else {
        dmac_stop(s, n);
    }
}

static void dmac_run(AtbmHeraDmacState *s)
{
    if (s->running) {
        s->again = true;
        return;
    }
    s->running = true;
    do {
        s->again = false;
        for (int n = 0; n < NCH; n++) {
            DmacChannel *c = &s->ch[n];

            for (;;) {
                while ((c->ctrl & C_EN) && c->remaining) {
                    bool src_hs = c->ctrl & BIT(17);
                    bool dst_hs = c->ctrl & BIT(16);

                    if ((src_hs && !dmac_req(s, extract32(c->ctrl, 8, 4))) ||
                        (dst_hs && !dmac_req(s, extract32(c->ctrl, 4, 4)))) {
                        break;
                    }
                    uint32_t used = dmac_unit(s, n);

                    if (!used) {
                        dmac_status(s, n, ST_ERR(n), C_MASK_ERR);
                        dmac_stop(s, n);
                        break;
                    }
                    c->remaining -= used;
                }
                if ((c->ctrl & C_EN) && !c->remaining) {
                    /* a chained descriptor goes round again */
                    dmac_complete(s, n);
                    if (c->ctrl & C_EN) {
                        continue;
                    }
                }
                break;
            }
        }
    } while (s->again);
    s->running = false;
    dmac_update_irq(s);
}

static void dmac_req_in(void *opaque, int n, int level)
{
    AtbmHeraDmacState *s = opaque;

    s->req_driven |= BIT(n);
    s->req_level = deposit32(s->req_level, n, 1, !!level);
    if (level) {
        dmac_run(s);
    }
}

static uint64_t dmac_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraDmacState *s = opaque;

    if (addr >= D_CH_BASE && addr < D_CH_BASE + NCH * D_CH_STRIDE) {
        DmacChannel *c = &s->ch[(addr - D_CH_BASE) / D_CH_STRIDE];

        switch ((addr - D_CH_BASE) % D_CH_STRIDE) {
        case 0x0:
            return c->ctrl;
        case 0x4:
            return c->src;
        case 0x8:
            return c->dst;
        case 0xc:
            return (c->ctrl & C_EN) ? c->remaining : c->size;
        case 0x10:
            return c->llp;
        }
    }
    switch (addr) {
    case D_IDREV:
        return 0;
    case D_CFG:
        return CFG_VALUE;
    case D_CTRL:
        return s->ctrl;
    case D_INTST:
        return s->intst;
    case D_CHEN:
        return s->chen;
    case D_CHABORT:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-dmac: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void dmac_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraDmacState *s = opaque;

    if (addr >= D_CH_BASE && addr < D_CH_BASE + NCH * D_CH_STRIDE) {
        int n = (addr - D_CH_BASE) / D_CH_STRIDE;
        DmacChannel *c = &s->ch[n];

        switch ((addr - D_CH_BASE) % D_CH_STRIDE) {
        case 0x0: {
            bool was = c->ctrl & C_EN;

            c->ctrl = val & 0x3fffffff;
            if (c->ctrl & C_EN) {
                s->chen |= BIT(n);
                if (!was) {
                    c->remaining = c->size;
                }
            } else {
                s->chen &= ~BIT(n);
            }
            break;
        }
        case 0x4:
            c->src = val;
            break;
        case 0x8:
            c->dst = val;
            break;
        case 0xc:
            c->size = val & 0x3fffff;
            break;
        case 0x10:
            c->llp = val;
            break;
        }
        dmac_run(s);
        return;
    }
    switch (addr) {
    case D_CTRL:
        s->ctrl = val;
        break;
    case D_INTST:
        s->intst &= ~val;
        dmac_update_irq(s);
        break;
    case D_CHABORT:
        for (int n = 0; n < NCH; n++) {
            if ((val & BIT(n)) && (s->ch[n].ctrl & C_EN)) {
                dmac_status(s, n, ST_ABT(n), C_MASK_ABT);
                dmac_stop(s, n);
            }
        }
        dmac_update_irq(s);
        break;
    case D_IDREV:
    case D_CFG:
    case D_CHEN:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-dmac: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps dmac_ops = {
    .read = dmac_read,
    .write = dmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dmac_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraDmacState *s = ATBM_HERA_DMAC(obj);

    s->ctrl = 0;
    s->intst = 0;
    s->chen = 0;
    memset(s->ch, 0, sizeof(s->ch));
    /* the request lines are inputs: a peripheral's reset may have come first */
    s->running = false;
    dmac_update_irq(s);
}

static void dmac_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraDmacState *s = ATBM_HERA_DMAC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &dmac_ops, s,
                          TYPE_ATBM_HERA_DMAC, DMAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void dmac_init(Object *obj)
{
    AtbmHeraDmacState *s = ATBM_HERA_DMAC(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), dmac_req_in, "req", NREQ);
}

static const VMStateDescription vmstate_dmac_ch = {
    .name = TYPE_ATBM_HERA_DMAC "/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, DmacChannel),
        VMSTATE_UINT32(src, DmacChannel),
        VMSTATE_UINT32(dst, DmacChannel),
        VMSTATE_UINT32(size, DmacChannel),
        VMSTATE_UINT32(llp, DmacChannel),
        VMSTATE_UINT32(remaining, DmacChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_dmac = {
    .name = TYPE_ATBM_HERA_DMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, AtbmHeraDmacState),
        VMSTATE_UINT32(intst, AtbmHeraDmacState),
        VMSTATE_UINT32(chen, AtbmHeraDmacState),
        VMSTATE_STRUCT_ARRAY(ch, AtbmHeraDmacState, NCH, 1,
                             vmstate_dmac_ch, DmacChannel),
        VMSTATE_UINT32(req_level, AtbmHeraDmacState),
        VMSTATE_UINT32(req_driven, AtbmHeraDmacState),
        VMSTATE_END_OF_LIST()
    }
};

static void dmac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = dmac_realize;
    dc->vmsd = &vmstate_dmac;
    rc->phases.hold = dmac_reset_hold;
}

static const TypeInfo dmac_info = {
    .name = TYPE_ATBM_HERA_DMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraDmacState),
    .instance_init = dmac_init,
    .class_init = dmac_class_init,
};

static void dmac_register_types(void)
{
    type_register_static(&dmac_info);
}

type_init(dmac_register_types)
