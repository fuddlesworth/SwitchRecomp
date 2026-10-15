/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "bios.h"
#include "board/board.h"
#include <math.h>
#include <string.h>

#define AGB_PI 3.14159265358979323846

/* Div and DivArm: r0 = numerator over r1 = denominator, giving the quotient in r0, the remainder in r1 and
   the absolute quotient in r3. The real BIOS hangs on a zero denominator; the HLE call returns the values a
   trapped caller observes instead. */
static void bios_div(uint32_t regs[16], int32_t numerator, int32_t denominator) {
    int64_t quotient;
    if (!denominator) {
        regs[0] = (uint32_t)(numerator < 0 ? -1 : 1);
        regs[1] = (uint32_t)numerator;
        regs[3] = 1;
        return;
    }
    if (denominator == -1 && numerator == INT32_MIN) {
        regs[0] = (uint32_t)INT32_MIN;
        regs[1] = 0;
        regs[3] = (uint32_t)INT32_MIN;
        return;
    }
    quotient = numerator / denominator;
    regs[0] = (uint32_t)quotient;
    regs[1] = (uint32_t)(numerator % denominator);
    quotient = quotient < 0 ? -quotient : quotient;
    regs[3] = (uint32_t)quotient;
}

static uint32_t bios_sqrt(uint32_t value) {
    uint32_t root = 0, bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else
            root >>= 1;
        bit >>= 2;
    }
    return root;
}

/* Angles are turns in 16 bits: 0x10000 is a full circle, 0x4000 is a right angle. ArcTan takes the tangent
   in 1.14 fixed point and returns half a turn at most. */
static int32_t bios_arctan(int32_t tangent) {
    return (int32_t)llround(atan((double)tangent / 16384.0) * 65536.0 / (2.0 * AGB_PI));
}

static int32_t bios_arctan2(int32_t x, int32_t y) {
    return (int32_t)((uint32_t)llround(atan2((double)y, (double)x) * 65536.0 / (2.0 * AGB_PI)) & 0xFFFF);
}

/* CpuSet: r2 carries the unit count, the fill flag (bit 24) and the width (bit 26, 32 bits or 16 bits). */
static void bios_cpu_set(AgbBoard* board, uint32_t regs[16]) {
    uint32_t source = regs[0], destination = regs[1], count = regs[2] & 0x1FFFFF;
    int fill = (int)(regs[2] >> 24 & 1), wide = (int)(regs[2] >> 26 & 1);
    if (wide) {
        uint32_t value = agb_read32(board, source);
        while (count--) {
            if (!fill) value = agb_read32(board, source);
            agb_write32(board, destination, value);
            source += 4;
            destination += 4;
        }
    } else {
        uint16_t value = agb_read16(board, source);
        while (count--) {
            if (!fill) value = agb_read16(board, source);
            agb_write16(board, destination, value);
            source += 2;
            destination += 2;
        }
    }
}

/* CpuFastSet moves 32-bit words in eight word blocks: r2's count rounds up. */
static void bios_cpu_fast_set(AgbBoard* board, uint32_t regs[16]) {
    uint32_t source = regs[0], destination = regs[1];
    uint32_t count = (regs[2] & 0x1FFFFF) + 7u & ~(uint32_t)7u;
    int fill = (int)(regs[2] >> 24 & 1);
    uint32_t value = agb_read32(board, source);
    while (count--) {
        if (!fill) value = agb_read32(board, source);
        agb_write32(board, destination, value);
        source += 4;
        destination += 4;
    }
}

/* The affine calls build the matrix of a rotation and a scale in 8.8 fixed point, from an angle whose low
   byte is dropped, giving 256 steps around the circle. */
