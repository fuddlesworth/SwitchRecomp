/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED     0x001F8000FC0007E0ull
#define EXIT      0xE30000000007000Full
#define F2I       0x5CB0118000070A00ull
#define FMUL_FTZ  0x5C68100000070000ull
#define FMUL_KEEP 0x5C68000000070000ull
#define FADD_KEEP 0x5C58000000070000ull
#define FMUL_IMM  0x3868003F00070000ull
#define FFMA_FMZ  0x59C0000000070000ull
#define FFMA_KEEP 0x5980000000070000ull

static int failures;
static const char* storage_paths[2];
static const char* nro_paths[4];
static const char* issue126_paths[17];
static const char* stage_paths[3];
static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static u32* translate_with_rt_mask(const u64* instructions, int count, int stage, ShaderInfo* info, size_t* words,
                                   int flush, u32 rt_mask) {
    u64 code[64] = {0};
    int at = 10;
    for (int i = 0; i <= count; i++) {
        if ((at - 10) % 4 == 0) code[at++] = SCHED;
        code[at++] = i < count ? instructions[i] : EXIT;
    }
    memset(info, 0, sizeof *info);
    info->rt_int_mask = rt_mask;
    if (stage == ST_VS) info->clip_enable = rt_mask;
    if (stage == ST_CS) {
        info->cs_local[0] = info->cs_local[1] = info->cs_local[2] = 1;
        info->shared_size = 16;
    }
    return shader_to_spirv((const u8*)code, sizeof code, stage, info, words, flush, 1, 1);
}
static u32* translate(const u64* instructions, int count, int stage, ShaderInfo* info, size_t* words, int flush) {
    return translate_with_rt_mask(instructions, count, stage, info, words, flush, 0);
}

static int count_opcode(const u32* code, size_t words, u32 opcode, u32 operand) {
    int found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return -1;
        u32 value_index = op == 17 ? 1 : 2;
        if (op == opcode && (operand == 0xffffffffu || (width > value_index && code[i + value_index] == operand)))
            found++;
        i += width;
    }
    return found;
}

static void save_fixture(const char* path, const u32* code, size_t words) {
    if (!path || !code) return;
    FILE* out = fopen(path, "wb");
    check(out && fwrite(code, sizeof *code, words, out) == words, "shader fixture written");
    if (out) fclose(out);
}

static int has_glsl_abs(const u32* code, size_t words) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 12 && width >= 6 && code[i + 4] == 4) return 1; /* GLSL.std.450 FAbs */
        i += width;
    }
    return 0;
}

static int abs_feeds_negate(const u32* code, size_t words) {
    u32 abs_id = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 12 && width >= 6 && code[i + 4] == 4) abs_id = code[i + 2];
        if ((code[i] & 0xffff) == 127 && width >= 4 && code[i + 3] == abs_id && abs_id) return 1;
        i += width;
    }
    return 0;
}

static void f2i_modifiers(void) {
    const u64 cases[] = {F2I | (1ull << 45), F2I | (1ull << 49), F2I | (1ull << 45) | (1ull << 49)};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&cases[i], 1, ST_FS, &info, &words, 1);
        check(code && !info.unsupported, "F2I modifier translates");
        if (code) {
            check((count_opcode(code, words, 127, 0xffffffffu) > 0) == (i != 1), "F2I bit 45 negates");
            check(has_glsl_abs(code, words) == (i != 0), "F2I bit 49 takes absolute value");
            if (i == 2) check(abs_feeds_negate(code, words), "F2I applies absolute value before negation");
        }
        free(code);
    }
}

static void global_buffers(void) {
    static const struct {
        u64 instructions[3];
        int count;
        u32 offset;
        int write;
    } cases[] = {
        {{0xEF9500007307FF00ull, 0xEED4200000070000ull}, 2, 0x730, 0},
        {{0x4C1080001CC70100ull, 0x4C1008001CD7FF01ull, 0xEEDC200000070002ull}, 3, 0x730, 1},
        {{0x4C98078014470002ull, 0xEED4200000070200ull}, 2, 0x510, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_FS, &info, &words, 1);
        if (!code || info.unsupported || info.ngbuf != 1 || info.gbuf_cbuf_off[0] != cases[i].offset ||
            !!(info.gbuf_wmask & 1) != cases[i].write)
            fprintf(stderr, "global case %zu: %s, buffers %d, offset %x, writes %x\n", i, info.first_unsupported,
                    info.ngbuf, info.gbuf_cbuf_off[0], info.gbuf_wmask);
        check(code && !info.unsupported && info.ngbuf == 1 && info.gbuf_cbuf_off[0] == cases[i].offset &&
                  !!(info.gbuf_wmask & 1) == cases[i].write,
              "global buffer found at first use");
        free(code);
    }
    const u64 unused[] = {0x4C98078018870002ull};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(unused, 1, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && info.ngbuf == 0, "unused constant is not a global buffer");
    free(code);
}

