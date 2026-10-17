/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC blocks from rEden's decoder tests: within one of the reference, exact with -DSNX_ASTCB_FINDINGS */
#include "nx/gpu/texture/astc.h"
#include <stdio.h>
#include <string.h>

#define SNX_ASTCB_MAGENTA     0xFFFF00FFu
/* a 4x4 LDR void extent block's colour: RGBA 0x2020, 0x4040, 0x8080, 0xFFFF */
#define SNX_ASTCB_VOID_COLOUR 0xFF804020u

typedef struct {
    u32 width, height;
    u8 bits[16];
} SnxAstcbBlock;

typedef struct {
    SnxAstcbBlock block;
    u32 texels[16]; /* a 4x4 block, row by row */
} SnxAstcbExpected;

static int failures, checks;

/* illegal blocks, at least one reaching each error exit of rEden's decoder */
static const SnxAstcbBlock snx_astcb_illegal_at_each_exit[] = {
    /* reserved block mode */
    {4, 4, {0x60, 0x8d, 0x30, 0xbe, 0x55, 0x15, 0x91, 0xb8, 0xaa, 0x9f, 0x57, 0xe0, 0x8b, 0x62, 0xcf, 0xd2}},
    /* void extent with a reserved bit clear */
    {4, 4, {0xfc, 0x35, 0x48, 0x6f, 0xe7, 0x3d, 0x07, 0xcc, 0xb0, 0xd0, 0xc6, 0x2a, 0x86, 0x0d, 0x41, 0xcb}},
    /* HDR void extent */
    {4, 4, {0xfc, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x62, 0x24, 0xa8, 0xfc, 0x1e, 0x2e, 0x36, 0x95}},
    /* weight grid wider than the footprint */
    {4, 4, {0xc3, 0xb3, 0xd0, 0x66, 0x13, 0x34, 0x5a, 0xf5, 0x54, 0x55, 0xcf, 0xad, 0x5b, 0x91, 0xb3, 0xf6}},
    /* weight grid taller than the footprint */
    {4, 4, {0x8d, 0xc4, 0x02, 0x4e, 0xf7, 0x8b, 0x98, 0x13, 0x54, 0x0d, 0x10, 0x38, 0xd4, 0x61, 0x74, 0x97}},
    /* more than 64 weights */
    {12, 12, {0xfb, 0x64, 0x30, 0x9d, 0xa4, 0x79, 0xaf, 0xc3, 0xb4, 0xf6, 0x9c, 0x4c, 0xff, 0x77, 0xd1, 0x1d}},
    /* dual plane with four partitions */
    {4, 4, {0x02, 0xbe, 0x96, 0x98, 0xf4, 0xdd, 0x26, 0x93, 0x19, 0x00, 0xaf, 0x21, 0x3f, 0x73, 0xe4, 0xbb}},
    /* fewer than 24 weight bits */
    {4, 4, {0x3d, 0x19, 0x66, 0x7b, 0x85, 0x93, 0x20, 0x2f, 0x7e, 0x0c, 0x35, 0x9b, 0x4e, 0x6e, 0x41, 0xd2}},
    /* more than 96 weight bits */
    {4, 4, {0xde, 0xc7, 0x51, 0xc7, 0x9a, 0x69, 0x83, 0xf1, 0x2d, 0xec, 0xe7, 0xe5, 0xac, 0x93, 0xce, 0x2a}},
};

