/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the compute engine's launches, and inline-to-memory uploads */
#include "gpu/engines_int.h"
/* ============================================================ compute + I2M */
volatile u64 g_compute_launches;
void compute_method(GpuChannel* ch, u32 m, u32 v, int last) {
    (void)last;
    m &= 0xFFF;
    ch->rcomp[m] = v;
    switch (m) {
        case 0x6C: i2m_launch(ch, ch->rcomp, 0x60); break;
        case 0x6D: i2m_data(ch, v); break;
        case 0xAF: /* launch; the first few programs get dumped for debugging */ {
            g_compute_launches++;
            u64 qmd = (u64)ch->rcomp[0xAD] << 8;
            u64 code = ((u64)ch->rcomp[0x582] << 32) | ch->rcomp[0x583];
            u32 q[64];
            gmem_read(qmd, q, 256);
            if (g_renderer_up) { g_renderer->dispatch(ch, q, code); }
            u64 prog = code + q[8];
            static u64 seen[32];
            static int ns;
            int known = 0;
            for (int i = 0; i < ns; i++)
                if (seen[i] == prog) known = 1;
            if (!known && ns < 32 && g_opt_diag) {
                seen[ns++] = prog;
                char p[1200]; /* captures/ next to the games, like the rest of what the runtime writes */
                snprintf(p, sizeof p, "%scaptures", g_exedir);
                plat_mkdir(p);
                snprintf(p, sizeof p, "%scaptures/compute", g_exedir);
                plat_mkdir(p);
                snprintf(p, sizeof p, "%scaptures/compute/c%02d_qmd.bin", g_exedir, ns);
                FILE* f = fopen(p, "wb");
                if (f) {
                    fwrite(q, 1, 256, f);
                    fclose(f);
                }
                u8* b = malloc(0x10000);
                gmem_read_code(prog, b, 0x10000);
                snprintf(p, sizeof p, "%scaptures/compute/c%02d_cs.bin", g_exedir, ns);
                f = fopen(p, "wb");
                if (f) {
                    fwrite(b, 1, 0x10000, f);
                    fclose(f);
                }
                free(b);
                for (int i = 0; i < 8; i++)
                    if ((q[20] >> i) & 1) {
                        u64 a = ((u64)(q[30 + 2 * i] & 0xFF) << 32) | q[29 + 2 * i];
                        u32 sz = (q[30 + 2 * i] >> 15) & 0x1FFFF;
                        if (!sz || sz > 0x10000) sz = 0x10000;
                        u8* cb = malloc(sz);
                        gmem_read(a, cb, sz);
                        snprintf(p, sizeof p, "%scaptures/compute/c%02d_cb%d.bin", g_exedir, ns, i);
                        f = fopen(p, "wb");
                        if (f) {
                            fwrite(cb, 1, sz, f);
                            fclose(f);
                        }
                        free(cb);
                    }
                LOG("compute %d: grid %ux%ux%u, block %ux%ux%u, shared %u bytes, cbufs %02x (saved in captures/compute)\n",
                    ns, q[12] & 0x7FFFFFFF, q[13] & 0xFFFF, q[13] >> 16, q[18] >> 16, q[19] & 0xFFFF, q[19] >> 16,
                    q[17] & 0x3FFFF, q[20] & 0xFF);
            }
        } break;
        case 0x6C3: query_get(ch, ch->rcomp, 0x6C0, v); break; /* release the semaphore, the game may be waiting */
    }
}
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v) {
    m &= 0x7F;
    ch->ri2m[m] = v;
    if (m == 0x6C)
        i2m_launch(ch, ch->ri2m, 0x60);
    else if (m == 0x6D)
        i2m_data(ch, v);
}
