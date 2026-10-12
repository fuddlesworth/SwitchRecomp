/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* hid. Games read the shared memory directly, so we fill it in the real layout (17 entry LIFOs). */
#include "hle/hid.h"
#include "hle/services.h"
#include "kernel/ipc.h"
#include "kernel/kernel.h"
#include <string.h>
#include "hle/hid_layout.h"

static KShmem* g_hid_shm;
PadState g_pad;
static u64 g_samp;
static u32 g_style_supported = 0x7F;
static KEvent* g_style_ev[10]; /* AcquireNpadStyleSetUpdateEventHandle, per npad index */
/* the legacy lifos (GetJoyXpadLifoHandle, GetSixAxisSensorLifoHandle, GetJoySixAxisSensorLifoHandle): one shared
   memory each, as the console gives the same one on every call. Nothing writes them */
static KShmem* g_xpad_shm;
static KShmem* g_six_shm;
static KShmem* g_joysix_shm;

static void lifo_push(u8* lifo, u32 esize, const void* state, u32 ssize, u64 samp) {
    /* lifo header: unused, buffer_count, tail, count. entries: {u64 sampling_number; state} */
    u64 cnt = 17, tail, count;
    memcpy(&tail, lifo + 0x10, 8);
    memcpy(&count, lifo + 0x18, 8);
    tail = (tail + 1) % cnt;
    u8* e = lifo + 0x20 + tail * esize;
    memcpy(e, &samp, 8);
    memcpy(e + 8, state, ssize);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(lifo + 8, &cnt, 8);
    memcpy(lifo + 0x10, &tail, 8);
    if (count < 16) count++;
    memcpy(lifo + 0x18, &count, 8);
}

/* npad index: 0..7 = No1..No8, 8 = Handheld (0x20), 9 = Other (0x10). footer: AppletFooterUiType */
static void npad_setup(u8* base, int idx, u32 style, u32 devtype, u8 footer) {
    u8* e = base + HO_HidSharedMemory_npad + idx * HS_HidNpadSharedMemoryEntry;
    memcpy(e + HO_HidNpadInternalState_style_set, &style, 4);
    u32 jam = 0;
    memcpy(e + HO_HidNpadInternalState_joy_assignment_mode, &jam, 4);
    /* color attribute 0 = Ok (1 is ReadError) */
    u32 col[3] = {0, 0xFF323232u, 0xFF0A0A0A};
    memcpy(e + HO_HidNpadInternalState_full_key_color, col, 12);
    u32 jc[5] = {0, 0xFF3C3C3Cu, 0xFF0A0A0A, 0xFF3C3C3Cu, 0xFF0A0A0A};
    memcpy(e + HO_HidNpadInternalState_joy_color, jc, 20);
    memcpy(e + HO_HidNpadInternalState_device_type, &devtype, 4);
    /* IsCharging JoyDual/Left/Right, IsPowered JoyDual, IsAbxyButtonOriented, IsSlSrButtonOriented,
       IsPlusAvailable, IsMinusAvailable */
    u64 props = (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 11) | (1ull << 12) | (1ull << 13) |
                (1ull << 14);
    memcpy(e + HO_HidNpadInternalState_system_properties, &props, 8);
    u32 bat[3] = {4, 4, 4};
    memcpy(e + HO_HidNpadInternalState_battery_level, bat, 12);
    u32 fattr = 0;
    memcpy(e + HO_HidNpadInternalState_applet_footer_ui_attribute, &fattr, 4);
    e[HO_HidNpadInternalState_applet_footer_ui_type] = footer;
}

