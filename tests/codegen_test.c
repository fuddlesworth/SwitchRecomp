/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic ISA/CFG/module tests. The generated ABI fixture is compiled by the
   supplied host Clang, then checks static and interpreted architectural results. */
#include "common/cpu/aarch64/a64.h"
#include "common/cpu/recompiler/codegen.h"
#include "nx/loader/codegen_view.h"
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void put32(unsigned char* image, size_t offset, SnxU32 word) {
    unsigned i;
    for (i = 0; i < 4; ++i) image[offset + i] = (unsigned char)(word >> (8 * i));
}

static void decoder_test(void) {
    static const SnxU32 invalid[] = {0,
                                     0xFFFFFFFFUL,
                                     0x1200FC00UL,
                                     0x52C00000UL,
                                     0x53400000UL,
                                     0x13808000UL,
                                     0xF8600800UL,
                                     0x0A008000UL,
                                     0x0BC00000UL,
                                     0x0B201400UL,
                                     0x1E180000UL,
                                     0x1EE60020UL, /* General-register half FMOV requires later FP16. */
                                     0x1EE70020UL,
                                     0x9EE60020UL,
                                     0x9EE70020UL};
    static const SnxU32 backwards[] = {0x17FFFFFFUL, 0x97FFFFFFUL, 0x54FFFFE0UL, 0xB4FFFFE0UL, 0x3607FFE0UL};
    size_t i;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        check(snx_a64_decode(invalid[i])->kind == SNX_FLOW_UNDEF, "reject reserved/invalid encoding");
    check(snx_a64_decode(0x1EE24020UL)->kind == SNX_FLOW_PLAIN, "A57 FCVT S,H remains allocated");
    check(snx_a64_decode(0x1EE2C020UL)->kind == SNX_FLOW_PLAIN, "A57 FCVT D,H remains allocated");
    check(snx_a64_decode(0x1E23C020UL)->kind == SNX_FLOW_PLAIN, "A57 FCVT H,S remains allocated");
    check(snx_a64_decode(0x1E63C020UL)->kind == SNX_FLOW_PLAIN, "A57 FCVT H,D remains allocated");
    check(snx_a64_decode(0xD503201FUL)->kind == SNX_FLOW_PLAIN, "NOP decode");
    check(snx_a64_decode(0xD65F03C0UL)->kind == SNX_FLOW_RET, "RET decode");
    check(snx_a64_decode(0x94000002UL)->kind == SNX_FLOW_BL, "BL decode");
    for (i = 0; i < sizeof(backwards) / sizeof(backwards[0]); ++i) {
        check(snx_a64_target(backwards[i], 0x1000) == 0xFFC, "sign-extend negative branch offset");
        check(snx_a64_target(backwards[i], 0) == ~(SnxU64)3, "wrap negative branch target to 64 bits");
    }
}

static int has_leader(const SnxFunction* function, SnxU64 address) {
    size_t i;
    for (i = 0; i < function->count; ++i)
        if (snx_function_address(function, i) == address) return function->leaders[i] != 0;
    return 0;
}

static void setup_program(SnxProgram* program, SnxModuleImage* module, unsigned char* image, size_t size) {
    memset(program, 0, sizeof(*program));
    memset(module, 0, sizeof(*module));
    module->name = "synthetic";
    module->image = image;
    module->image_size = size;
    module->base = SNX_CODE_BASE;
    module->size = 0x1000;
    module->text_size = (SnxU32)size;
    program->modules = module;
    program->module_count = 1;
    program->npdm.title_id = ((SnxU64)0x12345678UL << 32) | 0x9ABCDEF0UL;
    program->npdm.stack = 0x100000;
    program->npdm.prio = 44;
    program->npdm.core = 2;
}

