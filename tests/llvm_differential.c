/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend against the portable one, instruction by instruction. For words of every pattern and case of
   a64_templates.inc, the reference step function (snx_a64_static_step's C, compiled by Clang) and the LLVM
   backend's (snx_ir_static_step, compiled and linked by the backend itself) run from the same random CPU state and
   guest memory; registers, flags, FP state, the exclusive monitor, memory, the returned pc and the runtime callbacks
   must all match.
     llvm_differential CLANG REPO [--match NAME] [--per-case N] [--simd N] [--states N] [--seed N] [--quiet]
   --match keeps patterns whose name starts with NAME. */
#include "common.h"
#include "cpu.h"
#include "a64.h"
#include "ir.h"
#include "link.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef SNX_TEST_GCCJIT
#include "jit.h"
#endif
extern const unsigned char snx_rtlib_object[];
extern const unsigned long snx_rtlib_object_size;

typedef u64 (*StepFn)(CPU* c);
typedef StepFn* (*TableFn)(void);

#define MEMORY_SIZE 0x220000u
#define PC_BASE     0x100000u

static u64 seed = 0x9E3779B97F4A7C15ull;
static u64 next_random(void) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

typedef struct {
    SnxU32 word;
    const SnxA64Pattern* pattern;
    size_t index;
} Word;

/* ---- runtime callbacks, recorded ---- */
typedef struct {
    int kind;
    u64 a, b;
} Event;
typedef struct {
    Event events[16];
    int count;
} Log;
static Log* current;
static void record(int kind, u64 a, u64 b) {
    if (current->count < 16) {
        current->events[current->count].kind = kind;
        current->events[current->count].a = a;
        current->events[current->count++].b = b;
    }
}
static void cb_svc(CPU* c, u32 imm) {
    (void)c;
    record(1, imm, 0);
}
static void cb_undef(CPU* c, u64 pc, u32 w) {
    (void)c;
    record(2, pc, w);
}
static void cb_note(u64 pc) { record(3, pc, 0); }
static u64 cb_ticks(void) {
    record(4, 0, 0);
    return 123456789;
}
static void cb_brk(CPU* c, u64 pc, u32 imm) {
    (void)c;
    record(5, pc, imm);
}
static void cb_fpcr(CPU* c) { record(6, c->fpcr, 0); }
static void cb_sev(void) { record(7, 0, 0); }
static void cb_wfe(CPU* c) {
    (void)c;
    record(8, 0, 0);
}

static int memory_pattern(const char* name) {
    return !strncmp(name, "LDST", 4) || !strcmp(name, "LDR_LIT") || !strncmp(name, "SIMD_LDST", 9) ||
           !strcmp(name, "SYS");
}

static u64 special64(void) {
    static const u64 values[] = {0,
                                 1,
                                 ~0ull,
                                 0x7FFFFFFFFFFFFFFFull,
                                 0x8000000000000000ull,
                                 0xFFFFFFFFull,
                                 0x80000000ull,
                                 0x7FFFFFFFull,
                                 0x3FF0000000000000ull, /* 1.0 */
                                 0x7FF0000000000000ull, /* inf */
                                 0x7FF8000000000001ull, /* qNaN */
                                 0x7FF0000000000001ull, /* sNaN */
                                 0x000FFFFFFFFFFFFFull, /* denormal */
                                 0x3F8000003F800000ull, /* 1.0f, 1.0f */
                                 0x7F8000007FC00001ull, /* inf, qNaN */
                                 0x007FFFFF80000000ull, /* denormal, -0 */
                                 0x7F8000017F800000ull};
    u64 r = next_random();
    if (r % 3 == 0) return values[(r >> 8) % (sizeof(values) / sizeof(values[0]))];
    if (r % 3 == 1) return next_random() >> (r >> 58);
    return next_random();
}

/* floats at the edges of the LLVM backend's FP fast paths: exponents 0, 1, 2, precision+1..3, the top, and small
   integers and products that round or not */
static u32 edge32(void) {
    static const u32 exponents[] = {0, 1, 2, 3, 24, 25, 26, 27, 100, 127, 128, 150, 200, 252, 253, 254, 255};
    u64 r = next_random();
    u32 e = exponents[r % (sizeof(exponents) / sizeof(exponents[0]))];
    u32 m =
        (r >> 8) & 7 ? (u32)(r >> 20) & 0x7FFFFF : (u32)((r >> 20) & 7) << 20; /* sometimes few bits: exact results */
    return (u32)((r >> 40) & 1) << 31 | e << 23 | m;
}
static u64 edge64(void) {
    static const u64 exponents[] = {0, 1, 2, 3, 53, 54, 55, 56, 500, 1023, 1024, 1075, 1500, 2044, 2045, 2046, 2047};
    u64 r = next_random(), r2 = next_random();
    u64 e = exponents[r % (sizeof(exponents) / sizeof(exponents[0]))];
    u64 m = (r >> 8) & 7 ? r2 & 0xFFFFFFFFFFFFFull : (r2 & 7) << 49;
    return ((r >> 40) & 1) << 63 | e << 52 | m;
}
static u64 fp_value(void) {
    u64 r = next_random();
    if (r % 4 == 0) return (u64)edge32() << 32 | edge32();
    if (r % 4 == 1) return edge64();
    return special64();
}

static void random_state(CPU* c, unsigned char* memory, const Word* w) {
    unsigned i;
    int addresses = memory_pattern(w->pattern->name);
    static const u32 fpcr[] = {0, 1u << 24, 1u << 25, 3u << 22, 1u << 22, 2u << 22, 1u << 26, 0x07C00000u};
    for (i = 0; i < 32; ++i) {
        c->x[i] = addresses ? 0x1000 + (next_random() % 0x7000 & ~(u64)15) : special64();
        c->v[i].d[0] = fp_value();
        c->v[i].d[1] = fp_value();
    }
    c->n = next_random() & 1;
    c->z = next_random() & 1;
    c->cf = next_random() & 1;
    c->vf = next_random() & 1;
    c->fpcr = fpcr[next_random() % 8] | (next_random() % 2 ? (u32)(next_random() & 0x07C00000u) : 0);
    c->fpsr = next_random() % 4 ? (next_random() % 2 ? 0x10 : 0) : (u32)(next_random() & 0xF800009Fu);
    c->tpidr = next_random();
    c->tpidrro = next_random();
    c->excl_addr = next_random();
    c->excl_val = next_random();
    c->excl_val2 = next_random();
    c->excl_size = (u32)(next_random() % 4);
    c->excl_on = next_random() & 1;
    if (!strcmp(w->pattern->name, "LDSTX") && next_random() % 2) {
        /* a monitor that matches, holding memory's current value, so exclusive stores can succeed */
        unsigned rn = (w->word >> 5) & 31;
        u64 a = c->x[rn];
        c->excl_on = 1;
        c->excl_addr = a;
        c->excl_size = ((w->word >> 30) & 3) | ((w->word >> 21) & 1 ? 0x10 : 0);
        memcpy(&c->excl_val, memory + a, 8);
        if (!((w->word >> 21) & 1) && (w->word >> 30) < 3) c->excl_val &= (1ull << (8u << (w->word >> 30))) - 1;
        if ((w->word >> 21) & 1 && (w->word >> 30) == 2)
            c->excl_val2 = 0;
        else
            memcpy(&c->excl_val2, memory + a + 8, 8);
    }
}

static int same_events(const Log* a, const Log* b) {
    int i;
    if (a->count != b->count) return 0;
    for (i = 0; i < a->count; ++i)
        if (a->events[i].kind != b->events[i].kind || a->events[i].a != b->events[i].a ||
            a->events[i].b != b->events[i].b)
            return 0;
    return 1;
}

static void report(const Word* w, const CPU* start, const CPU* p, const CPU* l, u64 rp, u64 rl, const Log* lp,
                   const Log* ll, int memory_differs) {
    unsigned i;
    fprintf(stderr, "MISMATCH %s case %lu word %08lX\n", w->pattern->name, (unsigned long)w->index,
            (unsigned long)w->word);
    if (rp != rl)
        fprintf(stderr, "  returned pc %llx (portable) %llx (llvm)\n", (unsigned long long)rp, (unsigned long long)rl);
    for (i = 0; i < 32; ++i)
        if (p->x[i] != l->x[i])
            fprintf(stderr, "  x%u: start %016llx portable %016llx llvm %016llx\n", i, (unsigned long long)start->x[i],
                    (unsigned long long)p->x[i], (unsigned long long)l->x[i]);
    for (i = 0; i < 32; ++i)
        if (memcmp(&p->v[i], &l->v[i], 16))
            fprintf(stderr, "  v%u: start %016llx:%016llx portable %016llx:%016llx llvm %016llx:%016llx\n", i,
                    (unsigned long long)start->v[i].d[1], (unsigned long long)start->v[i].d[0],
                    (unsigned long long)p->v[i].d[1], (unsigned long long)p->v[i].d[0],
                    (unsigned long long)l->v[i].d[1], (unsigned long long)l->v[i].d[0]);
    if (p->n != l->n || p->z != l->z || p->cf != l->cf || p->vf != l->vf)
        fprintf(stderr, "  nzcv portable %u%u%u%u llvm %u%u%u%u\n", p->n, p->z, p->cf, p->vf, l->n, l->z, l->cf, l->vf);
    if (p->fpcr != l->fpcr || p->fpsr != l->fpsr)
        fprintf(stderr, "  fpcr/fpsr portable %08x/%08x llvm %08x/%08x (start %08x/%08x)\n", p->fpcr, p->fpsr, l->fpcr,
                l->fpsr, start->fpcr, start->fpsr);
    if (p->pc != l->pc)
        fprintf(stderr, "  c->pc portable %llx llvm %llx\n", (unsigned long long)p->pc, (unsigned long long)l->pc);
    if (p->excl_on != l->excl_on || p->excl_addr != l->excl_addr || p->excl_val != l->excl_val ||
        p->excl_val2 != l->excl_val2 || p->excl_size != l->excl_size)
        fprintf(stderr, "  exclusive monitor differs\n");
    if (p->tpidr != l->tpidr) fprintf(stderr, "  tpidr differs\n");
    if (memory_differs) fprintf(stderr, "  guest memory differs\n");
    if (!same_events(lp, ll)) fprintf(stderr, "  runtime callbacks differ (%d vs %d)\n", lp->count, ll->count);
}

static int same_cpu(const CPU* a, const CPU* b) {
    return !memcmp(a->x, b->x, sizeof(a->x)) && a->pc == b->pc && a->n == b->n && a->z == b->z && a->cf == b->cf &&
           a->vf == b->vf && !memcmp(a->v, b->v, sizeof(a->v)) && a->fpcr == b->fpcr && a->fpsr == b->fpsr &&
           a->tpidr == b->tpidr && a->tpidrro == b->tpidrro && a->excl_addr == b->excl_addr &&
           a->excl_val == b->excl_val && a->excl_val2 == b->excl_val2 && a->excl_size == b->excl_size &&
           a->excl_on == b->excl_on;
}

static void* load(const char* path, TableFn* table) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    *table = h ? (TableFn)(void*)GetProcAddress(h, "snx_test_steps") : NULL;
    return h;
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) fprintf(stderr, "%s\n", dlerror());
    *table = h ? (TableFn)dlsym(h, "snx_test_steps") : NULL;
    return h;
