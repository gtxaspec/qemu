/*
 * Ingenic A1 XGMAC Ethernet controller
 *
 * The A1 GMAC is a Synopsys XGMAC 3.1 core (VERSION 0x00207631 on the
 * A1N): MDIO at 0x200, PTP at 0xd00, DMA block at 0x3000, version and
 * feature registers at 0x110-0x12c.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_INGENIC_A1_XGMAC_H
#define HW_NET_INGENIC_A1_XGMAC_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_INGENIC_A1_XGMAC "ingenic-a1-xgmac"
OBJECT_DECLARE_SIMPLE_TYPE(IngenicA1XgmacState, INGENIC_A1_XGMAC)

#define INGENIC_A1_XGMAC_IOSIZE  0x4000
#define INGENIC_A1_XGMAC_TXBUF   (64 * 1024 + 256)

struct IngenicA1XgmacState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    NICState *nic;
    NICConf conf;

    uint32_t regs[INGENIC_A1_XGMAC_IOSIZE / 4];
    uint32_t rx_idx;
    uint32_t tx_frame_len;
    uint8_t tx_frame[INGENIC_A1_XGMAC_TXBUF];
    uint16_t phy_regs[32];
    uint32_t tx_cur;
    uint32_t tx_mss;
    uint32_t tx_ctl;        /* TDES3 of the frame's first descriptor */
    int64_t ptp_ref;        /* virtual clock at the last time load, ns */
    uint64_t ptp_base;      /* system time at ptp_ref, ns */
};

#endif /* HW_NET_INGENIC_A1_XGMAC_H */