static void float_controls(void) {
    const u64 cases[][2] = {{FMUL_FTZ, FMUL_FTZ},
                            {FMUL_FTZ, FADD_KEEP},
                            {FMUL_KEEP, FMUL_KEEP},
                            {FFMA_FMZ, FFMA_FMZ},
                            {FFMA_FMZ, FFMA_KEEP}};
    for (int i = 0; i < 5; i++) {
        for (int available = 0; available < 2; available++) {
            ShaderInfo info;
            size_t words = 0;
            u32* code = translate(cases[i], 2, ST_FS, &info, &words, available);
            check(code && !info.unsupported, "FP32 mode translates");
            if (code) {
                int flush = (i == 0 || i == 3) && available;
                check(info.fp32_flush == (i != 2) && info.fp32_preserve == (i == 1 || i == 2 || i == 4),
                      "FP32 modes recorded");
                check((count_opcode(code, words, 17, 4465) > 0) == flush, "FP32 flush capability");
                int mode = 0;
                for (size_t k = 5; k < words;) {
                    u32 width = code[k] >> 16;
                    if (!width || k + width > words) break;
                    if ((code[k] & 0xffff) == 16 && width >= 4 && code[k + 2] == 4460) mode = 1;
                    k += width;
                }
                check(mode == flush, "FP32 flush execution mode");
            }
            free(code);
        }
    }
}

static void precise_math(void) {
    const u64 math[] = {FMUL_KEEP, FMUL_IMM};
    const int stages[] = {ST_VS, ST_FS, ST_CS};
    for (size_t i = 0; i < sizeof stages / sizeof *stages; i++) {
        int stage = stages[i];
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(math, 2, stage, &info, &words, 1);
        if (!code || info.unsupported) fprintf(stderr, "precise stage %d: %s\n", stage, info.first_unsupported);
        check(code && !info.unsupported, "precise float math translates");
        if (code) check(count_opcode(code, words, 71, 42) >= 2, "float math has NoContraction");
        free(code);
    }
}

/* F2F between two halves with bit 42 rounds to an integer: DREDGE's colour grading takes frac() of a half as x minus
   F2F.F16.F16.FLOOR of it, and without the floor every colour came out grey */
static int glsl_ext_count(const u32* code, size_t words, u32 inst) {
    int found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return -1;
        if (op == 12 && width >= 5 && code[i + 4] == inst) found++;
        i += width;
    }
    return found;
}
static void f2f_half_rounding(void) {
    static const struct {
        u64 insn;
        u32 glsl; /* GLSL.std.450: 1 Round, 2 RoundEven, 3 Trunc, 8 Floor, 9 Ceil */
        const char* what;
    } cases[] = {
        {0x5ca8068000270508ull, 8, "F2F.F16.F16.FLOOR of the high half floors"},
        {0x5ca8048000570507ull, 8, "F2F.F16.F16.FLOOR of the low half floors"},
        {0x5ca8068000270508ull & ~(1ull << 39), 2, "F2F.F16.F16.ROUND rounds to even"},
        {0x5ca8068000270508ull | (1ull << 40), 3, "F2F.F16.F16.TRUNC truncates"},
        {0x5ca8068000270508ull & ~(1ull << 39) | (1ull << 40), 9, "F2F.F16.F16.CEIL rounds up"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&cases[i].insn, 1, ST_FS, &info, &words, 1);
        check(code && !info.unsupported, "F2F between halves translates");
        if (code) check(glsl_ext_count(code, words, cases[i].glsl) > 0, cases[i].what);
        free(code);
    }
    /* without bit 42 it stays a plain move */
    u64 plain = 0x5ca8068000270508ull & ~(1ull << 42);
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(&plain, 1, ST_FS, &info, &words, 1);
    check(code && !info.unsupported, "F2F between halves without rounding translates");
    if (code) check(glsl_ext_count(code, words, 8) == 0, "F2F between halves without bit 42 does not floor");
    free(code);
}

/* HFMA2 with an immediate B (DREDGE's fragment shaders): the two halves of B are 9-bit immediates at 20 and 30 with
   their signs at 29 and 56, C is the register at 39 with its swizzle at 53 */
static int has_constant(const u32* code, size_t words, u32 value) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return 0;
        if (op == 43 && width == 4 && code[i + 3] == value) return 1;
        i += width;
    }
    return 0;
}
static int has_glsl_fma(const u32* code, size_t words) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return 0;
        if (op == 12 && width >= 5 && code[i + 4] == 50) return 1;
        i += width;
    }
    return 0;
}
static void hfma2_immediate(void) {
    static const struct {
        u64 insn;
        u32 imm;
    } cases[] = {
        {0x704384c611870b05ull, 0x46004600u}, /* HFMA2 R5, R11.H1_H1, 6, 6, R9.H0_H0 */
        {0x716287c230871206ull, 0xc200c200u}, /* HFMA2 R6, R18.H0_H0, -3, -3, R15.H1_H1 */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&cases[i].insn, 1, ST_FS, &info, &words, 1);
        if (!code || info.unsupported) fprintf(stderr, "HFMA2 immediate: %s\n", info.first_unsupported);
        check(code && !info.unsupported, "HFMA2 with an immediate translates");
        if (code) {
            check(has_constant(code, words, cases[i].imm), "HFMA2's immediate halves keep their values and signs");
            check(has_glsl_fma(code, words), "HFMA2 with an immediate is a fused multiply-add");
        }
        free(code);
    }
}

