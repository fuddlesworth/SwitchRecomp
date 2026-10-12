/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the service files in hle share (services.c registers them all) */
#ifndef SWR_SERVICES_INT_H
#define SWR_SERVICES_INT_H
#include "hle/services.h"
#include "core/main.h"
#include "core/options.h"
#include "kernel/ipc.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* services.c */
u32 ev_handle(KEvent* e);
/* the docked and language options as they were at startup: the .ini says they apply on restart, and ini_poll
   reloads the globals while the game runs */
extern int g_hle_docked, g_hle_lang;

/* sm.c */
SvcObj* new_sm(void);
int quiet_ok(SvcObj* o, IpcCtx* c);
SvcObj* quiet(const char* name);

/* am.c */
extern KEvent* g_msg_event;
extern KEvent* g_launch_ev;
extern KEvent* g_res_ev;
extern KEvent* g_sleep_ev;
extern u64 g_user_id[2];
void am_push_msg(u32 m);
SvcObj* new_appletoe(void);
SvcObj* new_appletae(void);

/* set.c */
u64 set_language_code(int lang); /* the LanguageCode of a language index, en-US for one out of range */
SvcObj* new_set(void);
SvcObj* new_setsys(void);

/* time.c */
void time_init(void);
SvcObj* new_time(void);

/* acc.c */
SvcObj* new_acc(void);

/* audio.c */
extern KEvent* g_aud_ev;
void audio_clock(void* arg);
SvcObj* new_audren(void);
SvcObj* new_audout(void);
SvcObj* new_hwopus(void);

/* misc.c */
SvcObj* new_apm(void);
SvcObj* new_lm(void);
SvcObj* new_pctl(void);
SvcObj* new_nifm(void);
SvcObj* new_friend(void);
SvcObj* new_ns(void);
SvcObj* new_bsd(void);
SvcObj* new_ssl(void);
SvcObj* new_prepo(void);
SvcObj* new_psm(void);
SvcObj* new_fatal(void);
SvcObj* new_csrng(void);
SvcObj* new_mm(void);
SvcObj* new_nfp(void);
SvcObj* new_caps(void);
SvcObj* new_bcat(void);
SvcObj* new_aoc(void);
SvcObj* new_irs(void);
SvcObj* new_mii(void);
SvcObj* new_ldn(void);
SvcObj* new_ectx(void);
SvcObj* new_spl(void);
SvcObj* new_audin(void);

/* audren.c */
int audren_update(SvcObj* o, IpcCtx* c);

#endif
