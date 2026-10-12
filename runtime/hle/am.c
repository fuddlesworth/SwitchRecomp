/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* am: the applet manager (appletOE, appletAE) and its storages */
#include "hle/services_int.h"
/* ================================================================ am */
KEvent* g_msg_event;
static u32 g_msgq[16];
static int g_nmsg;
KEvent* g_launch_ev;
KEvent* g_res_ev;
KEvent* g_sleep_ev; /* GetAcquiredSleepLockEvent: signaled when a RequestToAcquireSleepLock is granted */
u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
void am_push_msg(u32 m) {
    pm_lock(&g_klock);
    int queued = g_nmsg < 16;
    if (queued) g_msgq[g_nmsg++] = m;
    pm_unlock(&g_klock);
    /* a full queue is already signaled, and the message is lost: say so rather than signal for nothing */
    if (queued)
        kevent_signal(g_msg_event);
    else
        WARN("am: applet message queue full, message %u dropped\n", m);
}

/* IStorage / IStorageAccessor */
typedef struct {
    u8* d;
    u64 size;
} StorageData;
static void storage_destroy(SvcObj* o) { free(((StorageData*)o->data)->d); }
/* an accessor holds a reference on its storage (u[0]), dropped when the game closes the accessor */
static void storage_acc_destroy(SvcObj* o) { svcobj_unref((SvcObj*)(size_t)o->u[0]); }
static int storage_acc_disp(SvcObj* o, IpcCtx* c) {
    StorageData* s = ((SvcObj*)(size_t)o->u[0])->data;
    switch (c->cmd) {
        case 0: out64(c, s->size); return 0;
        case 10: {
            u64 off = in64(c, 0), a, n;
            buf_in(c, 0, &a, &n);
            /* off + n can wrap: compare against what is left instead */
            if (off > s->size || n > s->size - off) return RC(128, 503);
            memcpy(s->d + off, GP(a), n);
            return 0;
        }
        case 11: {
            u64 off = in64(c, 0), a, n;
            buf_out(c, 0, &a, &n);
            if (off > s->size) return RC(128, 503);
            if (off + n > s->size) n = s->size - off;
            memcpy(GP(a), s->d + off, n);
            return 0;
        }
    }
    return stub_ok(o, c);
}
static int storage_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: { /* Open */
            SvcObj* a = svcobj_new("IStorageAccessor", storage_acc_disp, 0);
            a->u[0] = (u64)(size_t)o;
            __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
            a->destroy = storage_acc_destroy;
            out_obj(c, a);
            return 0;
        }
        /* OpenTransferStorage gives an ITransferStorageAccessor, which only a storage made over transfer memory
           has. These storages are plain memory (CreateTransferMemoryStorage copies nothing either) */
        case 1: return RC(128, 503);
    }
    return stub_ok(o, c);
}
/* NULL when the guest asks for more than the host can allocate */
static SvcObj* am_storage_new(u64 size, const void* init) {
    u8* d = calloc(1, size ? size : 1);
    if (!d) {
        WARN("am: no memory for a %llu byte storage\n", (unsigned long long)size);
        return NULL;
    }
    SvcObj* o = svcobj_new("IStorage", storage_disp, sizeof(StorageData));
    StorageData* s = o->data;
    s->d = d;
    s->size = size;
    if (init) memcpy(s->d, init, size);
    o->destroy = storage_destroy;
    return o;
}
/* the result of a Create*Storage command */
static int out_storage(IpcCtx* c, u64 size) {
    SvcObj* st = am_storage_new(size, NULL);
    if (!st) return RC(128, 503);
    out_obj(c, st);
    return 0;
}

