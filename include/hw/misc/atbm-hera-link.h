/*
 * ATBM6441 "Hera": the link between a camera SoC's QEMU and the chip's
 *
 * The chip runs in qemu-system-nds32 (machine atbm-hera), the camera in
 * another QEMU (qemu-system-mipsel), joined by a UNIX socket. The
 * camera's side owns the SDIO host and puts an "atbm-hera-link-sdio" card
 * on its bus; the chip's side is the "atbm-hera-link" device of the Hera
 * board, which serves what the card forwards from the chip's HIF block and
 * raises the SDIO interrupt back. The chip is the server: start it first.
 * The two sides also trade the levels of the lines the chip's pads and the
 * SoC's GPIOs share, and the SoC powers the chip and holds it in reset.
 * The UART between them is not on this socket: it is a plain chardev
 * socket from the SoC's UART to the chip's UART1.
 *
 * Messages are four little-endian words, a payload of the length in c
 * following DATA and SDIO_WRITE. The chip opens with HELLO and the SoC
 * answers it; the chip then sends IRQ and a PAD for each pad it drives.
 * Every SDIO_READ is answered by one DATA and every SDIO_WRITE by one ACK,
 * and the SoC's side waits for it; PAD, IRQ and the chip's messages that
 * the request caused arrive in order before the answer, so a request
 * returns with the interrupt line as the request left it. SDIO_CFG,
 * SDIO_RESET, PAD, POWER and RESET are not answered.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ATBM_HERA_LINK_H
#define HW_MISC_ATBM_HERA_LINK_H

#define ATBM_HERA_LINK_VERSION  1

/* The chip's pads, as the GPIO block numbers them */
#define ATBM_HERA_LINK_PADS     28

/* the largest transfer a request carries */
#define ATBM_HERA_LINK_MAX_XFER (1024 * 1024)

/* in c of SDIO_READ and SDIO_WRITE with the length: the address counts up */
#define ATBM_HERA_LINK_INCR     0x80000000u

enum {
    /* chip to SoC (HELLO both ways) */
    AHL_HELLO = 1,          /* c: link version */
    AHL_DATA,               /* c: length, the payload follows */
    AHL_ACK,                /* the answer to an SDIO_WRITE */
    AHL_IRQ,                /* c: level of the SDIO interrupt to the host */
    /* SoC to chip */
    AHL_SDIO_READ,          /* a: function, b: address, c: length | INCR */
    AHL_SDIO_WRITE,         /* the same, the payload follows */
    AHL_SDIO_CFG,           /* a: CCCR or FBR address, c: the byte written */
    AHL_SDIO_RESET,         /* the bus's I/O reset (CCCR ABORT RES) */
    AHL_POWER,              /* c: 1 the chip is powered, 0 it is off */
    AHL_RESET,              /* c: 1 the reset pin is held, 0 it is let go */
    /* both ways */
    AHL_PAD,                /* a: pad number, c: the level the sender drives */
};

typedef struct AtbmHeraLinkMsg {
    uint32_t type;
    uint32_t a;
    uint32_t b;
    uint32_t c;
} AtbmHeraLinkMsg;

#endif /* HW_MISC_ATBM_HERA_LINK_H */
