/*
 * Ingenic A1 XGMAC Ethernet controller
 *
 * The A1 GMAC is a Synopsys XGMAC 3.1 core (user ID 0x76, Synopsys ID
 * 0x31). This models enough of it for the vendor 4.4 and mainline Linux
 * stmmac dwxgmac2 drivers and U-Boot: the version and feature registers,
 * the MDIO bus with an attached PHY, the IEEE 1588 time base, and DMA
 * channel 0 with descriptor-based TX/RX (checksum insertion, TCP
 * segmentation, header split) and interrupts.
 *
 * Measured on an A1N: VERSION, and what mainline stmmac prints at probe
 * (checksum offload both ways, advanced timestamping, TSO and header
 * split, 8 L3/L4 filters, no safety features, no flexible RX parser).
 * The remaining feature fields are the databook's defaults for those
 * options and what the drivers need.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/net/ingenic-a1-xgmac.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "system/dma.h"
#include "system/address-spaces.h"

/* Control / status registers */
#define XGMAC_VERSION           0x0110
#define XGMAC_VERSION_VALUE     0x00207631      /* A1N silicon */
#define XGMAC_HW_FEATURE4       0x012c
#define XGMAC_PACKET_FILTER     0x0008
#define XGMAC_ADDR0_HIGH        0x0300
#define XGMAC_ADDR0_LOW         0x0304
#define XGMAC_PTP_TCR           0x0d00
#define XGMAC_PTP_STSR          0x0d08
#define XGMAC_PTP_STNSR         0x0d0c
#define XGMAC_PTP_STSUR         0x0d10
#define XGMAC_PTP_STNSUR        0x0d14
#define XGMAC_PTP_TSINIT        (1U << 2)
#define XGMAC_PTP_TSUPDT        (1U << 3)
#define XGMAC_PTP_TSADDREG      (1U << 5)
#define XGMAC_PTP_TSCTRLSSR     (1U << 9)
#define XGMAC_PTP_ADDSUB        (1U << 31)
#define XGMAC_HW_FEATURE0       0x011c
#define XGMAC_HW_FEATURE1       0x0120
#define XGMAC_HW_FEATURE2       0x0124
#define XGMAC_HW_FEATURE3       0x0128
#define XGMAC_MDIO_ADDR         0x0200
#define XGMAC_MDIO_DATA         0x0204
#define XGMAC_MDIO_BUSY         (1U << 22)
#define XGMAC_MDIO_CMD_SHIFT    16
#define XGMAC_MDIO_CMD_WRITE    1
#define XGMAC_MDIO_CMD_READ     3

/* DMA block - channel 0 only (HW_FEATURE2 reports a single channel) */
#define XGMAC_DMA_MODE          0x3000
#define XGMAC_DMA_MODE_SWR      (1U << 0)
#define XGMAC_CH0_CONTROL       0x3100
#define XGMAC_CH_SPH            (1U << 24)
#define XGMAC_CH0_TX_CONTROL    0x3104
#define XGMAC_TXST              (1U << 0)
#define XGMAC_CH0_TXDESC_LADDR  0x3114
#define XGMAC_CH0_RXDESC_LADDR  0x311c
#define XGMAC_CH0_RX_CONTROL    0x3108
#define XGMAC_RXST              (1U << 0)
#define XGMAC_RX_CONFIG         0x0004
#define XGMAC_CONFIG_RE         (1U << 0)
#define XGMAC_CH0_TXDESC_TAIL   0x3124
#define XGMAC_CH0_RXDESC_TAIL   0x312c
#define XGMAC_CH0_TXDESC_RING   0x3130
#define XGMAC_CH0_RXDESC_RING   0x3134
#define XGMAC_CH0_INT_EN        0x3138
#define XGMAC_CH0_STATUS        0x3160

/* DMA channel status / interrupt-enable bits (positions align) */
#define XGMAC_INT_TI            (1U << 0)
#define XGMAC_INT_RI            (1U << 6)
#define XGMAC_INT_RBU           (1U << 7)
#define XGMAC_INT_AIS           (1U << 14)
#define XGMAC_INT_NIS           (1U << 15)

