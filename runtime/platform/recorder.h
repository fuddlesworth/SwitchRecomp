/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the video recorder (recorder_win32.c, Media Foundation) */
#ifndef SWR_RECORDER_H
#define SWR_RECORDER_H
#include "abi/cpu.h"
#include <wchar.h>

int rec_open(const wchar_t* path, int w, int h, int fps);
void rec_frame(const u8* nv12, i64 t100, i64 dur100);
void rec_close(void);

#endif