#endif
}

/* words of one case: random operand fields, the case's specialization fields set, valid encodings only */
static size_t generate(const SnxA64Pattern* p, size_t index, size_t want, Word* out) {
    size_t got = 0, attempts;
    for (attempts = 0; attempts < want * 400 && got < want; ++attempts) {
        SnxU32 w = ((SnxU32)next_random() & ~p->mask) | p->value;
        size_t k = index;
        int n;
        for (n = (int)p->field_count - 1; n >= 0; --n) {
            SnxU32 mask = (SnxU32)((1UL << p->fields[n].width) - 1);
            w = (w & ~(mask << p->fields[n].lo)) | ((SnxU32)(k & mask) << p->fields[n].lo);
            k >>= p->fields[n].width;
        }
        if (snx_a64_decode(w) != &p->cases[index]) continue;
        out[got].word = w;
        out[got].pattern = p;
        out[got++].index = index;
    }
    return got;
}

/* snx_a64_fast_simd's encodings, which random fields rarely produce: 3-same logical/FP/ADD, FP by element, DUP/INS/
   UMOV, MOVI #0 and MOV (ORR Vd,Vn,Vn), with random operands */
static size_t directed(const char* match, size_t want, Word* out) {
    static const SnxU32 same_ops[] = {3, 0x10, 0x18, 0x19, 0x1A, 0x1B, 0x1F}, element_ops[] = {1, 5, 9},
                        copy_ops[] = {0, 1, 3, 7};
    size_t got = 0, i;
    if (match && strncmp("SIMD", match, strlen(match) < 4 ? strlen(match) : 4)) return 0;
    for (i = 0; i < want; ++i) {
        SnxU32 r = (SnxU32)next_random(), n = (r >> 5) & 31, words[20];
        unsigned k;
        words[0] = 0x2F00E400UL | (r & 31) | ((r >> 9) & 1) << 30;
        words[1] = 0x0EA01C00UL | ((r >> 10) & 1) << 30 | n << 16 | n << 5 | (r & 31);
        words[2] = ((r & ~0x9F20FC00UL) | 0x0E200400UL) | same_ops[next_random() % 7] << 11;
        words[3] = ((r & ~0x9F00F400UL) | (next_random() % 2 ? 0x5F000000UL : 0x0F000000UL) | 0x00800000UL) |
                   element_ops[next_random() % 3] << 12;
        words[4] = ((r & ~0x9FE0FC00UL) | 0x0E000400UL) | copy_ops[next_random() % 4] << 11;
        words[5] = ((r & ~0x9F20FC00UL) | 0x0E200400UL) | same_ops[next_random() % 7] << 11 | 0x40000000UL;
        /* snx_ir_more_simd: LD1/ST1 (1-4 registers), LD1R and lanes, CNT/NOT/RBIT, PMULL 1Q, MOVI family, UZP/TRN/ZIP,
           EXT, AES */
        words[6] = (r & 0x40C0001FUL) | 0x0C000000UL | (SnxU32)(next_random() % 2) << 23 |
                   (SnxU32)(next_random() % 2 ? 31 : (r >> 16) & 31) << 16 |
                   (SnxU32) "\x07\x0A\x06\x02"[next_random() % 4] << 12 | ((r >> 10) & 3) << 10 | ((r >> 5) & 31) << 5;
        words[7] = (r & 0x40E0FFFFUL & ~0x00202000UL) | 0x0D000000UL;
        words[8] = (r & 0x60C003FFUL) | 0x0E205800UL;
        words[9] = (r & 0x401F03FFUL) | 0x0EE0E000UL;
        words[10] = (r & 0x6007FBFFUL) | 0x0F000400UL;
        words[11] = (r & 0x40DF73FFUL) | 0x0E000800UL;
        words[12] = (r & 0x401F7BFFUL) | 0x2E000000UL;
        words[13] = (r & 0x000033FFUL) | 0x4E284800UL;
        /* three-same integer, shifts by immediate, FABS/FNEG */
        words[14] = (r & 0x60DFFBFFUL) | 0x0E200400UL;
        words[15] = (r & 0x607FFBFFUL & ~0x0000F800UL) | 0x0F000400UL |
                    (SnxU32) "\x00\x02\x08\x0A\x14\x10"[next_random() % 6] << 11 | (1u << (19 + (r >> 30) % 4));
        words[16] = (r & 0x60C003FFUL) | 0x0EA0F800UL;
        /* saturating and halving adds, BSL/BIT/BIF: three-same ops 0x00-0x05 and 3 with U */
        words[17] =
            (r & 0x40DF07FFUL) | 0x0E200400UL | (SnxU32)(next_random() % 6) << 11 | (SnxU32)(next_random() % 2) << 29;
        words[18] = (r & 0x40DF03FFUL) | 0x2E201C00UL | (1u + (SnxU32)(next_random() % 3)) << 22;
        words[19] = (r & 0x40DF03FFUL) | 0x2E605800UL; /* RBIT */
        for (k = 0; k < 20; ++k) {
            const SnxA64Case* c = snx_a64_decode(words[k]);
            if (c->kind != SNX_FLOW_PLAIN) continue;
            out[got].word = words[k];
            out[got].pattern = snx_a64_case_pattern(c, &out[got].index);
            if (out[got].pattern) ++got;
        }
    }
    return got;
}