static void affine(int32_t* pa, int32_t* pb, int32_t* pc, int32_t* pd, int32_t sx, int32_t sy, uint32_t angle) {
    double theta = (double)((angle >> 8) & 0xFF) * (2.0 * AGB_PI / 256.0);
    int64_t cs = llround(cos(theta) * 16384.0), sn = llround(sin(theta) * 16384.0);
    *pa = (int32_t)(sx * cs >> 14);
    *pb = (int32_t)(-(int64_t)sx * sn >> 14);
    *pc = (int32_t)(sy * sn >> 14);
    *pd = (int32_t)(sy * cs >> 14);
}

/* BgAffineSet: r2 entries of origin (24.8), screen coordinates, scale (8.8) and angle, 20 bytes each,
   written as the matrix and the start coordinate (24.8), 16 bytes each. */
static void bios_bg_affine_set(AgbBoard* board, uint32_t regs[16]) {
    uint32_t source = regs[0], destination = regs[1];
    for (uint32_t count = regs[2]; count; --count) {
        int32_t ox = (int32_t)agb_read32(board, source);
        int32_t oy = (int32_t)agb_read32(board, source + 4);
        int32_t cx = (int16_t)agb_read16(board, source + 8);
        int32_t cy = (int16_t)agb_read16(board, source + 10);
        int32_t a, b, c, d;
        affine(&a, &b, &c, &d, (int16_t)agb_read16(board, source + 12), (int16_t)agb_read16(board, source + 14),
               agb_read16(board, source + 16));
        agb_write16(board, destination, (uint16_t)a);
        agb_write16(board, destination + 2, (uint16_t)b);
        agb_write16(board, destination + 4, (uint16_t)c);
        agb_write16(board, destination + 6, (uint16_t)d);
        agb_write32(board, destination + 8, (uint32_t)(ox - (int32_t)(((int64_t)a * cx + (int64_t)b * cy) >> 8)));
        agb_write32(board, destination + 12, (uint32_t)(oy - (int32_t)(((int64_t)c * cx + (int64_t)d * cy) >> 8)));
        source += 20;
        destination += 16;
    }
}

/* ObjAffineSet: r2 entries of scale (8.8) and angle, 8 bytes each, written as four matrix halfwords with
   the stride r3 names, the layout OAM's affine parameters have. */
static void bios_obj_affine_set(AgbBoard* board, uint32_t regs[16]) {
    uint32_t source = regs[0], destination = regs[1], stride = regs[3];
    for (uint32_t count = regs[2]; count; --count) {
        int32_t a, b, c, d;
        affine(&a, &b, &c, &d, (int16_t)agb_read16(board, source), (int16_t)agb_read16(board, source + 2),
               agb_read16(board, source + 4));
        agb_write16(board, destination, (uint16_t)a);
        agb_write16(board, destination + stride, (uint16_t)b);
        agb_write16(board, destination + stride * 2, (uint16_t)c);
        agb_write16(board, destination + stride * 3, (uint16_t)d);
        source += 8;
        destination += stride * 4;
    }
}

/* BitUnPack widens the packed fields of the source, adding r2's offset to every field that is not zero
   when the offset only applies to nonzero fields. */
static void bios_bit_unpack(AgbBoard* board, uint32_t regs[16]) {
    uint32_t source = regs[0], destination = regs[1], info = regs[2];
    uint32_t length = agb_read16(board, info);
    uint32_t in_width = agb_read8(board, info + 2), out_width = agb_read8(board, info + 3);
    uint32_t offset = agb_read32(board, info + 4);
    uint32_t accumulator = 0, mask;
    unsigned held = 0;
    int zero_only = (int)(offset >> 31 & 1);
    if ((in_width != 1 && in_width != 2 && in_width != 4 && in_width != 8) ||
        (out_width != 1 && out_width != 2 && out_width != 4 && out_width != 8 && out_width != 16 && out_width != 32) ||
        out_width < in_width)
        return;
    offset &= 0x7FFFFFFF;
    mask = in_width == 8 ? 0xFFu : (1u << in_width) - 1;
    for (uint32_t byte = 0; byte < length; ++byte) {
        uint32_t unit = agb_read8(board, source++);
        for (uint32_t bit = 0; bit < 8; bit += in_width) {
            uint32_t value = unit >> bit & mask;
            if (value || !zero_only) value = (value + offset) & (out_width == 32 ? 0xFFFFFFFFu : (1u << out_width) - 1);
            accumulator |= value << held;
            held += out_width;
            if (held == 32) {
                agb_write32(board, destination, accumulator);
                destination += 4;
                accumulator = 0;
                held = 0;
            }
        }
    }
    if (held) {
        if (held <= 8)
            agb_write8(board, destination, (uint8_t)accumulator);
        else if (held <= 16)
            agb_write16(board, destination, (uint16_t)accumulator);
        else
            agb_write32(board, destination, accumulator);
    }
}

