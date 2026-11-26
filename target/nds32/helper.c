/*
 * NDS32 system registers, exceptions and the internal interrupt controller
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "qemu/plugin.h"
#include "qemu/main-loop.h"

const char *nds32_excp_name(int excp)
{
    static const char * const names[] = {
        [NDS32_VEC_RESET] = "reset",
        [NDS32_VEC_TLB_FILL] = "TLB fill",
        [NDS32_VEC_PTE_NP] = "PTE not present",
        [NDS32_VEC_TLB_MISC] = "TLB misc",
        [NDS32_VEC_TLB_VLPT] = "TLB VLPT miss",
        [NDS32_VEC_MACH_ERR] = "machine error",
        [NDS32_VEC_DEBUG] = "debug",
        [NDS32_VEC_GENERAL] = "general exception",
        [NDS32_VEC_SYSCALL] = "syscall",
    };

    if (excp >= NDS32_VEC_HW0) {
        return "interrupt";
    }
    return excp >= 0 && excp < ARRAY_SIZE(names) ? names[excp] : "unknown";
}

/* Writable bits of the registers that are not plain storage */
static uint32_t sr_wmask(uint32_t idx)
{
    switch (idx) {
    case SR_CPU_VER:
    case SR_CORE_ID:
    case SR_ICM_CFG:
    case SR_DCM_CFG:
    case SR_MMU_CFG:
    case SR_MSC_CFG:
    case SR_MSC_CFG2:
    case SR_FUCOP_EXIST:
        return 0;
    case SR_PSW:
    case SR_IPSW:
    case SR_P_IPSW:
        return PSW_WMASK;
    case SR_IVB:
        return IVB_WMASK;
    default:
        return ~0u;
    }
}

static unsigned nds32_pri_of(CPUNDS32State *env, int src)
{
    if (!(env->sr[SR_IVB] & IVB_PPL)) {
        return 0;
    }
    if (src < 16) {
        return (env->sr[SR_INT_PRI] >> (src * 2)) & 3;
    }
    return (env->sr[SR_INT_PRI2] >> ((src - 16) * 2)) & 3;
}

/* Pending and enabled interrupt sources, bit n = source n */
static uint32_t nds32_irq_sources(CPUNDS32State *env)
{
    uint32_t trig = env->sr[SR_INT_TRIGGER];
    uint32_t pend = (env->irq_lines & ~trig) | (env->irq_latch & trig);
    uint32_t src = pend & env->sr[SR_INT_MASK2];

    if (env->swi_pend && (env->sr[SR_INT_MASK] & INT_MASK_SIM)) {
        src |= 1u << 14;
    }
    return src;
}

bool nds32_irq_pending(CPUNDS32State *env)
{
    return nds32_irq_sources(env) != 0;
}

void nds32_update_irq(CPUNDS32State *env)
{
    CPUState *cs = env_cpu(env);
    bool locked = bql_locked();

    /* called from helpers as well as from device code */
    if (!locked) {
        bql_lock();
    }
    if (nds32_irq_sources(env)) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
    if (!locked) {
        bql_unlock();
    }
}

/* Pick the interrupt to take now, or -1 */
static int nds32_irq_select(CPUNDS32State *env)
{
    uint32_t src = nds32_irq_sources(env);
    uint32_t psw = env->sr[SR_PSW];
    unsigned level = (psw & PSW_INTL_MASK) >> PSW_INTL_SHIFT;
    unsigned cur = level ? env->pri_at_level[level] : 4;
    unsigned best = 4;
    int pick = -1;
    int i;

    if (!(psw & PSW_GIE)) {
        return -1;
    }
    for (i = 0; i < NDS32_NUM_IRQS; i++) {
        if (src & (1u << i)) {
            unsigned pri = nds32_pri_of(env, i);

            if (pri < best && pri < cur) {
                best = pri;
                pick = i;
            }
        }
    }
    return pick;
}

