/*
 * ATBM6441 "Hera" HIF: the SDIO slave's host interface block
 *
 * The block has two faces. The chip's CPU sees descriptor rings and
 * interrupt registers at 0x0ab00000 (MMIO region 0). The SDIO host sees
 * function 1: eight 32-bit registers, a message queue and two windows onto
 * the chip's memory. The host face is this interface, for a model of the
 * host's end to call; an SDIO card (hw/sd/atbm-hera-sdio.c) is one, and
 * a link to a separate machine that owns the SDIO host would be another.
 *
 * Function 1 addresses, as the host driver builds them (17 bits):
 *   [4:2]  the register: 0 CONFIG, 1 CONTROL, 2 QUEUE, 3 AHB window,
 *          4 window base address, 5 SRAM window, 6 and 7 test registers
 *   [1:0]  the byte of the register a CMD52 touches
 *   [5]    force, [12:6] the buffer number: a write to QUEUE is the buffer
 *          (input descriptor) it names, a read is output descriptor
 *          number - 1 (the driver counts 1 to 4)
 *
 * Lines: "host-irq" (out) is the SDIO interrupt to the host, "cpu-reset"
 * (out) is the host holding the chip's CPU in reset through CONFIG.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ATBM_HERA_HIF_H
#define HW_MISC_ATBM_HERA_HIF_H

#include "qom/object.h"

#define TYPE_ATBM_HERA_HIF "atbm-hera-hif"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraHifState, ATBM_HERA_HIF)

/* A function 1 read or write, the way an SDIO card model hands it over */
void atbm_hera_hif_host_read(AtbmHeraHifState *s, uint32_t addr, bool incr,
                             uint8_t *buf, size_t len);
void atbm_hera_hif_host_write(AtbmHeraHifState *s, uint32_t addr, bool incr,
                              const uint8_t *buf, size_t len);

/* The level of the interrupt to the host */
bool atbm_hera_hif_host_irq(AtbmHeraHifState *s);

#endif