/* The compression streams share a header holding the decompressed size and a signature nibble. Each
   variant writes bytes or halfwords; the VRAM bus only takes halfwords. */
static void put_byte(AgbBoard* board, uint32_t address, uint8_t value, int halfwords, uint16_t* held, int* count) {
    if (!halfwords) {
        agb_write8(board, address, value);
        return;
    }
    *held = (uint16_t)(*held | value << (8 * *count));
    *count ^= 1;
    if (!*count) {
        agb_write16(board, address & ~(uint32_t)1, *held);
        *held = 0;
    }
}

static void bios_lz77_uncomp(AgbBoard* board, uint32_t regs[16], int halfwords) {
    uint32_t source = regs[0], destination = regs[1], size = agb_read32(board, source) >> 8, written = 0;
    uint16_t held = 0;
    int count = 0;
    source += 4;
    while (written < size) {
        uint32_t flags = agb_read8(board, source++);
        for (uint32_t bit = 8; bit && written < size; --bit) {
            uint8_t value;
            if (flags & 0x80) {
                uint32_t first = agb_read8(board, source++), second = agb_read8(board, source++);
                uint32_t length = (first >> 4) + 3, distance = ((first & 0xF) << 8 | second) + 1;
                while (length-- && written < size) {
                    value = agb_read8(board, destination - distance);
                    put_byte(board, destination, value, halfwords, &held, &count);
                    ++destination;
                    ++written;
                }
            } else {
                value = agb_read8(board, source++);
                put_byte(board, destination, value, halfwords, &held, &count);
                ++destination;
                ++written;
            }
            flags <<= 1;
        }
    }
    if (halfwords && count) agb_write16(board, destination & ~(uint32_t)1, held);
}

static void bios_rl_uncomp(AgbBoard* board, uint32_t regs[16], int halfwords) {
    uint32_t source = regs[0], destination = regs[1], size = agb_read32(board, source) >> 8, written = 0;
    uint16_t held = 0;
    int count = 0;
    source += 4;
    while (written < size) {
        uint32_t flags = agb_read8(board, source++);
        if (flags & 0x80) {
            uint32_t length = (flags & 0x7F) + 3;
            uint8_t value = agb_read8(board, source++);
            while (length-- && written < size) {
                put_byte(board, destination, value, halfwords, &held, &count);
                ++destination;
                ++written;
            }
        } else {
            uint32_t length = (flags & 0x7F) + 1;
            while (length-- && written < size) {
                uint8_t value = agb_read8(board, source++);
                put_byte(board, destination, value, halfwords, &held, &count);
                ++destination;
                ++written;
            }
        }
    }
    if (halfwords && count) agb_write16(board, destination & ~(uint32_t)1, held);
}

/* The difference filters undo a delta encoding: every unit is the difference to the one before it. The
   VRAM variant widens filtered bytes to halfwords. */
