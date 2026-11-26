/*
 * ATBM6441 "Hera" HIF: the SDIO slave's host interface block, at
 * 0x0ab00000 on the chip side (interface for the host side:
 * include/hw/misc/atbm-hera-hif.h)
 *
 * The chip side, from hi_sdio.h and the SDK's SDIO demo (sdio_demo_main.c):
 *   0x000 .. 0x0ff  the input ring, the messages the host sends: 64
 *                   descriptors, one word each (32 of them every other
 *                   word with the "input64" property off). The firmware
 *                   arms one with the message buffer's offset in SRAM
 *                   ([16:0]) and bit 17. A host write to the buffer it
 *                   names clears bit 17 and puts the length, in 16-bit
 *                   words, in [29:18]. One write may carry several
 *                   messages, one after the other, each from a function
 *                   1 block boundary; they fill the descriptors that
 *                   follow the one the write names.
 *   0x100 .. 0x11f  the output ring, the messages for the host: 4
 *                   descriptors of an offset word and a word with the
 *                   length in 16-bit words in [15:1] and busy in [0]. The
 *                   firmware sets busy; the host's read of the message
 *                   clears it.
 *   0x120 INT_ENABLE, 0x124 INT_RAWSTATUS (the interrupt is the AND of
 *   the two), 0x128 INT_CLEAR: [0] priority output done, [1] output
 *   done, [2] input arrived, [3] buffer number mismatch, [6] a read with
 *   nothing to send, [11] a write to a buffer that is not armed (the
 *   others are error causes the model never raises)
 *   0x12c, 0x130 priority output, 0x134 CONFIG, 0x138 CONTROL, 0x13c
 *   general purpose, 0x140 mode, 0x144 CCCR: kept
 *   0x148 function 1 block size, the host's (read only)
 *   0x14c host IRQ, 0x150 long channel number, 0x154 channel length,
 *   0x15c write channel flag: kept, CONFIG and CONTROL as below
 *   0x16c SDIO soft reset: bit 0 sets the output pointer back to 0
 * The host's function 1 (see the header): CONFIG [14] holds the CPU in
 * reset, [16] and [17] enable the host interrupt, [11] and [13] ask for
 * a window prefetch and clear themselves; CONTROL [12] asks for a wake
 * and [13] answers it (at once), [11:0] and [15:14] hold the length of
 * the next output message in 16-bit words, 0 with none; QUEUE, the data
 * of the ring; the windows read and write the chip's memory at the base
 * register's address; the last 16 bits of a message read are CONTROL
 * again, the length of the one after.
 * The host interrupt is up while an output message is waiting and
 * CONFIG enables it.
 *
 * MMIO region 1 is a test port that lets a CPU or a qtest play the host:
 *   0x00 ADDR (17-bit function 1 address), 0x04 LEN (bytes), 0x08 INCR,
 *   0x10 DATA (a stream of words: writes collect LEN bytes and then are
 *   written; the first read fetches LEN bytes and the reads stream them),
 *   0x14 the host interrupt level, 0x18 function 1's block size.
 *
 * Not modelled: the SDIO bus (the card model has it), the block size but
 * for where a message starts, the CRC and framing errors, the sleep
 * handshake beyond CONTROL's, the prefetch (reads of a window are direct),
 * PRIORITY output channel use.
 * Assumptions: 64 input descriptors by default (the host driver's
 * SDIO_INPUT_64 build); a read with no message returns zeros; the soft
 * reset keeps the descriptors; host IRQ bit 0 also raises the host
 * interrupt. Inferred from the host driver (hal_apollo bh_sdio.c
 * atbm_sdio_tx_bh, which packs messages into one write, each padded to the
 * block size, and counts a buffer for each): where a message in a write
 * ends. The block is taken to go by the word that opens every message,
 * its length in [15:0] with a check of it in [31:24] (the sum of the
 * length's two bytes, plus its carry), and to start the next message at
 * the next block; a write that does not open with such a word is one
 * message, and so is every write while the host has set no block size.
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
#include "hw/misc/atbm-hera-hif.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"

#define H_IN_BASE       0x000
#define H_OUT_BASE      0x100
#define H_INT_ENABLE    0x120
#define H_INT_RAW       0x124
#define H_INT_CLEAR     0x128
#define H_CONFIG        0x134
#define H_CONTROL       0x138
#define H_FN1_BLKSZ     0x148
#define H_HOST_IRQ      0x14c
#define H_SOFT_RESET    0x16c
#define HIF_SIZE        0x200
#define HIF_REGS        (HIF_SIZE / 4)

#define NOUT            4
#define MAX_XFER        8192

#define DESC_ARMED      BIT(17)
#define DESC_OFF_MASK   0x1ffff
#define OUT_BUSY        BIT(0)

#define INT_PRIO_CMPL   BIT(0)
#define INT_OUT_CMPL    BIT(1)
#define INT_IN_CMPL     BIT(2)
#define INT_ERR_NUM     BIT(3)
#define INT_ERR_NODATA  BIT(6)
#define INT_ERR_NOBUF   BIT(11)

#define CFG_AHB_PFETCH  BIT(11)
#define CFG_APB_PFETCH  BIT(13)
#define CFG_CPU_RESET   BIT(14)
#define CFG_IRQ_EN      (BIT(16) | BIT(17))
#define CTL_WUP         BIT(12)
#define CTL_RDY         BIT(13)

#define R_CONFIG        0
#define R_CONTROL       1
#define R_QUEUE         2
#define R_AHB           3
#define R_BASE          4
#define R_SRAM          5

#define P_ADDR          0x00
#define P_LEN           0x04
#define P_INCR          0x08
#define P_DATA          0x10
#define P_IRQ           0x14
#define P_BLKSZ         0x18
#define PORT_SIZE       0x100

struct AtbmHeraHifState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion port;
    qemu_irq irq;
    qemu_irq host_irq;
    qemu_irq cpu_reset;

    bool input64;
    uint32_t sram_base;

    uint32_t reg[HIF_REGS];     /* the rings and the kept registers */
    uint32_t inten;
    uint32_t raw;
    uint32_t config;
    uint32_t control;
    uint32_t base;
    uint32_t tset[2];
    uint32_t blksz;
    unsigned out_rd;

    uint32_t p_addr;
    uint32_t p_len;
    uint32_t p_incr;
    uint32_t p_pos;
    bool p_reading;
    uint8_t p_buf[MAX_XFER];
};

