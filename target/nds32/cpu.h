/*
 * Andes NDS32 (AndeStar V3) CPU definitions
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NDS32_CPU_H
#define NDS32_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"

#define NDS32_SR_COUNT 1024

/* System register index: group << 7 | number << 3 | extension */
#define NDS32_SR(grp, num, ext) (((grp) << 7) | ((num) << 3) | (ext))

enum {
    SR_CPU_VER = NDS32_SR(0, 0, 0),
    SR_CORE_ID = NDS32_SR(0, 0, 1),
    SR_ICM_CFG = NDS32_SR(0, 1, 0),
    SR_DCM_CFG = NDS32_SR(0, 2, 0),
    SR_MMU_CFG = NDS32_SR(0, 3, 0),
    SR_MSC_CFG = NDS32_SR(0, 4, 0),
    SR_MSC_CFG2 = NDS32_SR(0, 4, 1),
    SR_FUCOP_EXIST = NDS32_SR(0, 5, 0),

    SR_PSW = NDS32_SR(1, 0, 0),
    SR_IPSW = NDS32_SR(1, 0, 1),
    SR_P_IPSW = NDS32_SR(1, 0, 2),
    SR_IVB = NDS32_SR(1, 1, 1),
    SR_INT_CTRL = NDS32_SR(1, 1, 2),
    SR_INT_GPR_PUSH_DIS = NDS32_SR(1, 1, 3),
    SR_EVA = NDS32_SR(1, 2, 1),
    SR_P_EVA = NDS32_SR(1, 2, 2),
    SR_ITYPE = NDS32_SR(1, 3, 1),
    SR_P_ITYPE = NDS32_SR(1, 3, 2),
    SR_MERR = NDS32_SR(1, 4, 1),
    SR_IPC = NDS32_SR(1, 5, 1),
    SR_P_IPC = NDS32_SR(1, 5, 2),
    SR_OIPC = NDS32_SR(1, 5, 3),
    SR_P_P0 = NDS32_SR(1, 6, 2),
    SR_P_P1 = NDS32_SR(1, 7, 2),
    SR_INT_MASK = NDS32_SR(1, 8, 0),
    SR_INT_MASK2 = NDS32_SR(1, 8, 1),
    SR_INT_PEND = NDS32_SR(1, 9, 0),
    SR_INT_PEND2 = NDS32_SR(1, 9, 1),
    SR_INT_TRIGGER = NDS32_SR(1, 9, 4),
    SR_SP_USR = NDS32_SR(1, 10, 0),
    SR_SP_PRIV = NDS32_SR(1, 10, 1),
    SR_INT_PRI = NDS32_SR(1, 11, 0),
    SR_INT_PRI2 = NDS32_SR(1, 11, 1),

    SR_MMU_CTL = NDS32_SR(2, 0, 0),
    SR_L1_PPTB = NDS32_SR(2, 1, 0),
    SR_ILMB = NDS32_SR(2, 6, 0),
    SR_DLMB = NDS32_SR(2, 7, 0),
    SR_CACHE_CTL = NDS32_SR(2, 8, 0),
    SR_MISC_CTL = NDS32_SR(2, 15, 1),
};

/* PSW fields */
#define PSW_GIE        (1u << 0)
#define PSW_INTL_SHIFT 1
#define PSW_INTL_MASK  (3u << PSW_INTL_SHIFT)
#define PSW_POM_SHIFT  3
#define PSW_POM_MASK   (3u << PSW_POM_SHIFT)
#define PSW_BE         (1u << 5)
#define PSW_IT         (1u << 6)
#define PSW_DT         (1u << 7)
#define PSW_DEX        (1u << 10)
#define PSW_HSS        (1u << 11)
#define PSW_IFCON      (1u << 15)
#define PSW_WMASK      0x0007ffffu

/* POM: 0 user, 1 superuser */
#define PSW_POM_SU     (1u << PSW_POM_SHIFT)

/* IVB fields */
#define IVB_PPL        (1u << 0)
#define IVB_NIVIC_MASK (7u << 1)
#define IVB_EVIC       (1u << 13)
/*
 * IVIC_VER, read-only: 1 is the controller with $INT_MASK2/$INT_PRI2,
 * which this core's interrupt enables are in; firmware tests bit 11 to
 * know whether to save and restore them (CPU_RETENTION_FUNC)
 */
#define IVB_IVIC_VER1  (1u << 11)
#define IVB_ESZ_SHIFT  14
#define IVB_BASE_MASK  0xffff0000u
#define IVB_WMASK      (IVB_BASE_MASK | (3u << IVB_ESZ_SHIFT) | IVB_EVIC | \
                        IVB_NIVIC_MASK | IVB_PPL)

