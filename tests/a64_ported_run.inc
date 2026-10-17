/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runner of the ported A64 cases (a64_ported_cases.h). a64_ported.c emits each case's instructions as AOT static
   steps (snx_a64p_aot, snx_a64p_minmax_aot) and the interpreter's snx_step, then includes this; every case runs on
   both paths. Guest FPCR reaches the host FP state as the runtime's does (a64_audit_state.h). */
#include "a64_ported_cases.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* guest memory: one window, enough for every case's accesses */
#define SNX_A64P_WINDOW 0x4000u
#define SNX_A64P_FZ     0x01000000u
#define SNX_A64P_DN     0x02000000u

static u8 snx_a64p_window[SNX_A64P_WINDOW], snx_a64p_want_window[SNX_A64P_WINDOW];
static u64 snx_a64p_base;
static CPU snx_a64p_want;
static int snx_a64p_traps, snx_a64p_failures, snx_a64p_checks;
static const char *snx_a64p_path, *snx_a64p_name;

static void snx_a64p_undef(CPU* c, u64 pc, u32 w) {
    (void)c;
    (void)pc;
    (void)w;
    ++snx_a64p_traps;
}
static void snx_a64p_svc(CPU* c, u32 imm) {
    (void)c;
    (void)imm;
    ++snx_a64p_traps;
}
static void snx_a64p_brk(CPU* c, u64 pc, u32 imm) {
    (void)c;
    (void)pc;
    (void)imm;
    ++snx_a64p_traps;
}
static u64 snx_a64p_ticks(void) { return 0; }

static void snx_a64p_reset(CPU* c, RtAPI* rt) {
    memset(rt, 0, sizeof(*rt));
    rt->undef = snx_a64p_undef;
    rt->svc = snx_a64p_svc;
    rt->brk = snx_a64p_brk;
    rt->ticks = snx_a64p_ticks;
    rt->fpcr_changed = audit_fpcr;
    memset(c, 0, sizeof(*c));
    c->rt = rt;
    snx_a64p_traps = 0;
}

/* the window holds guest addresses base.. ; the guest's own base + address arithmetic lands in it */
static void snx_a64p_memory(CPU* c, u64 base, int zeros) {
    u32 i;
    snx_a64p_base = base;
    for (i = 0; i < SNX_A64P_WINDOW; ++i) snx_a64p_window[i] = zeros ? 0 : (u8)(base + i);
    c->mem = (u8*)((uintptr_t)snx_a64p_window - (uintptr_t)base);
}
static u8* snx_a64p_at(u64 address, u64 size) {
    if (address - snx_a64p_base >= SNX_A64P_WINDOW || address - snx_a64p_base + size > SNX_A64P_WINDOW) return NULL;
    return snx_a64p_window + (address - snx_a64p_base);
}

/* interpreter_visitor.cpp's Initial(), its region at address 0: no zero byte in memory, so a stored zero shows; SP
   ends in 0xee0, so SP stored in place of zero shows at every size */
static void snx_a64p_preset_nce(CPU* c) {
    u32 i;
    snx_a64p_memory(c, 0, 1);
    for (i = 0; i < SNX_A64P_WINDOW; ++i) snx_a64p_window[i] = (u8)(0x80 | (u32)(i * 0x9E3779B1u) >> 25);
    for (i = 0; i < 31; ++i) c->x[i] = 0xDEAD000000000000ull | i;
    c->x[0] = 0x1000;
    c->x[1] = 0x1800;
    c->x[2] = 0x30;
    c->x[31] = 0x2EE0;
    for (i = 0; i < 32; ++i) {
        c->v[i].d[0] = 0x5555000000000000ull | i;
        c->v[i].d[1] = 0xAAAA000000000000ull | i;
    }
}

/* a u64 in hex as %0<digits>llx prints it, through two unsigned longs: C90 has no long long */
static const char* snx_a64p_hex(u64 v, int digits) {
    static char ring[4][24];
    static unsigned at;
    char* out = ring[at++ % 4];
    unsigned long hi = (unsigned long)(v >> 32), lo = (unsigned long)(v & 0xFFFFFFFFu);
    if (hi || digits > 8)
        sprintf(out, "%0*lx%08lx", digits > 8 ? digits - 8 : 1, hi, lo);
    else
        sprintf(out, "%0*lx", digits, lo);
    return out;
}