static void analysis_test(void) {
    static const SnxU32 words[] = {0xAA1E03F3UL, 0x91000400UL, 0x9400000AUL, 0xF1002C1FUL, 0x540000A1UL,
                                   0x91000800UL, 0xAA1303FEUL, 0xD65F03C0UL, 0xD503201FUL, 0x91019000UL,
                                   0x17FFFFFCUL, 0xD4200000UL, 0x91002800UL, 0xD65F03C0UL};
    unsigned char image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde ranges[2];
    SnxU64 interior, symbol;
    const SnxFunction* function;
    size_t i;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    ranges[0].start = 0;
    ranges[0].size = 44;
    ranges[1].start = 20;
    ranges[1].size = 8;
    module.fde = ranges;
    module.fde_count = 2;
    symbol = 20;
    module.functions = &symbol;
    module.function_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze FDE and BL target");
    check(codegen.function_count == 2, "overlapping FDE discarded, BL function discovered");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && function->addresses == NULL && function->count == 11, "FDE uses a compact address range");
    if (function) {
        check(!has_leader(function, SNX_CODE_BASE + 20), "discarded FDE start is not an extra symbol seed");
        check(has_leader(function, SNX_CODE_BASE + 12), "BL continuation leader");
        check(has_leader(function, SNX_CODE_BASE + 24), "backwards B target leader");
        check(has_leader(function, SNX_CODE_BASE + 36), "conditional target leader");
    }
    check(snx_codegen_function(&codegen, SNX_CODE_BASE + 48) != NULL, "recursive BL target exists");
    snx_codegen_free(&codegen);
    interior = SNX_CODE_BASE + 20;
    check(!snx_nx_codegen_analyze(&codegen, &program, &interior, 1, 1), "analyze explicit interior hint");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && has_leader(function, interior), "execution hint adds interior dispatch entry");
    snx_codegen_free(&codegen);
}

static void large_range_test(void) {
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde range;
    unsigned char* image;
    size_t i, size;
    size = 1024 * 1024;
    image = (unsigned char*)snx_alloc(size);
    for (i = 0; i < size; i += 4) put32(image, i, 0xD503201FUL);
    put32(image, size - 4, 0xD65F03C0UL);
    setup_program(&program, &module, image, size);
    range.start = 0;
    range.size = size;
    module.fde = &range;
    module.fde_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze a large FDE");
    check(codegen.function_count == 1 && codegen.functions[0].addresses == NULL &&
              codegen.functions[0].count == size / 4 && codegen.functions[0].leader_count == 1,
          "large FDE stays a range, without one address allocation per instruction");
    snx_codegen_free(&codegen);
    free(image);
}

