/*
 * NDS32 (AndeStar V3) translation
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "qemu/log.h"
#include "qemu/qemu-print.h"
#include "qemu/bitops.h"
#include "exec/translator.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/log.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef  HELPER_H

/* is_jmp field values */
#define DISAS_EXIT    DISAS_TARGET_0  /* exit to the main loop, pc is stored */

typedef struct DisasContext {
    DisasContextBase base;
    uint32_t tb_flags;
    int ilen;
    bool be;
    bool user;
} DisasContext;

static TCGv_i32 cpu_gpr[32];
static TCGv_i32 cpu_pc;
static TCGv_i32 cpu_psw;
static TCGv_i32 cpu_ifc_lp;
static TCGv_i32 cpu_itb;
static TCGv_i32 cpu_d[4];
static TCGv_i32 cpu_ov;
static TCGv_i32 cpu_lock_addr;
static TCGv_i32 cpu_lock_val;
static TCGv_i32 cpu_lock_valid;

/* The 4-bit register field of the 16-bit forms skips r12-r15 */
static inline int r45(int n)
{
    return n < 12 ? n : n + 4;
}

#include "decode-insn32.c.inc"
#include "decode-insn16.c.inc"

void nds32_translate_init(void)
{
    static const char * const regnames[32] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
        "r24", "r25", "r26", "r27", "fp", "gp", "lp", "sp",
    };
    static const char * const dnames[4] = {
        "d0.lo", "d0.hi", "d1.lo", "d1.hi",
    };
    int i;

    for (i = 0; i < 32; i++) {
        cpu_gpr[i] = tcg_global_mem_new_i32(tcg_env,
                                            offsetof(CPUNDS32State, gpr[i]),
                                            regnames[i]);
    }
    for (i = 0; i < 4; i++) {
        cpu_d[i] = tcg_global_mem_new_i32(tcg_env,
                                          offsetof(CPUNDS32State, d[i]),
                                          dnames[i]);
    }
    cpu_pc = tcg_global_mem_new_i32(tcg_env, offsetof(CPUNDS32State, pc),
                                    "pc");
    cpu_psw = tcg_global_mem_new_i32(tcg_env,
                                     offsetof(CPUNDS32State, sr[SR_PSW]),
                                     "psw");
    cpu_ifc_lp = tcg_global_mem_new_i32(tcg_env,
                                        offsetof(CPUNDS32State, ifc_lp),
                                        "ifc_lp");
    cpu_itb = tcg_global_mem_new_i32(tcg_env, offsetof(CPUNDS32State, itb),
                                     "itb");
    cpu_ov = tcg_global_mem_new_i32(tcg_env, offsetof(CPUNDS32State, ov),
                                    "ov");
    cpu_lock_addr = tcg_global_mem_new_i32(tcg_env,
                                           offsetof(CPUNDS32State, lock_addr),
                                           "lock_addr");
    cpu_lock_val = tcg_global_mem_new_i32(tcg_env,
                                          offsetof(CPUNDS32State, lock_val),
                                          "lock_val");
    cpu_lock_valid = tcg_global_mem_new_i32(tcg_env,
                                            offsetof(CPUNDS32State,
                                                     lock_valid),
                                            "lock_valid");
}

static inline uint32_t cur_pc(DisasContext *ctx)
{
    return ctx->base.pc_next;
}

static inline uint32_t next_pc(DisasContext *ctx)
{
    return ctx->base.pc_next + ctx->ilen;
}

static MemOp mo_data(DisasContext *ctx)
{
    return ctx->be ? MO_BE : MO_LE;
}

static void gen_raise(DisasContext *ctx, uint32_t vec, uint32_t etype,
                      uint32_t swid)
{
    tcg_gen_movi_i32(cpu_pc, cur_pc(ctx));
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(vec),
                               tcg_constant_i32(etype),
                               tcg_constant_i32(swid));
    ctx->base.is_jmp = DISAS_NORETURN;
}

static bool priv_check(DisasContext *ctx)
{
    if (ctx->user) {
        gen_raise(ctx, NDS32_VEC_GENERAL, ETYPE_PRIV_INSN, 0);
        return false;
    }
    return true;
}

static void gen_goto_tb(DisasContext *ctx, int n, uint32_t dest)
{
    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(n);
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, n);
    } else {
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
}

static bool gen_jump(DisasContext *ctx, uint32_t dest)
{
    gen_goto_tb(ctx, 0, dest);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_jump_reg(DisasContext *ctx, TCGv_i32 target)
{
    tcg_gen_mov_i32(cpu_pc, target);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_bcond(DisasContext *ctx, TCGCond cond, TCGv_i32 a, TCGv_i32 b,
                      int32_t off)
{
    TCGLabel *taken = gen_new_label();

    tcg_gen_brcond_i32(cond, a, b, taken);
    gen_goto_tb(ctx, 0, next_pc(ctx));
    gen_set_label(taken);
    gen_goto_tb(ctx, 1, cur_pc(ctx) + off);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_bcondi(DisasContext *ctx, TCGCond cond, TCGv_i32 a, int32_t b,
                       int32_t off)
{
    TCGLabel *taken = gen_new_label();

    tcg_gen_brcondi_i32(cond, a, b, taken);
    gen_goto_tb(ctx, 0, next_pc(ctx));
    gen_set_label(taken);
    gen_goto_tb(ctx, 1, cur_pc(ctx) + off);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* ---------------- ALU ---------------- */

typedef void (*gen_bin_fn)(TCGv_i32, TCGv_i32, TCGv_i32);

static bool alu_rrr(arg_r3 *a, gen_bin_fn fn)
{
    fn(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool alu_shifted(arg_r3s *a, gen_bin_fn fn, bool left)
{
    TCGv_i32 t = tcg_temp_new_i32();

    if (left) {
        tcg_gen_shli_i32(t, cpu_gpr[a->rb], a->sh);
    } else {
        tcg_gen_shri_i32(t, cpu_gpr[a->rb], a->sh);
    }
    fn(cpu_gpr[a->rt], cpu_gpr[a->ra], t);
    return true;
}

static bool trans_add_slli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_add_i32, true);
}

static bool trans_sub_slli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_sub_i32, true);
}

static bool trans_and_slli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_and_i32, true);
}

static bool trans_xor_slli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_xor_i32, true);
}

static bool trans_or_slli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_or_i32, true);
}

static bool trans_add_srli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_add_i32, false);
}

static bool trans_sub_srli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_sub_i32, false);
}

static bool trans_and_srli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_and_i32, false);
}

static bool trans_xor_srli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_xor_i32, false);
}

static bool trans_or_srli(DisasContext *ctx, arg_r3s *a)
{
    return alu_shifted(a, tcg_gen_or_i32, false);
}

static bool trans_nor(DisasContext *ctx, arg_r3 *a)
{
    return alu_rrr(a, tcg_gen_nor_i32);
}

static bool trans_slt(DisasContext *ctx, arg_r3 *a)
{
    tcg_gen_setcond_i32(TCG_COND_LTU, cpu_gpr[a->rt], cpu_gpr[a->ra],
                        cpu_gpr[a->rb]);
    return true;
}

static bool trans_slts(DisasContext *ctx, arg_r3 *a)
{
    tcg_gen_setcond_i32(TCG_COND_LT, cpu_gpr[a->rt], cpu_gpr[a->ra],
                        cpu_gpr[a->rb]);
    return true;
}

static void gen_shift_reg(TCGv_i32 d, TCGv_i32 x, TCGv_i32 n,
                          gen_bin_fn fn)
{
    TCGv_i32 t = tcg_temp_new_i32();

    tcg_gen_andi_i32(t, n, 31);
    fn(d, x, t);
}

static bool trans_sll(DisasContext *ctx, arg_r3 *a)
{
    gen_shift_reg(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb],
                  tcg_gen_shl_i32);
    return true;
}

static bool trans_srl(DisasContext *ctx, arg_r3 *a)
{
    gen_shift_reg(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb],
                  tcg_gen_shr_i32);
    return true;
}

static bool trans_sra(DisasContext *ctx, arg_r3 *a)
{
    gen_shift_reg(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb],
                  tcg_gen_sar_i32);
    return true;
}