static unsigned hif_ndesc(AtbmHeraHifState *s)
{
    return s->input64 ? 64 : 32;
}

static uint32_t *hif_in_desc(AtbmHeraHifState *s, unsigned i)
{
    return &s->reg[(H_IN_BASE + i * (s->input64 ? 4 : 8)) / 4];
}

static uint32_t *hif_out_a(AtbmHeraHifState *s, unsigned i)
{
    return &s->reg[(H_OUT_BASE + i * 8) / 4];
}

static uint32_t *hif_out_b(AtbmHeraHifState *s, unsigned i)
{
    return &s->reg[(H_OUT_BASE + i * 8 + 4) / 4];
}

/* the next message's length in 16-bit words, 0 with none */
static uint32_t hif_next_words(AtbmHeraHifState *s)
{
    uint32_t b = *hif_out_b(s, s->out_rd);

    return (b & OUT_BUSY) ? (b >> 1) & 0x7fff : 0;
}

bool atbm_hera_hif_host_irq(AtbmHeraHifState *s)
{
    return ((s->config & CFG_IRQ_EN) && hif_next_words(s)) ||
           (s->reg[H_HOST_IRQ / 4] & 1);
}

static void hif_update(AtbmHeraHifState *s)
{
    qemu_set_irq(s->irq, !!(s->raw & s->inten));
    qemu_set_irq(s->host_irq, atbm_hera_hif_host_irq(s));
}

static uint32_t hif_control_view(AtbmHeraHifState *s)
{
    uint32_t words = hif_next_words(s);

    return (s->control & (CTL_WUP | CTL_RDY)) | (words & 0xfff) |
           ((words >> 12) & 3) << 14;
}

/*
 * The length of the message a host write holds at buf, by the word that
 * opens it; 0 if that is no such word
 */
