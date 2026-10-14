/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's side of the Android app (docs/ANDROID.md) */
#ifndef SNX_GUI_ANDROID_H
#define SNX_GUI_ANDROID_H
#include <stddef.h>

/* The app's own files as the working directory and HOME: what the driver extracts and recompiles goes there, where
   code may run, and the keys to its .switch/ */
void gui_android_start(void);
/* The folder of the app's native libraries, where the driver is (libscarletnx-run.so); NULL when unknown */
const char* gui_android_libraries(void);
/* Runs the runtime with argv in the app's game activity, a process of its own; 0 with message when it cannot */
int gui_android_play(const char* const* argv, char* message, size_t size);
/* Text (ASCII) to the share sheet, for the user to send or keep; 0 with message when it cannot */
int gui_android_share(const char* text, char* message, size_t size);
#endif
