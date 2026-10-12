/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the settings from <Game>.ini (options.c) */
#ifndef SWR_OPTIONS_H
#define SWR_OPTIONS_H

void ini_override(const char* k, const char* v); /* a --key=value from the command line, before ini_open. Keeps v */
void ini_set(const char* k, const char* v);
void ini_open(const char* path); /* watch the file: ini_poll reloads it when it changes, and writes back what changed */
void ini_poll(void);
extern int g_opt_docked, g_opt_lang, g_opt_fps, g_opt_drs;
extern int g_opt_diag, g_opt_shadows, g_opt_prof;
extern float g_opt_resf;

#endif