static size_t hif_msg_len(const uint8_t *buf, size_t len)
{
    uint32_t flag, sum;

    if (len < 4) {
        return 0;
    }
    flag = ldl_le_p(buf);
    sum = (flag & 0xff) + ((flag >> 8) & 0xff);
    sum = (sum + (sum >> 8)) & 0xff;
    if ((flag & 0xffff) < 4 || (flag & 0xffff) > len || flag >> 24 != sum) {
        return 0;
    }
    return flag & 0xffff;
}

void atbm_hera_hif_set_block_size(AtbmHeraHifState *s, unsigned size)
{
    s->blksz = size;
}

static void hif_queue_write(AtbmHeraHifState *s, unsigned buf_id,
                            const uint8_t *buf, size_t len)
{
    unsigned n = 0;

    while (len) {
        uint32_t *d = hif_in_desc(s, (buf_id + n) % hif_ndesc(s));
        size_t msg = s->blksz ? hif_msg_len(buf, len) : 0;
        size_t step = msg ? MIN(len, ROUND_UP(msg, s->blksz)) : len;

        if (!msg) {
            if (n) {
                /* what follows the last message is padding */
                break;
            }
            msg = len;
        }
        if (!(*d & DESC_ARMED)) {
            s->raw |= INT_ERR_NOBUF;
            break;
        }
        if (address_space_write(&address_space_memory,
                                s->sram_base + (*d & DESC_OFF_MASK),
                                MEMTXATTRS_UNSPECIFIED, buf,
                                msg) != MEMTX_OK) {
            s->raw |= INT_ERR_NUM;
            break;
        }
        trace_atbm_hera_hif_input((buf_id + n) % hif_ndesc(s),
                                  *d & DESC_OFF_MASK, msg);
        *d = (*d & DESC_OFF_MASK) | (((step + 1) / 2) & 0xfff) << 18;
        s->raw |= INT_IN_CMPL;
        buf += step;
        len -= step;
        n++;
    }
    hif_update(s);
}

static void hif_queue_read(AtbmHeraHifState *s, unsigned buf_id,
                           uint8_t *buf, size_t len)
{
    unsigned idx = s->out_rd;
    uint32_t a = *hif_out_a(s, idx), b = *hif_out_b(s, idx);

    memset(buf, 0, len);
    if (!(b & OUT_BUSY)) {
        s->raw |= INT_ERR_NODATA;
        hif_update(s);
        return;
    }
    if (buf_id && (buf_id - 1) % NOUT != idx) {
        qemu_log_mask(LOG_GUEST_ERROR, "atbm-hera-hif: read of buffer %u, "
                      "descriptor %u is next\n", buf_id, idx);
    }
    address_space_read(&address_space_memory,
                       s->sram_base + (a & DESC_OFF_MASK),
                       MEMTXATTRS_UNSPECIFIED, buf,
                       MIN(len, (size_t)((b >> 1) & 0x7fff) * 2));
    trace_atbm_hera_hif_output(idx, a & DESC_OFF_MASK, (b >> 1) & 0x7fff);
    *hif_out_b(s, idx) = b & ~OUT_BUSY;
    s->out_rd = (idx + 1) % NOUT;
    s->raw |= INT_OUT_CMPL;
    if (len >= 2) {
        stw_le_p(buf + len - 2, hif_control_view(s));
    }
    hif_update(s);
}

static uint32_t hif_host_reg_read(AtbmHeraHifState *s, unsigned reg)
{
    switch (reg) {
    case R_CONFIG:
        return s->config;
    case R_CONTROL:
        return hif_control_view(s);
    case R_BASE:
        return s->base;
    case 6:
    case 7:
        return s->tset[reg - 6];
    default:
        return 0;
    }
}

static void hif_host_reg_write(AtbmHeraHifState *s, unsigned reg, uint32_t val)
{
    uint32_t old = s->config;

    switch (reg) {
    case R_CONFIG:
        /* a prefetch is done as soon as it is asked for */
        s->config = val & ~(CFG_AHB_PFETCH | CFG_APB_PFETCH);
        if ((old ^ s->config) & CFG_CPU_RESET) {
            bool level = !!(s->config & CFG_CPU_RESET);

            trace_atbm_hera_hif_cpu_reset("host", level);
            qemu_set_irq(s->cpu_reset, level);
        }
        break;
    case R_CONTROL:
        s->control = (val & CTL_WUP) ? CTL_WUP | CTL_RDY
                                     : s->control & CTL_RDY;
        break;
    case R_BASE:
        s->base = val;
        break;
    case 6:
    case 7:
        s->tset[reg - 6] = val;
        break;
    default:
        break;
    }
    hif_update(s);
}