#ifdef SNX_TEST_GCCJIT
/* the libgccjit backend's step functions (snx_jit_static_step), linked by GCC's driver with the runtime library */
static int build_gccjit(const Word* words, size_t count, const char* dir, const char* path, unsigned long* fallbacks) {
    SnxJit* jit = snx_jit_new('2');
    gcc_jit_rvalue** fns = (gcc_jit_rvalue**)snx_alloc(count * sizeof(*fns));
    gcc_jit_type *param = jit->t_ptr, *step_ptr, *table_type;
    gcc_jit_lvalue* table;
    gcc_jit_function* getter;
    char* rtlib = snx_path_join(dir, "rtlib.o");
    size_t i;
    int rc = 0;
    step_ptr = gcc_jit_context_new_function_ptr_type(jit->ctx, NULL, jit->t_u64, 1, &param, 0);
    for (i = 0; i < count; ++i) {
        char name[32];
        sprintf(name, "S_%lu", (unsigned long)i);
        fns[i] = gcc_jit_context_new_cast(
            jit->ctx, NULL, gcc_jit_function_get_address(snx_jit_static_step(jit, words[i].word, PC_BASE, name), NULL),
            step_ptr);
    }
    table_type = gcc_jit_context_new_array_type(jit->ctx, NULL, step_ptr, (int)count);
    table = gcc_jit_context_new_global(jit->ctx, NULL, GCC_JIT_GLOBAL_INTERNAL, table_type, "TABLE");
    gcc_jit_global_set_initializer_rvalue(
        table, gcc_jit_context_new_array_constructor(jit->ctx, NULL, table_type, count, fns));
    getter = gcc_jit_context_new_function(jit->ctx, NULL, GCC_JIT_FUNCTION_EXPORTED, jit->t_ptr, "snx_test_steps", 0,
                                          NULL, 0);
    gcc_jit_block_end_with_return(
        gcc_jit_function_new_block(getter, "entry"), NULL,
        gcc_jit_context_new_cast(jit->ctx, NULL, gcc_jit_lvalue_get_address(table, NULL), jit->t_ptr));
    snx_write_file(rtlib, snx_rtlib_object, snx_rtlib_object_size);
    gcc_jit_context_add_driver_option(jit->ctx, rtlib);
    gcc_jit_context_compile_to_file(jit->ctx, GCC_JIT_OUTPUT_KIND_DYNAMIC_LIBRARY, path);
    if (snx_jit_error(jit)) rc = snx_error("libgccjit: %s", snx_jit_error(jit));
    *fallbacks = jit->fallbacks;
    snx_jit_free(jit);
    free(fns);
    free(rtlib);
    return rc;
}
#endif

