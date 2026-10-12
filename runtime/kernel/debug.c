/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* debugging output: guest symbols, backtraces, unresolved calls, register and thread dumps. These run from the crash
   handler too, where a fault while reading guest memory would end the report: every read is checked first */
#include "kernel/kernel_int.h"
/* [a, a+n) is mapped game memory, safe to read */
static int mapped(u64 a, u64 n) {
    if (a < AS_BASE || a >= AS_SIZE || n > AS_SIZE - a) return 0;
    for (u64 p = a; p < a + n;) {
        MemBlock b;
        mem_query(p, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) return 0;
        p = b.addr + b.size;
    }
    return 1;
}
/* the return addresses of the frame-pointer chain from fp, at most max */
static int frame_walk(u64 fp, u64* lrs, int max) {
    int n = 0;
    while (n < max && !(fp & 7) && mapped(fp, 16)) {
        lrs[n++] = gr64(fp + 8);
        fp = gr64(fp);
    }
    return n;
}
/* the code in [from, to) as lines of 16 words, the lines that are mapped. lvl as logf_'s: 0 = WARN, 1 = LOG */
static void dump_code(int lvl, u64 from, u64 to) {
    for (u64 base = from & ~3ull; base < to; base += 0x40) {
        if (!mapped(base, 0x40)) continue;
        char line[400];
        int n = snprintf(line, sizeof line, "  code %llx:", (unsigned long long)base);
        for (int w = 0; w < 16; w++) n += snprintf(line + n, sizeof line - n, " %08x", gr32(base + w * 4));
        logf_(lvl, "%s\n", line);
    }
}
/* the last SVCs and IPC requests. The counters move while other threads run, so each is read once */
static void dump_rings(int lvl) {
    u32 sn = __atomic_load_n(&g_svc_ring_n, __ATOMIC_RELAXED), in = __atomic_load_n(&g_ipc_ring_n, __ATOMIC_RELAXED);
    logf_(lvl, "  last SVCs (num args -> result out1, called from):\n");
    for (u32 k = sn > 40 ? sn - 40 : 0; k < sn; k++) {
        SvcRec* r = &g_svc_ring[k & 63];
        logf_(lvl, "    %02x %llx %llx %llx %llx -> %x %llx  lr=%llx\n", r->imm, (unsigned long long)r->a[0],
              (unsigned long long)r->a[1], (unsigned long long)r->a[2], (unsigned long long)r->a[3], r->res,
              (unsigned long long)r->o1, (unsigned long long)r->lr);
    }
    logf_(lvl, "  last IPC calls:\n");
    for (u32 k = in > 32 ? in - 32 : 0; k < in; k++) logf_(lvl, "    %s\n", g_ipc_ring[k & 31]);
}
/* ============================================================ symbols */
/* nearest exported symbol via each module's dynsym */
typedef struct {
    u64 addr;
    u32 name_off;
    u32 mod;
} GSym;
static GSym* g_gsyms;
static int g_ngsyms;
static u64 g_gstr[16];
static int gsym_cmp(const void* a, const void* b) {
    u64 x = ((const GSym*)a)->addr, y = ((const GSym*)b)->addr;
    return x < y ? -1 : x > y;
}
/* read once at startup: the crash handler only looks them up, it doesn't allocate */
void kernel_load_symbols(void) {
    for (u32 m = 0; m < g_mod->nmods && m < 16; m++) {
        u64 base = g_mod->mods[m].base;
        if (!base || !mapped(base, 8)) continue;
        u64 mod0 = base + gr32(base + 4);
        if (!mapped(mod0, 8) || gr32(mod0) != 0x30444F4D) continue;
        u64 dyn = mod0 + (u64)(i64)(i32)gr32(mod0 + 4), symtab = 0, strtab = 0, hash = 0;
        for (int i = 0; i < 256 && mapped(dyn + i * 16, 16); i++) {
            u64 tag = gr64(dyn + i * 16), val = gr64(dyn + i * 16 + 8);
            if (!tag) break;
            if (tag == 6)
                symtab = val;
            else if (tag == 5)
                strtab = val;
            else if (tag == 4)
                hash = val;
        }
        if (!symtab || !strtab) continue;
        u64 n = hash && mapped(base + hash, 8) ? gr32(base + hash + 4) : (strtab > symtab ? (strtab - symtab) / 24 : 0);
        if (n > 2000000 || !mapped(base + symtab, n * 24)) n = 0;
        g_gstr[m] = base + strtab;
        g_gsyms = realloc(g_gsyms, (g_ngsyms + n) * sizeof(GSym));
        for (u64 i = 1; i < n; i++) {
            u64 s = base + symtab + i * 24;
            u64 v = gr64(s + 8);
            u16 sh = *(u16*)GP(s + 6);
            if (!v || !sh) continue;
            g_gsyms[g_ngsyms].addr = base + v;
            g_gsyms[g_ngsyms].name_off = gr32(s);
            g_gsyms[g_ngsyms].mod = m;
            g_ngsyms++;
        }
    }
    if (g_ngsyms) qsort(g_gsyms, g_ngsyms, sizeof(GSym), gsym_cmp);
}
static const char* gsym(u64 a) {
    static __thread char buf[4][200]; /* a few per thread, so one printf can hold several */
    static __thread int k;
    char* o = buf[k++ & 3];
    o[0] = 0;
    int lo = 0, hi = g_ngsyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_gsyms[mid].addr <= a) {
            best = mid;
            lo = mid + 1;
        } else
            hi = mid - 1;
    }
    if (best < 0 || a - g_gsyms[best].addr > 0x20000) return o;
    u64 name = g_gstr[g_gsyms[best].mod] + g_gsyms[best].name_off;
    if (!mapped(name, 1)) return o;
    snprintf(o, 200, " <%.150s+%llx>", (const char*)GP(name), (unsigned long long)(a - g_gsyms[best].addr));
    return o;
}
static void guest_backtrace(CPU* c) {
    WARN("  pc=%llx lr=%llx sp=%llx fp=%llx\n", (unsigned long long)c->pc, (unsigned long long)c->x[30],
         (unsigned long long)c->x[31], (unsigned long long)c->x[29]);
    WARN("  pc%s  lr%s  x16->%s  x17%s\n", gsym(c->pc), gsym(c->x[30]), mapped(c->x[16], 8) ? gsym(gr64(c->x[16])) : "",
         gsym(c->x[17]));
    u64 lrs[24];
    int n = frame_walk(c->x[29], lrs, 24);
    for (int i = 0; i < n; i++) WARN("   #%d %llx%s\n", i, (unsigned long long)lrs[i], gsym(lrs[i]));
}
/* imports rtld couldn't resolve (left at 0) */
static void dump_unresolved(void) {
    int shown = 0;
    for (u32 m = 0; m < g_mod->nmods; m++) {
        u64 base = g_mod->mods[m].base;
        if (!base || !mapped(base, 8)) continue;
        u64 mod0 = base + gr32(base + 4);
        if (!mapped(mod0, 8) || gr32(mod0) != 0x30444F4D) continue;
        u64 dyn = mod0 + (u64)(i64)(i32)gr32(mod0 + 4);
        u64 symtab = 0, strtab = 0, jmprel = 0, pltsz = 0, rela = 0, relasz = 0;
        for (int i = 0; i < 256 && mapped(dyn + i * 16, 16); i++) {
            u64 tag = gr64(dyn + i * 16), val = gr64(dyn + i * 16 + 8);
            if (!tag) break;
            if (tag == 6)
                symtab = val;
            else if (tag == 5)
                strtab = val;
            else if (tag == 23)
                jmprel = val;
            else if (tag == 2)
                pltsz = val;
            else if (tag == 7)
                rela = val;
            else if (tag == 8)
                relasz = val;
        }
        if (!symtab || !strtab) continue;
        u64 tabs[2][2] = {{jmprel, pltsz}, {rela, relasz}};
        for (int t = 0; t < 2; t++) {
            if (!tabs[t][0] || !mapped(base + tabs[t][0], tabs[t][1])) continue;
            for (u64 o = 0; o + 24 <= tabs[t][1]; o += 24) {
                u64 r = base + tabs[t][0] + o;
                u64 off = gr64(r), info = gr64(r + 8);
                u32 ty = (u32)info, si = (u32)(info >> 32);
                if (!si || (ty != 1026 && ty != 1025 && ty != 257)) continue;
                if (!mapped(base + off, 8) || gr64(base + off) != 0) continue;
                u64 s = base + symtab + si * 24ull;
                if (!mapped(s, 24) || !mapped(base + strtab + gr32(s), 2)) continue;
                const char* nm = (const char*)GP(base + strtab + gr32(s));
                u8 inf = *(u8*)GP(s + 4);
                if (nm[0] == '_' && nm[1] == '_' && (inf >> 4) == 2) continue; /* linker symbols */
                if (shown++ < 80)
                    WARN("  unresolved in %s: %s%s (GOT %llx)\n", g_mod->mods[m].name, nm,
                         (inf >> 4) == 2 ? " [weak]" : "", (unsigned long long)(base + off));
            }
        }
    }
    if (shown) WARN("  %d unresolved imports in total\n", shown);
}
void kernel_dump_regs(CPU* c) {
    if (c->pc < 0x1000 || getenv("SWR_UNRES")) dump_unresolved();
    for (int i = 0; i < 31; i += 4)
        WARN("  x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n", i, (unsigned long long)c->x[i], i + 1,
             (unsigned long long)c->x[i + 1], i + 2, (unsigned long long)c->x[i + 2], i + 3,
             (unsigned long long)(i + 3 < 32 ? c->x[i + 3] : 0));
    guest_backtrace(c);
    /* code around lr */
    u64 lr = c->x[30] & ~3ull;
    if (lr > 0x100) dump_code(0, lr - 0xC0, lr + 0x40);
    /* what the regs point at (objects, vtables) */
    for (int r = 0; r < 29; r++) {
        u64 a = c->x[r];
        if (a < 0x10000 || (a & 7) || !mapped(a, 32)) continue;
        WARN("  mem x%d %llx: %llx %llx %llx %llx\n", r, (unsigned long long)a, (unsigned long long)gr64(a),
             (unsigned long long)gr64(a + 8), (unsigned long long)gr64(a + 16), (unsigned long long)gr64(a + 24));
        u64 v = gr64(a);
        if (v >= 0x10000 && !(v & 7) && mapped(v, 64)) {
            {
                char sl[1200];
                int n = 0;
                for (int e = 0; e < 8; e++) {
                    const char* s = gsym(gr64(v + e * 8));
                    if (s[0]) n += snprintf(sl + n, sizeof sl - n, " [%d]%s", e, s);
                    if (n > 1000) break;
                }
                if (n) WARN("    vtable:%s\n", sl);
            }
            WARN("    -> %llx: %llx %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)v,
                 (unsigned long long)gr64(v), (unsigned long long)gr64(v + 8), (unsigned long long)gr64(v + 16),
                 (unsigned long long)gr64(v + 24), (unsigned long long)gr64(v + 32), (unsigned long long)gr64(v + 40),
                 (unsigned long long)gr64(v + 48), (unsigned long long)gr64(v + 56));
        }
    }
    dump_rings(0);
    /* code around the last svcs, usually the check that failed before an abort */
    {
        u32 sn = __atomic_load_n(&g_svc_ring_n, __ATOMIC_RELAXED);
        u64 seen[6];
        int ns = 0;
        for (u32 k = sn; k > 0 && k + 12 > sn && ns < 4; k--) {
            u64 l = g_svc_ring[(k - 1) & 63].lr;
            int dup = 0;
            for (int j = 0; j < ns; j++)
                if (seen[j] == l) dup = 1;
            if (dup) continue;
            seen[ns++] = l;
            dump_code(0, l - 0x100, l + 0x100);
        }
    }
    /* and around the callers */
    u64 lrs[8];
    int n = frame_walk(c->x[29], lrs, 8);
    for (int i = 0; i < n; i++) dump_code(0, lrs[i] - 0x140, lrs[i] + 0x40);
}

