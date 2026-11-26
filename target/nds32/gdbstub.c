/*
 * NDS32 gdbstub
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "gdbstub/helpers.h"

/*
 * Register order of gdb/features/nds32-core.xml, which is what the
 * Andes gdb (org.gnu.gdb.nds32.core) expects: GPRs, pc, then the
 * registers it names ifc_lp, itb, ir0 (psw), the zero overhead loop
 * registers lb/le/lc (not implemented), d0lo..d1hi and system registers.
 */
static const uint16_t gdb_sr[] = {
    SR_IPSW, SR_P_IPSW, SR_IVB, SR_EVA, SR_P_EVA, SR_ITYPE, SR_P_ITYPE,
    SR_MERR, SR_IPC, SR_P_IPC, SR_OIPC, SR_P_P0, SR_P_P1, SR_INT_MASK,
    SR_INT_PEND, SR_SP_USR, SR_SP_PRIV, SR_INT_PRI, SR_INT_CTRL,
    SR_INT_MASK2, SR_INT_PEND2, SR_INT_PRI2, SR_INT_TRIGGER,
    SR_CPU_VER, SR_ICM_CFG, SR_DCM_CFG, SR_MMU_CFG, SR_MSC_CFG, SR_CORE_ID,
    SR_FUCOP_EXIST, SR_MSC_CFG2,
    SR_MMU_CTL, SR_L1_PPTB, SR_ILMB, SR_DLMB, SR_CACHE_CTL, SR_MISC_CTL,
};

#define GDB_PC      32
#define GDB_IFC_LP  33
#define GDB_ITB     34
#define GDB_PSW     35
#define GDB_LB      36
#define GDB_LC      38
#define GDB_D0LO    39
#define GDB_D1HI    42
#define GDB_SR0     43

int nds32_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n)
{
    CPUNDS32State *env = cpu_env(cs);

    if (n < 32) {
        return gdb_get_reg32(mem_buf, env->gpr[n]);
    }
    switch (n) {
    case GDB_PC:
        return gdb_get_reg32(mem_buf, env->pc);
    case GDB_IFC_LP:
        return gdb_get_reg32(mem_buf, env->ifc_lp);
    case GDB_ITB:
        return gdb_get_reg32(mem_buf, env->itb);
    case GDB_PSW:
        return gdb_get_reg32(mem_buf, env->sr[SR_PSW]);
    case GDB_LB ... GDB_LC:
        return gdb_get_reg32(mem_buf, 0);
    case GDB_D0LO ... GDB_D1HI:
        return gdb_get_reg32(mem_buf, env->d[n - GDB_D0LO]);
    default:
        if (n >= GDB_SR0 && n < GDB_SR0 + ARRAY_SIZE(gdb_sr)) {
            return gdb_get_reg32(mem_buf,
                                 nds32_mfsr(env, gdb_sr[n - GDB_SR0]));
        }
        return 0;
    }
}

int nds32_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n)
{
    CPUNDS32State *env = cpu_env(cs);
    uint32_t val = ldl_le_p(mem_buf);

    if (n < 32) {
        env->gpr[n] = val;
        return 4;
    }
    switch (n) {
    case GDB_PC:
        env->pc = val;
        break;
    case GDB_IFC_LP:
        env->ifc_lp = val;
        break;
    case GDB_ITB:
        env->itb = val;
        break;
    case GDB_PSW:
        nds32_mtsr(env, SR_PSW, val);
        break;
    case GDB_LB ... GDB_LC:
        break;
    case GDB_D0LO ... GDB_D1HI:
        env->d[n - GDB_D0LO] = val;
        break;
    default:
        if (n >= GDB_SR0 && n < GDB_SR0 + ARRAY_SIZE(gdb_sr)) {
            nds32_mtsr(env, gdb_sr[n - GDB_SR0], val);
            break;
        }
        return 0;
    }
    return 4;
}
