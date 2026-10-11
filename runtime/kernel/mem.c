/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* Guest address space. One big host reservation, guest address == offset into it.
   The block list (kept sorted) is only there for svcQueryMemory and friends. */
#include "kernel/mem.h"
#include "gpu/gpu.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>

u8* g_mem;
u64 g_heap_size, g_heap_max = 0x180000000ull; /* heap cap: 6 GB */
static MemBlock* blk;
static int nblk, capblk;
static PMutex g_memlock;

void mem_init(void) {
    pm_init(&g_memlock);
    g_mem = plat_vm_reserve(AS_SIZE);
    if (!g_mem) fatal("Could not reserve %llu GB of virtual memory for the game.", (unsigned long long)(AS_SIZE >> 30));
    /* page 0 is mapped and zeroed so a NULL(+off) read gives 0 instead of a crash */
    plat_vm_commit(g_mem, 0x10000);
}

static void ins_at(int i, MemBlock b) {
    if (nblk == capblk) {
        capblk = capblk ? capblk * 2 : 256;
        blk = realloc(blk, sizeof(MemBlock) * capblk);
    }
    memmove(blk + i + 1, blk + i, sizeof(MemBlock) * (nblk - i));
    blk[i] = b;
    nblk++;
}
static void del_at(int i) {
    memmove(blk + i, blk + i + 1, sizeof(MemBlock) * (nblk - i - 1));
    nblk--;
}

/* split so [a,a+s) starts and ends on block edges, returns index of first block */
static void split_at(u64 a) {
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr < a && a < b->addr + b->size) {
            MemBlock n = *b;
            n.addr = a;
            n.size = b->addr + b->size - a;
            b->size = a - b->addr;
            ins_at(i + 1, n);
            return;
        }
    }
}
static void merge(void) {
    for (int i = 0; i + 1 < nblk;) {
        MemBlock *a = &blk[i], *b = &blk[i + 1];
        if (a->addr + a->size == b->addr && a->state == b->state && a->perm == b->perm && a->attr == b->attr &&
            a->ipc == b->ipc && a->dev == b->dev) {
            a->size += b->size;
            del_at(i + 1);
        } else
            i++;
    }
}

int mem_is_free(u64 a, u64 s) {
    if (a < AS_BASE || a + s > AS_SIZE || a + s < a) return 0;
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr < a + s && a < b->addr + b->size) return 0;
    }
    return 1;
}

int mem_map(u64 a, u64 s, u32 state, u32 perm) {
    if (!s) return 0;
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    pm_lock(&g_memlock);
    if (!mem_is_free(a, s)) {
        pm_unlock(&g_memlock);
        return -1;
    }
    if (plat_vm_commit(GP(a), s)) {
        pm_unlock(&g_memlock);
        return -2;
    }
    int i = 0;
    while (i < nblk && blk[i].addr < a) i++;
    MemBlock b = {a, s, state, perm, 0, 0, 0};
    ins_at(i, b);
    merge();
    pm_unlock(&g_memlock);
    return 0;
}

int mem_unmap(u64 a, u64 s) {
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    pm_lock(&g_memlock);
    split_at(a);
    split_at(a + s);
    for (int i = 0; i < nblk;) {
        MemBlock* b = &blk[i];
        if (b->addr >= a && b->addr + b->size <= a + s)
            del_at(i);
        else
            i++;
    }
    plat_vm_decommit(GP(a), s);
    {
        if (g_ww_touch && s) {
            for (u64 o = 0; o < s; o += 0x10000) g_ww_touch(a + o);
            g_ww_touch(a + s - 1);
        }
    } /* fresh pages read as zero */
    pm_unlock(&g_memlock);
    return 0;
}

int mem_set_state(u64 a, u64 s, u32 state, u32 perm) {
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    pm_lock(&g_memlock);
    split_at(a);
    split_at(a + s);
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr >= a && b->addr + b->size <= a + s) {
            if (state != 0xFFFFFFFFu) b->state = state;
            if (perm != 0xFFFFFFFFu) b->perm = perm;
        }
    }
    merge();
    pm_unlock(&g_memlock);
    return 0;
}

