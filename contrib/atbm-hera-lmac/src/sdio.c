/*
 * lmac_qemu: what the SDIO demo asks of the lower MAC directly
 *
 * In the SDIO build the camera's driver sends its data frames as WSM
 * transmit requests over SDIO, and the demo passes each straight to the
 * LMAC (HI_SDIO_TxReq -> WSM_TransmitReq), not through HMAC. Its own
 * object, so only a firmware that calls it links hi_sdio.o's functions.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <string.h>
#include "lmac_qemu.h"

/* the SDIO input buffers' payload size (hi_sdio.o: 0x668 less 8) */
#define SDIO_IN_SIZE        1632

/* struct wsm_tx: the frame after 24 bytes, 2 more with this flag */
#define TX_FLAGS            11
#define TX_FRAME            24
#define TX_TWO_BYTE_OFFSET  0x80
#define FC_PROTECTED        0x4000
#define CCMP_MIC            8

/*
 * The camera's driver (hal_apollo txrx.c atbm_tx_h_crypt) marks a frame
 * protected and makes room for the CCMP header only: the chip adds the
 * MIC. The port takes an encrypted frame with room for both, as the
 * other chips' hosts give it.
 */
static void tx_mic_room(uint8_t *msg)
{
    unsigned len = msg[0] | msg[1] << 8;
    unsigned off = TX_FRAME + (msg[TX_FLAGS] & TX_TWO_BYTE_OFFSET ? 2 : 0);

    if (len < off + 2 || len + CCMP_MIC > SDIO_IN_SIZE ||
        !((msg[off] | msg[off + 1] << 8) & FC_PROTECTED)) {
        return;
    }
    memset(msg + len, 0, CCMP_MIC);
    len += CCMP_MIC;
    msg[0] = len & 0xff;
    msg[1] = len >> 8;
}

void WSM_TransmitReq(void *msg)
{
    tx_mic_room(msg);
    if (lmac_qemu_request(msg, SDIO_IN_SIZE)) {
        HiSdio_ProgOutput(msg);
    } else {
        HiSdio_ProgInput(msg);
    }
}

/* the LMAC's 802.11 status codes map to WSM ones; the model has only 0 */
uint32_t Get_WSM_Status(uint32_t status)
{
    return status;
}

/*
 * the receive indication, after its header: status, channel, rate, RSSI,
 * flags, then the frame
 */
#define RX_STATUS           4
#define RX_FRAME            20
#define RX_COPY_ID          0x0814
#define MIB_FLAG_ALL_TO_CHIP (1u << 19)

static uint16_t ld16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint16_t be16(const uint8_t *p)
{
    return p[0] << 8 | p[1];
}

/* Both: a copy for the host (as the vendor LMAC sends it), then HMAC */
static int rx_copy(uint8_t *msg)
{
    uint8_t *c = HOST_SDIO_INIT_DONE() ? lmac_qemu_rx_buf() : 0;
    unsigned len = ld16(msg);

    if (c) {
        memcpy(c, msg, len);
        c[2] = RX_COPY_ID & 0xff;
        c[3] = RX_COPY_ID >> 8;
        HiSdio_ProgOutput(c);
    }
    return LMAC_QEMU_RX_HMAC;
}

static int rx_host(uint8_t *msg)
{
    HiSdio_ProgOutput(msg);
    return LMAC_QEMU_RX_TAKEN;
}

static int rx_ip(uint8_t *msg, uint8_t *ip, int v6)
{
    unsigned proto = v6 ? ip[6] : ip[9];
    uint8_t *l4 = ip + (v6 ? 40 : 4 * (ip[0] & 0xf));
    uint16_t frag = v6 ? 0 : be16(ip + 6);
    int r;

    if (proto == 6) {
        r = atbmwifi_tcpfilter(ld16(l4 + 2), ld16(l4), l4, ip, v6);
        return r == 1 ? rx_host(msg) : r == -1 ? LMAC_QEMU_RX_DROP
                                               : LMAC_QEMU_RX_HMAC;
    }
    if (proto == 17) {
        if (atbmwifi_dnsfilter(ld16(l4 + 2), ld16(l4), v6)) {
            return rx_copy(msg);
        }
        r = atbmwifi_udpfilter(ld16(l4 + 2), ld16(l4), l4, ip, v6);
        if (r == 1) {
            if (frag && test_match_frag_identifier(ld16(ip + 4))) {
                return LMAC_QEMU_RX_HMAC;
            }
            return rx_host(msg);
        }
        if (r == -1) {
            return LMAC_QEMU_RX_DROP;
        }
        if (HOST_SDIO_INIT_DONE() && (frag & 0x3fff)) {
            set_udp_frag_identifier(ld16(l4 + 2), ld16(l4), ld16(ip + 4));
        }
        return LMAC_QEMU_RX_HMAC;
    }
    if (HOST_SDIO_can_wakeup_host() &&
        HI_SDIO_Support_OtherIP_In_Sleep() == 0) {
        return LMAC_QEMU_RX_DROP;
    }
    if (proto == (v6 ? 58 : 1) && atbmwifi_icmpfilter(v6)) {
        return rx_copy(msg);
    }
    return LMAC_QEMU_RX_HMAC;
}

/*
 * The received frame split of the SDIO build's LMAC: data frames for the
 * camera go to it over SDIO, the rest to HMAC (the chip's own network
 * stack), by the filters the application provides (user_main.c). The
 * order and the filters' meaning (1 the host, -1 drop, 0 the chip) are
 * the vendor LMACtoUMAC_ReceiveFrame's.
 */
int lmac_qemu_rx_route(void *m)
{
    uint8_t *msg = m, *f = msg + RX_FRAME, *llc;
    uint16_t fc = ld16(f), type;
    unsigned hdr = 24;

    if (ld16(msg) < RX_FRAME + 24 + 8 || msg[RX_STATUS] ||
        (MIB_Flags & MIB_FLAG_ALL_TO_CHIP) || (fc & 0x0c) != 0x08) {
        return LMAC_QEMU_RX_HMAC;
    }
    if (fc & 0x80) {
        hdr += 2;
    }
    if (fc & 0x4000) {
        hdr += 8;           /* the CCMP header the model leaves in */
    }
    llc = f + hdr;
    if (llc + 8 > msg + ld16(msg)) {
        return LMAC_QEMU_RX_HMAC;
    }
    type = be16(llc + 6);
    if (type == 0x0800 || type == 0x86dd) {
        return rx_ip(msg, llc + 8, type == 0x86dd);
    }
    if (type == 0x0806 && atbmwifi_arpfilter()) {
        return rx_copy(msg);
    }
    return LMAC_QEMU_RX_HMAC;
}
