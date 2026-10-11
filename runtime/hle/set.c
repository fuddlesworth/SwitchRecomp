/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* set and set:sys: system settings */
#include "hle/services_int.h"
/* ================================================================ set */
static int set_disp(SvcObj* o, IpcCtx* c) {
    static const char* codes[] = {"ja", "en-US", "fr",    "de",    "it",    "es",     "zh-CN",   "ko",      "nl",
                                  "pt", "ru",    "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    switch (c->cmd) {
        case 0: {
            u64 v = 0;
            const char* s = codes[g_opt_lang % 18];
            memcpy(&v, s, strlen(s));
            out64(c, v);
            return 0;
        } /* GetLanguageCode */
        case 1:
        case 5: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            u32 n = (u32)(s / 8);
            u32 max = c->cmd == 1 ? 15 : 18;
            if (n > max) n = max;
            for (u32 i = 0; i < n; i++) {
                u64 v = 0;
                memcpy(&v, codes[i], strlen(codes[i]));
                gw64(a + i * 8, v);
            }
            out32(c, n);
            return 0;
        }
        case 2: {
            u32 idx = in32(c, 0);
            u64 v = 0;
            if (idx < 18) memcpy(&v, codes[idx], strlen(codes[idx]));
            out64(c, v);
            return 0;
        } /* MakeLanguageCode */
        case 3: out32(c, 15); return 0;
        case 6: out32(c, 18); return 0;
        case 4: out32(c, 1); return 0; /* GetRegionCode: usa */
        case 7:
        case 9: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            memset(GP(a), 0, s);
            return 0;
        }
        case 8: out32(c, 0); return 0;
        case 11: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            memset(GP(a), 0, s);
            strcpy((char*)GP(a), "SwitchRecomp");
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
        case 38:
        case 62: out32(c, 0); return 0;
        case 37: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            memset(GP(a), 0, s);
            out64(c, 0);
            return 0;
        } /* GetSettingsItemValueSize */
        case 90:
        case 91: out32(c, 0); return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_setsys(void) { return svcobj_new("set:sys", setsys_disp, 0); }
