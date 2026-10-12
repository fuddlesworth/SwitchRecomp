/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* pl: the shared fonts */
#include "core/main.h"
#include "hle/services.h"
#include "kernel/ipc.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pl:u shared fonts. We don't ship Nintendo's: use GAME_data/fonts/<Name>.bfttf|.ttf if there, else a system TTF
   (Arial, Segoe UI or Tahoma on Windows, DejaVu Sans or FreeSans elsewhere). The shared memory holds each font as
   plain TTF behind the 8 byte obfuscated header the SDK reads its size from (a .bfttf is decrypted into it). */
static const char* g_font_names[6] = {
    "FontStandard", "FontChineseSimplified", "FontExtendedChineseSimplified", "FontChineseTraditional",
    "FontKorean",   "FontNintendoExtended"};
static struct {
    u32 font_off[6], font_size[6];
    KShmem* font_shm;
    PMutex font_lock; /* the first request fills the shared memory, the others wait for it */
} pl;
static u8* read_file_all(const char* path, u64* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    u8* d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    *size = (u64)n;
    return d;
}
static u32 bswap32_(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
static u8* font_fallback(u64* size) {
    const char* cands[8];
    int n = 0;
    char w1[512], w2[512], w3[512];
#if defined(_WIN32)
    const char* wd = getenv("WINDIR");
    if (!wd) wd = "C:\\Windows";
    snprintf(w1, sizeof w1, "%s\\Fonts\\arial.ttf", wd);
    snprintf(w2, sizeof w2, "%s\\Fonts\\segoeui.ttf", wd);
    snprintf(w3, sizeof w3, "%s\\Fonts\\tahoma.ttf", wd);
    cands[n++] = w1;
    cands[n++] = w2;
    cands[n++] = w3;
#else
    (void)w1;
    (void)w2;
    (void)w3;
    cands[n++] = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    cands[n++] = "/usr/share/fonts/TTF/DejaVuSans.ttf";
    cands[n++] = "/usr/share/fonts/truetype/freefont/FreeSans.ttf";
#endif
    for (int i = 0; i < n; i++) {
        u8* d = read_file_all(cands[i], size);
        if (d) {
            LOG("pl: system font -> %s\n", cands[i]);
            return d;
        }
    }
    return NULL;
}
static void fonts_fill(u8* shm, u64 cap) {
    const u32 RESULT = 0x7f9a0218u, MAGIC = 0x36f81a1eu;
    u32 key = bswap32_(RESULT ^ MAGIC);
    u64 off = 0;
    u8* fb = NULL;
    u64 fbs = 0;
    u32 fb_off = 0, fb_size = 0;
    for (int i = 0; i < 6; i++) {
        char p[1200];
        u64 sz = 0;
        u8* d = NULL;
        int is_bfttf = 0;
        snprintf(p, sizeof p, "%sfonts/%s.bfttf", g_datadir, g_font_names[i]);
        d = read_file_all(p, &sz);
        if (d) is_bfttf = 1;
        if (!d) {
            snprintf(p, sizeof p, "%sfonts/%s.ttf", g_datadir, g_font_names[i]);
            d = read_file_all(p, &sz);
        }
        if (!d) { /* one fallback font for every slot */
            if (fb_size) {
                pl.font_off[i] = fb_off;
                pl.font_size[i] = fb_size;
                continue;
            }
            if (!fb) fb = font_fallback(&fbs);
            if (!fb) {
                WARN("pl: no TTF font found, so system text will not be shown\n");
                return;
            }
            d = fb;
            sz = fbs;
        } else
            LOG("pl: %s <- %s\n", g_font_names[i], p);
        if (is_bfttf && sz < 8) { /* not even the header */
            WARN("pl: %s is not a BFTTF font\n", p);
            free(d);
            continue;
        }
        u64 words = (sz + 3) / 4;
        if (off + 8 + words * 4 > cap) {
            WARN("pl: font %s does not fit\n", g_font_names[i]);
            if (d != fb) free(d);
            continue;
        }
        u32 h0 = bswap32_(MAGIC), h1 = bswap32_((u32)(words * 4)) ^ key;
        memcpy(shm + off, &h0, 4);
        memcpy(shm + off + 4, &h1, 4);
        if (is_bfttf) { /* bfttf -> decrypt, shmem holds plain ttf */
            u32 w0, w1;
            memcpy(&w0, d, 4);
            memcpy(&w1, d + 4, 4);
            u32 k2 = w0 ^ RESULT;
            u32 tsz = bswap32_(w1 ^ k2);
            if (tsz > sz - 8) tsz = (u32)(sz - 8);
            /* word by word; a last partial word reads only the bytes the file has */
            for (u64 k = 0; k + 8 < sz; k += 4) {
                u32 w = 0;
                memcpy(&w, d + 8 + k, sz - 8 - k < 4 ? sz - 8 - k : 4);
                w = bswap32_(w ^ k2);
                memcpy(shm + off + 8 + k, &w, 4);
            }
            pl.font_off[i] = (u32)off + 8;
            pl.font_size[i] = tsz;
        } else {
            memcpy(shm + off + 8, d, sz);
            pl.font_off[i] = (u32)off + 8;
            pl.font_size[i] = (u32)sz;
        }
        if (d == fb) {
            fb_off = pl.font_off[i];
            fb_size = pl.font_size[i];
        }
        off = (off + 8 + words * 4 + 0xF) & ~0xFull;
        if (d != fb) free(d);
    }
    if (fb) free(fb);
}
static int pl_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&pl.font_lock);
    KShmem* shm = pl.font_shm;
    if (!shm) {
        /* the service keeps it for the whole run: its reference keeps it alive when the game closes its handle */
        shm = kshmem_new(0x1100000, "font");
        kobj_ref(&shm->o);
        fonts_fill(kshmem_ptr(shm), 0x1100000);
        pl.font_shm = shm;
    }
    pm_unlock(&pl.font_lock);
    u32 id = in32(c, 0);
    if (id > 5) id = 0;
    switch (c->cmd) {
        case 0: return 0;                             /* RequestLoad */
        case 1: out32(c, 1); return 0;                /* GetLoadState: loaded */
        case 2: out32(c, pl.font_size[id]); return 0; /* GetSize */
        case 3: out32(c, pl.font_off[id]); return 0;  /* GetSharedMemoryAddressOffset */
        case 4: out_copy_h(c, h_new(&shm->o)); return 0;
        case 5:
        case 6: { /* GetSharedFontInOrderOfPriority */
            u64 a0 = 0, s0 = 0, a1 = 0, s1 = 0, a2 = 0, s2 = 0;
            buf_out(c, 0, &a0, &s0);
            buf_out(c, 1, &a1, &s1);
            buf_out(c, 2, &a2, &s2);
            u32 n = 6;
            for (u32 i = 0; i < n; i++) {
                if (a0 && (i + 1) * 4 <= s0) gw32(a0 + i * 4, i);
                if (a1 && (i + 1) * 4 <= s1) gw32(a1 + i * 4, pl.font_off[i]);
                if (a2 && (i + 1) * 4 <= s2) gw32(a2 + i * 4, pl.font_size[i]);
            }
            out8(c, 1);
            c->out_size = 4;
            out32(c, n);
            return 0;
        }
    }
    return stub_ok(o, c);
}
static SvcObj* new_pl(void) { return svcobj_new("pl:u", pl_disp, 0); }
void svc_pl_init(void) {
    pm_init(&pl.font_lock);
    svc_register("pl:u", new_pl);
    svc_register("pl:s", new_pl);
}
