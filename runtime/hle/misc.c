/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the smaller services, most of which answer success: apm, lm, pctl, nifm, friend, ns, bsd, ssl, prepo, psm, fatal, csrng, mm, nfp, caps, bcat, aoc, irs, mii, ldn, ectx, spl */
#include "hle/services_int.h"
/* ================================================================ apm and misc */
static int apm_session(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 1) out32(c, 0x00020003);
    return 0;
}
static int apm_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out_obj(c, svcobj_new("apm:ISession", apm_session, 0)); return 0;
        case 1: out32(c, (u32)g_opt_docked); return 0;
        case 6: out8(c, 0); return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_apm(void) { return svcobj_new("apm", apm_disp, 0); }
static int lm_logger(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0) {
        u64 a, s;
        buf_in(c, 0, &a, &s);
        if (a && s > 0x18) { /* lm packet: 0x18 hdr, then (type,len,data) fields */
            u8* p = GP(a);
            u64 i = 0x18;
            char msg[1024];
            int n = 0;
            while (i + 2 < s) {
                u8 t = p[i], l = p[i + 1];
                if (i + 2 + l > s) break;
                if (t == 2 && n + l < 1000) {
                    memcpy(msg + n, p + i + 2, l);
                    n += l;
                }
                i += 2 + l;
            }
            if (n) {
                msg[n] = 0;
                LOG("[game lm] %s%s", msg, (msg[n - 1] == '\n') ? "" : "\n");
            }
        }
    }
    return 0;
}
static int lm_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, svcobj_new("ILogger", lm_logger, 0));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_lm(void) { return svcobj_new("lm", lm_disp, 0); }

static int pctl_obj(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 1001:
        case 1002:
        case 1004: return 0;
        case 1031: out8(c, 0); return 0;
        case 1032: out32(c, 0); return 0;
        case 1403: out8(c, 0); return 0;
        case 1018: out8(c, 1); return 0; /* IsFreeCommunicationAvailable */
        case 1039: out32(c, 0); return 0;
        case 1065: out8(c, 1); return 0; /* IsStereoVisionPermitted */
        case 1453: out8(c, 0); return 0; /* IsPlayTimerEnabled */
        case 1473: out8(c, 0); return 0;
        case 1432:
        case 1433:
        case 1434:
        case 1457: out_copy_h(c, ev_handle(kevent_new("pctl ev", 0))); return 0;
    }
    return 0;
}
static int pctl_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0 || c->cmd == 1) {
        out_obj(c, svcobj_new("IParentalControlService", pctl_obj, 0));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_pctl(void) { return svcobj_new("pctl", pctl_disp, 0); }

static int nifm_req(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: out32(c, 1); return 0;
        case 1: return RC(110, 300);
        case 2: {
            KEvent* a = kevent_new("nifm0", 0);
            KEvent* b = kevent_new("nifm1", 0);
            a->signaled = 1;
            out_copy_h(c, ev_handle(a));
            out_copy_h(c, ev_handle(b));
            return 0;
        }
        case 3:
        case 4:
        case 5:
        case 6:
        case 11:
        case 21: return 0;
    }
    return 0;
}
static int nifm_gen(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 4: out_obj(c, svcobj_new("IRequest", nifm_req, 0)); return 0;
        case 2: out_obj(c, svcobj_new("IScanRequest", nifm_req, 0)); return 0;
        case 5:
        case 12: return RC(110, 300);
        case 18: out32(c, 0); return 0;
        case 21: out8(c, 0); return 0;
        case 22: out8(c, 0); return 0;
        case 1: outbytes(c, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10", 16); return 0;
    }
    return 0;
}
static int nifm_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 4 || c->cmd == 5) {
        out_obj(c, svcobj_new("IGeneralService", nifm_gen, 0));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_nifm(void) { return svcobj_new("nifm", nifm_disp, 0); }

static int friend_svc(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0) {
        out_copy_h(c, ev_handle(kevent_new("friend", 0)));
        return 0;
    }
    if (c->cmd >= 10100 && c->cmd <= 10102) {
        out32(c, 0);
        return 0;
    }
    return 0;
}
static int friend_notif(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0) {
        out_copy_h(c, ev_handle(kevent_new("friend notif", 0)));
        return 0;
    }
    if (c->cmd == 2) return RC(121, 15);
    return 0;
}
static int friend_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, svcobj_new("IFriendService", friend_svc, 0));
        return 0;
    }
    if (c->cmd == 1) {
        out_obj(c, svcobj_new("INotificationService", friend_notif, 0));
        return 0;
    }
    if (c->cmd == 2) {
        out_obj(c, quiet("IDaemonSuspendSessionService"));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_friend(void) { return svcobj_new("friend", friend_disp, 0); }
static int ns_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 7996:
        case 7989:
        case 7995:
        case 7988:
        case 7994:
        case 7997:
        case 7998:
        case 7999:
        case 7991: out_obj(c, quiet("ns:sub")); return 0;
    }
    return 0;
}
SvcObj* new_ns(void) { return svcobj_new("ns", ns_disp, 0); }