static void fragment_storage_images(void) {
    static const struct {
        u64 instructions[2];
        u8 kind;
        u32 opcode;
    } cases[] = {
        {{0x4C98078804A70002ull, 0xEB20010600F70004ull}, 2, 99},
        {{0x4C98078804A70001ull, 0xEB50008602070A00ull}, 1, 239},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, 2, ST_FS, &info, &words, 1);
        check(code && !info.unsupported && info.nimg == 1, "fragment storage image translates");
        if (code) {
            save_fixture(storage_paths[i], code, words);
            check(info.img[0].kind == cases[i].kind && info.img[0].bindless && info.img[0].bl_cbuf == 2 &&
                      info.img[0].cbuf_off == 74,
                  "fragment storage image resolves its handle and integer format");
            check(count_opcode(code, words, cases[i].opcode, 0xffffffffu) > 0,
                  "fragment storage image emits the write or atomic operation");
            if (cases[i].kind == 1)
                check(count_opcode(code, words, 60, 0xffffffffu) > 0, "image atomic uses a texel pointer");
        }
        free(code);
    }
    ShaderInfo alternate;
    size_t words = 0;
    u32* code = translate_with_rt_mask(cases[0].instructions, 2, ST_FS, &alternate, &words, 1, 1u << 8);
    check(code && !alternate.unsupported && alternate.nimg == 1 && alternate.img[0].kind == 0,
          "fragment surface store selects a float image from its runtime format key");
    free(code);
    const u64 clamped[] = {cases[1].instructions[0], cases[1].instructions[1] | (1ull << 49)};
    code = translate(clamped, 2, ST_FS, &alternate, &words, 1);
    check(!code && alternate.unsupported, "unsupported surface atomic clamp mode is rejected");
    free(code);
}

/* w without its predicate guard, with these destination, coordinate and handle registers */
static u64 unguarded(u64 w, u32 d, u32 a, u32 handle) {
    return (w & ~(0xffull | 0xffull << 8 | 0xfull << 16 | 0xffull << 39)) | d | (u64)a << 8 | 7ull << 16 |
           (u64)handle << 39;
}
static u64 mov32i(u32 reg, u32 value) { return 0x0100000000000000ull | ((u64)value << 20) | 0x71000ull | reg; }

static int brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    *value = offset == 0 ? 16 : 1024; /* second word belongs to another constant, not this jump table */
    return 1;
}

static int clamped_brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    *value = offset < 8 ? 24 : 1024;
    return 1;
}

static int invalid_brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset) return 0;
    *value = 1024;
    return 1;
}

static void brx_length(void) {
    u64 code[64] = {0};
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0xef90000000070001ull; /* LDC R1, c[0][0] */
    code[12] = 0xe25000000007010full; /* BRX R1 */
    code[13] = 0xe30000000007000full;
    code[14] = code[10];
    code[15] = code[13];
    u32 targets[64], len = 0;
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = brx_table;
    shader_hash((const u8*)code, sizeof code, &len);
    int count = shader_brx_targets(code, 12, 64, targets, 64);
    check(count == 1 && targets[0] == 15, "BRX table stops at out-of-range trailing constant");
    check(len == 16 * 8, "BRX program length ends after its last reachable exit");
    g_shader_cb_read = invalid_brx_table;
    check(shader_brx_targets(code, 12, 64, targets, 64) == -1, "BRX rejects a table with no valid targets");
    memset(code, 0, sizeof code);
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0x3820000000170b0bull; /* IMNMX R11, R11, 1: two entries */
    code[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    code[13] = 0xef94000000070d0dull; /* LDC R13, c[0][R13] */
    code[14] = code[10];
    code[15] = 0xe250000000070d0full; /* BRX R13 */
    code[16] = 0xe30000000007000full;
    code[18] = code[10];
    code[19] = code[16];
    g_shader_cb_read = clamped_brx_table;
    shader_hash((const u8*)code, sizeof code, &len);
    count = shader_brx_targets(code, 15, 64, targets, 64);
    check(count == 2 && targets[0] == 19 && targets[1] == 19,
          "BRX reads only the entries permitted by its IMNMX clamp");
    check(len == 20 * 8, "clamped BRX bounds program length before padding");
    memset(code, 0, sizeof code);
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0xe250000000070d0full; /* dynamic BRX with no readable jump table */
    code[12] = 0xe30000000007000full;
    g_shader_cb_read = NULL;
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 13 * 8, "unresolved BRX stops at the exit before an all-zero tail");
    code[15] = code[12];
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 16 * 8, "unresolved BRX keeps nonzero instructions beyond an earlier exit");
    g_shader_cb_read = old;
}