/* Descriptor bits */
#define XGMAC_TDES3_OWN         (1U << 31)
#define XGMAC_TDES3_CTXT        (1U << 30)
#define XGMAC_TDES3_FD          (1U << 29)
#define XGMAC_TDES3_LD          (1U << 28)
#define XGMAC_TDES3_TCMSSV      (1U << 26)
#define XGMAC_TDES3_TSE         (1U << 18)
#define XGMAC_TDES3_CIC_SHIFT   16
#define XGMAC_TDES3_CIC_MASK    3U
#define XGMAC_CIC_IPHDR         1
#define XGMAC_CIC_FULL          3
#define XGMAC_RDES3_OWN         (1U << 31)
#define XGMAC_RDES3_FD          (1U << 29)
#define XGMAC_RDES3_LD          (1U << 28)
#define XGMAC_RDES3_L34T_SHIFT  20
#define XGMAC_L34T_IP4TCP       0x1
#define XGMAC_L34T_IP4UDP       0x2
#define XGMAC_L34T_IP4ICMP      0x3
#define XGMAC_L34T_IP6TCP       0x9
#define XGMAC_L34T_IP6UDP       0xa
#define XGMAC_L34T_IP6ICMP      0xb
#define XGMAC_DESC_LEN_MASK     0x3fff

/* MII (PHY) register numbers */
#define MII_BMCR                0
#define MII_BMSR                1
#define MII_BMCR_RESET          (1U << 15)
#define MII_BMCR_ANRESTART      (1U << 9)

#define R(off)                  ((off) / 4)

static void xgmac_update_irq(IngenicA1XgmacState *s)
{
    uint32_t pending = s->regs[R(XGMAC_CH0_STATUS)] &
                       s->regs[R(XGMAC_CH0_INT_EN)];
    qemu_set_irq(s->irq, !!pending);
}

static void xgmac_phy_reset(IngenicA1XgmacState *s)
{
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->phy_regs[MII_BMCR]  = 0x1000;        /* autoneg enable */
    s->phy_regs[MII_BMSR]  = 0x796d;        /* link up, autoneg complete */
    s->phy_regs[2]         = 0x2000;        /* PHY ID1 */
    s->phy_regs[3]         = 0xa140;        /* PHY ID2 */
    s->phy_regs[4]         = 0x05e1;        /* ANAR */
    s->phy_regs[5]         = 0x45e1;        /* ANLPAR */
}

