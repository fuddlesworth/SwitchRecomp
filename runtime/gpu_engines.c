/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#include <stdio.h>
#include <stdlib.h>
/* engines: MME, 3D (regs, cb data, reports, clears), 2D, DMA, compute */
#include "gpu.h"

/* ============================================================ MME (macros) */
typedef struct {
    u32 regs[8];
    u32 carry;
    u32 method, incr, idx;
    const u32* params;
    u32 np, pi;
    GpuChannel* ch;
    u32 pc;
    int delayed;
    u32 dpc;
} Mme;
static void mme_send(Mme* m, u32 v) {
    maxwell3d_method(m->ch, m->method, v, 1);
    m->method += m->incr;
    m->method &= 0xFFF;
}
static void mme_setmethod(Mme* m, u32 v) {
    m->method = v & 0xFFF;
    m->incr = (v >> 12) & 0x3F;
}
static u32 mme_fetch(Mme* m) { return m->pi < m->np ? m->params[m->pi++] : 0; }
static void mme_setreg(Mme* m, u32 r, u32 v) {
    if (r) m->regs[r] = v;
}
static void mme_result(Mme* m, u32 op, u32 dst, u32 res) {
    switch (op) {
        case 0: mme_setreg(m, dst, mme_fetch(m)); break;
        case 1: mme_setreg(m, dst, res); break;
        case 2:
            mme_setreg(m, dst, res);
            mme_setmethod(m, res);
            break;
        case 3:
            mme_setreg(m, dst, mme_fetch(m));
            mme_send(m, res);
            break;
        case 4:
            mme_setreg(m, dst, res);
            mme_send(m, res);
            break;
        case 5:
            mme_setreg(m, dst, mme_fetch(m));
            mme_setmethod(m, res);
            break;
        case 6:
            mme_setreg(m, dst, res);
            mme_setmethod(m, res);
            mme_send(m, mme_fetch(m));
            break;
        case 7:
            mme_setreg(m, dst, res);
            mme_setmethod(m, res);
            mme_send(m, (res >> 12) & 0x3F);
            break;
    }
}
static u32 mme_alu(Mme* m, u32 op, u32 a, u32 b) {
    u64 r;
    switch (op) {
        case 0:
            r = (u64)a + b;
            m->carry = (u32)(r >> 32);
            return (u32)r;
        case 1:
            r = (u64)a + b + m->carry;
            m->carry = (u32)(r >> 32);
            return (u32)r;
        case 2:
            r = (u64)a - b;
            m->carry = r < 0x100000000ull;
            return (u32)r;
        case 3:
            r = (u64)a - b - (m->carry ? 0 : 1);
            m->carry = r < 0x100000000ull;
            return (u32)r;
        case 8: return a ^ b;
        case 9: return a | b;
        case 10: return a & b;
        case 11: return a & ~b;
        case 12: return ~(a & b);
    }
    {
        static int said;
        if (said < 8) {
            said++;
            WARN("mme: alu %u (not a Maxwell ALU operation) in macro %u at pc %x\n", op, m->idx, m->pc - 4);
        }
        static int dumped;
        if (!dumped) {
            dumped = 1;
            u32 st = m->ch->mme_start[m->idx & 0xFF];
            LOG("mme: macro %u starts at word %x (upload pointer %x); code from there:\n", m->idx, st,
                m->ch->mme_upload_ptr);
            for (u32 i = 0; i < 64; i += 8) {
                const u32* c = &m->ch->mme_code[(st + i) & 0x1FFF];
                LOG("  %04x: %08x %08x %08x %08x %08x %08x %08x %08x\n", st + i, c[0], c[1], c[2], c[3], c[4], c[5],
                    c[6], c[7]);
            }
            LOG("  macro start table:");
            for (u32 i = 0; i < 0x80; i++) {
                if (i % 16 == 0) LOG("\n   ");
                LOG(" %x", m->ch->mme_start[i]);
            }
            LOG("\n");
        }
    }
    return 0;
}
static int mme_step(Mme* m, int delay) {
    u32 base = m->pc;
    u32 w = m->ch->mme_code[(m->pc / 4) & 0x1FFF];
    if (m->delayed) {
        m->pc = m->dpc;
        m->delayed = 0;
    } else
        m->pc += 4;
    u32 op = w & 7, rop = (w >> 4) & 7, dst = (w >> 8) & 7, sa = (w >> 11) & 7, sb = (w >> 14) & 7;
    i32 imm = ((i32)(w << 0)) >> 14; /* imm: 18 bits signed @14 */
    imm = (i32)(w >> 14);
    if (imm & 0x20000) imm |= ~0x3FFFF;
    u32 bf_src = (w >> 17) & 31, bf_size = (w >> 22) & 31, bf_dst = (w >> 27) & 31;
    u32 mask = (1u << bf_size) - 1;
    if (bf_size == 0) mask = 0;
    switch (op) {
        case 0: mme_result(m, rop, dst, mme_alu(m, (w >> 17) & 31, m->regs[sa], m->regs[sb])); break;
        case 1: mme_result(m, rop, dst, m->regs[sa] + (u32)imm); break;
        case 2: {
            u32 d = m->regs[sa], s = m->regs[sb];
            s = (s >> bf_src) & mask;
            d &= ~(mask << bf_dst);
            d |= s << bf_dst;
            mme_result(m, rop, dst, d);
            break;
        }
        case 3: {
            u32 d = m->regs[sa], s = m->regs[sb];
            mme_result(m, rop, dst, ((s >> d) & mask) << bf_dst);
            break;
        }
        case 4: {
            u32 d = m->regs[sa], s = m->regs[sb];
            mme_result(m, rop, dst, ((s >> bf_src) & mask) << d);
            break;
        }
        case 5: {
            u32 a = (m->regs[sa] + (u32)imm) & 0xFFF;
            mme_result(m, rop, dst, m->ch->r3d[a]);
            break;
        }
        case 7: {
            u32 v = m->regs[sa];
            int nz = (w >> 4) & 1, annul = (w >> 5) & 1;
            int taken = nz ? v != 0 : v == 0;
            if (taken) {
                if (annul) {
                    m->pc = base + (u32)(imm * 4);
                    return 1;
                }
                m->delayed = 1;
                m->dpc = base + (u32)(imm * 4);
                mme_step(m, 1);
                return 1;
            }
            return 1;
        } /* bit 7 is not exit on branches (yuzu does the same) */
        default: WARN("mme: op %u\n", op); break;
    }
    if (((w >> 7) & 1) && !delay) {
        mme_step(m, 1);
        return 0;
    }
    return 1;
}
static void mme_run(GpuChannel* ch, u32 idx, const u32* params, u32 np) {
    Mme m;
    memset(&m, 0, sizeof m);
    m.ch = ch;
    m.idx = idx;
    m.params = params;
    m.np = np;
    m.pi = 1;
    m.regs[1] = np ? params[0] : 0;
    m.pc = ch->mme_start[idx & 0xFF] * 4;
    for (int guard = 0; guard < 5000000; guard++)
        if (!mme_step(&m, 0)) break;
}