static void nro_shader_modes(void) {
    const u64 raw[] = {0x4c98078800070004ull, mov32i(4, 1), mov32i(5, 3), 0xeb18048600470400ull};
    const u64 ms[] = {0x4c98078800070004ull, mov32i(5, 4), mov32i(0, 3), mov32i(1, 5), 0xdd3c0007a0470000ull};
    const u64 ms_array[] = {0x4c98078800070004ull, mov32i(5, 2), mov32i(0, 3),
                            mov32i(1, 5),          mov32i(2, 5), 0xdd3c0007b0470000ull};
    const u64 exchange[] = {mov32i(2, 1), mov32i(3, 2), 0xef5d00000007ff02ull, 0xec8000002027ff02ull};
    struct {
        const u64* instructions;
        int count;
        u8 flags;
    } cases[] = {{raw, 4, 1}, {ms, 5, 2}, {ms_array, 6, 2}, {exchange, 4, 0}};
    for (int i = 0; i < 4; i++) {
        ShaderInfo info = {0};
        info.shared_size = 16;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_CS, &info, &words, 1);
        check(code && !info.unsupported, "NRO shader mode translates");
        if (code) {
            if (i < 3)
                check(info.ntex == 1 && (info.tex[0].flags & 3) == cases[i].flags, "NRO texture mode is recorded");
            save_fixture(nro_paths[i], code, words);
        }
        free(code);
    }
}