/* INT_MASK bits */
#define INT_MASK_SIM   (1u << 16)
#define INT_MASK_IDIVZE (1u << 30)

/* Exception vectors */
#define NDS32_VEC_RESET     0
#define NDS32_VEC_TLB_FILL  1
#define NDS32_VEC_PTE_NP    2
#define NDS32_VEC_TLB_MISC  3
#define NDS32_VEC_TLB_VLPT  4
#define NDS32_VEC_MACH_ERR  5
#define NDS32_VEC_DEBUG     6
#define NDS32_VEC_GENERAL   7
#define NDS32_VEC_SYSCALL   8
#define NDS32_VEC_HW0       9
#define NDS32_NUM_IRQS      32

/* ITYPE.ETYPE of general exceptions */
#define ETYPE_ALIGN         0
#define ETYPE_RESERVED_INSN 1
#define ETYPE_TRAP          2
#define ETYPE_ARITH         3
#define ETYPE_PRECISE_BUS   4
#define ETYPE_IMPRECISE_BUS 5
#define ETYPE_COPROC        6
#define ETYPE_PRIV_INSN     7
#define ETYPE_RESERVED_VAL  8
#define ETYPE_NO_LOCAL_MEM  9

/* usr register indexes (instruction field value) */
#define USR_D0_LO   0x000
#define USR_D0_HI   0x020
#define USR_D1_LO   0x040
#define USR_D1_HI   0x060
#define USR_ITB     0x380
#define USR_IFC_LP  0x3a0
#define USR_PC      0x3e0

struct NDS32CPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

typedef struct CPUArchState {
    uint32_t gpr[32];
    uint32_t pc;
    uint32_t d[4];              /* d0.lo d0.hi d1.lo d1.hi */
    uint32_t itb;
    uint32_t ifc_lp;
    uint32_t ov;

    uint32_t excp_etype;
    uint32_t excp_swid;
    uint32_t excp_eva;
    bool excp_inst;

    uint32_t lock_addr;
    uint32_t lock_val;
    bool lock_valid;

    /* Fields up to here are cleared on reset. */
    struct {} end_reset_fields;

    uint32_t sr[NDS32_SR_COUNT];

    /* IVIC state */
    uint32_t irq_lines;         /* level of the 32 external inputs */
    uint32_t irq_latch;         /* latched edges of edge-triggered inputs */
    bool swi_pend;
    uint8_t pri_at_level[4];    /* priority being serviced, per INTL */
} CPUNDS32State;

/**
 * NDS32CPU:
 * @env: #CPUNDS32State
 *
 * An NDS32 CPU.
 */
struct ArchCPU {
    CPUState parent_obj;

    CPUNDS32State env;

    uint32_t cpu_ver;
    uint32_t swi_vector;
#ifndef CONFIG_USER_ONLY
    /* pulsed as the core enters standby: what a power controller watches */
    qemu_irq standby;
    /* the power controller keeps the core in standby, whatever is pending */
    bool power_hold;
#endif
};

#define CPU_RESOLVING_TYPE TYPE_NDS32_CPU

#define TB_FLAGS_BE     PSW_BE
#define TB_FLAGS_POM    PSW_POM_MASK

void nds32_cpu_dump_state(CPUState *cs, FILE *f, int flags);
int nds32_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg);
int nds32_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg);
void nds32_translate_init(void);
void nds32_translate_code(CPUState *cs, TranslationBlock *tb,
                          int *max_insns, vaddr pc, void *host_pc);
const char *nds32_excp_name(int excp);
int print_insn_nds32(bfd_vma addr, disassemble_info *info);

uint32_t nds32_mfsr(CPUNDS32State *env, uint32_t idx);
void nds32_mtsr(CPUNDS32State *env, uint32_t idx, uint32_t val);
void nds32_update_irq(CPUNDS32State *env);
bool nds32_irq_pending(CPUNDS32State *env);

#ifndef CONFIG_USER_ONLY
extern const VMStateDescription vmstate_nds32_cpu;

void nds32_cpu_do_interrupt(CPUState *cs);
bool nds32_cpu_exec_interrupt(CPUState *cs, int interrupt_request);
G_NORETURN void nds32_cpu_do_unaligned_access(CPUState *cs, vaddr addr,
                                              MMUAccessType access_type,
                                              int mmu_idx, uintptr_t retaddr);
bool nds32_cpu_translate_for_debug(CPUState *cs, vaddr addr,
                                   TranslateForDebugResult *result);
bool nds32_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr);
#endif

#endif /* NDS32_CPU_H */
