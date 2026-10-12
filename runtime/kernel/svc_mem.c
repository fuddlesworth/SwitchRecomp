/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the memory SVCs (heap, QueryMemory, MapMemory, physical, shared and transfer memory) and GetInfo */
#include "kernel/kernel_int.h"
#include "core/clock.h"
/* ============================================================ memory */
u32 svc_query(u64 info, u64 addr) {
    MemBlock b;
    mem_query(addr, &b);
    gw64(info, b.addr);
    gw64(info + 8, b.size);
    gw32(info + 16, b.state & 0xFF);
    gw32(info + 20, b.attr);
    gw32(info + 24, b.perm & 7);
    gw32(info + 28, b.ipc);
    gw32(info + 32, b.dev);
    gw32(info + 36, 0);
    return 0;
}
/* the console's argument checks: InvalidAddress for an unaligned address, InvalidSize for a size of 0 or one that
   isn't whole pages, InvalidCurrentMemory when the range wraps */
static u32 check_range(u64 a, u64 s) {
    if (a & 0xFFF) return R_BADADDR;
    if (!s || (s & 0xFFF)) return R_BADSIZE;
    if (a + s < a) return R_BADMEMSTATE;
    return 0;
}
/* every block of [a, a+s) is mapped, in state st (any state when ~0), with (attr & mask) == want */
static int range_is(u64 a, u64 s, u32 st, u32 mask, u32 want) {
    for (u64 p = a; p < a + s;) {
        MemBlock b;
        mem_query(p, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) return 0;
        if ((st != 0xFFFFFFFFu && b.state != st) || (b.attr & mask) != want) return 0;
        p = b.addr + b.size;
    }
    return 1;
}
u32 svc_map_memory(u64 dst, u64 src, u64 size) {
    u32 r = check_range(dst, size);
    if (!r) r = check_range(src, size);
    if (r) return r;
    int in_stack = dst >= STACK_BASE && dst + size <= STACK_BASE + STACK_SIZE;
    int in_alias = dst >= ALIAS_BASE && dst + size <= ALIAS_BASE + ALIAS_SIZE;
    if (!in_stack && !in_alias) return R_BADREGION;
    int m = mem_map(dst, size, in_stack ? MS_STACK : MS_NORMAL, PERM_RW);
    if (m) return m == -2 ? R_OUTOFMEM : R_BADMEMSTATE;
    memcpy(GP(dst), GP(src), size);
    mem_set_attr(src, size, 1, 1); /* locked */
    mem_set_state(src, size, 0xFFFFFFFFu, PERM_NONE);
    return 0;
}
u32 svc_unmap_memory(u64 dst, u64 src, u64 size) {
    u32 r = check_range(dst, size);
    if (!r) r = check_range(src, size);
    if (r) return r;
    int in_stack = dst >= STACK_BASE && dst + size <= STACK_BASE + STACK_SIZE;
    int in_alias = dst >= ALIAS_BASE && dst + size <= ALIAS_BASE + ALIAS_SIZE;
    if (!in_stack && !in_alias) return R_BADREGION;
    /* only what MapMemory mapped: the alias in the state it got, the source still locked */
    if (!range_is(dst, size, in_stack ? MS_STACK : MS_NORMAL, 0, 0) || !range_is(src, size, 0xFFFFFFFFu, 1, 1))
        return R_BADMEMSTATE;
    memcpy(GP(src), GP(dst), size);
    mem_unmap(dst, size);
    mem_set_attr(src, size, 1, 0);
    mem_set_state(src, size, 0xFFFFFFFFu, PERM_RW);
    return 0;
}
/* svcSetHeapSize: 2 MB steps inside the heap region, and no further than the memory the resource limit gives the
   process. The SDK sizes its heap from GetInfo's total and used memory, so it gets what memory_gb sets */
