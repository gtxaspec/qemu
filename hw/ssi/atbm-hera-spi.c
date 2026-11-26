/*
 * ATBM6441 "Hera" SPI master: an Andes ATCSPI200
 *
 * SPI1 (0x16a00000) is the controller of the internal flash, SPI3
 * (0x16d00000) the user SPI master; SPI2 (0x16c00000) is the slave HIF
 * (atbm-hera-spi-slave). The registers (flash.h and the drivers in
 * src/driver/spi):
 *   0x10 IFSET     address length [17:16] (+1 bytes), data length [12:8],
 *                  data merge [7] (a 32-bit access moves 4 bytes), mode
 *   0x14 PIO       direct pin control: kept
 *   0x20 TRANSCTRL [30] command, [29] address, [27:24] transfer mode,
 *                  [20:12] write count - 1, [10:9] dummy count - 1,
 *                  [8:0] read count - 1
 *   0x24 CMD       a write starts the transaction: chip select low, the
 *                  command, the address, the phases the mode asks for
 *   0x28 ADDR      0x2c DATA, the FIFO port
 *   0x30 CTRL      [2] TX FIFO reset, [1] RX FIFO reset, [0] reset (all
 *                  self-clearing), [4]/[3] TX/RX DMA enable, thresholds
 *   0x34 STATUS    [0] busy, [14] RX empty, [12:8] RX entries, [22] TX
 *                  empty, [23] TX full, [20:16] TX entries
 *   0x38 INTEN, 0x3c INTST (write 1 to clear): [4] transfer end, [3] TX
 *                  threshold, [2] RX threshold, [1] TX underrun,
 *                  [0] RX overrun
 *   0x40 TIMING, 0x50 MEMCTRL, 0x60 SLVST, 0x64 SLVCNT: kept
 *   0x7c HWCFG     read-only
 * Transfer modes: 0 write and read together, 1 write, 2 read, 3 write then
 * read, 4 read then write, 5 write, dummy, read, 6 read, dummy, write,
 * 7 none, 8 dummy then write, 9 dummy then read.
 *
 * The bus is the "ssi" bus with one chip select line "cs" (active low).
 * The FIFOs are not clocked in the background: a transaction proceeds as
 * far as the FIFOs let it, when the command is written, a byte is written
 * to DATA (the transmit side drains at once) or one is read (the receive
 * side refills at once), so a DMA or PIO reader sees data as long as the
 * read count has bytes left. Busy is 1 until every byte has been clocked.
 * The DMA request lines "dreq-tx" and "dreq-rx" follow CTRL's enables and
 * the FIFOs.
 *
 * With "slave" set (SPI2) the block reads as the slave-capable one and
 * keeps SLVST and SLVCNT, but is still driven as a master: the slave
 * protocol of the SPI host interface (hi_spi.c, its status word, DMA
 * channels and GPIO handshake) is not modelled.
 *
 * Not modelled: the data length (8 bits only), LSB first, the direct pin
 * mode and the slave mode, SCLK timing (a byte takes no time).
 * Assumptions: FIFO depths of 4 entries (HWCFG says so); the end interrupt
 * is raised when the last byte has been clocked; IFSET resets to what the
 * firmware's spi_init() writes (3-byte address, 8-bit data, no merge): the
 * SDK's flash driver reads the data port a byte per 32-bit access without
 * ever writing IFSET, so the bootrom must leave it that way.
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
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"

#define TYPE_ATBM_HERA_SPI "atbm-hera-spi"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraSpiState, ATBM_HERA_SPI)

#define S_IDREV         0x00
#define S_IFSET         0x10
#define S_PIO           0x14
#define S_REQDELAY      0x18
#define S_TRANSCTRL     0x20
#define S_CMD           0x24
#define S_ADDR          0x28
#define S_DATA          0x2c
#define S_CTRL          0x30
#define S_ST            0x34
#define S_INTEN         0x38
#define S_INTST         0x3c
#define S_TIMING        0x40
#define S_MEMCTRL       0x50
#define S_SLVST         0x60
#define S_SLVCNT        0x64
#define S_HWCFG         0x7c
#define SPI_SIZE        0x100

#define IDREV_VALUE     0x02020000
#define HWCFG_VALUE     0x00001711
#define HWCFG_SLAVE     BIT(14)

#define IFSET_MERGE     BIT(7)

#define CTRL_RST        BIT(0)
#define CTRL_RXRST      BIT(1)
#define CTRL_TXRST      BIT(2)
#define CTRL_RXDMA      BIT(3)
#define CTRL_TXDMA      BIT(4)

#define INT_RXOR        BIT(0)
#define INT_TXUR        BIT(1)
#define INT_RXTH        BIT(2)
#define INT_TXTH        BIT(3)
#define INT_END         BIT(4)
#define INT_LATCHED     (INT_RXOR | INT_TXUR | INT_END)

#define FIFO_ENTRIES    4
#define FIFO_BYTES      (FIFO_ENTRIES * 4)

enum { M_DUPLEX, M_WR, M_RD, M_WR_RD, M_RD_WR, M_WR_DMY_RD, M_RD_DMY_WR,
       M_NONE, M_DMY_WR, M_DMY_RD };
enum { P_NONE, P_WR, P_RD, P_DMY, P_DUPLEX };

#define MAX_PHASES      3

typedef struct SpiFifo {
    uint8_t buf[FIFO_BYTES];
    uint32_t head;
    uint32_t len;
} SpiFifo;

struct AtbmHeraSpiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *ssi;
    bool slave;
    qemu_irq irq;
    qemu_irq cs;
    qemu_irq dreq_tx;
    qemu_irq dreq_rx;

    uint32_t ifset;
    uint32_t pio;
    uint32_t reqdelay;
    uint32_t transctrl;
    uint32_t addr;
    uint32_t ctrl;
    uint32_t inten;
    uint32_t intst;
    uint32_t timing;
    uint32_t memctrl;
    uint32_t slvst;
    uint32_t slvcnt;

    SpiFifo tx;
    SpiFifo rx;

    bool busy;
    uint8_t phase_type[MAX_PHASES];
    uint32_t phase_left[MAX_PHASES];
    uint32_t cur;           /* the phase under way */
    bool tx_req;
    bool rx_req;
};

