/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the unattended-run options both window backends take (unattended.c): seconds, frames, shot, shot_every, press,
   press_at, press_len, script. Also the stick handling the backends share */
#ifndef SWR_UNATTENDED_H
#define SWR_UNATTENDED_H
#include "abi/cpu.h"
#include <stddef.h>

int unattended_ini(const char* k, const char* v); /* 1 when k is one of the options */
void unattended_start(void);                      /* time zero for seconds, press_at and the script */
int unattended_active(void);                      /* seconds or frames is set: nobody is at the screen */
/* the buttons the options hold now: HidNpadButton bits, and 40..47 for the stick directions (as keys, see
   input_key_sticks) */
u64 unattended_buttons(void);
const char* unattended_shot(void); /* the shot path, NULL when unset */
/* shot_every: 1 when a numbered screenshot is due, with its path (no extension) in path */
int unattended_periodic_shot(char* path, size_t n);
int unattended_done(void); /* the frames or seconds are reached */
/* the result line on stderr (w and h: the image's size, 0 when unknown), then the hints, the renderer's save (its
   pipeline cache: the process ends without shutting the renderer down) and the exit */
__attribute__((noreturn)) void unattended_exit(u64 hash, int w, int h);

/* stick directions held as keys (bits 40..47: left stick left, up, right, down, then the right stick) as axes:
   st = {lx, ly, rx, ry} */
void input_key_sticks(u64 k, i32 st[4]);
/* a stick as the console reports it: each axis within -32767..32767, and the pair within the circle of that radius
   (a keyboard diagonal or a square gate goes past it) */
void input_stick_clamp(i32* x, i32* y);

#endif