static void snx_a64p_expect(int ok, const char* what, u64 got_hi, u64 got, u64 want_hi, u64 want) {
    ++snx_a64p_checks;
    if (ok) return;
    ++snx_a64p_failures;
    fprintf(stderr, "FAIL %s [%s]: %s is %s:%s, expected %s:%s\n", snx_a64p_name, snx_a64p_path, what,
            snx_a64p_hex(got_hi, 16), snx_a64p_hex(got, 16), snx_a64p_hex(want_hi, 16), snx_a64p_hex(want, 16));
}

/* one pass of the whole script on one path; returns the number of cases */
static int snx_a64p_script_run(int interp) {
    size_t i;
    CPU c;
    RtAPI rt;
    size_t first = 0, count = 0;
    u64 pc = 0, host;
    int cases = 0;
    char what[64];
    snx_a64p_path = interp ? "interpreter" : "AOT";
    snx_a64p_reset(&c, &rt);
    for (i = 0; snx_a64p_script[i].op != SNX_A64P_OP_END; ++i) {
        const SnxA64pOp* op = &snx_a64p_script[i];
        u8* p;
        switch (op->op) {
            u64 t;
            case SNX_A64P_OP_CASE:
                snx_a64p_name = op->text;
                snx_a64p_reset(&c, &rt);
                snx_a64p_memory(&c, 0, 0);
                first = i + 1;
                count = 0;
                pc = 0;
                ++cases;
                break;
            case SNX_A64P_OP_CODE:
                /* a case's code comes first, so address / 4 indexes it */
                snx_a64p_expect(i == first + count, "the script: code after the state", 0, 0, 0, 0);
                ++count;
                break;
            case SNX_A64P_OP_SET_X: c.x[op->reg] = op->a; break;
            case SNX_A64P_OP_SET_V:
                c.v[op->reg].d[0] = op->a;
                c.v[op->reg].d[1] = op->b;
                break;
            case SNX_A64P_OP_SET_FPCR: c.fpcr = (u32)op->a; break;
            case SNX_A64P_OP_SET_FPSR: c.fpsr = (u32)op->a; break;
            case SNX_A64P_OP_SET_NZCV: set_nzcv(&c, (u32)(op->a >> 28)); break;
            case SNX_A64P_OP_CLEAR_V: memset(c.v, 0, sizeof(c.v)); break;
            case SNX_A64P_OP_MEMORY: snx_a64p_memory(&c, op->a, (int)op->reg); break;
            case SNX_A64P_OP_SET_MEM:
                p = snx_a64p_at(op->a, strlen(op->text) + 1);
                snx_a64p_expect(p != NULL, "the script: text outside the memory window", 0, 0, 0, 0);
                if (p) memcpy(p, op->text, strlen(op->text) + 1);
                break;
            case SNX_A64P_OP_RUN:
                host = audit_host_save();
                audit_fpcr(&c);
                pc = op->a;
                for (t = 0; t < op->b; ++t) {
                    size_t k;
                    /* past the code, dynarmic's environment reads B . */
                    if ((pc & 3) || pc / 4 >= count) break;
                    k = first + (size_t)(pc / 4);
                    pc = interp ? snx_step(&c, (u32)snx_a64p_script[k].a, pc) : snx_a64p_aot[k](&c);
                }
                audit_host_restore(host);
                snx_a64p_expect(!snx_a64p_traps, "undefined-instruction, SVC or BRK traps", 0, (u64)snx_a64p_traps, 0,
                                0);
                snx_a64p_traps = 0;
                break;
            case SNX_A64P_OP_X:
                snprintf(what, sizeof(what), "X%u & %s", op->reg, snx_a64p_hex(op->b, 16));
                snx_a64p_expect((c.x[op->reg] & op->b) == op->a, what, 0, c.x[op->reg] & op->b, 0, op->a);
                break;
            case SNX_A64P_OP_V:
                snprintf(what, sizeof(what), "V%u", op->reg);
                snx_a64p_expect(c.v[op->reg].d[0] == op->a && c.v[op->reg].d[1] == op->b, what, c.v[op->reg].d[1],
                                c.v[op->reg].d[0], op->b, op->a);
                break;
            case SNX_A64P_OP_PC: snx_a64p_expect(pc == op->a, "PC", 0, pc, 0, op->a); break;
            case SNX_A64P_OP_FPSR:
                snprintf(what, sizeof(what), "FPSR & %s", snx_a64p_hex(op->b, 8));
                snx_a64p_expect((c.fpsr & op->b) == op->a, what, 0, c.fpsr & op->b, 0, op->a);
                break;
            case SNX_A64P_OP_NZCV: snx_a64p_expect(get_nzcv(&c) == op->a, "NZCV", 0, get_nzcv(&c), 0, op->a); break;
            case SNX_A64P_OP_MEM64: {
                u64 v = 0;
                p = snx_a64p_at(op->a, 8);
                if (p) memcpy(&v, p, 8);
                snprintf(what, sizeof(what), "the 64 bits at %s", snx_a64p_hex(op->a, 16));
                snx_a64p_expect(p && v == op->b, what, 0, v, 0, op->b);
                break;
            }
            case SNX_A64P_OP_MEM_SAME: {
                u8 *x = snx_a64p_at(op->a, op->reg), *y = snx_a64p_at(op->b, op->reg);
                snprintf(what, sizeof(what), "%u bytes at %s against %s", op->reg, snx_a64p_hex(op->a, 0),
                         snx_a64p_hex(op->b, 0));
                snx_a64p_expect(x && y && !memcmp(x, y, op->reg), what, 0, 0, 0, 0);
                break;
            }
            case SNX_A64P_OP_PRESET:
                snx_a64p_expect(op->reg == 1, "the script: an unknown preset", 0, op->reg, 0, 1);
                snx_a64p_preset_nce(&c);
                break;
            case SNX_A64P_OP_MARK:
                snx_a64p_want = c;
                memcpy(snx_a64p_want_window, snx_a64p_window, SNX_A64P_WINDOW);
                break;
            case SNX_A64P_OP_WANT_X: snx_a64p_want.x[op->reg] = op->a; break;
            case SNX_A64P_OP_WANT_V:
                snx_a64p_want.v[op->reg].d[0] = op->a;
                snx_a64p_want.v[op->reg].d[1] = op->b;
                break;
            case SNX_A64P_OP_WANT_MEM: {
                const u64 v = op->b;
                p = snx_a64p_at(op->a, op->reg);
                snx_a64p_expect(p != NULL && op->reg <= 8, "the script: an expectation outside the window", 0, op->a, 0,
                                0);
                if (p && op->reg <= 8) memcpy(snx_a64p_want_window + (p - snx_a64p_window), &v, op->reg);
                break;
            }
            case SNX_A64P_OP_SAME: {
                u32 n;
                u32 k;
                int same = 1;
                for (n = 0; n < 32 && same; ++n) {
                    snprintf(what, sizeof(what), n == 31 ? "SP" : "X%u", n);
                    same = c.x[n] == snx_a64p_want.x[n];
                    snx_a64p_expect(same, what, 0, c.x[n], 0, snx_a64p_want.x[n]);
                }
                for (n = 0; n < 32 && same; ++n) {
                    snprintf(what, sizeof(what), "V%u", n);
                    same = !memcmp(&c.v[n], &snx_a64p_want.v[n], 16);
                    snx_a64p_expect(same, what, c.v[n].d[1], c.v[n].d[0], snx_a64p_want.v[n].d[1],
                                    snx_a64p_want.v[n].d[0]);
                }
                for (k = 0; k < SNX_A64P_WINDOW && same; ++k) {
                    snprintf(what, sizeof(what), "the byte at %s", snx_a64p_hex(snx_a64p_base + k, 0));
                    same = snx_a64p_window[k] == snx_a64p_want_window[k];
                    if (!same) snx_a64p_expect(0, what, 0, snx_a64p_window[k], 0, snx_a64p_want_window[k]);
                }
                if (same) snx_a64p_expect(1, "the whole state", 0, 0, 0, 0);
                break;
            }
            default: snx_a64p_expect(0, "the script: an unknown step", 0, op->op, 0, 0);
        }
    }
    return cases;
}