static void xgmac_reset_regs(IngenicA1XgmacState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    /* the MAC address registers read as an unprogrammed (invalid) address */
    s->regs[R(XGMAC_ADDR0_HIGH)] = 0x8000ffff;
    s->regs[R(XGMAC_ADDR0_LOW)] = 0xffffffff;
    s->rx_idx = 0;
    s->tx_cur = 0;
    s->tx_mss = 0;
    s->tx_frame_len = 0;
    s->ptp_base = 0;
    s->ptp_ref = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* IEEE 1588 system time: digital rollover, free running from the last load */
static uint64_t xgmac_ptp_now(IngenicA1XgmacState *s)
{
    return s->ptp_base + qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->ptp_ref;
}

static void xgmac_ptp_cmd(IngenicA1XgmacState *s, uint32_t val)
{
    uint64_t delta = s->regs[R(XGMAC_PTP_STSUR)] * NANOSECONDS_PER_SECOND +
                     (s->regs[R(XGMAC_PTP_STNSUR)] & 0x7fffffff);

    if (val & XGMAC_PTP_TSINIT) {
        s->ptp_base = delta;
    } else if (val & XGMAC_PTP_TSUPDT) {
        uint64_t now = xgmac_ptp_now(s);

        s->ptp_base = (s->regs[R(XGMAC_PTP_STNSUR)] & XGMAC_PTP_ADDSUB) ?
                      now - delta : now + delta;
    }
    s->ptp_ref = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /* the update commands self-clear when the hardware has taken them */
    s->regs[R(XGMAC_PTP_TCR)] = val & ~(XGMAC_PTP_TSINIT | XGMAC_PTP_TSUPDT |
                                        XGMAC_PTP_TSADDREG);
}

/* Frame parsing for checksum insertion, TSO and header split */
typedef struct XgmacPkt {
    unsigned l2;            /* Ethernet header with VLAN tags */
    unsigned l3;            /* IP header */
    unsigned l4;            /* TCP/UDP header, 0 for anything else */
    uint8_t proto;
    bool ipv6;
    bool frag;              /* a fragment: no L4 checksum */
    bool noL4;              /* not the first fragment: no L4 header */
    uint8_t l34t;           /* RDES3 L3/L4 type */
} XgmacPkt;

static bool xgmac_parse(const uint8_t *f, size_t len, XgmacPkt *k)
{
    unsigned type;

    memset(k, 0, sizeof(*k));
    if (len < 14) {
        return false;
    }
    k->l2 = 14;
    type = lduw_be_p(f + 12);
    if (type == 0x8100 && len >= 18) {
        k->l2 = 18;
        type = lduw_be_p(f + 16);
    }
    if (type == 0x0800 && len >= k->l2 + 20 && (f[k->l2] >> 4) == 4) {
        const uint8_t *ip = f + k->l2;
        unsigned off = lduw_be_p(ip + 6);

        k->l3 = (ip[0] & 0xf) * 4;
        if (k->l3 < 20 || k->l2 + k->l3 > len) {
            return false;
        }
        k->proto = ip[9];
        k->noL4 = (off & 0x1fff) != 0;
        k->frag = k->noL4 || (off & 0x2000);
    } else if (type == 0x86dd && len >= k->l2 + 40 &&
               (f[k->l2] >> 4) == 6) {
        k->ipv6 = true;
        k->l3 = 40;
        k->proto = f[k->l2 + 6];
    } else {
        return false;
    }

    switch (k->proto) {
    case 6:
        if (!k->noL4 && len >= k->l2 + k->l3 + 20) {
            k->l4 = (f[k->l2 + k->l3 + 12] >> 4) * 4;
            if (k->l4 < 20 || k->l2 + k->l3 + k->l4 > len) {
                k->l4 = 0;
            }
        }
        k->l34t = k->ipv6 ? XGMAC_L34T_IP6TCP : XGMAC_L34T_IP4TCP;
        break;
    case 17:
        if (!k->noL4 && len >= k->l2 + k->l3 + 8) {
            k->l4 = 8;
        }
        k->l34t = k->ipv6 ? XGMAC_L34T_IP6UDP : XGMAC_L34T_IP4UDP;
        break;
    case 1:
        k->l34t = XGMAC_L34T_IP4ICMP;
        break;
    case 58:
        k->l34t = XGMAC_L34T_IP6ICMP;
        break;
    }
    return true;
}

static uint32_t csum_sum(const uint8_t *p, size_t n, uint32_t sum)
{
    for (; n > 1; n -= 2, p += 2) {
        sum += lduw_be_p(p);
    }
    if (n) {
        sum += p[0] << 8;
    }
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum;
}

static void xgmac_ip_csum(uint8_t *f, const XgmacPkt *k)
{
    uint8_t *ip = f + k->l2;

    if (!k->ipv6) {
        stw_be_p(ip + 10, 0);
        stw_be_p(ip + 10, csum_fold(csum_sum(ip, k->l3, 0)));
    }
}

/* TCP, UDP and ICMP checksum over the whole datagram, pseudo-header included */
static void xgmac_l4_csum(uint8_t *f, size_t len, const XgmacPkt *k)
{
    uint8_t *ip = f + k->l2;
    uint8_t *l4 = ip + k->l3;
    size_t l4len, at;
    uint32_t sum = 0;
    uint16_t c;

    if (k->ipv6) {
        l4len = lduw_be_p(ip + 4);
    } else {
        l4len = lduw_be_p(ip + 2) - k->l3;
    }
    if (l4len > len - k->l2 - k->l3) {
        l4len = len - k->l2 - k->l3;
    }

    switch (k->proto) {
    case 6:
        at = 16;
        break;
    case 17:
        at = 6;
        break;
    case 1:
    case 58:
        at = 2;
        break;
    default:
        return;
    }
    if (l4len < at + 2) {
        return;
    }
    if (k->proto == 17 && !k->ipv6 && lduw_be_p(l4 + at) == 0) {
        return;     /* no UDP checksum in use */
    }
    if (k->proto != 1) {
        sum = csum_sum(ip + (k->ipv6 ? 8 : 12), k->ipv6 ? 32 : 8, 0);
        sum += k->proto == 58 ? 58 : k->proto;
        sum += l4len;
    }
    stw_be_p(l4 + at, 0);
    c = csum_fold(csum_sum(l4, l4len, sum));
    stw_be_p(l4 + at, (c == 0 && k->proto == 17) ? 0xffff : c);
}

static void xgmac_insert_csum(uint8_t *f, size_t len, unsigned cic)
{
    XgmacPkt k;

    if (!xgmac_parse(f, len, &k)) {
        return;
    }
    xgmac_ip_csum(f, &k);
    /* 2 (no pseudo-header) is not used by the drivers: handled as 3 */
    if (cic > XGMAC_CIC_IPHDR && !k.frag) {
        xgmac_l4_csum(f, len, &k);
    }
}

static void xgmac_send(IngenicA1XgmacState *s, const uint8_t *buf, size_t len)
{
    qemu_send_packet(qemu_get_queue(s->nic), buf, len);
}

/*
 * TCP segmentation: the descriptors carry one frame with all the headers
 * and the whole payload. Each segment gets the headers with the length,
 * IPv4 ID, sequence number, FIN/PSH (last segment only), CWR (first only)
 * and checksums fixed up.
 */
static void xgmac_tso(IngenicA1XgmacState *s)
{
    uint8_t *f = s->tx_frame;
    size_t len = s->tx_frame_len;
    unsigned mss = s->tx_mss;
    XgmacPkt k;
    unsigned hdr;
    size_t payload, off = 0;
    uint32_t seq;
    unsigned id = 0, n = 0;
    uint8_t *seg;

    if (!xgmac_parse(f, len, &k) || k.proto != 6 || !k.l4 || !mss) {
        xgmac_insert_csum(f, len, XGMAC_CIC_FULL);
        xgmac_send(s, f, len);
        return;
    }

    hdr = k.l2 + k.l3 + k.l4;
    payload = len - hdr;
    seq = ldl_be_p(f + k.l2 + k.l3 + 4);
    if (!k.ipv6) {
        id = lduw_be_p(f + k.l2 + 4);
    }
    seg = g_malloc(hdr + mss);

    do {
        size_t chunk = MIN(mss, payload - off);
        bool last = off + chunk >= payload;
        uint8_t *ip = seg + k.l2;
        uint8_t *tcp = ip + k.l3;

        memcpy(seg, f, hdr);
        memcpy(seg + hdr, f + hdr + off, chunk);
        if (k.ipv6) {
            stw_be_p(ip + 4, k.l4 + chunk);
        } else {
            stw_be_p(ip + 2, k.l3 + k.l4 + chunk);
            stw_be_p(ip + 4, id + n);
        }
        stl_be_p(tcp + 4, seq + off);
        if (!last) {
            tcp[13] &= ~0x09;           /* FIN, PSH */
        }
        if (off) {
            tcp[13] &= ~0x80;           /* CWR */
        }
        xgmac_insert_csum(seg, hdr + chunk, XGMAC_CIC_FULL);
        xgmac_send(s, seg, hdr + chunk);
        off += chunk;
        n++;
    } while (off < payload);

    g_free(seg);
}

static void xgmac_tx_append(IngenicA1XgmacState *s, uint32_t addr,
                            uint32_t len)
{
    if (len && s->tx_frame_len + len <= sizeof(s->tx_frame)) {
        dma_memory_read(&address_space_memory, addr & 0x1fffffff,
                        s->tx_frame + s->tx_frame_len, len,
                        MEMTXATTRS_UNSPECIFIED);
        s->tx_frame_len += len;
    }
}

static void xgmac_tx_finish(IngenicA1XgmacState *s)
{
    if (s->tx_frame_len) {
        if (s->tx_ctl & XGMAC_TDES3_TSE) {
            xgmac_tso(s);
        } else {
            unsigned cic = (s->tx_ctl >> XGMAC_TDES3_CIC_SHIFT) &
                           XGMAC_TDES3_CIC_MASK;

            if (cic) {
                xgmac_insert_csum(s->tx_frame, s->tx_frame_len, cic);
            }
            xgmac_send(s, s->tx_frame, s->tx_frame_len);
        }
    }
    s->tx_frame_len = 0;
}

/*
 * Process the TX descriptor ring for channel 0. Like the receive DMA, the
 * transmit DMA only runs between the driver's start and stop: stmmac frees
 * the ring while it resets the adapter, and the frame it allocates next
 * may live anywhere. Reading the old ring then takes whatever sits there
 * for descriptors, and clearing the OWN bit of one clears bit 31 of
 * unrelated kernel data, such as a pointer.
 */
static void xgmac_process_tx(IngenicA1XgmacState *s)
{
    uint32_t base = s->regs[R(XGMAC_CH0_TXDESC_LADDR)];
    uint32_t tail = s->regs[R(XGMAC_CH0_TXDESC_TAIL)];
    uint32_t ring_len = (s->regs[R(XGMAC_CH0_TXDESC_RING)] & 0x3ff) + 1;
    bool sent = false;

    if (!(s->regs[R(XGMAC_CH0_TX_CONTROL)] & XGMAC_TXST) ||
        !base || !ring_len) {
        return;
    }
    if (s->tx_cur < base || s->tx_cur >= base + ring_len * 16) {
        s->tx_cur = base;
    }

    for (uint32_t i = 0; i < ring_len; i++) {
        uint32_t phys = s->tx_cur & 0x1fffffff;
        uint32_t des[4];

        dma_memory_read(&address_space_memory, phys, des, sizeof(des),
                        MEMTXATTRS_UNSPECIFIED);
        if (!(des[3] & XGMAC_TDES3_OWN)) {
            break;
        }

        if (des[3] & XGMAC_TDES3_CTXT) {
            /* context descriptor: the TSO segment size (VLAN is unused) */
            if (des[3] & XGMAC_TDES3_TCMSSV) {
                s->tx_mss = des[2] & XGMAC_DESC_LEN_MASK;
            }
        } else {
            /*
             * A frame spans FD..LD descriptors (paged skbs: headers in
             * the first buffer, page fragments in the rest); each holds
             * up to two buffers, the second one addressed by TDES1 on
             * this 32-bit DMA. Sending each descriptor as its own frame
             * truncates anything multi-buffer.
             */
            if (des[3] & XGMAC_TDES3_FD) {
                s->tx_frame_len = 0;
                s->tx_ctl = des[3];
            }
            xgmac_tx_append(s, des[0], des[2] & XGMAC_DESC_LEN_MASK);
            xgmac_tx_append(s, des[1],
                            (des[2] >> 16) & XGMAC_DESC_LEN_MASK);
            if (des[3] & XGMAC_TDES3_LD) {
                xgmac_tx_finish(s);
            }
        }

        des[3] &= ~XGMAC_TDES3_OWN;
        dma_memory_write(&address_space_memory, phys, des, sizeof(des),
                         MEMTXATTRS_UNSPECIFIED);
        sent = true;

        s->tx_cur += 16;
        if (s->tx_cur >= base + ring_len * 16) {
            s->tx_cur = base;
        }
        if (s->tx_cur == tail) {
            break;
        }
    }

    if (sent) {
        s->regs[R(XGMAC_CH0_STATUS)] |= XGMAC_INT_TI | XGMAC_INT_NIS;
        xgmac_update_irq(s);
    }
}

/*
 * The receive DMA runs between the driver's start (RXST and the MAC's RE)
 * and its stop; a stopped ring may already be freed memory, as it is
 * while stmmac resets the adapter, so nothing may be written to it.
 */
static bool xgmac_rx_running(IngenicA1XgmacState *s)
{
    return (s->regs[R(XGMAC_CH0_RX_CONTROL)] & XGMAC_RXST) &&
           (s->regs[R(XGMAC_RX_CONFIG)] & XGMAC_CONFIG_RE);
}

static bool xgmac_can_receive(NetClientState *nc)
{
    return xgmac_rx_running(qemu_get_nic_opaque(nc));
}

static ssize_t xgmac_receive(NetClientState *nc, const uint8_t *buf,
                             size_t size)
{
    IngenicA1XgmacState *s = qemu_get_nic_opaque(nc);
    uint32_t ring = s->regs[R(XGMAC_CH0_RXDESC_LADDR)];
    uint32_t ring_len = (s->regs[R(XGMAC_CH0_RXDESC_RING)] & 0x3ff) + 1;

    if (!xgmac_rx_running(s)) {
        return 0;
    }
    if (!ring || size > 2048) {
        return -1;
    }

    /*
     * Consume descriptors in ring order from a tracked index, like the
     * real DMA. Scanning for the first owned descriptor instead lets
     * the model diverge from the driver's cur_rx after any refill
     * hiccup, and every frame then lands where the driver is not
     * looking until the ring happens to realign.
     */
    {
        uint32_t i = s->rx_idx % ring_len;
        uint32_t phys = (ring + i * 16) & 0x1fffffff;
        uint32_t des[4];
        XgmacPkt k;
        unsigned hlen = 0;
        uint32_t l34t = 0;

        dma_memory_read(&address_space_memory, phys, des, sizeof(des),
                        MEMTXATTRS_UNSPECIFIED);
        uint32_t bufaddr = des[0] & 0x1fffffff;
        if (!(des[3] & XGMAC_RDES3_OWN) || !bufaddr) {
            /*
             * Ring momentarily empty: ask the net layer to queue the
             * frame and pause; it is flushed from the tail pointer
             * write that hands buffers back, and from the status ack
             * and INT_EN rewrite of the NAPI cycle.
             */
            s->regs[R(XGMAC_CH0_STATUS)] |= XGMAC_INT_RBU | XGMAC_INT_AIS;
            xgmac_update_irq(s);
            return 0;
        }

        if (xgmac_parse(buf, size, &k)) {
            l34t = k.l34t;
            /* split header: TCP and UDP headers to buffer 1, rest to 2 */
            if (k.l4 && (s->regs[R(XGMAC_CH0_CONTROL)] & XGMAC_CH_SPH)) {
                hlen = k.l2 + k.l3 + k.l4;
            }
        }
        s->rx_idx = (i + 1) % ring_len;
        if (hlen) {
            dma_memory_write(&address_space_memory, bufaddr, buf, hlen,
                             MEMTXATTRS_UNSPECIFIED);
            dma_memory_write(&address_space_memory, des[2] & 0x1fffffff,
                             buf + hlen, size - hlen,
                             MEMTXATTRS_UNSPECIFIED);
        } else {
            dma_memory_write(&address_space_memory, bufaddr, buf, size,
                             MEMTXATTRS_UNSPECIFIED);
        }

        /*
         * Hand the descriptor back: clear OWN, mark first and last
         * descriptor, store the length and the header length. The stmmac
         * driver strips a 4-byte FCS from the reported length, so
         * account for it (QEMU frames carry no FCS).
         */
        des[0] = 0;
        des[1] = 0;
        des[2] = hlen;
        des[3] = XGMAC_RDES3_FD | XGMAC_RDES3_LD |
                 (l34t << XGMAC_RDES3_L34T_SHIFT) |
                 (((uint32_t)size + 4) & XGMAC_DESC_LEN_MASK);
        dma_memory_write(&address_space_memory, phys, des, sizeof(des),
                         MEMTXATTRS_UNSPECIFIED);

        s->regs[R(XGMAC_CH0_STATUS)] |= XGMAC_INT_RI | XGMAC_INT_NIS;
        xgmac_update_irq(s);
        return size;
    }
}

static void xgmac_mdio_xfer(IngenicA1XgmacState *s, uint32_t data)
{
    uint32_t addr = s->regs[R(XGMAC_MDIO_ADDR)];
    uint32_t phy = (addr >> 16) & 0x1f;
    uint32_t reg = addr & 0x1f;
    uint32_t cmd = (data >> XGMAC_MDIO_CMD_SHIFT) & 0x3;

    /* Only the PHY at MDIO address 0 is present. */
    if (phy != 0) {
        s->regs[R(XGMAC_MDIO_DATA)] = 0xffff;
        return;
    }

    if (cmd == XGMAC_MDIO_CMD_READ) {
        s->regs[R(XGMAC_MDIO_DATA)] = s->phy_regs[reg];
    } else if (cmd == XGMAC_MDIO_CMD_WRITE) {
        uint16_t val = data & 0xffff;
        switch (reg) {
        case MII_BMSR:
        case 2:
        case 3:
            break;  /* read-only */
        case MII_BMCR:
            /* reset and autoneg-restart complete immediately */
            s->phy_regs[reg] = val & ~(MII_BMCR_RESET | MII_BMCR_ANRESTART);
            break;
        default:
            s->phy_regs[reg] = val;
            break;
        }
        s->regs[R(XGMAC_MDIO_DATA)] = data & ~XGMAC_MDIO_BUSY;
    } else {
        s->regs[R(XGMAC_MDIO_DATA)] = data & ~XGMAC_MDIO_BUSY;
    }
}

static uint64_t xgmac_read(void *opaque, hwaddr offset, unsigned size)
{
    IngenicA1XgmacState *s = opaque;

    if (offset >= INGENIC_A1_XGMAC_IOSIZE) {
        return 0;
    }

    switch (offset) {
    case XGMAC_VERSION:
        return XGMAC_VERSION_VALUE;
    case XGMAC_HW_FEATURE0:
        /*
         * PHYSEL 2 and RX/TX checksum and advanced timestamp are from
         * the A1N; also SMA (MDIO), MMC counters and GMII/10/100.
         */
        return 0x01015122;
    case XGMAC_HW_FEATURE1:
        /*
         * 8 L3/L4 filters, TSO and header split (A1N); 3 = 256-bin hash
         * table, 4 KiB FIFOs, 32-bit DMA addresses.
         */
        return 0x43060145;
    case XGMAC_HW_FEATURE2:
        /* one DMA channel and one queue each way */
        return 0x00000000;
    case XGMAC_HW_FEATURE3:
    case XGMAC_HW_FEATURE4:
        /* no safety features, flexible RX parser, EST, FPE or TBS */
        return 0x00000000;
    case XGMAC_MDIO_DATA:
        /* BUSY always clear - transactions complete synchronously */
        return s->regs[R(offset)] & ~XGMAC_MDIO_BUSY;
    case XGMAC_DMA_MODE:
        /* software reset bit self-clears */
        return s->regs[R(offset)] & ~XGMAC_DMA_MODE_SWR;
    case XGMAC_PTP_STSR:
        return xgmac_ptp_now(s) / NANOSECONDS_PER_SECOND;
    case XGMAC_PTP_STNSR:
        return xgmac_ptp_now(s) % NANOSECONDS_PER_SECOND;
    default:
        return s->regs[R(offset)];
    }
}

static void xgmac_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    IngenicA1XgmacState *s = opaque;
    uint32_t val = value;

    if (offset >= INGENIC_A1_XGMAC_IOSIZE) {
        return;
    }

    switch (offset) {
    case XGMAC_CH0_TXDESC_LADDR:
        /* New ring: the DMA starts from its head. */
        s->regs[R(offset)] = val;
        s->tx_cur = 0;
        return;
    case XGMAC_CH0_RXDESC_LADDR:
        /* New ring: the DMA starts consuming from its head. */
        s->regs[R(offset)] = val;
        s->rx_idx = 0;
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    case XGMAC_MDIO_DATA:
        /* Writing the data register triggers the MDIO transaction. */
        xgmac_mdio_xfer(s, val);
        break;
    case XGMAC_CH0_STATUS:
        /* write-1-to-clear */
        s->regs[R(offset)] &= ~val;
        xgmac_update_irq(s);
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    case XGMAC_CH0_INT_EN:
        s->regs[R(offset)] = val;
        xgmac_update_irq(s);
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    case XGMAC_CH0_TXDESC_TAIL:
        s->regs[R(offset)] = val;
        xgmac_process_tx(s);
        break;
    case XGMAC_CH0_RXDESC_TAIL:
        /* buffers handed back: deliver what waited for them */
        s->regs[R(offset)] = val;
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    case XGMAC_DMA_MODE:
        if (val & XGMAC_DMA_MODE_SWR) {
            xgmac_reset_regs(s);
            xgmac_update_irq(s);
            return;
        }
        s->regs[R(offset)] = val;
        break;
    case XGMAC_PTP_TCR:
        xgmac_ptp_cmd(s, val);
        break;
    case XGMAC_CH0_RX_CONTROL:
    case XGMAC_RX_CONFIG:
        s->regs[R(offset)] = val;
        if (s->nic && xgmac_rx_running(s)) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    default:
        s->regs[R(offset)] = val;
        break;
    }
}

