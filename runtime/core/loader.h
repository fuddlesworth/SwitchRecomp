/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the game module (<Game>_code.so) and loading it (loader.c) */
#ifndef SWR_LOADER_H
#define SWR_LOADER_H
#include "abi/cpu.h"

typedef struct SwrModInfo {
    const char* name;
    u64 base, size;
    u8 build_id[32];
    u32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    u32 is_nro;
} SwrModInfo;
typedef struct SwrModule {
    u32 abi;
    u32 nmods;
    const SwrModInfo* mods;
    u32 nentries;
    const u64* entry_addr;
    const GuestFn* entry_fn;
    GuestFn interp;
    u64 code_lo, code_hi;
    const char* title;
    u64 title_id;
    u32 main_stack, main_prio, main_core;
    void (*init)(const RtAPI* rt);
    u64 (*step)(CPU* c, u32 w, u64 pc);
} SwrModule;
extern const SwrModule* g_mod;
extern GuestFn* g_disp;
extern GuestFn swr_interp_entry;
int loader_load(void);
extern u64 g_code_lo, g_code_hi;

#endif