static void issue126_shader_modes(void) {
    const u64 intmath[] = {
        0x1f00000000070000ull | (7ull << 20) | (1ull << 8),                               /* IMUL32I low */
        0x1f00000000070002ull | (1ull << 53) | (1ull << 54) | (3ull << 20) | (1ull << 8), /* signed high */
        0x8000000000070004ull | (3ull << 20) | (1ull << 8),                               /* IMAD32I */
    };
    const u64 clip[] = {0xeff000000007ff00ull | (0x2c0ull << 20)};
    const u64 bindless[] = {
        0x4c98078800070004ull, /* MOV R4, c[0][...] supplies the texture handle */
        0xdea0000000070008ull | (6ull << 28) | (1ull << 37) | (4ull << 20), /* TEX.B cube LZ */
    };
    const u64 indexed_handle[] = {
        0xef94002000070004ull, /* LDC R4, c[0][R0 + 0x20] */
        0xdd380007a0470000ull, /* TLD.B using the handle loaded above */
    };
    const u64 tex3d[] = {0x4c98078800070004ull, 0xdea0000000070008ull | (4ull << 28) | (3ull << 37) | (4ull << 20)};
    const u64 cube_array_bias[] = {0x4c98078800070004ull,
                                   0xdea0000000070008ull | (7ull << 28) | (2ull << 37) | (4ull << 20)};
    const u64 tex1d_array[] = {0x4c98078800070004ull,
                               0xdea0000000070008ull | (1ull << 28) | (1ull << 37) | (4ull << 20)};
    const u64 split_handle[] = {
        0x4c98078800070004ull,
        0x4c98078800470005ull,
        0x5c40000000070006ull | (4ull << 8) | (5ull << 20) | (1ull << 41), /* LOP.OR R6, R4, R5 */
        0xdea0000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    const u64 dynamic_3d_offset[] = {0x4c98078800070004ull,
                                     0xdea0000000070008ull | (4ull << 28) | (1ull << 36) | (4ull << 20)};
    /* Ender Magnolia ORs the sampler half in from a constant buffer operand. */
    const u64 split_cbuf_handle[] = {
        0x4c98078805c70006ull, /* MOV R6, c[0x2][0x170] */
        0x4c47020815c70606ull, /* LOP.OR R6, R6, c[0x2][0x570] */
        0xdea0000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    /* Ender Magnolia's buffer images: SULD.D.32, SUATOM.ADD and SUST.P on the handle MOV R4, c[0x2][0x128] */
    const u64 buffer_surfaces[] = {
        0x4c98078804a70004ull,
        unguarded(0xeb10020200480370ull, 8, 0, 4),
        (unguarded(0xea70070200980b0aull, 9, 0, 4) & ~(0xffull << 20)) | (1ull << 20),
        unguarded(0xeb203a0200f77614ull, 12, 0, 4),
    };
    /* TXQ.B: the handle in R4, the level in R5 */
    const u64 bindless_query[] = {(0x4c98078805a7001eull & ~0xffull) | 4, 0x5c9807800ff70005ull, 0xdf50000180470400ull};
    /* a 2D texture offset from a register the translator can't resolve: the coordinates move instead */
    const u64 dynamic_2d_offset[] = {0x4c98078800070004ull,
                                     0xdea0000000070008ull | (2ull << 28) | (1ull << 36) | (4ull << 20)};
    /* the lane ID after a branch: lanes reconverge on the subgroup's lowest pc */
    const u64 lane_after_branch[] = {0xe24000000000000full | (16ull << 20), FMUL_KEEP | 5, 0xf0c8000000070003ull};
    /* a clip distance and flow: every OpVariable before the clip distance's store */
    const u64 clip_flow[] = {0xe24000000007000full, 0xeff000000007ff00ull | (0x2c0ull << 20)};
    /* Single-register writers and a TEX.B masked to one result write just below the handle. */
    const u64 handle_past_writers[] = {
        0x4c98078800070006ull,
        FMUL_KEEP | 5,
        FFMA_KEEP | 3,
        0xdea0000000070004ull | (2ull << 28) | (1ull << 37) | (6ull << 20) | (1ull << 31),
        0xdea0000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    /* edenprobe's indexed handle: the table index from an attribute, shifted to a 16-byte stride, then LDC */
    const u64 indexed_table[] = {
        0xe003ff870ff7ff00ull, /* IPA R0, a[0x70] */
        0x5cb0118000070a00ull, /* F2I.U32 R0, R0 */
        0x3848000000470000ull, /* SHL R0, R0, 4 */
        0xef94002000070004ull, /* LDC R4, c[2][R0] */
        0x5c9807800ff70000ull, /* MOV R0, RZ */
        0x5c9807800ff70001ull, /* MOV R1, RZ */
        0xdd380007a0470000ull, /* TLD.B using the handle loaded above */
    };
    struct {
        const u64* instructions;
        int count, stage;
        u32 mask;
        u32 opcode;
    } cases[] = {{intmath, 3, ST_FS, 0, 151},
                 {clip, 1, ST_VS, 1, 65},
                 {bindless, 2, ST_FS, 0, 88},
                 {indexed_handle, 2, ST_FS, 0, 95},
                 {tex3d, 2, ST_FS, 0, 88},
                 {cube_array_bias, 2, ST_FS, 0, 87},
                 {tex1d_array, 2, ST_FS, 0, 88},
                 {split_handle, 4, ST_FS, 0, 88},
                 {dynamic_3d_offset, 2, ST_FS, 0, 103},
                 {split_cbuf_handle, 3, ST_FS, 0, 88},
                 {handle_past_writers, 5, ST_FS, 0, 88},
                 {buffer_surfaces, 4, ST_CS, 0, 98},
                 {bindless_query, 3, ST_FS, 0, 103},
                 {clip_flow, 2, ST_VS, 1, 65},
                 {dynamic_2d_offset, 2, ST_FS, 0, 103},
                 {lane_after_branch, 3, ST_FS, 0, 354},
                 {indexed_table, 7, ST_FS, 0, 95}};
    for (int i = 0; i < 17; i++) {
        ShaderInfo info;
        size_t words = 0;
        g_shader_texture_arrays = i == 16; /* the device's non-uniform indexing, for the table */
        u32* code = translate_with_rt_mask(cases[i].instructions, cases[i].count, cases[i].stage, &info, &words, 1,
                                           cases[i].mask);
        check(code && !info.unsupported, "issue 126 shader translates");
        if (code) {
            check(count_opcode(code, words, cases[i].opcode, 0xffffffffu) > 0,
                  "issue 126 emits the expected SPIR-V instruction");
            if (i == 1) check(info.uses_clip_mask == 1, "enabled clip distance is reported");
            if (i == 7) check(info.ntex == 1 && info.tex[0].bl2, "split bindless handle retains both descriptor reads");
            if (i == 9)
                check(info.ntex == 1 && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0x170 / 4 &&
                          info.tex[0].bl2 && info.tex[0].bl2_cbuf == 2 && info.tex[0].bl2_off == 0x570 / 4,
                      "a handle ORed with a constant buffer operand keeps both reads");
            if (i == 10) check(info.ntex == 1, "a handle is tracked past writes to the registers below it");
            if (i == 11)
                check(info.nimg == 2 && info.img[0].type == 1 && info.img[0].kind == 1 && info.img[1].type == 1 &&
                          info.img[1].kind == 2,
                      "buffer loads and atomics share an R32ui image, stores take a typeless one");
            if (i == 12) check(info.ntex == 1 && info.tex[0].bindless, "TXQ.B queries its tracked handle");
            if (i == 16)
                check(info.ntex == 1 && info.tex[0].flags == (SNX_TEX_INDEXED | 4 << 4) &&
                          count_opcode(code, words, 17, 5307) == 1,
                      "an indexed handle samples an element of the texture array, 16 bytes apart");
            save_fixture(issue126_paths[i], code, words);
        }
        free(code);
    }
    g_shader_texture_arrays = 0;
    /* A TEX.B with all four results from R4 overwrites the handle in R6. */
    const u64 clobbered_handle[] = {
        0x4c98078800070006ull,
        0xdea0000000070004ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
        0xdea0000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(clobbered_handle, 3, ST_FS, &info, &words, 1);
    check(!code && info.unsupported && !strcmp(info.first_unsupported, "untracked texture handle"),
          "a handle overwritten by a texture result is not tracked");
    free(code);
    /* A handle loaded under P0 reaches a TEX.B under P0, and not one under !P0. */
    const u64 tex_b = 0xdea0000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20);
    const u64 guarded[] = {0x4c98078800000006ull, tex_b & ~(0xfull << 16)};
    code = translate(guarded, 2, ST_FS, &info, &words, 1);
    check(code && !info.unsupported, "a handle loaded under a predicate reaches a use under the same one");
    free(code);
    const u64 other_guard[] = {0x4c98078800000006ull, (tex_b & ~(0xfull << 16)) | (8ull << 16)};
    code = translate(other_guard, 2, ST_FS, &info, &words, 1);
    check(!code && info.unsupported, "a handle loaded under P0 doesn't reach a use under !P0");
    free(code);
    /* A handle loaded before a forward branch reaches its target, unless the skipped code writes it. */
    const u64 branch = 0xe24000000000000full | (16ull << 20); /* @P0 BRA over the next instruction */
    const u64 across[] = {0x4c98078800070006ull, branch, FMUL_KEEP | 5, tex_b};
    code = translate(across, 4, ST_FS, &info, &words, 1);
    check(code && !info.unsupported, "a handle loaded before a forward branch reaches the branch target");
    free(code);
    /* IPA of a[0x7c] reads the interpolated 1/w as it is, FragCoord.w */
    {
        const u64 position_w[] = {0xe003ff87cff7ff1dull};
        code = translate(position_w, 1, ST_FS, &info, &words, 1);
        check(code && !info.unsupported && count_opcode(code, words, 136, 0xffffffffu) == 0,
              "IPA of a[0x7c] reads FragCoord.w without a division");
        free(code);
    }
    const u64 skipped_write[] = {0x4c98078800070006ull, branch, FMUL_KEEP | 6, tex_b};
    code = translate(skipped_write, 4, ST_FS, &info, &words, 1);
    check(!code && info.unsupported, "a handle written in code a branch skips doesn't reach the target");
    free(code);
}

/* a tessellation or geometry program: the instructions after an SPH with words 2 to 4 set */
static u32* translate_stage(const u64* instructions, int count, int stage, const u32* sph, u32 primitive,
                            ShaderInfo* info, size_t* words) {
    u64 code[128] = {0};
    int at = 10;
    memcpy((u32*)code + 2, sph, 3 * sizeof *sph); /* SPH words 2, 3 and 4 */
    for (int i = 0; i <= count; i++) {
        if ((at - 10) % 4 == 0) code[at++] = SCHED;
        code[at++] = i < count ? instructions[i] : EXIT;
    }
    memset(info, 0, sizeof *info);
    info->rt_int_mask = primitive;
    if (stage == ST_TCS) info->clip_enable = SNX_SH_NOT_LAST;
    return shader_to_spirv((const u8*)code, sizeof code, stage, info, words, 1, 1, 1);
}

/* edenprobe's tessellated patch: a control program passing its 4 vertices through and writing every level from
   vertex 0's attribute, and an evaluation program mixing the corners by the tessellation coordinate and reading
   the outer level back. Then a geometry program passing a triangle's first vertex on as a strip of one. */
static void between_stages(void) {
    const u64 control[] = {
        0xf0c8000001170004ull, 0xf0c8000001d70000ull, 0x384700000ff70001ull, 0x3800000081070000ull,
        0x5b00020000170002ull, 0x5b007fa800170001ull, 0x5b30011800170000ull, 0xefd0000000070005ull,
        0xefd982800707ff00ull, 0xeff1ff800707ff00ull, 0xefd982800907ff00ull, 0xeff1ff800907ff00ull,
        0xe29000000c800000ull, 0x5b6403800ff70407ull, 0xf0f800000008000full, 0xf0c8000001d70000ull,
        0x384700000ff70001ull, 0x3800000081070000ull, 0x5b007f8000170002ull, 0x5b007fa800170001ull,
        0x5b30011800170000ull, 0xefd0000000070000ull, 0xefd8000009c7ff00ull, 0x386810437f070000ull,
        0x5ca8140000070a00ull, 0xeff07f808007ff00ull, 0xeff07f808047ff00ull, 0xeff07f808087ff00ull,
        0xeff07f8080c7ff00ull, 0xeff07f808107ff00ull, 0xeff07f808147ff00ull, 0xf0f800000007000full,
    };
    const u64 evaluation[] = {
        0xf0c8000000070002ull, 0xefd881012f07ff00ull, 0xf0c8000001d70002ull, 0x384700000ff70203ull,
        0xeff0ff800807ff00ull, 0x3800000081070202ull, 0x010000000017f004ull, 0x5b00020000370204ull,
        0x5b007fa800370211ull, 0x5b30021801170204ull, 0xefd0000000070408ull, 0xefd984000707ff04ull,
        0x5b007f8000370208ull, 0x5b30041801170208ull, 0xefd000000007080cull, 0xefd986000707ff08ull,
        0x5c58300000870404ull, 0x59a004000047000dull, 0x5c58300000970504ull, 0x59a004800047000eull,
        0x5c58300000a70604ull, 0x59a005000047000full, 0x5c58300000b70704ull, 0x59a0058000470010ull,
        0x010000000027f004ull, 0x5b00020000370204ull, 0x5b30021801170204ull, 0xefd0000000070408ull,
        0xefd984000707ff04ull, 0x010000000037f008ull, 0x5b00040000370203ull, 0x5b30019801170202ull,
        0xefd0000000070202ull, 0xefd981000707ff08ull, 0x5c58300000870402ull, 0x59a0040000270002ull,
        0xefd87f808007ff08ull, 0x5c58300000970503ull, 0x59a0048000370003ull, 0x5c58300000a70604ull,
        0x59a0050000470006ull, 0xefd806000987ff0aull, 0x5c58300000b70704ull, 0x59a0058000470007ull,
        0x5c58300000d70202ull, 0x59a0068000270104ull, 0x5c58300000e70302ull, 0x59a0070000270105ull,
        0x5c58300000f70602ull, 0x59a0078000270106ull, 0x5c58300001070702ull, 0x59a0080000270107ull,
        0xefd886000907ff02ull, 0x1e23b8080817080bull, 0xeff1ff800707ff04ull, 0x5c98078000270008ull,
        0x5c98078000370009ull, 0xeff1ff800907ff08ull,
    };
    const u64 geometry[] = {
        0xefd9ff800707ff00ull, /* ALD R0, a[0x70] of vertex RZ, 4 words */
        0xeff1ff800707ff00ull, /* AST a[0x70], R0, 4 words */
        0xfbe000800ff70404ull, /* OUT.EMIT R4, R4, RZ */
        0xfbe001000ff70404ull, /* OUT.CUT R4, R4, RZ */
    };
    const u32 control_sph[3] = {4u << 24, 0, 0};         /* 4 threads, each an output vertex */
    const u32 geometry_sph[3] = {1u << 24, 7u << 24, 3}; /* 1 invocation, a triangle strip of up to 3 vertices */
    const u32 none[3] = {0, 0, 0};
    struct {
        const u64* instructions;
        int count, stage;
        const u32* sph;
        u32 primitive, mode;
    } cases[] = {{control, (int)(sizeof control / sizeof *control), ST_TCS, control_sph, 0, 26},
                 /* quads, equal spacing, connected counterclockwise triangles */
                 {evaluation, (int)(sizeof evaluation / sizeof *evaluation), ST_TES, none, 0x302, 24},
                 {geometry, 4, ST_GS, geometry_sph, 3, 29}};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate_stage(cases[i].instructions, cases[i].count, cases[i].stage, cases[i].sph,
                                    cases[i].primitive, &info, &words);
        check(code && !info.unsupported, "a tessellation or geometry program translates");
        if (code) {
            check(count_opcode(code, words, 16, cases[i].mode) == 1, "its execution mode says its primitive");
            if (i == 2) check(count_opcode(code, words, 218, 0xffffffffu) == 1, "OUT.EMIT emits a vertex");
            if (stage_paths[i]) save_fixture(stage_paths[i], code, words);
        }
        free(code);
    }
}

/* whether the program decorates a variable BuiltIn Layer */
static int has_layer(const u32* code, size_t words) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return -1;
        if ((code[i] & 0xffff) == 71 && width == 4 && code[i + 2] == 11 && code[i + 3] == 9) return 1;
        i += width;
    }
    return 0;
}