static bool spi_merge(AtbmHeraSpiState *s)
{
    return s->ifset & IFSET_MERGE;
}

static uint32_t fifo_cap(AtbmHeraSpiState *s)
{
    return spi_merge(s) ? FIFO_BYTES : FIFO_ENTRIES;
}

static uint32_t fifo_entries(AtbmHeraSpiState *s, SpiFifo *f)
{
    return spi_merge(s) ? DIV_ROUND_UP(f->len, 4) : f->len;
}

static void fifo_push(SpiFifo *f, uint8_t b)
{
    f->buf[(f->head + f->len++) % FIFO_BYTES] = b;
}

static uint8_t fifo_pop(SpiFifo *f)
{
    uint8_t b = f->buf[f->head];

    f->head = (f->head + 1) % FIFO_BYTES;
    f->len--;
    return b;
}

static void spi_update(AtbmHeraSpiState *s)
{
    uint32_t pend = s->intst & INT_LATCHED;
    uint32_t rxth = extract32(s->ctrl, 8, 5);
    uint32_t txth = extract32(s->ctrl, 16, 5);
    bool tx_req, rx_req;

    if (fifo_entries(s, &s->rx) >= MAX(rxth, 1)) {
        pend |= INT_RXTH;
    }
    if (fifo_entries(s, &s->tx) <= txth) {
        pend |= INT_TXTH;
    }
    qemu_set_irq(s->irq, !!(pend & s->inten));

    tx_req = (s->ctrl & CTRL_TXDMA) && s->tx.len < fifo_cap(s);
    rx_req = (s->ctrl & CTRL_RXDMA) && (s->rx.len > 0 || s->busy);
    if (tx_req != s->tx_req) {
        s->tx_req = tx_req;
        qemu_set_irq(s->dreq_tx, tx_req);
    }
    if (rx_req != s->rx_req) {
        s->rx_req = rx_req;
        qemu_set_irq(s->dreq_rx, rx_req);
    }
}