/* blocks with an HDR endpoint mode in one partition: the error colour there only */
static const SnxAstcbExpected snx_astcb_hdr_partitions[] = {
    /* edenprobe's hdrfirst: mode 7 then mode 9, dual plane */
    {{4, 4, {0x2e, 0x4f, 0x2d, 0xfd, 0x10, 0xda, 0x68, 0xca, 0xb6, 0x76, 0x8c, 0x2b, 0xcc, 0xf2, 0x51, 0xeb}},
     {SNX_ASTCB_MAGENTA, 0xffe5db74, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, 0xffe0db73,
      SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, 0xffdfdb73, 0xffe3db74, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, 0xffebdc76,
      SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA}},
    /* mode 10 then mode 7, dual plane */
    {{4, 4, {0x51, 0xac, 0x29, 0x53, 0xd8, 0x46, 0x65, 0x10, 0x12, 0xfc, 0x09, 0xdc, 0x71, 0x39, 0xd4, 0x99}},
     {SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, 0x663d7a06, 0xaa142806, 0x663d7a22,
      0x2266cc22, 0x2266cc22, 0xaa142814, 0x2266cc14, 0x2266cc14, 0xaa142806, 0xaa142814, 0xaa142814, 0x2266cc06}},
    /* mode 14 alone: its alpha endpoints are LDR, the texels are still the error colour */
    {{4, 4, {0x2f, 0xc3, 0x3b, 0xee, 0xa5, 0x4c, 0x6f, 0xd8, 0xd7, 0xbb, 0x2b, 0x23, 0x4c, 0x9c, 0xc6, 0x49}},
     {SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA,
      SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA,
      SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA, SNX_ASTCB_MAGENTA}},
};

/* colour integer sequences that end inside a trit or quint group, followed by unallocated bits */
typedef struct {
    SnxAstcbExpected expected;
    u32 first_unallocated, unallocated;
} SnxAstcbCutShort;

static const SnxAstcbCutShort snx_astcb_cut_short[] = {
    /* 12 values in trits (groups of 5, 5 and 2), bits 61 and 62 unallocated */
    {{{4, 4, {0xcf, 0x15, 0x76, 0x88, 0xf5, 0x26, 0x4f, 0xfe, 0x80, 0x6f, 0x4a, 0xbe, 0x55, 0x83, 0xbd, 0x56}},
      {0x70af6faf, 0x8c596f59, 0x963d6f3d, 0x8f506f50, 0x9933cc33, 0x7c89a389, 0x70af87af, 0x50708570, 0xccffe2ff,
       0xdad6b5d6, 0xe3ba98ba, 0xeba283a2, 0xeba2ffa2, 0xdccfffcf, 0xdccfcfcf, 0xeba266a2}},
     61,
     2},
    /* 14 values in quints (groups of 3, 3, 3, 3 and 2), bits 76 and 77 unallocated */
    {{{4, 4, {0xce, 0x0b, 0xe4, 0x05, 0x7d, 0x5b, 0x47, 0x91, 0xd2, 0x7d, 0xa3, 0xe2, 0x5e, 0x47, 0x70, 0xf8}},
      {0x307f8d7f, 0x356d8476, 0x356d8476, 0xffe1c3a8, 0x38637f71, 0x37668072, 0x356e8476, 0x317d8c7e, 0x3376887a,
       0x34718678, 0x34708577, 0x34708577, 0x366a8375, 0x366c8375, 0x34718678, 0x327a8a7c}},
     76,
     2},
};

/* illegal by their colour values alone */
static const SnxAstcbBlock snx_astcb_illegal_colour[] = {
    /* edenprobe's error: 10 values in 15 colour bits, where six levels need 26 */
    {4, 4, {0x5f, 0x0f, 0x35, 0xcb, 0xe9, 0x15, 0x32, 0xa7, 0x3f, 0xdc, 0x63, 0xdb, 0x94, 0x74, 0x55, 0xe2}},
    /* 10 values in 25 colour bits, one short */
    {4, 4, {0x5e, 0xef, 0x7a, 0xf3, 0x2a, 0x3c, 0x46, 0x89, 0x67, 0xd8, 0xf8, 0x37, 0x9e, 0x70, 0x87, 0x56}},
    /* 6 values, and the other fields already take one bit more than the block has */
    {4, 4, {0x53, 0xac, 0xad, 0x9c, 0x64, 0xd9, 0x76, 0xa3, 0x85, 0xe7, 0x1d, 0xa0, 0xc5, 0x38, 0xa6, 0x23}},
    /* 20 values in 65 colour bits, where 52 would do */
    {4, 4, {0x9e, 0x1b, 0x78, 0xb3, 0x57, 0x10, 0x09, 0x3e, 0xf9, 0xa5, 0x0a, 0x28, 0xc8, 0xb0, 0x89, 0xe6}},
};

