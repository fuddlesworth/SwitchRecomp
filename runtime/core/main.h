/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* startup and the runtime-wide state main.c owns, the crash report (crash.c) and vi's CPU-side present (present.c) */
#ifndef SWR_MAIN_H
#define SWR_MAIN_H
#include "abi/cpu.h"

int swr_main(int argc, char** argv);
extern char g_datadir[1024], g_exename[256], g_exedir[1024];
extern volatile int g_quit;
extern int g_frames;
void crash_report(void* host_pc, void* fault, int sig);
/* vi's queued framebuffers (present.c): shown by the renderer, or converted to RGBA on the CPU when there is none */
void present_init(void);
void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt);
void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out);

#endif
