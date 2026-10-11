/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* debugging output: guest symbols, backtraces, unresolved calls, register and thread dumps */
#include "kernel/kernel_int.h"
/* ============================================================ SVC */
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
static void gsym_load(void) {
    static int done;
    if (done) return;
    done = 1;
    for (u32 m = 0; m < g_mod->nmods && m < 16; m++) {
        u64 base = g_mod->mods[m].base;
        if (!base) continue;
        MemBlock b;
        mem_query(base, &b);
        if (b.state == MS_FREE) continue;
        u64 mod0 = base + gr32(base + 4);
        if (gr32(mod0) != 0x30444F4D) continue;
        u64 dyn = mod0 + (u64)(i64)(i32)gr32(mod0 + 4), symtab = 0, strtab = 0, hash = 0;
        for (int i = 0; i < 256; i++) {
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
        u64 n = hash ? gr32(base + hash + 4) : (strtab > symtab ? (strtab - symtab) / 24 : 0);
        if (n > 2000000) n = 0;
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
    static char buf[4][200];
    static int k;
    char* o = buf[k++ & 3];
    o[0] = 0;
    gsym_load();
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
    snprintf(o, 200, " <%.150s+%llx>", (const char*)GP(g_gstr[g_gsyms[best].mod] + g_gsyms[best].name_off),
             (unsigned long long)(a - g_gsyms[best].addr));
    return o;
}
static void guest_backtrace(CPU* c) {
    WARN("  pc=%llx lr=%llx sp=%llx fp=%llx\n", (unsigned long long)c->pc, (unsigned long long)c->x[30],
         (unsigned long long)c->x[31], (unsigned long long)c->x[29]);
    WARN("  pc%s  lr%s  x16->%s  x17%s\n", gsym(c->pc), gsym(c->x[30]),
         c->x[16] >= 0x10000 && c->x[16] < AS_SIZE - 8 ? gsym(gr64(c->x[16])) : "", gsym(c->x[17]));
    u64 fp = c->x[29];
    for (int i = 0; i < 24 && fp >= AS_BASE && fp < AS_SIZE && !(fp & 7); i++) {
        MemBlock b;
        mem_query(fp, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) break;
        u64 lr = gr64(fp + 8);
        WARN("   #%d %llx%s\n", i, (unsigned long long)lr, gsym(lr));
        fp = gr64(fp);
    }
}
/* imports rtld couldn't resolve (left at 0) */
static void dump_unresolved(void) {
    int shown = 0;
    for (u32 m = 0; m < g_mod->nmods; m++) {
        u64 base = g_mod->mods[m].base;
        if (!base) continue;
        MemBlock b;
        mem_query(base, &b);
        if (b.state == MS_FREE) continue;
        u64 mod0 = base + gr32(base + 4);
        if (gr32(mod0) != 0x30444F4D) continue;
        u64 dyn = mod0 + (u64)(i64)(i32)gr32(mod0 + 4);
        u64 symtab = 0, strtab = 0, jmprel = 0, pltsz = 0, rela = 0, relasz = 0;
        for (int i = 0; i < 256; i++) {
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
            if (!tabs[t][0]) continue;
            for (u64 o = 0; o + 24 <= tabs[t][1]; o += 24) {
                u64 r = base + tabs[t][0] + o;
                u64 off = gr64(r), info = gr64(r + 8);
                u32 ty = (u32)info, si = (u32)(info >> 32);
                if (!si || (ty != 1026 && ty != 1025 && ty != 257)) continue;
                if (gr64(base + off) != 0) continue;
                u64 s = base + symtab + si * 24ull;
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
    {
        u64 lr = c->x[30] & ~3ull;
        MemBlock cb;
        mem_query(lr, &cb);
        if (cb.state != MS_FREE && lr > 0x100)
            for (u64 base = lr - 0xC0; base < lr + 0x40; base += 0x40) {
                char line[400];
                int n = snprintf(line, sizeof line, "  code %llx:", (unsigned long long)base);
                for (int w = 0; w < 16; w++) n += snprintf(line + n, sizeof line - n, " %08x", gr32(base + w * 4));
                WARN("%s\n", line);
            }
    }
    /* what the regs point at (objects, vtables) */
    for (int r = 0; r < 29; r++) {
        u64 a = c->x[r];
        if (a < 0x10000 || a >= AS_SIZE - 64 || (a & 7)) continue;
        MemBlock b;
        mem_query(a, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) continue;
        WARN("  mem x%d %llx: %llx %llx %llx %llx\n", r, (unsigned long long)a, (unsigned long long)gr64(a),
             (unsigned long long)gr64(a + 8), (unsigned long long)gr64(a + 16), (unsigned long long)gr64(a + 24));
        u64 v = gr64(a);
        if (v >= 0x10000 && v < AS_SIZE - 64 && !(v & 7)) {
            MemBlock vb;
            mem_query(v, &vb);
            if (vb.state != MS_FREE && vb.state != MS_INACCESSIBLE) {
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
                     (unsigned long long)gr64(v + 24), (unsigned long long)gr64(v + 32),
                     (unsigned long long)gr64(v + 40), (unsigned long long)gr64(v + 48),
                     (unsigned long long)gr64(v + 56));
            }
        }
    }
    WARN("  last SVCs (num args -> result out1, called from):\n");
    for (u32 k = g_svc_ring_n > 40 ? g_svc_ring_n - 40 : 0; k < g_svc_ring_n; k++) {
        SvcRec* r = &g_svc_ring[k & 63];
        WARN("    %02x %llx %llx %llx %llx -> %x %llx  lr=%llx\n", r->imm, (unsigned long long)r->a[0],
             (unsigned long long)r->a[1], (unsigned long long)r->a[2], (unsigned long long)r->a[3], r->res,
             (unsigned long long)r->o1, (unsigned long long)r->lr);
    }
    WARN("  last IPC calls:\n");
    for (u32 k = g_ipc_ring_n > 32 ? g_ipc_ring_n - 32 : 0; k < g_ipc_ring_n; k++) WARN("    %s\n", g_ipc_ring[k & 31]);
    /* code around the last svcs, usually the check that failed before an abort */
    {
        u64 seen[6];
        int ns = 0;
        for (u32 k = g_svc_ring_n; k > 0 && k + 12 > g_svc_ring_n && ns < 4; k--) {
            u64 lr = g_svc_ring[(k - 1) & 63].lr;
            int dup = 0;
            for (int j = 0; j < ns; j++)
                if (seen[j] == lr) dup = 1;
            if (dup) continue;
            seen[ns++] = lr;
            for (u64 base = lr - 0x100; base < lr + 0x100; base += 0x40) {
                MemBlock cb;
                mem_query(base, &cb);
                if (cb.state == MS_FREE) break;
                char line[400];
                int n = snprintf(line, sizeof line, "  code %llx:", (unsigned long long)base);
                for (int w = 0; w < 16; w++) n += snprintf(line + n, sizeof line - n, " %08x", gr32(base + w * 4));
                WARN("%s\n", line);
            }
        }
    }
    /* and around the callers */
    u64 fp = c->x[29];
    for (int i = 0; i < 8 && fp >= AS_BASE && fp < AS_SIZE && !(fp & 7); i++) {
        MemBlock b;
        mem_query(fp, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) break;
        u64 lr = gr64(fp + 8);
        fp = gr64(fp);
        MemBlock cb;
        mem_query(lr - 0x60, &cb);
        if (cb.state == MS_FREE) continue;
        for (u64 base = lr - 0x140; base < lr + 0x40; base += 0x40) {
            char line[400];
            int n = snprintf(line, sizeof line, "  code %llx:", (unsigned long long)base);
            for (int w = 0; w < 16; w++) n += snprintf(line + n, sizeof line - n, " %08x", gr32(base + w * 4));
            WARN("%s\n", line);
        }
    }
}

/* dump code around an address */
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
    LOG("---- game thread state ----\n");
    for (int i = 0; i < g_nthreads; i++) {
        KThread* t = g_threads[i];
        if (!t || t->state == 2) continue;
        int k = t->wait_kind;
        if (k < 0 || k > 4) k = 0;
        LOG("thread %llu '%s' prio %d: %s", (unsigned long long)t->id, t->name, t->prio, kinds[k]);
        if (k == 1) LOG(" [%s%s]", t->wait_name ? t->wait_name : "?", t->wait_timeout < 0 ? "" : " with timeout");
        if (k >= 2)
            LOG(" [addr %llx value %x]", (unsigned long long)t->wait_addr,
                (k == 2 || k == 3) ? gr32(t->wait_addr) : 0u);
        if (k == 2) { /* the mutex's owner, from the handle in its tag */
            KObj* o = h_get(gr32(t->wait_addr) & ~HANDLE_WAIT);
            if (o && o->type == KO_THREAD) LOG(" owned by thread %llu", (unsigned long long)((KThread*)o)->id);
        }
        LOG("  lr=%llx pc=%llx sp=%llx\n", (unsigned long long)t->cpu.x[30], (unsigned long long)t->cpu.pc,
            (unsigned long long)t->cpu.x[31]);
        {
            u64 fp = t->cpu.x[29];
            LOG("    stack:");
            for (int i = 0; i < 10 && fp >= AS_BASE && fp < AS_SIZE && !(fp & 7); i++) {
                MemBlock b;
                mem_query(fp, &b);
                if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) break;
                LOG(" %llx", (unsigned long long)gr64(fp + 8));
                fp = gr64(fp);
            }
            LOG("\n");
        }
    }
    /* threads spinning on SleepThread: dump regs + nearby code */
    {
        {
            for (int i = 0; i < g_nthreads; i++) {
                KThread* t = g_threads[i];
                if (!t || t->state == 2 || t->wait_kind != 0 || !t->sleep_n) continue;
                LOG("thread %llu looping on SleepThread (%llu times) from lr=%llx\n    regs:",
                    (unsigned long long)t->id, (unsigned long long)t->sleep_n, (unsigned long long)t->sleep_lr);
                for (int r = 0; r < 31; r++)
                    LOG("%s x%d=%llx", (r % 6 == 0) ? "\n     " : "", r, (unsigned long long)t->sleep_x[r]);
                LOG("\n");
                for (int r = 0; r < 31; r++) {
                    u64 a = t->sleep_x[r] & ~7ull;
                    MemBlock b;
                    if (a < AS_BASE || a >= AS_SIZE) continue;
                    mem_query(a, &b);
                    if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !(b.perm & 1)) continue;
                    LOG("    mem x%d [%llx]:", r, (unsigned long long)a);
                    for (int j = 0; j < 8; j++) LOG(" %llx", (unsigned long long)gr64(a + j * 8));
                    LOG("\n");
                }
                u64 fp = t->cpu.x[29], ras[4];
                int nra = 0;
                ras[nra++] = t->sleep_lr;
                for (int j = 0; j < 3 && fp >= AS_BASE && fp < AS_SIZE && !(fp & 7); j++) {
                    MemBlock b;
                    mem_query(fp, &b);
                    if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) break;
                    ras[nra++] = gr64(fp + 8);
                    fp = gr64(fp);
                }
                for (int j = 0; j < nra; j++) kernel_dump_code(ras[j]);
            }
            KThread* m = g_main;
            if (m) {
                u64 fp = m->cpu.x[29];
                for (int j = 0; j < 4 && fp >= AS_BASE && fp < AS_SIZE && !(fp & 7); j++) {
                    MemBlock b;
                    mem_query(fp, &b);
                    if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) break;
                    kernel_dump_code(gr64(fp + 8));
                    fp = gr64(fp);
                }
            }
        }
    }
    {
        {
            LOG("  last SVCs:\n");
            for (u32 q = g_svc_ring_n > 40 ? g_svc_ring_n - 40 : 0; q < g_svc_ring_n; q++) {
                SvcRec* r = &g_svc_ring[q & 63];
                LOG("    %02x %llx %llx %llx %llx -> %x %llx  lr=%llx\n", r->imm, (unsigned long long)r->a[0],
                    (unsigned long long)r->a[1], (unsigned long long)r->a[2], (unsigned long long)r->a[3], r->res,
                    (unsigned long long)r->o1, (unsigned long long)r->lr);
            }
            LOG("  last IPCs:\n");
            for (u32 q = g_ipc_ring_n > 32 ? g_ipc_ring_n - 32 : 0; q < g_ipc_ring_n; q++)
                LOG("    %s\n", g_ipc_ring[q & 31]);
        }
    }
    LOG("---------------------------------------\n");
    fflush(NULL);
}
