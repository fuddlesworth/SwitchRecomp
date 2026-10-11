/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* lsfg stubs for GLES builds (Android has no frame generation) */
#include "gpu.h"
#include <string.h>
#include <stdlib.h>
int g_lsfg_opt = 0, g_lsfg_quality = 0, g_lsfg_mult = 2, g_lsfg_height = 720;
float g_lsfg_flow = 0.5f, g_lsfg_target = 60.f;
char g_lsfg_dll[1024];
int lsfg_init(void) { return 0; }
int lsfg_ready(void) { return 0; }
int lsfg_context(u32 W, u32 H) {
    (void)W;
    (void)H;
    return 0;
}
unsigned lsfg_in_tex(void) { return 0; }
void lsfg_reset(void) {}
int lsfg_process(int n, unsigned* o) {
    (void)n;
    (void)o;
    return 0;
}
void lsfg_ini(const char* k, const char* v) {
    if (!strcmp(k, "lsfg"))
        g_lsfg_opt = atoi(v);
    else if (!strcmp(k, "lsfg_dll")) {
        strncpy(g_lsfg_dll, v, 1023);
        g_lsfg_dll[1023] = 0;
    } else if (!strcmp(k, "lsfg_flow")) {
        g_lsfg_flow = (float)atof(v);
        if (g_lsfg_flow < 0.25f) g_lsfg_flow = 0.25f;
        if (g_lsfg_flow > 1) g_lsfg_flow = 1;
    } else if (!strcmp(k, "lsfg_multiplier")) {
        g_lsfg_mult = atoi(v);
        if (g_lsfg_mult < 2) g_lsfg_mult = 2;
        if (g_lsfg_mult > 4) g_lsfg_mult = 4;
    } else if (!strcmp(k, "lsfg_resolution")) {
        g_lsfg_height = atoi(v);
        if (g_lsfg_height < 240) g_lsfg_height = 240;
        if (g_lsfg_height > 2160) g_lsfg_height = 2160;
    } else if (!strcmp(k, "lsfg_target")) {
        g_lsfg_target = (float)atof(v);
        if (g_lsfg_target < 30) g_lsfg_target = 30;
    }
}
void lsfg_debug_tex(unsigned* a, unsigned* b, u32* w, u32* h) {
    *a = *b = 0;
    *w = *h = 0;
}
