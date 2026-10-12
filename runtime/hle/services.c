/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the events the services share, and the registration of the services in sm.c, am.c, set.c, time.c, acc.c,
   audio.c and misc.c */
#include "hle/services_int.h"

/* ================================================================ shared events */
u32 ev_handle(KEvent* e) { return h_new(&e->o); }
int g_hle_docked, g_hle_lang;

/* the service keeps these events for the whole run: its reference keeps them alive when the game closes the
   handles it was given */
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}

void svc_core_init(void) {
    g_hle_docked = g_opt_docked;
    g_hle_lang = g_opt_lang;
    am_init();
    audio_init();
    time_init();
    plat_thread(audio_clock, NULL, 64 * 1024);
    svc_register("sm:", new_sm);
    svc_register("appletOE", new_appletoe);
    svc_register("appletAE", new_appletae);
    svc_register("set", new_set);
    svc_register("set:sys", new_setsys);
    svc_register("time:u", new_time);
    svc_register("time:a", new_time);
    svc_register("time:s", new_time);
    svc_register("acc:u0", new_acc);
    svc_register("acc:u1", new_acc);
    svc_register("acc:su", new_acc);
    svc_register("apm", new_apm);
    svc_register("apm:p", new_apm);
    svc_register("apm:am", new_apm);
    svc_register("lm", new_lm);
    svc_register("pctl", new_pctl);
    svc_register("pctl:a", new_pctl);
    svc_register("pctl:s", new_pctl);
    svc_register("pctl:r", new_pctl);
    svc_register("nifm:u", new_nifm);
    svc_register("nifm:a", new_nifm);
    svc_register("nifm:s", new_nifm);
    svc_register("friend:u", new_friend);
    svc_register("friend:a", new_friend);
    svc_register("friend:v", new_friend);
    svc_register("ns:am2", new_ns);
    svc_register("ns:ec", new_ns);
    svc_register("ns:rid", new_ns);
    svc_register("ns:rt", new_ns);
    svc_register("ns:web", new_ns);
    svc_register("ns:ro", new_ns);
    svc_register("audren:u", new_audren);
    svc_register("audout:u", new_audout);
    svc_register("audin:u", new_audin);
    svc_register("hwopus", new_hwopus);
    svc_register("bsd:u", new_bsd);
    svc_register("bsd:s", new_bsd);
    svc_register("ssl", new_ssl);
    svc_register("prepo:u", new_prepo);
    svc_register("prepo:a", new_prepo);
    svc_register("prepo:a2", new_prepo);
    svc_register("psm", new_psm);
    svc_register("fatal:u", new_fatal);
    svc_register("csrng", new_csrng);
    svc_register("mm:u", new_mm);
    svc_register("nfp:user", new_nfp);
    svc_register("nfc:user", new_nfp);
    svc_register("caps:u", new_caps);
    svc_register("caps:a", new_caps);
    svc_register("caps:su", new_caps);
    svc_register("caps:ss", new_caps);
    svc_register("bcat:u", new_bcat);
    svc_register("bcat:a", new_bcat);
    svc_register("aoc:u", new_aoc);
    svc_register("irs", new_irs);
    svc_register("mii:e", new_mii);
    svc_register("mii:u", new_mii);
    svc_register("ldn:u", new_ldn);
    svc_register("ectx:aw", new_ectx);
    svc_register("spl:", new_spl);
    svc_register("spl:mig", new_spl);
}