static void emit_semantic_fixture(FILE* out) {
    static const SnxU32 words[] = {
        0x31000420UL, /* ADDS W0,W1,#1: zero-extension and NZCV. */
        0xBA1F03E0UL, /* ADCS X0,XZR,XZR: integer carry input. */
        0xDA1F03E0UL, /* SBC X0,XZR,XZR: 64-bit complement of zero. */
        0xD3485C20UL, /* UBFX X0,X1,#8,#16. */
        0x93401C20UL, /* SXTB X0,W1. */
        0x93C12020UL, /* ROR X0,X1,#8. */
        0x9E220020UL, /* SCVTF S0,X1: direct int64 -> float32 rounding. */
        0x9E660020UL, /* FMOV X0,D1: preserve NaN payload bits. */
        0x1E620C20UL, /* FCSEL D0,D1,D2,EQ: preserve selected bits. */
        0x4EA28420UL, /* ADD V0.4S,V1.4S,V2.4S. */
        0x4E22CC20UL, /* FMLA V0.4S,V1.4S,V2.4S. */
        0x4E040C20UL, /* DUP V0.4S,W1. */
        0x4E183C20UL, /* UMOV X0,V1.D[1]. */
        0x5FA21820UL, /* FMLA S0,S1,V2.S[3]: scalar upper-lane clearing. */
        0xF94007E0UL, /* LDR X0,[SP,#8]. */
        0xF900043FUL, /* STR XZR,[X1,#8]. */
        0x17FFFFFFUL, /* B pc-4. */
        0x1200FC00UL, /* Invalid logical immediate traps in both paths. */
        0x1E221820UL, /* FDIV S0,S1,S2: architectural invalid NaN and IOC. */
        0x1F020C20UL, /* FMADD S0,S1,S2,S3: addend-first NaN priority. */
        0x1E22C020UL, /* FCVT D0,S1: inspect signalling before widening. */
        0x1E200020UL, /* FCVTNS W0,S1: QNaN is invalid for integer conversion. */
        0x4E22D420UL, /* FADD V0.4S,V1.4S,V2.4S: AOT obeys guest rounding. */
        0x5FA21820UL, /* FMLA S0,S1,V2.S[3]: by-element SNaN priority. */
        0x1E63C020UL, /* FCVT H0,D1: alternative half infinity conversion. */
        0x9E580020UL, /* FCVTZS X0,D1,#64: scaling before integer saturation. */
        0x1E61C020UL, /* FSQRT D0,D1: directed rounding and IXC. */
        0x1E228820UL, /* FNMUL S0,S1,S2: negate the architectural NaN result. */
        0x1E23C020UL, /* FCVT H0,S1: rounds in the FPCR mode, here toward plus infinity. */
        0x1E63C020UL, /* FCVT H0,D1: a negative overflow toward plus infinity is the largest finite. */
        0x0E216820UL, /* FCVTN V0.4H,V1.4S toward zero: truncates, overflows finite, keeps -0. */
        0x1E23C020UL, /* FCVT H0,S1: FZ flushes a denormal operand before rounding, and sets IDC. */
        0x1F420C20UL, /* FMADD D0,D1,D2,D3: inf*0 gives the default NaN. */
        0x1F620C20UL, /* FNMADD D0,D1,D2,D3: of two quiet NaNs, the first (negated). */
        0x4E62CC20UL, /* FMLA V0.2D,V1.2D,V2.2D: the same, per lane. */
        0x7EE1D820UL, /* FRSQRTE D0,D1: FZ flushes a denormal operand, so +inf and IDC. */
        0x4EA1D820UL, /* FRECPE V0.4S,V1.4S: the same per lane, with the sign. */
        0x4EA1D820UL, /* FRECPE V0.4S,V1.4S without FZ: denormals get an estimate. */
        0x4EA1D820UL, /* FRECPE V0.4S,V1.4S under FZ: from 2^126 up the estimate is a zero, UFC. */
        0x4EA1D820UL, /* FRECPE V0.4S,V1.4S under RP: a tiny operand overflows to +inf or -max, OFC and IXC. */
        0x4EE1D820UL, /* FRECPE V0.2D,V1.2D under RZ: to the largest finite. */
        0x5EE1D820UL, /* FRECPE D0,D1 under FZ: from 2^1022 up, a zero. */
        0x4EA1D820UL, /* FRECPE V0.4S,V1.4S under RM: -inf or +max. */
        0x9E630020UL, /* UCVTF D0,X1: 0 is +0 under round toward minus infinity. */
        0x6E61D820UL, /* UCVTF V0.2D,V1.2D: the same per lane; 2^63+1 rounds down. */
        0x9E630020UL  /* UCVTF D0,X1: 2^63+1 rounds up, once, toward plus infinity. */
    };
    size_t i;
    char function[32];
    fputs("#include \"cpu.h\"\n#include \"cpu_helpers.c\"\n#include <stdio.h>\nGuestFn snx_interp_entry;\n", out);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        sprintf(function, "step_%lu", (unsigned long)i);
        snx_a64_static_step(out, words[i], 0x1000, function);
    }
    snx_a64_interp(out, 1);
    fputs("typedef u64(*Step)(CPU*);\nstatic const Step steps[]={", out);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) fprintf(out, "%sstep_%lu", i ? "," : "", (unsigned long)i);
    fputs("};\nstatic const u32 words[]={", out);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        fprintf(out, "%s0x%08lXu", i ? "," : "", (unsigned long)words[i]);
    fputs("};\nstatic int traps;\nstatic void undefined(CPU*c,u64 pc,u32 w){(void)c;(void)pc;(void)w;++traps;}\n", out);
    fputs("int main(void){ unsigned mode,i,j; CPU c; RtAPI rt; u64 pc,value; u8 memory[64]; int ok;\n", out);
    fputs(
        " memset(&rt,0,sizeof(rt));rt.undef=undefined;\n for(mode=0;mode<2;mode++)for(i=0;i<sizeof(words)/sizeof(words[0]);i++){\n",
        out);
    fputs(" memset(&c,0,sizeof(c));memset(memory,0xAA,sizeof(memory));c.mem=memory;c.rt=&rt;traps=0;\n switch(i){\n",
          out);
    fputs(" case 0:c.x[1]=0xFFFFFFFFULL;break; case 1:c.cf=1;break; case 2:c.cf=0;break;\n", out);
    fputs(" case 3:c.x[1]=0x12345678;break;case 4:c.x[1]=0x80;break;case 5:c.x[1]=0x123456789ABCDEF0ULL;break;\n", out);
    fputs(" case 6:c.x[1]=16777217;break;case 7:c.v[1].d[0]=0x7FF8000000001234ULL;break;\n", out);
    fputs(" case 8:c.z=1;c.v[1].d[0]=0x7FF8000000001234ULL;c.v[2].d[0]=0x3FF0000000000000ULL;break;\n", out);
    fputs(" case 9:for(j=0;j<4;j++){c.v[1].s[j]=0xFFFFFFFFu;c.v[2].s[j]=j+1;}break;\n", out);
    fputs(" case 10:for(j=0;j<4;j++){c.v[0].f[j]=3;c.v[1].f[j]=4;c.v[2].f[j]=5;}break;\n", out);
    fputs(" case 11:c.x[1]=0x123456789ABCDEF0ULL;break;case 12:c.v[1].d[1]=0xFEDCBA9876543210ULL;break;\n", out);
    fputs(" case 13:c.v[0].d[1]=~0ULL;c.v[0].f[0]=3;c.v[1].f[0]=4;c.v[2].f[3]=5;break;\n", out);
    fputs(" case 14:c.x[31]=16;value=0x123456789ABCDEF0ULL;memcpy(memory+24,&value,8);break;\n", out);
    fputs(" case 15:c.x[1]=16;c.x[31]=0xFFFFFFFF;break;case 18:c.v[2].s[0]=0x80000000;break;\n", out);
    fputs(" case 19:c.v[1].s[0]=0xFFC00456;c.v[2].s[0]=0x7FC00789;c.v[3].s[0]=0x7FC00123;break;\n", out);
    fputs(" case 20:c.v[1].s[0]=0x7F800456;break;case 21:c.x[0]=~0ULL;c.v[1].s[0]=0xFFC00123;break;\n", out);
    fputs(" case 22:c.fpcr=1u<<22;for(j=0;j<4;j++){c.v[1].s[j]=0x3F800000;c.v[2].s[j]=0x33800000;}break;\n", out);
    fputs(
        " case 23:c.v[0].s[0]=0x7FC00123;c.v[0].d[1]=~0ULL;c.v[1].s[0]=0xFF800456;c.v[2].s[3]=0x7FC00ABC;c.fpsr=0x08000010;break;\n",
        out);
    fputs(" case 24:c.fpcr=1u<<26;c.v[1].d[0]=0x7FF0000000000000ULL;break;\n", out);
    fputs(
        " case 25:c.v[1].d[0]=0x3FE0000000000000ULL;break;case 26:c.fpcr=3u<<22;c.v[1].d[0]=0x4000000000000000ULL;break;\n",
        out);
    fputs(" case 27:c.v[2].s[0]=0x7F800000;break;\n", out);
    /* FCVT and FCVTN to half: the FPCR mode, overflow and FZ */
    fputs(" case 28:c.fpcr=0x400000;c.v[1].s[0]=0x3F800001u;break;\n", out);
    fputs(" case 29:c.fpcr=0x400000;c.v[1].d[0]=0xC202A05F20000000ULL;break;\n", out);
    fputs(" case 30:c.fpcr=0xC00000;c.v[0].d[0]=c.v[0].d[1]=~0ULL;c.v[1].s[0]=0x3F801FFFu;c.v[1].s[1]=0x477FF000u;\n",
          out);
    fputs(" c.v[1].s[2]=0x33000001u;c.v[1].s[3]=0x80000000u;break;case 31:c.fpcr=0x1400000;c.v[1].s[0]=1;break;\n",
          out);
    /* fused multiply-add NaNs in f64 */
    fputs(" case 32:c.v[1].d[0]=0x7FF0000000000000ULL;c.v[3].d[0]=0x3FF0000000000000ULL;break;\n", out);
    fputs(" case 33:c.v[1].d[0]=0x7FF8000000000000ULL;c.v[2].d[0]=~0ULL;c.v[3].d[0]=0x3FF0000000000000ULL;break;\n",
          out);
    fputs(" case 34:c.v[0].d[0]=c.v[0].d[1]=0x3FF0000000000000ULL;c.v[1].d[0]=0x7FF0000000000000ULL;\n", out);
    fputs(" c.v[1].d[1]=0x7FF8000000000001ULL;c.v[2].d[1]=0xFFF8000000000002ULL;break;\n", out);
    /* FRECPE and FRSQRTE: FZ and the FPCR mode */
    fputs(" case 35:c.fpcr=0x1000000;c.v[1].d[0]=1;break;\n", out);
    fputs(" case 36:case 37:c.fpcr=i==36?0x1000000:0;c.v[1].s[0]=0x00400000u;c.v[1].s[1]=0x80400000u;\n", out);
    fputs(" c.v[1].s[2]=0x007FFFFFu;c.v[1].s[3]=0x3F800000u;break;\n", out);
    fputs(" case 38:c.fpcr=0x1000000;c.v[1].s[0]=0x7E800000u;c.v[1].s[1]=0xFF000000u;c.v[1].s[2]=0x7E7FFFFFu;\n", out);
    fputs(" c.v[1].s[3]=0x3F800000u;break;case 39:c.fpcr=0x400000;c.v[1].s[0]=1;c.v[1].s[1]=0x80000001u;\n", out);
    fputs(" c.v[1].s[2]=0x001FFFFFu;c.v[1].s[3]=0x00200000u;break;case 40:c.fpcr=0xC00000;c.v[1].d[0]=1;\n", out);
    fputs(
        " c.v[1].d[1]=0x7FD0000000000000ULL;break;case 41:c.fpcr=0x1000000;c.v[1].d[0]=0xFFD0000000000000ULL;break;\n",
        out);
    fputs(" case 42:c.fpcr=0x800000;c.v[1].s[0]=1;c.v[1].s[1]=0x80000001u;c.v[1].s[2]=0x7E800000u;\n", out);
    fputs(" c.v[1].s[3]=0x3F800000u;break;\n", out);
    /* UCVTF of 0 and of 2^63+1 in the directed modes */
    fputs(" case 43:c.fpcr=0x800000;break;case 44:c.fpcr=0x800000;c.v[1].d[1]=0x8000000000000001ULL;break;\n", out);
    fputs(" case 45:c.fpcr=0x400000;c.x[1]=0x8000000000000001ULL;break;\n", out);
    fputs(" default:break;}\n", out);
    fputs(" pc=mode?snx_step(&c,words[i],0x1000):steps[i](&c);ok=pc==(i==16?0xFFC:i==17?0x1000:0x1004);\n switch(i){\n",
          out);
    fputs(" case 0:ok&=c.x[0]==0&&c.z==1&&c.cf==1&&!c.n&&!c.vf;break;case 1:ok&=c.x[0]==1;break;\n", out);
    fputs(" case 2:ok&=c.x[0]==~0ULL;break;case 3:ok&=c.x[0]==0x3456;break;case 4:ok&=c.x[0]==~127ULL;break;\n", out);
    fputs(
        " case 5:ok&=c.x[0]==0xF0123456789ABCDEULL;break;case 6:ok&=c.v[0].s[0]==0x4B800000u&&!c.v[0].d[1]&&c.fpsr==0x10;break;\n",
        out);
    fputs(
        " case 7:ok&=c.x[0]==0x7FF8000000001234ULL;break;case 8:ok&=c.v[0].d[0]==0x7FF8000000001234ULL&&!c.v[0].d[1];break;\n",
        out);
    fputs(" case 9:for(j=0;j<4;j++)ok&=c.v[0].s[j]==j;break;case 10:for(j=0;j<4;j++)ok&=c.v[0].f[j]==23;break;\n", out);
    fputs(
        " case 11:for(j=0;j<4;j++)ok&=c.v[0].s[j]==0x9ABCDEF0u;break;case 12:ok&=c.x[0]==0xFEDCBA9876543210ULL;break;\n",
        out);
    fputs(
        " case 13:ok&=c.v[0].f[0]==23&&!c.v[0].s[1]&&!c.v[0].d[1];break;case 14:ok&=c.x[0]==0x123456789ABCDEF0ULL;break;\n",
        out);
    fputs(" case 15:memcpy(&value,memory+24,8);ok&=value==0&&c.x[31]==0xFFFFFFFF;break;case 17:ok&=traps==1;break;\n",
          out);
    fputs(
        " case 18:ok&=c.v[0].s[0]==0x7FC00000&&c.fpsr==1;break;case 19:ok&=c.v[0].s[0]==0x7FC00123&&c.fpsr==0;break;\n",
        out);
    fputs(" case 20:ok&=c.v[0].d[0]==0x7FF8008AC0000000ULL&&c.fpsr==1;break;case 21:ok&=c.x[0]==0&&c.fpsr==1;break;\n",
          out);
    fputs(" case 22:for(j=0;j<4;j++)ok&=c.v[0].s[j]==0x3F800001;ok&=c.fpsr==0x10;break;\n", out);
    fputs(" case 23:ok&=c.v[0].s[0]==0xFFC00456&&!c.v[0].s[1]&&!c.v[0].d[1]&&c.fpsr==0x08000011;break;\n", out);
    fputs(
        " case 24:ok&=c.v[0].d[0]==0x7FFF&&!c.v[0].d[1]&&c.fpsr==1;break;case 25:ok&=c.x[0]==0x7FFFFFFFFFFFFFFFULL&&c.fpsr==1;break;\n",
        out);
    fputs(
        " case 26:ok&=c.v[0].d[0]==0x3FF6A09E667F3BCCULL&&c.fpsr==0x10;break;case 27:ok&=c.v[0].s[0]==0xFFC00000&&c.fpsr==1;break;\n",
        out);
    fputs(" case 28:ok&=c.v[0].d[0]==0x3C01&&!c.v[0].d[1];break;case 29:ok&=c.v[0].d[0]==0xFBFF&&!c.v[0].d[1];break;\n",
          out);
    fputs(" case 30:ok&=c.v[0].d[0]==0x800000007BFF3C00ULL&&!c.v[0].d[1];break;\n", out);
    fputs(" case 31:ok&=!c.v[0].d[0]&&!c.v[0].d[1]&&(c.fpsr&0x80);break;\n", out);
    fputs(" case 32:ok&=c.v[0].d[0]==0x7FF8000000000000ULL&&!c.v[0].d[1];break;\n", out);
    fputs(" case 33:ok&=c.v[0].d[0]==0xFFF8000000000000ULL&&!c.v[0].d[1];break;\n", out);
    fputs(" case 34:ok&=c.v[0].d[0]==0x7FF8000000000000ULL&&c.v[0].d[1]==0x7FF8000000000001ULL;break;\n", out);
    fputs(" case 35:ok&=c.v[0].d[0]==0x7FF0000000000000ULL&&!c.v[0].d[1]&&(c.fpsr&0x80);break;\n", out);
    fputs(" case 36:ok&=c.v[0].d[0]==0xFF8000007F800000ULL&&c.v[0].d[1]==0x3F7F80007F800000ULL&&(c.fpsr&0x80);break;\n",
          out);
    fputs(" case 37:ok&=c.v[0].d[0]==0xFEFF80007EFF8000ULL&&c.v[0].d[1]==0x3F7F80007E800000ULL&&!c.fpsr;break;\n", out);
    fputs(" case 38:ok&=c.v[0].d[0]==0x8000000000000000ULL&&c.v[0].d[1]==0x3F7F800000800000ULL&&c.fpsr==8;break;\n",
          out);
    fputs(" case 39:ok&=c.v[0].d[0]==0xFF7FFFFF7F800000ULL&&c.v[0].d[1]==0x7F7F80007F800000ULL&&c.fpsr==0x14;break;\n",
          out);
    fputs(" case 40:ok&=c.v[0].d[0]==0x7FEFFFFFFFFFFFFFULL&&c.v[0].d[1]==0x000FF80000000000ULL&&c.fpsr==0x14;break;\n",
          out);
    fputs(" case 41:ok&=c.v[0].d[0]==0x8000000000000000ULL&&!c.v[0].d[1]&&c.fpsr==8;break;\n", out);
    fputs(" case 42:ok&=c.v[0].d[0]==0xFF8000007F7FFFFFULL&&c.v[0].d[1]==0x3F7F8000007FC000ULL&&c.fpsr==0x14;break;\n",
          out);
    fputs(" case 43:ok&=!c.v[0].d[0]&&!c.v[0].d[1];break;\n", out);
    fputs(" case 44:ok&=!c.v[0].d[0]&&c.v[0].d[1]==0x43E0000000000000ULL;break;\n", out);
    fputs(" case 45:ok&=c.v[0].d[0]==0x43E0000000000001ULL&&!c.v[0].d[1];break;\n", out);
    fputs(" default:break;}\n", out);
    fputs(" if(!ok||(i!=17&&traps)){fprintf(stderr,\"semantic case %u mode %u failed\\n\",i,mode);return 1;}}\n", out);
    fputs(" puts(\"Static/interpreted scalar, FP, SIMD, memory and trap semantics passed\");return 0;}\n", out);
}