static int snx_a64p_is_nan(u32 v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu); }

/* fp_min_max.cpp's run_test, for each instruction on each path */
static int snx_a64p_minmax_run(void) {
    int interp;
    static const u32 bases[] = {0, SNX_A64P_FZ};
    const size_t rows = sizeof(snx_a64p_minmax) / sizeof(snx_a64p_minmax[0]);
    const size_t words = sizeof(snx_a64p_minmax_words) / sizeof(snx_a64p_minmax_words[0]);
    char what[96];
    int runs = 0;
    CPU c;
    RtAPI rt;
    for (interp = 0; interp < 2; ++interp) {
        size_t w;
        snx_a64p_path = interp ? "interpreter" : "AOT";
        for (w = 0; w < words; ++w) {
            size_t b;
            int dn;
            size_t r;
            int swap;
            const SnxA64pMinMaxWord* word = &snx_a64p_minmax_words[w];
            snx_a64p_name = word->text;
            for (b = 0; b < 2; ++b)
                for (dn = 0; dn < 2; ++dn)
                    for (r = 0; r < rows; ++r)
                        for (swap = 0; swap < (snx_a64p_minmax[r].one_way ? 1 : 2); ++swap) {
                            u64 host;
                            const SnxA64pMinMax* row = &snx_a64p_minmax[r];
                            u32 want = row->result[word->column];
                            if (dn && snx_a64p_is_nan(want)) want = 0x7FC00000u;
                            snx_a64p_reset(&c, &rt);
                            snx_a64p_memory(&c, 0, 0);
                            c.fpcr = bases[b] | (dn ? SNX_A64P_DN : 0);
                            c.v[0].d[0] = 42;
                            c.v[1].d[0] = swap ? row->b : row->a;
                            c.v[2].d[0] = swap ? row->a : row->b;
                            host = audit_host_save();
                            audit_fpcr(&c);
                            if (interp)
                                snx_step(&c, word->word, 0);
                            else
                                snx_a64p_minmax_aot[w](&c);
                            audit_host_restore(host);
                            snprintf(what, sizeof(what), "V0[63:0] for %s, %s under FPCR %08x",
                                     snx_a64p_hex(c.v[1].d[0], 8), snx_a64p_hex(c.v[2].d[0], 8), c.fpcr);
                            snx_a64p_expect(c.v[0].d[0] == want && !snx_a64p_traps, what, 0, c.v[0].d[0], 0, want);
                            ++runs;
                        }
        }
    }
    return runs;
}

