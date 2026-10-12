/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* set and set:sys: system settings */
#include "hle/services_int.h"
/* ================================================================ set */
/* the LanguageCode list in index order (nn::settings::Language), used by am's GetDesiredLanguage too */
static const char* const g_lang_codes[] = {"ja",    "en-US", "fr",     "de",      "it",      "es",
                                           "zh-CN", "ko",    "nl",     "pt",      "ru",      "zh-TW",
                                           "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
#define NLANG ((int)(sizeof g_lang_codes / sizeof g_lang_codes[0]))
static u64 lang_code(int i) {
    u64 v = 0;
    memcpy(&v, g_lang_codes[i], strlen(g_lang_codes[i]));
    return v;
}
u64 set_language_code(int lang) { return lang_code(lang >= 0 && lang < NLANG ? lang : 1); }
static int set_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out64(c, set_language_code(g_hle_lang)); return 0; /* GetLanguageCode */
        case 1:
        case 5: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            u32 n = (u32)(s / 8);
            u32 max = c->cmd == 1 ? 15 : NLANG;
            if (n > max) n = max;
            for (u32 i = 0; i < n; i++) gw64(a + i * 8, lang_code((int)i));
            out32(c, n);
            return 0;
        }
        case 2: {
            u32 idx = in32(c, 0);
            out64(c, idx < NLANG ? lang_code((int)idx) : 0);
            return 0;
        } /* MakeLanguageCode */
        case 3: out32(c, 15); return 0;
        case 6: out32(c, NLANG); return 0;
        case 4: out32(c, 1); return 0; /* GetRegionCode: usa */
        case 7:
        case 9: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a) memset(GP(a), 0, s);
            return 0;
        }
        case 8: out32(c, 0); return 0;
        case 11: { /* GetDeviceNickName: a 0x80 byte buffer */
            static const char name[] = "ScarletNX";
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (!a || !s) return 0;
            memset(GP(a), 0, s);
            memcpy(GP(a), name, s < sizeof name ? s - 1 : sizeof name);
            return 0;
        }
    }
    return stub_ok(o, c);
}
SvcObj* new_set(void) { return svcobj_new("set", set_disp, 0); }
static int setsys_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 3:
        case 4: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            u8 fw[0x100];
            memset(fw, 0, sizeof fw);
            fw[0] = 12;
            fw[1] = 1;
            fw[2] = 0;
            fw[4] = 12;
            fw[5] = 1;
            fw[6] = 0;
            fw[7] = 0;
            strcpy((char*)fw + 8, "NintendoSDK Firmware for NX");
            strcpy((char*)fw + 0x28, "12.1.0-1.0");
            strcpy((char*)fw + 0x68, "12.1.0");
            strcpy((char*)fw + 0x80, "NintendoSDK Firmware for NX 12.1.0-1.0");
            memcpy(GP(a), fw, s < 0x100 ? s : 0x100);
            return 0;
        }
        case 23: out32(c, 0); return 0; /* GetColorSetId */
        case 37: out64(c, 0); return 0; /* GetSettingsItemValueSize(class, key in X buffers): no such setting */
        case 38: {                      /* GetSettingsItemValue: the value goes to the B buffer, its size out */
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a) memset(GP(a), 0, s);
            out64(c, 0);
            return 0;
        }
        case 62: out8(c, 0); return 0; /* GetDebugModeFlag */
        case 90: {                     /* GetMiiAuthorId */
            u8 id[16];
            memset(id, 0, sizeof id);
            outbytes(c, id, 16);
            return 0;
        }
        case 91: return 0; /* SetShutdownRtcValue */
    }
    return stub_ok(o, c);
}
SvcObj* new_setsys(void) { return svcobj_new("set:sys", setsys_disp, 0); }
