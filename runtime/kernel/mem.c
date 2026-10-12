/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Guest address space. One big host reservation, guest address == offset into it.
   The block list (kept sorted) says what is mapped where, in which state: mapping checks it, svcQueryMemory reads it. */
#include "kernel/mem.h"
#include "gpu/gpu.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>

u8* g_mem;
u64 g_heap_size, g_heap_max; /* heap_max_mb's cap, 0 = none */
static MemBlock* blk;
static int nblk, capblk;
static PMutex g_memlock;
/* the thread holding g_memlock. A crash inside mem.c is reported from the same thread, and the report's mem_query
   must not wait for a lock that thread will never release */
static __thread int t_memlock_held;
static void memlock(void) {
    pm_lock(&g_memlock);
    t_memlock_held = 1;
}
static void memunlock(void) {
    t_memlock_held = 0;
    pm_unlock(&g_memlock);
}

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

/* split the block that contains a so that a block starts at a */
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
/* [a, a+s) lies in the game's address space (page 0 below AS_BASE is the runtime's) */
static int in_as(u64 a, u64 s) { return a >= AS_BASE && a + s <= AS_SIZE && a + s >= a; }

static int is_free_nl(u64 a, u64 s) {
    if (!in_as(a, s)) return 0;
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr < a + s && a < b->addr + b->size) return 0;
    }
    return 1;
}
static void ins_sorted_nl(MemBlock b) {
    int i = 0;
    while (i < nblk && blk[i].addr < b.addr) i++;
    ins_at(i, b);
    merge();
}
static int map_nl(u64 a, u64 s, u32 state, u32 perm) {
    if (!is_free_nl(a, s)) return -1;
    if (plat_vm_commit(GP(a), s)) return -2;
    ins_sorted_nl((MemBlock){a, s, state, perm, 0, 0, 0});
    return 0;
}

int mem_map(u64 a, u64 s, u32 state, u32 perm) {
    if (!s) return 0;
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    memlock();
    int r = map_nl(a, s, state, perm);
    memunlock();
    return r;
}

/* maps the free parts of [a, a+s) in one hold of the lock. The bytes mapped, ~0 when the host had no memory */
u64 mem_map_holes(u64 a, u64 s, u32 state, u32 perm) {
    if (!in_as(a, s)) return ~0ull;
    u64 n = 0;
    memlock();
    for (u64 p = a; p < a + s;) {
        u64 e = a + s; /* the hole ends at the next block, or the next block starts here */
        int used = 0;
        for (int i = 0; i < nblk; i++) {
            MemBlock* b = &blk[i];
            if (b->addr <= p && p < b->addr + b->size) {
                e = b->addr + b->size < e ? b->addr + b->size : e;
                used = 1;
                break;
            }
            if (b->addr > p && b->addr < e) e = b->addr;
        }
        if (!used) {
            if (map_nl(p, e - p, state, perm)) {
                memunlock();
                return ~0ull;
            }
            n += e - p;
        }
        p = e;
    }
    memunlock();
    return n;
}

int mem_unmap(u64 a, u64 s) {
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    if (!in_as(a, s)) return -1;
    memlock();
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
    memunlock();
    return 0;
}

int mem_set_state(u64 a, u64 s, u32 state, u32 perm) {
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    if (!in_as(a, s)) return -1;
    memlock();
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
    memunlock();
    return 0;
}

int mem_set_attr(u64 a, u64 s, u32 mask, u32 attr) {
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    if (!in_as(a, s)) return -1;
    memlock();
    split_at(a);
    split_at(a + s);
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr >= a && b->addr + b->size <= a + s) b->attr = (b->attr & ~mask) | (attr & mask);
    }
    merge();
    memunlock();
    return 0;
}

void mem_query(u64 a, MemBlock* o) {
    int held = t_memlock_held; /* only when crashing inside mem.c */
    if (!held) memlock();
    if (a >= AS_SIZE) {
        o->addr = AS_SIZE;
        o->size = 0 - AS_SIZE;
        o->state = MS_INACCESSIBLE;
        o->perm = 0;
        o->attr = o->ipc = o->dev = 0;
        if (!held) memunlock();
        return;
    }
    u64 lo = 0, hi = AS_SIZE;
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr <= a && a < b->addr + b->size) {
            *o = *b;
            if (!held) memunlock();
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
        if (!held) memunlock();
        return;
    }
    if (lo < AS_BASE) lo = AS_BASE;
    o->addr = lo;
    o->size = hi - lo;
    o->state = MS_FREE;
    o->perm = 0;
    o->attr = o->ipc = o->dev = 0;
    if (!held) memunlock();
}

static u64 find_free_nl(u64 lo, u64 hi, u64 size, u64 align) {
    if (!align) align = 0x1000;
    u64 a = (lo + align - 1) & ~(align - 1);
    for (;;) {
        if (a + size > hi || a + size < a) return 0;
        int clash = 0;
        for (int i = 0; i < nblk; i++) {
            MemBlock* b = &blk[i];
            if (b->addr < a + size && a < b->addr + b->size) {
                a = (b->addr + b->size + align - 1) & ~(align - 1);
                clash = 1;
                break;
            }
        }
        if (!clash) return a;
    }
}
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) {
    memlock();
    u64 a = find_free_nl(lo, hi, size, align);
    memunlock();
    return a;
}

/* finding and mapping in one hold, so two threads (CreateThread's TLS) can't be handed the same pages. The gap found
   has a page more on each side, left free as guard pages */
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    size = (size + 0xFFF) & ~0xFFFull;
    memlock();
    u64 a = find_free_nl(KERN_BASE, KERN_END, size + 0x2000, 0x1000);
    int r = a ? map_nl(a + 0x1000, size, state, perm) : -1;
    memunlock();
    if (r) fatal("kmem_alloc: out of memory (%llx)", (unsigned long long)size);
    return a + 0x1000;
}

/* 0 when the host has no memory for it. The checks the console makes are svc_set_heap_size's */
u64 mem_heap_set(u64 size) {
    if (size > HEAP_SIZE) return 0;
    memlock();
    u64 cur = g_heap_size;
    if (size > cur) {
        u64 add = size - cur;
        if (plat_vm_commit(GP(HEAP_BASE + cur), add)) {
            memunlock();
            return 0;
        }
        /* a fresh block: the one ending at the heap's end can be MapMemory's locked source, merge() joins the
           rest */
        ins_sorted_nl((MemBlock){HEAP_BASE + cur, add, MS_NORMAL, PERM_RW, 0, 0, 0});
        g_heap_size = size;
        memunlock();
    } else if (size < cur) {
        g_heap_size = size;
        memunlock();
        mem_unmap(HEAP_BASE + size, cur - size);
    } else
        memunlock();
    return HEAP_BASE;
}