int mem_set_attr(u64 a, u64 s, u32 mask, u32 attr) {
    pm_lock(&g_memlock);
    split_at(a);
    split_at(a + s);
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr >= a && b->addr + b->size <= a + s) b->attr = (b->attr & ~mask) | (attr & mask);
    }
    merge();
    pm_unlock(&g_memlock);
    return 0;
}

void mem_query(u64 a, MemBlock* o) {
    pm_lock(&g_memlock);
    if (a >= AS_SIZE) {
        o->addr = AS_SIZE;
        o->size = 0 - AS_SIZE;
        o->state = MS_INACCESSIBLE;
        o->perm = 0;
        o->attr = o->ipc = o->dev = 0;
        pm_unlock(&g_memlock);
        return;
    }
    u64 lo = 0, hi = AS_SIZE;
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr <= a && a < b->addr + b->size) {
            *o = *b;
            pm_unlock(&g_memlock);
            return;
        }
        if (b->addr + b->size <= a && b->addr + b->size > lo) lo = b->addr + b->size;
        if (b->addr > a && b->addr < hi) hi = b->addr;
    }
    if (a < AS_BASE) {
        hi = hi < AS_BASE ? hi : AS_BASE;
        o->addr = 0;
        o->size = hi;
        o->state = MS_INACCESSIBLE;
        o->perm = 0;
        o->attr = o->ipc = o->dev = 0;
        pm_unlock(&g_memlock);
        return;
    }
    if (lo < AS_BASE) lo = AS_BASE;
    o->addr = lo;
    o->size = hi - lo;
    o->state = MS_FREE;
    o->perm = 0;
    o->attr = o->ipc = o->dev = 0;
    pm_unlock(&g_memlock);
}

u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) {
    if (!align) align = 0x1000;
    pm_lock(&g_memlock);
    u64 a = (lo + align - 1) & ~(align - 1);
    for (;;) {
        if (a + size > hi || a + size < a) {
            pm_unlock(&g_memlock);
            return 0;
        }
        int clash = 0;
        for (int i = 0; i < nblk; i++) {
            MemBlock* b = &blk[i];
            if (b->addr < a + size && a < b->addr + b->size) {
                a = (b->addr + b->size + align - 1) & ~(align - 1);
                clash = 1;
                break;
            }
        }
        if (!clash) {
            pm_unlock(&g_memlock);
            return a;
        }
    }
}

u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    size = (size + 0xFFF) & ~0xFFFull;
    u64 a = mem_find_free(KERN_BASE, KERN_END, size + 0x1000, 0x1000); /* + guard page */
    if (!a || mem_map(a, size, state, perm)) fatal("kmem_alloc: out of memory (%llx)", (unsigned long long)size);
    return a;
}

u64 mem_heap_set(u64 size) {
    if (size > g_heap_max) return 0;
    if (size > g_heap_size) {
        u64 add = size - g_heap_size;
        pm_lock(&g_memlock);
        int ok = !plat_vm_commit(GP(HEAP_BASE + g_heap_size), add);
        if (ok) {
            int found = 0;
            for (int i = 0; i < nblk; i++)
                if (blk[i].addr + blk[i].size == HEAP_BASE + g_heap_size && blk[i].state == MS_NORMAL && g_heap_size) {
                    blk[i].size += add;
                    found = 1;
                    break;
                }
            if (!found) {
                int i = 0;
                while (i < nblk && blk[i].addr < HEAP_BASE + g_heap_size) i++;
                MemBlock b = {HEAP_BASE + g_heap_size, add, MS_NORMAL, PERM_RW, 0, 0, 0};
                ins_at(i, b);
            }
            merge();
        }
        pm_unlock(&g_memlock);
        if (!ok) return 0;
    } else if (size < g_heap_size) {
        mem_unmap(HEAP_BASE + size, g_heap_size - size);
    }
    g_heap_size = size;
    return HEAP_BASE;
}
