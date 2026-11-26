/*
 * ATBM6441 "Hera" I2C master: an Andes ATCIIC100 at 0x16900000
 *
 * The registers (i2c.c in the SDK):
 *   0x00 ID/revision, 0x10 CFG (the FIFO depth in [1:0]: 2, 4, 8, 16),
 *   0x14 INTEN, 0x18 INTST and bus status, 0x1c slave ADDR, 0x20 DATA
 *   (the FIFO port), 0x24 CTL, 0x28 CMD, 0x2c SETUP
 *   INTST: [14] SDA, [13] SCL, [11] bus busy, [10] ACK (1 for an ACK),
 *     [9] transaction complete, [8] byte received, [7] byte transmitted,
 *     [6] start, [5] stop, [4] arbitration lost, [3] address hit
 *     (write 1 to clear these), and the FIFO levels [2] half, [1] full,
 *     [0] empty. INTEN has the same bits; the interrupt (chip number 22)
 *     is INTST & INTEN.
 *   CTL: [12] start, [11] address, [10] data and [9] stop phases, [8] 1 to
 *     read, [7:0] the byte count (0 is 256)
 *   CMD: 1 issues the transaction, 2 and 3 answer ACK or NACK (slave only),
 *     4 clears the FIFO, 5 resets the block
 *   SETUP: [0] enable, [2] master, [3] DMA enable
 * A transaction is the phases CTL names: a start, the address byte (ADDR
 * and the direction), the data and a stop. It runs as far as the FIFO
 * lets it, as the SPI does: a write drains the FIFO into the bus as it
 * is filled, a read refills it as it is emptied. A start with the bus
 * still held by an earlier transaction is a repeated start.
 *
 * Not modelled: slave mode (it never sees an address), 10-bit addresses,
 * arbitration, the timing fields, DMA.
 * Assumptions: ID/revision reads 0; the ACK bit is 1 for an ACK; the FIFO
 * depth is 4 at reset; a NACKed address ends the transaction at once with
 * the ACK bit 0 and the complete flag set.
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
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"

#define TYPE_ATBM_HERA_I2C "atbm-hera-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraI2cState, ATBM_HERA_I2C)

#define I_IDREV         0x00
#define I_CFG           0x10
#define I_INTEN         0x14
#define I_INTST         0x18
#define I_ADDR          0x1c
#define I_DATA          0x20
#define I_CTL           0x24
#define I_CMD           0x28
#define I_SETUP         0x2c
#define I2C_SIZE        0x100

#define F_EMPTY         BIT(0)
#define F_FULL          BIT(1)
#define F_HALF          BIT(2)
#define F_ADDRHIT       BIT(3)
#define F_ARBLOSE       BIT(4)
#define F_STOP          BIT(5)
#define F_START         BIT(6)
#define F_BYTETRANS     BIT(7)
#define F_BYTERECV      BIT(8)
#define F_CMPL          BIT(9)
#define S_ACK           BIT(10)
#define S_BUSY          BIT(11)
#define S_SCL           BIT(13)
#define S_SDA           BIT(14)
#define INT_LATCHED     (F_ADDRHIT | F_ARBLOSE | F_STOP | F_START | \
                         F_BYTETRANS | F_BYTERECV | F_CMPL)
#define INT_MASK        (INT_LATCHED | F_HALF | F_FULL | F_EMPTY)

#define CTL_START       BIT(12)
#define CTL_ADDR        BIT(11)
#define CTL_DATA        BIT(10)
#define CTL_STOP        BIT(9)
#define CTL_READ        BIT(8)

#define CMD_TRANS       1
#define CMD_FIFO_CLR    4
#define CMD_RESET       5

#define SETUP_EN        BIT(0)
#define SETUP_MASTER    BIT(2)

#define MAX_FIFO        16

typedef struct I2cFifo {
    uint8_t buf[MAX_FIFO];
    uint32_t head;
    uint32_t len;
} I2cFifo;

struct AtbmHeraI2cState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t cfg;
    uint32_t inten;
    uint32_t flags;
    uint32_t addr;
    uint32_t ctl;
    uint32_t setup;
    I2cFifo tx;
    I2cFifo rx;

    bool busy;          /* a transaction is under way */
    bool held;          /* the bus is held between transactions */
    bool ack;
    bool in_data;
    bool reading;
    uint32_t left;
};

