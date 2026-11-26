/*
 * The SPI NOR flash inside an ATBM6441 "Hera"
 *
 * A GigaDevice/Winbond-class part of 2 or 4 MiB with the command set
 * the SDK's flash code uses (flash.h):
 *   03 read, 0b fast read, 3b dual output, 6b quad output, bb dual I/O,
 *   eb quad I/O; 9f JEDEC ID, 90 manufacturer/device ID, ab release
 *   from deep power down (and the ID), b9 deep power down;
 *   06 write enable, 04 write disable, 50 volatile status write enable;
 *   05 and 35 read status 1 and 2, 01 and 31 write status 1 and 2;
 *   02 page program, 32 quad page program, 20 sector erase (4 KiB),
 *   52 (32 KiB) and d8 (64 KiB) block erase, c7 and 60 chip erase.
 * The wires are bytes: a dual or quad command moves the same bytes. After
 * the address a fast read has a dummy byte, eb has three (the mode byte
 * and two dummy), bb one (the mode byte): the controller must clock them.
 *
 * A program, erase or status write starts when chip select goes high and
 * needs WEL; it makes WIP read 1 for a time, short against a real part's
 * (page 300 us, sector 4 ms, block 10 ms, chip 50 ms), and clears WEL.
 * A program only clears bits, wrapping inside its 256-byte page. Block
 * protect bits are kept and not enforced.
 *
 * The array is a RAM region, which the board maps for execute-in-place
 * reads. "drive" is the flash image (written back on a program or an
 * erase); without one the array starts erased (0xff).
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/block/atbm-hera-nor.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"

#define PAGE_SIZE_B     256
#define SECTOR_SIZE     (4 * KiB)
#define BLOCK32_SIZE    (32 * KiB)
#define BLOCK64_SIZE    (64 * KiB)

#define SR1_WIP         BIT(0)
#define SR1_WEL         BIT(1)
#define SR1_WMASK       0xfc

enum {
    CMD_NONE,
    CMD_ADDR,           /* collecting address bytes */
    CMD_DUMMY,          /* skipping dummy bytes */
    CMD_READ,
    CMD_PROGRAM,
    CMD_ID,
    CMD_REMS,
    CMD_RES,
    CMD_SR1,
    CMD_SR2,
    CMD_WRSR,
    CMD_WRSR2,
    CMD_IGNORE,
};

static void nor_sync(AtbmHeraNorState *s, uint32_t off, uint32_t len)
{
    memory_region_set_dirty(&s->array, off, len);
    if (s->blk) {
        if (blk_pwrite(s->blk, off, len, s->data + off, 0) < 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "atbm-hera-nor: write-back failed at 0x%x\n", off);
        }
    }
}

static void nor_done(void *opaque)
{
    AtbmHeraNorState *s = opaque;

    s->sr1 &= ~SR1_WIP;
}