/* ============================================================ Maxwell 3D */
#define R3(i) (ch->r3d[i])
extern void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex,
                     u32 base_instance, u32 instances);
extern void r3d_clear(GpuChannel* ch, u32 v);
extern void r3d_const_upload(GpuChannel* ch, u32 v);
extern void r3d_cb_bind(GpuChannel* ch, int stage, u32 v);
extern u64 g_draws;
static u32 g_instance_count, g_draw_inline_count;
/* instancing comes in as N BEGIN/END pairs with the next-instance bit. we accumulate them and emit
   one draw of N instances once something else shows up */
static struct {
    int on, indexed;
    GpuChannel* ch;
    u32 topo, first, count, bv, bi, inst;
} g_pd;
u64 g_draws_merged;
void r3d_pending_flush(void) {
    if (!g_pd.on) return;
    g_pd.on = 0;
    r3d_draw(g_pd.ch, g_pd.indexed, g_pd.topo, g_pd.first, g_pd.count, g_pd.bv, g_pd.bi, g_pd.inst);
}
int r3d_pending_on(void) { return g_pd.on; }

/* timestamps for report semaphores.
   Games with DRS (SMO via al::GpuPerf / agl::perf::GPUTimeStamp) time the GPU with these and drop resolution
   when it looks slow. Our "GPU" timing is meaningless for that, see below. */
