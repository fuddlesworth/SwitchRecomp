/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* vi's framebuffers: handed to the renderer, or converted to RGBA on the CPU when there is none */
#include "core/clock.h"
#include "core/main.h"
#include "core/options.h"
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "hle/nvdrv.h"
#include "kernel/mem.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>

static u32* g_frame;
static int g_fw, g_fh;
static PMutex g_flock;
static u64 g_last_present;
void present_init(void) { pm_init(&g_flock); }
void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out) {
    u32 bpp = (fmt == 4 || fmt == 6 || fmt == 7) ? 2 : 4;
    u8* lin = malloc((size_t)w * h * bpp);
    if (layout == 3 || layout == 2) {
        deswizzle(lin, w * bpp, GP(a), w * bpp, h, bhl);
    } else {
        for (u32 y = 0; y < h; y++) memcpy(lin + (size_t)y * w * bpp, GP(a + (u64)y * pitch), w * bpp);
    }
    if (bpp == 4) {
        if (fmt == 5) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                u32 p = ((u32*)lin)[i];
                out[i] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
            }
        } else
            memcpy(out, lin, (size_t)w * h * 4);
    } else {
        u16* s = (u16*)lin;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            u32 p = s[i], r, g, b;
            if (fmt == 4) {
                r = (p >> 11) & 31;
                g = (p >> 5) & 63;
                b = p & 31;
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
            } else if (fmt == 6) {
                r = (p >> 11) & 31;
                g = (p >> 6) & 31;
                b = (p >> 1) & 31;
                r = (r << 3) | (r >> 2);
                g = (g << 3) | (g >> 2);
                b = (b << 3) | (b >> 2);
            } else {
                r = ((p >> 12) & 15) * 17;
                g = ((p >> 8) & 15) * 17;
                b = ((p >> 4) & 15) * 17;
            }
            out[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }
    free(lin);
}
void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt,
                    const i32 crop[4]) {
    u64 a = nvmap_addr_by_id(nvmap_id);
    if (!a) {
        WARN("vi: buffer without memory (nvmap %u)\n", nvmap_id);
        return;
    }
    a += offset;
    if (g_renderer_up) {
        g_renderer->present_buffer(a, w, h, pitch, layout, bhl, fmt, crop);
    } else {
        pm_lock(&g_flock);
        if (g_fw != (int)w || g_fh != (int)h) {
            free(g_frame);
            g_frame = malloc((size_t)w * h * 4);
            g_fw = (int)w;
            g_fh = (int)h;
        }
        vi_convert_cpu(a, w, h, pitch, layout, bhl, fmt, g_frame);
        video_present_rgba(g_frame, (int)w, (int)h, (int)w);
        pm_unlock(&g_flock);
    }
    g_frames++;
    /* fps_limit: 1 = 60, otherwise N fps */
    if (g_opt_fps) {
        u64 per = g_opt_fps >= 60 && g_opt_fps <= 240 ? 1000000000ull / (u64)g_opt_fps : 16666667ull;
        if (g_opt_fps >= 10 && g_opt_fps < 60) {
            static int said;
            if (!said) {
                said = 1;
                WARN("fps_limit=%d would run the game at %d%% speed (one game step per image): using 60\n", g_opt_fps,
                     g_opt_fps * 100 / 60);
            }
        }
        u64 now = plat_ns();
        u64 next = g_last_present + per;
        if (g_last_present && now < next) plat_sleep_ns((i64)(next - now));
        now = plat_ns();
        g_last_present = (g_last_present && now - g_last_present < 2 * per) ? next : now;
    }
}