/* legal on both limits: 18 values in exactly the 47 colour bits six levels need */
static const SnxAstcbExpected snx_astcb_colour_limits = {
    {4, 4, {0xde, 0x33, 0x14, 0xd0, 0xd9, 0xd4, 0xd5, 0x66, 0xbb, 0xdc, 0xcd, 0x33, 0xe2, 0xd9, 0xb9, 0xb9}},
    {0xff9707c2, 0xff7a5d50, 0xffd8c697, 0xff7d545c, 0xff68920a, 0xff71782d, 0xffdec3a2, 0xff7d545c, 0xff8f1fa3,
     0xff766940, 0xff786446, 0xff940eb9, 0xff8a2d8f, 0xffedbbc2, 0xff873783, 0xff873783}};

/* mode 1, dual plane: 17 channels where C >> 8 and the nearest byte differ */
static const SnxAstcbExpected snx_astcb_rounding = {
    {4, 4, {0xcf, 0x25, 0x82, 0x11, 0xd3, 0x9e, 0xca, 0x7c, 0x05, 0xc8, 0x1c, 0x0c, 0x6b, 0x99, 0x84, 0x0a}},
    {0xffb2b0b0, 0xffb6b6b6, 0xffb8b6b6, 0xffb8b0b0, 0xffb7b8b8, 0xffb3b5b5, 0xffb2b4b4, 0xffb3b5b5, 0xffb3b3b3,
     0xffb1b7b7, 0xffb2b7b7, 0xffb5b3b3, 0xffb3b0b0, 0xffb1b6b6, 0xffb1b6b6, 0xffb2b0b0}};

