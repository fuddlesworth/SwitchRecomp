/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the shader translator's files in gpu/shader share */
#ifndef SNX_TRANSLATOR_H
#define SNX_TRANSLATOR_H
#include <string.h>
#include <ctype.h>
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define B(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))
/* text */
typedef struct {
    char* p;
    size_t n, cap;
} Sb;
/* opcode table */
enum {
    O_NONE = 0,
    O_FADD_R,
    O_FADD_C,
    O_FADD_I,
    O_FADD32I,
    O_FMUL_R,
    O_FMUL_C,
    O_FMUL_I,
    O_FMUL32I,
    O_FFMA_R,
    O_FFMA_RC,
    O_FFMA_CR,
    O_FFMA_I,
    O_FFMA32I,
    O_FMNMX_R,
    O_FMNMX_C,
    O_FMNMX_I,
    O_FSETP_R,
    O_FSETP_C,
    O_FSETP_I,
    O_FSET_R,
    O_FSET_C,
    O_FSET_I,
    O_FCMP_R,
    O_FCMP_RC,
    O_FCMP_CR,
    O_FCMP_I,
    O_MUFU,
    O_RRO_R,
    O_RRO_C,
    O_RRO_I,
    O_F2F_R,
    O_F2F_C,
    O_F2F_I,
    O_F2I_R,
    O_F2I_C,
    O_F2I_I,
    O_I2F_R,
    O_I2F_C,
    O_I2F_I,
    O_I2I_R,
    O_I2I_C,
    O_I2I_I,
    O_MOV_R,
    O_MOV_C,
    O_MOV_I,
    O_MOV32I,
    O_IADD_R,
    O_IADD_C,
    O_IADD_I,
    O_IADD32I,
    O_IADD3_R,
    O_IADD3_C,
    O_IADD3_I,
    O_ISCADD_R,
    O_ISCADD_C,
    O_ISCADD_I,
    O_ISCADD32I,
    O_IMNMX_R,
    O_IMNMX_C,
    O_IMNMX_I,
    O_ISETP_R,
    O_ISETP_C,
    O_ISETP_I,
    O_ISET_R,
    O_ISET_C,
    O_ISET_I,
    O_ICMP_R,
    O_ICMP_RC,
    O_ICMP_CR,
    O_ICMP_I,
    O_LOP_R,
    O_LOP_C,
    O_LOP_I,
    O_LOP32I,
    O_LOP3_R,
    O_LOP3_C,
    O_LOP3_I,
    O_SHL_R,
    O_SHL_C,
    O_SHL_I,
    O_SHR_R,
    O_SHR_C,
    O_SHR_I,
    O_SHF_LR,
    O_SHF_LI,
    O_SHF_RR,
    O_SHF_RI,
    O_BFE_R,
    O_BFE_C,
    O_BFE_I,
    O_BFI_R,
    O_BFI_RC,
    O_BFI_CR,
    O_BFI_I,
    O_SEL_R,
    O_SEL_C,
    O_SEL_I,
    O_POPC_R,
    O_POPC_C,
    O_POPC_I,
    O_FLO_R,
    O_FLO_C,
    O_FLO_I,
    O_PRMT_R,
    O_PRMT_RC,
    O_PRMT_CR,
    O_PRMT_I,
    O_XMAD_R,
    O_XMAD_RC,
    O_XMAD_CR,
    O_XMAD_I,
    O_IMUL_R,
    O_IMUL_C,
    O_IMUL_I,
    O_IMUL32I,
    O_IMAD_R,
    O_IMAD_RC,
    O_IMAD_CR,
    O_IMAD_I,
    O_IMAD32I,
    O_LEA_HR,
    O_LEA_HC,
    O_LEA_LR,
    O_LEA_LC,
    O_LEA_LI,
    O_PSETP,
    O_PSET,
    O_P2R_R,
    O_P2R_C,
    O_P2R_I,
    O_R2P_R,
    O_R2P_C,
    O_R2P_I,
    O_CSET,
    O_CSETP,
    O_S2R,
    O_CS2R,
    O_IPA,
    O_ALD,
    O_AST,
    O_OUT_R,
    O_OUT_C,
    O_OUT_I,
    O_LDC,
    O_LDG,
    O_STG,
    O_LDS,
    O_STS,
    O_LDL,
    O_STL,
    O_LD,
    O_ST,
    O_ATOM,
    O_ATOMS,
    O_RED,
    O_ATOM_CAS,
    O_ATOMS_CAS,
    O_TEX,
    O_TEX_B,
    O_TEXS,
    O_TLD,
    O_TLDS,
    O_TLD4,
    O_TLD4S,
    O_TXQ,
    O_TXQ_B,
    O_TMML,
    O_TXD,
    O_SULD,
    O_SUST,
    O_SURED,
    O_SUATOM,
    O_SUATOM_CAS,
    O_BRA,
    O_BRX,
    O_SSY,
    O_SYNC,
    O_PBK,
    O_BRK,
    O_PCNT,
    O_CONT,
    O_EXIT,
    O_KIL,
    O_JMP,
    O_CAL,
    O_RET,
    O_PRET,
    O_PEXIT,
    O_NOP,
    O_BAR,
    O_MEMBAR,
    O_DEPBAR,
    O_VOTE,
    O_VOTE_VTG,
    O_SHFL,
    O_FSWZADD,
    O_FCHK_R,
    O_FCHK_C,
    O_FCHK_I,
    O_HADD2_R,
    O_HADD2_C,
    O_HADD2_I,
    O_HADD2_32I,
    O_HMUL2_R,
    O_HMUL2_C,
    O_HMUL2_I,
    O_HMUL2_32I,
    O_HFMA2_R,
    O_HFMA2_RC,
    O_HFMA2_CR,
    O_HFMA2_I,
    O_HFMA2_32I,
    O_HSETP2_R,
    O_HSETP2_C,
    O_HSETP2_I,
    O_HSET2_R,
    O_HSET2_C,
    O_HSET2_I,
    O_DADD_R,
    O_DADD_C,
    O_DADD_I,
    O_DMUL_R,
    O_DMUL_C,
    O_DMUL_I,
    O_DFMA_R,
    O_DFMA_RC,
    O_DFMA_CR,
    O_DFMA_I,
    O_DSET_R,
    O_DSET_C,
    O_DSET_I,
    O_DSETP_R,
    O_DSETP_C,
    O_DSETP_I,
    O_DMNMX_R,
    O_DMNMX_C,
    O_DMNMX_I,
    O_VMNMX,
    O_VMAD,
    O_VSETP,
    O_VADD,
    O_IDE,
    O_CCTL,
    O_CCTLL,
    O_BPT,
    O_ISBERD,
    O_AL2P,
    O_PIXLD,
    O_TXA,
    O_B2R,
    O_R2B,
    O_LEPC,
    O_SETCRSPTR,
    O_GETCRSPTR,
    O__COUNT
};
typedef struct {
    const char* pat;
    int id;
    const char* name;
    u64 mask, val;
    int bits;
} Op;
/* context */
typedef struct {
    const u64* w;
    u32 nwords;
    ShaderInfo* si;
    int stage;
    Sb out;
    Sb decl;
    u8 used_reg[256];
    u8 used_pred[8];
    int uses_cc;
    u8 in_used[32], out_used[32];
    int uses_local, uses_shared, uses_img_raw;
    int uses_draw_params; /* Vulkan: gl_BaseInstanceARB */
    u8 leader[65536 / 8];
    int max_idx;
    int has_flow;
    int indent;
    u8 gtag[256];      /* reg -> global buffer it holds the base of (index+1) */
    u8 tgt[65536 / 8]; /* branch targets (BRA/SSY/PBK/PCNT and BRX table) */
    u32* pop_tgt;      /* static stack resolution: target of each SYNC/BRK/CONT (0 = unreachable) */
    int stk_static;    /* 1 if SSY/PBK/PCNT got resolved statically (no stack array emitted) */
    int uses_crs;      /* the dynamic CRS stack is used */
    u8 wtag[256];      /* 1: reg = IPA(position.w), 2: reg = RCP of that (same block only) */
    int tag_r, tag_v;  /* label set by the current instruction */
    int nest;          /* structured mode: inside an if, so don't label anything (doesn't dominate the rest) */
    u8 bl2;            /* track_handle found a split handle: its second read */
    u32 bl2_bank, bl2_off;
    int uses_shadow_lod; /* depth compare with an explicit LOD or bias on an array or cube texture */
} Ctx;
#define IDX_OK(i)    ((i) >= 11 && ((i) - 10) % 4 != 0)
/* the CRS stack token SSY/PBK/PCNT push (and SYNC/BRK/CONT pop down to): 0 SSY, 1 PBK, 2 PCNT */
#define CRS_KIND(id) ((id) == O_SSY || (id) == O_SYNC ? 0 : (id) == O_PBK || (id) == O_BRK ? 1 : 2)

