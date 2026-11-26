/*
 * NDS32 CPU migration state
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "migration/vmstate.h"

const VMStateDescription vmstate_nds32_cpu = {
    .name = "cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(env.gpr, NDS32CPU, 32),
        VMSTATE_UINT32(env.pc, NDS32CPU),
        VMSTATE_UINT32_ARRAY(env.d, NDS32CPU, 4),
        VMSTATE_UINT32(env.itb, NDS32CPU),
        VMSTATE_UINT32(env.ifc_lp, NDS32CPU),
        VMSTATE_UINT32(env.ov, NDS32CPU),
        VMSTATE_UINT32(env.lock_addr, NDS32CPU),
        VMSTATE_UINT32(env.lock_val, NDS32CPU),
        VMSTATE_BOOL(env.lock_valid, NDS32CPU),
        VMSTATE_UINT32_ARRAY(env.sr, NDS32CPU, NDS32_SR_COUNT),
        VMSTATE_UINT32(env.irq_lines, NDS32CPU),
        VMSTATE_UINT32(env.irq_latch, NDS32CPU),
        VMSTATE_BOOL(env.swi_pend, NDS32CPU),
        VMSTATE_UINT8_ARRAY(env.pri_at_level, NDS32CPU, 4),
        VMSTATE_END_OF_LIST()
    }
};