/* the layer takes ShaderLayer (the device's shaderOutputLayer) when a vertex program writes it, and a fragment
   program reads it with Geometry (geometryShader) or ShaderLayer. Without the capability the write is dropped and
   the read is 0, never a module the device can't take */
static void layer_capabilities(void) {
    const u64 write = 0xeff07f800647ff00ull; /* AST a[0x64], R0 */
    const u64 read = 0xe003ff864ff7ff00ull;  /* IPA R0, a[0x64] */
    for (int out = 0; out < 2; out++) {
        g_shader_output_layer = out;
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&write, 1, ST_VS, &info, &words, 1);
        check(code && !info.unsupported, "a vertex program writing the layer translates");
        if (code) {
            check((count_opcode(code, words, 17, 69) == 1) == out, "a layer write takes ShaderLayer when it can");
            check(has_layer(code, words) == out, "a layer write is dropped without ShaderLayer");
            check(count_opcode(code, words, 17, 2) == 0, "a layer write never takes Geometry");
        }
        free(code);
        for (int geometry = 0; geometry < 2; geometry++) {
            g_shader_geometry = geometry;
            code = translate(&read, 1, ST_FS, &info, &words, 1);
            check(code && !info.unsupported, "a fragment program reading the layer translates");
            if (code) {
                check(count_opcode(code, words, 17, 2) == geometry, "a layer read takes Geometry when it can");
                check(count_opcode(code, words, 17, 69) == (!geometry && out),
                      "a layer read takes ShaderLayer without Geometry");
                check(has_layer(code, words) == (geometry || out), "a layer read is 0 without either");
            }
            free(code);
        }
    }
    g_shader_output_layer = g_shader_geometry = 1;
}

