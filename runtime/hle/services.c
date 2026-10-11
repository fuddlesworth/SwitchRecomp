/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* sm, am, set, time, acc, apm, plus a bunch of stub services that just say ok (nifm, shop, prepo...) */
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
#include "hle/services_int.h"

/* ================================================================ shared events */
u32 ev_handle(KEvent* e) { return h_new(&e->o); }

void svc_core_init(void) {
    g_msg_event = kevent_new("applet msg", 0);
    g_launch_ev = kevent_new("launchable", 0);
    g_res_ev = kevent_new("res change", 0);
    g_opmode_ev = kevent_new("opmode", 0);
    g_aud_ev = kevent_new("audio renderer", 0);
    plat_thread(audio_clock, NULL, 64 * 1024);
    am_push_msg(0xF);  /* FocusStateChanged */
    am_push_msg(0x1F); /* OperationModeChanged / resume */
    kevent_signal(g_launch_ev);
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
    svc_register("audin:u", new_audout);
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
