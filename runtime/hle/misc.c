/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the smaller services, most of which answer success: apm, lm, pctl, nifm, friend, ns, bsd, ssl, prepo, psm, fatal, csrng, mm, nfp, caps, bcat, aoc, irs, mii, ldn, ectx, spl, audin */
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
        case 1: out32(c, (u32)g_hle_docked); return 0;
        case 6: out8(c, 0); return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_apm(void) { return svcobj_new("apm", apm_disp, 0); }
/* an unsigned LEB128 number of lm's packet fields; 0 past the end */
static u64 uleb128(const u8* p, u64 s, u64* i) {
    u64 v = 0;
    for (int sh = 0; *i < s && sh < 64; sh += 7) {
        u8 b = p[(*i)++];
        v |= (u64)(b & 0x7F) << sh;
        if (!(b & 0x80)) break;
    }
    return v;
}
static int lm_logger(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0) {
        u64 a, s;
        buf_in(c, 0, &a, &s);
        if (a && s > 0x18) { /* lm packet: 0x18 hdr, then fields {uleb128 key; uleb128 size; data} */
            u8* p = GP(a);
            u64 i = 0x18;
            char msg[1024];
            u64 n = 0;
            while (i < s) {
                u64 t = uleb128(p, s, &i), l = uleb128(p, s, &i);
                if (l > s - i) break;
                if (t == 2 && n + l < 1000) { /* the text */
                    memcpy(msg + n, p + i, l);
                    n += l;
                }
                i += l;
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
    switch (c->cmd) {
        case 1001:
        case 1002:
        case 1004: return 0;
        case 1031: out8(c, 0); return 0;
        case 1032: out32(c, 0); return 0;
        case 1403: out8(c, 0); return 0;
        case 1039: out32(c, 0); return 0;
        case 1065: out8(c, 1); return 0; /* IsStereoVisionPermitted */
        case 1453: out8(c, 0); return 0; /* IsPlayTimerEnabled */
        case 1432:
        case 1433:
        case 1434:
        case 1457:
        case 1473: out_copy_h(c, ev_handle(kevent_new("pctl ev", 0))); return 0; /* 1473 GetUnlinkedEvent */
    }
    return stub_ok(o, c);
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
    return stub_ok(o, c);
}
/* IScanRequest: a scan that is over at once and found nothing */
static int nifm_scan(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: return 0;             /* Submit */
        case 1: out8(c, 0); return 0; /* IsProcessing */
        case 2: return 0;             /* GetResult */
        case 3: {                     /* GetSystemEventReadableHandle */
            KEvent* e = kevent_new("nifm scan", 0);
            e->signaled = 1;
            out_copy_h(c, ev_handle(e));
            return 0;
        }
    }
    return stub_ok(o, c);
}
static int nifm_gen(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 4: out_obj(c, svcobj_new("IRequest", nifm_req, 0)); return 0;
        case 2: out_obj(c, svcobj_new("IScanRequest", nifm_scan, 0)); return 0;
        /* no network: the queries about the connection fail like 12 GetCurrentIpAddress does */
        case 5:
        case 12:
        case 15:
        case 18: return RC(110, 300);
        case 21: out8(c, 0); return 0;
        case 22: out8(c, 0); return 0;
        case 1: { /* GetClientId: a u32 in a C buffer */
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a && s >= 4) gw32(a, 1);
            return 0;
        }
    }
    return stub_ok(o, c);
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
    if (c->cmd == 0) {
        out_copy_h(c, ev_handle(kevent_new("friend", 0)));
        return 0;
    }
    if (c->cmd >= 10100 && c->cmd <= 10102) {
        out32(c, 0);
        return 0;
    }
    return stub_ok(o, c);
}
static int friend_notif(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_copy_h(c, ev_handle(kevent_new("friend notif", 0)));
        return 0;
    }
    if (c->cmd == 2) return RC(121, 15);
    return stub_ok(o, c);
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
    return stub_ok(o, c);
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
static int prepo_disp(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
SvcObj* new_prepo(void) { return svcobj_new("prepo", prepo_disp, 0); }
static int psm_session(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_copy_h(c, ev_handle(kevent_new("psm", 0)));
        return 0;
    }
    return stub_ok(o, c);
}
static int psm_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out32(c, 100); return 0;
        case 1: out32(c, 1); return 0;
        case 7: out_obj(c, svcobj_new("IPsmSession", psm_session, 0)); return 0;
        case 4:
        case 5: out8(c, 0); return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_psm(void) { return svcobj_new("psm", psm_disp, 0); }