/* Loads from a constant buffer (a Uniform variable bound below 18) by result type: u32 or anything else. */
static void cbuf_load_types(const u32* code, size_t words, int* integer, int* other) {
    u32 bound = words > 5 ? code[3] : 0, uint_type = 0;
    u32* binding = calloc(bound + 1, sizeof *binding); /* binding + 1, 0 for none */
    u8* cbuf = calloc(bound + 1, 1);                   /* a constant buffer variable or a pointer into one */
    *integer = *other = 0;
    for (size_t i = 5; binding && cbuf && i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) break;
        if (op == 21 && width == 4 && code[i + 2] == 32 && !code[i + 3]) uint_type = code[i + 1];
        if (op == 71 && width == 4 && code[i + 2] == 33 && code[i + 1] < bound) binding[code[i + 1]] = code[i + 3] + 1;
        if (op == 59 && width >= 4 && code[i + 3] == 2 && code[i + 2] < bound && binding[code[i + 2]] &&
            binding[code[i + 2]] <= 18)
            cbuf[code[i + 2]] = 1;
        if (op == 65 && width >= 4 && code[i + 3] < bound && cbuf[code[i + 3]] && code[i + 2] < bound)
            cbuf[code[i + 2]] = 1;
        if (op == 61 && width >= 4 && code[i + 3] < bound && cbuf[code[i + 3]])
            ++*(code[i + 1] == uint_type ? integer : other);
        i += width;
    }
    free(binding);
    free(cbuf);
}