static void bios_diff_unfilter(AgbBoard* board, uint32_t regs[16], int in_width, int out_width) {
    uint32_t source = regs[0] & ~(uint32_t)3, destination = regs[1];
    uint32_t remaining = agb_read32(board, source) >> 8, previous = 0, pair = 0;
    uint16_t held = 0;
    source += 4;
    while (remaining) {
        uint32_t value = in_width == 2 ? agb_read16(board, source) : agb_read8(board, source);
        value = in_width == 2 ? (uint16_t)(value + previous) : (uint8_t)(value + previous);
        previous = value;
        source += in_width;
        if (out_width == 2 && in_width == 1) {
            held = (uint16_t)(held | value << (8 * pair));
            if (++pair == 2) {
                agb_write16(board, destination, held);
                destination += 2;
                remaining -= 2;
                pair = 0;
                held = 0;
            }
        } else if (out_width == 1) {
            agb_write8(board, destination, (uint8_t)value);
            ++destination;
            --remaining;
        } else {
            agb_write16(board, destination, (uint16_t)value);
            destination += 2;
            remaining -= 2;
        }
    }
    regs[0] = source;
    regs[1] = destination;
}

/* MidiKey2Freq plays the wave header's sample rate at a MIDI key: r1 is the key, r2's high byte the
   fraction of it. */
static void bios_midi_key_to_freq(AgbBoard* board, uint32_t regs[16]) {
    uint32_t frequency = agb_read32(board, regs[0] + 4);
    double key = (double)regs[1] + (double)(regs[2] & 0xFF) / 256.0;
    regs[0] = (uint32_t)(frequency / pow(2.0, (180.0 - key) / 12.0));
}

/* RegisterRamReset: r0's flags clear the work RAMs, the video areas and the register file. The IWRAM
   clear leaves the top 0x200 bytes alone, where the stacks live. */
static void bios_register_ram_reset(AgbBoard* board, uint32_t regs[16]) {
    uint32_t flags = regs[0];
    if (flags & 0x01) memset(board->memory.ewram, 0, AGB_EWRAM_SIZE);
    if (flags & 0x02) memset(board->memory.iwram, 0, AGB_IWRAM_SIZE - 0x200);
    if (flags & 0x04) memset(board->memory.palette, 0, AGB_PALETTE_SIZE);
    if (flags & 0x08) memset(board->memory.vram, 0, AGB_VRAM_SIZE);
    if (flags & 0x10) memset(board->memory.oam, 0, AGB_OAM_SIZE);
    if (flags & 0xE0) memset(board->io, 0, sizeof(board->io));
}

typedef struct {
    uint8_t number;
    const char* name;
    int stub; /* a known call with no HLE implementation yet */
} AgbBiosCall;

static const AgbBiosCall calls[] = {
    {AGB_SWI_SOFT_RESET, "SoftReset", 1},
    {AGB_SWI_REGISTER_RAM_RESET, "RegisterRamReset", 0},
    {AGB_SWI_HALT, "Halt", 1},
    {AGB_SWI_STOP, "Stop", 1},
    {AGB_SWI_INTR_WAIT, "IntrWait", 1},
    {AGB_SWI_VBLANK_INTR_WAIT, "VBlankIntrWait", 1},
    {AGB_SWI_DIV, "Div", 0},
    {AGB_SWI_DIV_ARM, "DivArm", 0},
    {AGB_SWI_SQRT, "Sqrt", 0},
    {AGB_SWI_ARCTAN, "ArcTan", 0},
    {AGB_SWI_ARCTAN2, "ArcTan2", 0},
    {AGB_SWI_CPU_SET, "CpuSet", 0},
    {AGB_SWI_CPU_FAST_SET, "CpuFastSet", 0},
    {AGB_SWI_GET_BIOS_CHECKSUM, "GetBiosChecksum", 0},
    {AGB_SWI_BG_AFFINE_SET, "BgAffineSet", 0},
    {AGB_SWI_OBJ_AFFINE_SET, "ObjAffineSet", 0},
    {AGB_SWI_BIT_UNPACK, "BitUnPack", 0},
    {AGB_SWI_LZ77_UNCOMP_WRAM, "LZ77UnCompWram", 0},
    {AGB_SWI_LZ77_UNCOMP_VRAM, "LZ77UnCompVram", 0},
    {AGB_SWI_HUFF_UNCOMP, "HuffUnComp", 1},
    {AGB_SWI_RL_UNCOMP_WRAM, "RLUnCompWram", 0},
    {AGB_SWI_RL_UNCOMP_VRAM, "RLUnCompVram", 0},
    {AGB_SWI_DIFF_8BIT_UNFILTER_WRAM, "Diff8bitUnFilterWram", 0},
    {AGB_SWI_DIFF_8BIT_UNFILTER_VRAM, "Diff8bitUnFilterVram", 0},
    {AGB_SWI_DIFF_16BIT_UNFILTER, "Diff16bitUnFilter", 0},
    {AGB_SWI_SOUND_BIAS, "SoundBias", 1},
    {AGB_SWI_MIDI_KEY_2_FREQ, "MidiKey2Freq", 0},
    {AGB_SWI_SOUND_DRIVER_GET_JUMP_LIST, "SoundDriverGetJumpList", 1},
    {AGB_SWI_SOUND_DRIVER_GET_VERSION, "SoundDriverGetVersion", 1},
};