int main(int argc, char** argv) {
    const char *clang, *repo, *match = NULL;
    size_t per_case = 4, per_simd = 300, states = 6, count = 0, capacity = 0, npatterns, i, k, failures = 0,
           compared = 0;
    size_t pattern_failures[64] = {0};
    Word* words = NULL;
    const SnxA64Pattern* patterns = snx_a64_patterns(&npatterns);
    char error[1024], *dir, *c_path, *c_lib, *l_lib, *text = NULL, *abi, *common;
    const char* args[24];
    FILE* f;
    SnxIr* ir;
    unsigned char *object = NULL, *memory_start, *memory_p, *memory_l;
    size_t object_size;
    SnxLinkInput inputs[2];
    SnxLinkOutput linked;
    const char* exports[1] = {"snx_test_steps"};
    TableFn table_p, table_l;
    StepFn *steps_p, *steps_l;
    RtAPI rt;
    int quiet = 0, a, gccjit = 0, march;
#ifdef SNX_TEST_GCCJIT
    TableFn table_g = NULL;
#endif
    StepFn* steps_g = NULL;
    TableFn table_h;
    StepFn* steps_h;
    char* c_native;
    unsigned long fallbacks, gccjit_fallbacks = 0;
    if (argc < 3) {
        fputs("usage: llvm_differential CLANG REPO [--match NAME] [--per-case N] [--simd N] [--states N] [--seed N]\n",
              stderr);
        return 2;
    }
    clang = argv[1];
    repo = argv[2];
    for (a = 3; a < argc; ++a) {
        if (!strcmp(argv[a], "--quiet"))
            quiet = 1;
        else if (!strcmp(argv[a], "--gccjit"))
            gccjit = 1;
        else if (a + 1 < argc && !strcmp(argv[a], "--match"))
            match = argv[++a];
        else if (a + 1 < argc && !strcmp(argv[a], "--per-case"))
            per_case = strtoul(argv[++a], NULL, 10);
        else if (a + 1 < argc && !strcmp(argv[a], "--simd"))
            per_simd = strtoul(argv[++a], NULL, 10);
        else if (a + 1 < argc && !strcmp(argv[a], "--states"))
            states = strtoul(argv[++a], NULL, 10);
        else if (a + 1 < argc && !strcmp(argv[a], "--seed"))
            seed = strtoull(argv[++a], NULL, 0) | 1;
        else
            return snx_error("unknown argument %s", argv[a]);
    }
    /* words */
    for (i = 0; i < npatterns; ++i) {
        const SnxA64Pattern* p = &patterns[i];
        if (match && strncmp(p->name, match, strlen(match))) continue;
        for (k = 0; k < p->case_count; ++k) {
            size_t want = !strncmp(p->name, "SIMD", 4) ? per_simd : per_case;
            if (p->cases[k].kind == SNX_FLOW_UNDEF) continue;
            if (count + want > capacity) {
                capacity = (count + want) * 2;
                words = (Word*)snx_realloc(words, capacity * sizeof(*words));
            }
            count += generate(p, k, want, words + count);
        }
    }
    words = (Word*)snx_realloc(words, (count + 20 * per_simd) * sizeof(*words));
    count += directed(match, per_simd, words + count);
    if (!count) return snx_error("no words to test");
    dir = snx_temp_dir(NULL);
    c_path = snx_path_join(dir, "portable.c");
    c_lib = snx_path_join(dir, "portable.so");
    l_lib = snx_path_join(dir, "llvm.so");
    /* the reference: snx_a64_static_step's C for each word, compiled by Clang with the flags generated C had */
    f = snx_fopen(c_path, "wb");
    fputs(
        "#include \"cpu.h\"\n#include \"cpu_helpers.c\"\nGuestFn snx_interp_entry;\ntypedef u64 (*StepFnPtr)(CPU*);\n",
        f);
    for (i = 0; i < count; ++i) {
        char name[32];
        sprintf(name, "S_%lu", (unsigned long)i);
        snx_a64_static_step(f, words[i].word, PC_BASE, name);
    }
    fputs("static StepFnPtr TABLE[]={", f);
    for (i = 0; i < count; ++i) fprintf(f, "%sS_%lu", i ? "," : "", (unsigned long)i);
    fputs("};\nSNX_EXPORT StepFnPtr* snx_test_steps(void){ return TABLE; }\n", f);
    fclose(f);
    abi = snx_path_join(repo, "src/common/cpu/aarch64");
    common = snx_path_join(repo, "src/common");
    c_native = snx_path_join(dir, "native.so");
    k = 0;
    args[k++] = clang;
    args[k++] = "-O2";
    args[k++] = "-shared";
    args[k++] = "-w";
    march = k;
#if defined(__aarch64__)
    args[k++] = "-march=armv8-a"; /* the reference: the helpers' plain C (no feature macros) */
#else
    args[k++] = "-march=x86-64";
    args[k++] = "-mcx16";
#endif
    args[k++] = "-fno-strict-aliasing";
    args[k++] = "-fwrapv";
#ifndef _WIN32
    args[k++] = "-fPIC";
#endif
    args[k++] = "-I";
    args[k++] = abi;
    args[k++] = "-I";
    args[k++] = common;
    args[k++] = c_path;
    args[k++] = "-o";
    args[k++] = c_lib;
    args[k] = NULL;
    if (!quiet) printf("%lu words; compiling the reference steps\n", (unsigned long)count);
    if (snx_run(args, NULL, NULL, &text)) return snx_error("portable compile failed:\n%s", text ? text : "");
    free(text);
    /* again with -march=native: the helpers' paths for the host's extensions (AES-NI, PCLMULQDQ, CRC32, GFNI) */
    args[march] = "-march=native";
    args[k - 1] = c_native;
    text = NULL;
    if (snx_run(args, NULL, NULL, &text)) return snx_error("native portable compile failed:\n%s", text ? text : "");
    free(text);
    /* the LLVM backend: snx_ir_static_step for each word, linked with the runtime library */
    if (!quiet) printf("compiling the LLVM steps\n");
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    ir = snx_ir_new("steps", '2', error, sizeof(error));
    if (!ir) return snx_error("%s", error);
    {
        LLVMValueRef *fns = (LLVMValueRef*)snx_alloc(count * sizeof(*fns)), table, getter;
        LLVMTypeRef type = LLVMArrayType(ir->t_ptr, (unsigned)count);
        for (i = 0; i < count; ++i) {
            char name[32];
            sprintf(name, "S_%lu", (unsigned long)i);
            fns[i] = snx_ir_static_step(ir, words[i].word, PC_BASE, name);
            LLVMSetVisibility(fns[i], LLVMHiddenVisibility);
        }
        table = LLVMAddGlobal(ir->mod, type, "TABLE");
        LLVMSetLinkage(table, LLVMInternalLinkage);
        LLVMSetInitializer(table, LLVMConstArray(ir->t_ptr, fns, (unsigned)count));
        getter = LLVMAddFunction(ir->mod, "snx_test_steps", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
        LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, getter, "entry"));
        LLVMBuildRet(ir->b, table);
        free(fns);
    }
    fallbacks = ir->fallbacks;
    if (snx_ir_object(ir, '2', &object, &object_size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    inputs[0].data = snx_rtlib_object;
    inputs[0].size = snx_rtlib_object_size;
    inputs[0].name = "rtlib";
    inputs[1].data = object;
    inputs[1].size = object_size;
    inputs[1].name = "steps";
    if (snx_link(inputs, 2, exports, 1, "llvm.so", &linked, error, sizeof(error))) return snx_error("link: %s", error);
    if (snx_write_file(l_lib, linked.data, linked.size)) return snx_error("cannot write %s", l_lib);
    free(linked.data);
    free(object);
    if (!load(c_lib, &table_p) || !table_p) return snx_error("cannot load the reference steps");
    if (!load(c_native, &table_h) || !table_h) return snx_error("cannot load the native reference steps");
    steps_h = table_h();
    if (!load(l_lib, &table_l) || !table_l) return snx_error("cannot load the LLVM steps");
    steps_p = table_p();
    steps_l = table_l();
    if (gccjit) {
#ifdef SNX_TEST_GCCJIT
        char* g_lib = snx_path_join(dir, "gccjit.so");
        if (!quiet) printf("compiling the libgccjit steps\n");
        if (build_gccjit(words, count, dir, g_lib, &gccjit_fallbacks)) return 1;
        if (!load(g_lib, &table_g) || !table_g) return snx_error("cannot load the libgccjit steps");
        steps_g = table_g();
        free(g_lib);
#else
        return snx_error("built without libgccjit");
#endif
    }
    /* run */
    memset(&rt, 0, sizeof(rt));
    rt.svc = cb_svc;
    rt.undef = cb_undef;
    rt.note_hint = cb_note;
    rt.ticks = cb_ticks;
    rt.brk = cb_brk;
    rt.fpcr_changed = cb_fpcr;
    rt.sev = cb_sev;
    rt.wfe = cb_wfe;
    memory_start = (unsigned char*)malloc(MEMORY_SIZE);
    memory_p = (unsigned char*)malloc(MEMORY_SIZE);
    memory_l = (unsigned char*)malloc(MEMORY_SIZE);
    for (i = 0; i < MEMORY_SIZE; i += 8) {
        u64 r = special64();
        memcpy(memory_start + i, &r, 8);
    }
    for (i = 0; i < count; ++i) {
        size_t s;
        for (s = 0; s < states; ++s) {
            CPU start, p, l, h;
            Log log_p, log_l, log_h;
            u64 rp, rl, rh;
            int memory_differs, helper_memory;
            memset(&start, 0, sizeof(start));
            random_state(&start, memory_start, &words[i]);
            start.rt = &rt;
            p = l = start;
            memcpy(memory_p, memory_start, MEMORY_SIZE);
            memcpy(memory_l, memory_start, MEMORY_SIZE);
            p.mem = memory_p;
            l.mem = memory_l;
            memset(&log_p, 0, sizeof(log_p));
            memset(&log_l, 0, sizeof(log_l));
            current = &log_p;
            rp = steps_p[i](&p);
            current = &log_l;
            rl = steps_l[i](&l);
            memory_differs = memcmp(memory_p, memory_l, MEMORY_SIZE) != 0;
            {
                h = start;
                memcpy(memory_l, memory_start, MEMORY_SIZE);
                h.mem = memory_l;
                memset(&log_h, 0, sizeof(log_h));
                current = &log_h;
                rh = steps_h[i](&h);
                helper_memory = memcmp(memory_p, memory_l, MEMORY_SIZE) != 0; /* memory_l is reused: compared above */
                if (rp != rh || !same_cpu(&p, &h) || helper_memory || !same_events(&log_p, &log_h)) {
                    size_t index = (size_t)(words[i].pattern - patterns);
                    fprintf(stderr, "the helpers built with -march=native differ from the plain C ones:\n");
                    if (pattern_failures[index]++ < 3)
                        report(&words[i], &start, &p, &h, rp, rh, &log_p, &log_h, helper_memory);
                    ++failures;
                    break;
                }
            }
            ++compared;
            if (steps_g) { /* the libgccjit steps against the reference too */
                CPU g = start;
                Log log_g;
                u64 rg;
                memcpy(memory_l, memory_start, MEMORY_SIZE);
                g.mem = memory_l;
                memset(&log_g, 0, sizeof(log_g));
                current = &log_g;
                rg = steps_g[i](&g);
                helper_memory = memcmp(memory_p, memory_l, MEMORY_SIZE) != 0;
                if (rp != rg || !same_cpu(&p, &g) || helper_memory || !same_events(&log_p, &log_g)) {
                    size_t index = (size_t)(words[i].pattern - patterns);
                    fprintf(stderr, "the libgccjit steps differ from the reference ones:\n");
                    if (pattern_failures[index]++ < 3)
                        report(&words[i], &start, &p, &g, rp, rg, &log_p, &log_g, helper_memory);
                    ++failures;
                    break;
                }
            }
            if (rp != rl || !same_cpu(&p, &l) || memory_differs || !same_events(&log_p, &log_l)) {
                size_t index = (size_t)(words[i].pattern - patterns);
                if (pattern_failures[index]++ < 3)
                    report(&words[i], &start, &p, &l, rp, rl, &log_p, &log_l, memory_differs);
                ++failures;
                break;
            }
        }
    }
    for (i = 0; i < npatterns; ++i)
        if (pattern_failures[i])
            printf("  %s: %lu words differ\n", patterns[i].name, (unsigned long)pattern_failures[i]);
    /* every pattern has an emitter, in both backends */
    fallbacks += gccjit_fallbacks;
    if (gccjit) printf("libgccjit: %lu through snx_step\n", gccjit_fallbacks);
    printf("LLVM differential: %lu words, %lu runs, %lu differing words, %lu through snx_step: %s\n",
           (unsigned long)count, (unsigned long)compared, (unsigned long)failures, fallbacks,
           failures || fallbacks ? "FAILED" : "passed");
    snx_remove_tree(dir);
    return failures || fallbacks;
}