/* A constant buffer word is loaded as an integer, whatever reads it: a float load may flush integers whose bits look
   like a denormal, such as the 0x140-0x280 entries of a BRX jump table, which then sends the program back to its
   start forever. */
static void cbuf_integer_loads(void) {
    static const u64 graphics[] = {
        0x4C98079008070005ull, /* MOV R5, c[4][0x200] */
        0x3848000000270000ull, /* SHL R0, R0, 2 */
        0xEF94001000070000ull, /* LDC R0, c[1][R0] */
        0x4C68101001470C00ull, /* FMUL R0, R12, c[4][0x50] */
    };
    static const int stages[] = {ST_VS, ST_FS, ST_CS};
    for (size_t i = 0; i < sizeof stages / sizeof *stages; i++) {
        ShaderInfo info;
        size_t words = 0;
        int integer, other;
        u32* code = translate(graphics, 4, stages[i], &info, &words, 1);
        check(code && !info.unsupported, "constant buffer reads translate");
        if (code) {
            cbuf_load_types(code, words, &integer, &other);
            check(integer >= 3 && !other, "constant buffer words are loaded as integers");
        }
        free(code);
    }
    /* the compute translator for straight-line programs */
    u64 compute[64] = {0};
    compute[10] = SCHED;
    compute[11] = 0x4C98078000470004ull; /* MOV R4, c[0][0x10] */
    compute[12] = EXIT;
    ShaderInfo info;
    memset(&info, 0, sizeof info);
    info.cs_local[0] = info.cs_local[1] = info.cs_local[2] = 1;
    size_t words = 0;
    int integer, other;
    u32* code = shader_compute_to_spirv((const u8*)compute, sizeof compute, &info, &words, 1, 1);
    check(code != NULL, "compute constant buffer read translates");
    if (code) {
        cbuf_load_types(code, words, &integer, &other);
        check(integer == 1 && !other, "compute constant buffer words are loaded as integers");
    }
    free(code);
}

/* Fire Emblem Three Houses fragment shaders sample fp16 surfaces with TLDS.
   The f32 forms translate; the fp16 form must translate the same way, with the
   components converted and packed into halves. */
static void tlds_fp16(void) {
    static const u64 words[] = {0xD244012030570A02ull, 0xD252010040570A18ull, 0xD252014000570A1Aull,
                                0xD25200E000B70A17ull, 0xD252014010B70A06ull};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
        ShaderInfo info;
        size_t size = 0;
        u32* code = translate(&words[i], 1, ST_FS, &info, &size, 1);
        if (!code || info.unsupported)
            fprintf(stderr, "tlds fp16 %zu: %s\n", i, info.first_unsupported);
        check(code && !info.unsupported, "TLDS fp16 translates");
        if (code) check(count_opcode(code, size, 115, 0xffffffffu) >= 1, "TLDS fp16 converts to halves");
        free(code);
    }
}

/* a rejected instruction is logged with its word */
static void unsupported_names_word(void) {
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate((const u64[]){0xFFFFFFFFFFFFFFFFull}, 1, ST_FS, &info, &words, 0);
    check(!code && !strcmp(info.first_unsupported, "unknown opcode ffffffffffffffff"), "an unknown opcode is named");
    free(code);
    /* SULD.D.64 on a buffer image: its 32-bit form is translated */
    code = translate((const u64[]){0xEB10030200580502ull}, 1, ST_CS, &info, &words, 0);
    check(!code && !strcmp(info.first_unsupported, "SULD eb10030200580502"), "a SULD form not done yet is named");
    free(code);
}

int main(int argc, char** argv) {
    if (argc != 1 && argc != 27) return 2;
    if (argc == 27) {
        storage_paths[0] = argv[1];
        storage_paths[1] = argv[2];
        for (int i = 0; i < 4; i++) nro_paths[i] = argv[i + 3];
        for (int i = 0; i < 17; i++) issue126_paths[i] = argv[i + 7];
        for (int i = 0; i < 3; i++) stage_paths[i] = argv[i + 24];
    }
    brx_length();
    cbuf_integer_loads();
    unsupported_names_word();
    f2i_modifiers();
    global_buffers();
    float_controls();
    precise_math();
    f2f_half_rounding();
    hfma2_immediate();
    tlds_fp16();
    fragment_storage_images();
    nro_shader_modes();
    issue126_shader_modes();
    between_stages();
    layer_capabilities();
    return failures ? 1 : 0;
}
