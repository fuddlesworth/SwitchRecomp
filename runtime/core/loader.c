/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NSO/NRO loading. The code segment has to match what we recompiled byte for byte. */
#include "core/loader.h"
#include "core/main.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 g_code_lo, g_code_hi;

static int lz4_dec(const u8* src, u32 slen, u8* dst, u32 dlen) {
    u32 i = 0, o = 0;
    while (i < slen) {
        u8 tok = src[i++];
        u32 l = tok >> 4;
        if (l == 15) {
            u8 b;
            do {
                b = src[i++];
                l += b;
            } while (b == 255);
        }
        if (o + l > dlen || i + l > slen) return -1;
        memcpy(dst + o, src + i, l);
        i += l;
        o += l;
        if (i >= slen) break;
        u32 off = src[i] | (src[i + 1] << 8);
        i += 2;
        u32 m = tok & 15;
        if (m == 15) {
            u8 b;
            do {
                b = src[i++];
                m += b;
            } while (b == 255);
        }
        m += 4;
        if (off > o || o + m > dlen) return -1;
        for (u32 k = 0; k < m; k++) {
            dst[o] = dst[o - off];
            o++;
        }
    }
    return (int)o;
}

static u8* read_all(const char* path, u64* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* b = malloc(n > 0 ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *size = (u64)n;
    return b;
}

static int load_one(const SwrModInfo* mi) {
    char p[1200];
    u64 fsz = 0;
    u8* f = NULL;
    const char* dirs[] = {"exefs/", ""};
    for (int k = 0; k < 2 && !f; k++) {
        snprintf(p, sizeof p, "%s%s%s", g_datadir, dirs[k], mi->name);
        f = read_all(p, &fsz);
        if (!f && mi->is_nro) {
            snprintf(p, sizeof p, "%s%s%s.nro", g_datadir, dirs[k], mi->name);
            f = read_all(p, &fsz);
        }
    }
    if (!f) {
        WARN("Cannot find module %s in %sexefs/\n", mi->name, g_datadir);
        return -1;
    }
    u64 size = mi->size;
    if (mem_map(mi->base, size, MS_CODE, PERM_RW)) {
        WARN("could not map %s at %llx\n", mi->name, (unsigned long long)mi->base);
        return -1;
    }
    u8* dst = GP(mi->base);
    if (!memcmp(f, "NSO0", 4)) {
        u32 flags;
        memcpy(&flags, f + 0xC, 4);
        u32 seg[3][3];
        for (int i = 0; i < 3; i++) memcpy(seg[i], f + 0x10 + i * 0x10, 12);
        u32 fs[3];
        memcpy(fs, f + 0x60, 12);
        if (memcmp(f + 0x40, mi->build_id, 32)) {
            WARN("WARNING: module %s is not the one that was recompiled (different build id).\n", mi->name);
            free(f);
            return -2;
        }
        for (int i = 0; i < 3; i++) {
            if (flags & (1u << i)) {
                if (lz4_dec(f + seg[i][0], fs[i], dst + seg[i][1], seg[i][2]) != (int)seg[i][2]) {
                    WARN("LZ4 failed in %s\n", mi->name);
                    free(f);
                    return -1;
                }
            } else
                memcpy(dst + seg[i][1], f + seg[i][0], seg[i][2]);
        }
    } else if (fsz > 0x20 && !memcmp(f + 0x10, "NRO0", 4)) {
        u32 isz;
        memcpy(&isz, f + 0x18, 4);
        memcpy(dst, f, isz < fsz ? isz : fsz);
    } else {
        WARN("%s: unknown format\n", p);
        free(f);
        return -1;
    }
    free(f);
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
