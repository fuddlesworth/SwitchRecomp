/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* EAC R11 and RG11 blocks from rEden's decode_eac.cpp: the KDFS 1.3 examples and edenprobe's */
#include "nx/gpu/texture/etc2.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;

/* base, multiplier and table, then sixteen 3-bit indices in row-major order; the block stores them by column */
static void block(u8 out[8], u32 base, u32 multiplier, u32 table, const u8 indices[16]) {
    u32 k;
    u32 i;
    u64 word = (u64)(base & 0xFF) << 56 | (u64)(multiplier & 0xF) << 52 | (u64)(table & 0xF) << 48;
    for (k = 0; k < 16; ++k) word |= (u64)(indices[(k % 4) * 4 + k / 4] & 7u) << (45 - 3 * k);
    for (i = 0; i < 8; ++i) out[i] = (u8)(word >> (56 - 8 * i));
}
static void fill(u8 indices[16], u8 index) { memset(indices, index, 16); }

static void expect(u32 got, u32 want, const char* what) {
    ++checks;
    if (got == want) return;
    fprintf(stderr, "FAIL: %s is %u (%04x), expected %u (%04x)\n", what, got, got, want, want);
    ++failures;
}

/* texel `texel` of one 4x4 R11 block */
static u16 decode_one(int kind, u32 base, u32 multiplier, u32 table, u8 index, u32 texel) {
    u8 blk[8], indices[16];
    u16 texels[16];
    fill(indices, index);
    block(blk, base, multiplier, table, indices);
    etc2_decode_image(blk, 4, 4, kind, (u8*)texels);
    return texels[texel];
}

static void worked_examples(void) {
    /* unsigned: 103 * 8 + 4 + (-10 * 2 * 8) = 668, widened to 21386 */
    expect(decode_one(EAC_R, 103, 2, 13, 3, 5), 21386, "R11 unsigned, base 103, multiplier 2");
    /* a multiplier of nought counts as one eighth: 103 * 8 + 4 - 10 = 818 */
    expect(decode_one(EAC_R, 103, 0, 13, 3, 5), (818 << 5) | (818 >> 6), "R11 unsigned, multiplier 0");
    /* signed: 60 * 8 + (-10 * 2 * 8) = 320; 60 * 8 - 10 = 470, widened on the magnitude to 15054 */
    expect(decode_one(EAC_R_SNORM, 60, 2, 13, 3, 0), (320 << 5) | (320 >> 5), "R11 signed, base 60, multiplier 2");
    expect(decode_one(EAC_R_SNORM, 60, 0, 13, 3, 0), 15054, "R11 signed, multiplier 0");
    /* -128 is read as -127: -127 * 8 + 0 = -1016, so -((1016 << 5) | (1016 >> 5)) */
    expect((u16)decode_one(EAC_R_SNORM, 0x80, 0, 13, 4, 0), (u16)-32543, "R11 signed, base -128");
    /* both clamps: -1023 and 1023 become -32767 and 32767 */
    expect((u16)decode_one(EAC_R_SNORM, 0x81, 15, 0, 3, 0), (u16)-32767, "R11 signed, clamped below");
    expect(decode_one(EAC_R_SNORM, 127, 15, 0, 7, 0), 32767, "R11 signed, clamped above");
}

/* edenprobe's RG11 block: red table 0 multiplier 11, green table 13 multiplier 0 */
static void every_index(void) {
    u32 i;
    u32 t;
    static const u16 red11[8] = {764, 500, 236, 0, 1204, 1468, 1732, 2047};
    static const u16 green11[8] = {397, 390, 389, 388, 378, 385, 386, 387};
    u8 forward[16], backward[16], both[16];
    u16 texels[32];
    char what[64];
    for (i = 0; i < 16; ++i) {
        forward[i] = (u8)(i & 7);
        backward[i] = (u8)(7 - (i & 7));
    }
    block(both, 0x80, 11, 0, forward);
    block(both + 8, 0x30, 0, 13, backward);
    etc2_decode_image(both, 4, 4, EAC_RG, (u8*)texels);
    for (t = 0; t < 16; ++t) {
        const u32 r = red11[t % 8], g = green11[t % 8];
        snprintf(what, sizeof(what), "RG11 texel %u red", t);
        expect(texels[2 * t], (r << 5) | (r >> 6), what);
        snprintf(what, sizeof(what), "RG11 texel %u green", t);
        expect(texels[2 * t + 1], (g << 5) | (g >> 6), what);
    }
}

/* a 6x5 two-layer image of constant blocks: every texel names its block, nothing lands past the end */
static void partial_blocks(void) {
    u32 layer;
    u32 b;
    size_t i;
    u32 y;
    u32 x;
    enum { SNX_EACB_W = 6, SNX_EACB_H = 5 };
    const u16 guard = 0xA5A5;
    u8 input[2][4][8], indices[16];
    u16 texels[SNX_EACB_W * SNX_EACB_H * 2 + SNX_EACB_W * 4];
    char what[64];
    fill(indices, 4);
    for (layer = 0; layer < 2; ++layer)
        for (b = 0; b < 4; ++b) block(input[layer][b], 10 + layer * 4 + b, 0, 13, indices);
    for (i = 0; i < sizeof(texels) / sizeof(texels[0]); ++i) texels[i] = guard;
    for (layer = 0; layer < 2; ++layer)
        etc2_decode_image(input[layer][0], SNX_EACB_W, SNX_EACB_H, EAC_R,
                          (u8*)(texels + layer * SNX_EACB_W * SNX_EACB_H));
    for (layer = 0; layer < 2; ++layer)
        for (y = 0; y < SNX_EACB_H; ++y)
            for (x = 0; x < SNX_EACB_W; ++x) {
                const u32 v = (10 + layer * 4 + (y / 4) * 2 + x / 4) * 8 + 4;
                snprintf(what, sizeof(what), "6x5 layer %u texel (%u, %u)", layer, x, y);
                expect(texels[(layer * SNX_EACB_H + y) * SNX_EACB_W + x], (v << 5) | (v >> 6), what);
            }
    for (i = SNX_EACB_W * SNX_EACB_H * 2; i < sizeof(texels) / sizeof(texels[0]); ++i)
        expect(texels[i], guard, "the guard past the last row");
}

int main(void) {
    worked_examples();
    every_index();
    partial_blocks();
    printf("EAC blocks from rEden's decode_eac test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