void hid_update(void) {
    if (!g_hid_shm) return;
    u8* b = kshmem_ptr(g_hid_shm);
    g_samp++;
    u8 st[0x28];
    memset(st, 0, sizeof st);
    u64 btn = g_pad.buttons;
    /* sticks also set the stick-direction button bits, like hw */
    if (g_pad.lx < -16000) btn |= 1ull << 16;
    if (g_pad.ly > 16000) btn |= 1ull << 17;
    if (g_pad.lx > 16000) btn |= 1ull << 18;
    if (g_pad.ly < -16000) btn |= 1ull << 19;
    if (g_pad.rx < -16000) btn |= 1ull << 20;
    if (g_pad.ry > 16000) btn |= 1ull << 21;
    if (g_pad.rx > 16000) btn |= 1ull << 22;
    if (g_pad.ry < -16000) btn |= 1ull << 23;
    memcpy(st + HO_HidNpadCommonState_sampling_number, &g_samp, 8);
    memcpy(st + HO_HidNpadCommonState_buttons, &btn, 8);
    i32 s4[4] = {g_pad.lx, g_pad.ly, g_pad.rx, g_pad.ry};
    memcpy(st + HO_HidNpadCommonState_analog_stick_l, s4, 16);
    u32 attr = 1;
    memcpy(st + HO_HidNpadCommonState_attributes, &attr, 4); /* IsConnected */
    /* No1 = pro / dual joycon, Handheld = attached */
    u8* e0 = b + HO_HidSharedMemory_npad;
    lifo_push(e0 + HO_HidNpadInternalState_full_key_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, g_samp);
    lifo_push(e0 + HO_HidNpadInternalState_joy_dual_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, g_samp);
    lifo_push(e0 + HO_HidNpadInternalState_handheld_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, g_samp);
    u8* eh = b + HO_HidSharedMemory_npad + 8 * HS_HidNpadSharedMemoryEntry;
    lifo_push(eh + HO_HidNpadInternalState_handheld_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, g_samp);
    lifo_push(eh + HO_HidNpadInternalState_full_key_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, g_samp);
    /* unused npads still need a sampling number, the SDK looks at it */
    u8 z[0x28];
    memset(z, 0, sizeof z);
    memcpy(z, &g_samp, 8);
    for (int i = 1; i < 10; i++) {
        if (i == 8) continue;
        u8* e = b + HO_HidSharedMemory_npad + i * HS_HidNpadSharedMemoryEntry;
        lifo_push(e + HO_HidNpadInternalState_full_key_lifo, HS_HidNpadCommonStateAtomicStorage, z, 0x28, g_samp);
    }
    /* six-axis: at rest, gravity down */
    u8 six[HS_HidSixAxisSensorState]; /* {u64 delta_time; u64 sampling_number; ...}: no motion, so no delta */
    memset(six, 0, sizeof six);
    memcpy(six + 8, &g_samp, 8);
    float acc[3] = {0, 0, -1};
    memcpy(six + HO_HidSixAxisSensorState_acceleration, acc, 12);
    float dir[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(six + HO_HidSixAxisSensorState_direction, dir, 36);
    u32 sattr = 1;
    memcpy(six + HO_HidSixAxisSensorState_attributes, &sattr, 4);
    lifo_push(e0 + HO_HidNpadInternalState_full_key_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage, six,
              sizeof six, g_samp);
    lifo_push(e0 + HO_HidNpadInternalState_joy_dual_left_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
              six, sizeof six, g_samp);
    lifo_push(e0 + HO_HidNpadInternalState_joy_dual_right_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
              six, sizeof six, g_samp);
    lifo_push(eh + HO_HidNpadInternalState_handheld_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage, six,
              sizeof six, g_samp);
    /* touch screen: no touches, only the counters */
    u8 ts[HS_HidTouchScreenState];
    memset(ts, 0, sizeof ts);
    memcpy(ts, &g_samp, 8);
    lifo_push(b + HO_HidSharedMemory_touchscreen + HO_HidTouchScreenSharedMemoryFormat_lifo,
              HS_HidTouchScreenStateAtomicStorage, ts, sizeof ts, g_samp);
    /* keyboard/mouse/debug pad: only bump the counters */
    u8 kb[HS_HidKeyboardState];
    memset(kb, 0, sizeof kb);
    memcpy(kb, &g_samp, 8);
    lifo_push(b + HO_HidSharedMemory_keyboard, 8 + HS_HidKeyboardState, kb, sizeof kb, g_samp);
    u8 ms[HS_HidMouseState];
    memset(ms, 0, sizeof ms);
    memcpy(ms, &g_samp, 8);
    lifo_push(b + HO_HidSharedMemory_mouse, 8 + HS_HidMouseState, ms, sizeof ms, g_samp);
    u8 dp[HS_HidDebugPadState];
    memset(dp, 0, sizeof dp);
    memcpy(dp, &g_samp, 8);
    lifo_push(b + HO_HidSharedMemory_debug_pad, 8 + HS_HidDebugPadState, dp, sizeof dp, g_samp);
}

static int hid_applet_res(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out_copy_h(c, h_new(&g_hid_shm->o));
        return 0;
    }
    return stub_ok(o, c);
}
static int hid_vibration_list(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return 0;
} /* IActiveVibrationDeviceList: 0 ActivateVibrationDevice */
static u64 g_hold_type = 0, g_comm_mode = 0, g_handheld_mode = 0;
/* No1 reports ONE style like real hardware (the SDK asserts on multi-style sets, e.g. in GetVibrationDeviceHandles):
   Pro Controller if the game supports it, else dual Joy-Con, else nothing (a game that takes only the handheld
   style finds No1 disconnected). Input goes to every lifo, so either one reads the pad */