/* astc_decoder_gpu.cpp: 4x4 blocks, each texel astcenc's */
static const SnxAstcbExpected snx_astcb_astcenc[] = {
    /* 4-bit weights, dual plane, RGBA endpoints */
    {{4, 4, {0x02, 0xa6, 0xbb, 0x3d, 0x74, 0x25, 0x24, 0xf3, 0x14, 0xe3, 0x6f, 0x68, 0xe0, 0xd9, 0x5d, 0x17}},
     {0xafecfdd5, 0xaaebfdd7, 0xa6ebfed8, 0x95ecfcd4, 0xa8ecfdd5, 0xacecfdd6, 0xa8ebfdd7, 0x96ecfcd4, 0x9fecfcd3,
      0xafecfcd4, 0xaaecfdd5, 0x98ecfdd5, 0x97edfcd3, 0xb1edfcd3, 0xacecfcd4, 0x99ecfdd5}},
    /* 4-bit weights, luminance */
    {{4, 4, {0xae, 0x23, 0x64, 0x6a, 0x5d, 0xb6, 0xa9, 0x70, 0x26, 0x76, 0xc8, 0x1e, 0xc6, 0x3c, 0x03, 0x5d}},
     {0xffb0b0b0, 0xffb2b2b2, 0xffa7a7a7, 0xff8c8c8c, 0xffb5b5b5, 0xffb5b5b5, 0xffababab, 0xff939393, 0xffadadad,
      0xffafafaf, 0xffababab, 0xff9c9c9c, 0xff969696, 0xff9d9d9d, 0xffa3a3a3, 0xffa9a9a9}},
    {{4, 4, {0x2e, 0x03, 0xe0, 0x31, 0x1c, 0xa8, 0x20, 0x4a, 0x4c, 0xaa, 0xaa, 0x41, 0xb3, 0x3c, 0x2c, 0x72}},
     {0xff262626, 0xff515151, 0xff8b8b8b, 0xffb7b7b7, 0xff8b8b8b, 0xff9f9f9f, 0xffadadad, 0xffc1c1c1, 0xff929292,
      0xffa6a6a6, 0xffb3b3b3, 0xffc8c8c8, 0xff414141, 0xff696969, 0xff9c9c9c, 0xffc8c8c8}},
    /* 5-bit weights */
    {{4, 4, {0xbf, 0x03, 0x12, 0xe5, 0x6f, 0x01, 0x19, 0xeb, 0xca, 0x9e, 0x90, 0x67, 0xde, 0x47, 0xc0, 0x05}},
     {0xff898989, 0xffcccccc, 0xffcccccc, 0xff898989, 0xff939393, 0xffd1d1d1, 0xffe2e2e2, 0xffcccccc, 0xffadadad,
      0xffdbdbdb, 0xffe3e3e3, 0xffd6d6d6, 0xffdedede, 0xffe5e5e5, 0xffd4d4d4, 0xffa7a7a7}},
    /* 5-bit weights, dual plane, RGB endpoints */
    {{4, 4, {0xbf, 0x07, 0x5b, 0x3a, 0x38, 0xe6, 0x42, 0x47, 0xb4, 0x29, 0xe3, 0x5f, 0x6f, 0x11, 0x53, 0x2f}},
     {0xff0cc2b6, 0xff0ebeb6, 0xff13b5b6, 0xff1ba4b6, 0xff14b2b6, 0xff17adb6, 0xff1aa7b6, 0xff1da0b6, 0xff1ba4b6,
      0xff1ba4b6, 0xff1ca3b6, 0xff1e9fb6, 0xff2297b6, 0xff1ba4b6, 0xff19a9b6, 0xff1ca2b6}},
    {{4, 4, {0x53, 0x22, 0xd2, 0xd2, 0x9e, 0xef, 0x1c, 0xdd, 0xf9, 0xbe, 0xf7, 0xc6, 0x57, 0x68, 0x53, 0x96}},
     {0xff666666, 0xff747474, 0xff727272, 0xff6b6b6b, 0xff5b5b5b, 0xff767676, 0xff6d6d6d, 0xff696969, 0xff6d6d6d,
      0xff6d6d6d, 0xff838383, 0xff828282, 0xff7b7b7b, 0xff808080, 0xff5d5d5d, 0xff636363}},
    /* RGBA endpoints in quints (QUANT_160), the last quint block cut short */
    {{4, 4, {0xde, 0xa3, 0x03, 0xf8, 0x86, 0x8d, 0xd8, 0x9a, 0x38, 0x61, 0x1a, 0xb2, 0xe9, 0xf1, 0xd9, 0x7c}},
     {0x7dcfb1a4, 0x85d9b3ab, 0x89dcb4ad, 0x87dab4ac, 0x78c9afa1, 0x82d4b2a8, 0x84d7b3aa, 0x80d2b2a7, 0x8adeb5ae,
      0x86d9b3ab, 0x85d9b3ab, 0x89ddb4ae, 0x7bcdb0a3, 0x85d9b3ab, 0x86d9b4ab, 0x7dcfb1a4}},
    /* RGBA endpoints in trits (QUANT_96), the last trit block cut short; dual plane */
    {{4, 4, {0xbf, 0x85, 0x81, 0x0d, 0xc2, 0x15, 0x22, 0xf5, 0xa1, 0xf1, 0xaa, 0x48, 0x84, 0x38, 0xa9, 0xda}},
     {0xa8a2225a, 0xc6761e83, 0xb88a1a70, 0x7ddf1621, 0xe34e15a9, 0xac9b2161, 0x89ce2131, 0x6ff3160e, 0xd75e1499,
      0xa5a52057, 0x9ab62147, 0x97ba1644, 0x7ddf1c21, 0xb88a1c70, 0xe34e1aa9, 0xff2516cf}},
};

