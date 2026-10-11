/* Guest memory ranges as buffers the GPU reads: vertices, indices and storage buffers. A range is checked once per
   guest submit (and again after the GPU front end wrote it). Data that changed is copied into the frame's staging
   ring, which draws read directly; data that stayed the same over a few checks gets a buffer of its own, written
   once and never changed (a change makes a new one), so nothing waits for the GPU. Big ranges that have been stable
   for long are checked less often, like gpu_gl.c's buf_cached. */
#include "vk.h"

typedef struct Vb {
    u64 start, len, hash, shash;
    u32 epoch, chk_epoch, schk, stable, used;
    u64 gw;    /* GPU-side write log position at the last check */
    u64 frame; /* the staging frame buf/off point into, when not own */
    int own;   /* buf is this entry's own buffer */
    u32 wseen; /* write-tracking clock when it last matched memory */
    VkBuffer buf;
    VmaAllocation mem;
    u64 off;
    struct Vb* next;
} Vb;
#define VBH 4096
static Vb* g_vbh[VBH];
extern u64 g_gw_gen;
extern int gpu_mem_dirty_since(u64 since, u64 a, u64 sz);

/* written-page tracking (plat_ww_scan): a generation per 4K page of guest memory, bumped when a scan finds it
   written. A cache entry remembers the clock when it last matched memory, and is stale only if a page in its range
   has a newer generation. Scanning re-protects what it reports, and the shared table keeps that from hiding a change
   from another entry over the same memory. The table has a leaf of 1024 pages per 4 MB that has been written, with
   the newest generation in the leaf beside it so whole leaves are skipped */
#define WW_LEAF_SHIFT 22
static u32* g_wleaf[AS_SIZE >> WW_LEAF_SHIFT];
static u32 g_wleaf_max[AS_SIZE >> WW_LEAF_SHIFT];
static u32 g_wclock = 1;
static int g_ww = -1; /* -1 not tried yet, 0 not available, 1 on */
static void ww_mark(u8* p, u64 n) {
    u64 o = (u64)(p - g_mem);
    if (o >= AS_SIZE || !n) return;
    for (u64 pg = o >> 12, e = (o + n - 1) >> 12; pg <= e && pg < (AS_SIZE >> 12); pg++) {
        u64 l = pg >> (WW_LEAF_SHIFT - 12);
        if (!g_wleaf[l] && !(g_wleaf[l] = calloc(1u << (WW_LEAF_SHIFT - 12), 4))) continue;
        g_wleaf[l][pg & ((1u << (WW_LEAF_SHIFT - 12)) - 1)] = g_wclock;
        g_wleaf_max[l] = g_wclock;
    }
}
int vk_written_since(const u8* host, u64 len, u32 since) {
    if (!g_ww || !host || !len) return -1;
    if (g_ww < 0) {
        g_ww = plat_ww_scan(g_mem, 4096, ww_mark) == 0;
        LOG("vulkan: %s\n",
            g_ww ? "write tracking on (caches skip unchanged memory)" : "no write tracking (caches check contents)");
        if (!g_ww) return -1;
    }
    g_wclock++;
    if (plat_ww_scan((u8*)host, len, ww_mark) < 0) {
        g_ww = 0;
        LOG("vulkan: write tracking failed, caches check contents\n");
        return -1;
    }
    u64 o = (u64)(host - g_mem);
    for (u64 pg = o >> 12, e = (o + len - 1) >> 12; pg <= e;) {
        u64 l = pg >> (WW_LEAF_SHIFT - 12), lend = (l + 1) << (WW_LEAF_SHIFT - 12);
        if (g_wleaf[l] && g_wleaf_max[l] > since)
            for (u64 q = pg; q <= e && q < lend; q++)
                if (g_wleaf[l][q & ((1u << (WW_LEAF_SHIFT - 12)) - 1)] > since) return 1;
        pg = lend;
    }
    return 0;
}
u32 vk_ww_now(void) { return g_wclock; }

static u8* g_tmp;
static u64 g_tmp_cap;
/* the guest bytes, without a copy when they are contiguous in host memory */
static const u8* guest_bytes(u64 gva, u64 len) {
    u8* p = gmem_ptr(gva, len);
    if (p) return p;
    if (g_tmp_cap < len) {
        u8* n = realloc(g_tmp, len);
        if (!n) return NULL;
        g_tmp = n;
        g_tmp_cap = len;
    }
    gmem_read(gva, g_tmp, len);
    return g_tmp;
}

static u64 sample_hash(u64 gva, u64 len) {
    u8 tmp[2048 + 128];
    if (len <= sizeof tmp) {
        gmem_read(gva, tmp, len);
        return vk_hash(tmp, len);
    }
    u32 n = 0;
    gmem_read(gva, tmp, 64);
    n = 64;
    gmem_read(gva + len - 64, tmp + n, 64);
    n += 64;
    u64 step = (len - 32) / 62;
    for (int i = 0; i < 62; i++) {
        gmem_read(gva + (u64)i * step, tmp + n, 32);
        n += 32;
    }
    return vk_hash(tmp, n);
}

