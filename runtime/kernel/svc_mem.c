/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the memory SVCs (QueryMemory, MapMemory, shared and transfer memory) and GetInfo */
#include "kernel/kernel_int.h"
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
u32 svc_map_memory(u64 dst, u64 src, u64 size) {
    if ((dst | src | size) & 0xFFF || !size) return R_BADADDR;
    int in_stack = dst >= STACK_BASE && dst + size <= STACK_BASE + STACK_SIZE;
    int in_alias = dst >= ALIAS_BASE && dst + size <= ALIAS_BASE + ALIAS_SIZE;
    if (!in_stack && !in_alias) return R_BADREGION;
    if (!mem_is_free(dst, size)) return R_BADMEMSTATE;
    if (mem_map(dst, size, in_stack ? MS_STACK : MS_NORMAL, PERM_RW)) return R_OUTOFMEM;
    memcpy(GP(dst), GP(src), size);
    mem_set_attr(src, size, 1, 1); /* locked */
    mem_set_state(src, size, 0xFFFFFFFFu, PERM_NONE);
    return 0;
}
u32 svc_unmap_memory(u64 dst, u64 src, u64 size) {
    if ((dst | src | size) & 0xFFF || !size) return R_BADADDR;
    memcpy(GP(src), GP(dst), size);
    mem_unmap(dst, size);
    mem_set_attr(src, size, 1, 0);
    mem_set_state(src, size, 0xFFFFFFFFu, PERM_RW);
    return 0;
}
static void shm_destroy(KObj* o) {
    KShmem* s = (KShmem*)o;
    if (!s->gaddr) free(s->host);
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
u8* kshmem_ptr(KShmem* s) { return s->gaddr ? GP(s->gaddr) : s->host; }
u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm) {
    KShmem* s = (KShmem*)h_get_type(h, KO_SHMEM);
    if (!s) return R_BADHANDLE;
    if ((addr | size) & 0xFFF || !size) return R_BADADDR;
    if (size < s->size) size = s->size;
    if (mem_map(addr, size, MS_SHARED, perm & 3)) return R_BADMEMSTATE;
    memcpy(GP(addr), s->host, s->size);
    s->gaddr = addr;
    return 0;
}
u32 svc_unmap_shm(u32 h, u64 addr, u64 size) {
    KShmem* s = (KShmem*)h_get_type(h, KO_SHMEM);
    if (!s) return R_BADHANDLE;
    if (s->gaddr == addr) {
        memcpy(s->host, GP(addr), s->size);
        s->gaddr = 0;
    }
    mem_unmap(addr, size);
    return 0;
}
void tmem_destroy(KObj* o) {
    KTmem* t = (KTmem*)o;
    mem_set_state(t->addr, t->size, 0xFFFFFFFFu, PERM_RW);
    free(o->waiters);
    free(o);
}

/* ============================================================ GetInfo */
/* application memory size. Retail gives 3.2 GB; memory_gb can raise it (yuzu calls this extended memory).
   TotK defaults to 5.2 GB, 3.2 isn't enough for it and its allocator returns NULL during load. */
int g_opt_mem_gb;
u64 mem_total(void) {
    if (g_opt_mem_gb >= 8) return 0x1CD500000ull;
    if (g_opt_mem_gb >= 6) return 0x14D500000ull;
    if (g_opt_mem_gb >= 4 || g_opt_mem_gb < 0) return 0xCD500000ull;
    if (g_title_id == 0x0100F2C0115B6000ull) return 0x14D500000ull; /* Tears of the Kingdom */
    return 0xCD500000ull;
}
/* svcMapPhysicalMemory pages count as used, like on hw */
u64 g_phys_mapped;
u64 used_memory(void) { return g_heap_size + g_phys_mapped + 0x10000000ull; }
void mem_report(char* out, int n) {
    snprintf(out, n, "heap %llu MB, mapped physical memory %llu MB, used %llu MB of %llu MB",
             (unsigned long long)(g_heap_size >> 20), (unsigned long long)(g_phys_mapped >> 20),
             (unsigned long long)(used_memory() >> 20), (unsigned long long)(mem_total() >> 20));
}
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
    switch (id0) {
        case 0: *out = 0xF; return 0; /* CoreMask */
        case 1:
            *out = 0xFFFFFFFFFFFFFFFFull >> (64 - 60) << 4;
            *out = 0xFFFFFFFFFFFFFFF0ull;
            return 0;
        case 2: *out = ALIAS_BASE; return 0;
        case 3: *out = ALIAS_SIZE; return 0;
        case 4: *out = HEAP_BASE; return 0;
        case 5: *out = HEAP_SIZE; return 0;
        case 6: *out = mem_total(); return 0; /* total app memory */
        case 7: *out = used_memory(); return 0;
        case 8: *out = 0; return 0;
        case 9: *out = 0; return 0; /* resource limit handle 0 */
        case 10: *out = 0; return 0;
        case 11: {
            u64 r = plat_ns() * 0x9E3779B97F4A7C15ull ^ (id1 * 0xC2B2AE3D27D4EB4Full);
            *out = r;
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
        case 20: {
            static u64 uectx;
            if (!uectx) uectx = kmem_alloc(0x1000, MS_TLS, PERM_RW);
            *out = uectx;
            return 0;
        }
        case 21: *out = mem_total(); return 0;
        case 22: *out = used_memory(); return 0;
        case 23: *out = 1; return 0;
        case 24: {
            int live = 0;
            for (int i = 0; i < g_nthreads; i++)
                if (g_threads[i] && g_threads[i]->state != 2) live++;
            *out = (u64)(live < 96 ? 96 - live : 0);
            return 0;
        } /* FreeThreadCount: Unity skips its job threads if this is 0 */
        case 0xF0000002: {
            KThread* t = kthread_cur();
            (void)t;
            *out = plat_ns() * 192 / 10000;
            return 0;
        }
        case 26: *out = 1; return 0;
        case 27:
        case 28: *out = 0; return 0;
        default:
            WARN("GetInfo %u/%llu not implemented\n", id0, (unsigned long long)id1);
            *out = 0;
            return 0;
    }
    (void)h;
}
