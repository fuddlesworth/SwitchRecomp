/* Picking a renderer, when the runtime has both: the window backend asks renderer_wants_vulkan at startup, sets
   g_renderer (gpu/renderer.h) and falls back to OpenGL when Vulkan can't start. */
#include <string.h>
#include "gpu/gl/lsfg.h"
#include "gpu/gpu.h"
#include "platform/log.h"
#include "gpu/renderer.h"

static int g_pref; /* renderer=: 0 auto, 1 vulkan, 2 opengl */

int renderer_ini(const char* k, const char* v) {
    if (strcmp(k, "renderer")) return 0;
    g_pref = !strcmp(v, "vulkan") ? 1 : !strcmp(v, "opengl") ? 2 : 0;
    return 1;
}
int renderer_wants_vulkan(void) {
    if (g_pref) return g_pref == 1;
    /* LSFG frame generation runs on OpenGL only: with the user's Lossless.dll set up, auto keeps it */
    if (lsfg_available()) {
        LOG("renderer: OpenGL, for LSFG frame generation (renderer=vulkan uses Vulkan without it)\n");
        return 0;
    }
    return 1;
}
