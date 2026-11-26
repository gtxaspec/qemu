/*
 * QEMU NDS32 CPU
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "exec/translation-block.h"
#include "accel/tcg/cpu-ops.h"
#include "hw/core/qdev-properties.h"
#include "tcg/tcg.h"

static void nds32_cpu_set_pc(CPUState *cs, vaddr value)
{
    cpu_env(cs)->pc = value;
}

static vaddr nds32_cpu_get_pc(CPUState *cs)
{
    return cpu_env(cs)->pc;
}

static TCGTBCPUState nds32_get_tb_cpu_state(CPUState *cs)
{
    CPUNDS32State *env = cpu_env(cs);

    return (TCGTBCPUState){
        .pc = env->pc,
        .flags = env->sr[SR_PSW] & (TB_FLAGS_BE | TB_FLAGS_POM),
    };
}

static void nds32_cpu_synchronize_from_tb(CPUState *cs,
                                          const TranslationBlock *tb)
{
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu_env(cs)->pc = tb->pc;
}

static void nds32_restore_state_to_opc(CPUState *cs,
                                       const TranslationBlock *tb,
                                       const uint64_t *data)
{
    cpu_env(cs)->pc = data[0];
}

#ifndef CONFIG_USER_ONLY
static bool nds32_cpu_has_work(CPUState *cs)
{
    /* standby wakes on any enabled pending interrupt, with or without GIE */
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD);
}
#endif

static int nds32_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
}

static void nds32_disas_set_info(const CPUState *cpu, disassemble_info *info)
{
    info->endian = BFD_ENDIAN_BIG;
    info->print_insn = print_insn_nds32;
}

static void nds32_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    NDS32CPU *cpu = NDS32_CPU(cs);
    NDS32CPUClass *ncc = NDS32_CPU_GET_CLASS(obj);
    CPUNDS32State *env = &cpu->env;

    if (ncc->parent_phases.hold) {
        ncc->parent_phases.hold(obj, type);
    }

    memset(env, 0, offsetof(CPUNDS32State, end_reset_fields));
    memset(env->sr, 0, sizeof(env->sr));
    env->swi_pend = false;
    memset(env->pri_at_level, 4, sizeof(env->pri_at_level));

    env->sr[SR_CPU_VER] = cpu->cpu_ver;
    env->sr[SR_ICM_CFG] = (1u << 10);       /* one ILM bank */
    env->sr[SR_DCM_CFG] = (1u << 10);       /* one DLM bank */
    env->sr[SR_MSC_CFG] = (1u << 5) | (1u << 6) | (1u << 10) | (1u << 12);
    env->sr[SR_PSW] = PSW_POM_SU;

    cs->exception_index = -1;
    nds32_update_irq(env);
}

#ifndef CONFIG_USER_ONLY
static void nds32_cpu_set_irq(void *opaque, int irq, int level)
{
    NDS32CPU *cpu = opaque;
    CPUNDS32State *env = &cpu->env;
    uint32_t bit = 1u << irq;

    if (level) {
        if (!(env->irq_lines & bit)) {
            env->irq_latch |= bit;
        }
        env->irq_lines |= bit;
    } else {
        env->irq_lines &= ~bit;
    }
    nds32_update_irq(env);
}
#endif

static void nds32_cpu_realizefn(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    NDS32CPUClass *ncc = NDS32_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_common_realize(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    ncc->parent_realize(dev, errp);
}

static void nds32_cpu_initfn(Object *obj)
{
#ifndef CONFIG_USER_ONLY
    qdev_init_gpio_in_named(DEVICE(obj), nds32_cpu_set_irq, "IRQ",
                            NDS32_NUM_IRQS);
#endif
}

static ObjectClass *nds32_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;

    typename = g_strdup_printf(NDS32_CPU_TYPE_NAME("%s"), cpu_model);
    oc = object_class_by_name(typename);
    g_free(typename);

    return oc;
}

/*
 * The AndeStar core id fields (CPUID/REV) of CPU_VER are not documented
 * for this part; the id is a best guess and CFGID advertises the
 * performance and 16-bit extensions.
 */
static void n9_initfn(Object *obj)
{
    NDS32_CPU(obj)->cpu_ver = (0x09u << 24) | 0x3;
}

static void n12_initfn(Object *obj)
{
    NDS32_CPU(obj)->cpu_ver = (0x0cu << 24) | 0x3;
}

static const Property nds32_cpu_properties[] = {
    DEFINE_PROP_UINT32("swi-vector", NDS32CPU, swi_vector, 14),
};

#ifndef CONFIG_USER_ONLY
#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps nds32_sysemu_ops = {
    .has_work = nds32_cpu_has_work,
    .translate_for_debug = nds32_cpu_translate_for_debug,
};
#endif

static const TCGCPUOps nds32_tcg_ops = {
    .guest_default_memory_order = 0,
    .mttcg_supported = false,

    .initialize = nds32_translate_init,
    .translate_code = nds32_translate_code,
    .get_tb_cpu_state = nds32_get_tb_cpu_state,
    .synchronize_from_tb = nds32_cpu_synchronize_from_tb,
    .restore_state_to_opc = nds32_restore_state_to_opc,
    .mmu_index = nds32_cpu_mmu_index,

#ifndef CONFIG_USER_ONLY
    .tlb_fill = nds32_cpu_tlb_fill,
    .pointer_wrap = cpu_pointer_wrap_uint32,
    .cpu_exec_interrupt = nds32_cpu_exec_interrupt,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = nds32_cpu_do_interrupt,
#endif
};

static void nds32_cpu_class_init(ObjectClass *oc, const void *data)
{
    NDS32CPUClass *ncc = NDS32_CPU_CLASS(oc);
    CPUClass *cc = CPU_CLASS(ncc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, nds32_cpu_realizefn,
                                    &ncc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, nds32_cpu_reset_hold, NULL,
                                       &ncc->parent_phases);
    device_class_set_props(dc, nds32_cpu_properties);

    cc->class_by_name = nds32_cpu_class_by_name;
    cc->exception_name = nds32_excp_name;
    cc->dump_state = nds32_cpu_dump_state;
    cc->set_pc = nds32_cpu_set_pc;
    cc->get_pc = nds32_cpu_get_pc;
    cc->gdb_core_xml_file = "nds32-core.xml";
    cc->gdb_read_register = nds32_cpu_gdb_read_register;
    cc->gdb_write_register = nds32_cpu_gdb_write_register;
#ifndef CONFIG_USER_ONLY
    dc->vmsd = &vmstate_nds32_cpu;
    cc->sysemu_ops = &nds32_sysemu_ops;
#endif
    cc->disas_set_info = nds32_disas_set_info;
    cc->tcg_ops = &nds32_tcg_ops;
}

#define DEFINE_NDS32_CPU_TYPE(cpu_model, initfn)    \
    {                                               \
        .parent = TYPE_NDS32_CPU,                   \
        .instance_init = initfn,                    \
        .name = NDS32_CPU_TYPE_NAME(cpu_model),     \
    }

static const TypeInfo nds32_cpus_type_infos[] = {
    { /* base class should be registered first */
        .name = TYPE_NDS32_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(NDS32CPU),
        .instance_align = __alignof(NDS32CPU),
        .instance_init = nds32_cpu_initfn,
        .abstract = true,
        .class_size = sizeof(NDS32CPUClass),
        .class_init = nds32_cpu_class_init,
    },
    DEFINE_NDS32_CPU_TYPE("n9", n9_initfn),
    DEFINE_NDS32_CPU_TYPE("n12", n12_initfn),
};

DEFINE_TYPES(nds32_cpus_type_infos)
