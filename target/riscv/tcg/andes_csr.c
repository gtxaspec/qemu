/*
 * Andes AndeStar V5 custom CSRs, as the ATBM6461's core uses them
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "accel/tcg/cpu-ops.h"
#include "exec/tb-flush.h"
#include "target/riscv/tcg/csr.h"

#define CSR_ANDES_UITB   0x800
/* micm_cfg, mdcm_cfg, mmsc_cfg, mmsc_cfg2 */
#define CSR_ANDES_CFG0   0xfc0

/* 0x7c0 to 0x7df, uitb, the four read-only config CSRs */
static int andes_idx(int csrno)
{
    if (csrno >= 0x7c0 && csrno <= 0x7df) {
        return csrno - 0x7c0;
    }
    if (csrno == CSR_ANDES_UITB) {
        return 32;
    }
    return 33 + csrno - CSR_ANDES_CFG0;
}

static RISCVException any(CPURISCVState *env, int csrno)
{
    return RISCV_EXCP_NONE;
}

static RISCVException read_andes(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->andes_csr[andes_idx(csrno)];
    return RISCV_EXCP_NONE;
}

static RISCVException write_andes(CPURISCVState *env, int csrno,
                                  target_ulong val, uintptr_t ra)
{
    int idx = andes_idx(csrno);

    /* EXEC.IT is translated against the table, so a new one voids the TBs */
    if (csrno == CSR_ANDES_UITB && env->andes_csr[idx] != val) {
        queue_tb_flush(env_cpu(env));
    }
    env->andes_csr[idx] = val;
    return RISCV_EXCP_NONE;
}

static RISCVException write_ignore(CPURISCVState *env, int csrno,
                                   target_ulong val, uintptr_t ra)
{
    qemu_log_mask(LOG_UNIMP, "andes: write to read-only CSR 0x%x\n", csrno);
    return RISCV_EXCP_NONE;
}

#define ANDES_RW(n) \
    { .csrno = n, .csr_ops = { "andes", any, read_andes, write_andes } }
#define ANDES_RO(n) \
    { .csrno = n, .csr_ops = { "andes", any, read_andes, write_ignore } }

const RISCVCSR andes_csr_list[] = {
    ANDES_RW(0x7c0), ANDES_RW(0x7c1), ANDES_RW(0x7c2), ANDES_RW(0x7c3),
    ANDES_RW(0x7c4), ANDES_RW(0x7c5), ANDES_RW(0x7c6), ANDES_RW(0x7c7),
    ANDES_RW(0x7c8), ANDES_RW(0x7c9), ANDES_RW(0x7ca), ANDES_RW(0x7cb),
    ANDES_RW(0x7cc), ANDES_RW(0x7cd), ANDES_RW(0x7ce), ANDES_RW(0x7cf),
    ANDES_RW(0x7d0), ANDES_RW(0x7d1), ANDES_RW(0x7d2), ANDES_RW(0x7d3),
    ANDES_RW(0x7d4), ANDES_RW(0x7d5), ANDES_RW(0x7d6), ANDES_RW(0x7d7),
    ANDES_RW(0x7d8), ANDES_RW(0x7d9), ANDES_RW(0x7da), ANDES_RW(0x7db),
    ANDES_RW(0x7dc), ANDES_RW(0x7dd), ANDES_RW(0x7de), ANDES_RW(0x7df),
    ANDES_RW(CSR_ANDES_UITB),
    ANDES_RO(0xfc0), ANDES_RO(0xfc1), ANDES_RO(0xfc2), ANDES_RO(0xfc3),
    { }
};
