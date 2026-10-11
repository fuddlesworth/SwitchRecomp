/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
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
static int acc_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out32(c, 1); return 0;
        case 1: out8(c, 1); return 0;
        case 2:
        case 3: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a) {
                memset(GP(a), 0, s);
                if (s >= 16) memcpy(GP(a), g_user_id, 16);
            }
            return 0;
        }
        case 4: outbytes(c, g_user_id, 16); return 0;
        case 5: out_obj(c, svcobj_new("IProfile", profile_disp, 0)); return 0;
        case 50: out8(c, 0); return 0;
        case 51: outbytes(c, g_user_id, 16); return 0;
        case 60: out32(c, 1); return 0;
        case 100:
        case 140:
        case 150:
        case 111: return 0;
        case 101: out_obj(c, svc_generic("IManagerForApplication")); return 0;
        case 110: return 0;
        case 130:
        case 131: return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_acc(void) { return svcobj_new("acc:u0", acc_disp, 0); }