static int fatal_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
    fatal("The game called fatal:u (code %x)", in32(c, 0));
    return 0;
}
SvcObj* new_fatal(void) { return svcobj_new("fatal:u", fatal_disp, 0); }
/* random bytes for csrng and spl: splitmix64 over a shared counter, so parallel callers never share a state */
static void random_fill(u8* d, u64 n) {
    static u64 ctr = 0x9E3779B97F4A7C15ull;
    for (u64 i = 0; i < n; i += 8) {
        u64 z = __atomic_add_fetch(&ctr, 0x9E3779B97F4A7C15ull, __ATOMIC_RELAXED);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        memcpy(d + i, &z, n - i < 8 ? n - i : 8);
    }
}
static int csrng_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) { /* GenerateRandomBytes */
        u64 a, s;
        buf_out(c, 0, &a, &s);
        if (a) random_fill(GP(a), s);
        return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_csrng(void) { return svcobj_new("csrng", csrng_disp, 0); }
/* mm:u (multimedia clock requests): 4 Initialize -> id, 5 Finalize(id), 6 SetAndWait(id,min,max), 7 Get(id) -> rate,
   and the old forms 0-3 that name the module instead of an id. Get has to return what SetAndWait asked for: NvMM
   reads the decoder clock back. Each Initialize is its own request with a new id, as the SDK keeps them */
#define MM_REQ_MAX 32
static struct {
    u32 id, module, rate;
} g_mm[MM_REQ_MAX];
static u32 g_mm_next = 1;
static int mm_find(u32 key, int by_module) { /* g_klock held */
    for (int i = 0; i < MM_REQ_MAX; i++)
        if (g_mm[i].id && (by_module ? g_mm[i].module == key : g_mm[i].id == key)) return i;
    return -1;
}
static int mm_disp(SvcObj* o, IpcCtx* c) {
    u32 key = in32(c, 0);
    int old = c->cmd < 4;
    switch (c->cmd) {
        case 0:
        case 4: { /* Initialize(module, priority, event clear mode) */
            pm_lock(&g_klock);
            u32 id = g_mm_next++;
            int i = 0;
            while (i < MM_REQ_MAX && g_mm[i].id) i++;
            if (i < MM_REQ_MAX) {
                g_mm[i].id = id;
                g_mm[i].module = key;
                g_mm[i].rate = 0;
            }
            pm_unlock(&g_klock);
            if (i == MM_REQ_MAX) WARN("mm:u: more than %d clock requests\n", MM_REQ_MAX);
            if (!old) out32(c, id);
            return 0;
        }
        case 1:
        case 5: { /* Finalize */
            pm_lock(&g_klock);
            int i = mm_find(key, old);
            if (i >= 0) g_mm[i].id = 0;
            pm_unlock(&g_klock);
            return 0;
        }
        case 2:
        case 6: { /* SetAndWait(.., min, max) */
            pm_lock(&g_klock);
            int i = mm_find(key, old);
            if (i >= 0) g_mm[i].rate = in32(c, 4);
            pm_unlock(&g_klock);
            return 0;
        }
        case 3:
        case 7: { /* Get */
            pm_lock(&g_klock);
            int i = mm_find(key, old);
            u32 r = i >= 0 ? g_mm[i].rate : 0;
            pm_unlock(&g_klock);
            out32(c, r);
            return 0;
        }
    }
    return stub_ok(o, c);
}
SvcObj* new_mm(void) { return svcobj_new("mm:u", mm_disp, 0); }
static int nfp_user(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 1: out32(c, 0); return 0;
        case 17:
        case 18: out_copy_h(c, ev_handle(kevent_new("nfp", 0))); return 0;
        case 19: out32(c, 1); return 0;
    }
    return stub_ok(o, c);
}
static int nfp_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, svcobj_new("IUser", nfp_user, 0));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_nfp(void) { return svcobj_new("nfp:user", nfp_disp, 0); }
static int caps_disp(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
SvcObj* new_caps(void) { return svcobj_new("caps", caps_disp, 0); }
/* BCAT. behave like an offline console: sync completes immediately (Done/OK) and the delivery cache
   is empty. games do wait on the progress object, so it has to exist */
static int bcat_progress(SvcObj* o, IpcCtx* c) {
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
    return stub_ok(o, c);
}
static int bcat_dir(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: return 0; /* Open */
        case 1:
        case 2: out32(c, 0); return 0;
    } /* Read / GetCount: no files */
    return stub_ok(o, c);
}
static int bcat_file(SvcObj* o, IpcCtx* c) {
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
    return stub_ok(o, c);
}
static int bcat_storage(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out_obj(c, svcobj_new("IDeliveryCacheFileService", bcat_file, 0)); return 0;
        case 1: out_obj(c, svcobj_new("IDeliveryCacheDirectoryService", bcat_dir, 0)); return 0;
        case 10: out32(c, 0); return 0;
    } /* EnumerateDeliveryCacheDirectory: none */
    return stub_ok(o, c);
}
static int bcat_service(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 10100:
        case 10101:
        case 20100:
        case 20101:
        case 20300:
        case 20301: out_obj(c, svcobj_new("IDeliveryCacheProgressService", bcat_progress, 0)); return 0;
    }
    return stub_ok(o, c);
}
static int bcat_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out_obj(c, svcobj_new("IBcatService", bcat_service, 0)); return 0;
        case 1:
        case 2: out_obj(c, svcobj_new("IDeliveryCacheStorageService", bcat_storage, 0)); return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_bcat(void) { return svcobj_new("bcat", bcat_disp, 0); }
