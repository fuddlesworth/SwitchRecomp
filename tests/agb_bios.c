/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "bios/bios.h"
#include "board/board.h"
#include <stdio.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static AgbBoard board;

static int call(uint32_t* regs, uint32_t swi) {
    memset(regs + 4, 0, 12 * sizeof(uint32_t));
    return agb_bios_call(&board, swi, regs);
}

int main(void) {
    uint32_t regs[16];
    agb_board_init(&board);

    /* dispatch: implemented calls run, stubs are known calls, everything else is rejected */
    memset(regs, 0, sizeof(regs));
    CHECK(agb_bios_call(&board, AGB_SWI_DIV, regs) == 0);
    CHECK(agb_bios_call(&board, AGB_SWI_HALT, regs) == 1);
    CHECK(agb_bios_call(&board, 0x55, regs) == -1);
    CHECK(!strcmp(agb_bios_name(AGB_SWI_BG_AFFINE_SET), "BgAffineSet"));
    CHECK(!agb_bios_name(0x55));

    /* Div and DivArm */
    memset(regs, 0, sizeof(regs));
    regs[0] = 100;
    regs[1] = 7;
    CHECK(call(regs, AGB_SWI_DIV) == 0);
    CHECK(regs[0] == 14 && regs[1] == 2 && regs[3] == 14);
    regs[0] = (uint32_t)-100;
    regs[1] = 7;
    CHECK(call(regs, AGB_SWI_DIV) == 0);
    CHECK((int32_t)regs[0] == -14 && (int32_t)regs[1] == -2 && regs[3] == 14);
    regs[0] = (uint32_t)-100;
    regs[1] = 0;
    CHECK(call(regs, AGB_SWI_DIV) == 0);
    CHECK((int32_t)regs[0] == -1 && (int32_t)regs[1] == -100 && regs[3] == 1);
    regs[0] = 5; /* denominator */
    regs[1] = 30;
    CHECK(call(regs, AGB_SWI_DIV_ARM) == 0); /* DivArm takes the arguments the other way round */
    CHECK(regs[0] == 6 && regs[1] == 0 && regs[3] == 6);

    /* Sqrt rounds down */
    regs[0] = 1000;
    CHECK(call(regs, AGB_SWI_SQRT) == 0);
    CHECK(regs[0] == 31);
    regs[0] = 0xFFFFFFFF;
    CHECK(call(regs, AGB_SWI_SQRT) == 0);
    CHECK(regs[0] == 0xFFFF);

    /* ArcTan and ArcTan2: angles are turns in 16 bits */
    regs[0] = 0x4000; /* tan = 1: 45 degrees */
    CHECK(call(regs, AGB_SWI_ARCTAN) == 0);
    CHECK((int32_t)regs[0] == 0x2000);
    regs[0] = 100;
    regs[1] = 100;
    CHECK(call(regs, AGB_SWI_ARCTAN2) == 0);
    CHECK(regs[0] == 0x2000);
    regs[0] = 0;
    regs[1] = 5;
    CHECK(call(regs, AGB_SWI_ARCTAN2) == 0);
    CHECK(regs[0] == 0x4000);
    regs[0] = (uint32_t)-5;
    regs[1] = 0;
    CHECK(call(regs, AGB_SWI_ARCTAN2) == 0);
    CHECK(regs[0] == 0x8000);
    regs[0] = 5;
    regs[1] = (uint32_t)-5;
    CHECK(call(regs, AGB_SWI_ARCTAN2) == 0);
    CHECK(regs[0] == 0xE000);

    /* GetBiosChecksum names the BIOS revision the console reports */
    CHECK(call(regs, AGB_SWI_GET_BIOS_CHECKSUM) == 0);
    CHECK(regs[0] == 0xBAAE187F);

    /* CpuSet: halfwords, words and the fill modes */
    agb_write16(&board, 0x02000000, 0x1234);
    agb_write16(&board, 0x02000002, 0x5678);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000000;
    regs[1] = 0x03000100;
    regs[2] = 2 | (1u << 26);
    CHECK(call(regs, AGB_SWI_CPU_SET) == 0);
    CHECK(agb_read32(&board, 0x03000100) == 0x56781234);
    regs[0] = 0x02000000;
    regs[1] = 0x02000100;
    regs[2] = 4 | (1u << 24); /* fill with the first halfword */
    CHECK(call(regs, AGB_SWI_CPU_SET) == 0);
    CHECK(agb_read16(&board, 0x02000100) == 0x1234 && agb_read16(&board, 0x02000106) == 0x1234);

    /* CpuFastSet rounds its count up to eight words */
    for (int i = 0; i < 8; ++i) agb_write32(&board, 0x02000000 + (uint32_t)i * 4, 0x1000u + (uint32_t)i);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000000;
    regs[1] = 0x03000000;
    regs[2] = 1;
    CHECK(call(regs, AGB_SWI_CPU_FAST_SET) == 0);
    CHECK(agb_read32(&board, 0x03000000) == 0x1000);
    CHECK(agb_read32(&board, 0x0300001C) == 0x1007);

    /* BitUnPack: 4 bit fields to bytes, offset only on nonzero fields */
    agb_write16(&board, 0x02000000, 0x20F1); /* fields 1 F 0 2 */
    agb_write16(&board, 0x02000010, 2);      /* source length in bytes */
    agb_write8(&board, 0x02000012, 4);
    agb_write8(&board, 0x02000013, 8);
    agb_write32(&board, 0x02000014, 0x80000000u | 0x10);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000000;
    regs[1] = 0x02000200;
    regs[2] = 0x02000010;
    CHECK(call(regs, AGB_SWI_BIT_UNPACK) == 0);
    CHECK(agb_read8(&board, 0x02000200) == 0x11);
    CHECK(agb_read8(&board, 0x02000201) == 0x1F);
    CHECK(agb_read8(&board, 0x02000202) == 0x00);
    CHECK(agb_read8(&board, 0x02000203) == 0x12);

    /* LZ77: three literals and one back reference, through both bus widths */
    agb_write32(&board, 0x02000000, (8u << 8) | 0x10);
    agb_write8(&board, 0x02000004, 0x10); /* literals for bits 7..5, then a copy */
    agb_write8(&board, 0x02000005, 'A');
    agb_write8(&board, 0x02000006, 'B');
    agb_write8(&board, 0x02000007, 'C');
    agb_write8(&board, 0x02000008, 0x20); /* length 5, distance 2: B C B C B */
    agb_write8(&board, 0x02000009, 0x01);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000000;
    regs[1] = 0x02010000;
    CHECK(call(regs, AGB_SWI_LZ77_UNCOMP_WRAM) == 0);
    CHECK(!memcmp(&board.memory.ewram[0x10000], "ABCBCBCB", 8));
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000000;
    regs[1] = 0x06000000;
    CHECK(call(regs, AGB_SWI_LZ77_UNCOMP_VRAM) == 0);
    CHECK(!memcmp(&board.memory.vram[0], "ABCBCBCB", 8));

    /* RL: a run and a literal block */
    agb_write32(&board, 0x02000100, (6u << 8) | 0x30);
    agb_write8(&board, 0x02000104, 0x81); /* four Z */
    agb_write8(&board, 0x02000105, 'Z');
    agb_write8(&board, 0x02000106, 0x01); /* two literals */
    agb_write8(&board, 0x02000107, 'X');
    agb_write8(&board, 0x02000108, 'Y');
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000100;
    regs[1] = 0x02020000;
    CHECK(call(regs, AGB_SWI_RL_UNCOMP_WRAM) == 0);
    CHECK(!memcmp(&board.memory.ewram[0x20000], "ZZZZXY", 6));

    /* the difference filters undo the delta encoding */
    agb_write32(&board, 0x02000300, (4u << 8) | 0x80);
    agb_write8(&board, 0x02000304, 10);
    agb_write8(&board, 0x02000305, 5);
    agb_write8(&board, 0x02000306, 2);
    agb_write8(&board, 0x02000307, 1);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000300;
    regs[1] = 0x02030000;
    CHECK(call(regs, AGB_SWI_DIFF_8BIT_UNFILTER_WRAM) == 0);
    CHECK(agb_read8(&board, 0x02030000) == 10 && agb_read8(&board, 0x02030001) == 15);
    CHECK(agb_read8(&board, 0x02030002) == 17 && agb_read8(&board, 0x02030003) == 18);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000300;
    regs[1] = 0x06010000;
    CHECK(call(regs, AGB_SWI_DIFF_8BIT_UNFILTER_VRAM) == 0);
    CHECK(agb_read16(&board, 0x06010000) == 0x0F0A && agb_read16(&board, 0x06010002) == 0x1211);

    /* BgAffineSet: a 90 degree turn, no scale, the origin at the screen center */
    agb_write32(&board, 0x02000400, 128 << 8); /* origin x (24.8) */
    agb_write32(&board, 0x02000404, 64 << 8);  /* origin y */
    agb_write16(&board, 0x02000408, 120);      /* screen x */
    agb_write16(&board, 0x0200040A, 60);       /* screen y */
    agb_write16(&board, 0x0200040C, 256);      /* scale x (8.8) */
    agb_write16(&board, 0x0200040E, 256);      /* scale y */
    agb_write16(&board, 0x02000410, 0x4000);   /* 90 degrees */
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000400;
    regs[1] = 0x02000600;
    regs[2] = 1;
    CHECK(call(regs, AGB_SWI_BG_AFFINE_SET) == 0);
    CHECK((int16_t)agb_read16(&board, 0x02000600) == 0);    /* pa = cos */
    CHECK((int16_t)agb_read16(&board, 0x02000602) == -256); /* pb = -sin */
    CHECK((int16_t)agb_read16(&board, 0x02000604) == 256);  /* pc = sin */
    CHECK((int16_t)agb_read16(&board, 0x02000606) == 0);    /* pd = cos */
    CHECK((int32_t)agb_read32(&board, 0x02000608) == (128 << 8) - (-256 * 60 >> 8));
    CHECK((int32_t)agb_read32(&board, 0x0200060C) == (64 << 8) - (120 * 256 >> 8));

    /* ObjAffineSet writes the matrix with the stride OAM uses */
    agb_write16(&board, 0x02000500, 256);
    agb_write16(&board, 0x02000502, 256);
    agb_write16(&board, 0x02000504, 0);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000500;
    regs[1] = 0x07000000;
    regs[2] = 1;
    regs[3] = 2;
    CHECK(call(regs, AGB_SWI_OBJ_AFFINE_SET) == 0);
    CHECK((int16_t)agb_read16(&board, 0x07000000) == 256);
    CHECK((int16_t)agb_read16(&board, 0x07000002) == 0);
    CHECK((int16_t)agb_read16(&board, 0x07000004) == 0);
    CHECK((int16_t)agb_read16(&board, 0x07000006) == 256);

    /* MidiKey2Freq: the wave header's rate at the top of the key range is the rate itself */
    agb_write32(&board, 0x02000704, 16744000);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x02000700;
    regs[1] = 180;
    regs[2] = 0;
    CHECK(call(regs, AGB_SWI_MIDI_KEY_2_FREQ) == 0);
    CHECK(regs[0] == 16744000);
    regs[0] = 0x02000700;
    regs[1] = 192; /* one octave up: twice the rate */
    CHECK(call(regs, AGB_SWI_MIDI_KEY_2_FREQ) == 0);
    CHECK(regs[0] == 16744000 * 2);

    /* RegisterRamReset clears the flagged areas and leaves the top of IWRAM alone */
    agb_write32(&board, 0x02000000, 0x12345678);
    agb_write32(&board, 0x03007FFC, 0xCAFEBABE);
    agb_write32(&board, 0x06000000, 0x12345678);
    memset(regs, 0, sizeof(regs));
    regs[0] = 0x01 | 0x02 | 0x08;
    CHECK(call(regs, AGB_SWI_REGISTER_RAM_RESET) == 0);
    CHECK(agb_read32(&board, 0x02000000) == 0);
    CHECK(agb_read32(&board, 0x06000000) == 0);
    CHECK(agb_read32(&board, 0x03007FFC) == 0xCAFEBABE);

    /* the memory map's mirrors */
    agb_write32(&board, 0x02000000, 0xAABBCCDD);
    CHECK(agb_read32(&board, 0x02040000) == 0xAABBCCDD); /* EWRAM mirrors every 256 KiB */
    agb_write32(&board, 0x03000010, 0x11223344);
    CHECK(agb_read32(&board, 0x03008010) == 0x11223344); /* IWRAM mirrors every 32 KiB */
    agb_write32(&board, 0x06018000, 0x55667788);
    CHECK(agb_read32(&board, 0x06010000) == 0x55667788); /* the last VRAM 32 KiB mirrors the one above */
    agb_write8(&board, 0x05000003, 0x7E);                /* palette byte writes land in both halves */
    CHECK(agb_read16(&board, 0x05000002) == 0x7E7E);
    agb_write8(&board, 0x07000003, 0x7E); /* OAM is halfword wide: byte writes are dropped */
    CHECK(agb_read16(&board, 0x07000002) == 0);
    agb_write8(&board, 0x06018000, 0xAA); /* so are byte writes to the OBJ half of VRAM */
    CHECK(agb_read32(&board, 0x06010000) == 0x55667788);
    agb_write16(&board, 0x0E000000, 0x1234); /* the save memory only keeps the low byte */
    CHECK(agb_read16(&board, 0x0E000000) == 0x3434);

    agb_board_destroy(&board);
    puts("agb_bios: ok");
    return 0;
}
