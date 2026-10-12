/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* nvdrv: nvmap, nvhost-as-gpu, nvhost-ctrl, nvhost-ctrl-gpu, nvhost-gpu. Enough for NVN and deko3d. */
#include "hle/nvdrv.h"
#include "gpu/gpu.h"
#include "kernel/ipc.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "video/host1x.h"
#include "core/clock.h"
#include <stdlib.h>
#include <string.h>
static void nv_events_check(void);

/* ============================================================ nvmap */
typedef struct {
    u32 size, align, heap, flags;
    u8 kind;
    u64 addr;
    u32 id;
    int alive;
    u32 refs; /* CREATE and each FROM_ID take one, FREE drops one: the handle dies with the last */
} NvMapH;
/* what MAP_BUFFER_EX mapped, for UNMAP_BUFFER: [gva, gva+size) maps cpu. base is the address UNMAP_BUFFER names; a
   modify (flag 0x100) or FREE_SPACE splits a mapping into pieces that keep it. va is the VA the mapping took from
   the allocator (0 when the game placed it), given back when it is unmapped */
typedef struct {
    u64 gva, size, cpu;
    u64 base, va;
} GMap;
typedef struct {
    u64 a, e;
} VaRange;
/* nvhost-ctrl events = async syncpoint waits */
typedef struct {
    KEvent* ev;
    u32 id, thresh;
    volatile int armed;
} NvEvent;
typedef struct {
    int dev;
    u32 syncpt;
    u64 user_data;
    u32 class_ids[8];
    int nclass;
    /* QueryEvent's events of a channel (1 SM exception, 2 SM pause, 3 error notifier, as Ryujinx numbers them), made
       on first query and never signaled: nothing here raises them */
    KEvent* ev[4];
    void* h1x; /* DEV_NVDEC, DEV_VIC: the fd's host1x channel (host1x_open) */
} NvFile;
#define SYNCPT_N 128 /* the syncpoint ids syncpt_alloc hands out (see there) */
/* nv_init sets the two fields that don't start at 0: an initializer would put the whole table in the binary */
static struct {
    NvMapH nvh[8192];
    u32 nvh_next; /* handles below this have been handed out */
    /* freed handles, handed out again before nvh_next grows: movies create and free buffers for every clip, and a
       handle past the table would be written outside it */
    u32 nvh_free[8192];
    u32 nvh_nfree;
    PMutex lock;
    u64** gmmu;    /* gmmu[i] -> 4096 entries of (guest addr | 1) */
    PMutex aslock; /* held across ioctl_as: the mappings below and the VA allocator */
    GMap* gmaps;
    u32 ngmaps, capgmaps;
    u32 big_page_bits; /* 64K unless asked otherwise */
    VaRange* vafree;
    u32 nvafree, capvafree;
    int va_ready; /* the free ranges hold the whole region at first (va_init) */
    u32 syncpt[256];
    u32 syncpt_max[256];
    PCond sync_cv;
    PMutex sync_lk;
    u8 syncpt_used[SYNCPT_N];
    NvEvent nvev[64];
    /* arming (game thread) races with nv_events_check (GPU/video threads after an incr).
       clear the event BEFORE arming and keep both under one lock, otherwise an incr in between
       signals and the clear wipes it -> lost wakeup, game waits forever. A mutex, not a spin: kevent_signal runs under
       it */
    PMutex nvev_lk;
    NvFile nvf[256];
} nv;
u64 nvmap_addr(u32 h) {
    if (h >= 8192) return 0;
    return nv.nvh[h].addr;
}
u32 nvmap_size(u32 h) {
    if (h >= 8192) return 0;
    return nv.nvh[h].size;
}
static u32 nvmap_from_id(u32 id) {
    for (u32 i = 1; i < nv.nvh_next; i++)
        if (nv.nvh[i].alive && nv.nvh[i].id == id) return i;
    return 0;
}
u64 nvmap_addr_by_id(u32 id) {
    u32 h = nvmap_from_id(id);
    if (!h) h = id < 8192 ? id : 0;
    return nvmap_addr(h);
}

/* ============================================================ GPU VA space (40 bit) */
/* two level table, gva -> guest address, 4K granularity. The GPU reads it on the submitting threads while ioctl_as
   changes it on others: an L2 table is installed with a CAS and loaded with acquire, entries are relaxed atomics */