static int bsd_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: out32(c, 0); return 0;
        case 1: return 0;
        case 2:
            out32(c, (u32)-1);
            out32(c, 106);
            return 0;
    }
    out32(c, (u32)-1);
    out32(c, 106);
    return 0;
}
SvcObj* new_bsd(void) { return svcobj_new("bsd", bsd_disp, 0); }
static int ssl_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, quiet("ISslContext"));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_ssl(void) { return svcobj_new("ssl", ssl_disp, 0); }
static int prepo_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
}
SvcObj* new_prepo(void) { return svcobj_new("prepo", prepo_disp, 0); }
static int psm_session(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0) {
        out_copy_h(c, ev_handle(kevent_new("psm", 0)));
        return 0;
    }
    return 0;
}
static int psm_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: out32(c, 100); return 0;
        case 1: out32(c, 1); return 0;
        case 7: out_obj(c, svcobj_new("IPsmSession", psm_session, 0)); return 0;
        case 4:
        case 5: out8(c, 0); return 0;
    }
    return 0;
}
SvcObj* new_psm(void) { return svcobj_new("psm", psm_disp, 0); }
static int fatal_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    fatal("The game called fatal:u (code %x)", in32(c, 0));
    return 0;
}
SvcObj* new_fatal(void) { return svcobj_new("fatal:u", fatal_disp, 0); }
static int csrng_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0) {
        u64 a, s;
        buf_out(c, 0, &a, &s);
        static u64 x = 0x9E3779B97F4A7C15ull;
        for (u64 i = 0; i < s; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            GP(a)[i] = (u8)x;
        }
    }
    return 0;
}
SvcObj* new_csrng(void) { return svcobj_new("csrng", csrng_disp, 0); }
/* mm:u (multimedia clock requests): 4 Initialize -> id, 6 SetAndWait(id,min,max), 7 Get(id) -> rate.
   Get has to return what SetAndWait asked for: NvMM reads the decoder clock back */
static u32 g_mm_rate[16];
static int mm_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 4) {
        out32(c, 1);
        return 0;
    }
    if (c->cmd == 6) {
        u32 id = in32(c, 0), mn = in32(c, 4);
        g_mm_rate[id & 15] = mn;
        return 0;
    }
    if (c->cmd == 7) {
        out32(c, g_mm_rate[in32(c, 0) & 15]);
        return 0;
    }
    return 0;
}
SvcObj* new_mm(void) { return svcobj_new("mm:u", mm_disp, 0); }
static int nfp_user(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 1: out32(c, 0); return 0;
        case 17:
        case 18: out_copy_h(c, ev_handle(kevent_new("nfp", 0))); return 0;
        case 19: out32(c, 1); return 0;
    }
    return 0;
}
static int nfp_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, svcobj_new("IUser", nfp_user, 0));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_nfp(void) { return svcobj_new("nfp:user", nfp_disp, 0); }
static int caps_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
}
SvcObj* new_caps(void) { return svcobj_new("caps", caps_disp, 0); }
/* BCAT. behave like an offline console: sync completes immediately (Done/OK) and the delivery cache
   is empty. games do wait on the progress object, so it has to exist */
