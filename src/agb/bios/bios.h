/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* the AGB BIOS as HLE: the console's software interrupts run as board routines instead of ROM code. The
   calls follow the GBA BIOS convention: arguments in r0 to r3, results in r0 to r3. */
#ifndef SNX_AGB_BIOS_H
#define SNX_AGB_BIOS_H
#include <stdint.h>

typedef struct AgbBoard AgbBoard;

enum {
    AGB_SWI_SOFT_RESET = 0x00,
    AGB_SWI_REGISTER_RAM_RESET = 0x01,
    AGB_SWI_HALT = 0x02,
    AGB_SWI_STOP = 0x03,
    AGB_SWI_INTR_WAIT = 0x04,
    AGB_SWI_VBLANK_INTR_WAIT = 0x05,
    AGB_SWI_DIV = 0x06,
    AGB_SWI_DIV_ARM = 0x07,
    AGB_SWI_SQRT = 0x08,
    AGB_SWI_ARCTAN = 0x09,
    AGB_SWI_ARCTAN2 = 0x0A,
    AGB_SWI_CPU_SET = 0x0B,
    AGB_SWI_CPU_FAST_SET = 0x0C,
    AGB_SWI_GET_BIOS_CHECKSUM = 0x0D,
    AGB_SWI_BG_AFFINE_SET = 0x0E,
    AGB_SWI_OBJ_AFFINE_SET = 0x0F,
    AGB_SWI_BIT_UNPACK = 0x10,
    AGB_SWI_LZ77_UNCOMP_WRAM = 0x11,
    AGB_SWI_LZ77_UNCOMP_VRAM = 0x12,
    AGB_SWI_HUFF_UNCOMP = 0x13,
    AGB_SWI_RL_UNCOMP_WRAM = 0x14,
    AGB_SWI_RL_UNCOMP_VRAM = 0x15,
    AGB_SWI_DIFF_8BIT_UNFILTER_WRAM = 0x16,
    AGB_SWI_DIFF_8BIT_UNFILTER_VRAM = 0x17,
    AGB_SWI_DIFF_16BIT_UNFILTER = 0x18,
    AGB_SWI_SOUND_BIAS = 0x19,
    AGB_SWI_MIDI_KEY_2_FREQ = 0x1F,
    AGB_SWI_SOUND_DRIVER_GET_JUMP_LIST = 0x28,
    AGB_SWI_SOUND_DRIVER_GET_VERSION = 0x2F
};

/* Runs one software interrupt. The register file is the caller's r0 through r15: the call reads its
   arguments there and writes its results there. Returns 0 when the call ran, 1 when it is a known call
   with no HLE implementation yet, and -1 when the number is not a GBA BIOS call. */
int agb_bios_call(AgbBoard* board, uint32_t swi, uint32_t regs[16]);
/* The call's name for diagnostics, or NULL when the number is unknown. */
const char* agb_bios_name(uint32_t swi);
#endif