static unsigned i2c_depth(AtbmHeraI2cState *s)
{
    return 2u << (s->cfg & 3);
}

static I2cFifo *i2c_fifo(AtbmHeraI2cState *s)
{
    return (s->ctl & CTL_READ) && s->reading ? &s->rx : &s->tx;
}

static uint32_t i2c_status(AtbmHeraI2cState *s)
{
    I2cFifo *f = i2c_fifo(s);
    uint32_t v = s->flags & INT_LATCHED;
    unsigned depth = i2c_depth(s);

    if (!f->len) {
        v |= F_EMPTY;
    }
    if (f->len >= depth) {
        v |= F_FULL;
    }
    if (f->len >= depth / 2) {
        v |= F_HALF;
    }
    return v;
}

static void i2c_update(AtbmHeraI2cState *s)
{
    qemu_set_irq(s->irq, !!(i2c_status(s) & s->inten));
}

static void i2c_finish(AtbmHeraI2cState *s)
{
    if (s->ctl & CTL_STOP) {
        i2c_end_transfer(s->bus);
        s->held = false;
        s->flags |= F_STOP;
    }
    s->busy = false;
    s->flags |= F_CMPL;
}

static void i2c_run(AtbmHeraI2cState *s)
{
    while (s->busy && s->in_data) {
        if (!s->left) {
            i2c_finish(s);
            break;
        }
        if (s->reading) {
            if (s->rx.len >= i2c_depth(s)) {
                return;
            }
            if (s->left == 1) {
                i2c_nack(s->bus);
            }
            s->rx.buf[(s->rx.head + s->rx.len++) % MAX_FIFO] =
                i2c_recv(s->bus);
            s->flags |= F_BYTERECV;
        } else {
            if (!s->tx.len) {
                return;
            }
            s->ack = !i2c_send(s->bus, s->tx.buf[s->tx.head]);
            s->tx.head = (s->tx.head + 1) % MAX_FIFO;
            s->tx.len--;
            s->flags |= F_BYTETRANS;
        }
        s->left--;
    }
}

static void i2c_start(AtbmHeraI2cState *s)
{
    uint32_t cnt = s->ctl & 0xff;
    int nack = 0;

    if (s->busy) {
        qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-i2c: command while busy\n");
        return;
    }
    if (!(s->setup & SETUP_EN) || !(s->setup & SETUP_MASTER)) {
        qemu_log_mask(LOG_UNIMP, "atbm-hera-i2c: transaction without "
                      "master mode\n");
        return;
    }
    s->busy = true;
    s->reading = !!(s->ctl & CTL_READ);
    s->left = cnt ? cnt : 256;
    s->in_data = false;
    if (s->ctl & CTL_START) {
        if (s->held) {
            i2c_end_transfer(s->bus);
        }
        s->flags |= F_START;
        s->held = true;
    }
    if (s->ctl & CTL_ADDR) {
        uint8_t a = s->addr & 0x7f;

        nack = s->reading ? i2c_start_recv(s->bus, a)
                          : i2c_start_send(s->bus, a);
        s->ack = !nack;
        if (nack) {
            i2c_end_transfer(s->bus);
            s->held = false;
            s->busy = false;
            s->flags |= F_CMPL;
            return;
        }
    }
    if (s->ctl & CTL_DATA) {
        s->in_data = true;
        i2c_run(s);
    } else {
        i2c_finish(s);
    }
}