/* sead::GraphicsNvn::convertGPUTimeStampToSystemTick compares them against the CPU tick, so they must use
   the same base (guest_ns). We stamp each report with the time the work was submitted, which makes the
   measured GPU time ~0 and the game stays at full res. game_dynamic_resolution=1 stamps real time instead. */
volatile u64 g_gpu_sub_ns;
u64 gpu_ticks(void) {
    extern int g_opt_drs;
    extern u64 guest_ns(void);
    static u64 last;
    u64 n = g_opt_drs ? guest_ns() : (g_gpu_sub_ns ? g_gpu_sub_ns : guest_ns());
    if (n <= last) n = last + 16;
    last = n; /* monotonic, nudged by 1ns within a frame */
    return n * 384 / 625;
}
static void query_get(GpuChannel* ch, u32* r, int base, u32 v) {
    u64 addr = ((u64)(r[base] & 0xFF) << 32) | r[base + 1];
    u32 seq = r[base + 2];
    u32 op = v & 3;
    if (op == 0) { /* release */
        if (((v >> 4) & 1) || ((v >> 28) & 1))
            gmem_w32(addr, seq);
        else {
            u64 q[2] = {seq, gpu_ticks()};
            gmem_write(addr, q, 16);
        }
    } else if (op == 1) { /* acquire */
        u64 end = plat_ns() + 1000000000ull;
        while (gmem_r32(addr) != seq && plat_ns() < end) plat_yield();
    } else if (op == 2) { /* counter */
        u32 sel = (v >> 23) & 31;
        u64 val = sel == 0 ? 0 : (sel == 1 ? g_draws * 3 : 1000);
        if ((v >> 28) & 1)
            gmem_w32(addr, (u32)val);
        else {
            u64 q[2] = {val, gpu_ticks()};
            gmem_write(addr, q, 16);
        }
    }
}

void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last) {
    if (m >= 0xE00) { /* macro call */
        if (!ch->mme_active) {
            ch->mme_active = 1;
            ch->mme_index = (m - 0xE00) / 2;
            ch->mme_nparams = 0;
        }
        if (ch->mme_nparams < 0x10000) ch->mme_params[ch->mme_nparams++] = v;
        if (last) {
            ch->mme_active = 0;
            mme_run(ch, ch->mme_index, ch->mme_params, ch->mme_nparams);
            ch->mme_nparams = 0;
        }
        return;
    }
    m &= 0xFFF;
    if (g_pd.on) { /* keep accumulating only for BEGIN(next instance), END, ranges, or writes that don't change a reg */
        int keep;
        switch (m) {
            case 0x586: keep = (v >> 26) & 1; break;
            case 0x585:
            case 0x35D:
            case 0x35E:
            case 0x5F7:
            case 0x5F8: keep = 1; break;
            case 0x45:
            case 0x46:
            case 0x47:
            case 0x48:
            case 0x6C:
            case 0x6D:
            case 0xB2:
            case 0x674:
            case 0x6C3:
            case 0x904:
            case 0x90C:
            case 0x914:
            case 0x91C:
            case 0x924: keep = 0; break;
            default: keep = (m < 0x8E4 || m > 0x8F3) && R3(m) == v; break;
        }
        if (!keep || g_pd.ch != ch) r3d_pending_flush();
    }
    R3(m) = v;
    switch (m) {
        case 0x45: ch->mme_upload_ptr = v; break;
        case 0x46: ch->mme_code[ch->mme_upload_ptr++ & 0x1FFF] = v; break;
        case 0x47: ch->mme_start_ptr = v; break;
        case 0x48: ch->mme_start[ch->mme_start_ptr++ & 0xFF] = v; break;
        case 0x6C: i2m_launch(ch, ch->r3d, 0x60); break;
        case 0x6D: i2m_data(ch, v); break;
        case 0xB2:
            if (v & (1u << 20)) {
                extern void gpu_gl_stream_incr(u32);
                gpu_gl_stream_incr(v & 0xFFFF);
            }
            break;                           /* SyncInfo */
        case 0x674: r3d_clear(ch, v); break; /* ClearBuffers */
        case 0x6C3: query_get(ch, ch->r3d, 0x6C0, v); break;
        case 0x585: { /* END: flush */
            u32 topo = R3(0x586) & 0xFFFF;
            int idx = R3(0x5F8) != 0;
            u32 first = idx ? R3(0x5F7) : R3(0x35D), count = idx ? R3(0x5F8) : R3(0x35E), bv = idx ? R3(0x50D) : 0,
                bi = R3(0x50E);
            if (g_pd.on && g_pd.ch == ch && ((R3(0x586) >> 26) & 1) && g_pd.indexed == idx && g_pd.topo == topo &&
                g_pd.first == first && g_pd.count == count && g_pd.bv == bv && g_pd.bi == bi) {
                g_pd.inst = g_instance_count + 1;
                g_draws_merged++;
            } /* one more instance */
            else {
                r3d_pending_flush();
                g_pd.on = 1;
                g_pd.ch = ch;
                g_pd.indexed = idx;
                g_pd.topo = topo;
                g_pd.first = first;
                g_pd.count = count;
                g_pd.bv = bv;
                g_pd.bi = bi;
                g_pd.inst = g_instance_count + 1;
            }
            /* hw consumes the counts on each draw; if we don't, the next non-indexed draw picks up stale indices */
            R3(0x5F8) = 0;
            R3(0x35E) = 0;
            break;
        }
        case 0x586: /* BEGIN, bit 26 = next instance */
            if (v & (1u << 26))
                g_instance_count++;
            else if (!(v & (1u << 27)))
                g_instance_count = 0;
            break;
        case 0x8E4:
        case 0x8E5:
        case 0x8E6:
        case 0x8E7:
        case 0x8E8:
        case 0x8E9:
        case 0x8EA:
        case 0x8EB:
        case 0x8EC:
        case 0x8ED:
        case 0x8EE:
        case 0x8EF:
        case 0x8F0:
        case 0x8F1:
        case 0x8F2:
        case 0x8F3: r3d_const_upload(ch, v); break;
        case 0x904:
        case 0x90C:
        case 0x914:
        case 0x91C:
        case 0x924: r3d_cb_bind(ch, (int)((m - 0x904) / 8), v); break;
        case 0x6F8:
        case 0x6F9:
        case 0x6FA:
        case 0x6FB:
        case 0x6FC:
        case 0x6FD:
        case 0x6FE:
        case 0x6FF: /* draw inline */ break;
        default: break;
    }
    (void)g_draw_inline_count;
}

