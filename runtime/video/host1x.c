/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* nvhost-nvdec / nvhost-vic (host1x channels).
   We parse the host1x stream, decode NVDEC H.264 jobs with FFmpeg (SPS/PPS get rebuilt from the
   picture setup the game fills in) into the surface each job names, and VIC jobs convert their input slot's
   surface, unscaled, into the output surface.
   Struct layouts / method offsets come from NVIDIA's open-gpu-doc (nvdec_drv.h, clc5b0.h, VIC). */
#include "video/host1x_int.h"

/* ------------------------------------------------------------ IOVA (what the video engines see through the SMMU) */
/* A mapping lives until nvmap frees its handle (host1x_forget_handle), not until UNMAP_BUFFER: NVDEC and VIC map the
   same handles around every job, and players allocate fresh handles for each movie, so a table that only grew filled
   up after a few movies and every later job saw unmapped memory. g_iom is kept sorted by iova so that freed ranges
   are found again first-fit. */
typedef struct {
    u32 iova, size;
    u64 addr;
    u32 h;
} IoMap;
#define IOVA_BASE  0x10000000ull
#define IOVA_END   0x100000000ull /* MAP_BUFFER hands the iova back as a u32 */
#define IOVA_GUARD 0x10000ull
static IoMap g_iom[1024];
static int g_niom;
static PMutex g_hlk;
static int g_hinit; /* 0, 1 while pm_init runs, 2 ready: nvmap's FREE can get here first, from any thread */
static void h_init(void) {
    int s = 0;
    if (__atomic_load_n(&g_hinit, __ATOMIC_ACQUIRE) == 2) return;
    if (__atomic_compare_exchange_n(&g_hinit, &s, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pm_init(&g_hlk);
        __atomic_store_n(&g_hinit, 2, __ATOMIC_RELEASE);
    } else
        while (__atomic_load_n(&g_hinit, __ATOMIC_ACQUIRE) != 2) plat_yield();
}
static u64 iova_span(u32 size) { return (((u64)size + 0xFFFFu) & ~0xFFFFull) + IOVA_GUARD; }
static u32 iova_map(u32 h) {
    u64 a = nvmap_addr(h);
    if (!a) a = nvmap_addr_by_id(h);
    if (!a) return 0;
    for (int i = 0; i < g_niom; i++)
        if (g_iom[i].h == h && g_iom[i].addr == a) return g_iom[i].iova;
    /* nvmap's ids are its handle numbers, so h names the handle either way. A window of a made-up size would let
       iova_ptr hand out memory past the end of the buffer */
    u32 sz = nvmap_size(h);
    if (!sz) {
        WARN_LIMIT(4, "video: nvmap handle %u has no size, not mapped for NVDEC/VIC\n", h);
        return 0;
    }
    u64 need = iova_span(sz), base = IOVA_BASE;
    int at = 0;
    for (; at < g_niom; at++) {
        if (base + need <= g_iom[at].iova) break;
        u64 e = g_iom[at].iova + iova_span(g_iom[at].size);
        if (e > base) base = e;
    }
    if (g_niom >= 1024 || base + need > IOVA_END) {
        WARN_LIMIT(8,
                   "video: no room to map nvmap handle %u (%u bytes, %d mappings): NVDEC and VIC see unmapped memory\n",
                   h, sz, g_niom);
        return 0;
    }
    memmove(&g_iom[at + 1], &g_iom[at], (size_t)(g_niom - at) * sizeof g_iom[0]);
    g_iom[at].iova = (u32)base;
    g_iom[at].size = sz;
    g_iom[at].addr = a;
    g_iom[at].h = h;
    g_niom++;
    return (u32)base;
}
void host1x_forget_handle(u32 h) {
    h_init();
    pm_lock(&g_hlk);
    int k = 0;
    for (int i = 0; i < g_niom; i++)
        if (g_iom[i].h != h) g_iom[k++] = g_iom[i];
    g_niom = k;
    pm_unlock(&g_hlk);
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
/* A channel is one nvdrv fd, as on the console: the game opens two NVDEC and two VIC fds per movie and sends a
   stream's jobs through one of them, so the registers and the decoder (its reference pictures and headers) stay with
   their stream. The engines only meet in guest memory: NVDEC writes each picture into the surface its job names, and
   VIC reads its input slot from there */
#define H1_NSYNCPT 16 /* the syncpoint indexes GET_SYNCPOINT takes, nvhost's NVHOST_MODULE_MAX_SYNCPTS */
typedef struct {
    int is_vic;
    u32 syncpt[H1_NSYNCPT]; /* 0 until GET_SYNCPOINT or a submit asks for it */
    u32 regs[0x1000 / 4];
    u32 method;
    NvdecCtx* dec; /* NVDEC channels only */
} H1Ch;
static void dev_method(H1Ch* ch, u32 m, u32 v) {
    if (m >= 0x1000 / 4) return; /* past the register file: m << 2 would wrap onto a real register, even 0x300 */
    u32 off = m << 2;
    ch->regs[off / 4] = v;
    if (off == 0x300) {
        if (ch->is_vic)
            vic_execute(ch->regs);
        else
            nvdec_execute(ch->dec, ch->regs);
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
                LOG_LIMIT(6, "video: host1x opcode %u not handled (word %08x)\n", op, x);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------ channel ioctls */
void* host1x_open(int is_vic) {
    h_init();
    H1Ch* ch = calloc(1, sizeof *ch);
    if (!ch) return NULL;
    ch->is_vic = is_vic;
    if (!is_vic && !(ch->dec = nvdec_new())) {
        free(ch);
        return NULL;
    }
    return ch;
}
void host1x_close(void* p) {
    H1Ch* ch = p;
    if (!ch) return;
    h_init();
    pm_lock(&g_hlk);
    for (int i = 0; i < H1_NSYNCPT; i++)
        if (ch->syncpt[i]) syncpt_free(ch->syncpt[i]);
    nvdec_free(ch->dec);
    pm_unlock(&g_hlk);
    free(ch);
}
/* syncpoints come from nvdrv's allocator: NvRmStream indexes a 128-entry table by syncpoint id, and an id past it
   (upstream used 200) writes beyond the table into the game's heap. A channel's go back when its fd closes: four
   channels per movie would otherwise use up the ids after a few dozen movies */
static u32 ch_syncpt(H1Ch* ch, u32 i) {
    if (!ch->syncpt[i]) ch->syncpt[i] = syncpt_alloc();
    return ch->syncpt[i];
}
u32 host1x_ioctl(void* p, u32 cmd, u8* d, u32 n) {
    H1Ch* ch = p;
    if (!ch) return 4; /* BadParameter */
    int is_vic = ch->is_vic;
    h_init();
    pm_lock(&g_hlk);
    u32 r = 0, grp = (cmd >> 8) & 0xFF, nr = cmd & 0xFF;
    if (grp == 0x48) { /* SET_NVMAP_FD etc */
    } else if (grp == 0)
        switch (nr) {
            case 0x01: { /* SUBMIT: counts, cmdbufs {memid, offset, words}, relocs, reloc shifts, syncpt incrs {id, n, ?x3}, fence thresholds (u32 each, written back) */
                u32 ncb = rd32(d, 0), nrel = rd32(d, 4), nsp = rd32(d, 8), nfe = rd32(d, 12);
                /* in 64 bits: counts from the game can wrap a u32 sum back under n */
                u64 cbo = 16, spo = cbo + (u64)ncb * 12 + (u64)nrel * 16 + (u64)nrel * 4, feo = spo + (u64)nsp * 20;
                if (feo > n) {
                    r = 4; /* BadParameter */
                    break;
                }
                if (nrel)
                    LOG_LIMIT(1, "video: %s submit has %u relocations, which aren't applied\n",
                              is_vic ? "vic" : "nvdec", nrel);
                for (u32 i = 0; i < ncb; i++) {
                    u32 cb = (u32)(cbo + (u64)i * 12);
                    u32 mem = rd32(d, cb), off = rd32(d, cb + 4), wc = rd32(d, cb + 8);
                    u64 a = nvmap_addr(mem);
                    if (!a) a = nvmap_addr_by_id(mem);
                    if (!a || !wc || wc > 0x100000 || (u64)off + (u64)wc * 4 > nvmap_size(mem)) {
                        /* the job is lost, but its syncpoint increments below still happen so the game doesn't wait
                           forever on them */
                        LOG_LIMIT(4,
                                  "video: %s command buffer skipped (nvmap %u, offset %x, %u words, buffer %u bytes)\n",
                                  is_vic ? "vic" : "nvdec", mem, off, wc, nvmap_size(mem));
                        continue;
                    }
                    run_words(ch, (const u32*)GP(a + off), wc);
                }
                for (u32 i = 0; i < nsp; i++) {
                    u32 sp = (u32)(spo + (u64)i * 20);
                    u32 id = rd32(d, sp), inc = rd32(d, sp + 4);
                    if (!id) id = ch_syncpt(ch, 0);
                    for (u32 k = 0; k < inc && k < 64; k++) syncpt_incr(id);
                    if (i < nfe && feo + (u64)i * 4 + 4 <= n) {
                        u32 v = syncpt_read(id);
                        memcpy(d + feo + (u64)i * 4, &v, 4);
                    }
                }
                LOG_LIMIT(
                    3,
                    "video: %s submit: %u command buffers, %u relocs, %u syncpoint increments, %u fences (%u bytes)\n",
                    is_vic ? "vic" : "nvdec", ncb, nrel, nsp, nfe, n);
                break;
            }
            case 0x02: { /* GET_SYNCPOINT: {u32 index, u32 id} */
                u32 i = rd32(d, 0);
                if (i >= H1_NSYNCPT) {
                    r = 4; /* BadParameter */
                    break;
                }
                u32 v = ch_syncpt(ch, i);
                memcpy(d + 4, &v, 4);
                break;
            }
            case 0x03: {
                u32 v = 0;
                memcpy(d + 4, &v, 4);
                break;
            } /* GET_WAITBASE */
            case 0x07: break; /* SET_SUBMIT_TIMEOUT */
            case 0x09: {
                u32 ne = rd32(d, 0);
                for (u32 i = 0; i < ne && 12 + (u64)i * 8 + 8 <= n; i++) {
                    u32 h = rd32(d, 12 + i * 8);
                    u32 io = iova_map(h);
                    memcpy(d + 12 + i * 8 + 4, &io, 4);
                }
                break;
            } /* MAP_BUFFER */
            /* UNMAP_BUFFER: the mapping stays until nvmap frees the handle (host1x_forget_handle). NVDEC and VIC map
               the same handles and unmap them after every job, so dropping it here would take it from the other */
            case 0x0A: break;
            default: {
                LOG_LIMIT(8, "video: %s ioctl %08x not handled\n", is_vic ? "vic" : "nvdec", cmd);
                break;
            }
        }
    else { LOG_LIMIT(8, "video: %s ioctl %08x not handled\n", is_vic ? "vic" : "nvdec", cmd); }
    pm_unlock(&g_hlk);
    return r;
}
