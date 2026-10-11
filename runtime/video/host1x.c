/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* nvhost-nvdec / nvhost-vic (host1x channels).
   We parse the host1x stream, decode NVDEC H.264 jobs with FFmpeg (SPS/PPS get rebuilt from the
   picture setup the game fills in) and VIC jobs copy the last decoded picture into the output surface.
   Struct layouts / method offsets come from NVIDIA's open-gpu-doc (nvdec_drv.h, clc5b0.h, VIC). */
#include "video/host1x.h"
#include "gpu/gpu.h"
#include "hle/nvdrv.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>
#include "video/host1x_int.h"

/* ------------------------------------------------------------ IOVA (what the video engines see through the SMMU) */
typedef struct {
    u32 iova, size;
    u64 addr;
    u32 h;
} IoMap;
static IoMap g_iom[1024];
static int g_niom;
static u32 g_iova_next = 0x10000000u;
static PMutex g_hlk;
static int g_hinit;
static void h_init(void) {
    if (!g_hinit) {
        pm_init(&g_hlk);
        g_hinit = 1;
    }
}
static u32 iova_map(u32 h) {
    u64 a = nvmap_addr(h);
    if (!a) a = nvmap_addr_by_id(h);
    if (!a) return 0;
    for (int i = 0; i < g_niom; i++)
        if (g_iom[i].h == h && g_iom[i].addr == a) return g_iom[i].iova;
    u32 sz = nvmap_size(h);
    if (!sz) sz = 0x100000;
    if (g_niom >= 1024) return 0;
    u32 base = g_iova_next;
    g_iova_next += (sz + 0xFFFFu) & ~0xFFFFu;
    g_iova_next += 0x10000u;
    g_iom[g_niom].iova = base;
    g_iom[g_niom].size = sz;
    g_iom[g_niom].addr = a;
    g_iom[g_niom].h = h;
    g_niom++;
    return base;
}
/* 40-bit iova (engines get it >>8) -> host pointer */
u8* iova_ptr(u64 iova, u64 len) {
    for (int i = 0; i < g_niom; i++) {
        IoMap* m = &g_iom[i];
        if (iova >= m->iova && iova + len <= (u64)m->iova + m->size) return GP(m->addr + (iova - m->iova));
    }
    return NULL;
}

/* ------------------------------------------------------------ host1x command stream */
typedef struct {
    int is_vic;
    u32 syncpt;
    u32 regs[0x1000 / 4];
    u32 method;
} H1Ch;
static void dev_method(H1Ch* ch, u32 m, u32 v) {
    u32 off = m << 2;
    if (off < 0x1000) ch->regs[off / 4] = v;
    if (off == 0x300) {
        if (ch->is_vic)
            vic_execute(ch->regs);
        else
            nvdec_execute(ch->regs);
    }
}
static void reg_write(H1Ch* ch, u32 cls, u32 off, u32 v) {
    if (cls == 0x1) return; /* host1x class itself, incrs are already counted from the submit */
    if (off == 0x10)
        ch->method = v;
    else if (off == 0x11)
        dev_method(ch, ch->method, v);
}
static void run_words(H1Ch* ch, const u32* w, u32 n) {
    u32 cls = ch->is_vic ? 0x5D : 0xF0;
    for (u32 i = 0; i < n;) {
        u32 x = w[i++], op = x >> 28;
        switch (op) {
            case 0: {
                cls = (x >> 6) & 0x3FF;
                u32 off = (x >> 16) & 0xFFF, mask = x & 0x3F;
                for (int b = 0; b < 6; b++)
                    if (mask & (1u << b)) {
                        if (i >= n) return;
                        reg_write(ch, cls, off + b, w[i++]);
                    }
                break;
            }
            case 1: {
                u32 off = (x >> 16) & 0xFFF, c = x & 0xFFFF;
                for (u32 k = 0; k < c && i < n; k++) reg_write(ch, cls, off + k, w[i++]);
                break;
            }
            case 2: {
                u32 off = (x >> 16) & 0xFFF, c = x & 0xFFFF;
                for (u32 k = 0; k < c && i < n; k++) reg_write(ch, cls, off, w[i++]);
                break;
            }
            case 3: {
                u32 off = (x >> 16) & 0xFFF, mask = x & 0xFFFF;
                for (int b = 0; b < 16; b++)
                    if (mask & (1u << b)) {
                        if (i >= n) return;
                        reg_write(ch, cls, off + b, w[i++]);
                    }
                break;
            }
            case 4: reg_write(ch, cls, (x >> 16) & 0xFFF, x & 0xFFFF); break;
            default: {
                static int said;
                if (said < 6) {
                    said++;
                    LOG("video: host1x opcode %u not handled (word %08x)\n", op, x);
                }
                return;
            }
        }
    }
}