/* ============================================================ compute + I2M */
volatile u64 g_compute_launches;
void compute_method(GpuChannel* ch, u32 m, u32 v, int last) {
    (void)last;
    m &= 0xFFF;
    ch->rcomp[m] = v;
    switch (m) {
        case 0x6C: i2m_launch(ch, ch->rcomp, 0x60); break;
        case 0x6D: i2m_data(ch, v); break;
        case 0xAF: /* launch; the first few programs get dumped for debugging */ {
            g_compute_launches++;
            extern int g_gl;
            extern void gpu_gl_note_compute(void);
            if (g_gl) gpu_gl_note_compute();
            u64 qmd = (u64)ch->rcomp[0xAD] << 8;
            u64 code = ((u64)ch->rcomp[0x582] << 32) | ch->rcomp[0x583];
            u32 q[64];
            gmem_read(qmd, q, 256);
            if (g_gl) {
                extern void gpu_gl_dispatch(GpuChannel*, const u32*, u64);
                gpu_gl_dispatch(ch, q, code);
            }
            u64 prog = code + q[8];
            static u64 seen[32];
            static int ns;
            int known = 0;
            for (int i = 0; i < ns; i++)
                if (seen[i] == prog) known = 1;
            extern int g_opt_diag;
            if (!known && ns < 32 && g_opt_diag) {
                seen[ns++] = prog;
                extern int plat_mkdir(const char*);
                plat_mkdir("captures");
                plat_mkdir("captures/compute");
                char p[160];
                snprintf(p, sizeof p, "captures/compute/c%02d_qmd.bin", ns);
                FILE* f = fopen(p, "wb");
                if (f) {
                    fwrite(q, 1, 256, f);
                    fclose(f);
                }
                u8* b = malloc(0x10000);
                gmem_read(prog, b, 0x10000);
                snprintf(p, sizeof p, "captures/compute/c%02d_cs.bin", ns);
                f = fopen(p, "wb");
                if (f) {
                    fwrite(b, 1, 0x10000, f);
                    fclose(f);
                }
                free(b);
                for (int i = 0; i < 8; i++)
                    if ((q[20] >> i) & 1) {
                        u64 a = ((u64)(q[30 + 2 * i] & 0xFF) << 32) | q[29 + 2 * i];
                        u32 sz = (q[30 + 2 * i] >> 15) & 0x1FFFF;
                        if (!sz || sz > 0x10000) sz = 0x10000;
                        u8* cb = malloc(sz);
                        gmem_read(a, cb, sz);
                        snprintf(p, sizeof p, "captures/compute/c%02d_cb%d.bin", ns, i);
                        f = fopen(p, "wb");
                        if (f) {
                            fwrite(cb, 1, sz, f);
                            fclose(f);
                        }
                        free(cb);
                    }
                LOG("compute %d: grid %ux%ux%u, block %ux%ux%u, shared %u bytes, cbufs %02x (saved in captures/compute)\n",
                    ns, q[12] & 0x7FFFFFFF, q[13] & 0xFFFF, q[13] >> 16, q[18] >> 16, q[19] & 0xFFFF, q[19] >> 16,
                    q[17] & 0x3FFFF, q[20] & 0xFF);
            }
        } break;
        case 0x6C3: query_get(ch, ch->rcomp, 0x6C0, v); break; /* release the semaphore, the game may be waiting */
    }
}
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v) {
    m &= 0x7F;
    ch->ri2m[m] = v;
    if (m == 0x6C)
        i2m_launch(ch, ch->ri2m, 0x60);
    else if (m == 0x6D)
        i2m_data(ch, v);
}

