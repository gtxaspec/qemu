/*
 * NDS32 disassembler
 *
 * Copyright (c) 2026 gtxaspec
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "disas/dis-asm.h"
#include "cpu.h"

typedef struct DisasContext {
    disassemble_info *info;
    bfd_vma addr;
} DisasContext;

static const char * const regnames[32] = {
    "$r0", "$r1", "$r2", "$r3", "$r4", "$r5", "$r6", "$r7",
    "$r8", "$r9", "$r10", "$r11", "$r12", "$r13", "$r14", "$r15",
    "$r16", "$r17", "$r18", "$r19", "$r20", "$r21", "$r22", "$r23",
    "$r24", "$r25", "$r26", "$r27", "$fp", "$gp", "$lp", "$sp",
};

#define R(n) regnames[(n) & 31]
#define R45(n) ((n) < 12 ? (n) : (n) + 4)

#define out(ctx, ...) \
    ((ctx)->info->fprintf_func((ctx)->info->stream, __VA_ARGS__))

#include "decode-insn32.c.inc"
#include "decode-insn16.c.inc"

static bool trans_lbi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lbi", R(a->rt), R(a->ra), a->imm * 1);
    return true;
}

static bool trans_lhi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lhi", R(a->rt), R(a->ra), a->imm * 2);
    return true;
}

static bool trans_lwi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lwi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_ldi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "ldi", R(a->rt), R(a->ra), a->imm * 8);
    return true;
}

static bool trans_lbi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "lbi.bi", R(a->rt), R(a->ra), a->imm * 1);
    return true;
}

static bool trans_lhi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "lhi.bi", R(a->rt), R(a->ra), a->imm * 2);
    return true;
}

static bool trans_lwi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "lwi.bi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_ldi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "ldi.bi", R(a->rt), R(a->ra), a->imm * 8);
    return true;
}

static bool trans_sbi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "sbi", R(a->rt), R(a->ra), a->imm * 1);
    return true;
}

static bool trans_shi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "shi", R(a->rt), R(a->ra), a->imm * 2);
    return true;
}

static bool trans_swi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "swi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_sdi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "sdi", R(a->rt), R(a->ra), a->imm * 8);
    return true;
}

static bool trans_sbi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "sbi.bi", R(a->rt), R(a->ra), a->imm * 1);
    return true;
}

static bool trans_shi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "shi.bi", R(a->rt), R(a->ra), a->imm * 2);
    return true;
}

static bool trans_swi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "swi.bi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_sdi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "sdi.bi", R(a->rt), R(a->ra), a->imm * 8);
    return true;
}

static bool trans_lbsi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lbsi", R(a->rt), R(a->ra), a->imm * 1);
    return true;
}

static bool trans_lhsi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lhsi", R(a->rt), R(a->ra), a->imm * 2);
    return true;
}

static bool trans_lwsi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lwsi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_lbsi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "lbsi.bi", R(a->rt), R(a->ra), a->imm * 1);
    return true;
}

static bool trans_lhsi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "lhsi.bi", R(a->rt), R(a->ra), a->imm * 2);
    return true;
}

static bool trans_lwsi_bi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "lwsi.bi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_dprefi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "dprefi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_lbi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, [$gp + %d]", "lbi.gp", R(a->rt), a->imm * 1);
    return true;
}

static bool trans_lbsi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, [$gp + %d]", "lbsi.gp", R(a->rt), a->imm * 1);
    return true;
}

static bool trans_lhi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, [$gp + %d]", "lhi.gp", R(a->rt), a->imm * 2);
    return true;
}

static bool trans_lhsi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, [$gp + %d]", "lhsi.gp", R(a->rt), a->imm * 2);
    return true;
}

static bool trans_shi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, [$gp + %d]", "shi.gp", R(a->rt), a->imm * 2);
    return true;
}

static bool trans_lwi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, [$gp + %d]", "lwi.gp", R(a->rt), a->imm * 4);
    return true;
}

static bool trans_swi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, [$gp + %d]", "swi.gp", R(a->rt), a->imm * 4);
    return true;
}

static bool trans_sbi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, [$gp + %d]", "sbi.gp", R(a->rt), a->imm * 1);
    return true;
}

static bool trans_addi_gp(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, %d", "addi.gp", R(a->rt), a->imm);
    return true;
}

static bool trans_lb(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "lb", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lh(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "lh", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lw(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "lw", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_ld(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "ld", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lb_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "lb.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lh_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "lh.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lw_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "lw.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_ld_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "ld.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sb(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "sb", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sh(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "sh", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sw(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "sw", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sd(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "sd", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sb_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "sb.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sh_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "sh.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sw_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "sw.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sd_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "sd.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lbs(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "lbs", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lhs(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "lhs", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_dpref(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "dpref", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lbs_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "lbs.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lhs_bi(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s], %s << %d",
        "lhs.bi", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_llw(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "llw", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_scw(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "scw", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lbup(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "lbup", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lwup(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "lwup", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_sbup(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "sbup", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_swup(DisasContext *ctx, arg_mem *a)
{
    out(ctx, "%-8s %s, [%s + %s << %d]",
        "swup", R(a->rt), R(a->ra), R(a->rb), a->sv);
    return true;
}

static bool trans_lsmw(DisasContext *ctx, arg_lsmw *a)
{
    static const char * const am[8] = {
        "bi", "bim", "bd", "bdm", "ai", "aim", "ad", "adm"
    };
    out(ctx, "%s.%s %s, [%s], %s, 0x%x",
        (a->s ? "smw" : "lmw"), am[a->abdim], R(a->rb), R(a->ra), R(a->re),
        a->enb4);
    return true;
}

static bool trans_lsmwa(DisasContext *ctx, arg_lsmw *a)
{
    static const char * const am[8] = {
        "bi", "bim", "bd", "bdm", "ai", "aim", "ad", "adm"
    };
    out(ctx, "%s.%s %s, [%s], %s, 0x%x",
        (a->s ? "smw" : "lmw"), am[a->abdim], R(a->rb), R(a->ra), R(a->re),
        a->enb4);
    return true;
}

static bool trans_add_slli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "add.slli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_sub_slli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "sub.slli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_and_slli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "and.slli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_xor_slli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "xor.slli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_or_slli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "or.slli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_nor(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "nor", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_slt(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "slt", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_slts(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "slts", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_slli(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "slli", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_srli(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "srli", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_srai(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "srai", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_rotri(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "rotri", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_sll(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "sll", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_srl(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "srl", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_sra(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "sra", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_rotr(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "rotr", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_seb(DisasContext *ctx, arg_r2 *a)
{
    out(ctx, "%-8s %s, %s", "seb", R(a->rt), R(a->ra));
    return true;
}

static bool trans_seh(DisasContext *ctx, arg_r2 *a)
{
    out(ctx, "%-8s %s, %s", "seh", R(a->rt), R(a->ra));
    return true;
}

static bool trans_bitc(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "bitc", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_zeh(DisasContext *ctx, arg_r2 *a)
{
    out(ctx, "%-8s %s, %s", "zeh", R(a->rt), R(a->ra));
    return true;
}

static bool trans_wsbh(DisasContext *ctx, arg_r2 *a)
{
    out(ctx, "%-8s %s, %s", "wsbh", R(a->rt), R(a->ra));
    return true;
}

static bool trans_or_srli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "or.srli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_divsr(DisasContext *ctx, arg_r4 *a)
{
    out(ctx, "%-8s %s, %s, %s, %s",
        "divsr", R(a->rt), R(a->rd), R(a->ra), R(a->rb));
    return true;
}

static bool trans_divr(DisasContext *ctx, arg_r4 *a)
{
    out(ctx, "%-8s %s, %s, %s, %s",
        "divr", R(a->rt), R(a->rd), R(a->ra), R(a->rb));
    return true;
}

static bool trans_sva(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "sva", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_svs(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "svs", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_cmovz(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "cmovz", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_cmovn(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "cmovn", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_add_srli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "add.srli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_sub_srli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "sub.srli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_and_srli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "and.srli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_xor_srli(DisasContext *ctx, arg_r3s *a)
{
    out(ctx, "%-8s %s, %s, %s, %d",
        "xor.srli", R(a->rt), R(a->ra), R(a->rb), a->sh);
    return true;
}

static bool trans_max(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "max", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_min(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "min", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_ave(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "ave", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_abs(DisasContext *ctx, arg_r2 *a)
{
    out(ctx, "%-8s %s, %s", "abs", R(a->rt), R(a->ra));
    return true;
}

static bool trans_clips(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "clips", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_clip(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "clip", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_clo(DisasContext *ctx, arg_r2 *a)
{
    out(ctx, "%-8s %s, %s", "clo", R(a->rt), R(a->ra));
    return true;
}

static bool trans_clz(DisasContext *ctx, arg_r2 *a)
{
    out(ctx, "%-8s %s, %s", "clz", R(a->rt), R(a->ra));
    return true;
}

static bool trans_bset(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "bset", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_bclr(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "bclr", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_btgl(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "btgl", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_btst(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "btst", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_mfusr(DisasContext *ctx, arg_usr *a)
{
    out(ctx, "%-8s %s, usr#0x%x", "mfusr", R(a->rt), a->usr);
    return true;
}

static bool trans_mtusr(DisasContext *ctx, arg_usr *a)
{
    out(ctx, "%-8s %s, usr#0x%x", "mtusr", R(a->rt), a->usr);
    return true;
}

static bool trans_mul(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "mul", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_mults64(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "mults64", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_mult64(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "mult64", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_madds64(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "madds64", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_madd64(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "madd64", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_msubs64(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "msubs64", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_msub64(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "msub64", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_divs(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "divs", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_div(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "div", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_mult32(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "mult32", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_madd32(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "madd32", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_msub32(DisasContext *ctx, arg_dt3 *a)
{
    out(ctx, "%-8s $d%d, %s, %s", "msub32", a->dt, R(a->ra), R(a->rb));
    return true;
}

static bool trans_mulsr64(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "mulsr64", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_mulr64(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "mulr64", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_maddr32(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "maddr32", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_msubr32(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "msubr32", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_movi(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, %d", "movi", R(a->rt), a->imm);
    return true;
}

static bool trans_sethi(DisasContext *ctx, arg_ri *a)
{
    out(ctx, "%-8s %s, 0x%x", "sethi", R(a->rt), a->imm);
    return true;
}

static bool trans_j(DisasContext *ctx, arg_j *a)
{
    out(ctx, "%-8s 0x%x", "j", (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_jal(DisasContext *ctx, arg_j *a)
{
    out(ctx, "%-8s 0x%x", "jal", (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_jr(DisasContext *ctx, arg_jreg *a)
{
    out(ctx, "%-8s %s", "jr", R(a->rb));
    return true;
}

static bool trans_jral(DisasContext *ctx, arg_jreg *a)
{
    out(ctx, "%-8s %s, %s", "jral", R(a->rt), R(a->rb));
    return true;
}

static bool trans_jrnez(DisasContext *ctx, arg_jreg *a)
{
    out(ctx, "%-8s %s", "jrnez", R(a->rb));
    return true;
}

static bool trans_jralnez(DisasContext *ctx, arg_jreg *a)
{
    out(ctx, "%-8s %s, %s", "jralnez", R(a->rt), R(a->rb));
    return true;
}

static bool trans_beq(DisasContext *ctx, arg_br1 *a)
{
    out(ctx, "%-8s %s, %s, 0x%x",
        "beq", R(a->rt), R(a->ra), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bne(DisasContext *ctx, arg_br1 *a)
{
    out(ctx, "%-8s %s, %s, 0x%x",
        "bne", R(a->rt), R(a->ra), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_ifcall(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "ifcall", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_beqz(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "beqz", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bnez(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "bnez", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bgez(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "bgez", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bltz(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "bltz", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bgtz(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "bgtz", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_blez(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "blez", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bgezal(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "bgezal", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bltzal(DisasContext *ctx, arg_brr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "bltzal", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_addi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "addi", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_subri(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "subri", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_andi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "andi", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_xori(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "xori", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_ori(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "ori", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_beqc(DisasContext *ctx, arg_br3 *a)
{
    out(ctx, "%-8s %s, %d, 0x%x",
        "beqc", R(a->rt), a->cimm, (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bnec(DisasContext *ctx, arg_br3 *a)
{
    out(ctx, "%-8s %s, %d, 0x%x",
        "bnec", R(a->rt), a->cimm, (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_slti(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "slti", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_sltsi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "sltsi", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_bitci(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "bitci", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_standby(DisasContext *ctx, arg_misc *a)
{
    out(ctx, "%s", "standby");
    return true;
}

static bool trans_cctl(DisasContext *ctx, arg_misc *a)
{
    out(ctx, "%s", "cctl");
    return true;
}

static bool trans_mfsr(DisasContext *ctx, arg_sr *a)
{
    out(ctx, "%-8s %s, sr#0x%x", "mfsr", R(a->rt), a->sr);
    return true;
}

static bool trans_mtsr(DisasContext *ctx, arg_sr *a)
{
    out(ctx, "%-8s %s, sr#0x%x", "mtsr", R(a->rt), a->sr);
    return true;
}

static bool trans_setend(DisasContext *ctx, arg_setend *a)
{
    out(ctx, "%s", a->e ? "setend.b" : "setend.l");
    return true;
}

static bool trans_setgie(DisasContext *ctx, arg_setend *a)
{
    out(ctx, "%s", a->e ? "setgie.e" : "setgie.d");
    return true;
}

static bool trans_iret(DisasContext *ctx, arg_misc *a)
{
    out(ctx, "%s", "iret");
    return true;
}

static bool trans_trap(DisasContext *ctx, arg_swid *a)
{
    out(ctx, "%-8s #0x%x", "trap", a->swid);
    return true;
}

static bool trans_teqz(DisasContext *ctx, arg_rt_swid *a)
{
    out(ctx, "%-8s %s, #0x%x", "teqz", R(a->rt), a->swid);
    return true;
}

static bool trans_tnez(DisasContext *ctx, arg_rt_swid *a)
{
    out(ctx, "%-8s %s, #0x%x", "tnez", R(a->rt), a->swid);
    return true;
}

static bool trans_dsb(DisasContext *ctx, arg_misc *a)
{
    out(ctx, "%s", "dsb");
    return true;
}

static bool trans_isb(DisasContext *ctx, arg_misc *a)
{
    out(ctx, "%s", "isb");
    return true;
}

static bool trans_break(DisasContext *ctx, arg_swid *a)
{
    out(ctx, "%-8s #0x%x", "break", a->swid);
    return true;
}

static bool trans_syscall(DisasContext *ctx, arg_swid *a)
{
    out(ctx, "%-8s #0x%x", "syscall", a->swid);
    return true;
}

static bool trans_msync(DisasContext *ctx, arg_misc *a)
{
    out(ctx, "%s", "msync");
    return true;
}

static bool trans_isync(DisasContext *ctx, arg_rt1 *a)
{
    out(ctx, "%-8s %s", "isync", R(a->rt));
    return true;
}

static bool trans_tlbop(DisasContext *ctx, arg_misc *a)
{
    out(ctx, "%s", "tlbop");
    return true;
}

static bool trans_ffb(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "ffb", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_ffmism(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "ffmism", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_flmism(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "flmism", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_ffzmism(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "ffzmism", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_ffbi(DisasContext *ctx, arg_rri *a)
{
    out(ctx, "%-8s %s, %s, %d", "ffbi", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_pbsad(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "pbsad", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_pbsada(DisasContext *ctx, arg_r3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "pbsada", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_beqz38(DisasContext *ctx, arg_hbr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "beqz38", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bnez38(DisasContext *ctx, arg_hbr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "bnez38", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_movi55(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "movi55", R(a->rt), a->imm);
    return true;
}

static bool trans_add45(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "add45", R(R45(a->rt)), R(a->ra));
    return true;
}

static bool trans_sub45(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "sub45", R(R45(a->rt)), R(a->ra));
    return true;
}

static bool trans_addi45(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "addi45", R(R45(a->rt)), a->imm);
    return true;
}

static bool trans_subi45(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "subi45", R(R45(a->rt)), a->imm);
    return true;
}

static bool trans_srai45(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "srai45", R(R45(a->rt)), a->imm);
    return true;
}

static bool trans_srli45(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "srli45", R(R45(a->rt)), a->imm);
    return true;
}

static bool trans_slli333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, %s, %d", "slli333", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_add333(DisasContext *ctx, arg_h3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "add333", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_sub333(DisasContext *ctx, arg_h3 *a)
{
    out(ctx, "%-8s %s, %s, %s", "sub333", R(a->rt), R(a->ra), R(a->rb));
    return true;
}

static bool trans_addi333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, %s, %d", "addi333", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_subi333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, %s, %d", "subi333", R(a->rt), R(a->ra), a->imm);
    return true;
}

static bool trans_zeb33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "zeb33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_zeh33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "zeh33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_seb33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "seb33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_seh33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "seh33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_xlsb33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "xlsb33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_x11b33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "x11b33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_bmski33(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "bmski33", R(a->rt), a->imm);
    return true;
}

static bool trans_fexti33(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "fexti33", R(a->rt), a->imm);
    return true;
}

static bool trans_lwi333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lwi333", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_lwi333_bi(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "lwi333.bi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_lhi333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lhi333", R(a->rt), R(a->ra), a->imm * 2);
    return true;
}

static bool trans_lbi333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lbi333", R(a->rt), R(a->ra), a->imm * 1);
    return true;
}

static bool trans_swi333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "swi333", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_swi333_bi(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, [%s], %d", "swi333.bi", R(a->rt), R(a->ra), a->imm * 4);
    return true;
}

static bool trans_shi333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "shi333", R(a->rt), R(a->ra), a->imm * 2);
    return true;
}

static bool trans_sbi333(DisasContext *ctx, arg_hri *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "sbi333", R(a->rt), R(a->ra), a->imm * 1);
    return true;
}

static bool trans_addri36_sp(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, [$sp + %d]", "addri36.sp", R(a->rt), a->imm * 4);
    return true;
}

static bool trans_lwi45_fe(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, [$r8 + %d]",
        "lwi45.fe", R(R45(a->rt)), (a->imm - 32) * 4);
    return true;
}

static bool trans_lwi450(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "lwi450", R(R45(a->rt)), R(a->ra));
    return true;
}

static bool trans_swi450(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "swi450", R(R45(a->rt)), R(a->ra));
    return true;
}

static bool trans_lwi37(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lwi37", R(a->rt), "$fp", a->imm * 4);
    return true;
}

static bool trans_swi37(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "swi37", R(a->rt), "$fp", a->imm * 4);
    return true;
}

static bool trans_slts45(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "slts45", R(R45(a->rt)), R(a->ra));
    return true;
}

static bool trans_slt45(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "slt45", R(R45(a->rt)), R(a->ra));
    return true;
}

static bool trans_sltsi45(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "sltsi45", R(R45(a->rt)), a->imm);
    return true;
}

static bool trans_slti45(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "slti45", R(R45(a->rt)), a->imm);
    return true;
}

static bool trans_beqzs8(DisasContext *ctx, arg_himm *a)
{
    out(ctx, "%-8s 0x%x", "beqzs8", (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_bnezs8(DisasContext *ctx, arg_himm *a)
{
    out(ctx, "%-8s 0x%x", "bnezs8", (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_addi10_sp(DisasContext *ctx, arg_himm *a)
{
    out(ctx, "%-8s %d", "addi10.sp", a->imm);
    return true;
}

static bool trans_lwi37_sp(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "lwi37.sp", R(a->rt), "$sp", a->imm * 4);
    return true;
}

static bool trans_swi37_sp(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, [%s + %d]", "swi37.sp", R(a->rt), "$sp", a->imm * 4);
    return true;
}

static bool trans_ifcall9(DisasContext *ctx, arg_himm *a)
{
    out(ctx, "%-8s 0x%x", "ifcall9", (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_movpi45(DisasContext *ctx, arg_hi *a)
{
    out(ctx, "%-8s %s, %d", "movpi45", R(R45(a->rt)), a->imm + 16);
    return true;
}

static bool trans_movd44(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "movd44", R(a->rt * 2), R(a->ra * 2));
    return true;
}

static bool trans_push25(DisasContext *ctx, arg_hpp25 *a)
{
    out(ctx, "%-8s %s, %d",
        "push25", R(((const uint8_t[]){6, 8, 10, 14})[a->re]), a->imm * 8);
    return true;
}

static bool trans_pop25(DisasContext *ctx, arg_hpp25 *a)
{
    out(ctx, "%-8s %s, %d",
        "pop25", R(((const uint8_t[]){6, 8, 10, 14})[a->re]), a->imm * 8);
    return true;
}

static bool trans_neg33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "neg33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_not33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "not33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_mul33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "mul33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_xor33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "xor33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_and33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "and33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_or33(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "or33", R(a->rt), R(a->ra));
    return true;
}

static bool trans_ifret16(DisasContext *ctx, arg_hnone *a)
{
    out(ctx, "%s", "ifret16");
    return true;
}

static bool trans_mov55(DisasContext *ctx, arg_h2 *a)
{
    out(ctx, "%-8s %s, %s", "mov55", R(a->rt), R(a->ra));
    return true;
}

static bool trans_j8(DisasContext *ctx, arg_himm *a)
{
    out(ctx, "%-8s 0x%x", "j8", (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_beqs38(DisasContext *ctx, arg_hbr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "beqs38", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

static bool trans_break16(DisasContext *ctx, arg_himm *a)
{
    out(ctx, "%-8s #%d", "break16", a->imm);
    return true;
}

static bool trans_ex9_it(DisasContext *ctx, arg_himm *a)
{
    out(ctx, "%-8s #%d", "ex9.it", a->imm);
    return true;
}

static bool trans_jr5(DisasContext *ctx, arg_hra *a)
{
    out(ctx, "%-8s %s", "jr5", R(a->ra));
    return true;
}

static bool trans_jral5(DisasContext *ctx, arg_hra *a)
{
    out(ctx, "%-8s %s", "jral5", R(a->ra));
    return true;
}

static bool trans_ex9_it5(DisasContext *ctx, arg_himm *a)
{
    out(ctx, "%-8s #%d", "ex9.it5", a->imm);
    return true;
}

static bool trans_ret5(DisasContext *ctx, arg_hra *a)
{
    out(ctx, "%-8s %s", "ret5", R(a->ra));
    return true;
}

static bool trans_add5_pc(DisasContext *ctx, arg_hra *a)
{
    out(ctx, "%-8s %s", "add5.pc", R(a->ra));
    return true;
}

static bool trans_bnes38(DisasContext *ctx, arg_hbr *a)
{
    out(ctx, "%-8s %s, 0x%x",
        "bnes38", R(a->rt), (uint32_t)(ctx->addr + (a->imm << 1)));
    return true;
}

int print_insn_nds32(bfd_vma addr, disassemble_info *info)
{
    DisasContext ctx = { .info = info, .addr = addr };
    bfd_byte buf[4];
    uint32_t insn;
    int len;

    if (info->read_memory_func(addr, buf, 2, info)) {
        info->memory_error_func(-1, addr, info);
        return -1;
    }
    insn = bfd_getb16(buf);
    if (insn & 0x8000) {
        len = 2;
        if (!decode_insn16(&ctx, insn)) {
            out(&ctx, ".hword   0x%04x", insn);
        }
        return len;
    }
    if (info->read_memory_func(addr + 2, buf + 2, 2, info)) {
        info->memory_error_func(-1, addr + 2, info);
        return -1;
    }
    insn = bfd_getb32(buf);
    if (!decode_insn32(&ctx, insn)) {
        out(&ctx, ".word    0x%08x", insn);
    }
    return 4;
}
