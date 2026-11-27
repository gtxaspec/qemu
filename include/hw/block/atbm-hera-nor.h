/*
 * The SPI NOR flash inside an ATBM6441 "Hera"
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_BLOCK_ATBM_HERA_NOR_H
#define HW_BLOCK_ATBM_HERA_NOR_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_ATBM_HERA_NOR "atbm-hera-nor"
OBJECT_DECLARE_SIMPLE_TYPE(AtbmHeraNorState, ATBM_HERA_NOR)

struct AtbmHeraNorState {
    /*< private >*/
    SSIPeripheral parent_obj;
    /*< public >*/

    /* the array as a RAM region, for the board to alias as XIP windows */
    MemoryRegion array;
    uint8_t *data;
    uint8_t *pristine;          /* "restore-on-reset": the array as loaded */
    bool restore_on_reset;
    BlockBackend *blk;
    QEMUTimer *timer;

    uint32_t size;
    uint32_t jedec_id;

    uint8_t sr1;
    uint8_t sr2;
    uint8_t sr2_reset;
    uint8_t new_sr1;
    uint8_t new_sr2;
    uint8_t op;
    int32_t state;
    int32_t after_addr;
    uint32_t addr;
    uint32_t naddr;
    uint32_t dummy;
    uint32_t nbytes;
    uint32_t prog_lo;
    uint32_t prog_hi;
    bool deep_pd;
    qemu_irq powered_down;
    bool wsr_volatile;
};

#endif
