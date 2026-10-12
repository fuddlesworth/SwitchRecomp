/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* acc: user accounts */
#include "hle/services_int.h"
/* ================================================================ acc */
static int profile_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0:
        case 1: {
            if (c->cmd == 0) {
                u64 a, s;
                buf_out(c, 0, &a, &s);
                if (a) { memset(GP(a), 0, s); }
            }
            u8 base[0x38];
            memset(base, 0, sizeof base);
            memcpy(base, g_user_id, 16);
            u64 ts = 1600000000;
            memcpy(base + 0x10, &ts, 8);
            strcpy((char*)base + 0x18, "Player");
            outbytes(c, base, 0x38);
            return 0;
        }
        case 10: out32(c, 0); return 0;
        case 11: out32(c, 0); return 0;
    }
    return stub_ok(o, c);
}
/* the account list of the user commands (ListAllUsers, ListOpenUsers, ListQualifiedUsers...): the one user */
static void list_users(IpcCtx* c) {
    u64 a, s;
    buf_out(c, 0, &a, &s);
    if (a) {
        memset(GP(a), 0, s);
        if (s >= 16) memcpy(GP(a), g_user_id, 16);
    }
}
/* IAsyncContext of an operation that needs the network: done at once, and failed the way nifm fails here, since
   there is no network */
static int async_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: { /* GetSystemEvent */
            KEvent* e = kevent_new("acc async");
            e->signaled = 1;
            out_copy_h(c, ev_handle(e));
            return 0;
        }
        case 1: return 0;             /* Cancel */
        case 2: out8(c, 1); return 0; /* HasDone */
        case 3: return RC(110, 300);  /* GetResult */
    }
    return stub_ok(o, c);
}
/* IManagerForApplication (the user's network service account): the account is there, its id token never arrives */
static int manager_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: return 0;                                                         /* CheckAvailability */
        case 1: out64(c, 0x0123456789ABCDEFull); return 0;                        /* GetAccountId */
        case 2: out_obj(c, svcobj_new("IAsyncContext", async_disp, 0)); return 0; /* EnsureIdTokenCacheAsync */
        case 3: out32(c, 0); return 0;                                            /* LoadIdTokenCache: empty */
        case 160: return 0;                                                       /* StoreOpenContext */
    }
    return stub_ok(o, c);
}
static int acc_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out32(c, 1); return 0;
        case 1: out8(c, 1); return 0;
        case 2:   /* ListAllUsers */
        case 3:   /* ListOpenUsers */
        case 131: /* ListOpenContextStoredUsers */
        case 141: /* ListQualifiedUsers */ list_users(c); return 0;
        case 4: outbytes(c, g_user_id, 16); return 0;
        case 5: out_obj(c, svcobj_new("IProfile", profile_disp, 0)); return 0;
        case 6: { /* GetProfileDigest */
            u8 d[16];
            memset(d, 0, sizeof d);
            outbytes(c, d, 16);
            return 0;
        }
        case 50: out8(c, 0); return 0;
        case 51: outbytes(c, g_user_id, 16); return 0;
        case 100:
        case 140:
        case 150:
        case 111: return 0;
        case 101: /* GetBaasAccountManagerForApplication */
        case 130: /* LoadOpenContext */ out_obj(c, svcobj_new("IManagerForApplication", manager_disp, 0)); return 0;
        case 110: return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_acc(void) { return svcobj_new("acc:u0", acc_disp, 0); }