static uint64_t i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraI2cState *s = opaque;

    switch (addr) {
    case I_IDREV:
        return 0;
    case I_CFG:
        return s->cfg;
    case I_INTEN:
        return s->inten;
    case I_INTST: {
        uint32_t v = i2c_status(s) | S_SCL | S_SDA;

        if (s->ack) {
            v |= S_ACK;
        }
        if (s->busy || s->held) {
            v |= S_BUSY;
        }
        return v;
    }
    case I_ADDR:
        return s->addr;
    case I_DATA: {
        uint32_t v = 0;

        if (s->rx.len) {
            v = s->rx.buf[s->rx.head];
            s->rx.head = (s->rx.head + 1) % MAX_FIFO;
            s->rx.len--;
        }
        i2c_run(s);
        i2c_update(s);
        return v;
    }
    case I_CTL:
        return s->ctl;
    case I_CMD:
        return 0;
    case I_SETUP:
        return s->setup;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-i2c: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void i2c_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraI2cState *s = opaque;

    switch (addr) {
    case I_CFG:
    case I_IDREV:
        break;
    case I_INTEN:
        s->inten = val & INT_MASK;
        break;
    case I_INTST:
        s->flags &= ~(val & INT_LATCHED);
        break;
    case I_ADDR:
        s->addr = val & 0x3ff;
        break;
    case I_DATA:
        if (s->tx.len < i2c_depth(s)) {
            s->tx.buf[(s->tx.head + s->tx.len++) % MAX_FIFO] = val;
        }
        i2c_run(s);
        break;
    case I_CTL:
        s->ctl = val & 0x1fff;
        break;
    case I_CMD:
        switch (val & 7) {
        case CMD_TRANS:
            i2c_start(s);
            break;
        case CMD_FIFO_CLR:
            s->tx.len = s->rx.len = 0;
            break;
        case CMD_RESET:
            if (s->held) {
                i2c_end_transfer(s->bus);
            }
            s->held = s->busy = false;
            s->tx.len = s->rx.len = 0;
            s->flags = 0;
            break;
        default:
            break;
        }
        break;
    case I_SETUP:
        s->setup = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-i2c: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
    i2c_update(s);
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void i2c_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraI2cState *s = ATBM_HERA_I2C(obj);

    s->cfg = 1;
    s->inten = 0;
    s->flags = 0;
    s->addr = 0;
    s->ctl = CTL_START | CTL_ADDR | CTL_DATA | CTL_STOP;
    s->setup = (5 << 24) | (1 << 21) | (5 << 16) | BIT(13) | (10 << 4);
    s->tx.len = s->rx.len = 0;
    s->busy = s->held = s->in_data = s->reading = false;
    s->ack = false;
    s->left = 0;
    i2c_update(s);
}

static void i2c_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraI2cState *s = ATBM_HERA_I2C(dev);

    s->bus = i2c_init_bus(dev, "i2c");
    memory_region_init_io(&s->iomem, OBJECT(dev), &i2c_ops, s,
                          TYPE_ATBM_HERA_I2C, I2C_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void i2c_init(Object *obj)
{
    AtbmHeraI2cState *s = ATBM_HERA_I2C(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_i2c_fifo = {
    .name = TYPE_ATBM_HERA_I2C "/fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(buf, I2cFifo, MAX_FIFO),
        VMSTATE_UINT32(head, I2cFifo),
        VMSTATE_UINT32(len, I2cFifo),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_i2c = {
    .name = TYPE_ATBM_HERA_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cfg, AtbmHeraI2cState),
        VMSTATE_UINT32(inten, AtbmHeraI2cState),
        VMSTATE_UINT32(flags, AtbmHeraI2cState),
        VMSTATE_UINT32(addr, AtbmHeraI2cState),
        VMSTATE_UINT32(ctl, AtbmHeraI2cState),
        VMSTATE_UINT32(setup, AtbmHeraI2cState),
        VMSTATE_STRUCT(tx, AtbmHeraI2cState, 1, vmstate_i2c_fifo, I2cFifo),
        VMSTATE_STRUCT(rx, AtbmHeraI2cState, 1, vmstate_i2c_fifo, I2cFifo),
        VMSTATE_BOOL(busy, AtbmHeraI2cState),
        VMSTATE_BOOL(held, AtbmHeraI2cState),
        VMSTATE_BOOL(ack, AtbmHeraI2cState),
        VMSTATE_BOOL(in_data, AtbmHeraI2cState),
        VMSTATE_BOOL(reading, AtbmHeraI2cState),
        VMSTATE_UINT32(left, AtbmHeraI2cState),
        VMSTATE_END_OF_LIST()
    }
};

static void i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = i2c_realize;
    dc->vmsd = &vmstate_i2c;
    rc->phases.hold = i2c_reset_hold;
}

static const TypeInfo i2c_info = {
    .name = TYPE_ATBM_HERA_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraI2cState),
    .instance_init = i2c_init,
    .class_init = i2c_class_init,
};

static void i2c_register_types(void)
{
    type_register_static(&i2c_info);
}

type_init(i2c_register_types)
