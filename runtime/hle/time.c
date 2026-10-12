/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* time: clocks and the time zone */
#include "hle/services_int.h"
#include "core/clock.h"
/* ================================================================ time */
/* The steady clock is the guest clock (guest_ns, the time base of the ticks the game reads) in seconds, with
   g_clock_id as its source id. The system clocks are a SystemClockContext {i64 offset; SteadyClockTimePoint
   {i64 time; u8 source_id[16]}} on it: the time is offset + the steady time, and offset is set once at startup
   from the host's clock, so the IPC answers and the shared memory agree */
static KShmem* g_time_shm;
static i64 g_tz_off;  /* local - UTC, seconds: the guest's wall clock is the host's local time */
static i64 g_sys_off; /* the system clocks' offset */
static u8 g_clock_id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static i64 steady_s(void) { return (i64)(guest_ns() / 1000000000ull); }
static void sysclock_ctx(u8 out[0x20]) {
    i64 tp = steady_s();
    memcpy(out, &g_sys_off, 8);
    memcpy(out + 8, &tp, 8);
    memcpy(out + 16, g_clock_id, 16);
}
/* the time a SystemClockContext gives now: 0 (and failure) when it is on another steady clock */
static int ctx_time(const u8* ctx, i64* t) {
    i64 off;
    memcpy(&off, ctx, 8);
    if (memcmp(ctx + 16, g_clock_id, 16)) {
        *t = 0;
        return 0;
    }
    *t = off + steady_s();
    return 1;
}
static int sysclock_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out64(c, (u64)(g_sys_off + steady_s())); return 0; /* GetCurrentTime */
        case 1: return 0;
        case 2: { /* GetSystemClockContext */
            u8 ctx[0x20];
            sysclock_ctx(ctx);
            outbytes(c, ctx, 0x20);
            return 0;
        }
        case 3: return 0;
        case 4: out_copy_h(c, ev_handle(kevent_new("clock op", 0))); return 0; /* GetOperationEventReadableHandle */
    }
    return stub_ok(o, c);
}
static int steady_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: { /* GetCurrentTimePoint */
            out64(c, (u64)steady_s());
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
static i64 days_from_civil(i64 y, unsigned m, unsigned d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (i64)doe - 719468;
}
/* CalendarTime {i16 year; i8 month, day, hour, minute, second; pad} then CalendarAdditionalInfo {u32 day_of_week;
   u32 day_of_year; char tz_name[8]; u32 is_dst; i32 utc_offset} of a local time t */
static void to_calendar(i64 t, u8 out[0x20]) {
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
    memset(out, 0, 0x20);
    u16 yy = (u16)y;
    memcpy(out, &yy, 2);
    out[2] = (u8)m;
    out[3] = (u8)d;
    out[4] = (u8)hh;
    out[5] = (u8)mm;
    out[6] = (u8)ss;
    u32 wday = (u32)((days % 7 + 11) % 7);
    u32 yday = (u32)(days - days_from_civil(y, 1, 1));
    memcpy(out + 8, &wday, 4);
    memcpy(out + 12, &yday, 4);
    memcpy(out + 16, "UTC", 4);
    i32 off = (i32)g_tz_off;
    memcpy(out + 28, &off, 4);
}
static int tz_disp(SvcObj* o, IpcCtx* c) {
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
        case 3: { /* LoadLocationNameList(u32 index): the one location, "UTC" */
            u64 a, s;
            buf_out(c, 0, &a, &s);
            u32 n = in32(c, 0) == 0 && a && s >= 0x24;
            if (n) {
                memset(GP(a), 0, 0x24);
                memcpy(GP(a), "UTC", 4);
            }
            out32(c, n);
            return 0;
        }
        case 4: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a) memset(GP(a), 0, s);
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
            u8 cal[0x20];
            to_calendar((i64)in64(c, 0) + g_tz_off, cal);
            outbytes(c, cal, 0x20);
            return 0;
        }
        case 201:
        case 202: {
            u64 a, s;
            buf_out(c, 0, &a, &s); /* rough inverse */
            u16 y = (u16)(in8(c, 0) | in8(c, 1) << 8);
            u8 mo = in8(c, 2), d = in8(c, 3), h = in8(c, 4), mi = in8(c, 5), se = in8(c, 6);
            i64 t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
            t -= g_tz_off;
            if (a && s >= 8) gw64(a, (u64)t);
            out32(c, 1);
            return 0;
        }
    }
    return stub_ok(o, c);
}
static void time_shm_update(void) {
    u8* p = kshmem_ptr(g_time_shm);
    /* steady clock {i64 base_time_ns; u8 id[16]} x2 @0x08/0x20; system clocks {i64 offset; i64 time_point; u8 id[16]} x2 */
    u32 cnt = 0;
    memcpy(p, &cnt, 4);
    for (int k = 0; k < 2; k++) {
        i64 base = 0;
        memcpy(p + 8 + k * 0x18, &base, 8);
        memcpy(p + 8 + k * 0x18 + 8, g_clock_id, 16);
    }
    for (int c2 = 0; c2 < 2; c2++) {
        u8* q = p + (c2 ? 0x80 : 0x38);
        memcpy(q, &cnt, 4);
        for (int k = 0; k < 2; k++) {
            u8* e = q + 8 + k * 0x20;
            i64 tp = 0;
            memcpy(e, &g_sys_off, 8);
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
/* ClockSnapshot (0xD0): the user and network contexts (0x00, 0x20), their times (0x40, 0x48), CalendarTimes (0x50,
   0x58) and CalendarAdditionalInfos (0x60, 0x78), the steady clock's time point (0x90), the location name (0xA8),
   automatic correction (0xCC) and the type asked for (0xCD). A context on another steady clock gives time 0 */
static int clock_snapshot(IpcCtx* c, const u8* user, const u8* net, u8 type) {
    u64 a, s;
    buf_out(c, 0, &a, &s);
    if (!a || s < 0xD0) return RC(116, 1);
    u8 b[0xD0], cal[0x20];
    memset(b, 0, sizeof b);
    memcpy(b, user, 0x20);
    memcpy(b + 0x20, net, 0x20);
    i64 ut, nt;
    if (!ctx_time(user, &ut)) return RC(116, 102); /* TimeMismatch */
    ctx_time(net, &nt);
    memcpy(b + 0x40, &ut, 8);
    memcpy(b + 0x48, &nt, 8);
    to_calendar(ut + g_tz_off, cal);
    memcpy(b + 0x50, cal, 8);
    memcpy(b + 0x60, cal + 8, 0x18);
    to_calendar(nt + g_tz_off, cal);
    memcpy(b + 0x58, cal, 8);
    memcpy(b + 0x78, cal + 8, 0x18);
    i64 tp = steady_s();
    memcpy(b + 0x90, &tp, 8);
    memcpy(b + 0x98, g_clock_id, 16);
    memcpy(b + 0xA8, "UTC", 4);
    b[0xCC] = 1;
    b[0xCD] = type;
    memcpy(GP(a), b, sizeof b);
    return 0;
}
/* the two ClockSnapshots of CalculateStandardUserSystemClockDifferenceByUser and CalculateSpanBetween, in X buffers */
static int two_snapshots(IpcCtx* c, u8 sa[0xD0], u8 sb[0xD0]) {
    u64 a0, s0, a1, s1;
    buf_in(c, 0, &a0, &s0);
    buf_in(c, 1, &a1, &s1);
    if (!a0 || !a1 || s0 < 0xD0 || s1 < 0xD0) return 0;
    memcpy(sa, GP(a0), 0xD0);
    memcpy(sb, GP(a1), 0xD0);
    return 1;
}
static int time_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0:
        case 1:
        case 4: out_obj(c, svcobj_new("ISystemClock", sysclock_disp, 0)); return 0;
        case 2: out_obj(c, svcobj_new("ISteadyClock", steady_disp, 0)); return 0;
        case 3: out_obj(c, svcobj_new("ITimeZoneService", tz_disp, 0)); return 0;
        case 5: out_obj(c, svcobj_new("IEphemeralNetworkSystemClock", sysclock_disp, 0)); return 0;
        case 20: out_copy_h(c, h_new(&g_time_shm->o)); return 0;
        case 30: /* GetStandardNetworkClockOperationEventReadableHandle */
        case 31: /* GetEphemeralNetworkClockOperationEventReadableHandle */
            out_copy_h(c, ev_handle(kevent_new("clock op", 0)));
            return 0;
        case 100: out8(c, 1); return 0; /* IsStandardUserSystemClockAutomaticCorrectionEnabled */
        case 101: return 0;
        case 102: out32(c, 2019); return 0; /* GetStandardUserSystemClockInitialYear */
        case 200: out8(c, 1); return 0;
        case 300: { /* CalculateMonotonicSystemClockBaseTimePoint(SystemClockContext): offset + steady - ticks */
            if (c->raw_size < 0x20 || memcmp(c->raw + 16, g_clock_id, 16)) return RC(116, 102); /* TimeMismatch */
            out64(c, in64(c, 0));
            return 0;
        }
        case 400: { /* GetClockSnapshot(u8 type) */
            u8 user[0x20], net[0x20];
            sysclock_ctx(user);
            sysclock_ctx(net);
            return clock_snapshot(c, user, net, in8(c, 0));
        }
        case 401: { /* GetClockSnapshotFromSystemClockContext(u8 type, user context @8, network context @0x28) */
            u8 user[0x20], net[0x20];
            for (u32 i = 0; i < 0x20; i++) {
                user[i] = in8(c, 8 + i);
                net[i] = in8(c, 0x28 + i);
            }
            return clock_snapshot(c, user, net, in8(c, 0));
        }
        case 500: { /* CalculateStandardUserSystemClockDifferenceByUser: TimeSpan (ns) between the user contexts */
            u8 sa[0xD0], sb[0xD0];
            if (!two_snapshots(c, sa, sb)) return RC(116, 1);
            i64 oa, ob;
            memcpy(&oa, sa, 8);
            memcpy(&ob, sb, 8);
            int same = !memcmp(sa + 16, sb + 16, 16), auto_corrected = sa[0xCC] && sb[0xCC];
            out64(c, (u64)(same && !auto_corrected ? (ob - oa) * 1000000000ll : 0));
            return 0;
        }
        case 501: { /* CalculateSpanBetween: TimeSpan (ns) between the steady time points, or the network times */
            u8 sa[0xD0], sb[0xD0];
            if (!two_snapshots(c, sa, sb)) return RC(116, 1);
            i64 ta, tb;
            if (!memcmp(sa + 0x98, sb + 0x98, 16)) {
                memcpy(&ta, sa + 0x90, 8);
                memcpy(&tb, sb + 0x90, 8);
            } else {
                memcpy(&ta, sa + 0x48, 8);
                memcpy(&tb, sb + 0x48, 8);
                if (!ta || !tb) return RC(116, 102); /* TimeMismatch */
            }
            out64(c, (u64)((tb - ta) * 1000000000ll));
            return 0;
        }
    }
    return stub_ok(o, c);
}
SvcObj* new_time(void) { return svcobj_new("time", time_disp, 0); }
/* at startup, before any session: the clocks' offsets and the shared memory, which the service keeps for the whole
   run (its reference keeps it alive when the game closes its handle) */
void time_init(void) {
    g_tz_off = plat_tz_offset();
    LOG("time: PC time zone UTC%+.1f\n", g_tz_off / 3600.0);
    g_sys_off = plat_unix_time() - steady_s();
    g_time_shm = kshmem_new(0x1000, "time");
    kobj_ref(&g_time_shm->o);
    time_shm_update();
}