static void spi_finish(AtbmHeraSpiState *s)
{
    s->busy = false;
    qemu_set_irq(s->cs, 1);
    /* what the transfer did not use of the write FIFO is dropped */
    s->tx.len = 0;
    s->intst |= INT_END;
}

/* Run the transaction as far as the FIFOs let it */
static void spi_run(AtbmHeraSpiState *s)
{
    while (s->busy) {
        uint32_t *left = &s->phase_left[s->cur];

        if (s->cur >= MAX_PHASES || s->phase_type[s->cur] == P_NONE ||
            !*left) {
            if (s->cur + 1 >= MAX_PHASES ||
                s->phase_type[s->cur + 1] == P_NONE) {
                spi_finish(s);
                break;
            }
            s->cur++;
            continue;
        }
        switch (s->phase_type[s->cur]) {
        case P_WR:
            if (!s->tx.len) {
                return;
            }
            ssi_transfer(s->ssi, fifo_pop(&s->tx));
            break;
        case P_RD:
            if (s->rx.len >= fifo_cap(s)) {
                return;
            }
            fifo_push(&s->rx, ssi_transfer(s->ssi, 0));
            break;
        case P_DUPLEX:
            if (!s->tx.len || s->rx.len >= fifo_cap(s)) {
                return;
            }
            fifo_push(&s->rx, ssi_transfer(s->ssi, fifo_pop(&s->tx)));
            break;
        default:
            ssi_transfer(s->ssi, 0);
            break;
        }
        (*left)--;
    }
}

static void spi_start(AtbmHeraSpiState *s, uint32_t cmd)
{
    uint32_t tc = s->transctrl;
    uint32_t mode = extract32(tc, 24, 4);
    uint32_t wcnt = extract32(tc, 12, 9) + 1;
    uint32_t dcnt = extract32(tc, 9, 2) + 1;
    uint32_t rcnt = extract32(tc, 0, 9) + 1;
    uint32_t alen = extract32(s->ifset, 16, 2) + 1;
    int p = 0;

    if (s->busy) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "atbm-hera-spi: command while busy\n");
        return;
    }
    memset(s->phase_type, P_NONE, sizeof(s->phase_type));
    memset(s->phase_left, 0, sizeof(s->phase_left));
    s->cur = 0;

#define PHASE(t, n) do { s->phase_type[p] = (t); s->phase_left[p++] = (n); } \
                    while (0)
    switch (mode) {
    case M_DUPLEX:
        PHASE(P_DUPLEX, wcnt);
        break;
    case M_WR:
        PHASE(P_WR, wcnt);
        break;
    case M_RD:
        PHASE(P_RD, rcnt);
        break;
    case M_WR_RD:
        PHASE(P_WR, wcnt);
        PHASE(P_RD, rcnt);
        break;
    case M_RD_WR:
        PHASE(P_RD, rcnt);
        PHASE(P_WR, wcnt);
        break;
    case M_WR_DMY_RD:
        PHASE(P_WR, wcnt);
        PHASE(P_DMY, dcnt);
        PHASE(P_RD, rcnt);
        break;
    case M_RD_DMY_WR:
        PHASE(P_RD, rcnt);
        PHASE(P_DMY, dcnt);
        PHASE(P_WR, wcnt);
        break;
    case M_DMY_WR:
        PHASE(P_DMY, dcnt);
        PHASE(P_WR, wcnt);
        break;
    case M_DMY_RD:
        PHASE(P_DMY, dcnt);
        PHASE(P_RD, rcnt);
        break;
    default:
        break;
    }
#undef PHASE

    s->busy = true;
    qemu_set_irq(s->cs, 0);
    if (extract32(tc, 30, 1)) {
        ssi_transfer(s->ssi, cmd & 0xff);
    }
    if (extract32(tc, 29, 1)) {
        for (int i = alen - 1; i >= 0; i--) {
            ssi_transfer(s->ssi, (s->addr >> (8 * i)) & 0xff);
        }
    }
    spi_run(s);
}

