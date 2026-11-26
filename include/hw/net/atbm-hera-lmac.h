/*
 * ATBM6441 (Hera) paravirtual LMAC port
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Not a block of the chip. The Hera's Wi-Fi stack runs on its own CPU in
 * two halves: the upper MAC (HMAC, the supplicant, the network stack) and
 * the lower MAC (LMAC), which drives the radio's undocumented MAC and PHY.
 * They trade the same WSM messages a Linux host trades with the other
 * ATBM60xx chips' firmware. A firmware linked with a small replacement
 * for the LMAC (outside QEMU) hands each WSM request to this port, and
 * this port answers with the WSM firmware model the ATBM60xx front ends
 * share (atbm-wifi.c), joined to the simulated access point (wifi-ap.c).
 *
 * Registers are 32 bits wide, little endian. Messages in guest memory are
 * the WSM ones: { le16 len; le16 id; body[len - 4] }, len counting the
 * header. Ids carry a sequence number in bits 15:13, which the port
 * ignores on requests and sets on what it gives back.
 */

#ifndef HW_NET_ATBM_HERA_LMAC_H
#define HW_NET_ATBM_HERA_LMAC_H

#define TYPE_ATBM_HERA_LMAC         "atbm-hera-lmac"

/* where the Hera board maps the port, and its interrupt */
#define ATBM_HERA_LMAC_BASE         0x0a580000
#define ATBM_HERA_LMAC_SIZE         0x1000
/* the MAC's "NTD" interrupt, which nothing else uses once the LMAC is gone */
#define ATBM_HERA_LMAC_IRQ          0

/* reads ATBM_HERA_LMAC_MAGIC ("QLMC") and the ABI version */
#define ATBM_HERA_LMAC_ID           0x00
#define ATBM_HERA_LMAC_VERSION      0x04
#define ATBM_HERA_LMAC_MAGIC        0x434d4c51
#define ATBM_HERA_LMAC_ABI          1

/*
 * CTRL: RUN set starts the firmware model, which gives a startup
 * indication shortly after; RUN cleared stops it and drops whatever it
 * had for the guest (the reset value)
 */
#define ATBM_HERA_LMAC_CTRL         0x08
#define ATBM_HERA_LMAC_CTRL_RUN     (1u << 0)

/* STATUS: a message waits (bit 0), and its length (31:16) */
#define ATBM_HERA_LMAC_STATUS       0x0c
#define ATBM_HERA_LMAC_STATUS_PEND  (1u << 0)

/* IRQ_EN bit 0: the interrupt follows STATUS_PEND (level) */
#define ATBM_HERA_LMAC_IRQ_EN       0x10

/*
 * A request: REQ_SIZE is the size of the buffer it sits in, and writing
 * its address to REQ hands it over; the port reads it then and is done
 * with it on return. If the request had an answer (its confirm, id
 * (id & 0x3ff) | 0x400), the confirm is written over the request in that
 * buffer, and REQ_RESULT reads 1 | its length << 16; else 0. A transmit
 * request is answered only if it asks for a confirm.
 */
#define ATBM_HERA_LMAC_REQ_SIZE     0x14
#define ATBM_HERA_LMAC_REQ          0x18
#define ATBM_HERA_LMAC_REQ_RESULT   0x1c
#define ATBM_HERA_LMAC_RESULT_CNF   (1u << 0)

/*
 * Indications (startup, received frames, scan complete, ...): with a
 * buffer of IND_SIZE bytes at IND_ADDR, a read of IND_POP writes the next
 * message there and reads its length, or 0 if none waits. A message
 * longer than the buffer is dropped and counted in IND_DROPPED.
 */
#define ATBM_HERA_LMAC_IND_ADDR     0x20
#define ATBM_HERA_LMAC_IND_SIZE     0x24
#define ATBM_HERA_LMAC_IND_POP      0x28
#define ATBM_HERA_LMAC_IND_DROPPED  0x2c

/* the station's MAC address the model gives (bytes 0-3, then 4-5) */
#define ATBM_HERA_LMAC_MAC_LO       0x30
#define ATBM_HERA_LMAC_MAC_HI       0x34

#endif