/* ============================================================ Fermi 2D (blits) */
typedef struct {
    u32 format, linear, bh, depth, layer, pitch, width, height;
    u64 addr;
} Surf2D;
static void surf2d(u32* r, int b, Surf2D* s) {
    s->format = r[b];
    s->linear = r[b + 1];
    s->bh = (r[b + 2] >> 4) & 15;
    s->depth = r[b + 3];
    s->layer = r[b + 4];
    s->pitch = r[b + 5];
    s->width = r[b + 6];
    s->height = r[b + 7];
    s->addr = ((u64)r[b + 8] << 32) | r[b + 9];
}
u32 fmt_bpp_2d(u32 f) {
    switch (f) {
        case 0xE6:
        case 0xE9:
        case 0xD5:
        case 0xCF:
        case 0xD8:
        case 0xD9:
        case 0xDF:
        case 0xC2:
        case 0xCA:
        case 0xDE:
        case 0xC8:
        case 0xE0:
        case 0xE2: return 4;
        case 0xF3:
        case 0xE8:
        case 0xF8:
        case 0xEA:
        case 0xEB:
        case 0xEC:
        case 0xFE:
        case 0xFF: return 1;
        case 0xC0:
        case 0xC1:
        case 0xC3:
        case 0xC4:
        case 0xC5:
        case 0xC6:
        case 0xC7: return 16;
        case 0xCB:
        case 0xCC:
        case 0xCE: return 8;
        default: return 4;
    }
}
extern int r3d_blit(Surf2D* src, Surf2D* dst, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                    double sdy);
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) {
    m &= 0x3FF;
    ch->r2d[m] = v;
    if (m == 0x237) { /* src_y0 write kicks the blit */
        u32* r = ch->r2d;
        Surf2D dst, src;
        surf2d(r, 0x80, &dst);
        surf2d(r, 0x8C, &src);
        i32 dx0 = (i32)r[0x22C], dy0 = (i32)r[0x22D], dw = (i32)r[0x22E], dh = (i32)r[0x22F];
        /* 32.32 fixed point, frac reg comes before int reg (DU_DX_FRAC, DU_DX_INT...) */
        i64 dudx = (i64)(((u64)r[0x231] << 32) | r[0x230]);
        i64 dvdy = (i64)(((u64)r[0x233] << 32) | r[0x232]);
        i64 sx0 = (i64)(((u64)r[0x235] << 32) | r[0x234]);
        i64 sy0 = (i64)(((u64)r[0x237] << 32) | r[0x236]);
        double sx = (double)sx0 / 4294967296.0, sy = (double)sy0 / 4294967296.0, sdx = (double)dudx / 4294967296.0,
               sdy = (double)dvdy / 4294967296.0;
        /* CENTER origin (sample_mode bit0 == 0): coords address texel centers */
        if (!(r[0x223] & 1)) {
            sx -= sdx * 0.5;
            sy -= sdy * 0.5;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
        }
        if (r3d_blit(&src, &dst, dx0, dy0, dw, dh, sx, sy, sdx, sdy)) return;
        extern int g_gl;
        extern void gpu_gl_rt_flush(u64);
        extern void gpu_gl_rt_reload(u64);
        if (g_gl) {
            gpu_gl_rt_flush(src.addr);
            gpu_gl_rt_flush(dst.addr);
        }
        u32 bpp = fmt_bpp_2d(dst.format), sbpp = fmt_bpp_2d(src.format);
        if (bpp != sbpp) bpp = sbpp;
        for (i32 y = 0; y < dh; y++) {
            u32 syy = (u32)(sy + sdy * y);
            for (i32 x = 0; x < dw; x++) {
                u32 sxx = (u32)(sx + sdx * x);
                u8 px[16];
                u64 so = src.linear ? (u64)syy * src.pitch + sxx * bpp
                                    : bl_offset(sxx * bpp, syy, 0, src.width * bpp, src.height, src.bh, 0);
                u64 dof = dst.linear ? (u64)(dy0 + y) * dst.pitch + (dx0 + x) * bpp
                                     : bl_offset((dx0 + x) * bpp, dy0 + y, 0, dst.width * bpp, dst.height, dst.bh, 0);
                gmem_read(src.addr + so, px, bpp);
                gmem_write(dst.addr + dof, px, bpp);
            }
        }
        if (g_gl) gpu_gl_rt_reload(dst.addr);
    }
}

