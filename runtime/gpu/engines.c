/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#include <string.h>
#include "core/main.h"
#include "core/options.h"
#include "gpu/renderer.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
/* engines: MME, 3D (regs, cb data, reports, clears), 2D, DMA, compute */
#include "gpu/gpu.h"
#include "core/clock.h"
#include "gpu/engines_int.h"

/* ============================================================ Maxwell 3D */
#define R3(i) (ch->r3d[i])
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
    static u64 last;
    u64 n = g_opt_drs ? guest_ns() : (g_gpu_sub_ns ? g_gpu_sub_ns : guest_ns());
    if (n <= last) n = last + 16;
    last = n; /* monotonic, nudged by 1ns within a frame */
    return n * 384 / 625;
}
void query_get(GpuChannel* ch, u32* r, int base, u32 v) {
    u64 addr = ((u64)(r[base] & 0xFF) << 32) | r[base + 1];
    u32 seq = r[base + 2];
    u32 op = v & 3;
    if (op != 1) gpu_signal_point();
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
            if (v & (1u << 20)) { g_renderer->stream_incr(v & 0xFFFF); }
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