static const MemoryRegionOps xgmac_ops = {
    .read = xgmac_read,
    .write = xgmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

static void xgmac_reset_hold(Object *obj, ResetType type)
{
    IngenicA1XgmacState *s = INGENIC_A1_XGMAC(obj);

    xgmac_reset_regs(s);
    xgmac_phy_reset(s);
    qemu_set_irq(s->irq, 0);
}

static void xgmac_realize(DeviceState *dev, Error **errp)
{
    IngenicA1XgmacState *s = INGENIC_A1_XGMAC(dev);

    static NetClientInfo net_info = {
        .type = NET_CLIENT_DRIVER_NIC,
        .size = sizeof(NICState),
        .can_receive = xgmac_can_receive,
        .receive = xgmac_receive,
    };

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_info, &s->conf, TYPE_INGENIC_A1_XGMAC,
                          dev->id, &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void xgmac_init(Object *obj)
{
    IngenicA1XgmacState *s = INGENIC_A1_XGMAC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &xgmac_ops, s,
                          TYPE_INGENIC_A1_XGMAC, INGENIC_A1_XGMAC_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static int xgmac_post_load(void *opaque, int version_id)
{
    IngenicA1XgmacState *s = opaque;

    /* only the first 2 KiB of a frame in flight are migrated */
    if (s->tx_frame_len > 2048) {
        s->tx_frame_len = 0;
    }
    return 0;
}

static const VMStateDescription vmstate_xgmac = {
    .name = TYPE_INGENIC_A1_XGMAC,
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = xgmac_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IngenicA1XgmacState,
                             INGENIC_A1_XGMAC_IOSIZE / 4),
        VMSTATE_UINT16_ARRAY(phy_regs, IngenicA1XgmacState, 32),
        VMSTATE_UINT32(tx_cur, IngenicA1XgmacState),
        VMSTATE_UINT32_V(rx_idx, IngenicA1XgmacState, 2),
        VMSTATE_UINT32_V(tx_frame_len, IngenicA1XgmacState, 2),
        VMSTATE_SUB_ARRAY(tx_frame, IngenicA1XgmacState, 0, 2048, 2,
                          vmstate_info_uint8, uint8_t),
        VMSTATE_UINT32_V(tx_mss, IngenicA1XgmacState, 3),
        VMSTATE_UINT32_V(tx_ctl, IngenicA1XgmacState, 3),
        VMSTATE_INT64_V(ptp_ref, IngenicA1XgmacState, 3),
        VMSTATE_UINT64_V(ptp_base, IngenicA1XgmacState, 3),
        VMSTATE_END_OF_LIST()
    },
};

static const Property xgmac_props[] = {
    DEFINE_NIC_PROPERTIES(IngenicA1XgmacState, conf),
};

static void xgmac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = xgmac_realize;
    dc->vmsd = &vmstate_xgmac;
    device_class_set_props(dc, xgmac_props);
    rc->phases.hold = xgmac_reset_hold;
}

static const TypeInfo xgmac_type_info = {
    .name = TYPE_INGENIC_A1_XGMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IngenicA1XgmacState),
    .instance_init = xgmac_init,
    .class_init = xgmac_class_init,
};

static void xgmac_register_types(void)
{
    type_register_static(&xgmac_type_info);
}

type_init(xgmac_register_types)