static void nor_busy(AtbmHeraNorState *s, int64_t ns)
{
    s->sr1 |= SR1_WIP;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void nor_erase(AtbmHeraNorState *s, uint32_t addr, uint32_t len,
                      int64_t ns)
{
    addr &= ~(len - 1);
    memset(s->data + addr, 0xff, len);
    nor_sync(s, addr, len);
    nor_busy(s, ns);
}

/* the command is complete: what a rise of chip select starts */
static void nor_commit(AtbmHeraNorState *s)
{
    switch (s->op) {
    case 0x02:
    case 0x32:
        if (s->sr1 & SR1_WEL) {
            if (s->prog_hi > s->prog_lo) {
                nor_sync(s, s->prog_lo, s->prog_hi - s->prog_lo);
                nor_busy(s, 300 * SCALE_US);
            }
            s->sr1 &= ~SR1_WEL;
        }
        break;
    case 0x20:
    case 0x52:
    case 0xd8:
    case 0xc7:
    case 0x60:
        if (s->sr1 & SR1_WEL) {
            uint32_t len = s->op == 0x20 ? SECTOR_SIZE :
                           s->op == 0x52 ? BLOCK32_SIZE : BLOCK64_SIZE;

            if (s->op == 0xc7 || s->op == 0x60) {
                nor_erase(s, 0, s->size, 50 * SCALE_MS);
            } else {
                nor_erase(s, s->addr, len,
                          (s->op == 0x20 ? 4 : 10) * SCALE_MS);
            }
            s->sr1 &= ~SR1_WEL;
        }
        break;
    case 0x01:
    case 0x31:
        if ((s->sr1 & SR1_WEL) && s->nbytes && s->state != CMD_IGNORE) {
            if (s->op == 0x01) {
                s->sr1 = (s->sr1 & ~SR1_WMASK) | (s->new_sr1 & SR1_WMASK);
            }
            s->sr2 = s->new_sr2;
            s->wsr_volatile = false;
        }
        s->sr1 &= ~SR1_WEL;
        break;
    default:
        break;
    }
    s->op = 0;
}

static void nor_start(AtbmHeraNorState *s, uint8_t cmd)
{
    s->op = cmd;
    s->nbytes = 0;
    s->addr = 0;
    s->naddr = 0;
    s->state = CMD_IGNORE;

    if (s->deep_pd && cmd != 0xab) {
        return;
    }
    switch (cmd) {
    case 0x03:
        s->state = CMD_ADDR;
        s->after_addr = CMD_READ;
        s->dummy = 0;
        break;
    case 0x0b:
    case 0x3b:
    case 0x6b:
    case 0xbb:
        s->state = CMD_ADDR;
        s->after_addr = CMD_DUMMY;
        s->dummy = 1;
        break;
    case 0xeb:
        s->state = CMD_ADDR;
        s->after_addr = CMD_DUMMY;
        s->dummy = 3;
        break;
    case 0x02:
    case 0x32:
        s->state = CMD_ADDR;
        s->after_addr = CMD_PROGRAM;
        s->prog_lo = s->size;
        s->prog_hi = 0;
        break;
    case 0x20:
    case 0x52:
    case 0xd8:
        s->state = CMD_ADDR;
        s->after_addr = CMD_IGNORE;
        break;
    case 0xc7:
    case 0x60:
        break;
    case 0x9f:
        s->state = CMD_ID;
        break;
    case 0x90:
        s->state = CMD_ADDR;
        s->after_addr = CMD_REMS;
        s->dummy = 0;
        break;
    case 0xab:
        s->deep_pd = false;
        s->state = CMD_RES;
        s->naddr = 0;
        break;
    case 0xb9:
        s->deep_pd = true;
        break;
    case 0x06:
        s->sr1 |= SR1_WEL;
        break;
    case 0x04:
        s->sr1 &= ~SR1_WEL;
        break;
    case 0x50:
        s->sr1 |= SR1_WEL;
        s->wsr_volatile = true;
        break;
    case 0x05:
        s->state = CMD_SR1;
        break;
    case 0x35:
        s->state = CMD_SR2;
        break;
    case 0x01:
        s->state = CMD_WRSR;
        s->new_sr1 = s->sr1;
        s->new_sr2 = s->sr2;
        break;
    case 0x31:
        s->state = CMD_WRSR2;
        s->new_sr2 = s->sr2;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-nor: command 0x%02x\n", cmd);
        break;
    }
}

static uint32_t nor_transfer(SSIPeripheral *dev, uint32_t tx)
{
    AtbmHeraNorState *s = ATBM_HERA_NOR(dev);
    uint8_t rx = 0;

    if (!s->op) {
        if ((s->sr1 & SR1_WIP) && tx != 0x05 && tx != 0x35) {
            s->op = tx;
            s->state = CMD_IGNORE;
            return 0;
        }
        nor_start(s, tx);
        return 0;
    }
    if (s->sr1 & SR1_WIP && s->state != CMD_SR1 && s->state != CMD_SR2) {
        return 0;
    }
    switch (s->state) {
    case CMD_ADDR:
        s->addr = (s->addr << 8) | tx;
        if (++s->naddr == 3) {
            s->addr &= s->size - 1;
            s->state = s->after_addr;
            if (s->after_addr == CMD_DUMMY && !s->dummy) {
                s->state = CMD_READ;
            }
        }
        break;
    case CMD_DUMMY:
        if (--s->dummy == 0) {
            s->state = CMD_READ;
        }
        break;
    case CMD_READ:
        rx = s->data[s->addr];
        s->addr = (s->addr + 1) & (s->size - 1);
        break;
    case CMD_PROGRAM:
        if (s->sr1 & SR1_WEL) {
            uint32_t page = s->addr & ~(PAGE_SIZE_B - 1);

            s->data[s->addr] &= tx;
            s->prog_lo = MIN(s->prog_lo, s->addr);
            s->prog_hi = MAX(s->prog_hi, s->addr + 1);
            s->addr = page | ((s->addr + 1) & (PAGE_SIZE_B - 1));
        }
        break;
    case CMD_ID:
        rx = (s->jedec_id >> (16 - 8 * MIN(s->nbytes, 2))) & 0xff;
        break;
    case CMD_REMS:
        /* the manufacturer, then the device (the capacity byte less 1) */
        rx = ((s->dummy++ & 1) ^ (s->addr & 1)) ? (s->jedec_id & 0xff) - 1 :
                                                   s->jedec_id >> 16;
        break;
    case CMD_RES:
        if (++s->naddr > 3) {
            rx = s->jedec_id & 0xff;
        }
        break;
    case CMD_SR1:
        rx = s->sr1;
        break;
    case CMD_SR2:
        rx = s->sr2;
        break;
    case CMD_WRSR:
        if (s->nbytes == 0) {
            s->new_sr1 = tx;
        } else {
            s->new_sr2 = tx;
        }
        break;
    case CMD_WRSR2:
        s->new_sr2 = tx;
        break;
    default:
        break;
    }
    s->nbytes++;
    return rx;
}

static int nor_set_cs(SSIPeripheral *dev, bool deselect)
{
    AtbmHeraNorState *s = ATBM_HERA_NOR(dev);

    if (deselect) {
        /* chip select went high: the command ends */
        if (s->op) {
            nor_commit(s);
        }
        s->state = CMD_NONE;
    } else {
        s->op = 0;
        s->state = CMD_NONE;
    }
    return 0;
}

static void nor_realize(SSIPeripheral *dev, Error **errp)
{
    AtbmHeraNorState *s = ATBM_HERA_NOR(dev);
    int64_t len;

    if (s->size != 2 * MiB && s->size != 4 * MiB) {
        error_setg(errp, "the flash size must be 2 or 4 MiB");
        return;
    }
    if (!s->jedec_id) {
        s->jedec_id = s->size == 2 * MiB ? 0xc84015 : 0xc84016;
    }
    memory_region_init_ram(&s->array, OBJECT(dev), "atbm-hera-nor.array",
                           s->size, errp);
    s->data = memory_region_get_ram_ptr(&s->array);
    memset(s->data, 0xff, s->size);
    /* the CPU and DMA read it; only the SPI commands change it */
    memory_region_set_readonly(&s->array, true);
    if (s->blk) {
        len = blk_getlength(s->blk);
        if (len < 0 || len > s->size) {
            error_setg(errp, "the flash image is %" PRId64 " bytes, the part "
                       "%u", len, s->size);
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (len && blk_pread(s->blk, 0, len, s->data, 0) < 0) {
            error_setg(errp, "cannot read the flash image");
            return;
        }
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nor_done, s);
}

static void nor_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraNorState *s = ATBM_HERA_NOR(obj);

    s->sr1 = 0;
    s->sr2 = s->sr2_reset;
    s->op = 0;
    s->state = CMD_NONE;
    s->deep_pd = false;
    s->wsr_volatile = false;
    timer_del(s->timer);
}

/*
 * The array is saved with the machine as RAM; the image on disk follows
 * the program and erase commands
 */
static const VMStateDescription vmstate_nor = {
    .name = TYPE_ATBM_HERA_NOR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, AtbmHeraNorState),
        VMSTATE_UINT8(sr1, AtbmHeraNorState),
        VMSTATE_UINT8(sr2, AtbmHeraNorState),
        VMSTATE_UINT8(op, AtbmHeraNorState),
        VMSTATE_INT32(state, AtbmHeraNorState),
        VMSTATE_INT32(after_addr, AtbmHeraNorState),
        VMSTATE_UINT32(addr, AtbmHeraNorState),
        VMSTATE_UINT32(naddr, AtbmHeraNorState),
        VMSTATE_UINT32(dummy, AtbmHeraNorState),
        VMSTATE_UINT32(nbytes, AtbmHeraNorState),
        VMSTATE_UINT32(prog_lo, AtbmHeraNorState),
        VMSTATE_UINT32(prog_hi, AtbmHeraNorState),
        VMSTATE_UINT8(new_sr1, AtbmHeraNorState),
        VMSTATE_UINT8(new_sr2, AtbmHeraNorState),
        VMSTATE_BOOL(deep_pd, AtbmHeraNorState),
        VMSTATE_BOOL(wsr_volatile, AtbmHeraNorState),
        VMSTATE_TIMER_PTR(timer, AtbmHeraNorState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property nor_props[] = {
    DEFINE_PROP_DRIVE("drive", AtbmHeraNorState, blk),
    DEFINE_PROP_UINT32("size", AtbmHeraNorState, size, 2 * MiB),
    DEFINE_PROP_UINT32("jedec-id", AtbmHeraNorState, jedec_id, 0),
    DEFINE_PROP_UINT8("status2", AtbmHeraNorState, sr2_reset, 0),
};

static void nor_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(oc);

    k->realize = nor_realize;
    k->transfer = nor_transfer;
    k->set_cs = nor_set_cs;
    k->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &vmstate_nor;
    device_class_set_props(dc, nor_props);
    rc->phases.hold = nor_reset_hold;
}

static const TypeInfo nor_info = {
    .name = TYPE_ATBM_HERA_NOR,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(AtbmHeraNorState),
    .class_init = nor_class_init,
};

static void nor_register_types(void)
{
    type_register_static(&nor_info);
}

type_init(nor_register_types)