static void npad1_setup(void) {
    u8* b = kshmem_ptr(g_hid_shm);
    if (g_style_supported & 1u)
        npad_setup(b, 0, 1u << 0, 1u << 0, 12); /* FullKey, footer SwitchProController */
    else if (g_style_supported & (1u << 2))
        npad_setup(b, 0, 1u << 2, (1u << 4) | (1u << 5), 5); /* JoyDual: JoyLeft + JoyRight, footer JoyDual */
    else
        npad_setup(b, 0, 0, 0, 0);
}
static int hid_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out_obj(c, svcobj_new("IAppletResource", hid_applet_res, 0)); return 0;
        case 40: out_copy_h(c, h_new(&kevent_new("xpad id", 0)->o)); return 0; /* AcquireXpadIdEventHandle */
        case 55:
        case 59: out64(c, 0); return 0;                            /* GetXpadIds: none */
        case 58: out_copy_h(c, h_new(&g_xpad_shm->o)); return 0;   /* GetJoyXpadLifoHandle */
        case 62: out_copy_h(c, h_new(&g_six_shm->o)); return 0;    /* GetSixAxisSensorLifoHandle */
        case 65: out_copy_h(c, h_new(&g_joysix_shm->o)); return 0; /* GetJoySixAxisSensorLifoHandle */
        case 68:
        case 82: out8(c, 1); return 0; /* IsSixAxisSensorFusionEnabled / AtRest */
        case 71:                       /* GetSixAxisSensorFusionParameters */
        case 74:                       /* GetAccelerometerParameters */
            out32(c, 0);
            out32(c, 0);
            return 0;
        case 77:                        /* GetAccelerometerPlayMode */
        case 80: out32(c, 0); return 0; /* GetGyroscopeZeroDriftMode */
        case 83: out8(c, 0); return 0;  /* IsFirmwareUpdateAvailableForSixAxisSensor */
        case 69:
        case 70:
        case 72:
        case 73:
        case 75:
        case 76:
        case 78:
        case 79:
        case 81: return 0;
        case 100: /* SetSupportedNpadStyleSet: No1's style can change, which its style event tells */
            g_style_supported = in32(c, 0);
            npad1_setup();
            kevent_signal(g_style_ev[0]);
            return 0;
        case 101: out32(c, g_style_supported); return 0;
        case 106: { /* AcquireNpadStyleSetUpdateEventHandle */
            u32 id = in32(c, 0);
            int i = id == 0x20 ? 8 : id == 0x10 ? 9 : (int)(id & 7);
            out_copy_h(c, h_new(&g_style_ev[i]->o));
            return 0;
        }
        case 108: { /* GetPlayerLedPattern: the LEDs of players 1 to 8 */
            static const u8 led[8] = {0x1, 0x3, 0x7, 0xF, 0x9, 0x5, 0xD, 0x6};
            u32 id = in32(c, 0);
            out64(c, id < 8 ? led[id] : 0);
            return 0;
        }
        case 120: g_hold_type = in64(c, 8); return 0;
        case 121: out64(c, g_hold_type); return 0;        /* GetNpadJoyHoldType */
        case 127: return 0;                               /* StopLrAssignmentMode */
        case 128: g_handheld_mode = in64(c, 8); return 0; /* SetNpadHandheldActivationMode(aruid, i64 mode) */
        case 129: out64(c, g_handheld_mode); return 0;    /* GetNpadHandheldActivationMode */
        case 133:
        case 134:
        case 135:
        case 136:
        case 137:
        case 138: out8(c, 0); return 0;
        case 200: {
            u32 h = in32(c, 0);
            u32 dev = (h >> 16) & 0xFF;
            out32(c, 1);
            out32(c, dev == 0 ? 1 : 2);
            return 0;
        } /* GetVibrationDeviceInfo: LRA, left/right */
        case 202: {
            float v[4] = {0.f, 160.f, 0.f, 320.f};
            outbytes(c, v, 16);
            return 0;
        } /* GetActualVibrationValue */
        case 203: out_obj(c, svcobj_new("IActiveVibrationDeviceList", hid_vibration_list, 0)); return 0;
        case 205:
        case 211: out8(c, 1); return 0; /* IsVibrationPermitted / Mounted */
        case 208: out32(c, 0); return 0;
        case 400: out8(c, 0); return 0; /* IsUsbFullKeyControllerEnabled */
        case 1000: g_comm_mode = in64(c, 8); return 0;
        case 1001: out64(c, g_comm_mode); return 0;
        case 1003: out8(c, 0); return 0;
        case 102:
        case 103:
        case 104:
        case 107:
        case 109:
        case 122:
        case 123:
        case 124:
        case 125:
        case 126:
        case 130:
        case 131:
        case 132:
        case 201:
        case 204:
        case 206:
        case 207:
        case 209:
        case 210:
        case 212:
        case 1002:
        case 1004:
        case 300:
        case 301:
        case 302:
        case 303:
        case 304:
        case 305:
        case 306:
        case 307:
        case 308:
        case 309:
        case 310:
        case 311:
        case 312:
        case 313:
        case 314:
        case 315:
        case 316:
        case 317:
        case 60:
        case 61:
        case 63:
        case 64:
        case 66:
        case 67:
        case 91:
        case 1:
        case 11:
        case 21:
        case 31:
        case 32:
        case 51:
        case 56: return 0;
    }
    return stub_ok(o, c);
}
static SvcObj* new_hid(void) { return svcobj_new("hid", hid_disp, 0); }
static int hidsys_disp(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
static SvcObj* new_hidsys(void) { return svcobj_new("hid:sys", hidsys_disp, 0); }

/* the service keeps these for the whole run: its reference keeps them alive when the game closes its handles */
static KShmem* owned_shm(u64 size, const char* name) {
    KShmem* s = kshmem_new(size, name);
    kobj_ref(&s->o);
    return s;
}
void svc_hid_init(void) {
    g_hid_shm = owned_shm(0x40000, "hid");
    g_xpad_shm = owned_shm(0x1000, "hid xpad lifo");
    g_six_shm = owned_shm(0x1000, "hid six-axis lifo");
    g_joysix_shm = owned_shm(0x1000, "hid joy six-axis lifo");
    /* the style events start signaled, so the game reads the styles the first time it waits */
    for (int i = 0; i < 10; i++) {
        g_style_ev[i] = kevent_new("npad style", 0);
        kobj_ref(&g_style_ev[i]->o);
        kevent_signal(g_style_ev[i]);
    }
    u8* b = kshmem_ptr(g_hid_shm);
    npad1_setup();
    npad_setup(b, 8, 1u << 1, (1u << 2) | (1u << 3), 4); /* Handheld, footer HandheldJoyConLeftJoyConRight */
    hid_update();
    hid_update();
    svc_register("hid", new_hid);
    svc_register("hid:sys", new_hidsys);
}
