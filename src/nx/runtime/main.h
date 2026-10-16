/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* startup and the runtime-wide state main.c owns, the crash report (crash.c), and vi's present and the frame pacing
   (present.c) */
#ifndef SNX_MAIN_H
#define SNX_MAIN_H
#include "cpu/aarch64/cpu.h"

int snx_main(int argc, char** argv);
extern char g_datadir[1024], g_exename[256], g_exedir[1024];
/* where the game's settings, its log and threads.txt, and its caches go: g_exedir, or the XDG folders of a game in
   an installed build's data folder */
extern char g_confdir[1024], g_statedir[1024], g_cachedir[1024];
extern volatile int g_quit;
extern int g_frames;
void crash_report(void* host_pc, void* fault, int sig);
/* vi's queued framebuffers (present.c): each shown by the renderer once the GPU has drawn it (the GPU thread) */
void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform);
/* the wait before the game is handed a buffer for its next frame: until the last frame's turn on the display is over,
   as fps_limit says, the swap interval it was queued with (60 Hz vsyncs) with fps_limit=game */
void present_pace(u32 interval);
void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out);

#endif