const char* agb_bios_name(uint32_t swi) {
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
        if (calls[i].number == swi) return calls[i].name;
    return NULL;
}

int agb_bios_call(AgbBoard* board, uint32_t swi, uint32_t regs[16]) {
    const char* name = agb_bios_name(swi);
    if (!name) return -1;
    switch (swi) {
        case AGB_SWI_REGISTER_RAM_RESET: bios_register_ram_reset(board, regs); break;
        case AGB_SWI_DIV: bios_div(regs, (int32_t)regs[0], (int32_t)regs[1]); break;
        case AGB_SWI_DIV_ARM: bios_div(regs, (int32_t)regs[1], (int32_t)regs[0]); break;
        case AGB_SWI_SQRT: regs[0] = bios_sqrt(regs[0]); break;
        case AGB_SWI_ARCTAN: regs[0] = (uint32_t)bios_arctan((int32_t)regs[0]); break;
        case AGB_SWI_ARCTAN2: regs[0] = (uint32_t)bios_arctan2((int32_t)regs[0], (int32_t)regs[1]); break;
        case AGB_SWI_CPU_SET: bios_cpu_set(board, regs); break;
        case AGB_SWI_CPU_FAST_SET: bios_cpu_fast_set(board, regs); break;
        case AGB_SWI_GET_BIOS_CHECKSUM: regs[0] = 0xBAAE187F; break;
        case AGB_SWI_BG_AFFINE_SET: bios_bg_affine_set(board, regs); break;
        case AGB_SWI_OBJ_AFFINE_SET: bios_obj_affine_set(board, regs); break;
        case AGB_SWI_BIT_UNPACK: bios_bit_unpack(board, regs); break;
        case AGB_SWI_LZ77_UNCOMP_WRAM: bios_lz77_uncomp(board, regs, 0); break;
        case AGB_SWI_LZ77_UNCOMP_VRAM: bios_lz77_uncomp(board, regs, 1); break;
        case AGB_SWI_RL_UNCOMP_WRAM: bios_rl_uncomp(board, regs, 0); break;
        case AGB_SWI_RL_UNCOMP_VRAM: bios_rl_uncomp(board, regs, 1); break;
        case AGB_SWI_DIFF_8BIT_UNFILTER_WRAM: bios_diff_unfilter(board, regs, 1, 1); break;
        case AGB_SWI_DIFF_8BIT_UNFILTER_VRAM: bios_diff_unfilter(board, regs, 1, 2); break;
        case AGB_SWI_DIFF_16BIT_UNFILTER: bios_diff_unfilter(board, regs, 2, 2); break;
        case AGB_SWI_MIDI_KEY_2_FREQ: bios_midi_key_to_freq(board, regs); break;
        default: {
            /* the table entry says whether the call is a stub */
            for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
                if (calls[i].number == swi) return calls[i].stub;
            return -1;
        }
    }
    return 0;
}