u32 svc_set_heap_size(u64 size) {
    u32 r = 0;
    if (size & 0x1FFFFF)
        r = R_BADSIZE;
    else if (size > HEAP_SIZE || (g_heap_max && size > g_heap_max))
        r = R_OUTOFMEM;
    else if (size > g_heap_size && size - g_heap_size + used_memory() > mem_total())
        r = R_LIMIT;
    else if (!mem_heap_set(size))
        r = R_OUTOFMEM;
    /* the SDK sizes its heap from GetInfo and retries smaller on failure: the log shows which size it got */
    LOG("heap: %llu MB requested%s\n", (unsigned long long)(size >> 20), r ? ", refused" : "");
    return r;
}
/* svcMapPhysicalMemory: backs the holes of a range of the alias region with memory, which counts as used */
u32 svc_map_physical(u64 a, u64 s) {
    u32 r = check_range(a, s);
    if (r) return r;
    if (a < ALIAS_BASE || a + s > ALIAS_BASE + ALIAS_SIZE) return R_BADREGION;
    u64 n = mem_map_holes(a, s, MS_NORMAL, PERM_RW);
    if (n == ~0ull) return R_OUTOFMEM;
    __atomic_add_fetch(&g_phys_mapped, n, __ATOMIC_SEQ_CST);
    return 0;
}
u32 svc_unmap_physical(u64 a, u64 s) {
    u32 r = check_range(a, s);
    if (r) return r;
    if (a < ALIAS_BASE || a + s > ALIAS_BASE + ALIAS_SIZE) return R_BADREGION;
    u64 n = 0;
    for (u64 p = a; p < a + s;) {
        MemBlock b;
        mem_query(p, &b);
        u64 e = b.addr + b.size;
        if (e > a + s) e = a + s;
        if (b.state == MS_NORMAL) n += e - p;
        p = e;
    }
    mem_unmap(a, s);
    u64 cur = __atomic_load_n(&g_phys_mapped, __ATOMIC_SEQ_CST);
    while (!__atomic_compare_exchange_n(&g_phys_mapped, &cur, cur > n ? cur - n : 0, 0, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {}
    return 0;
}
static void shm_destroy(KObj* o) {
    KShmem* s = (KShmem*)o;
    free(s->host); /* a mapping the game still has keeps its own copy in guest memory */
    free(o->waiters);
    free(s);
}
KShmem* kshmem_new(u64 size, const char* name) {
    KShmem* s = calloc(1, sizeof *s);
    s->o.type = KO_SHMEM;
    s->o.destroy = shm_destroy;
    s->o.name = name;
    s->size = (size + 0xFFF) & ~0xFFFull;
    s->host = calloc(1, s->size);
    return s;
}
/* the services write through this: the guest mapping once the game mapped it, the host copy before and after */
u8* kshmem_ptr(KShmem* s) {
    u64 g = __atomic_load_n(&s->gaddr, __ATOMIC_ACQUIRE);
    return g ? GP(g) : s->host;
}
u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm) {
    u32 r = check_range(addr, size);
    if (r) return r;
    KShmem* s = (KShmem*)h_get_ref(h, KO_SHMEM);
    if (!s) return R_BADHANDLE;
    if (size != s->size) { /* the console answers InvalidSize: keep mapping at least the whole object, but say so */
        WARN_LIMIT(1, "MapSharedMemory(%s) with size %llx, the object has %llx\n", s->o.name, (unsigned long long)size,
                   (unsigned long long)s->size);
        if (size < s->size) size = s->size;
    }
    if (mem_map(addr, size, MS_SHARED, perm & 3))
        r = R_BADMEMSTATE;
    else { /* under g_klock so the switch from the host copy happens in one step for kshmem_ptr's readers */
        pm_lock(&g_klock);
        memcpy(GP(addr), s->host, s->size);
        s->gsize = size;
        __atomic_store_n(&s->gaddr, addr, __ATOMIC_RELEASE);
        pm_unlock(&g_klock);
    }
    kobj_unref(&s->o);
    return r;
}
u32 svc_unmap_shm(u32 h, u64 addr, u64 size) {
    u32 r = check_range(addr, size);
    if (r) return r;
    KShmem* s = (KShmem*)h_get_ref(h, KO_SHMEM);
    if (!s) return R_BADHANDLE;
    pm_lock(&g_klock);
    u64 g = s->gaddr, gs = s->gsize;
    if (g == addr) {
        memcpy(s->host, GP(addr), s->size);
        __atomic_store_n(&s->gaddr, 0, __ATOMIC_RELEASE);
    }
    pm_unlock(&g_klock);
    if (g == addr)
        mem_unmap(addr, gs); /* what MapSharedMemory mapped */
    else
        r = R_BADMEMSTATE; /* not where this object is mapped */
    kobj_unref(&s->o);
    return r;
}
/* svcCreateTransferMemory locks the source with the permission it gives, closing the handle unlocks it */
void tmem_destroy(KObj* o) {
    KTmem* t = (KTmem*)o;
    mem_set_attr(t->addr, t->size, 1, 0);
    mem_set_state(t->addr, t->size, 0xFFFFFFFFu, PERM_RW);
    free(o->waiters);
    free(o);
}

/* ============================================================ GetInfo */
/* application memory size: the console's 3285 MB, or more with memory_gb 6 or 8 (the layouts yuzu calls extended
   memory). Games that need more than the console gives, like Tears of the Kingdom, need memory_gb=6 */
int g_opt_mem_gb;
u64 mem_total(void) {
    if (g_opt_mem_gb >= 8) return 0x1CD500000ull;
    if (g_opt_mem_gb >= 6) return 0x14D500000ull;
    return 0xCD500000ull;
}
/* svcMapPhysicalMemory pages count as used, like on hw */
u64 g_phys_mapped;
u64 used_memory(void) { return g_heap_size + __atomic_load_n(&g_phys_mapped, __ATOMIC_SEQ_CST) + 0x10000000ull; }
void mem_report(char* out, int n) {
    snprintf(out, n, "heap %llu MB, mapped physical memory %llu MB, used %llu MB of %llu MB",
             (unsigned long long)(g_heap_size >> 20), (unsigned long long)(g_phys_mapped >> 20),
             (unsigned long long)(used_memory() >> 20), (unsigned long long)(mem_total() >> 20));
}
/* the npdm's ThreadInfo capability sets them (main.c). These are the values before it was read */
u64 g_core_mask = 0xF, g_prio_mask = 0xFFFFFFFFFFFFFFF0ull;
static u32 svc_get_info_(u32 id0, u32 h, u64 id1, u64* out);
u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out) {
    u32 r = svc_get_info_(id0, h, id1, out);
    static u8 seen[64];
    if (id0 < 64 && !seen[id0]) {
        seen[id0] = 1;
        LOG("GetInfo %u/%llu -> %llx\n", id0, (unsigned long long)id1, (unsigned long long)*out);
    }
    return r;
}
static u32 svc_get_info_(u32 id0, u32 h, u64 id1, u64* out) {
    (void)h;
    *out = 0;
    switch (id0) {
        case 0: *out = g_core_mask; return 0;
        case 1: *out = g_prio_mask; return 0;
        case 2: *out = ALIAS_BASE; return 0;
        case 3: *out = ALIAS_SIZE; return 0;
        case 4: *out = HEAP_BASE; return 0;
        case 5: *out = HEAP_SIZE; return 0;
        case 6: *out = mem_total(); return 0; /* total app memory */
        case 7: *out = used_memory(); return 0;
        case 8: *out = 0; return 0;
        case 9: *out = 0; return 0; /* resource limit handle 0 */
        case 10: *out = 0; return 0;
        case 11: { /* RandomEntropy: four values the process gets when it starts, the same on every call */
            static u64 ent[4];
            if (id1 >= 4) return R_BADCOMBO;
            pm_lock(&g_klock);
            if (!ent[0]) {
                u64 x = plat_ns();
                for (int i = 0; i < 4; i++) { /* splitmix64 */
                    u64 z = (x += 0x9E3779B97F4A7C15ull);
                    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                    ent[i] = (z ^ (z >> 31)) | (i == 0); /* ent[0] != 0 marks them made */
                }
            }
            *out = ent[id1];
            pm_unlock(&g_klock);
            return 0;
        }
        case 12: *out = AS_BASE; return 0;
        case 13: *out = AS_SIZE - AS_BASE; return 0;
        case 14: *out = STACK_BASE; return 0;
        case 15: *out = STACK_SIZE; return 0;
        case 16: *out = g_sysres_size; return 0; /* SystemResourceSize (npdm) */
        case 17: *out = g_sysres_size ? 0x80000ull : 0; return 0;
        case 18: *out = g_title_id; return 0;
        case 19: *out = 0; return 0;
        case 20: { /* UserExceptionContextAddress: one page, made by the first call */
            static u64 uectx;
            pm_lock(&g_klock);
            if (!uectx) uectx = kmem_alloc(0x1000, MS_TLS, PERM_RW);
            *out = uectx;
            pm_unlock(&g_klock);
            return 0;
        }
        case 21: *out = mem_total(); return 0;
        case 22: *out = used_memory(); return 0;
        case 23: *out = 1; return 0;
        case 24: { /* FreeThreadCount: Unity skips its job threads if this is 0 */
            int live = kthread_live_count();
            *out = (u64)(live < THREAD_MAX ? THREAD_MAX - live : 0);
            return 0;
        }
        case 25:
        case 0xF0000002: /* ThreadTickCount. Guest threads aren't scheduled by us, so this is the system tick */
            if (id1 != ~0ull && id1 >= 4) return R_BADCOMBO;
            *out = guest_ticks();
            return 0;
        case 26: *out = 1; return 0;
        case 27:
        case 28: *out = 0; return 0;
        default: WARN("GetInfo %u/%llu not implemented\n", id0, (unsigned long long)id1); return R_BADENUM;
    }
}