/* shader.c */
void sb_put(Sb* s, const char* t);
void sb_f(Sb* s, const char* f, ...);
char* T(const char* f, ...);
char* R(Ctx* c, u32 r);
char* RF(Ctx* c, u32 r);
char* P(Ctx* c, u32 p);
char* PN(Ctx* c, u32 p, int neg);
char* CB(Ctx* c, u32 idx, u32 off);
char* CBF(Ctx* c, u32 idx, u32 off);
void emitl(Ctx* c, const char* f, ...);
void setr(Ctx* c, u32 r, const char* e);
void setrf(Ctx* c, u32 r, const char* e);
void setp(Ctx* c, u32 p, const char* e);
const char* bop(u32 op, const char* a, const char* b);
void setp2(Ctx* c, u32 pa, u32 pb, u32 bo, const char* t, const char* pc);
u32 cb_idx(u64 w);
u32 cb_off(u64 w);
u8 gbuf_tag(Ctx* c, u64 w);
char* imm20f(u64 w);
char* imm20i(u64 w);
char* imm32(u64 w);
char* imm32f(u64 w);
char* opB(Ctx* c, u64 w, int kind);
char* opBf(Ctx* c, u64 w, int kind);
char* fneg_abs(const char* e, int neg, int abs);
char* fsat(const char* e, int sat);
const char* fcmp_expr(u32 op, const char* a, const char* b);
const char* icmp_expr(u32 op, const char* a, const char* b, int sgn);

/* decode.c */
extern int ops_ready;
void ops_init(void);
const Op* decode(u64 w);

/* translate.c */
const char* samp_type(int type, int array, int shadow);
u32 bra_target(u64 w, u32 idx);
int flow_always(u64 w);
const char* flow_cond(Ctx* c, u64 w);
u32 lmem_words(Ctx* c);
void emit_tracked(Ctx* c, u32 idx, u64 w, const Op* op);
void exit_epilogue(Ctx* c);

#endif