void atbm_hera_hif_host_read(AtbmHeraHifState *s, uint32_t addr, bool incr,
                             uint8_t *buf, size_t len)
{
    unsigned reg = (addr >> 2) & 7;

    if (reg == R_QUEUE) {
        hif_queue_read(s, (addr >> 6) & 0x7f, buf, len);
        return;
    }
    if (reg == R_AHB || reg == R_SRAM) {
        if (address_space_read(&address_space_memory, s->base,
                               MEMTXATTRS_UNSPECIFIED, buf, len) != MEMTX_OK) {
            memset(buf, 0, len);
        }
        return;
    }
    for (size_t i = 0; i < len; i++) {
        uint32_t a = (addr & 0x1f) + (incr ? i : 0);

        buf[i] = hif_host_reg_read(s, (a >> 2) & 7) >> (8 * (a & 3));
    }
}

void atbm_hera_hif_host_write(AtbmHeraHifState *s, uint32_t addr, bool incr,
                              const uint8_t *buf, size_t len)
{
    unsigned reg = (addr >> 2) & 7;

    if (reg == R_QUEUE) {
        hif_queue_write(s, (addr >> 6) & 0x7f, buf, len);
        return;
    }
    if (reg == R_AHB || reg == R_SRAM) {
        address_space_write(&address_space_memory, s->base,
                            MEMTXATTRS_UNSPECIFIED, buf, len);
        return;
    }
    if (len >= 4 && !(addr & 3)) {
        hif_host_reg_write(s, reg, ldl_le_p(buf));
        return;
    }
    for (size_t i = 0; i < len; i++) {
        uint32_t a = (addr & 0x1f) + (incr ? i : 0);
        unsigned r = (a >> 2) & 7, sh = 8 * (a & 3);
        uint32_t v = hif_host_reg_read(s, r);

        hif_host_reg_write(s, r, (v & ~(0xffu << sh)) | buf[i] << sh);
    }
}

static uint64_t hif_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraHifState *s = opaque;

    switch (addr) {
    case H_INT_ENABLE:
        return s->inten;
    case H_INT_RAW:
        return s->raw;
    case H_INT_CLEAR:
        return 0;
    case H_CONFIG:
        return s->config;
    case H_CONTROL:
        return s->control;
    case H_FN1_BLKSZ:
        return s->blksz;
    default:
        return s->reg[addr / 4];
    }
}

static void hif_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraHifState *s = opaque;

    switch (addr) {
    case H_INT_ENABLE:
        s->inten = val;
        break;
    case H_INT_RAW:
    case H_FN1_BLKSZ:
        break;
    case H_INT_CLEAR:
        s->raw &= ~val;
        break;
    case H_CONFIG: {
        uint32_t old = s->config;

        s->config = val;
        if ((old ^ val) & CFG_CPU_RESET) {
            trace_atbm_hera_hif_cpu_reset("chip", !!(val & CFG_CPU_RESET));
            qemu_set_irq(s->cpu_reset, !!(val & CFG_CPU_RESET));
        }
        break;
    }
    case H_CONTROL:
        s->control = val & (CTL_WUP | CTL_RDY);
        break;
    case H_SOFT_RESET:
        s->reg[addr / 4] = val;
        if (val & 1) {
            s->out_rd = 0;
            s->raw = 0;
        }
        break;
    default:
        s->reg[addr / 4] = val;
        break;
    }
    hif_update(s);
}