/* dump code around an address, inside its block */
static void kernel_dump_code(u64 ra) {
    u64 a = (ra & ~3ull) - 0x180;
    MemBlock b;
    mem_query(ra, &b);
    if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !(b.perm & 1)) return;
    if (a < b.addr) a = b.addr;
    LOG("    code %llx:", (unsigned long long)a);
    for (int i = 0; i < 0x70 && a + i * 4 < b.addr + b.size; i++)
        LOG("%s%08x", (i % 16 == 0) ? "\n     " : " ", gr32(a + i * 4));
    LOG("\n");
}
/* threads.txt: per-thread state, used when the screen stops changing */
void kernel_dump_threads(void) {
    static const char* kinds[] = {"running/in svc", "objects", "mutex", "condvar", "address"};
    static u64 slept[256]; /* sleep_n at the last dump, by g_threads slot */
    LOG("---- game thread state ----\n");
    for (int i = 0; i < g_nthreads; i++) {
        KThread* t = g_threads[i];
        if (!t || t->state == 2) continue;
        int k = t->wait_kind;
        if (k < 0 || k > 4) k = 0;
        LOG("thread %llu '%s' prio %d: %s", (unsigned long long)t->id, t->name, t->prio,
            t->state == 0 ? "not started" : kinds[k]);
        if (k == 1) LOG(" [%s%s]", t->wait_name ? t->wait_name : "?", t->wait_timeout < 0 ? "" : " with timeout");
        if (k >= 2) {
            u64 a = t->wait_addr;
            LOG(" [addr %llx value %x", (unsigned long long)a, mapped(a, 4) ? gr32(a) : 0u);
            if (k == 3)
                LOG(" key %llx value %x", (unsigned long long)t->wait_key,
                    mapped(t->wait_key, 4) ? gr32(t->wait_key) : 0u);
            LOG("]");
        }
        LOG("  lr=%llx pc=%llx sp=%llx\n", (unsigned long long)t->cpu.x[30], (unsigned long long)t->cpu.pc,
            (unsigned long long)t->cpu.x[31]);
        u64 lrs[10];
        int n = frame_walk(t->cpu.x[29], lrs, 10);
        LOG("    stack:");
        for (int j = 0; j < n; j++) LOG(" %llx", (unsigned long long)lrs[j]);
        LOG("\n");
    }
    /* threads spinning on SleepThread, 100 times or more since the last dump: regs + nearby code */
    for (int i = 0; i < g_nthreads; i++) {
        KThread* t = g_threads[i];
        u64 sn = t ? t->sleep_n : 0, since = sn >= slept[i] ? sn - slept[i] : sn; /* the slot may hold a new thread */
        slept[i] = sn;
        if (!t || t->state == 2 || t->wait_kind != 0 || since < 100) continue;
        LOG("thread %llu looping on SleepThread (%llu times since the last dump) from lr=%llx\n    regs:",
            (unsigned long long)t->id, (unsigned long long)since, (unsigned long long)t->sleep_lr);
        for (int r = 0; r < 31; r++)
            LOG("%s x%d=%llx", (r % 6 == 0) ? "\n     " : "", r, (unsigned long long)t->sleep_x[r]);
        LOG("\n");
        for (int r = 0; r < 31; r++) {
            u64 a = t->sleep_x[r] & ~7ull;
            MemBlock b;
            if (!mapped(a, 64)) continue;
            mem_query(a, &b);
            if (!(b.perm & 1)) continue;
            LOG("    mem x%d [%llx]:", r, (unsigned long long)a);
            for (int j = 0; j < 8; j++) LOG(" %llx", (unsigned long long)gr64(a + j * 8));
            LOG("\n");
        }
        u64 ras[4];
        ras[0] = t->sleep_lr;
        int nra = 1 + frame_walk(t->cpu.x[29], ras + 1, 3);
        for (int j = 0; j < nra; j++) kernel_dump_code(ras[j]);
    }
    KThread* m = g_main;
    if (m && m->state != 2) {
        u64 lrs[4];
        int n = frame_walk(m->cpu.x[29], lrs, 4);
        for (int j = 0; j < n; j++) kernel_dump_code(lrs[j]);
    }
    dump_rings(1);
    LOG("---------------------------------------\n");
    fflush(NULL);
}