uint32_t nds32_mfsr(CPUNDS32State *env, uint32_t idx)
{
    switch (idx) {
    case SR_INT_PEND:
        return env->swi_pend ? 1u << 16 : 0;
    case SR_INT_PEND2: {
        uint32_t trig = env->sr[SR_INT_TRIGGER];

        return (env->irq_lines & ~trig) | (env->irq_latch & trig);
    }
    default:
        return env->sr[idx & (NDS32_SR_COUNT - 1)];
    }
}

void nds32_mtsr(CPUNDS32State *env, uint32_t idx, uint32_t val)
{
    uint32_t mask;

    idx &= NDS32_SR_COUNT - 1;
    switch (idx) {
    case SR_INT_PEND:
        env->swi_pend = (val >> 16) & 1;
        break;
    case SR_INT_PEND2:
        env->irq_latch &= ~val;
        break;
    default:
        mask = sr_wmask(idx);
        env->sr[idx] = (env->sr[idx] & ~mask) | (val & mask);
        break;
    }
    nds32_update_irq(env);
}

static G_NORETURN void raise_exc(CPUNDS32State *env, uint32_t vec,
                                 uint32_t etype, uint32_t swid, uintptr_t ra)
{
    CPUState *cs = env_cpu(env);

    env->excp_etype = etype;
    env->excp_swid = swid;
    cs->exception_index = vec;
    cpu_loop_exit_restore(cs, ra);
}

G_NORETURN void HELPER(raise_exception)(CPUNDS32State *env, uint32_t vec,
                                        uint32_t etype, uint32_t swid)
{
    raise_exc(env, vec, etype, swid, GETPC());
}

uint32_t HELPER(mfsr)(CPUNDS32State *env, uint32_t idx)
{
    return nds32_mfsr(env, idx);
}

void HELPER(mtsr)(CPUNDS32State *env, uint32_t idx, uint32_t val)
{
    nds32_mtsr(env, idx, val);
}

void HELPER(setgie)(CPUNDS32State *env, uint32_t enable)
{
    if (enable) {
        env->sr[SR_PSW] |= PSW_GIE;
    } else {
        env->sr[SR_PSW] &= ~PSW_GIE;
    }
    nds32_update_irq(env);
}

void HELPER(div0)(CPUNDS32State *env)
{
    if (env->sr[SR_INT_MASK] & INT_MASK_IDIVZE) {
        raise_exc(env, NDS32_VEC_GENERAL, ETYPE_ARITH, 0, GETPC());
    }
}

void HELPER(iret)(CPUNDS32State *env)
{
    uint32_t psw = env->sr[SR_PSW];
    unsigned level = (psw & PSW_INTL_MASK) >> PSW_INTL_SHIFT;

    switch (level) {
    case 2:
        env->pc = env->sr[SR_P_IPC];
        env->sr[SR_PSW] = env->sr[SR_P_IPSW];
        break;
    case 3:
        env->pc = env->sr[SR_OIPC];
        env->sr[SR_PSW] = (psw & ~PSW_INTL_MASK) | (2u << PSW_INTL_SHIFT);
        break;
    default:
        env->pc = env->sr[SR_IPC];
        env->sr[SR_PSW] = env->sr[SR_IPSW];
        break;
    }
    nds32_update_irq(env);
}

G_NORETURN void HELPER(standby)(CPUNDS32State *env)
{
    CPUState *cs = env_cpu(env);

    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}

bool nds32_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr)
{
    hwaddr phys = address & 0xffffffffu;

    tlb_set_page(cs, address & TARGET_PAGE_MASK, phys & TARGET_PAGE_MASK,
                 PAGE_READ | PAGE_WRITE | PAGE_EXEC, mmu_idx,
                 TARGET_PAGE_SIZE);
    return true;
}

bool nds32_cpu_translate_for_debug(CPUState *cs, vaddr addr,
                                   TranslateForDebugResult *result)
{
    *result = (TranslateForDebugResult) {
        .physaddr = addr & 0xffffffffu,
        .lg_page_size = TARGET_PAGE_BITS,
        .attrs.debug = 1,
    };
    return true;
}