static int am_csg(SvcObj* o, IpcCtx* c) { /* ICommonStateGetter */
    switch (c->cmd) {
        case 0: out_copy_h(c, ev_handle(g_msg_event)); return 0;
        case 1: {
            pm_lock(&g_klock);
            if (!g_nmsg) {
                g_msg_event->signaled = 0;
                pm_unlock(&g_klock);
                return RC(128, 3);
            }
            u32 m = g_msgq[0];
            memmove(g_msgq, g_msgq + 1, (--g_nmsg) * 4);
            if (!g_nmsg) g_msg_event->signaled = 0;
            pm_unlock(&g_klock);
            out32(c, m);
            return 0;
        }
        case 2: out64(c, 0); return 0;                 /* GetThisAppletKind: Application */
        case 5: out8(c, (u8)g_hle_docked); return 0;   /* GetOperationMode */
        case 6: out32(c, (u32)g_hle_docked); return 0; /* GetPerformanceMode */
        case 8: out8(c, 0); return 0;                  /* GetBootMode */
        case 9: out8(c, 1); return 0;                  /* GetCurrentFocusState: InFocus */
        case 10: kevent_signal(g_sleep_ev); return 0;  /* RequestToAcquireSleepLock: granted at once, no sleep here */
        case 13: out_copy_h(c, ev_handle(g_sleep_ev)); return 0; /* GetAcquiredSleepLockEvent */
        case 11:
        case 12:
        case 50:
        case 51:
        case 52:
        case 53:
        case 54:
        case 55:
        case 59:
        case 66:
        case 67:
        case 68: return 0;
        case 60:
            out32(c, g_hle_docked ? 1920 : 1280);
            out32(c, g_hle_docked ? 1080 : 720);
            return 0; /* GetDefaultDisplayResolution */
        case 61: out_copy_h(c, ev_handle(g_res_ev)); return 0;
        case 62: out32(c, 0); return 0; /* GetHdcpAuthenticationState: Unauthenticated, no TV link to protect */
        case 63: out_copy_h(c, ev_handle(g_res_ev)); return 0;
        case 91: out32(c, 0x00020003); return 0; /* GetCurrentPerformanceConfiguration, the one apm reports */
    }
    return stub_ok(o, c);
}
static int am_self(SvcObj* o, IpcCtx* c) { /* ISelfController */
    switch (c->cmd) {
        case 9: out_copy_h(c, ev_handle(g_launch_ev)); return 0; /* GetLibraryAppletLaunchableEvent */
        case 40: {
            static u64 layer = 1;
            out64(c, __atomic_fetch_add(&layer, 1, __ATOMIC_RELAXED));
            return 0;
        } /* CreateManagedDisplayLayer */
        case 41: out64(c, 0); return 0;
        case 44:
            out64(c, 1);
            out64(c, 2);
            return 0;
        case 62: return 0;
        case 63: out32(c, 0); return 0; /* GetIdleTimeDetectionExtension */
        case 69: out8(c, 0); return 0;  /* IsAutoSleepDisabled */
        case 90: out64(c, 0); return 0; /* GetAccumulatedSuspendedTickValue */
        case 91: out_copy_h(c, ev_handle(kevent_new("suspend", 0))); return 0;
        case 100:
        case 110:
        case 120: return 0;
    }
    return stub_ok(o, c);
}
static int am_window(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 1: out64(c, 0x1234); return 0;
        case 10:
        case 11:
        case 12: return 0;
    }
    return stub_ok(o, c);
}
static int am_appfn(SvcObj* o, IpcCtx* c) { /* IApplicationFunctions */
    switch (c->cmd) {
        case 1: {
            u32 kind = in32(c, 0);
            if (kind == 2) { /* PreselectedUser */
                static int given;
                if (__atomic_exchange_n(&given, 1, __ATOMIC_SEQ_CST)) return RC(128, 2);
                u8 buf[0x88];
                memset(buf, 0, sizeof buf);
                u32 magic = 0xC79497CA;
                memcpy(buf, &magic, 4);
                u32 one = 1;
                memcpy(buf + 4, &one, 4);
                memcpy(buf + 8, g_user_id, 16);
                SvcObj* st = am_storage_new(sizeof buf, buf);
                if (!st) return RC(128, 503);
                out_obj(c, st);
                return 0;
            }
            return RC(128, 2);
        }
        case 20: out64(c, 0); return 0;                             /* EnsureSaveData */
        case 21: out64(c, set_language_code(g_hle_lang)); return 0; /* GetDesiredLanguage */
        case 25: out64(c, 0); return 0;                             /* ExtendSaveData */
        case 26:
            out64(c, 0x4000000);
            out64(c, 0x4000000);
            return 0; /* GetSaveDataSize */
        case 110: out32(c, 0); return 0;
        case 22:
        case 27:
        case 28:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
        case 66:
        case 67:
        case 68:
        case 90:
        case 100:
        case 101:
        case 120:
        case 124: return 0;
        case 151: return RC(128, 2); /* TryPopFromNotificationStorageChannel: nothing queued */
        case 23: {
            char v[16] = "1.0.0";
            outbytes(c, v, 16);
            return 0;
        } /* GetDisplayVersion */
        case 40: out8(c, 0); return 0; /* NotifyRunning */
        case 50:
            out64(c, 0x1111222233334444ull);
            out64(c, 0x5555666677778888ull);
            return 0;
        case 60:
        case 61: return 0;
        case 130:
        case 140:
        case 150:
        case 160:
        case 210:
            out_copy_h(c, ev_handle(kevent_new("appfn ev", 0)));
            return 0; /* events (210 is new in recent SDKs) */
        case 141: return RC(128, 2);
        case 170: out8(c, 0); return 0;
    }
    return stub_ok(o, c);
}
/* library applets (swkbd, error...) finish immediately: no applet UI here */
typedef struct {
    u32 id;
    int popped_out, popped_interactive;
    SvcObj* in[8]; /* the storages PushInData passed, in order */
    int nin;
} LibApplet;
static void lib_applet_destroy(SvcObj* o) {
    LibApplet* la = o->data;
    for (int i = 0; i < la->nin; i++) svcobj_unref(la->in[i]);
}
/* the object a request passes in (InObject), with a reference the caller drops. Only domains pass objects, by
   their id in the domain: the same lookup as ipc.c's domain_get */
