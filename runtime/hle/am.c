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
KEvent* g_opmode_ev;
u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
void am_push_msg(u32 m) {
    pm_lock(&g_klock);
    if (g_nmsg < 16) g_msgq[g_nmsg++] = m;
    pm_unlock(&g_klock);
    kevent_signal(g_msg_event);
}

/* IStorage / IStorageAccessor */
typedef struct {
    u8* d;
    u64 size;
} StorageData;
static int storage_acc_disp(SvcObj* o, IpcCtx* c) {
    StorageData* s = (StorageData*)(size_t)o->u[0];
    switch (c->cmd) {
        case 0: out64(c, s->size); return 0;
        case 10: {
            u64 off = in64(c, 0), a, n;
            buf_in(c, 0, &a, &n);
            if (off + n > s->size) return RC(128, 503);
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
    if (c->cmd == 0 || c->cmd == 1) {
        SvcObj* a = svcobj_new("IStorageAccessor", storage_acc_disp, 0);
        a->u[0] = (u64)(size_t)o->data;
        o->refs++;
        out_obj(c, a);
        return 0;
    }
    return stub_ok(o, c);
}
static SvcObj* am_storage_new(u64 size, const void* init) {
    SvcObj* o = svcobj_new("IStorage", storage_disp, sizeof(StorageData));
    StorageData* s = o->data;
    s->d = calloc(1, size ? size : 1);
    s->size = size;
    if (init) memcpy(s->d, init, size);
    return o;
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
        case 5: out8(c, (u8)g_opt_docked); return 0;   /* GetOperationMode */
        case 6: out32(c, (u32)g_opt_docked); return 0; /* GetPerformanceMode */
        case 8: out8(c, 0); return 0;                  /* GetBootMode */
        case 9: out8(c, 1); return 0;                  /* GetCurrentFocusState: InFocus */
        case 13: out_copy_h(c, ev_handle(kevent_new("sleep lock", 0))); return 0;
        case 10:
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
        case 68:
        case 900: return 0;
        case 60:
            out32(c, g_opt_docked ? 1920 : 1280);
            out32(c, g_opt_docked ? 1080 : 720);
            return 0; /* GetDefaultDisplayResolution */
        case 61: out_copy_h(c, ev_handle(g_res_ev)); return 0;
        case 62:
            out32(c, g_opt_docked ? 1920 : 1280);
            out32(c, g_opt_docked ? 1080 : 720);
            return 0;
        case 63: out_copy_h(c, ev_handle(g_res_ev)); return 0;
        case 100: out8(c, 0); return 0;
        case 300: out32(c, 0); return 0; /* GetSettingsPlatformRegion */
        case 400:
        case 401:
        case 403: out_copy_h(c, ev_handle(kevent_new("csg ev", 0))); return 0;
    }
    return stub_ok(o, c);
}
static int am_self(SvcObj* o, IpcCtx* c) { /* ISelfController */
    switch (c->cmd) {
        case 9: out_copy_h(c, ev_handle(g_launch_ev)); return 0; /* GetLibraryAppletLaunchableEvent */
        case 40: {
            static u64 layer = 1;
            out64(c, layer++);
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
    return quiet_ok(o, c);
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
                if (given) return RC(128, 2);
                given = 1;
                u8 buf[0x88];
                memset(buf, 0, sizeof buf);
                u32 magic = 0xC79497CA;
                memcpy(buf, &magic, 4);
                u32 one = 1;
                memcpy(buf + 4, &one, 4);
                memcpy(buf + 8, g_user_id, 16);
                out_obj(c, am_storage_new(sizeof buf, buf));
                return 0;
            }
            return RC(128, 2);
        }
        case 10: out_obj(c, am_storage_new(in64(c, 0), NULL)); return 0;
        case 20: out64(c, 0); return 0; /* EnsureSaveData */
        case 21: {
            const char* l[] = {"ja", "en-US", "fr", "de",    "it",    "es",    "zh-CN", "ko",
                               "nl", "pt",    "ru", "zh-TW", "en-GB", "fr-CA", "es-419"};
            u64 v = 0;
            const char* s = l[g_opt_lang % 15];
            memcpy(&v, s, strlen(s));
            out64(c, v);
            return 0;
        }
        case 25: out64(c, 0); return 0; /* ExtendSaveData */
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
/* library applets (swkbd, error...) finish immediately with an empty result */
static int am_lib_accessor(SvcObj* o, IpcCtx* c) {
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
        case 100:
        case 103:
        case 50:
        case 60:
        case 150: return 0;
        case 101:
        case 104: {
            if (o->u[1]++) return RC(128, 2);
            /* applet output. web applets (Web, Shop, OfflineWeb, LoginShare, WifiWebAuth) return a 0x1010 byte
         WebCommonReturnValue (exit reason 0, last url, url size); callers check the size */
            u32 id = (u32)o->u[2];
            u64 n = 0x10;
            if (id == 0x13 || id == 0x14 || id == 0x17 || id == 0x18 || id == 0x19) n = 0x1010;
            SvcObj* st = am_storage_new(n, NULL);
            {
                static int said;
                if (said < 4) {
                    said++;
                    LOG("applet: library applet %02x closed right away (no applet UI here), %llu bytes of output\n", id,
                        (unsigned long long)n);
                }
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
    return 0;
}
static int am_lac(SvcObj* o, IpcCtx* c) { /* ILibraryAppletCreator */
    switch (c->cmd) {
        case 0: {
            SvcObj* a = svcobj_new("ILibraryAppletAccessor", am_lib_accessor, 0);
            a->u[2] = in32(c, 0);
            out_obj(c, a);
            return 0;
        }
        case 10: out_obj(c, am_storage_new(in64(c, 0), NULL)); return 0;
        case 11: out_obj(c, am_storage_new(in64(c, 0), NULL)); return 0;
    }
    return stub_ok(o, c);
}
static int am_audio(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 1) {
        out32(c, 0x3F800000);
        return 0;
    }
    if (c->cmd == 2) {
        out32(c, 0x3F800000);
        out32(c, 0x3F800000);
        return 0;
    }
    return 0;
}
static int am_display(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
}
static int am_debug(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
}
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