static int aoc_purchase(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 2) {
        out_copy_h(c, ev_handle(kevent_new("aoc purchase", 0)));
        return 0;
    }
    if (c->cmd == 3 || c->cmd == 4) return RC(164, 400);
    return stub_ok(o, c);
}
static int aoc_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0:
        case 1:
        case 2:
        case 3: out32(c, 0); return 0;                                 /* Count/List: no dlc */
        case 4:                                                        /* GetAddOnContentBaseIdByApplicationId */
        case 5: out64(c, (g_title_id & ~0xFFFull) + 0x1000); return 0; /* GetAddOnContentBaseId */
        case 6:                                                        /* PrepareAddOnContentByApplicationId */
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
    return stub_ok(o, c);
}
SvcObj* new_aoc(void) { return svcobj_new("aoc:u", aoc_disp, 0); }
static int irs_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 303: return 0; /* DeactivateIrsensor */
        case 304: {         /* GetIrsensorSharedMemoryHandle: the one shared memory, kept for the whole run */
            static KShmem* shm;
            KShmem* s = __atomic_load_n(&shm, __ATOMIC_ACQUIRE);
            if (!s) {
                KShmem* n = kshmem_new(0x8000, "irs");
                kobj_ref(&n->o);
                if (__atomic_compare_exchange_n(&shm, &s, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                    s = n;
                else
                    kobj_unref(&n->o); /* another thread made it first */
            }
            out_copy_h(c, h_new(&s->o));
            return 0;
        }
        case 311: out32(c, 0); return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_irs(void) { return svcobj_new("irs", irs_disp, 0); }
static int mii_db(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0 || c->cmd == 2 || c->cmd == 3) {
        out32(c, 0);
        return 0;
    }
    return stub_ok(o, c);
}
static int mii_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, svcobj_new("IDatabaseService", mii_db, 0));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_mii(void) { return svcobj_new("mii:u", mii_disp, 0); }
/* ldn: local wireless. Initialize and GetState follow the state, AttachStateChangeEvent gives an event. The rest
   (opening an access point or station, scanning, connecting) is not implemented and logs */
static int ldn_user(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out32(c, o->u[0] ? 1 : 0); return 0;                              /* GetState: Initialized, or None */
        case 100: out_copy_h(c, ev_handle(kevent_new("ldn state", 0))); return 0; /* AttachStateChangeEvent */
        case 400:                                                                 /* Initialize */
        case 402: o->u[0] = 1; return 0;
        case 401: o->u[0] = 0; return 0; /* Finalize */
    }
    return stub_ok(o, c);
}
static int ldn_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, svcobj_new("IUserLocalCommunicationService", ldn_user, 0));
        return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_ldn(void) { return svcobj_new("ldn:u", ldn_disp, 0); }
static int ectx_disp(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
SvcObj* new_ectx(void) { return svcobj_new("ectx:aw", ectx_disp, 0); }
static int spl_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out64(c, 0);
        return 0;
    }
    if (c->cmd == 7) { /* GenerateRandomBytes */
        u64 a, s;
        buf_out(c, 0, &a, &s);
        if (a) random_fill(GP(a), s);
        return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_spl(void) { return svcobj_new("spl:", spl_disp, 0); }
/* audin:u (IAudioInManager): no microphones. 0 ListAudioIns, 2 ListAudioInsAuto, 4 ListAudioInsAutoFiltered answer
   a count of 0, so the game opens none */
static int audin_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0:
        case 2:
        case 4: out32(c, 0); return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_audin(void) { return svcobj_new("audin:u", audin_disp, 0); }