static SvcObj* in_obj(IpcCtx* c, int i) {
    if (i >= c->nin_objs || !c->sess || !c->sess->domain) return NULL;
    Domain* d = c->sess->domain;
    u32 id = c->in_objs[i];
    pm_lock(&d->lk);
    SvcObj* o = (id && id <= (u32)d->n) ? d->obj[id - 1] : NULL;
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
    pm_unlock(&d->lk);
    return o;
}
/* the controller applet (ShowControllerSupport) answers ControllerSupportResultInfo {i8 player_count; pad[3];
   u32 selected_id} and a u32 result, which the SDK turns into success (0) or cancelled (2). The last storage
   pushed is the ControllerSupportArg, whose byte 0 is min_player_count: one controller (npad No1) satisfies at
   most one player, as a console with one controller connected would */
static SvcObj* controller_result(LibApplet* la) {
    i8 min_players = 0;
    SvcObj* arg = la->nin ? la->in[la->nin - 1] : NULL;
    if (arg && arg->dispatch == storage_disp && ((StorageData*)arg->data)->size)
        min_players = (i8)((StorageData*)arg->data)->d[0];
    u8 r[0xC];
    memset(r, 0, sizeof r);
    u32 result = min_players <= 1 ? 0 : 2;
    if (!result) r[0] = 1; /* player_count 1, selected_id 0 (No1) */
    memcpy(r + 8, &result, 4);
    if (result) LOG("applet: controller support asks for %d players, one controller here: cancelled\n", min_players);
    return am_storage_new(sizeof r, r);
}
static int am_lib_accessor(SvcObj* o, IpcCtx* c) {
    LibApplet* la = o->data;
    switch (c->cmd) {
        case 0: {
            KEvent* e = kevent_new("applet state", 0);
            e->signaled = 1;
            out_copy_h(c, h_new(&e->o));
            return 0;
        }
        case 1: out8(c, 1); return 0;
        case 10:
        case 20:
        case 25:
        case 30:
        case 103:
        case 50:
        case 60:
        case 150: return 0;
        case 100: { /* PushInData: the controller applet's answer depends on what was pushed */
            SvcObj* st = in_obj(c, 0);
            if (st && la->nin < 8)
                la->in[la->nin++] = st;
            else
                svcobj_unref(st);
            return 0;
        }
        case 101:
        case 104: {
            /* PopOutData and PopInteractiveOutData are separate channels, each with one storage */
            int* popped = c->cmd == 101 ? &la->popped_out : &la->popped_interactive;
            if (__atomic_exchange_n(popped, 1, __ATOMIC_SEQ_CST)) return RC(128, 2);
            /* applet output. web applets (Web, Shop, OfflineWeb, LoginShare, WifiWebAuth) return a 0x1010 byte
         WebCommonReturnValue (exit reason 0, last url, url size); callers check the size. PlayerSelect returns
         UiReturnArg {u64 result; Uid uid}: the one user, selected */
            u32 id = la->id;
            SvcObj* st;
            u64 n = 0x10;
            if (id == 0x0C && c->cmd == 101) {
                st = controller_result(la);
                n = 0xC;
            } else if (id == 0x10) {
                u8 r[0x18];
                memset(r, 0, sizeof r);
                memcpy(r + 8, g_user_id, 16);
                n = sizeof r;
                st = am_storage_new(n, r);
            } else {
                if (id == 0x13 || id == 0x14 || id == 0x17 || id == 0x18 || id == 0x19) n = 0x1010;
                st = am_storage_new(n, NULL);
            }
            if (!st) return RC(128, 503);
            {
                static int said;
                if (__atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 4)
                    LOG("applet: library applet %02x closed right away (no applet UI here), %llu bytes of output\n", id,
                        (unsigned long long)n);
            }
            out_obj(c, st);
            return 0;
        }
        case 105:
        case 106: {
            KEvent* e = kevent_new("applet data", 0);
            e->signaled = 1;
            out_copy_h(c, h_new(&e->o));
            return 0;
        }
        case 120: out64(c, 0); return 0;
    }
    return stub_ok(o, c);
}
static int am_lac(SvcObj* o, IpcCtx* c) { /* ILibraryAppletCreator */
    switch (c->cmd) {
        case 0: { /* CreateLibraryApplet(u32 applet id, u32 mode) */
            SvcObj* a = svcobj_new("ILibraryAppletAccessor", am_lib_accessor, sizeof(LibApplet));
            ((LibApplet*)a->data)->id = in32(c, 0);
            a->destroy = lib_applet_destroy;
            out_obj(c, a);
            return 0;
        }
        case 10:
            return out_storage(c, in64(c, 0)); /* CreateStorage(i64 size) */
        /* CreateTransferMemoryStorage(bool writable, i64 size, copy handle): a plain storage of that size. Backing
           it with the transfer memory needs a handle lookup the services can't do yet */
        case 11: return out_storage(c, in64(c, 8));
    }
    return stub_ok(o, c);
}
static int am_audio(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 1:                   /* GetMainAppletExpectedMasterVolume */
        case 2:                   /* GetLibraryAppletExpectedMasterVolume */
            out32(c, 0x3F800000); /* 1.0f */
            return 0;
    }
    return stub_ok(o, c);
}
static int am_display(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
static int am_debug(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
static int am_proxy(SvcObj* o, IpcCtx* c) { /* IApplicationProxy */
    switch (c->cmd) {
        case 0: out_obj(c, svcobj_new("ICommonStateGetter", am_csg, 0)); return 0;
        case 1: out_obj(c, svcobj_new("ISelfController", am_self, 0)); return 0;
        case 2: out_obj(c, svcobj_new("IWindowController", am_window, 0)); return 0;
        case 3: out_obj(c, svcobj_new("IAudioController", am_audio, 0)); return 0;
        case 4: out_obj(c, svcobj_new("IDisplayController", am_display, 0)); return 0;
        case 10: out_obj(c, quiet("IProcessWindingController")); return 0;
        case 11: out_obj(c, svcobj_new("ILibraryAppletCreator", am_lac, 0)); return 0;
        case 20: out_obj(c, svcobj_new("IApplicationFunctions", am_appfn, 0)); return 0;
        case 1000: out_obj(c, svcobj_new("IDebugFunctions", am_debug, 0)); return 0;
    }
    return stub_ok(o, c);
}
static int am_oe(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_obj(c, svcobj_new("IApplicationProxy", am_proxy, 0));
        return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_appletoe(void) { return svcobj_new("appletOE", am_oe, 0); }
SvcObj* new_appletae(void) { return svcobj_new("appletAE", am_oe, 0); }