static bool trans_rotr(DisasContext *ctx, arg_r3 *a)
{
    gen_shift_reg(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb],
                  tcg_gen_rotr_i32);
    return true;
}

static bool trans_slli(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_shli_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_srli(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_shri_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_srai(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_sari_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_rotri(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_rotri_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_seb(DisasContext *ctx, arg_r2 *a)
{
    tcg_gen_ext8s_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_seh(DisasContext *ctx, arg_r2 *a)
{
    tcg_gen_ext16s_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_zeh(DisasContext *ctx, arg_r2 *a)
{
    tcg_gen_ext16u_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_wsbh(DisasContext *ctx, arg_r2 *a)
{
    TCGv_i32 t = tcg_temp_new_i32();
    TCGv_i32 u = tcg_temp_new_i32();

    tcg_gen_andi_i32(t, cpu_gpr[a->ra], 0x00ff00ff);
    tcg_gen_shli_i32(t, t, 8);
    tcg_gen_shri_i32(u, cpu_gpr[a->ra], 8);
    tcg_gen_andi_i32(u, u, 0x00ff00ff);
    tcg_gen_or_i32(cpu_gpr[a->rt], t, u);
    return true;
}

static bool trans_bitc(DisasContext *ctx, arg_r3 *a)
{
    tcg_gen_andc_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_cmovz(DisasContext *ctx, arg_r3 *a)
{
    tcg_gen_movcond_i32(TCG_COND_EQ, cpu_gpr[a->rt], cpu_gpr[a->rb],
                        tcg_constant_i32(0), cpu_gpr[a->ra], cpu_gpr[a->rt]);
    return true;
}

static bool trans_cmovn(DisasContext *ctx, arg_r3 *a)
{
    tcg_gen_movcond_i32(TCG_COND_NE, cpu_gpr[a->rt], cpu_gpr[a->rb],
                        tcg_constant_i32(0), cpu_gpr[a->ra], cpu_gpr[a->rt]);
    return true;
}

/* Signed overflow of a +/- b as 0/1 */
static void gen_sv(TCGv_i32 d, TCGv_i32 x, TCGv_i32 y, bool sub)
{
    TCGv_i64 p = tcg_temp_new_i64();
    TCGv_i64 q = tcg_temp_new_i64();
    TCGv_i64 s = tcg_temp_new_i64();

    tcg_gen_ext_i32_i64(p, x);
    tcg_gen_ext_i32_i64(q, y);
    if (sub) {
        tcg_gen_sub_i64(s, p, q);
    } else {
        tcg_gen_add_i64(s, p, q);
    }
    tcg_gen_ext32s_i64(p, s);
    tcg_gen_setcond_i64(TCG_COND_NE, s, p, s);
    tcg_gen_extrl_i64_i32(d, s);
}

static bool trans_sva(DisasContext *ctx, arg_r3 *a)
{
    gen_sv(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb], false);
    return true;
}

static bool trans_svs(DisasContext *ctx, arg_r3 *a)
{
    gen_sv(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb], true);
    return true;
}

static bool trans_max(DisasContext *ctx, arg_r3 *a)
{
    tcg_gen_smax_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_min(DisasContext *ctx, arg_r3 *a)
{
    tcg_gen_smin_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_ave(DisasContext *ctx, arg_r3 *a)
{
    TCGv_i64 p = tcg_temp_new_i64();
    TCGv_i64 q = tcg_temp_new_i64();

    tcg_gen_ext_i32_i64(p, cpu_gpr[a->ra]);
    tcg_gen_ext_i32_i64(q, cpu_gpr[a->rb]);
    tcg_gen_add_i64(p, p, q);
    tcg_gen_addi_i64(p, p, 1);
    tcg_gen_sari_i64(p, p, 1);
    tcg_gen_extrl_i64_i32(cpu_gpr[a->rt], p);
    return true;
}

static bool trans_abs(DisasContext *ctx, arg_r2 *a)
{
    TCGv_i32 t = tcg_temp_new_i32();

    /* abs saturates: |INT_MIN| is INT_MAX */
    tcg_gen_abs_i32(t, cpu_gpr[a->ra]);
    tcg_gen_movcond_i32(TCG_COND_EQ, cpu_gpr[a->rt], t,
                        tcg_constant_i32(INT32_MIN),
                        tcg_constant_i32(INT32_MAX), t);
    return true;
}

static bool trans_clz(DisasContext *ctx, arg_r2 *a)
{
    tcg_gen_clzi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], 32);
    return true;
}

static bool trans_clo(DisasContext *ctx, arg_r2 *a)
{
    TCGv_i32 t = tcg_temp_new_i32();

    tcg_gen_not_i32(t, cpu_gpr[a->ra]);
    tcg_gen_clzi_i32(cpu_gpr[a->rt], t, 32);
    return true;
}

/* clips: signed saturation to imm+1 bits, clip: [0, 2^imm - 1] */
static void gen_clip(TCGv_i32 d, TCGv_i32 x, int imm, bool is_signed)
{
    TCGv_i32 lo = tcg_constant_i32(is_signed ? -(1ll << imm) : 0);
    TCGv_i32 hi = tcg_constant_i32((1ll << imm) - 1);
    TCGv_i32 t = tcg_temp_new_i32();
    TCGv_i32 sat = tcg_temp_new_i32();

    tcg_gen_smax_i32(t, x, lo);
    tcg_gen_smin_i32(t, t, hi);
    tcg_gen_setcond_i32(TCG_COND_NE, sat, t, x);
    tcg_gen_or_i32(cpu_ov, cpu_ov, sat);
    tcg_gen_mov_i32(d, t);
}

static bool trans_clips(DisasContext *ctx, arg_rri *a)
{
    gen_clip(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm, true);
    return true;
}

static bool trans_clip(DisasContext *ctx, arg_rri *a)
{
    gen_clip(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm, false);
    return true;
}

static bool trans_bset(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_ori_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], 1u << a->imm);
    return true;
}

static bool trans_bclr(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_andi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], ~(1u << a->imm));
    return true;
}

static bool trans_btgl(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_xori_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], 1u << a->imm);
    return true;
}

static bool trans_btst(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_extract_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm, 1);
    return true;
}

/* ---------------- multiply / divide ---------------- */

static bool trans_mul(DisasContext *ctx, arg_r3 *a)
{
    return alu_rrr(a, tcg_gen_mul_i32);
}

static TCGv_i64 d_get(int dt)
{
    TCGv_i64 t = tcg_temp_new_i64();

    tcg_gen_concat_i32_i64(t, cpu_d[dt * 2], cpu_d[dt * 2 + 1]);
    return t;
}

static void d_put(int dt, TCGv_i64 v)
{
    tcg_gen_extr_i64_i32(cpu_d[dt * 2], cpu_d[dt * 2 + 1], v);
}