static const MemoryRegionOps hif_ops = {
    .read = hif_read,
    .write = hif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t port_read(void *opaque, hwaddr addr, unsigned size)
{
    AtbmHeraHifState *s = opaque;

    switch (addr) {
    case P_ADDR:
        return s->p_addr;
    case P_LEN:
        return s->p_len;
    case P_INCR:
        return s->p_incr;
    case P_IRQ:
        return atbm_hera_hif_host_irq(s);
    case P_BLKSZ:
        return s->blksz;
    case P_DATA: {
        uint32_t v = 0;

        if (!s->p_reading) {
            atbm_hera_hif_host_read(s, s->p_addr, s->p_incr, s->p_buf,
                                    MIN(s->p_len, MAX_XFER));
            s->p_reading = true;
            s->p_pos = 0;
        }
        for (int i = 0; i < 4 && s->p_pos < MIN(s->p_len, MAX_XFER); i++) {
            v |= s->p_buf[s->p_pos++] << (8 * i);
        }
        if (s->p_pos >= MIN(s->p_len, MAX_XFER)) {
            s->p_reading = false;
            s->p_pos = 0;
        }
        return v;
    }
    default:
        return 0;
    }
}

static void port_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AtbmHeraHifState *s = opaque;

    switch (addr) {
    case P_ADDR:
        s->p_addr = val & 0x1ffff;
        s->p_pos = 0;
        s->p_reading = false;
        break;
    case P_LEN:
        s->p_len = val;
        s->p_pos = 0;
        s->p_reading = false;
        break;
    case P_INCR:
        s->p_incr = val & 1;
        break;
    case P_BLKSZ:
        s->blksz = val & 0xffff;
        break;
    case P_DATA: {
        uint32_t n = MIN(s->p_len, MAX_XFER);

        for (int i = 0; i < 4 && s->p_pos < n; i++) {
            s->p_buf[s->p_pos++] = val >> (8 * i);
        }
        if (s->p_pos >= n) {
            atbm_hera_hif_host_write(s, s->p_addr, s->p_incr, s->p_buf, n);
            s->p_pos = 0;
        }
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps port_ops = {
    .read = port_read,
    .write = port_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void hif_reset_hold(Object *obj, ResetType type)
{
    AtbmHeraHifState *s = ATBM_HERA_HIF(obj);

    memset(s->reg, 0, sizeof(s->reg));
    s->inten = 0;
    s->raw = 0;
    s->config = 0;
    s->control = 0;
    s->base = 0;
    s->tset[0] = s->tset[1] = 0;
    s->out_rd = 0;
    s->p_pos = 0;
    s->p_reading = false;
    qemu_set_irq(s->cpu_reset, 0);
    hif_update(s);
}

static void hif_realize(DeviceState *dev, Error **errp)
{
    AtbmHeraHifState *s = ATBM_HERA_HIF(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &hif_ops, s,
                          TYPE_ATBM_HERA_HIF, HIF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    memory_region_init_io(&s->port, OBJECT(dev), &port_ops, s,
                          TYPE_ATBM_HERA_HIF "-host-port", PORT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->port);
}

static void hif_init(Object *obj)
{
    AtbmHeraHifState *s = ATBM_HERA_HIF(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->host_irq, "host-irq", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cpu_reset, "cpu-reset", 1);
}

static const VMStateDescription vmstate_hif = {
    .name = TYPE_ATBM_HERA_HIF,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, AtbmHeraHifState, HIF_REGS),
        VMSTATE_UINT32(inten, AtbmHeraHifState),
        VMSTATE_UINT32(raw, AtbmHeraHifState),
        VMSTATE_UINT32(config, AtbmHeraHifState),
        VMSTATE_UINT32(control, AtbmHeraHifState),
        VMSTATE_UINT32(base, AtbmHeraHifState),
        VMSTATE_UINT32_ARRAY(tset, AtbmHeraHifState, 2),
        VMSTATE_UINT32(blksz, AtbmHeraHifState),
        VMSTATE_UINT32(out_rd, AtbmHeraHifState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property hif_props[] = {
    DEFINE_PROP_BOOL("input64", AtbmHeraHifState, input64, true),
    DEFINE_PROP_UINT32("sram-base", AtbmHeraHifState, sram_base, 0x09000000),
};

static void hif_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = hif_realize;
    dc->vmsd = &vmstate_hif;
    device_class_set_props(dc, hif_props);
    rc->phases.hold = hif_reset_hold;
}

static const TypeInfo hif_info = {
    .name = TYPE_ATBM_HERA_HIF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AtbmHeraHifState),
    .instance_init = hif_init,
    .class_init = hif_class_init,
};

static void hif_register_types(void)
{
    type_register_static(&hif_info);
}

type_init(hif_register_types)