/* FPToFixed.cpp, through the helper every FCVT* conversion to an integer calls */
static int snx_a64p_fixed_run(void) {
    size_t r;
    const size_t rows = sizeof(snx_a64p_fixed) / sizeof(snx_a64p_fixed[0]);
    char what[96];
    CPU c;
    RtAPI rt;
    snx_a64p_path = "snx_fp_to_int";
    snx_a64p_name = "FPToFixed";
    for (r = 0; r < rows; ++r) {
        u64 got;
        const SnxA64pFixed* f = &snx_a64p_fixed[r];
        snx_a64p_reset(&c, &rt);
        got = snx_fp_to_int(&c, f->input, f->fp_width, f->int_width, 0, 0, f->rm);
        snprintf(what, sizeof(what), "%s to %d bits, rounding %d", snx_a64p_hex(f->input, 16), f->int_width, f->rm);
        snx_a64p_expect(got == f->output, what, 0, got, 0, f->output);
        if (f->fpsr != ~0u) {
            snprintf(what, sizeof(what), "FPSR after %s to %d bits", snx_a64p_hex(f->input, 16), f->int_width);
            snx_a64p_expect(c.fpsr == f->fpsr, what, 0, c.fpsr, 0, f->fpsr);
        }
    }
    return (int)rows;
}

/* the steps a64_ported.c emitted are for these cases: a test binary older than a64_ported_cases.h is not */
static int snx_a64p_current(void) {
    size_t i;
    const size_t ops = sizeof(snx_a64p_script) / sizeof(snx_a64p_script[0]);
    const size_t words = sizeof(snx_a64p_minmax_words) / sizeof(snx_a64p_minmax_words[0]);
    int same = ops == sizeof(snx_a64p_emitted) / sizeof(snx_a64p_emitted[0]) &&
               words == sizeof(snx_a64p_emitted_minmax) / sizeof(snx_a64p_emitted_minmax[0]);
    for (i = 0; same && i < ops; ++i)
        same = snx_a64p_emitted[i] == (snx_a64p_script[i].op == SNX_A64P_OP_CODE ? (u32)snx_a64p_script[i].a : 0);
    for (i = 0; same && i < words; ++i) same = snx_a64p_emitted_minmax[i] == snx_a64p_minmax_words[i].word;
    if (!same) fputs("FAIL: the test binary predates a64_ported_cases.h; rebuild snx-test-a64-ported\n", stderr);
    return same;
}

int main(void) {
    int cases, minmax, fixed;
    if (!snx_a64p_current()) return 1;
    cases = snx_a64p_script_run(0);
    snx_a64p_script_run(1);
    minmax = snx_a64p_minmax_run();
    fixed = snx_a64p_fixed_run();
    printf("A64 cases ported from rEden's tests: %d cases on the AOT and interpreter paths, %d min/max runs, "
           "%d conversions, %d checks, %d failed\n",
           cases, minmax, fixed, snx_a64p_checks, snx_a64p_failures);
    return snx_a64p_failures != 0;
}