static bool gen_mul64(arg_dt3 *a, bool is_signed, int acc)
{
    TCGv_i32 lo = tcg_temp_new_i32();
    TCGv_i32 hi = tcg_temp_new_i32();
    TCGv_i64 p = tcg_temp_new_i64();

    if (is_signed) {
        tcg_gen_muls2_i32(lo, hi, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    } else {
        tcg_gen_mulu2_i32(lo, hi, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    }
    tcg_gen_concat_i32_i64(p, lo, hi);
    if (acc > 0) {
        tcg_gen_add_i64(p, d_get(a->dt), p);
    } else if (acc < 0) {
        tcg_gen_sub_i64(p, d_get(a->dt), p);
    }
    d_put(a->dt, p);
    return true;
}

static bool trans_mults64(DisasContext *ctx, arg_dt3 *a)
{
    return gen_mul64(a, true, 0);
}

static bool trans_mult64(DisasContext *ctx, arg_dt3 *a)
{
    return gen_mul64(a, false, 0);
}

static bool trans_madds64(DisasContext *ctx, arg_dt3 *a)
{
    return gen_mul64(a, true, 1);
}

static bool trans_madd64(DisasContext *ctx, arg_dt3 *a)
{
    return gen_mul64(a, false, 1);
}

static bool trans_msubs64(DisasContext *ctx, arg_dt3 *a)
{
    return gen_mul64(a, true, -1);
}

static bool trans_msub64(DisasContext *ctx, arg_dt3 *a)
{
    return gen_mul64(a, false, -1);
}

static bool trans_mult32(DisasContext *ctx, arg_dt3 *a)
{
    tcg_gen_mul_i32(cpu_d[a->dt * 2], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_madd32(DisasContext *ctx, arg_dt3 *a)
{
    TCGv_i32 t = tcg_temp_new_i32();

    tcg_gen_mul_i32(t, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    tcg_gen_add_i32(cpu_d[a->dt * 2], cpu_d[a->dt * 2], t);
    return true;
}

static bool trans_msub32(DisasContext *ctx, arg_dt3 *a)
{
    TCGv_i32 t = tcg_temp_new_i32();

    tcg_gen_mul_i32(t, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    tcg_gen_sub_i32(cpu_d[a->dt * 2], cpu_d[a->dt * 2], t);
    return true;
}

static bool gen_mulr64(arg_r3 *a, bool is_signed)
{
    TCGv_i32 lo = tcg_temp_new_i32();
    TCGv_i32 hi = tcg_temp_new_i32();
    int base = a->rt & ~1;

    if (is_signed) {
        tcg_gen_muls2_i32(lo, hi, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    } else {
        tcg_gen_mulu2_i32(lo, hi, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    }
    tcg_gen_mov_i32(cpu_gpr[base], lo);
    tcg_gen_mov_i32(cpu_gpr[base + 1], hi);
    return true;
}

static bool trans_mulsr64(DisasContext *ctx, arg_r3 *a)
{
    return gen_mulr64(a, true);
}

static bool trans_mulr64(DisasContext *ctx, arg_r3 *a)
{
    return gen_mulr64(a, false);
}

static bool trans_maddr32(DisasContext *ctx, arg_r3 *a)
{
    TCGv_i32 t = tcg_temp_new_i32();

    tcg_gen_mul_i32(t, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    tcg_gen_add_i32(cpu_gpr[a->rt], cpu_gpr[a->rt], t);
    return true;
}

static bool trans_msubr32(DisasContext *ctx, arg_r3 *a)
{
    TCGv_i32 t = tcg_temp_new_i32();

    tcg_gen_mul_i32(t, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    tcg_gen_sub_i32(cpu_gpr[a->rt], cpu_gpr[a->rt], t);
    return true;
}

/* Quotient in q, remainder in r; division by zero yields 0 and 0 */
static void gen_divide(TCGv_i32 q, TCGv_i32 r, TCGv_i32 x, TCGv_i32 y,
                       bool is_signed)
{
    TCGv_i32 nq = tcg_temp_new_i32();
    TCGv_i32 nr = tcg_temp_new_i32();
    TCGv_i32 den = tcg_temp_new_i32();
    TCGv_i32 zero = tcg_constant_i32(0);
    TCGv_i32 one = tcg_constant_i32(1);
    TCGv_i32 t0 = tcg_temp_new_i32();
    TCGv_i32 t1 = tcg_temp_new_i32();
    TCGLabel *nz = gen_new_label();
    TCGLabel *done = gen_new_label();

    tcg_gen_brcondi_i32(TCG_COND_NE, y, 0, nz);
    gen_helper_div0(tcg_env);
    tcg_gen_movi_i32(nr, 0);
    tcg_gen_movi_i32(nq, 0);
    tcg_gen_br(done);

    gen_set_label(nz);
    if (is_signed) {
        /* INT_MIN / -1 would trap the host: divide by 1 instead */
        tcg_gen_setcondi_i32(TCG_COND_EQ, t0, x, INT32_MIN);
        tcg_gen_setcondi_i32(TCG_COND_EQ, t1, y, -1);
        tcg_gen_and_i32(t0, t0, t1);
        tcg_gen_movcond_i32(TCG_COND_NE, den, t0, zero, one, y);
        tcg_gen_div_i32(nq, x, den);
        tcg_gen_rem_i32(nr, x, den);
    } else {
        tcg_gen_divu_i32(nq, x, y);
        tcg_gen_remu_i32(nr, x, y);
    }
    gen_set_label(done);
    /* the quotient is written last: it wins when both are the same register */
    tcg_gen_mov_i32(r, nr);
    tcg_gen_mov_i32(q, nq);
}

static bool trans_divsr(DisasContext *ctx, arg_r4 *a)
{
    gen_divide(cpu_gpr[a->rt], cpu_gpr[a->rd], cpu_gpr[a->ra],
               cpu_gpr[a->rb], true);
    return true;
}

static bool trans_divr(DisasContext *ctx, arg_r4 *a)
{
    gen_divide(cpu_gpr[a->rt], cpu_gpr[a->rd], cpu_gpr[a->ra],
               cpu_gpr[a->rb], false);
    return true;
}

static bool trans_divs(DisasContext *ctx, arg_dt3 *a)
{
    gen_divide(cpu_d[a->dt * 2], cpu_d[a->dt * 2 + 1], cpu_gpr[a->ra],
               cpu_gpr[a->rb], true);
    return true;
}

static bool trans_div(DisasContext *ctx, arg_dt3 *a)
{
    gen_divide(cpu_d[a->dt * 2], cpu_d[a->dt * 2 + 1], cpu_gpr[a->ra],
               cpu_gpr[a->rb], false);
    return true;
}

static bool trans_ffb(DisasContext *ctx, arg_r3 *a)
{
    gen_helper_ffb(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_ffbi(DisasContext *ctx, arg_rri *a)
{
    gen_helper_ffb(cpu_gpr[a->rt], cpu_gpr[a->ra], tcg_constant_i32(a->imm));
    return true;
}

static bool trans_ffmism(DisasContext *ctx, arg_r3 *a)
{
    gen_helper_ffmism(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_flmism(DisasContext *ctx, arg_r3 *a)
{
    gen_helper_flmism(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_ffzmism(DisasContext *ctx, arg_r3 *a)
{
    gen_helper_ffzmism(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_pbsad(DisasContext *ctx, arg_r3 *a)
{
    gen_helper_pbsad(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_pbsada(DisasContext *ctx, arg_r3 *a)
{
    TCGv_i32 t = tcg_temp_new_i32();

    gen_helper_pbsad(t, cpu_gpr[a->ra], cpu_gpr[a->rb]);
    tcg_gen_add_i32(cpu_gpr[a->rt], cpu_gpr[a->rt], t);
    return true;
}

/* ---------------- ALU with immediates ---------------- */

static bool trans_movi(DisasContext *ctx, arg_ri *a)
{
    tcg_gen_movi_i32(cpu_gpr[a->rt], a->imm);
    return true;
}

static bool trans_sethi(DisasContext *ctx, arg_ri *a)
{
    tcg_gen_movi_i32(cpu_gpr[a->rt], (uint32_t)a->imm << 12);
    return true;
}

static bool trans_addi(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_addi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_subri(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_subfi_i32(cpu_gpr[a->rt], a->imm, cpu_gpr[a->ra]);
    return true;
}

static bool trans_andi(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_andi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_xori(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_xori_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_ori(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_ori_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_bitci(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_andi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], ~(uint32_t)a->imm);
    return true;
}

static bool trans_slti(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_setcondi_i32(TCG_COND_LTU, cpu_gpr[a->rt], cpu_gpr[a->ra],
                         a->imm);
    return true;
}

static bool trans_sltsi(DisasContext *ctx, arg_rri *a)
{
    tcg_gen_setcondi_i32(TCG_COND_LT, cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

/* ---------------- loads and stores ---------------- */

static void gen_load(DisasContext *ctx, int rt, TCGv_i32 addr, MemOp mop)
{
    tcg_gen_qemu_ld_i32(cpu_gpr[rt], addr, 0, mop | MO_ALIGN | mo_data(ctx));
}

static void gen_store(DisasContext *ctx, int rt, TCGv_i32 addr, MemOp mop)
{
    tcg_gen_qemu_st_i32(cpu_gpr[rt], addr, 0, mop | MO_ALIGN | mo_data(ctx));
}

static bool ls_imm(DisasContext *ctx, int rt, int ra, int32_t off, MemOp mop,
                   bool store, bool post)
{
    TCGv_i32 addr = tcg_temp_new_i32();

    if (post) {
        tcg_gen_mov_i32(addr, cpu_gpr[ra]);
    } else {
        tcg_gen_addi_i32(addr, cpu_gpr[ra], off);
    }
    if (store) {
        gen_store(ctx, rt, addr, mop);
    } else {
        gen_load(ctx, rt, addr, mop);
    }
    if (post) {
        /* the base is updated after the access, so it adds to a loaded rt */
        tcg_gen_addi_i32(cpu_gpr[ra], cpu_gpr[ra], off);
    }
    return true;
}

#define LSI(name, mop, sh, store, post)                                      \
    static bool trans_##name(DisasContext *ctx, arg_rri *a)                  \
    {                                                                        \
        return ls_imm(ctx, a->rt, a->ra, a->imm << (sh), mop, store, post);  \
    }

LSI(lbi, MO_UB, 0, false, false)
LSI(lhi, MO_UW, 1, false, false)
LSI(lwi, MO_UL, 2, false, false)
LSI(lbsi, MO_SB, 0, false, false)
LSI(lhsi, MO_SW, 1, false, false)
LSI(lbi_bi, MO_UB, 0, false, true)
LSI(lhi_bi, MO_UW, 1, false, true)
LSI(lwi_bi, MO_UL, 2, false, true)
LSI(lbsi_bi, MO_SB, 0, false, true)
LSI(lhsi_bi, MO_SW, 1, false, true)
LSI(sbi, MO_UB, 0, true, false)
LSI(shi, MO_UW, 1, true, false)
LSI(swi, MO_UL, 2, true, false)
LSI(sbi_bi, MO_UB, 0, true, true)
LSI(shi_bi, MO_UW, 1, true, true)
LSI(swi_bi, MO_UL, 2, true, true)

/* lwsi is the same as lwi on a 32-bit machine */
LSI(lwsi, MO_UL, 2, false, false)
LSI(lwsi_bi, MO_UL, 2, false, true)

static bool trans_dprefi(DisasContext *ctx, arg_rri *a)
{
    return true;
}

static bool ls_dword(DisasContext *ctx, int rt, TCGv_i32 addr, bool store)
{
    TCGv_i32 a1 = tcg_temp_new_i32();
    int r0 = rt & ~1;

    tcg_gen_addi_i32(a1, addr, 4);
    if (store) {
        gen_store(ctx, r0, addr, MO_UL);
        gen_store(ctx, r0 + 1, a1, MO_UL);
    } else {
        gen_load(ctx, r0, addr, MO_UL);
        gen_load(ctx, r0 + 1, a1, MO_UL);
    }
    return true;
}

static bool ldst_dimm(DisasContext *ctx, arg_rri *a, bool store, bool post)
{
    TCGv_i32 addr = tcg_temp_new_i32();

    tcg_gen_addi_i32(addr, cpu_gpr[a->ra], post ? 0 : a->imm << 3);
    ls_dword(ctx, a->rt, addr, store);
    if (post) {
        tcg_gen_addi_i32(cpu_gpr[a->ra], cpu_gpr[a->ra], a->imm << 3);
    }
    return true;
}

static bool trans_ldi(DisasContext *ctx, arg_rri *a)
{
    return ldst_dimm(ctx, a, false, false);
}

static bool trans_sdi(DisasContext *ctx, arg_rri *a)
{
    return ldst_dimm(ctx, a, true, false);
}

static bool trans_ldi_bi(DisasContext *ctx, arg_rri *a)
{
    return ldst_dimm(ctx, a, false, true);
}

static bool trans_sdi_bi(DisasContext *ctx, arg_rri *a)
{
    return ldst_dimm(ctx, a, true, true);
}

/* $gp relative: lbi.gp etc. */
static bool ls_gp(DisasContext *ctx, arg_ri *a, int sh, MemOp mop, bool store)
{
    TCGv_i32 addr = tcg_temp_new_i32();

    tcg_gen_addi_i32(addr, cpu_gpr[29], a->imm << sh);
    if (store) {
        gen_store(ctx, a->rt, addr, mop);
    } else {
        gen_load(ctx, a->rt, addr, mop);
    }
    return true;
}

static bool trans_lbi_gp(DisasContext *ctx, arg_ri *a)
{
    return ls_gp(ctx, a, 0, MO_UB, false);
}

static bool trans_lbsi_gp(DisasContext *ctx, arg_ri *a)
{
    return ls_gp(ctx, a, 0, MO_SB, false);
}

static bool trans_lhi_gp(DisasContext *ctx, arg_ri *a)
{
    return ls_gp(ctx, a, 1, MO_UW, false);
}

static bool trans_lhsi_gp(DisasContext *ctx, arg_ri *a)
{
    return ls_gp(ctx, a, 1, MO_SW, false);
}

static bool trans_shi_gp(DisasContext *ctx, arg_ri *a)
{
    return ls_gp(ctx, a, 1, MO_UW, true);
}

static bool trans_lwi_gp(DisasContext *ctx, arg_ri *a)
{
    return ls_gp(ctx, a, 2, MO_UL, false);
}

static bool trans_swi_gp(DisasContext *ctx, arg_ri *a)
{
    return ls_gp(ctx, a, 2, MO_UL, true);
}

static bool trans_sbi_gp(DisasContext *ctx, arg_ri *a)
{
    return ls_gp(ctx, a, 0, MO_UB, true);
}

static bool trans_addi_gp(DisasContext *ctx, arg_ri *a)
{
    tcg_gen_addi_i32(cpu_gpr[a->rt], cpu_gpr[29], a->imm);
    return true;
}

/* Register offset forms: [ra + (rb << sv)] and [ra], rb << sv */
static bool ls_reg(DisasContext *ctx, arg_mem *a, MemOp mop, bool store,
                   bool post)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 off = tcg_temp_new_i32();
    TCGv_i32 newbase = tcg_temp_new_i32();

    tcg_gen_shli_i32(off, cpu_gpr[a->rb], a->sv);
    if (post) {
        tcg_gen_mov_i32(addr, cpu_gpr[a->ra]);
        tcg_gen_add_i32(newbase, cpu_gpr[a->ra], off);
    } else {
        tcg_gen_add_i32(addr, cpu_gpr[a->ra], off);
    }
    if (store) {
        gen_store(ctx, a->rt, addr, mop);
    } else {
        gen_load(ctx, a->rt, addr, mop);
    }
    if (post) {
        /* unlike the immediate forms, the new base wins over a loaded rt */
        tcg_gen_mov_i32(cpu_gpr[a->ra], newbase);
    }
    return true;
}

#define LSR(name, mop, store, post)                                          \
    static bool trans_##name(DisasContext *ctx, arg_mem *a)                  \
    {                                                                        \
        return ls_reg(ctx, a, mop, store, post);                             \
    }

LSR(lb, MO_UB, false, false)
LSR(lh, MO_UW, false, false)
LSR(lw, MO_UL, false, false)
LSR(lb_bi, MO_UB, false, true)
LSR(lh_bi, MO_UW, false, true)
LSR(lw_bi, MO_UL, false, true)
LSR(sb, MO_UB, true, false)
LSR(sh, MO_UW, true, false)
LSR(sw, MO_UL, true, false)
LSR(sb_bi, MO_UB, true, true)
LSR(sh_bi, MO_UW, true, true)
LSR(sw_bi, MO_UL, true, true)
LSR(lbs, MO_SB, false, false)
LSR(lhs, MO_SW, false, false)
LSR(lbs_bi, MO_SB, false, true)
LSR(lhs_bi, MO_SW, false, true)
LSR(lbup, MO_UB, false, false)
LSR(lwup, MO_UL, false, false)
LSR(sbup, MO_UB, true, false)
LSR(swup, MO_UL, true, false)

static bool trans_dpref(DisasContext *ctx, arg_mem *a)
{
    return true;
}

static bool ldst_dreg(DisasContext *ctx, arg_mem *a, bool store, bool post)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 off = tcg_temp_new_i32();

    tcg_gen_shli_i32(off, cpu_gpr[a->rb], a->sv);
    if (post) {
        tcg_gen_mov_i32(addr, cpu_gpr[a->ra]);
    } else {
        tcg_gen_add_i32(addr, cpu_gpr[a->ra], off);
    }
    ls_dword(ctx, a->rt, addr, store);
    if (post) {
        tcg_gen_add_i32(cpu_gpr[a->ra], cpu_gpr[a->ra], off);
    }
    return true;
}

static bool trans_ld(DisasContext *ctx, arg_mem *a)
{
    return ldst_dreg(ctx, a, false, false);
}

static bool trans_sd(DisasContext *ctx, arg_mem *a)
{
    return ldst_dreg(ctx, a, true, false);
}

static bool trans_ld_bi(DisasContext *ctx, arg_mem *a)
{
    return ldst_dreg(ctx, a, false, true);
}

static bool trans_sd_bi(DisasContext *ctx, arg_mem *a)
{
    return ldst_dreg(ctx, a, true, true);
}

static bool trans_llw(DisasContext *ctx, arg_mem *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 off = tcg_temp_new_i32();

    tcg_gen_shli_i32(off, cpu_gpr[a->rb], a->sv);
    tcg_gen_add_i32(addr, cpu_gpr[a->ra], off);
    gen_load(ctx, a->rt, addr, MO_UL);
    tcg_gen_mov_i32(cpu_lock_addr, addr);
    tcg_gen_mov_i32(cpu_lock_val, cpu_gpr[a->rt]);
    tcg_gen_movi_i32(cpu_lock_valid, 1);
    return true;
}

static bool trans_scw(DisasContext *ctx, arg_mem *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 off = tcg_temp_new_i32();
    TCGLabel *fail = gen_new_label();
    TCGLabel *done = gen_new_label();

    tcg_gen_shli_i32(off, cpu_gpr[a->rb], a->sv);
    tcg_gen_add_i32(addr, cpu_gpr[a->ra], off);
    tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_lock_valid, 0, fail);
    tcg_gen_brcond_i32(TCG_COND_NE, addr, cpu_lock_addr, fail);
    gen_store(ctx, a->rt, addr, MO_UL);
    tcg_gen_movi_i32(cpu_gpr[a->rt], 1);
    tcg_gen_br(done);
    gen_set_label(fail);
    tcg_gen_movi_i32(cpu_gpr[a->rt], 0);
    gen_set_label(done);
    tcg_gen_movi_i32(cpu_lock_valid, 0);
    return true;
}

/*
 * lmw/smw: the registers rb..re (empty when both are r31) followed by the
 * fp, gp, lp and sp selected by enb4 move to or from consecutive words.
 */
static bool gen_lsmw(DisasContext *ctx, int rb, int ra, int re, int enb4,
                     bool store, int abdim)
{
    int regs[32];
    int n = 0;
    int i;
    bool after = abdim & 4;
    bool dec = abdim & 2;
    bool update = abdim & 1;
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGv_i32 base = tcg_temp_new_i32();

    if (!(rb == 31 && re == 31)) {
        for (i = rb; i <= re; i++) {
            regs[n++] = i;
        }
    }
    for (i = 0; i < 4; i++) {
        if (enb4 & (8 >> i)) {
            regs[n++] = 28 + i;
        }
    }

    tcg_gen_mov_i32(base, cpu_gpr[ra]);
    if (dec) {
        tcg_gen_subi_i32(addr, base, after ? 4 * n : 4 * (n - 1));
    } else {
        tcg_gen_addi_i32(addr, base, after ? 4 : 0);
    }
    if (n == 0) {
        /* no register moves but the base may still be updated */
        goto update;
    }
    for (i = 0; i < n; i++) {
        if (store) {
            gen_store(ctx, regs[i], addr, MO_UL);
        } else {
            gen_load(ctx, regs[i], addr, MO_UL);
        }
        tcg_gen_addi_i32(addr, addr, 4);
    }
update:
    if (update) {
        /* applied to ra as left by the loads */
        if (dec) {
            tcg_gen_subi_i32(cpu_gpr[ra], cpu_gpr[ra], 4 * n);
        } else {
            tcg_gen_addi_i32(cpu_gpr[ra], cpu_gpr[ra], 4 * n);
        }
    }
    return true;
}

static bool trans_lsmw(DisasContext *ctx, arg_lsmw *a)
{
    return gen_lsmw(ctx, a->rb, a->ra, a->re, a->enb4, !!a->s, a->abdim);
}

static bool trans_lsmwa(DisasContext *ctx, arg_lsmw *a)
{
    return gen_lsmw(ctx, a->rb, a->ra, a->re, a->enb4, !!a->s, a->abdim);
}

/* ---------------- branches ---------------- */

static bool trans_j(DisasContext *ctx, arg_j *a)
{
    return gen_jump(ctx, cur_pc(ctx) + (a->imm << 1));
}

static bool trans_jal(DisasContext *ctx, arg_j *a)
{
    tcg_gen_movi_i32(cpu_gpr[30], next_pc(ctx));
    return gen_jump(ctx, cur_pc(ctx) + (a->imm << 1));
}

static bool trans_jr(DisasContext *ctx, arg_jreg *a)
{
    if ((a->hint & 3) == 3) {
        /* ifret */
        TCGv_i32 target = tcg_temp_new_i32();
        TCGv_i32 on = tcg_temp_new_i32();

        tcg_gen_andi_i32(on, cpu_psw, PSW_IFCON);
        tcg_gen_movcond_i32(TCG_COND_NE, target, on, tcg_constant_i32(0),
                            cpu_ifc_lp, tcg_constant_i32(next_pc(ctx)));
        tcg_gen_andi_i32(cpu_psw, cpu_psw, ~PSW_IFCON);
        return gen_jump_reg(ctx, target);
    }
    return gen_jump_reg(ctx, cpu_gpr[a->rb]);
}

static bool trans_jral(DisasContext *ctx, arg_jreg *a)
{
    TCGv_i32 target = tcg_temp_new_i32();

    tcg_gen_mov_i32(target, cpu_gpr[a->rb]);
    tcg_gen_movi_i32(cpu_gpr[a->rt], next_pc(ctx));
    return gen_jump_reg(ctx, target);
}

static bool trans_jrnez(DisasContext *ctx, arg_jreg *a)
{
    TCGv_i32 target = tcg_temp_new_i32();
    TCGv_i32 next = tcg_constant_i32(next_pc(ctx));

    tcg_gen_movcond_i32(TCG_COND_NE, target, cpu_gpr[a->rb],
                        tcg_constant_i32(0), cpu_gpr[a->rb], next);
    return gen_jump_reg(ctx, target);
}

static bool trans_jralnez(DisasContext *ctx, arg_jreg *a)
{
    TCGv_i32 target = tcg_temp_new_i32();
    TCGv_i32 next = tcg_constant_i32(next_pc(ctx));
    TCGv_i32 link = tcg_temp_new_i32();

    tcg_gen_movcond_i32(TCG_COND_NE, target, cpu_gpr[a->rb],
                        tcg_constant_i32(0), cpu_gpr[a->rb], next);
    tcg_gen_movcond_i32(TCG_COND_NE, link, cpu_gpr[a->rb],
                        tcg_constant_i32(0), next, cpu_gpr[a->rt]);
    tcg_gen_mov_i32(cpu_gpr[a->rt], link);
    return gen_jump_reg(ctx, target);
}

static bool trans_beq(DisasContext *ctx, arg_br1 *a)
{
    return gen_bcond(ctx, TCG_COND_EQ, cpu_gpr[a->rt], cpu_gpr[a->ra],
                     a->imm << 1);
}

static bool trans_bne(DisasContext *ctx, arg_br1 *a)
{
    return gen_bcond(ctx, TCG_COND_NE, cpu_gpr[a->rt], cpu_gpr[a->ra],
                     a->imm << 1);
}

static bool trans_bgezal(DisasContext *ctx, arg_brr *a)
{
    TCGv_i32 v = tcg_temp_new_i32();

    tcg_gen_mov_i32(v, cpu_gpr[a->rt]);
    tcg_gen_movi_i32(cpu_gpr[30], next_pc(ctx));
    return gen_bcondi(ctx, TCG_COND_GE, v, 0, a->imm << 1);
}

static bool trans_bltzal(DisasContext *ctx, arg_brr *a)
{
    TCGv_i32 v = tcg_temp_new_i32();

    tcg_gen_mov_i32(v, cpu_gpr[a->rt]);
    tcg_gen_movi_i32(cpu_gpr[30], next_pc(ctx));
    return gen_bcondi(ctx, TCG_COND_LT, v, 0, a->imm << 1);
}

static bool trans_beqz(DisasContext *ctx, arg_brr *a)
{
    return gen_bcondi(ctx, TCG_COND_EQ, cpu_gpr[a->rt], 0, a->imm << 1);
}

static bool trans_bnez(DisasContext *ctx, arg_brr *a)
{
    return gen_bcondi(ctx, TCG_COND_NE, cpu_gpr[a->rt], 0, a->imm << 1);
}

static bool trans_bgez(DisasContext *ctx, arg_brr *a)
{
    return gen_bcondi(ctx, TCG_COND_GE, cpu_gpr[a->rt], 0, a->imm << 1);
}

static bool trans_bltz(DisasContext *ctx, arg_brr *a)
{
    return gen_bcondi(ctx, TCG_COND_LT, cpu_gpr[a->rt], 0, a->imm << 1);
}

static bool trans_bgtz(DisasContext *ctx, arg_brr *a)
{
    return gen_bcondi(ctx, TCG_COND_GT, cpu_gpr[a->rt], 0, a->imm << 1);
}

static bool trans_blez(DisasContext *ctx, arg_brr *a)
{
    return gen_bcondi(ctx, TCG_COND_LE, cpu_gpr[a->rt], 0, a->imm << 1);
}

static bool trans_beqc(DisasContext *ctx, arg_br3 *a)
{
    return gen_bcondi(ctx, TCG_COND_EQ, cpu_gpr[a->rt], a->cimm, a->imm << 1);
}

static bool trans_bnec(DisasContext *ctx, arg_br3 *a)
{
    return gen_bcondi(ctx, TCG_COND_NE, cpu_gpr[a->rt], a->cimm, a->imm << 1);
}

/* Inline function call: nested calls keep the first return address */
static bool gen_ifcall(DisasContext *ctx, uint32_t dest)
{
    TCGv_i32 on = tcg_temp_new_i32();

    tcg_gen_andi_i32(on, cpu_psw, PSW_IFCON);
    tcg_gen_movcond_i32(TCG_COND_EQ, cpu_ifc_lp, on, tcg_constant_i32(0),
                        tcg_constant_i32(next_pc(ctx)), cpu_ifc_lp);
    tcg_gen_ori_i32(cpu_psw, cpu_psw, PSW_IFCON);
    return gen_jump(ctx, dest);
}

static bool trans_ifcall(DisasContext *ctx, arg_brr *a)
{
    return gen_ifcall(ctx, cur_pc(ctx) + (a->imm << 1));
}

/* ---------------- system ---------------- */

static bool usr_read(DisasContext *ctx, TCGv_i32 d, uint32_t usr)
{
    switch (usr) {
    case USR_D0_LO:
    case USR_D0_HI:
    case USR_D1_LO:
    case USR_D1_HI:
        tcg_gen_mov_i32(d, cpu_d[usr >> 5]);
        break;
    case USR_ITB:
        tcg_gen_mov_i32(d, cpu_itb);
        break;
    case USR_IFC_LP:
        tcg_gen_mov_i32(d, cpu_ifc_lp);
        break;
    case USR_PC:
        tcg_gen_movi_i32(d, cur_pc(ctx));
        break;
    case 0x3c0:
        tcg_gen_mov_i32(d, cpu_ov);
        break;
    default:
        tcg_gen_movi_i32(d, 0);
        break;
    }
    return true;
}

static bool trans_mfusr(DisasContext *ctx, arg_usr *a)
{
    return usr_read(ctx, cpu_gpr[a->rt], a->usr);
}

static bool trans_mtusr(DisasContext *ctx, arg_usr *a)
{
    switch (a->usr) {
    case USR_D0_LO:
    case USR_D0_HI:
    case USR_D1_LO:
    case USR_D1_HI:
        tcg_gen_mov_i32(cpu_d[a->usr >> 5], cpu_gpr[a->rt]);
        break;
    case USR_ITB:
        tcg_gen_mov_i32(cpu_itb, cpu_gpr[a->rt]);
        break;
    case USR_IFC_LP:
        tcg_gen_mov_i32(cpu_ifc_lp, cpu_gpr[a->rt]);
        break;
    case 0x3c0:
        tcg_gen_andi_i32(cpu_ov, cpu_gpr[a->rt], 1);
        break;
    default:
        break;
    }
    return true;
}

static bool trans_mfsr(DisasContext *ctx, arg_sr *a)
{
    if (!priv_check(ctx)) {
        return true;
    }
    gen_helper_mfsr(cpu_gpr[a->rt], tcg_env, tcg_constant_i32(a->sr));
    return true;
}

static bool trans_mtsr(DisasContext *ctx, arg_sr *a)
{
    if (!priv_check(ctx)) {
        return true;
    }
    tcg_gen_movi_i32(cpu_pc, next_pc(ctx));
    gen_helper_mtsr(tcg_env, tcg_constant_i32(a->sr), cpu_gpr[a->rt]);
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_setgie(DisasContext *ctx, arg_setend *a)
{
    if (!priv_check(ctx)) {
        return true;
    }
    tcg_gen_movi_i32(cpu_pc, next_pc(ctx));
    gen_helper_setgie(tcg_env, tcg_constant_i32(a->e));
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_setend(DisasContext *ctx, arg_setend *a)
{
    if (!priv_check(ctx)) {
        return true;
    }
    if (a->e) {
        tcg_gen_ori_i32(cpu_psw, cpu_psw, PSW_BE);
    } else {
        tcg_gen_andi_i32(cpu_psw, cpu_psw, ~PSW_BE);
    }
    tcg_gen_movi_i32(cpu_pc, next_pc(ctx));
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_iret(DisasContext *ctx, arg_misc *a)
{
    if (!priv_check(ctx)) {
        return true;
    }
    gen_helper_iret(tcg_env);
    ctx->base.is_jmp = DISAS_EXIT;
    return true;
}

static bool trans_standby(DisasContext *ctx, arg_misc *a)
{
    if (!priv_check(ctx)) {
        return true;
    }
    tcg_gen_movi_i32(cpu_pc, next_pc(ctx));
    gen_helper_standby(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_trap(DisasContext *ctx, arg_swid *a)
{
    gen_raise(ctx, NDS32_VEC_GENERAL, ETYPE_TRAP, a->swid);
    return true;
}

static bool trans_teqz(DisasContext *ctx, arg_rt_swid *a)
{
    TCGLabel *skip = gen_new_label();

    tcg_gen_brcondi_i32(TCG_COND_NE, cpu_gpr[a->rt], 0, skip);
    gen_raise(ctx, NDS32_VEC_GENERAL, ETYPE_TRAP, a->swid);
    gen_set_label(skip);
    ctx->base.is_jmp = DISAS_NEXT;
    return true;
}

static bool trans_tnez(DisasContext *ctx, arg_rt_swid *a)
{
    TCGLabel *skip = gen_new_label();

    tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_gpr[a->rt], 0, skip);
    gen_raise(ctx, NDS32_VEC_GENERAL, ETYPE_TRAP, a->swid);
    gen_set_label(skip);
    ctx->base.is_jmp = DISAS_NEXT;
    return true;
}

static bool trans_break(DisasContext *ctx, arg_swid *a)
{
    gen_raise(ctx, NDS32_VEC_DEBUG, 0, a->swid);
    return true;
}

static bool trans_syscall(DisasContext *ctx, arg_swid *a)
{
    gen_raise(ctx, NDS32_VEC_SYSCALL, 0, a->swid);
    return true;
}

static bool trans_nop_insn(DisasContext *ctx, arg_misc *a)
{
    return true;
}

static bool trans_dsb(DisasContext *ctx, arg_misc *a)
{
    return trans_nop_insn(ctx, a);
}

static bool trans_isb(DisasContext *ctx, arg_misc *a)
{
    return trans_nop_insn(ctx, a);
}

static bool trans_cctl(DisasContext *ctx, arg_misc *a)
{
    return trans_nop_insn(ctx, a);
}

static bool trans_msync(DisasContext *ctx, arg_misc *a)
{
    return trans_nop_insn(ctx, a);
}

static bool trans_tlbop(DisasContext *ctx, arg_misc *a)
{
    return true;
}

static bool trans_isync(DisasContext *ctx, arg_rt1 *a)
{
    return true;
}

/* ---------------- 16-bit forms ---------------- */

static bool trans_beqz38(DisasContext *ctx, arg_hbr *a)
{
    return gen_bcondi(ctx, TCG_COND_EQ, cpu_gpr[a->rt], 0, a->imm << 1);
}

static bool trans_bnez38(DisasContext *ctx, arg_hbr *a)
{
    return gen_bcondi(ctx, TCG_COND_NE, cpu_gpr[a->rt], 0, a->imm << 1);
}

static bool trans_beqs38(DisasContext *ctx, arg_hbr *a)
{
    return gen_bcond(ctx, TCG_COND_EQ, cpu_gpr[a->rt], cpu_gpr[5],
                     a->imm << 1);
}

static bool trans_bnes38(DisasContext *ctx, arg_hbr *a)
{
    return gen_bcond(ctx, TCG_COND_NE, cpu_gpr[a->rt], cpu_gpr[5],
                     a->imm << 1);
}

static bool trans_j8(DisasContext *ctx, arg_himm *a)
{
    return gen_jump(ctx, cur_pc(ctx) + (a->imm << 1));
}

static bool trans_beqzs8(DisasContext *ctx, arg_himm *a)
{
    return gen_bcondi(ctx, TCG_COND_EQ, cpu_gpr[15], 0, a->imm << 1);
}

static bool trans_bnezs8(DisasContext *ctx, arg_himm *a)
{
    return gen_bcondi(ctx, TCG_COND_NE, cpu_gpr[15], 0, a->imm << 1);
}

static bool trans_mov55(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_mov_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_movi55(DisasContext *ctx, arg_hi *a)
{
    tcg_gen_movi_i32(cpu_gpr[a->rt], a->imm);
    return true;
}

static bool trans_add45(DisasContext *ctx, arg_h2 *a)
{
    int rt = r45(a->rt);

    tcg_gen_add_i32(cpu_gpr[rt], cpu_gpr[rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_sub45(DisasContext *ctx, arg_h2 *a)
{
    int rt = r45(a->rt);

    tcg_gen_sub_i32(cpu_gpr[rt], cpu_gpr[rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_addi45(DisasContext *ctx, arg_hi *a)
{
    int rt = r45(a->rt);

    tcg_gen_addi_i32(cpu_gpr[rt], cpu_gpr[rt], a->imm);
    return true;
}

static bool trans_subi45(DisasContext *ctx, arg_hi *a)
{
    int rt = r45(a->rt);

    tcg_gen_subi_i32(cpu_gpr[rt], cpu_gpr[rt], a->imm);
    return true;
}

static bool trans_srai45(DisasContext *ctx, arg_hi *a)
{
    int rt = r45(a->rt);

    tcg_gen_sari_i32(cpu_gpr[rt], cpu_gpr[rt], a->imm);
    return true;
}

static bool trans_srli45(DisasContext *ctx, arg_hi *a)
{
    int rt = r45(a->rt);

    tcg_gen_shri_i32(cpu_gpr[rt], cpu_gpr[rt], a->imm);
    return true;
}

static bool trans_slli333(DisasContext *ctx, arg_hri *a)
{
    tcg_gen_shli_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_add333(DisasContext *ctx, arg_h3 *a)
{
    tcg_gen_add_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_sub333(DisasContext *ctx, arg_h3 *a)
{
    tcg_gen_sub_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], cpu_gpr[a->rb]);
    return true;
}

static bool trans_addi333(DisasContext *ctx, arg_hri *a)
{
    tcg_gen_addi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_subi333(DisasContext *ctx, arg_hri *a)
{
    tcg_gen_subi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], a->imm);
    return true;
}

static bool trans_zeb33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_ext8u_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_zeh33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_ext16u_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_seb33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_ext8s_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_seh33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_ext16s_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_xlsb33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_andi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], 1);
    return true;
}

static bool trans_x11b33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_andi_i32(cpu_gpr[a->rt], cpu_gpr[a->ra], 0x7ff);
    return true;
}

static bool trans_bmski33(DisasContext *ctx, arg_hi *a)
{
    tcg_gen_andi_i32(cpu_gpr[a->rt], cpu_gpr[a->rt], 1u << a->imm);
    return true;
}

static bool trans_fexti33(DisasContext *ctx, arg_hi *a)
{
    tcg_gen_andi_i32(cpu_gpr[a->rt], cpu_gpr[a->rt],
                     (2u << a->imm) - 1);
    return true;
}

static bool trans_neg33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_neg_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_not33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_not_i32(cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_mul33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_mul_i32(cpu_gpr[a->rt], cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_xor33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_xor_i32(cpu_gpr[a->rt], cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_and33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_and_i32(cpu_gpr[a->rt], cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_or33(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_or_i32(cpu_gpr[a->rt], cpu_gpr[a->rt], cpu_gpr[a->ra]);
    return true;
}

static bool trans_slts45(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_setcond_i32(TCG_COND_LT, cpu_gpr[15], cpu_gpr[r45(a->rt)],
                        cpu_gpr[a->ra]);
    return true;
}

static bool trans_slt45(DisasContext *ctx, arg_h2 *a)
{
    tcg_gen_setcond_i32(TCG_COND_LTU, cpu_gpr[15], cpu_gpr[r45(a->rt)],
                        cpu_gpr[a->ra]);
    return true;
}

static bool trans_sltsi45(DisasContext *ctx, arg_hi *a)
{
    tcg_gen_setcondi_i32(TCG_COND_LT, cpu_gpr[15], cpu_gpr[r45(a->rt)],
                         a->imm);
    return true;
}

static bool trans_slti45(DisasContext *ctx, arg_hi *a)
{
    tcg_gen_setcondi_i32(TCG_COND_LTU, cpu_gpr[15], cpu_gpr[r45(a->rt)],
                         a->imm);
    return true;
}

static bool trans_movpi45(DisasContext *ctx, arg_hi *a)
{
    tcg_gen_movi_i32(cpu_gpr[r45(a->rt)], a->imm + 16);
    return true;
}

static bool trans_movd44(DisasContext *ctx, arg_h2 *a)
{
    int rt = a->rt * 2;
    int ra = a->ra * 2;
    TCGv_i32 lo = tcg_temp_new_i32();
    TCGv_i32 hi = tcg_temp_new_i32();

    tcg_gen_mov_i32(lo, cpu_gpr[ra]);
    tcg_gen_mov_i32(hi, cpu_gpr[ra + 1]);
    tcg_gen_mov_i32(cpu_gpr[rt], lo);
    tcg_gen_mov_i32(cpu_gpr[rt + 1], hi);
    return true;
}

static bool trans_addri36_sp(DisasContext *ctx, arg_hi *a)
{
    tcg_gen_addi_i32(cpu_gpr[a->rt], cpu_gpr[31], a->imm << 2);
    return true;
}

static bool trans_addi10_sp(DisasContext *ctx, arg_himm *a)
{
    tcg_gen_addi_i32(cpu_gpr[31], cpu_gpr[31], a->imm);
    return true;
}

static bool trans_lwi333(DisasContext *ctx, arg_hri *a)
{
    return ls_imm(ctx, a->rt, a->ra, a->imm << 2, MO_UL, false, false);
}

static bool trans_lwi333_bi(DisasContext *ctx, arg_hri *a)
{
    return ls_imm(ctx, a->rt, a->ra, a->imm << 2, MO_UL, false, true);
}

static bool trans_lhi333(DisasContext *ctx, arg_hri *a)
{
    return ls_imm(ctx, a->rt, a->ra, a->imm << 1, MO_UW, false, false);
}

static bool trans_lbi333(DisasContext *ctx, arg_hri *a)
{
    return ls_imm(ctx, a->rt, a->ra, a->imm, MO_UB, false, false);
}

static bool trans_swi333(DisasContext *ctx, arg_hri *a)
{
    return ls_imm(ctx, a->rt, a->ra, a->imm << 2, MO_UL, true, false);
}

static bool trans_swi333_bi(DisasContext *ctx, arg_hri *a)
{
    return ls_imm(ctx, a->rt, a->ra, a->imm << 2, MO_UL, true, true);
}

static bool trans_shi333(DisasContext *ctx, arg_hri *a)
{
    return ls_imm(ctx, a->rt, a->ra, a->imm << 1, MO_UW, true, false);
}

static bool trans_sbi333(DisasContext *ctx, arg_hri *a)
{
    return ls_imm(ctx, a->rt, a->ra, a->imm, MO_UB, true, false);
}

static bool trans_lwi450(DisasContext *ctx, arg_h2 *a)
{
    return ls_imm(ctx, r45(a->rt), a->ra, 0, MO_UL, false, false);
}

static bool trans_swi450(DisasContext *ctx, arg_h2 *a)
{
    return ls_imm(ctx, r45(a->rt), a->ra, 0, MO_UL, true, false);
}

static bool trans_lwi45_fe(DisasContext *ctx, arg_hi *a)
{
    return ls_imm(ctx, r45(a->rt), 8, (a->imm - 32) << 2, MO_UL, false,
                  false);
}

static bool trans_lwi37(DisasContext *ctx, arg_hi *a)
{
    return ls_imm(ctx, a->rt, 28, a->imm << 2, MO_UL, false, false);
}

static bool trans_swi37(DisasContext *ctx, arg_hi *a)
{
    return ls_imm(ctx, a->rt, 28, a->imm << 2, MO_UL, true, false);
}

static bool trans_lwi37_sp(DisasContext *ctx, arg_hi *a)
{
    return ls_imm(ctx, a->rt, 31, a->imm << 2, MO_UL, false, false);
}

static bool trans_swi37_sp(DisasContext *ctx, arg_hi *a)
{
    return ls_imm(ctx, a->rt, 31, a->imm << 2, MO_UL, true, false);
}

static bool trans_jr5(DisasContext *ctx, arg_hra *a)
{
    return gen_jump_reg(ctx, cpu_gpr[a->ra]);
}

static bool trans_ret5(DisasContext *ctx, arg_hra *a)
{
    return gen_jump_reg(ctx, cpu_gpr[a->ra]);
}

static bool trans_jral5(DisasContext *ctx, arg_hra *a)
{
    TCGv_i32 target = tcg_temp_new_i32();

    tcg_gen_mov_i32(target, cpu_gpr[a->ra]);
    tcg_gen_movi_i32(cpu_gpr[30], next_pc(ctx));
    return gen_jump_reg(ctx, target);
}

static bool trans_add5_pc(DisasContext *ctx, arg_hra *a)
{
    tcg_gen_addi_i32(cpu_gpr[a->ra], cpu_gpr[a->ra], cur_pc(ctx));
    return true;
}

static bool trans_ifcall9(DisasContext *ctx, arg_himm *a)
{
    return gen_ifcall(ctx, cur_pc(ctx) + (a->imm << 1));
}

static bool trans_ifret16(DisasContext *ctx, arg_hnone *a)
{
    TCGv_i32 target = tcg_temp_new_i32();
    TCGv_i32 on = tcg_temp_new_i32();

    tcg_gen_andi_i32(on, cpu_psw, PSW_IFCON);
    tcg_gen_movcond_i32(TCG_COND_NE, target, on, tcg_constant_i32(0),
                        cpu_ifc_lp, tcg_constant_i32(next_pc(ctx)));
    tcg_gen_andi_i32(cpu_psw, cpu_psw, ~PSW_IFCON);
    return gen_jump_reg(ctx, target);
}

static bool trans_break16(DisasContext *ctx, arg_himm *a)
{
    gen_raise(ctx, NDS32_VEC_DEBUG, 0, a->imm);
    return true;
}

static bool trans_ex9_it(DisasContext *ctx, arg_himm *a)
{
    return false;
}

static bool trans_ex9_it5(DisasContext *ctx, arg_himm *a)
{
    return false;
}

/* push25/pop25: r6..re plus fp, gp, lp */
static const uint8_t pp25_re[4] = { 6, 8, 10, 14 };

static bool trans_push25(DisasContext *ctx, arg_hpp25 *a)
{
    gen_lsmw(ctx, 6, 31, pp25_re[a->re], 0xe, true, 7);
    if (a->imm) {
        tcg_gen_subi_i32(cpu_gpr[31], cpu_gpr[31], a->imm << 3);
    }
    return true;
}

static bool trans_pop25(DisasContext *ctx, arg_hpp25 *a)
{
    TCGv_i32 target = tcg_temp_new_i32();

    if (a->imm) {
        tcg_gen_addi_i32(cpu_gpr[31], cpu_gpr[31], a->imm << 3);
    }
    gen_lsmw(ctx, 6, 31, pp25_re[a->re], 0xe, false, 1);
    tcg_gen_mov_i32(target, cpu_gpr[30]);
    return gen_jump_reg(ctx, target);
}

/* ---------------- translator ---------------- */

static void nds32_tr_init_disas_context(DisasContextBase *dcbase,
                                        CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    int bound;

    ctx->tb_flags = ctx->base.tb->flags;
    ctx->be = !!(ctx->tb_flags & TB_FLAGS_BE);
    ctx->user = !(ctx->tb_flags & TB_FLAGS_POM);
    bound = -(ctx->base.pc_first | TARGET_PAGE_MASK) / 2;
    ctx->base.max_insns = MIN(ctx->base.max_insns, bound);
}

static void nds32_tr_tb_start(DisasContextBase *db, CPUState *cs)
{
}

static void nds32_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    tcg_gen_insn_start(dcbase->pc_next, 0, 0);
}

static void nds32_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPUNDS32State *env = cpu_env(cs);
    uint32_t pc = ctx->base.pc_next;
    uint32_t insn;
    bool ok;

    /*
     * Instructions are stored as big-endian halfwords whatever the data
     * endianness.  Bit 15 of the first halfword tells the 16-bit forms.
     */
    insn = translator_lduw_end(env, &ctx->base, pc, MO_BE);
    if (insn & 0x8000) {
        ctx->ilen = 2;
        ok = decode_insn16(ctx, insn);
    } else {
        insn = (insn << 16) |
               translator_lduw_end(env, &ctx->base, pc + 2, MO_BE);
        ctx->ilen = 4;
        ok = decode_insn32(ctx, insn);
    }
    if (!ok) {
        gen_raise(ctx, NDS32_VEC_GENERAL, ETYPE_RESERVED_INSN, 0);
    }
    ctx->base.pc_next = pc + ctx->ilen;

    if (ctx->base.is_jmp == DISAS_NEXT &&
        (ctx->base.pc_next & ~TARGET_PAGE_MASK) == 0) {
        ctx->base.is_jmp = DISAS_TOO_MANY;
    }
}

static void nds32_tr_tb_stop(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NORETURN:
        break;
    case DISAS_TOO_MANY:
        gen_goto_tb(ctx, 0, ctx->base.pc_next);
        break;
    case DISAS_EXIT:
        tcg_gen_exit_tb(NULL, 0);
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps nds32_tr_ops = {
    .init_disas_context = nds32_tr_init_disas_context,
    .tb_start           = nds32_tr_tb_start,
    .insn_start         = nds32_tr_insn_start,
    .translate_insn     = nds32_tr_translate_insn,
    .tb_stop            = nds32_tr_tb_stop,
};

void nds32_translate_code(CPUState *cs, TranslationBlock *tb,
                          int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext ctx;

    translator_loop(cs, tb, max_insns, pc, host_pc, &nds32_tr_ops, &ctx.base,
                    TCG_TYPE_VA);
}

void nds32_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUNDS32State *env = cpu_env(cs);
    int i;

    qemu_fprintf(f, "PC=%08x PSW=%08x IPC=%08x IPSW=%08x ITYPE=%08x\n",
                 env->pc, env->sr[SR_PSW], env->sr[SR_IPC],
                 env->sr[SR_IPSW], env->sr[SR_ITYPE]);
    for (i = 0; i < 32; i++) {
        qemu_fprintf(f, "R%02d=%08x%c", i, env->gpr[i],
                     (i % 4) == 3 ? '\n' : ' ');
    }
}