/* a block's texels as R | G << 8 | B << 16 | A << 24, row by row */
static void decode(const SnxAstcbBlock* block, u32* texels) {
    u32 i;
    u8 pixels[12 * 12 * 4];
    astc_decode_image(block->bits, block->width, block->height, block->width, block->height, pixels, 0);
    for (i = 0; i < block->width * block->height; ++i)
        texels[i] =
            pixels[4 * i] | (u32)pixels[4 * i + 1] << 8 | (u32)pixels[4 * i + 2] << 16 | (u32)pixels[4 * i + 3] << 24;
}

static void fail(const char* what, unsigned index, u32 texel, u32 got, u32 want) {
    fprintf(stderr, "FAIL: %s %u: texel %u is %08x, expected %08x\n", what, index, texel, got, want);
    ++failures;
}

/* every texel of block as want[i], or as fill when want is NULL */
static void expect(const SnxAstcbBlock* block, const u32* want, u32 fill, const char* what, unsigned index) {
    u32 i;
    u32 got[12 * 12];
    decode(block, got);
    ++checks;
    for (i = 0; i < block->width * block->height; ++i) {
        u32 w = want ? want[i] : fill;
        if (got[i] != w) {
            fail(what, index, i, got[i], w);
            return;
        }
    }
}

/* the error colour exact, every other channel within one */
static void expect_near(const SnxAstcbBlock* block, const u32* want, const char* what, unsigned index) {
    u32 i;
    u32 got[12 * 12];
    decode(block, got);
    ++checks;
    for (i = 0; i < block->width * block->height; ++i) {
        u32 c;
        int near = (got[i] == SNX_ASTCB_MAGENTA) == (want[i] == SNX_ASTCB_MAGENTA);
        for (c = 0; c < 32 && near; c += 8) {
            int d = (int)((got[i] >> c) & 0xFF) - (int)((want[i] >> c) & 0xFF);
            near = d >= -1 && d <= 1;
        }
        if (!near) {
            fail(what, index, i, got[i], want[i]);
            return;
        }
    }
}

/* a 4x4 LDR void extent: block mode 0xDFC, the four 13-bit coordinates, then the colour */
static SnxAstcbBlock void_extent(u32 s0, u32 s1, u32 t0, u32 t1) {
    u32 k;
    const u64 low = 0xDFCull | (u64)s0 << 12 | (u64)s1 << 25 | (u64)t0 << 38 | (u64)t1 << 51;
    const u64 high = 0xFFFF808040402020ull;
    SnxAstcbBlock block = {4, 4, {0}};
    for (k = 0; k < 8; ++k) {
        block.bits[k] = (u8)(low >> (8 * k));
        block.bits[8 + k] = (u8)(high >> (8 * k));
    }
    return block;
}

