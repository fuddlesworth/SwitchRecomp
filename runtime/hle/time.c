/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* time: clocks and the time zone */
#include "hle/services_int.h"
#include "core/clock.h"
/* ================================================================ time */
static KShmem* g_time_shm;
static int sysclock_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out64(c, (u64)plat_unix_time()); return 0;
        case 1: return 0;
        case 2: {
            u8 ctx[0x20];
            memset(ctx, 0, sizeof ctx);
            i64 t = plat_unix_time();
            memcpy(ctx, &t, 8);
            outbytes(c, ctx, 0x20);
            return 0;
        }
        case 3: return 0;
        case 4: out_copy_h(c, ev_handle(kevent_new("clock op", 0))); return 0; /* GetOperationEventReadableHandle */
    }
    return stub_ok(o, c);
}
static u8 g_clock_id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static int steady_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: {
            u64 s = plat_ns() / 1000000000ull;
            out64(c, s);
            outbytes(c, g_clock_id, 16);
            return 0;
        }
        case 2:
        case 3: out64(c, 0); return 0;
        case 100: out64(c, 0); return 0;
        case 200:
            out64(c, 0);
            out64(c, 0);
            return 0;
        case 300: out64(c, 0); return 0;
    }
    return stub_ok(o, c);
}
static i64 g_tz_off;
static void to_calendar(i64 t, IpcCtx* c) {
    i64 days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int hh = (int)(rem / 3600), mm = (int)(rem % 3600 / 60), ss = (int)(rem % 60);
    /* days -> y/m/d */
    i64 z = days + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = (i64)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    u16 yy = (u16)y;
    outbytes(c, &yy, 2);
    out8(c, (u8)m);
    out8(c, (u8)d);
    out8(c, (u8)hh);
    out8(c, (u8)mm);
    out8(c, (u8)ss);
    out8(c, 0);
    /* CalendarAdditionalInfo */
    u32 wday = (u32)((days % 7 + 11) % 7);
    out32(c, wday);
    u32 yday = 0;
    out32(c, yday);
    char tz[8] = "UTC";
    outbytes(c, tz, 8);
    out32(c, 0);
    out32(c, (u32)(i32)g_tz_off);
}
static int tz_disp(SvcObj* o, IpcCtx* c) {
    {
        static int init;
        if (!init) {
            init = 1;
            g_tz_off = plat_tz_offset();
            LOG("time: PC time zone UTC%+.1f\n", g_tz_off / 3600.0);
        }
    }
    switch (c->cmd) {
        case 0: {
            char n[0x24];
            memset(n, 0, sizeof n);
            strcpy(n, "UTC");
            outbytes(c, n, 0x24);
            return 0;
        }
        case 1: return 0;
        case 2: out32(c, 1); return 0;
        case 3: {
            out32(c, 1);
            return 0;
        }
        case 4: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            memset(GP(a), 0, s);
            return 0;
        }
        case 5: {
            char v[16] = "2021a";
            outbytes(c, v, 16);
            return 0;
        }
        /* guest wall clock = host local time */
        case 100:
        case 101: {
            i64 t = (i64)in64(c, 0) + g_tz_off;
            to_calendar(t, c);
            return 0;
        }
        case 201:
        case 202: {
            u64 a, s;
            buf_out(c, 0, &a, &s); /* rough inverse */
            u16 y;
            memcpy(&y, c->raw, 2);
            u8 mo = c->raw[2], d = c->raw[3], h = c->raw[4], mi = c->raw[5], se = c->raw[6];
            i64 yy = y - (mo <= 2);
            i64 era = (yy >= 0 ? yy : yy - 399) / 400;
            unsigned yoe = (unsigned)(yy - era * 400);
            unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            i64 days = era * 146097 + (i64)doe - 719468;
            i64 t = days * 86400 + h * 3600 + mi * 60 + se;
            t -= g_tz_off;
            if (a && s >= 8) gw64(a, (u64)t);
            out32(c, 1);
            return 0;
        }
    }
    return stub_ok(o, c);
}
static void time_shm_update(void) {
    if (!g_time_shm) return;
    u8* p = kshmem_ptr(g_time_shm);
    /* steady clock {i64 base_time_ns; u8 id[16]} x2 @0x08/0x20; system clocks {i64 offset; i64 time_point; u8 id[16]} x2 */
    u32 cnt = 0;
    memcpy(p, &cnt, 4);
    for (int k = 0; k < 2; k++) {
        i64 base = 0;
        memcpy(p + 8 + k * 0x18, &base, 8);
        memcpy(p + 8 + k * 0x18 + 8, g_clock_id, 16);
    }
    i64 elapsed = (i64)((plat_ns() - g_t0_ns()) / 1000000000ull);
    i64 off = plat_unix_time() - elapsed;
    for (int c2 = 0; c2 < 2; c2++) {
        u8* q = p + (c2 ? 0x80 : 0x38);
        memcpy(q, &cnt, 4);
        for (int k = 0; k < 2; k++) {
            u8* e = q + 8 + k * 0x20;
            i64 tp = 0;
            memcpy(e, &off, 8);
            memcpy(e + 8, &tp, 8);
            memcpy(e + 16, g_clock_id, 16);
        }
    }
    /* auto correction {u32 counter; bool[2]} @0xC8 */
    memcpy(p + 0xC8, &cnt, 4);
    p[0xCC] = 1;
    p[0xCD] = 1;
    /* ContinuousAdjustmentTimePoint (17.0+) {u32 counter; pad; 2 x {rtc_offset, diff_scale, shift+pad, lower{t,id}, upper{t,id}}} @0xD0 */
    memcpy(p + 0xD0, &cnt, 4);
    for (int k = 0; k < 2; k++) {
        u8* e = p + 0xD8 + k * 0x48;
        memset(e, 0, 0x48);
        memcpy(e + 0x18 + 8, g_clock_id, 16);
        memcpy(e + 0x30 + 8, g_clock_id, 16);
    }
}
static int time_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0:
        case 1:
        case 4: out_obj(c, svcobj_new("ISystemClock", sysclock_disp, 0)); return 0;
        case 2: out_obj(c, svcobj_new("ISteadyClock", steady_disp, 0)); return 0;
        case 3: out_obj(c, svcobj_new("ITimeZoneService", tz_disp, 0)); return 0;
        case 5: out_obj(c, svcobj_new("IEphemeralNetworkSystemClock", sysclock_disp, 0)); return 0;
        case 20: {
            if (!g_time_shm) {
                g_time_shm = kshmem_new(0x1000, "time");
                time_shm_update();
            }
            out_copy_h(c, h_new(&g_time_shm->o));
            return 0;
        }
        case 100:
        case 101: out8(c, 1); return 0;
        case 200: out8(c, 1); return 0;
        case 300: {
            i64 t = plat_unix_time();
            out64(c, (u64)t);
            return 0;
        }
        case 400: out8(c, 0); return 0;
        case 500:
        case 501: {
            u8 ctx[0x20];
            memset(ctx, 0, 0x20);
            outbytes(c, ctx, 0x20);
            return 0;
        }
    }
    return stub_ok(o, c);
}
SvcObj* new_time(void) { return svcobj_new("time", time_disp, 0); }