static void semantics_test(const char* compiler, const char* repo, const char* directory) {
    const char* args[16];
    char *source, *binary, *include, *root, *output;
    FILE* file;
    int n, status;
    source = snx_path_join(directory, "semantics.c");
#ifdef _WIN32
    binary = snx_path_join(directory, "semantics.exe");
#else
    binary = snx_path_join(directory, "semantics");
#endif
    include = snx_path_join(repo, "src/common/cpu/aarch64");
    root = snx_path_join(repo, "src"); /* what cpu.h includes */
    output = NULL;
    file = snx_fopen(source, "wb");
    check(file != NULL, "open ISA semantics fixture");
    if (!file) goto done;
    emit_semantic_fixture(file);
    check(!fclose(file), "write ISA semantics fixture");
    n = 0;
    args[n++] = compiler;
    args[n++] = "-O2";
    args[n++] = "-fno-strict-aliasing";
    args[n++] = "-fwrapv";
#if defined(__x86_64__) || defined(_M_X64)
    args[n++] = "-mcx16";
#endif
    args[n++] = "-Wno-unused-label";
    args[n++] = "-I";
    args[n++] = include;
    args[n++] = "-I";
    args[n++] = root;
    args[n++] = source;
#ifndef _WIN32
    args[n++] = "-lm";
#endif
    args[n++] = "-o";
    args[n++] = binary;
    args[n] = NULL;
    status = snx_run(args, directory, NULL, &output);
    if (status && output) fputs(output, stderr);
    check(!status, "compile emitted ISA semantics against the runtime ABI");
    free(output);
    output = NULL;
    if (!status) {
        args[0] = binary;
        args[1] = NULL;
        status = snx_run(args, directory, NULL, &output);
        if (output) fputs(output, status ? stderr : stdout);
        check(!status, "execute static/interpreted architectural expectations");
    }
done:
    free(source);
    free(binary);
    free(include);
    free(root);
    free(output);
}