int main(void) {
    size_t i;
    const size_t illegal = sizeof(snx_astcb_illegal_at_each_exit) / sizeof(snx_astcb_illegal_at_each_exit[0]);

    /* guest data reaches every one of these exits; each fills its block with opaque magenta */
    for (i = 0; i < illegal; ++i)
        expect(&snx_astcb_illegal_at_each_exit[i], NULL, SNX_ASTCB_MAGENTA, "illegal block", (unsigned)i);

    /* an HDR endpoint mode is the error colour on its partition only */
    for (i = 0; i < sizeof(snx_astcb_hdr_partitions) / sizeof(snx_astcb_hdr_partitions[0]); ++i) {
        expect_near(&snx_astcb_hdr_partitions[i].block, snx_astcb_hdr_partitions[i].texels, "HDR partition block",
                    (unsigned)i);
#ifdef SNX_ASTCB_FINDINGS
        expect(&snx_astcb_hdr_partitions[i].block, snx_astcb_hdr_partitions[i].texels, 0, "HDR partition block",
               (unsigned)i);
#endif
    }

    /* a trit or quint group cut short reads zeros: any unallocated bits decode the same */
    for (i = 0; i < sizeof(snx_astcb_cut_short) / sizeof(snx_astcb_cut_short[0]); ++i) {
        u32 value;
        const SnxAstcbCutShort* cut = &snx_astcb_cut_short[i];
        u32 own[16];
        decode(&cut->expected.block, own);
        expect_near(&cut->expected.block, cut->expected.texels, "cut-short block", (unsigned)i);
        for (value = 0; value < (1u << cut->unallocated); ++value) {
            u32 k;
            SnxAstcbBlock block = cut->expected.block;
            for (k = 0; k < cut->unallocated; ++k) {
                const u32 bit = cut->first_unallocated + k;
                block.bits[bit / 8] =
                    (u8)((block.bits[bit / 8] & ~(1u << (bit % 8))) | ((value >> k) & 1u) << (bit % 8));
            }
            expect(&block, own, 0, "cut-short block against its own bits, unallocated bits", (unsigned)(i * 4 + value));
#ifdef SNX_ASTCB_FINDINGS
            expect(&block, cut->expected.texels, 0, "cut-short block, unallocated bits", (unsigned)(i * 4 + value));
#endif
        }
    }

    /* more than 18 colour values, or too few colour bits, are illegal; exactly enough is legal */
    for (i = 0; i < sizeof(snx_astcb_illegal_colour) / sizeof(snx_astcb_illegal_colour[0]); ++i)
        expect(&snx_astcb_illegal_colour[i], NULL, SNX_ASTCB_MAGENTA, "illegal colour block", (unsigned)i);
    expect_near(&snx_astcb_colour_limits.block, snx_astcb_colour_limits.texels, "colour limits block", 0);
#ifdef SNX_ASTCB_FINDINGS
    expect(&snx_astcb_colour_limits.block, snx_astcb_colour_limits.texels, 0, "colour limits block", 0);
#endif

    /* a void extent needs each low coordinate below its high one, unless all four are ones */
    {
        size_t i;
        static const u32 cases[][5] = {
            {0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF, SNX_ASTCB_VOID_COLOUR}, /* all ones: no extent */
            {0x0100, 0x0200, 0x0300, 0x0400, SNX_ASTCB_VOID_COLOUR},
#ifdef SNX_ASTCB_FINDINGS
            {0x0200, 0x0200, 0x0300, 0x0400, SNX_ASTCB_MAGENTA},
            {0x0300, 0x0200, 0x0300, 0x0400, SNX_ASTCB_MAGENTA},
            {0x0100, 0x0200, 0x0400, 0x0400, SNX_ASTCB_MAGENTA},
            {0x1FFF, 0x1FFF, 0x1FFF, 0x1FFE, SNX_ASTCB_MAGENTA}, /* not all ones, so an extent out of order */
#endif
        };
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            SnxAstcbBlock block = void_extent(cases[i][0], cases[i][1], cases[i][2], cases[i][3]);
            expect(&block, NULL, cases[i][4], "void extent", (unsigned)i);
        }
    }

    /* the original's rounding block, where C >> 8 agrees with the reference */
    expect(&snx_astcb_rounding.block, snx_astcb_rounding.texels, 0, "rounding block", 0);

    /* 4 and 5-bit weights expand by bit replication; colour data cut short inside a trit or quint group reads zeros */
    for (i = 0; i < sizeof(snx_astcb_astcenc) / sizeof(snx_astcb_astcenc[0]); ++i) {
        expect_near(&snx_astcb_astcenc[i].block, snx_astcb_astcenc[i].texels, "astcenc block", (unsigned)i);
#ifdef SNX_ASTCB_FINDINGS
        expect(&snx_astcb_astcenc[i].block, snx_astcb_astcenc[i].texels, 0, "astcenc block", (unsigned)i);
#endif
    }

    printf("ASTC blocks from rEden's CPU and GPU decoder tests: %d blocks, %d failed\n", checks, failures);
    return failures != 0;
}