/* ============================================================ DMA */
volatile u64 g_dma_copies;
/* host pointer to a whole bl surface (slice dz, block depth bd) if it's contiguous, so DMA can memcpy
   instead of translating + tracking every 16 bytes */
static u8* bl_span_ptr(u64 base, u32 wbytes, u32 h, u32 bh, u32 z, u32 bd, u64* span) {
    static int off = -1;
    if (off < 0) off = getenv("SWR_DMA_SLOW") != NULL;
    if (off) return NULL;
    u64 gh = 8ull << bh, gd = 1ull << bd;
    u64 sz = (((u64)wbytes + 63) & ~63ull) * ((h + gh - 1) / gh * gh) * ((z / gd + 1) * gd);
    if (!sz || sz > (256ull << 20)) return NULL;
    u8* p = gmem_ptr(base, sz);
    if (p) *span = sz;
    return p;
}
static void bl_span_done(u64 base, u64 span) {
    extern void (*g_ww_touch)(u64);
    extern u8* g_mem;
    if (g_ww_touch) {
        u8* p = gmem_ptr(base, span);
        if (p)
            for (u64 o = 0; o < span; o += 0x10000) g_ww_touch((u64)(p - g_mem) + o);
    }
    gpu_mem_written(base, span);
}
void dma_method(GpuChannel* ch, u32 m, u32 v) {
    m &= 0x1FF;
    ch->rdma[m] = v;
    if (m != 0xC0) return; /* launch_dma */
    u32* r = ch->rdma;
    u64 src = ((u64)r[0x100] << 32) | r[0x101], dst = ((u64)r[0x102] << 32) | r[0x103];
    u32 pitch_in = r[0x104], pitch_out = r[0x105], line_len = r[0x106], lines = r[0x107];
    int src_linear = (v >> 7) & 1, dst_linear = (v >> 8) & 1, multi = (v >> 9) & 1, remap = (v >> 10) & 1;
    if (!multi) lines = 1;
    u32 bpp = 1;
    if (remap) {
        u32 rc = r[0x1C2];
        u32 comp = ((rc >> 16) & 3) + 1, csz = ((rc >> 20) & 3) + 1;
        bpp = comp * csz;
        line_len *= bpp;
    }
    /* bl params: dst 0x1C3.., src 0x1CA.. */
    u32 dbh = (r[0x1C3] >> 4) & 15, dw = r[0x1C4], dh = r[0x1C5], dd = r[0x1C6];
    (void)dd;
    u32 sbh = (r[0x1CA] >> 4) & 15, sw = r[0x1CB], sh = r[0x1CC];
    u32 sx = r[0x1CF] & 0xFFFF, sy = r[0x1CF] >> 16;
    u32 dx = r[0x1C8] & 0xFFFF, dy = r[0x1C8] >> 16;
    /* array layer = whole 2D bl surface stride */
    /* 3D (depth>1): layer reg is the z slice inside the volume, block depth at 11:8 */
    u32 dz = 0, sz_ = 0, dbd = 0, sbd = 0;
    if (!dst_linear && r[0x1C6] > 1) {
        dz = r[0x1C7];
        dbd = (r[0x1C3] >> 8) & 15;
    }
    if (!src_linear && r[0x1CD] > 1) {
        sz_ = r[0x1CE];
        sbd = (r[0x1CA] >> 8) & 15;
    }
    {
        u32 dl = dz ? 0 : r[0x1C7], sl = sz_ ? 0 : r[0x1CE];
        if (r[0x1C6] > 1) dl = 0;
        if (r[0x1CD] > 1) sl = 0;
        if (!dst_linear && dl) {
            u32 wbb = dw * (remap ? bpp : 1);
            u64 gh = 8u << dbh;
            u64 lsz = (((u64)wbb + 63) & ~63ull) * ((dh + gh - 1) / gh * gh);
            dst += lsz * dl;
        }
        if (!src_linear && sl) {
            u32 wbb = sw * (remap ? bpp : 1);
            u64 gh = 8u << sbh;
            u64 lsz = (((u64)wbb + 63) & ~63ull) * ((sh + gh - 1) / gh * gh);
            src += lsz * sl;
        }
    }
    {
        static u64 ndma;
        ndma++;
        extern volatile u64 g_dma_copies;
        g_dma_copies = ndma;
    }
    {
        static int said;
        if (said < 6) {
            said++;
            LOG("dma: %s->%s src=%llx dst=%llx pitch %u/%u len=%u lines=%u multi=%d remap=%d(%x) dst[bh=%u w=%u h=%u d=%u l=%u o=%x] src[bh=%u w=%u h=%u o=%x] launch=%x\n",
                src_linear ? "lin" : "bl", dst_linear ? "lin" : "bl", (unsigned long long)src, (unsigned long long)dst,
                pitch_in, pitch_out, line_len, lines, multi, remap, r[0x1C2], dbh, dw, dh, r[0x1C6], r[0x1C7], r[0x1C8],
                sbh, sw, sh, r[0x1CF], v);
        }
    }
    extern int g_gl;
    extern void gpu_gl_rt_flush(u64);
    extern void gpu_gl_rt_reload(u64);
    extern void gpu_gl_trace(const char*);
    if (g_gl) {
        char tb[256];
        snprintf(tb, sizeof tb, "DMA %s %llx -> %s %llx len=%u lines=%u remap=%d\n", src_linear ? "lin" : "bl",
                 (unsigned long long)src, dst_linear ? "lin" : "bl", (unsigned long long)dst, line_len, lines, remap);
        gpu_gl_trace(tb);
        gpu_gl_rt_flush(src);
        gpu_gl_rt_flush(dst);
    }
    u8* dpp = NULL;
    u8* spp = NULL;
    u64 dspan = 0, sspan = 0;
    (void)spp;
    (void)sspan;
    if (src_linear && dst_linear) {
        if (!multi) {
            u8* tmp = malloc(line_len);
            gmem_read(src, tmp, line_len);
            gmem_write(dst, tmp, line_len);
            free(tmp);
        } else {
            u8* tmp = malloc(line_len);
            for (u32 y = 0; y < lines; y++) {
                gmem_read(src + (u64)y * pitch_in, tmp, line_len);
                gmem_write(dst + (u64)y * pitch_out, tmp, line_len);
            }
            free(tmp);
        }
    } else if (src_linear && !dst_linear &&
               (dpp = bl_span_ptr(dst, dw * (remap ? bpp : 1), dh, dbh, dz, dbd, &dspan))) {
        /* contiguous dst: copy directly, one write record at the end */
        u32 wb = dw * (remap ? bpp : 1);
        u8* buf = malloc(line_len);
        for (u32 y = 0; y < lines; y++) {
            gmem_read(src + (u64)y * pitch_in, buf, line_len);
            for (u32 x = 0; x < line_len;) {
                u32 xb = dx * (remap ? bpp : 1) + x, n = 16 - (xb & 15);
                if (n > line_len - x) n = line_len - x;
                u64 o = bl_offset(xb, dy + y, dz, wb, dh, dbh, dbd);
                if (o + n <= dspan)
                    memcpy(dpp + o, buf + x, n);
                else
                    gmem_write(dst + o, buf + x, n);
                x += n;
            }
        }
        free(buf);
        bl_span_done(dst, dspan);
    } else if (src_linear && !dst_linear) {
        u32 wb = dw * (remap ? bpp : 1);
        u8* buf = malloc(line_len);
        for (u32 y = 0; y < lines; y++) {
            gmem_read(src + (u64)y * pitch_in, buf, line_len);
            /* bl keeps 16 byte groups contiguous, so copy per group not per byte (was ~11% in profiles) */
            for (u32 x = 0; x < line_len;) {
                u32 xb = dx * (remap ? bpp : 1) + x, n = 16 - (xb & 15);
                if (n > line_len - x) n = line_len - x;
                u64 o = bl_offset(xb, dy + y, dz, wb, dh, dbh, dbd);
                gmem_write(dst + o, buf + x, n);
                x += n;
            }
        }
        free(buf);
    } else if (!src_linear && dst_linear) {
        u32 wb = sw * (remap ? bpp : 1);
        u8* buf = malloc(line_len);
        for (u32 y = 0; y < lines; y++) {
            for (u32 x = 0; x < line_len;) {
                u32 xb = sx * (remap ? bpp : 1) + x, n = 16 - (xb & 15);
                if (n > line_len - x) n = line_len - x;
                u64 o = bl_offset(xb, sy + y, sz_, wb, sh, sbh, sbd);
                gmem_read(src + o, buf + x, n);
                x += n;
            }
            gmem_write(dst + (u64)y * pitch_out, buf, line_len);
        }
        free(buf);
    } else if (!src_linear && !dst_linear &&
               (dpp = bl_span_ptr(dst, dw * (remap ? bpp : 1), dh, dbh, dz, dbd, &dspan)) &&
               (spp = bl_span_ptr(src, sw * (remap ? bpp : 1), sh, sbh, sz_, sbd, &sspan))) {
        u32 swb = sw * (remap ? bpp : 1), dwb = dw * (remap ? bpp : 1);
        for (u32 y = 0; y < lines; y++)
            for (u32 x = 0; x < line_len;) {
                u32 xs = sx * (remap ? bpp : 1) + x, xd = dx * (remap ? bpp : 1) + x, n = 16 - (xs & 15),
                    m = 16 - (xd & 15);
                if (m < n) n = m;
                if (n > line_len - x) n = line_len - x;
                u64 so = bl_offset(xs, sy + y, sz_, swb, sh, sbh, sbd),
                    dof = bl_offset(xd, dy + y, dz, dwb, dh, dbh, dbd);
                if (so + n <= sspan && dof + n <= dspan)
                    memmove(dpp + dof, spp + so, n);
                else {
                    u8 b[16];
                    gmem_read(src + so, b, n);
                    gmem_write(dst + dof, b, n);
                }
                x += n;
            }
        bl_span_done(dst, dspan);
    } else {
        u32 swb = sw * (remap ? bpp : 1), dwb = dw * (remap ? bpp : 1);
        for (u32 y = 0; y < lines; y++)
            for (u32 x = 0; x < line_len;) {
                u32 xs = sx * (remap ? bpp : 1) + x, xd = dx * (remap ? bpp : 1) + x, n = 16 - (xs & 15),
                    m = 16 - (xd & 15);
                if (m < n) n = m;
                if (n > line_len - x) n = line_len - x;
                u8 b[16];
                gmem_read(src + bl_offset(xs, sy + y, sz_, swb, sh, sbh, sbd), b, n);
                gmem_write(dst + bl_offset(xd, dy + y, dz, dwb, dh, dbh, dbd), b, n);
                x += n;
            }
    }
    if (g_gl) gpu_gl_rt_reload(dst);
    /* completion semaphore */
    u32 sem = (v >> 3) & 3;
    if (sem) {
        u64 sa = ((u64)r[0x90] << 32) | r[0x91];
        if (sem == 1 || sem == 2) gmem_w32(sa, r[0x92]);
        if (sem == 2) {
            u64 t = gpu_ticks();
            gmem_w64(sa + 8, t);
        }
    }
}

void gpu_flush(GpuChannel* ch) {
    extern void r3d_flush(GpuChannel * ch);
    r3d_flush(ch);
}
