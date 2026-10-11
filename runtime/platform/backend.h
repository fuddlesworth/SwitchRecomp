/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* what a window backend (platform/backend/) provides: the window, input, presenting and the GL context */
#ifndef SWR_BACKEND_H
#define SWR_BACKEND_H
#include "abi/cpu.h"

void video_init(const char* title);
void video_present_rgba(const u32* px, int w, int h, int stride); /* final RGBA8 image */
void video_poll(void);
int video_has_gl(void);
void backend_ini(const char* k, const char* v);
void frontend_frame(void);
void backend_window_size(int* w, int* h);
/* the GL context: created on the GL thread, shared with the shader workers */
int backend_gl_create(void);
void* backend_gl_proc(const char* n);
void backend_gl_swap(void);
int backend_gl_worker_current(void);
int backend_glq_ok(void); /* GL calls can go through the GL thread (glq) */
/* the on-screen state of the recorder and LSFG (Windows title bar, SDL3) */
extern volatile int g_rec_toggle;
void backend_rec_state(int on);
void backend_lsfg_state(int on, int mult);
void win_fatal_box(const char* msg);
#if defined(__ANDROID__)
void android_log(const char* s); /* to logcat */
#endif

#endif