#define GPAGE_BITS 12
#define GL1        (1u << 16) /* 40-12 = 28 bits, split 16 + 12 */
u64 gmmu_translate(u64 gva) {
    u64 p = gva >> GPAGE_BITS;
    u32 i = (u32)(p >> 12) & (GL1 - 1), j = (u32)p & 0xFFF;
    u64* t = __atomic_load_n(&nv.gmmu[i], __ATOMIC_ACQUIRE);
    if (!t) return 0;
    u64 e = __atomic_load_n(&t[j], __ATOMIC_RELAXED);
    if (!(e & 1)) return 0;
    return (e & ~1ull) + (gva & 0xFFF);
}
static u64* gmmu_l2(u32 i) {
    u64* t = __atomic_load_n(&nv.gmmu[i], __ATOMIC_ACQUIRE);
    if (t) return t;
    u64* n = calloc(4096, 8);
    if (!n) fatal("nvdrv: out of memory for the GPU page table\n");
    if (__atomic_compare_exchange_n(&nv.gmmu[i], &t, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return n;
    free(n);
    return t;
}
static void gmmu_map(u64 gva, u64 cpu, u64 size) {
    for (u64 o = 0; o < size; o += 0x1000) {
        u64 p = (gva + o) >> GPAGE_BITS;
        u32 i = (u32)(p >> 12) & (GL1 - 1), j = (u32)p & 0xFFF;
        __atomic_store_n(&gmmu_l2(i)[j], (cpu + o) | 1, __ATOMIC_RELAXED);
    }
}
/* clears the pages of [gva, gva+size). With own set, only the pages still pointing where a mapping of cpu put them:
   unmapping a buffer leaves alone what was mapped over it since */
static void gmmu_unmap(u64 gva, u64 size, int own, u64 cpu) {
    for (u64 o = 0; o < size; o += 0x1000) {
        u64 p = (gva + o) >> GPAGE_BITS;
        u32 i = (u32)(p >> 12) & (GL1 - 1), j = (u32)p & 0xFFF;
        u64* t = __atomic_load_n(&nv.gmmu[i], __ATOMIC_ACQUIRE);
        if (!t) continue;
        if (!own || __atomic_load_n(&t[j], __ATOMIC_RELAXED) == ((cpu + o) | 1))
            __atomic_store_n(&t[j], 0, __ATOMIC_RELAXED);
    }
}
static GMap* gmap_add(void) {
    if (nv.ngmaps == nv.capgmaps) {
        u32 nc = nv.capgmaps ? nv.capgmaps * 2 : 1024;
        GMap* g = realloc(nv.gmaps, nc * sizeof *g);
        if (!g) fatal("nvdrv: out of memory for GPU mappings\n");
        nv.gmaps = g;
        nv.capgmaps = nc;
    }
    return &nv.gmaps[nv.ngmaps++];
}
/* takes [gva, gva+size) out of the tracked pieces, splitting a piece that straddles it */
static void gmap_cut(u64 gva, u64 size) {
    u64 e = gva + size;
    for (u32 k = 0; k < nv.ngmaps;) {
        GMap* g = &nv.gmaps[k];
        u64 ge = g->gva + g->size;
        if (ge <= gva || g->gva >= e) {
            k++;
        } else if (g->gva < gva && ge > e) { /* the cut is inside: keep the head, add the tail */
            GMap tail = *g;
            tail.cpu += e - g->gva;
            tail.gva = e;
            tail.size = ge - e;
            g->size = gva - g->gva;
            *gmap_add() = tail;
            k++;
        } else if (g->gva < gva) {
            g->size = gva - g->gva;
            k++;
        } else if (ge > e) {
            g->cpu += e - g->gva;
            g->size = ge - e;
            g->gva = e;
            k++;
        } else
            nv.gmaps[k] = nv.gmaps[--nv.ngmaps];
    }
}
/* GPU VA for ALLOC_SPACE and MAP_BUFFER_EX: first fit from 0x04_0000_0000 to the end of the 40-bit space the gmmu
   covers, in 64K units. Not bounded to the big page region GET_VA_REGIONS reports: Catherine maps more than its
   0x1BFFF big pages (its NVN init maps 0x20000000 bytes past it) and checks nothing against the regions. Free ranges
   are kept sorted and merged */
#define VA_BASE 0x0400000000ull
#define VA_END  (1ull << 40)
static void va_insert(u32 k, u64 a, u64 e) {
    if (nv.nvafree == nv.capvafree) {
        u32 nc = nv.capvafree ? nv.capvafree * 2 : 64;
        VaRange* v = realloc(nv.vafree, nc * sizeof *v);
        if (!v) fatal("nvdrv: out of memory for the GPU address space\n");
        nv.vafree = v;
        nv.capvafree = nc;
    }
    memmove(&nv.vafree[k + 1], &nv.vafree[k], (nv.nvafree - k) * sizeof *nv.vafree);
    nv.vafree[k].a = a;
    nv.vafree[k].e = e;
    nv.nvafree++;
}
static void va_remove(u32 k) {
    memmove(&nv.vafree[k], &nv.vafree[k + 1], (nv.nvafree - k - 1) * sizeof *nv.vafree);
    nv.nvafree--;
}
static void va_init(void) {
    if (nv.va_ready) return;
    nv.va_ready = 1;
    va_insert(0, VA_BASE, VA_END);
}
static u64 va_round(u64 size) { return (size + 0xFFFF) & ~0xFFFFull; }
/* takes [a, e) out of the free ranges */
static void va_take(u64 a, u64 e) {
    va_init();
    for (u32 k = 0; k < nv.nvafree;) {
        VaRange r = nv.vafree[k];
        if (r.e <= a || r.a >= e) {
            k++;
            continue;
        }
        va_remove(k);
        if (r.a < a) va_insert(k++, r.a, a);
        if (e < r.e) va_insert(k++, e, r.e);
    }
}
static void va_reserve(u64 a, u64 size) { va_take(a, a + va_round(size)); }
/* gives [a, a+size) back; only the part inside the region counts */
static void va_free(u64 a, u64 size) {
    va_init();
    u64 e = a + va_round(size), lo = VA_BASE, hi = VA_END;
    if (a < lo) a = lo;
    if (e > hi) e = hi;
    if (a >= e) return;
    va_take(a, e); /* what is already free isn't added twice */
    u32 k = 0;
    while (k < nv.nvafree && nv.vafree[k].a < a) k++;
    if (k > 0 && nv.vafree[k - 1].e == a) {
        a = nv.vafree[k - 1].a;
        va_remove(--k);
    }
    if (k < nv.nvafree && nv.vafree[k].a == e) {
        e = nv.vafree[k].e;
        va_remove(k);
    }
    va_insert(k, a, e);
}
/* 0 when the region has no room */
static u64 va_alloc(u64 size, u64 align) {
    va_init();
    if (align < 0x10000) align = 0x10000;
    for (u32 k = 0; k < nv.nvafree; k++) {
        u64 a = (nv.vafree[k].a + align - 1) & ~(align - 1);
        if (a < nv.vafree[k].a || a + va_round(size) > nv.vafree[k].e) continue;
        va_reserve(a, size);
        return a;
    }
    WARN("nvdrv: no room left in the GPU address space for %llx bytes\n", (unsigned long long)size);
    return 0;
}

/* ============================================================ syncpoints */
u32 syncpt_read(u32 id) { return __atomic_load_n(&nv.syncpt[id & 255], __ATOMIC_SEQ_CST); }
void syncpt_incr(u32 id) {
    pm_lock(&nv.sync_lk);
    nv.syncpt[id & 255]++;
    pc_broadcast(&nv.sync_cv);
    pm_unlock(&nv.sync_lk);
    nv_events_check();
}
int syncpt_wait(u32 id, u32 thresh, i64 timeout) {
    pm_lock(&nv.sync_lk);
    u64 end = plat_ns() + (u64)(timeout < 0 ? 0 : timeout);
    while ((i32)(nv.syncpt[id & 255] - thresh) < 0) {
        if (timeout == 0) {
            pm_unlock(&nv.sync_lk);
            return 0;
        }
        i64 rem = -1;
        if (timeout > 0) {
            u64 now = plat_ns();
            if (now >= end) {
                pm_unlock(&nv.sync_lk);
                return 0;
            }
            rem = (i64)(end - now);
        }
        pc_wait(&nv.sync_cv, &nv.sync_lk, rem);
    }
    pm_unlock(&nv.sync_lk);
    return 1;
}
/* every syncpoint user (GPU channels here, host1x channels in host1x.c) allocates here, so ids stay unique and low.
   NVIDIA's stream library sizes its per-syncpoint tables at 128 (NvRmChannelNumSyncPoints is a stub that fails), and an
   id past them writes into the game's heap: running out is fatal, and a closed GPU channel gives its id back */
u32 syncpt_alloc(void) {
    pm_lock(&nv.sync_lk);
    for (u32 id = 20; id < SYNCPT_N; id++)
        if (!nv.syncpt_used[id]) {
            nv.syncpt_used[id] = 1;
            nv.syncpt_max[id] = nv.syncpt[id]; /* an id used before starts with nothing promised past its value */
            pm_unlock(&nv.sync_lk);
            return id;
        }
    pm_unlock(&nv.sync_lk);
    fatal("nvdrv: all %u syncpoints are in use (NVIDIA's stream library has room for %u ids)\n", SYNCPT_N - 20,
          SYNCPT_N);
}
void syncpt_free(u32 id) {
    if (id >= SYNCPT_N) return;
    pm_lock(&nv.sync_lk);
    nv.syncpt_used[id] = 0;
    pm_unlock(&nv.sync_lk);
}

static void nvev_lock(void) { pm_lock(&nv.nvev_lk); }
static void nvev_unlock(void) { pm_unlock(&nv.nvev_lk); }
/* the KEvent of an event slot, made on first use. nvdrv keeps a reference of its own, so the game closing its
   handle doesn't free an event a later syncpoint increment signals. Called with the lock held */
static KEvent* nvev_get(u32 slot) {
    if (!nv.nvev[slot].ev) {
        nv.nvev[slot].ev = kevent_new("nvhost event");
        kobj_ref(&nv.nvev[slot].ev->o);
    }
    return nv.nvev[slot].ev;
}
static void nv_events_check(void) {
    nvev_lock();
    for (int i = 0; i < 64; i++)
        if (nv.nvev[i].armed && (i32)(syncpt_read(nv.nvev[i].id) - nv.nvev[i].thresh) >= 0) {
            nv.nvev[i].armed = 0;
            kevent_signal(nv.nvev[i].ev);
        }
    nvev_unlock();
}

/* ============================================================ files (fds) */
enum { DEV_NONE, DEV_NVMAP, DEV_AS, DEV_CTRL, DEV_CTRLGPU, DEV_GPU, DEV_NVDEC, DEV_VIC, DEV_OTHER };

static u32 ioctl_nvmap(u32 cmd, u8* d, u32 n) {
    (void)n;
    switch (cmd & 0xFF) {
        case 0x01: {
            u32 size;
            memcpy(&size, d, 4);
            pm_lock(&nv.lock);
            u32 h = nv.nvh_nfree ? nv.nvh_free[--nv.nvh_nfree] : nv.nvh_next < 8192 ? nv.nvh_next++ : 0;
            if (h) {
                memset(&nv.nvh[h], 0, sizeof nv.nvh[h]);
                nv.nvh[h].size = size;
                nv.nvh[h].id = h;
                nv.nvh[h].refs = 1;
                nv.nvh[h].alive = 1;
            }
            pm_unlock(&nv.lock);
            memcpy(d + 4, &h, 4);
            if (!h) {
                WARN("nvmap: all 8191 handles are in use\n");
                return 6; /* InsufficientMemory */
            }
            return 0;
        }
        case 0x03: { /* FROM_ID: another reference to the same handle */
            u32 id;
            memcpy(&id, d, 4);
            pm_lock(&nv.lock);
            u32 h = nvmap_from_id(id);
            if (h) nv.nvh[h].refs++;
            pm_unlock(&nv.lock);
            if (!h) return 1;
            memcpy(d + 4, &h, 4);
            return 0;
        }
        case 0x04: {
            u32 h;
            memcpy(&h, d, 4);
            if (!h || h >= 8192) return 1;
            NvMapH* m = &nv.nvh[h];
            memcpy(&m->heap, d + 4, 4);
            memcpy(&m->flags, d + 8, 4);
            memcpy(&m->align, d + 12, 4);
            m->kind = d[16];
            memcpy(&m->addr, d + 24, 8);
            LOGV("nvmap alloc h=%u size=%x addr=%llx\n", h, m->size, (unsigned long long)m->addr);
            return 0;
        }
        case 0x05: {
            u32 h;
            memcpy(&h, d, 4);
            if (h && h < 8192) {
                pm_lock(&nv.lock);
                int last = nv.nvh[h].alive && --nv.nvh[h].refs == 0;
                u64 a = nv.nvh[h].addr;
                u32 s = nv.nvh[h].size, f = nv.nvh[h].flags;
                if (last) nv.nvh[h].alive = 0;
                pm_unlock(&nv.lock);
                memcpy(d + 8, &a, 8);
                memcpy(d + 16, &s, 4);
                memcpy(d + 20, &f, 4);
                if (last) {
                    /* NVDEC and VIC keep a mapping per handle: drop it before the number can name another buffer */
                    host1x_forget_handle(h);
                    pm_lock(&nv.lock);
                    nv.nvh_free[nv.nvh_nfree++] = h;
                    pm_unlock(&nv.lock);
                }
            }
            return 0;
        }
        case 0x09: {
            u32 h, p, r = 0;
            memcpy(&h, d, 4);
            memcpy(&p, d + 4, 4);
            if (h >= 8192) return 1;
            NvMapH* m = &nv.nvh[h];
            switch (p) {
                case 1: r = m->size; break;
                case 2: r = m->align ? m->align : 0x1000; break;
                case 3: r = (u32)m->addr; break; /* Base: the address, as Ryujinx answers */
                case 4: r = 0x40000000; break;
                case 5: r = m->kind; break;
                case 6: r = 0; break;
            }
            memcpy(d + 8, &r, 4);
            return 0;
        }
        case 0x0E: {
            u32 h;
            memcpy(&h, d + 4, 4);
            u32 id = h < 8192 ? nv.nvh[h].id : 0;
            memcpy(d, &id, 4);
            return 0;
        }
    }
    WARN("nvmap ioctl %x\n", cmd);
    return 0;
}

static u32 ioctl_as(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    (void)n;
    switch (cmd & 0xFF) {
        case 0x01: return 0;
        case 0x02: {
            u32 pages, psz, flags;
            u64 off;
            memcpy(&pages, d, 4);
            memcpy(&psz, d + 4, 4);
            memcpy(&flags, d + 8, 4);
            memcpy(&off, d + 16, 8);
            u64 size = (u64)pages * psz, a = off;
            if (flags & 1) /* fixed: keep the allocator out of it */
                va_reserve(a, size);
            else if (!(a = va_alloc(size, off > psz ? off : psz)))
                return 6; /* InsufficientMemory */
            memcpy(d + 16, &a, 8);
            LOGV("as alloc_space %llx size %llx\n", (unsigned long long)a, (unsigned long long)size);
            return 0;
        }
        case 0x03: {
            u64 off;
            u32 pages, psz;
            memcpy(&off, d, 8);
            memcpy(&pages, d + 8, 4);
            memcpy(&psz, d + 12, 4);
            u64 size = (u64)pages * psz;
            gmmu_unmap(off, size, 0, 0);
            gmap_cut(off, size);
            va_free(off, size);
            return 0;
        }
        case 0x05: { /* UNMAP_BUFFER: every piece of the mapping at off */
            u64 off, va = 0;
            memcpy(&off, d, 8);
            for (u32 k = 0; k < nv.ngmaps;) {
                if (nv.gmaps[k].base != off) {
                    k++;
                    continue;
                }
                gmmu_unmap(nv.gmaps[k].gva, nv.gmaps[k].size, 1, nv.gmaps[k].cpu);
                if (nv.gmaps[k].va) va = nv.gmaps[k].va;
                nv.gmaps[k] = nv.gmaps[--nv.ngmaps];
            }
            if (va) va_free(off, va);
            return 0;
        }
        case 0x06: {
            u32 flags, kind, h, psz;
            i64 boff;
            u64 msize, off;
            memcpy(&flags, d, 4);
            memcpy(&kind, d + 4, 4);
            memcpy(&h, d + 8, 4);
            memcpy(&psz, d + 12, 4);
            memcpy(&boff, d + 16, 8);
            memcpy(&msize, d + 24, 8);
            memcpy(&off, d + 32, 8);
            if ((flags & 0x100) && h == 0) return 0; /* just updates kind on an existing map */
            if (h >= 8192 || !nv.nvh[h].alive) {
                WARN("as map: invalid nvmap handle %u flags=%x kind=%x psz=%x boff=%llx msize=%llx off=%llx n=%u\n", h,
                     flags, kind, psz, (unsigned long long)boff, (unsigned long long)msize, (unsigned long long)off, n);
                return 1;
            }
            u64 cpu = nv.nvh[h].addr + (u64)boff, size = msize ? msize : nv.nvh[h].size;
            if (flags & 0x100) { /* modify: remap inside an existing mapping, whose unmap takes these pages too */
                u64 base = off, va = 0;
                for (u32 k = 0; k < nv.ngmaps; k++)
                    if (nv.gmaps[k].gva <= off && off < nv.gmaps[k].gva + nv.gmaps[k].size) {
                        base = nv.gmaps[k].base;
                        va = nv.gmaps[k].va;
                        break;
                    }
                gmmu_map(off, cpu, size);
                gmap_cut(off, size);
                GMap* g = gmap_add();
                g->gva = off;
                g->size = size;
                g->cpu = cpu;
                g->base = base;
                g->va = va;
                return 0;
            }
            u64 gva = off, va = 0;
            if (!(flags & 1)) {
                if (!(gva = va_alloc(size, psz ? psz : 0x10000))) return 6; /* InsufficientMemory */
                va = size;
            }
            gmmu_map(gva, cpu, size);
            GMap* g = gmap_add();
            g->gva = gva;
            g->size = size;
            g->cpu = cpu;
            g->base = gva;
            g->va = va;
            memcpy(d + 32, &gva, 8);
            LOGV("as map h=%u cpu=%llx gva=%llx size=%llx\n", h, (unsigned long long)(nv.nvh[h].addr + boff),
                 (unsigned long long)gva, (unsigned long long)size);
            return 0;
        }
        case 0x08: {
            u32 bufsz = 2 * 24;
            memcpy(d + 8, &bufsz, 4);
            u8* rg = d + 16;
            u64 o0 = 0x04000000;
            u32 ps0 = 0x1000;
            u64 np0 = 0x3FFBFFF;
            u64 o1 = 0x0400000000ull;
            u32 ps1 = 1u << nv.big_page_bits;
            u64 np1 = 0x1BFFF;
            memcpy(rg, &o0, 8);
            memcpy(rg + 8, &ps0, 4);
            memset(rg + 12, 0, 4);
            memcpy(rg + 16, &np0, 8);
            memcpy(rg + 24, &o1, 8);
            memcpy(rg + 32, &ps1, 4);
            memset(rg + 36, 0, 4);
            memcpy(rg + 40, &np1, 8);
            if (extra && nextra) memcpy(extra, rg, nextra < 48 ? nextra : 48);
            return 0;
        }
        case 0x09: { /* ALLOC_AS_EX: {u32 flags, i32 as_fd, u32 big_page_size, u32 pad, u64 va_start, va_end, va_split}
                    big_page_size 0 means 128K (yuzu DEFAULT_BIG_PAGE_SIZE). REMAP uses big page units */
            u32 bps;
            memcpy(&bps, d + 8, 4);
            if (!bps) bps = 0x20000;
            if (bps == 0x10000 || bps == 0x20000) nv.big_page_bits = bps == 0x20000 ? 17 : 16;
            LOG("nvdrv: GPU address space init (big page field %x) -> %u KB big pages\n", bps,
                (1u << nv.big_page_bits) >> 10);
            return 0;
        }
        case 0x14: { /* REMAP entries: {u16 flags, u16 kind, u32 handle, u32 mem_off, u32 gpu_off, u32 pages}, offsets in big pages */
            u32 cnt = n / 20;
            for (u32 i = 0; i < cnt; i++) {
                u8* e = d + i * 20;
                u32 h, moff, goff, pages;
                memcpy(&h, e + 4, 4);
                memcpy(&moff, e + 8, 4);
                memcpy(&goff, e + 12, 4);
                memcpy(&pages, e + 16, 4);
                /* big page size comes from ALLOC_AS_EX */
                u64 gva = (u64)goff << nv.big_page_bits, size = (u64)pages << nv.big_page_bits;
                if (!h)
                    gmmu_unmap(gva, size, 0, 0);
                else if (h < 8192)
                    gmmu_map(gva, nv.nvh[h].addr + ((u64)moff << nv.big_page_bits), size);
                LOG_LIMIT(4, "nvdrv: remap handle %u: %llx bytes at gpu %llx (big page %u KB)\n", h,
                          (unsigned long long)size, (unsigned long long)gva, (1u << nv.big_page_bits) >> 10);
            }
            return 0;
        }
    }
    (void)extra;
    (void)nextra;
    WARN("as ioctl %x\n", cmd);
    return 0;
}

static u32 ioctl_ctrl(u32 cmd, u8* d, u32 n) {
    (void)n;
    switch (cmd & 0xFF) {
        case 0x14: {
            u32 id;
            memcpy(&id, d, 4);
            u32 v = syncpt_read(id);
            memcpy(d + 4, &v, 4);
            return 0;
        }
        case 0x15: {
            u32 id;
            memcpy(&id, d, 4);
            syncpt_incr(id);
            return 0;
        }
        case 0x16:
        case 0x19: {
            u32 id, th;
            i32 to;
            memcpy(&id, d, 4);
            memcpy(&th, d + 4, 4);
            memcpy(&to, d + 8, 4);
            int ok = syncpt_wait(id, th, to < 0 ? -1 : (i64)to * 1000000);
            if (n >= 16) {
                u32 v = syncpt_read(id);
                memcpy(d + 12, &v, 4);
            }
            return ok ? 0 : 5;
        }
        case 0x1A: {
            u32 id;
            memcpy(&id, d, 4);
            u32 v = nv.syncpt_max[id & 255];
            memcpy(d + 4, &v, 4);
            return 0;
        }
        case 0x1B: { /* GET_CONFIG {char domain[0x41], param[0x41], value[0x101]}: no value */
            memset(d + 0x82, 0, 0x101);
            return 0;
        }
        case 0x1C: { /* EVENT_SIGNAL: cancel the pending wait and wake the waiter. NVN does this before reusing an event,
                    ignoring it leaves someone blocked forever */
            u32 ev;
            memcpy(&ev, d, 4);
            u32 slot = ev & 63;
            nvev_lock();
            if (nv.nvev[slot].ev) {
                nv.nvev[slot].armed = 0;
                kevent_signal(nv.nvev[slot].ev);
            }
            nvev_unlock();
            LOG_LIMIT(2, "nvdrv: event %u signaled by the game\n", slot);
            return 0;
        }
        case 0x1D:
        case 0x1E: {
            u32 id, th;
            i32 to;
            u32 ev;
            memcpy(&id, d, 4);
            memcpy(&th, d + 4, 4);
            memcpy(&to, d + 8, 4);
            memcpy(&ev, d + 12, 4);
            if (syncpt_wait(id, th, 0)) {
                u32 v = syncpt_read(id);
                memcpy(d + 12, &v, 4);
                return 0;
            }
            if ((cmd & 0xFF) == 0x1D && to != 0) {
                int ok = syncpt_wait(id, th, to < 0 ? -1 : (i64)to * 1000000);
                if (ok) {
                    u32 v = syncpt_read(id);
                    memcpy(d + 12, &v, 4);
                    return 0;
                }
            }
            u32 slot = ev & 63;
            nvev_lock();
            int have = nv.nvev[slot].ev != NULL;
            if (have) {
                kevent_clear(nv.nvev[slot].ev);
                nv.nvev[slot].id = id;
                nv.nvev[slot].thresh = th;
                nv.nvev[slot].armed = 1;
            }
            nvev_unlock();
            if (have) nv_events_check();
            u32 r = 0x10000000u | (slot) | (id << 16);
            memcpy(d + 12, &r, 4);
            return 5;
        } /* timeout, caller will wait on the event */
        case 0x1F: {
            u32 ev;
            memcpy(&ev, d, 4);
            nvev_lock();
            nvev_get(ev & 63);
            nvev_unlock();
            return 0;
        }
        case 0x20:
        case 0x21: return 0;
    }
    WARN("ctrl ioctl %x\n", cmd);
    return 0;
}

static u32 ioctl_ctrlgpu(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    (void)n;
    switch (cmd & 0xFF) {
        case 0x01: {
            u32 v = 0x28000;
            memcpy(d, &v, 4);
            return 0;
        }
        case 0x02: {
            u32 z[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
            memcpy(d, z, 40);
            return 0;
        }
        case 0x03:
        case 0x04: return 0;
        case 0x05: {
            u8 ch[0xA0];
            memset(ch, 0, sizeof ch);
            u32* w = (u32*)ch;
            w[0] = 0x120;
            w[1] = 0xB;
            w[2] = 0xA1;
            w[3] = 1;
            u64 l2 = 0x40000;
            memcpy(ch + 0x10, &l2, 8);
            w[8] = 2;
            w[9] = 0x20;
            w[10] = 0x20000;
            w[11] = 0x20000;
            w[12] = 0x1B;
            w[13] = 0x30000;
            w[14] = 1;
            w[15] = 0x503;
            w[16] = 0x503;
            w[17] = 0x80;
            w[18] = 0x28;
            u64 fl = 0x55;
            memcpy(ch + 0x50, &fl, 8);
            w[22] = 0x902D;
            w[23] = 0xB197;
            w[24] = 0xB1C0;
            w[25] = 0xB06F;
            w[26] = 0xA140;
            w[27] = 0xB0B5;
            w[28] = 1;
            w[29] = 0;
            w[30] = 2;
            w[31] = 1;
            w[32] = 0;
            w[33] = 1;
            w[34] = 0x21D70;
            w[35] = 0;
            u64 cn = 0x6230326D67ull;
            memcpy(ch + 0x90, &cn, 8);
            u64 sz = 0xA0;
            memcpy(d, &sz, 8); /* d + 8, the game's buffer address, goes back as it came */
            memcpy(d + 16, ch, 0xA0);
            if (extra && nextra)
                memcpy(extra, ch, nextra < 0xA0 ? nextra : 0xA0); /* Ioctl3 passes data in the extra buffer */
            return 0;
        }
        case 0x06: {
            u32 m = 3;
            memcpy(d + 16, &m, 4);
            if (extra && nextra >= 4) memcpy(extra, &m, 4);
            return 0;
        }
        case 0x13: { /* NUM_VSMS: GM20B = 2 SMs (1 GPC x 2 TPC). Ioctl3 variant also returns {gpc,tpc} per SM */
            u32 z[2] = {2, 0};
            memcpy(d, z, 8);
            if (extra && nextra) {
                u8 mp[4] = {0, 0, 0, 1};
                memcpy(extra, mp, nextra < 4 ? nextra : 4);
            }
            return 0;
        }
        case 0x14:
            if ((cmd >> 30) == 3) { /* VSMS_MAPPING (0xC0044714): the 4 bytes in place are {gpc,tpc} per SM */
                u8 mp[4] = {0, 0, 0, 1};
                memcpy(d, mp, 4);
                if (extra && nextra) memcpy(extra, mp, nextra < 4 ? nextra : 4);
                return 0;
            }
            {
                u32 z[2] = {7, 1};
                memcpy(d, z, 8);
                return 0;
            } /* GET_ACTIVE_SLOT_MASK */
        case 0x15: {
            u32 z[2] = {7, 1};
            memcpy(d, z, 8);
            return 0;
        }
        case 0x1C: {
            u64 t = guest_ns();
            memcpy(d, &t, 8);
            return 0;
        } /* GET_GPU_TIME, same timebase as the system tick */
    }
    WARN("ctrl-gpu ioctl %x\n", cmd);
    return 0;
}

static u32 ioctl_gpu(NvFile* f, u32 cmd, u8* d, u32 n, u8* extra, u32 nextra) {
    switch (cmd & 0xFF) {
        case 0x01:
        case 0x03:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x1D:
        case 0x0A:
        case 0x17: return 0;
        case 0x14: memcpy(&f->user_data, d, 8); return 0;
        case 0x15: memcpy(d, &f->user_data, 8); return 0;
        case 0x16: memset(d, 0, n); return 0;
        case 0x09: {
            u32 cls;
            memcpy(&cls, d, 4);
            if (f->nclass < 8) f->class_ids[f->nclass++] = cls;
            u64 id = cls;
            memcpy(d + 8, &id, 8);
            return 0;
        }
        case 0x1A: { /* ALLOC_GPFIFO_EX2 {u32 num_entries, num_jobs, flags, NvFence fence, u32 reserved[3]} */
            if (!f->syncpt) f->syncpt = syncpt_alloc();
            u32 fence[2] = {f->syncpt, syncpt_read(f->syncpt)};
            memcpy(d + 12, fence, 8);
            return 0;
        }
        case 0x08:
        case 0x1B: {
            u32 num, flags;
            memcpy(&num, d + 8, 4);
            memcpy(&flags, d + 12, 4);
            u32 fid, fval;
            memcpy(&fid, d + 16, 4);
            memcpy(&fval, d + 20, 4);
            u64* entries;
            if ((cmd & 0xFF) == 0x08) { /* the entries follow the header in the ioctl buffer */
                entries = (u64*)(d + 24);
                if (n < 24)
                    num = 0;
                else if ((u64)num * 8 > n - 24)
                    num = (n - 24) / 8;
            } else {
                entries = (u64*)extra;
                if ((u64)num * 8 > nextra) num = nextra / 8;
            }
            if (!f->syncpt) f->syncpt = syncpt_alloc();
            if ((flags & 1) && !syncpt_wait(fid, fval, 2000000000))
                WARN("nvdrv: a submission waited 2 s for syncpoint %u to reach %u (it is at %u), and runs anyway\n",
                     fid, fval, syncpt_read(fid));
            /* the fence promises every increment of this submission. With IncrementWithValue (0x100) the stream
               increments the syncpoint fval times itself; the host adds 2 after it only for FenceIncrement (2), as
               Ryujinx's SubmitGpfifo does */
            u32 host = (flags & 2) ? 2 : 0;
            u32 incs = host + ((flags & 0x100) ? fval : 0);
            pm_lock(&nv.sync_lk);
            if ((i32)(nv.syncpt_max[f->syncpt] - nv.syncpt[f->syncpt]) < 0)
                nv.syncpt_max[f->syncpt] = nv.syncpt[f->syncpt];
            nv.syncpt_max[f->syncpt] += incs;
            u32 promised = nv.syncpt_max[f->syncpt];
            pm_unlock(&nv.sync_lk);
            u32 before = syncpt_read(f->syncpt);
            gpu_submit(entries, num, f->syncpt, host);
            {
                /* the submission ran before gpu_submit returned, so the syncpoint should now be at the fence: the log
                   shows whether the stream made the increments the accounting above expects of it */
                static u32 said;
                u32 after = syncpt_read(f->syncpt);
                if (said < 64) {
                    said++;
                    LOG("nvdrv: submit on syncpoint %u: flags %x value %u, %u entries, syncpoint %u -> %u, fence %u\n",
                        f->syncpt, flags, fval, num, before, after, promised);
                } else
                    LOGV("nvdrv: submit on syncpoint %u: flags %x value %u, %u entries, syncpoint %u -> %u, fence %u\n",
                         f->syncpt, flags, fval, num, before, after, promised);
                if (after != promised)
                    WARN_LIMIT(
                        16, "nvdrv: syncpoint %u is at %u after a submission (flags %x value %u), its fence says %u\n",
                        f->syncpt, after, flags, fval, promised);
            }
            fid = f->syncpt;
            fval = promised;
            memcpy(d + 16, &fid, 4);
            memcpy(d + 20, &fval, 4);
            return 0;
        }
    }
    WARN("gpu channel ioctl %x\n", cmd);
    return 0;
}

static int nvdrv_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: {
            u64 a, s;
            buf_in(c, 0, &a, &s);
            char name[64] = {0};
            memcpy(name, GP(a), s < 63 ? s : 63);
            int dev = !strcmp(name, "/dev/nvmap")             ? DEV_NVMAP
                      : !strcmp(name, "/dev/nvhost-as-gpu")   ? DEV_AS
                      : !strcmp(name, "/dev/nvhost-ctrl")     ? DEV_CTRL
                      : !strcmp(name, "/dev/nvhost-ctrl-gpu") ? DEV_CTRLGPU
                      : !strcmp(name, "/dev/nvhost-gpu")      ? DEV_GPU
                      : !strcmp(name, "/dev/nvhost-nvdec")    ? DEV_NVDEC
                      : !strcmp(name, "/dev/nvhost-vic")      ? DEV_VIC
                                                              : DEV_OTHER;
            /* an NVDEC or VIC fd is a host1x channel of its own, with its registers, syncpoints and decoder */
            int h1x_dev = dev == DEV_NVDEC || dev == DEV_VIC;
            void* h1x = h1x_dev ? host1x_open(dev == DEV_VIC) : NULL;
            int fd = -1;
            pm_lock(&nv.lock);
            for (int i = 1; i < 256 && (h1x || !h1x_dev); i++)
                if (!nv.nvf[i].dev) {
                    fd = i;
                    memset(&nv.nvf[i], 0, sizeof nv.nvf[i]);
                    nv.nvf[i].dev = dev;
                    nv.nvf[i].h1x = h1x;
                    break;
                }
            pm_unlock(&nv.lock);
            if (fd < 0) host1x_close(h1x);
            LOG("nvdrv: open %s -> %d\n", name, fd);
            out32(c, (u32)fd);
            out32(c, fd < 0 ? 0x30003u : 0); /* FileOperationFailed when every fd is taken (or no memory) */
            return 0;
        }
        case 1:
        case 11:
        case 12: {
            u32 fd = in32(c, 0), cmd = in32(c, 4);
            u64 ia = 0, is = 0, oa = 0, os = 0, ea = 0, es = 0;
            buf_in(c, 0, &ia, &is);
            buf_out(c, 0, &oa, &os);
            if (c->cmd == 11) { buf_in(c, 1, &ea, &es); }
            if (c->cmd == 12) { buf_out(c, 1, &ea, &es); }
            u32 size = (cmd >> 16) & 0x3FFF;
            u8 tmp[0x4000];
            memset(tmp, 0, sizeof tmp);
            u64 isz = is, osz = os;
            if (ia) memcpy(tmp, GP(ia), isz > sizeof tmp ? sizeof tmp : isz);
            /* nb is what the command's size field (or the larger buffer) says, n what the device may touch: both stay
               inside tmp */
            u64 nb64 = size ? size : (isz > osz ? isz : osz);
            u32 nb = (u32)(nb64 > sizeof tmp ? sizeof tmp : nb64);
            u32 n = (u32)(isz > nb ? (isz > sizeof tmp ? sizeof tmp : isz) : nb);
            if (fd >= 256 || !nv.nvf[fd].dev) {
                out32(c, 8);
                return 0;
            }
            NvFile* f = &nv.nvf[fd];
            u32 r = 0;
            {
                static u32 seen[256];
                static int nseen;
                int k;
                for (k = 0; k < nseen; k++)
                    if (seen[k] == (cmd ^ ((u32)f->dev << 28))) break;
                if (k == nseen && nseen < 256) {
                    seen[nseen++] = cmd ^ ((u32)f->dev << 28);
                    LOG("nvdrv: ioctl %08x (disp %d, ipc %u, in %llu out %llu extra %llu)\n", cmd, f->dev, c->cmd,
                        (unsigned long long)is, (unsigned long long)os, (unsigned long long)es);
                }
            }
            switch (f->dev) {
                case DEV_NVMAP: r = ioctl_nvmap(cmd, tmp, nb); break;
                case DEV_AS:
                    pm_lock(&nv.aslock);
                    r = ioctl_as(cmd, tmp, n, (c->cmd == 12 && ea) ? GP(ea) : NULL, (u32)es);
                    pm_unlock(&nv.aslock);
                    break;
                case DEV_CTRL: r = ioctl_ctrl(cmd, tmp, nb); break;
                case DEV_CTRLGPU: r = ioctl_ctrlgpu(cmd, tmp, nb, (c->cmd == 12 && ea) ? GP(ea) : NULL, (u32)es); break;
                case DEV_GPU: r = ioctl_gpu(f, cmd, tmp, n, ea ? GP(ea) : NULL, (u32)es); break;
                case DEV_NVDEC:
                case DEV_VIC: r = host1x_ioctl(f->h1x, cmd, tmp, n); break;
                default: WARN("nvdrv: ioctl %x to unknown device\n", cmd); break;
            }
            if (oa) memcpy(GP(oa), tmp, osz > sizeof tmp ? sizeof tmp : osz);
            out32(c, r);
            return 0;
        }
        case 2: {
            u32 fd = in32(c, 0);
            NvFile old;
            memset(&old, 0, sizeof old);
            pm_lock(&nv.lock);
            if (fd < 256 && nv.nvf[fd].dev) {
                old = nv.nvf[fd];
                memset(&nv.nvf[fd], 0, sizeof nv.nvf[fd]);
            }
            pm_unlock(&nv.lock);
            if (old.dev == DEV_GPU && old.syncpt) { /* the channel and its syncpoint go with the fd */
                gpu_channel_close(old.syncpt);
                syncpt_free(old.syncpt);
            }
            host1x_close(old.h1x); /* NVDEC, VIC: the channel, its decoder and syncpoints go with the fd */
            for (int k = 0; k < 4; k++)
                if (old.ev[k]) kobj_unref(&old.ev[k]->o);
            out32(c, 0);
            return 0;
        }
        case 3: out32(c, 0); return 0;
        case 4: { /* QueryEvent: the ctrl fd's ids name event slots, a channel's ids its own events */
            u32 fd = in32(c, 0), id = in32(c, 4);
            KEvent* e = NULL;
            pm_lock(&nv.lock);
            int dev = fd < 256 ? nv.nvf[fd].dev : DEV_NONE;
            if (dev != DEV_NONE && dev != DEV_CTRL && id >= 1 && id <= 3) {
                if (!nv.nvf[fd].ev[id]) {
                    nv.nvf[fd].ev[id] = kevent_new("nv channel event");
                    kobj_ref(&nv.nvf[fd].ev[id]->o); /* the fd's reference, dropped on close */
                }
                e = nv.nvf[fd].ev[id];
            }
            pm_unlock(&nv.lock);
            if (!e && (dev == DEV_NONE || dev == DEV_CTRL) && (((id >> 28) & 0xF) == 1 || id < 64)) {
                nvev_lock();
                e = nvev_get(id & 63);
                nvev_unlock();
            }
            if (!e) e = kevent_new("nv query");
            LOG_LIMIT(8, "nvdrv: QueryEvent fd %u (device %d) id %x\n", fd, dev, id);
            out_copy_h(c, h_new(&e->o));
            out32(c, 0);
            return 0;
        }
        case 5:
        case 6:
        case 8:
        case 9:
        case 10:
        case 13: out32(c, 0); return 0;
    }
    return stub_ok(o, c);
}
static SvcObj* new_nvdrv(void) { return svcobj_new("nvdrv", nvdrv_disp, 0); }

void nv_init(void) {
    nv.nvh_next = 1;
    nv.big_page_bits = 16;
    pm_init(&nv.lock);
    pm_init(&nv.aslock);
    pm_init(&nv.nvev_lk);
    pm_init(&nv.sync_lk);
    pc_init(&nv.sync_cv);
    nv.gmmu = calloc(GL1, sizeof(u64*));
    svc_register("nvdrv", new_nvdrv);
    svc_register("nvdrv:a", new_nvdrv);
    svc_register("nvdrv:s", new_nvdrv);
    svc_register("nvdrv:t", new_nvdrv);
}