int vk_buf_copy(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    u8* p = vk_stage(len ? len : 16, vk_stage_align(), buf, off);
    if (!p) return 0;
    if (!gva)
        memset(p, 0, len);
    else {
        const u8* src = guest_bytes(gva, len);
        if (!src) return 0;
        memcpy(p, src, len);
    }
    return 1;
}

/* the entry's data where draws read it */
static void use(Vb* e, VkBuffer* buf, u64* off) {
    e->used = g_vk_present;
    *buf = e->buf;
    *off = e->off;
}
static void drop_own(Vb* e) {
    if (e->own) vk_defer_buffer(e->buf, e->mem);
    e->own = 0;
    e->buf = VK_NULL_HANDLE;
}
static int make_own(Vb* e, const u8* src) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = e->len,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo info;
    VkBuffer b;
    VmaAllocation m;
    if (vmaCreateBuffer(g_vk.vma, &bci, &aci, &b, &m, &info) != VK_SUCCESS) return 0;
    memcpy(info.pMappedData, src, e->len);
    vmaFlushAllocation(g_vk.vma, m, 0, VK_WHOLE_SIZE);
    drop_own(e);
    e->own = 1;
    e->buf = b;
    e->mem = m;
    e->off = 0;
    return 1;
}

int vk_buf_get(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (!len) return 0;
    if (vk_compute_gpu_src(gva, len, buf, off)) return 1; /* a compute result that hasn't landed yet */
    u32 k = (u32)((gva >> 6) ^ (gva >> 20) ^ len) & (VBH - 1);
    Vb* e = g_vbh[k];
    for (; e; e = e->next)
        if (e->start == gva && e->len == len) break;
    /* valid: the data is where draws can read it this frame */
    int valid = e && e->buf && (e->own || e->frame == vk_frame_number());
    if (valid && !gpu_mem_dirty_since(e->gw, gva, len)) {
        if (e->epoch == g_vk_epoch) {
            use(e, buf, off);
            return 1;
        }
        /* with write tracking, an own buffer is checked only when its memory was written */
        int w = e->own ? vk_written_since(gmem_ptr(gva, len), len, e->wseen) : -1;
        if (w == 0) {
            e->epoch = g_vk_epoch;
            use(e, buf, off);
            return 1;
        }
        /* without it: own buffers that stayed the same for long are checked every few submits, big ones sampled in
           between */
        if (w < 0 && e->own && e->stable >= 16 && len >= (64u << 10) && g_vk_epoch - e->chk_epoch < 8) {
            e->epoch = g_vk_epoch;
            use(e, buf, off);
            return 1;
        }
        if (w < 0 && e->own && e->stable >= 32 && len < (64u << 10) && g_vk_epoch - e->chk_epoch < 4) {
            e->epoch = g_vk_epoch;
            use(e, buf, off);
            return 1;
        }
        if (w < 0 && e->own && e->stable >= 64 && len >= 4096 &&
            g_vk_epoch - e->chk_epoch < (e->stable >= 256 ? 240u : 64u)) {
            if (g_vk_epoch - e->schk < 4 || sample_hash(gva, len) == e->shash) {
                if (g_vk_epoch - e->schk >= 4) e->schk = g_vk_epoch;
                e->epoch = g_vk_epoch;
                use(e, buf, off);
                return 1;
            }
        }
    }
    u32 wnow = vk_ww_now();
    const u8* src = guest_bytes(gva, len);
    if (!src) return 0;
    u64 h = vk_hash(src, len);
    if (!e) {
        e = calloc(1, sizeof *e);
        e->start = gva;
        e->len = len;
        e->hash = h ^ 1;
        e->next = g_vbh[k];
        g_vbh[k] = e;
    }
    if (e->hash == h && e->stable < 100000)
        e->stable++;
    else if (e->hash != h) {
        e->hash = h;
        e->stable = 0;
        drop_own(e);
    }
    e->epoch = e->chk_epoch = g_vk_epoch;
    e->gw = g_gw_gen;
    e->wseen = wnow;
    if (e->stable >= 63 && len >= 4096) {
        e->shash = sample_hash(gva, len);
        e->schk = g_vk_epoch;
    }
    if (!e->own && !(e->stable >= 2 && make_own(e, src))) {
        /* changing (or new) data: this frame's staging ring */
        u8* p = vk_stage(len, vk_stage_align(), &e->buf, &e->off);
        if (!p) return 0;
        memcpy(p, src, len);
        e->frame = vk_frame_number();
    }
    use(e, buf, off);
    return 1;
}

/* buffers not read for ten seconds go */
void vk_buf_frame(void) {
    if (g_vk_present % 60 || g_vk_present < 600) return;
    for (int k = 0; k < VBH; k++)
        for (Vb** pp = &g_vbh[k]; *pp;) {
            Vb* e = *pp;
            if ((i32)(e->used - (g_vk_present - 600)) >= 0) {
                pp = &e->next;
                continue;
            }
            *pp = e->next;
            drop_own(e);
            free(e);
        }
}