/* ------------------------------------------------------------ channel ioctls */
/* syncpoints come from nvdrv's allocator: NvRmStream indexes a 128-entry table by syncpoint id, and an id past it
   (upstream used 200) writes beyond the table into the game's heap */
static H1Ch g_ch[2];
u32 host1x_ioctl(int is_vic, u32 cmd, u8* d, u32 n) {
    h_init();
    pm_lock(&g_hlk);
    H1Ch* ch = &g_ch[is_vic ? 1 : 0];
    ch->is_vic = is_vic;
    if (!ch->syncpt) ch->syncpt = syncpt_alloc();
    u32 r = 0, grp = (cmd >> 8) & 0xFF, nr = cmd & 0xFF;
    if (grp == 0x48) { /* SET_NVMAP_FD etc */
    } else if (grp == 0)
        switch (nr) {
            case 0x01: { /* SUBMIT: counts, cmdbufs {memid, offset, words}, relocs, reloc shifts, syncpt incrs {id, n, ?x3}, fences {id, value} */
                u32 ncb = rd32(d, 0), nrel = rd32(d, 4), nsp = rd32(d, 8), nfe = rd32(d, 12);
                u32 o = 16;
                u32 cbo = o;
                o += ncb * 12;
                o += nrel * 16;
                o += nrel * 4;
                u32 spo = o;
                o += nsp * 20;
                u32 feo = o;
                if (o > n) {
                    r = 1;
                    break;
                }
                for (u32 i = 0; i < ncb; i++) {
                    u32 mem = rd32(d, cbo + i * 12), off = rd32(d, cbo + i * 12 + 4), wc = rd32(d, cbo + i * 12 + 8);
                    u64 a = nvmap_addr(mem);
                    if (!a) a = nvmap_addr_by_id(mem);
                    if (!a || !wc || wc > 0x100000) continue;
                    run_words(ch, (const u32*)GP(a + off), wc);
                }
                for (u32 i = 0; i < nsp; i++) {
                    u32 id = rd32(d, spo + i * 20), inc = rd32(d, spo + i * 20 + 4);
                    if (!id) id = ch->syncpt;
                    for (u32 k = 0; k < inc && k < 64; k++) syncpt_incr(id);
                    if (i < nfe && feo + i * 4 + 4 <= n) {
                        u32 v = syncpt_read(id);
                        memcpy(d + feo + i * 4, &v, 4);
                    }
                } /* fence thresholds, u32 each */
                {
                    static int said;
                    if (said < 3) {
                        said++;
                        LOG("video: %s submit: %u command buffers, %u relocs, %u syncpoint increments, %u fences (%u bytes)\n",
                            is_vic ? "vic" : "nvdec", ncb, nrel, nsp, nfe, n);
                    }
                }
                break;
            }
            case 0x02: {
                u32 v = ch->syncpt;
                memcpy(d + 4, &v, 4);
                break;
            } /* GET_SYNCPOINT */
            case 0x03: {
                u32 v = 0;
                memcpy(d + 4, &v, 4);
                break;
            } /* GET_WAITBASE */
            case 0x07: break; /* SET_SUBMIT_TIMEOUT */
            case 0x09: {
                u32 ne = rd32(d, 0);
                for (u32 i = 0; i < ne && 12 + i * 8 + 8 <= n; i++) {
                    u32 h = rd32(d, 12 + i * 8);
                    u32 io = iova_map(h);
                    memcpy(d + 12 + i * 8 + 4, &io, 4);
                }
                break;
            } /* MAP_BUFFER */
            case 0x0A: break; /* UNMAP_BUFFER: we leave the iova mapped */
            default: {
                static int said;
                if (said < 8) {
                    said++;
                    LOG("video: %s ioctl %08x not handled\n", is_vic ? "vic" : "nvdec", cmd);
                }
                break;
            }
        }
    else {
        static int said;
        if (said < 8) {
            said++;
            LOG("video: %s ioctl %08x not handled\n", is_vic ? "vic" : "nvdec", cmd);
        }
    }
    pm_unlock(&g_hlk);
    return r;
}