static uint32_t spi_status(AtbmHeraSpiState *s)
{
    uint32_t rx = fifo_entries(s, &s->rx), tx = fifo_entries(s, &s->tx);
    uint32_t v = 0;

    v = deposit32(v, 0, 1, s->busy);
    v = deposit32(v, 8, 5, rx);
    v = deposit32(v, 14, 1, !rx);
    v = deposit32(v, 15, 1, rx >= FIFO_ENTRIES);
    v = deposit32(v, 16, 5, tx);
    v = deposit32(v, 22, 1, !tx);
    v = deposit32(v, 23, 1, s->tx.len >= fifo_cap(s));
    return v;
}

static uint64_t spi_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraSpiState *s = opaque;
    uint32_t v = 0;

    switch (addr) {
    case S_IDREV:
        return IDREV_VALUE;
    case S_IFSET:
        return s->ifset;
    case S_PIO:
        return s->pio;
    case S_REQDELAY:
        return s->reqdelay;
    case S_TRANSCTRL:
        return s->transctrl;
    case S_CMD:
        return 0;
    case S_ADDR:
        return s->addr;
    case S_DATA: {
        int n = spi_merge(s) ? 4 : 1;

        /* the transaction refills the FIFO as the port is read */
        spi_run(s);
        for (int i = 0; i < n && s->rx.len; i++) {
            v |= fifo_pop(&s->rx) << (8 * i);
            spi_run(s);
        }
        spi_update(s);
        return v;
    }
    case S_CTRL:
        return s->ctrl & ~(CTRL_RST | CTRL_RXRST | CTRL_TXRST);
    case S_ST:
        return spi_status(s);
    case S_INTEN:
        return s->inten;
    case S_INTST: {
        uint32_t rxth = extract32(s->ctrl, 8, 5);
        uint32_t txth = extract32(s->ctrl, 16, 5);

        v = s->intst & INT_LATCHED;
        if (fifo_entries(s, &s->rx) >= MAX(rxth, 1)) {
            v |= INT_RXTH;
        }
        if (fifo_entries(s, &s->tx) <= txth) {
            v |= INT_TXTH;
        }
        return v;
    }
    case S_TIMING:
        return s->timing;
    case S_MEMCTRL:
        return s->memctrl;
    case S_SLVST:
        return s->slvst;
    case S_SLVCNT:
        return s->slvcnt;
    case S_HWCFG:
        return HWCFG_VALUE | (s->slave ? HWCFG_SLAVE : 0);
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-spi: read at 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void spi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraSpiState *s = opaque;

    switch (addr) {
    case S_IFSET:
        s->ifset = val;
        break;
    case S_PIO:
        s->pio = val;
        break;
    case S_REQDELAY:
        s->reqdelay = val;
        break;
    case S_TRANSCTRL:
        s->transctrl = val;
        break;
    case S_CMD:
        spi_start(s, val);
        break;
    case S_ADDR:
        s->addr = val;
        break;
    case S_DATA: {
        int n = spi_merge(s) ? 4 : 1;

        for (int i = 0; i < n && s->tx.len < fifo_cap(s); i++) {
            fifo_push(&s->tx, val >> (8 * i));
        }
        spi_run(s);
        break;
    }
    case S_CTRL:
        s->ctrl = val & ~(CTRL_RST | CTRL_RXRST | CTRL_TXRST);
        if (val & CTRL_RST) {
            s->busy = false;
            qemu_set_irq(s->cs, 1);
        }
        if (val & (CTRL_RST | CTRL_TXRST)) {
            s->tx.len = 0;
        }
        if (val & (CTRL_RST | CTRL_RXRST)) {
            s->rx.len = 0;
        }
        break;
    case S_INTEN:
        s->inten = val & 0x3f;
        break;
    case S_INTST:
        s->intst &= ~val;
        break;
    case S_TIMING:
        s->timing = val;
        break;
    case S_MEMCTRL:
        s->memctrl = val;
        break;
    case S_SLVST:
        s->slvst = val;
        break;
    case S_SLVCNT:
        s->slvcnt = val;
        break;
    case S_IDREV:
    case S_ST:
    case S_HWCFG:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atbm-hera-spi: write 0x%" PRIx64
                      " at 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
    spi_update(s);
}

static const MemoryRegionOps spi_ops = {
    .read = spi_read,
    .write = spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void spi_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraSpiState *s = ATBM_HERA_SPI(obj);

    s->ifset = (2 << 16) | (7 << 8);
    s->pio = 0;
    s->reqdelay = 0;
    s->transctrl = 0;
    s->addr = 0;
    s->ctrl = 0;
    s->inten = 0;
    s->intst = 0;
    s->timing = (2 << 8) | 1;
    s->memctrl = 0;
    s->slvst = 0;
    s->slvcnt = 0;
    s->tx.len = s->tx.head = 0;
    s->rx.len = s->rx.head = 0;
    s->busy = false;
    s->tx_req = false;
    s->rx_req = false;
    qemu_set_irq(s->cs, 1);
    qemu_set_irq(s->dreq_tx, 0);
    qemu_set_irq(s->dreq_rx, 0);
    spi_update(s);
}

static void spi_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraSpiState *s = ATBM_HERA_SPI(dev);

    s->ssi = ssi_create_bus(dev, "ssi");
    memory_region_init_io(&s->iomem, OBJECT(dev), &spi_ops, s,
                          TYPE_ATBM_HERA_SPI, SPI_SIZE);
    /*
     * A DMA request raised by a register access is served at once, by
     * the controller reading or writing the FIFO port: the access nests
     */
    s->iomem.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void spi_init(Object *obj)
{
    AtbmHeraSpiState *s = ATBM_HERA_SPI(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cs, "cs", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dreq_tx, "dreq-tx", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dreq_rx, "dreq-rx", 1);
}

static const Property spi_props[] = {
    DEFINE_PROP_BOOL("slave", AtbmHeraSpiState, slave, false),
};

static const VMStateDescription vmstate_spi_fifo = {
    .name = TYPE_ATBM_HERA_SPI "/fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(buf, SpiFifo, FIFO_BYTES),
        VMSTATE_UINT32(head, SpiFifo),
        VMSTATE_UINT32(len, SpiFifo),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_spi = {
    .name = TYPE_ATBM_HERA_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ifset, AtbmHeraSpiState),
        VMSTATE_UINT32(pio, AtbmHeraSpiState),
        VMSTATE_UINT32(reqdelay, AtbmHeraSpiState),
        VMSTATE_UINT32(transctrl, AtbmHeraSpiState),
        VMSTATE_UINT32(addr, AtbmHeraSpiState),
        VMSTATE_UINT32(ctrl, AtbmHeraSpiState),
        VMSTATE_UINT32(inten, AtbmHeraSpiState),
        VMSTATE_UINT32(intst, AtbmHeraSpiState),
        VMSTATE_UINT32(timing, AtbmHeraSpiState),
        VMSTATE_UINT32(memctrl, AtbmHeraSpiState),
        VMSTATE_UINT32(slvst, AtbmHeraSpiState),
        VMSTATE_UINT32(slvcnt, AtbmHeraSpiState),
        VMSTATE_STRUCT(tx, AtbmHeraSpiState, 1, vmstate_spi_fifo, SpiFifo),
        VMSTATE_STRUCT(rx, AtbmHeraSpiState, 1, vmstate_spi_fifo, SpiFifo),
        VMSTATE_BOOL(busy, AtbmHeraSpiState),
        VMSTATE_UINT8_ARRAY(phase_type, AtbmHeraSpiState, MAX_PHASES),
        VMSTATE_UINT32_ARRAY(phase_left, AtbmHeraSpiState, MAX_PHASES),
        VMSTATE_UINT32(cur, AtbmHeraSpiState),
        VMSTATE_BOOL(tx_req, AtbmHeraSpiState),
        VMSTATE_BOOL(rx_req, AtbmHeraSpiState),
        VMSTATE_END_OF_LIST()
    }
};

static void spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = spi_realize;
    dc->vmsd = &vmstate_spi;
    device_class_set_props(dc, spi_props);
    rc->phases.hold = spi_reset_hold;
}

static const TypeInfo spi_info = {
    .name = TYPE_ATBM_HERA_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraSpiState),
    .instance_init = spi_init,
    .class_init = spi_class_init,
};

static void spi_register_types(void)
{
    type_register_static(&spi_info);
}

type_init(spi_register_types)
