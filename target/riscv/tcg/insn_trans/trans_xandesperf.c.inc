/*
 * RISC-V translation routines for Andes' performance extension
 * (XAndesPerf) and its EXEC.IT
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

static bool trans_lbgp(DisasContext *ctx, arg_lbgp *a)
{
    return gen_load(ctx, a, MO_SB);
}

static bool trans_lbugp(DisasContext *ctx, arg_lbugp *a)
{
    return gen_load(ctx, a, MO_UB);
}

static bool trans_lhgp(DisasContext *ctx, arg_lhgp *a)
{
    return gen_load(ctx, a, MO_SW);
}

static bool trans_lhugp(DisasContext *ctx, arg_lhugp *a)
{
    return gen_load(ctx, a, MO_UW);
}

static bool trans_lwgp(DisasContext *ctx, arg_lwgp *a)
{
    return gen_load(ctx, a, MO_SL);
}

static bool trans_lwugp(DisasContext *ctx, arg_lwugp *a)
{
    return gen_load(ctx, a, MO_UL);
}

static bool trans_sbgp(DisasContext *ctx, arg_sbgp *a)
{
    return gen_store(ctx, a, MO_SB);
}

static bool trans_shgp(DisasContext *ctx, arg_shgp *a)
{
    return gen_store(ctx, a, MO_SW);
}

static bool trans_swgp(DisasContext *ctx, arg_swgp *a)
{
    return gen_store(ctx, a, MO_SL);
}

static bool trans_addigp(DisasContext *ctx, arg_addigp *a)
{
    TCGv dest = dest_gpr(ctx, a->rd);

    tcg_gen_addi_tl(dest, get_gpr(ctx, 3, EXT_NONE), a->imm);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

/* BEQC, BNEC, BBC and BBS: a branch on rs1 against a constant or a bit */
static bool gen_andes_branch(DisasContext *ctx, arg_beqc *a, TCGCond cond,
                             bool bit)
{
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv t = tcg_temp_new();
    TCGLabel *l = gen_new_label();
    target_ulong orig_pc_save = ctx->pc_save;

    if (bit) {
        tcg_gen_andi_tl(t, src, 1u << (a->cimm & 31));
        tcg_gen_brcondi_tl(cond, t, 0, l);
    } else {
        tcg_gen_brcondi_tl(cond, src, a->cimm, l);
    }
    gen_goto_tb(ctx, 1, ctx->cur_insn_len);
    ctx->pc_save = orig_pc_save;

    gen_set_label(l);
    gen_goto_tb(ctx, 0, a->imm);
    ctx->pc_save = -1;
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_beqc(DisasContext *ctx, arg_beqc *a)
{
    return gen_andes_branch(ctx, a, TCG_COND_EQ, false);
}

static bool trans_bnec(DisasContext *ctx, arg_bnec *a)
{
    return gen_andes_branch(ctx, a, TCG_COND_NE, false);
}

static bool trans_bbc(DisasContext *ctx, arg_bbc *a)
{
    return gen_andes_branch(ctx, a, TCG_COND_EQ, true);
}

static bool trans_bbs(DisasContext *ctx, arg_bbs *a)
{
    return gen_andes_branch(ctx, a, TCG_COND_NE, true);
}

/*
 * BFOZ and BFOS. msb >= lsb extracts bits msb..lsb to the bottom; msb < lsb
 * inserts the bottom lsb - msb + 1 bits at msb; msb 0 with lsb above it
 * moves bit 0 alone to lsb. BFOS sign-extends where BFOZ zero-extends.
 */
static bool gen_bfo(DisasContext *ctx, arg_bfoz *a, bool sign)
{
    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    int msb = a->msb, lsb = a->lsb;

    if (msb == 0) {
        /* a single bit, at lsb */
        if (sign) {
            tcg_gen_sextract_tl(dest, src, 0, 1);
        } else {
            tcg_gen_andi_tl(dest, src, 1);
        }
        tcg_gen_shli_tl(dest, dest, lsb & 31);
    } else if (msb >= lsb) {
        int len = msb - lsb + 1;

        if (len > 32 || lsb > 31) {
            return false;
        }
        if (sign) {
            tcg_gen_sextract_tl(dest, src, lsb, len);
        } else {
            tcg_gen_extract_tl(dest, src, lsb, len);
        }
    } else {
        int len = lsb - msb + 1;

        if (msb > 31 || len > 32) {
            return false;
        }
        if (sign) {
            tcg_gen_sextract_tl(dest, src, 0, len);
        } else {
            tcg_gen_extract_tl(dest, src, 0, len);
        }
        tcg_gen_shli_tl(dest, dest, msb);
    }
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_bfoz(DisasContext *ctx, arg_bfoz *a)
{
    return gen_bfo(ctx, a, false);
}

static bool trans_bfos(DisasContext *ctx, arg_bfos *a)
{
    return gen_bfo(ctx, a, true);
}

static bool gen_lea(DisasContext *ctx, arg_r *a, int shift)
{
    TCGv dest = dest_gpr(ctx, a->rd);
    TCGv t = tcg_temp_new();

    tcg_gen_shli_tl(t, get_gpr(ctx, a->rs2, EXT_NONE), shift);
    tcg_gen_add_tl(dest, get_gpr(ctx, a->rs1, EXT_NONE), t);
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

/* On RV32 the .ze forms (rs2 zero-extended from 32 bits) are the plain ones */
static bool trans_lea_h(DisasContext *ctx, arg_lea_h *a)
{
    return gen_lea(ctx, a, 1);
}

static bool trans_lea_w(DisasContext *ctx, arg_lea_w *a)
{
    return gen_lea(ctx, a, 2);
}

static bool trans_lea_d(DisasContext *ctx, arg_lea_d *a)
{
    return gen_lea(ctx, a, 3);
}

static bool trans_lea_b_ze(DisasContext *ctx, arg_lea_b_ze *a)
{
    return gen_lea(ctx, a, 0);
}

static bool trans_lea_h_ze(DisasContext *ctx, arg_lea_h_ze *a)
{
    return gen_lea(ctx, a, 1);
}

static bool trans_lea_w_ze(DisasContext *ctx, arg_lea_w_ze *a)
{
    return gen_lea(ctx, a, 2);
}

static bool trans_lea_d_ze(DisasContext *ctx, arg_lea_d_ze *a)
{
    return gen_lea(ctx, a, 3);
}

static bool gen_andes_find(DisasContext *ctx, arg_r *a, int kind)
{
    TCGv dest = dest_gpr(ctx, a->rd);

    gen_helper_andes_find(dest, tcg_constant_i32(kind),
                          get_gpr(ctx, a->rs1, EXT_NONE),
                          get_gpr(ctx, a->rs2, EXT_NONE));
    gen_set_gpr(ctx, a->rd, dest);
    return true;
}

static bool trans_ffb(DisasContext *ctx, arg_ffb *a)
{
    return gen_andes_find(ctx, a, 0);
}

static bool trans_ffzmism(DisasContext *ctx, arg_ffzmism *a)
{
    return gen_andes_find(ctx, a, 1);
}

static bool trans_ffmism(DisasContext *ctx, arg_ffmism *a)
{
    return gen_andes_find(ctx, a, 2);
}

static bool trans_flmism(DisasContext *ctx, arg_flmism *a)
{
    return gen_andes_find(ctx, a, 3);
}

/*
 * EXEC.IT: executes the 32-bit instruction at uitb + imm as if it stood in
 * place of the EXEC.IT, so the link register takes the address after it. The
 * exception is a JAL: its immediate in the table is an address in the 2 MiB
 * the EXEC.IT is in. The table is read as the TB is translated, and a write
 * to uitb flushes the TBs. Andes' own model (andestech/qemu) does the same.
 */
static bool trans_exec_it(DisasContext *ctx, uint32_t opcode)
{
    CPURISCVState *env = cpu_env(ctx->cs);
    target_ulong entry = (env->andes_csr[32] & ~3u) +
        ((((opcode >> 8) & 1) << 11) | (((opcode >> 12) & 1) << 10) |
         (((opcode >> 3) & 1) << 9) | (((opcode >> 9) & 1) << 8) |
         (((opcode >> 5) & 3) << 6) | (((opcode >> 2) & 1) << 5) |
         (((opcode >> 10) & 3) << 3) | (((opcode >> 4) & 1) << 2));
    uint32_t insn = opcode_at(&ctx->base, entry);
    size_t i;

    if ((insn & 3) != 3) {
        return false;
    }
    if ((insn & 0x7f) == 0x6f) {
        target_ulong imm = extract32(insn, 21, 10) << 1 |
                           extract32(insn, 20, 1) << 11 |
                           extract32(insn, 12, 8) << 12 |
                           extract32(insn, 31, 1) << 20;
        target_ulong target = (ctx->base.pc_next & ~0x1fffffu) | imm;

        gen_jal(ctx, extract32(insn, 7, 5), target - ctx->base.pc_next);
        return true;
    }
    for (i = 0; i < ctx->decoders->len; i++) {
        riscv_cpu_decode_fn func = g_ptr_array_index(ctx->decoders, i);

        if (func(ctx, insn)) {
            return true;
        }
    }
    return false;
}
