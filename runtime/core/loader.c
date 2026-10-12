/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NSO/NRO loading. The code segment has to match what we recompiled byte for byte. */
#include "core/loader.h"
#include "core/main.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <string.h>

u64 g_code_lo, g_code_hi;

/* every read is checked against slen, every write against dlen: a damaged file fails instead of reading past it */
static int lz4_dec(const u8* src, u32 slen, u8* dst, u32 dlen) {
    u32 i = 0, o = 0;
    while (i < slen) {
        u8 tok = src[i++];
        u32 l = tok >> 4;
        if (l == 15) {
            u8 b;
            do {
                if (i >= slen) return -1;
                b = src[i++];
                l += b;
            } while (b == 255);
        }
        if (l > dlen - o || l > slen - i) return -1;
        memcpy(dst + o, src + i, l);
        i += l;
        o += l;
        if (i >= slen) break;
        if (slen - i < 2) return -1;
        u32 off = src[i] | (src[i + 1] << 8);
        i += 2;
        u32 m = tok & 15;
        if (m == 15) {
            u8 b;
            do {
                if (i >= slen) return -1;
                b = src[i++];
                m += b;
            } while (b == 255);
        }
        m += 4;
        if (!off || off > o || m > dlen - o) return -1;
        for (u32 k = 0; k < m; k++) {
            dst[o] = dst[o - off];
            o++;
        }
    }
    return (int)o;
}

static int load_one(const SwrModInfo* mi) {
    char p[1200];
    /* mapped, not read: the NSOs are tens of MB and only copied out once. The mapping ends at the file's end, so every
       read below is checked against fsz */
    PFileMap fm;
    int found = 0;
    const char* dirs[] = {"exefs/", ""};
    for (int k = 0; k < 2 && !found; k++) {
        snprintf(p, sizeof p, "%s%s%s", g_datadir, dirs[k], mi->name);
        found = !plat_file_map(&fm, p);
        if (!found && mi->is_nro) {
            snprintf(p, sizeof p, "%s%s%s.nro", g_datadir, dirs[k], mi->name);
            found = !plat_file_map(&fm, p);
        }
    }
    if (!found) {
        WARN("Cannot find module %s in %sexefs/\n", mi->name, g_datadir);
        return -1;
    }
    const u8* f = fm.data;
    u64 fsz = fm.size;
    u64 size = mi->size;
    if (mem_map(mi->base, size, MS_CODE, PERM_RW)) {
        WARN("could not map %s at %llx\n", mi->name, (unsigned long long)mi->base);
        plat_file_unmap(&fm);
        return -1;
    }
    u8* dst = GP(mi->base);
    if (fsz >= 0x100 && !memcmp(f, "NSO0", 4)) {
        u32 flags;
        memcpy(&flags, f + 0xC, 4);
        u32 seg[3][3];
        for (int i = 0; i < 3; i++) memcpy(seg[i], f + 0x10 + i * 0x10, 12);
        u32 fs[3];
        memcpy(fs, f + 0x60, 12);
        if (memcmp(f + 0x40, mi->build_id, 32)) {
            WARN("WARNING: module %s is not the one that was recompiled (different build id).\n", mi->name);
            plat_file_unmap(&fm);
            return -2;
        }
        for (int i = 0; i < 3; i++) { /* each segment inside the file, and inside the module's mapping */
            u32 stored = (flags & (1u << i)) ? fs[i] : seg[i][2];
            if (seg[i][0] > fsz || stored > fsz - seg[i][0] || seg[i][1] > size || seg[i][2] > size - seg[i][1]) {
                WARN("%s: segment %d lies outside the file or the module\n", mi->name, i);
                plat_file_unmap(&fm);
                return -1;
            }
            if (flags & (1u << i)) {
                if (lz4_dec(f + seg[i][0], fs[i], dst + seg[i][1], seg[i][2]) != (int)seg[i][2]) {
                    WARN("LZ4 failed in %s\n", mi->name);
                    plat_file_unmap(&fm);
                    return -1;
                }
            } else
                memcpy(dst + seg[i][1], f + seg[i][0], seg[i][2]);
        }
    } else if (fsz > 0x20 && !memcmp(f + 0x10, "NRO0", 4)) {
        u32 isz;
        memcpy(&isz, f + 0x18, 4);
        u64 n = isz < fsz ? isz : fsz;
        memcpy(dst, f, n < size ? n : size);
    } else {
        WARN("%s: unknown format\n", p);
        plat_file_unmap(&fm);
        return -1;
    }
    plat_file_unmap(&fm);
    /* .text RX, .rodata R, .data/.bss RW */
    u64 t0 = mi->base + mi->text_off, r0 = mi->base + mi->ro_off, d0 = mi->base + mi->data_off;
    mem_set_state(t0, ((mi->text_size + 0xFFF) & ~0xFFFu), MS_CODE, PERM_RX);
    mem_set_state(r0, ((mi->ro_size + 0xFFF) & ~0xFFFu), MS_CODE, PERM_R);
    mem_set_state(d0, size - (d0 - mi->base), MS_CODEDATA, PERM_RW);
    LOG("module %-8s at %010llx (%.1f MB)\n", mi->name, (unsigned long long)mi->base, (double)size / 1048576.0);
    return 0;
}

int loader_load(void) {
    for (u32 i = 0; i < g_mod->nmods; i++)
        if (load_one(&g_mod->mods[i])) return -1;
    return 0;
}