/* A loop of generated code runs with CPU.poll set: its backward branch calls rt->poll, the runtime's chance to take
   the core from a thread that makes no SVC, and the loop still runs to its end. The same loop through the
   interpreter, as a loop the recompiler never found runs, polls on its taken branch. */
static void poll_test(const char* compiler, const char* repo, const char* directory) {
    static const SnxU32 words[] = {0xF1000400UL,  /* SUBS X0,X0,#1 */
                                   0x54FFFFE1UL,  /* B.NE back to it */
                                   0xD65F03C0UL}; /* RET */
    unsigned char image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde range;
    const SnxFunction* function;
    const char* args[16];
    char *source, *binary, *include, *root, *output = NULL;
    FILE* file;
    size_t i;
    int n, status;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    range.start = 0;
    range.size = sizeof(image);
    module.fde = &range;
    module.fde_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze the poll loop");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function != NULL, "the poll loop is one function");
    source = snx_path_join(directory, "poll.c");
#ifdef _WIN32
    binary = snx_path_join(directory, "poll.exe");
#else
    binary = snx_path_join(directory, "poll");
#endif
    include = snx_path_join(repo, "src/common/cpu/aarch64");
    root = snx_path_join(repo, "src"); /* what cpu.h includes */
    file = function ? snx_fopen(source, "wb") : NULL;
    check(file != NULL, "open the poll fixture");
    if (!file) goto done;
    fputs("#include \"cpu.h\"\n#include \"cpu_helpers.c\"\n#include <stdio.h>\nGuestFn snx_interp_entry;\n", file);
    snx_codegen_emit(file, &codegen, function);
    snx_a64_interp(file, 0);
    fputs("static int polls;\nstatic void poll(CPU* c){ ++polls; __atomic_store_n(&c->poll,0,__ATOMIC_RELAXED); }\n",
          file);
    fputs("static int run(u64 count, u32 flag){ CPU c; RtAPI rt; memset(&c,0,sizeof c); memset(&rt,0,sizeof rt);\n"
          " rt.poll=poll; c.rt=&rt; c.x[0]=count; c.x[30]=0x2000; c.poll=flag; polls=0; F_",
          file);
    snx_a64_hex(file, SNX_CODE_BASE);
    fputs("(&c,", file);
    snx_a64_literal(file, SNX_CODE_BASE);
    fputs(");\n return c.x[0]==0 && c.pc==0x2000 ? polls : -1; }\n", file);
    /* the loop's words in guest memory at 0x1000, no dispatch table: every lookup lands in the interpreter */
    fputs("static int interp(u64 count, u32 flag){ static u32 mem[0x1000]; CPU c; RtAPI rt;\n"
          " memset(&c,0,sizeof c); memset(&rt,0,sizeof rt); mem[0x400]=0xF1000400u; mem[0x401]=0x54FFFFE1u;\n"
          " mem[0x402]=0xD65F03C0u; snx_interp_entry=snx_interp; rt.poll=poll; c.rt=&rt; c.mem=(u8*)mem;\n"
          " c.x[0]=count; c.x[30]=0x2000; c.poll=flag; polls=0; snx_interp(&c,0x1000);\n"
          " return c.x[0]==0 && c.pc==0x2000 ? polls : -1; }\n",
          file);
    fputs("int main(void){ int a=run(3,1), b=run(5,0), d=interp(3,1), e=interp(5,0);\n"
          " if(a!=1||b!=0){ fprintf(stderr,\"poll loop: %d polls with the flag, %d without\\n\",a,b); return 1; }\n"
          " if(d!=1||e!=0){ fprintf(stderr,\"interpreted poll loop: %d polls with the flag, %d without\\n\",d,e);"
          " return 1; }\n"
          " puts(\"A loop's backward branch calls rt->poll once when CPU.poll is set, compiled or interpreted\");"
          " return 0; }\n",
          file);
    check(!fclose(file), "write the poll fixture");
    n = 0;
    args[n++] = compiler;
    args[n++] = "-O2";
    args[n++] = "-fno-strict-aliasing";
    args[n++] = "-fwrapv";
#if defined(__x86_64__) || defined(_M_X64)
    args[n++] = "-mcx16";
#endif
    args[n++] = "-Wno-unused-label";
    args[n++] = "-I";
    args[n++] = include;
    args[n++] = "-I";
    args[n++] = root;
    args[n++] = source;
#ifndef _WIN32
    args[n++] = "-lm";
#endif
    args[n++] = "-o";
    args[n++] = binary;
    args[n] = NULL;
    status = snx_run(args, directory, NULL, &output);
    if (status && output) fputs(output, stderr);
    check(!status, "compile the poll fixture");
    free(output);
    output = NULL;
    if (!status) {
        args[0] = binary;
        args[1] = NULL;
        status = snx_run(args, directory, NULL, &output);
        if (output) fputs(output, status ? stderr : stdout);
        check(!status, "a loop with CPU.poll set calls rt->poll and runs to its end");
    }
done:
    snx_codegen_free(&codegen);
    free(source);
    free(binary);
    free(include);
    free(root);
    free(output);
}

int main(int argc, char** argv) {
    char* directory;
    if (argc != 3) return 2;
    directory = snx_temp_dir(NULL);
    check(directory != NULL, "create codegen test directory");
    if (!directory) return 1;
    decoder_test();
    analysis_test();
    large_range_test();
    semantics_test(argv[1], argv[2], directory);
    poll_test(argv[1], argv[2], directory);
    check(!snx_remove_tree(directory), "remove codegen test directory");
    free(directory);
    printf("Native codegen: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