static int bcat_progress(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: {
            KEvent* e = kevent_new("bcat progress", 0);
            e->signaled = 1;
            out_copy_h(c, ev_handle(e));
            return 0;
        } /* GetEvent */
        case 1: {
            u64 a, sz;
            buf_out(c, 0, &a, &sz);
            if (a && sz) {
                memset(GP(a), 0, sz);
                if (sz >= 8) {
                    u32 st = 9, rs = 0;
                    memcpy(GP(a), &st, 4);
                    memcpy(GP(a) + 4, &rs, 4);
                }
            }
            return 0;
        } /* GetImpl: Done/OK */
    }
    return 0;
}
static int bcat_dir(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: return 0; /* Open */
        case 1:
        case 2: out32(c, 0); return 0;
    } /* Read / GetCount: no files */
    return 0;
}
static int bcat_file(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: return RC(122, 2); /* Open: not found */
        case 1: out64(c, 0); return 0;
        case 2: out64(c, 0); return 0;
        case 3: {
            u8 d[0x10];
            memset(d, 0, sizeof d);
            outbytes(c, d, 0x10);
            return 0;
        }
    }
    return 0;
}
static int bcat_storage(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: out_obj(c, svcobj_new("IDeliveryCacheFileService", bcat_file, 0)); return 0;
        case 1: out_obj(c, svcobj_new("IDeliveryCacheDirectoryService", bcat_dir, 0)); return 0;
        case 10: out32(c, 0); return 0;
    } /* EnumerateDeliveryCacheDirectory: none */
    return 0;
}
static int bcat_service(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 10100:
        case 10101:
        case 20100:
        case 20101:
        case 20300:
        case 20301: out_obj(c, svcobj_new("IDeliveryCacheProgressService", bcat_progress, 0)); return 0;
    }
    return 0;
}
static int bcat_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: out_obj(c, svcobj_new("IBcatService", bcat_service, 0)); return 0;
        case 1:
        case 2: out_obj(c, svcobj_new("IDeliveryCacheStorageService", bcat_storage, 0)); return 0;
    }
    return 0;
}
SvcObj* new_bcat(void) { return svcobj_new("bcat", bcat_disp, 0); }
static int aoc_purchase(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 2) {
        out_copy_h(c, ev_handle(kevent_new("aoc purchase", 0)));
        return 0;
    }
    if (c->cmd == 3 || c->cmd == 4) return RC(164, 400);
    return 0;
}
static int aoc_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0:
        case 1:
        case 2:
        case 3: out32(c, 0); return 0; /* Count/List: no dlc */
        case 5:
        case 6: out64(c, (g_title_id & ~0xFFFull) + 0x1000); return 0; /* GetAddOnContentBaseId */
        case 7:
        case 9:
        case 11:
        case 12:
        case 13:
        case 50: return 0;
        case 8:
        case 10: out_copy_h(c, ev_handle(kevent_new("aoc", 0))); return 0; /* GetAddOnContentListChangedEvent */
        case 100:
        case 101: out_obj(c, svcobj_new("IPurchaseEventManager", aoc_purchase, 0)); return 0;
    }
    return 0;
}
SvcObj* new_aoc(void) { return svcobj_new("aoc:u", aoc_disp, 0); }
static int irs_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 303 || c->cmd == 304) {
        KShmem* s = kshmem_new(0x8000, "irs");
        out_copy_h(c, h_new(&s->o));
        return 0;
    }
    if (c->cmd == 311) {
        out32(c, 0);
        return 0;
    }
    return 0;
}
SvcObj* new_irs(void) { return svcobj_new("irs", irs_disp, 0); }
static int mii_db(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0 || c->cmd == 2 || c->cmd == 3) out32(c, 0);
    return 0;
}
static int mii_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, svcobj_new("IDatabaseService", mii_db, 0));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_mii(void) { return svcobj_new("mii:u", mii_disp, 0); }
static int ldn_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, quiet("IUserLocalCommunicationService"));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_ldn(void) { return svcobj_new("ldn:u", ldn_disp, 0); }
static int ectx_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
}
SvcObj* new_ectx(void) { return svcobj_new("ectx:aw", ectx_disp, 0); }
static int spl_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0) {
        out64(c, 0);
        return 0;
    }
    if (c->cmd == 7) {
        u64 a, s;
        buf_out(c, 0, &a, &s);
        memset(GP(a), 0x5A, s);
    }
    return 0;
}
SvcObj* new_spl(void) { return svcobj_new("spl:", spl_disp, 0); }
