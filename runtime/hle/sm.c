/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* sm: the service manager, which hands out sessions to the other services */
#include "hle/services_int.h"
/* ================================================================ sm */
static int sm_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: return 0; /* RegisterClient */
        case 1: {         /* GetServiceHandle */
            char name[9] = {0};
            memcpy(name, c->raw, 8);
            SvcObj* s = svc_open(name);
            if (!s) {
                WARN("sm: service '%s' does not exist in SwitchRecomp: using a stub\n", name);
                s = svc_generic(name);
            }
            LOG("sm: GetServiceHandle(%s)\n", name);
            out_move_h(c, session_new(s));
            return 0;
        }
        case 2:
        case 3:
        case 4: return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_sm(void) { return svcobj_new("sm:", sm_disp, 0); }

/* unknown service -> generic stub, everything succeeds */
typedef struct {
    char name[16];
} GenData;
static int generic_disp(SvcObj* o, IpcCtx* c) {
    if (ipc_warn_once(((GenData*)o->data)->name, c->cmd))
        WARN("  [not implemented] %s cmd %u -> ok\n", ((GenData*)o->data)->name, c->cmd);
    return 0;
}
SvcObj* svc_generic(const char* n) {
    SvcObj* o = svcobj_new("generic", generic_disp, sizeof(GenData));
    strncpy(((GenData*)o->data)->name, n, 15);
    o->name = ((GenData*)o->data)->name;
    return o;
}
int quiet_ok(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
}
SvcObj* quiet(const char* name) { return svcobj_new(name, quiet_ok, 0); }