void nds32_cpu_do_interrupt(CPUState *cs)
{
    CPUNDS32State *env = cpu_env(cs);
    NDS32CPU *cpu = env_archcpu(env);
    int vec = cs->exception_index;
    uint32_t psw = env->sr[SR_PSW];
    unsigned level = (psw & PSW_INTL_MASK) >> PSW_INTL_SHIFT;
    uint32_t itype = 0;
    uint32_t ret_pc = env->pc;
    uint32_t esz, base, new_psw;
    uint64_t last_pc = env->pc;
    unsigned pri = 4;
    bool is_irq = vec >= NDS32_VEC_HW0;

    if (vec == NDS32_VEC_GENERAL) {
        itype = env->excp_etype | (env->excp_inst ? 1u << 4 : 0);
    } else if (vec == NDS32_VEC_SYSCALL) {
        itype = env->excp_swid << 16;
    }
    if (vec == NDS32_VEC_GENERAL && env->excp_etype == ETYPE_TRAP) {
        itype |= env->excp_swid << 16;
    }

    if (is_irq) {
        int src = vec - NDS32_VEC_HW0;

        pri = nds32_pri_of(env, src == cpu->swi_vector ? 14 : src);
    }

    switch (level) {
    case 0:
        env->sr[SR_IPC] = ret_pc;
        env->sr[SR_IPSW] = psw;
        env->sr[SR_ITYPE] = itype;
        env->sr[SR_EVA] = env->excp_eva;
        break;
    case 1:
        env->sr[SR_P_IPC] = ret_pc;
        env->sr[SR_P_IPSW] = psw;
        env->sr[SR_P_ITYPE] = itype;
        env->sr[SR_P_EVA] = env->excp_eva;
        break;
    default:
        env->sr[SR_OIPC] = ret_pc;
        break;
    }

    new_psw = psw & ~(PSW_GIE | PSW_IFCON | PSW_IT | PSW_DT | PSW_POM_MASK |
                      PSW_HSS | PSW_DEX);
    new_psw |= PSW_POM_SU;
    if (level < 3) {
        new_psw = (new_psw & ~PSW_INTL_MASK) |
                  ((level + 1) << PSW_INTL_SHIFT);
    }
    env->sr[SR_PSW] = new_psw;
    env->pri_at_level[MIN(level + 1, 3)] = pri;

    esz = 4u << (2 * ((env->sr[SR_IVB] >> IVB_ESZ_SHIFT) & 3));
    base = env->sr[SR_IVB] & IVB_BASE_MASK;
    env->pc = base + vec * esz;

    qemu_log_mask(CPU_LOG_INT, "NDS32 %s vec %d ipc %08x -> %08x\n",
                  nds32_excp_name(vec), vec, ret_pc, env->pc);

    if (is_irq) {
        qemu_plugin_vcpu_interrupt_cb(cs, last_pc);
    } else {
        qemu_plugin_vcpu_exception_cb(cs, last_pc);
    }

    cs->exception_index = -1;
    nds32_update_irq(env);
}

bool nds32_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUNDS32State *env = cpu_env(cs);
    NDS32CPU *cpu = env_archcpu(env);
    int src;

    if (!(interrupt_request & CPU_INTERRUPT_HARD)) {
        return false;
    }
    src = nds32_irq_select(env);
    if (src < 0) {
        return false;
    }
    /* The software interrupt shares the vector of its configured source */
    if (src == 14 && env->swi_pend && (env->sr[SR_INT_MASK] & INT_MASK_SIM) &&
        !(env->irq_lines & env->sr[SR_INT_MASK2] & (1u << 14))) {
        src = cpu->swi_vector;
    }
    cs->exception_index = NDS32_VEC_HW0 + src;
    nds32_cpu_do_interrupt(cs);
    return true;
}
