/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* startup and the runtime-wide state main.c owns */
#ifndef SWR_MAIN_H
#define SWR_MAIN_H
#include "abi/cpu.h"
#include "kernel/ipc.h"

int swr_main(int argc, char** argv);
extern char g_datadir[1024], g_exename[256], g_exedir[1024];
extern volatile int g_quit;
extern int g_frames;
u64 g_t0_ns(void);  /* plat_ns() at startup */
u64 guest_ns(void); /* the guest's clock */
void crash_report(void* host_pc, void* fault, int sig);
void save_hints(void);
void swr_fpcr_apply(CPU* c);
int audren_update(SvcObj* o, IpcCtx* c);
/* vi's queued framebuffers: shown by the renderer, or converted to RGBA on the CPU when there is none */
void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt);
void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out);
/* the guest clock's smooth recording mode (F7): it runs slower than real time so each frame spans 1/recording_fps */
extern volatile int g_vt_on, g_rec_active;
void vt_set(int on);

#endif
