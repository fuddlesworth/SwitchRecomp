/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The whole machine: a hand assembled ROM goes through the AOT recompiler and runs on the board, through
   a BIOS call and an interrupt, and the board's memory shows what happened. */
#include "board/board.h"
#include "cpu/agb_cpu.h"
#include "cpu/memmap.h"
#include "cpu/recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void put32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}
static void put16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

/* The ROM: an ARM stub that branches into Thumb code, which enables the vblank interrupt, installs a
   handler at 0x03007FFC, waits for a vblank through the HLE BIOS and writes two markers on its way. The
   handler acknowledges the interrupt flag and writes a marker of its own. */
static uint8_t* make_rom(size_t* size) {
    uint8_t* rom = (uint8_t*)calloc(1, 0x200);
    *size = 0x200;
    put32(rom + 0x00, 0xEA00002E); /* B 0xC0, the header's entry branch */
    rom[0xB2] = 0x96;
    {
        unsigned sum = 0;
        for (int i = 0xA0; i <= 0xBC; ++i) sum += rom[i];
        rom[0xBD] = (uint8_t)(0u - sum - 0x19u);
    }
    /* ARM at 0xC0: r3 = 0x080000CC with the Thumb bit, then bx r3 */
    put32(rom + 0xC0, 0xE59F3000); /* ldr r3, [pc, #0] */
    put32(rom + 0xC4, 0xE12FFF13); /* bx r3 */
    put32(rom + 0xC8, 0x080000CD);
    /* Thumb at 0xCC: the vblank interrupt on, the handler installed, one BIOS wait, then the markers */
    put16(rom + 0xCC, 0x4808); /* ldr r0, [pc, #32]: DISPSTAT */
    put16(rom + 0xCE, 0x2108); /* movs r1, #8 */
    put16(rom + 0xD0, 0x6001); /* str r1, [r0]: vblank interrupt on */
    put16(rom + 0xD2, 0x4808); /* ldr r0, [pc, #32]: IME */
    put16(rom + 0xD4, 0x2101); /* movs r1, #1 */
    put16(rom + 0xD6, 0x6001); /* str r1, [r0] */
    put16(rom + 0xD8, 0x4807); /* ldr r0, [pc, #28]: IE */
    put16(rom + 0xDA, 0x2101); /* movs r1, #1 */
    put16(rom + 0xDC, 0x6001); /* str r1, [r0] */
    put16(rom + 0xDE, 0x4807); /* ldr r0, [pc, #28]: 0x03007FFC */
    put16(rom + 0xE0, 0x4907); /* ldr r1, [pc, #28]: the handler */
    put16(rom + 0xE2, 0x6001); /* str r1, [r0] */
    put16(rom + 0xE4, 0xDF05); /* swi 0x05: VBlankIntrWait */
    put16(rom + 0xE6, 0x4807); /* ldr r0, [pc, #28]: marker 1 */
    put16(rom + 0xE8, 0x212A); /* movs r1, #42 */
    put16(rom + 0xEA, 0x6001); /* str r1, [r0] */
    put16(rom + 0xEC, 0xE7FE); /* b 0xEC: spin */
    put16(rom + 0xEE, 0x46C0); /* nop */
    put32(rom + 0xF0, 0x04000004);
    put32(rom + 0xF4, 0x04000208);
    put32(rom + 0xF8, 0x04000200);
    put32(rom + 0xFC, 0x03007FFC);
    put32(rom + 0x100, 0x08000121); /* the handler, Thumb */
    put32(rom + 0x104, 0x02000000); /* marker 1 */
    /* the interrupt handler at 0x120 */
    put16(rom + 0x120, 0x2101); /* movs r1, #1 */
    put16(rom + 0x122, 0x4803); /* ldr r0, [pc, #12]: IF */
    put16(rom + 0x124, 0x8001); /* strh r1, [r0]: acknowledge */
    put16(rom + 0x126, 0x2107); /* movs r1, #7 */
    put16(rom + 0x128, 0x4802); /* ldr r0, [pc, #8]: marker 2 */
    put16(rom + 0x12A, 0x6001); /* str r1, [r0] */
    put16(rom + 0x12C, 0x4770); /* bx lr */
    put16(rom + 0x12E, 0x46C0);
    put32(rom + 0x130, 0x04000202);
    put32(rom + 0x134, 0x02000004); /* marker 2 */
    return rom;
}

int main(void) {
    AgbBoard board;
    AgbMap map;
    AgbCpu cpu;
    AgbRecomp recomp;
    char error[512] = {0};
    size_t size = 0;
    uint8_t* rom = make_rom(&size);
    int16_t* audio = (int16_t*)calloc(2 * AGB_APU_RATE, sizeof(int16_t));
    int result = 1;
    CHECK(rom && audio);
    agb_board_init(&board);
    CHECK(!agb_board_insert(&board, rom, size, error, sizeof error));
    free(rom);
    CHECK(!agb_map_init(&map, &board, error, sizeof error));
    agb_cpu_init(&cpu, &board, map.base);
    agb_recomp_init(&recomp, "agb_cpu_test.code", SNX_AGB_TEST_CC);
    if (agb_recomp_build(&recomp, &board, error, sizeof error)) {
        fprintf(stderr, "agb_cpu: recompiling the ROM: %s\n", error);
        goto done;
    }
    for (int frame = 0; frame < 5; ++frame) {
        uint32_t written = 0;
        if (agb_cpu_run(&board, &cpu, &recomp, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES, audio, AGB_APU_RATE, &written,
                        error, sizeof error)) {
            fprintf(stderr, "agb_cpu: %s\n", error);
            goto done;
        }
        CHECK(written >= 540 && written <= 560); /* the mixer filled the frame's audio at 32768 Hz */
    }
    /* the recompiled code ran: the BIOS call waited for a vblank and the markers landed */
    CHECK(agb_read32(&board, 0x02000000) == 42);
    CHECK(agb_read32(&board, 0x02000004) == 7);
    /* the interrupt handler acknowledged the flag and left the machine running */
    CHECK(agb_read16(&board, 0x04000202) == 0);
    CHECK(agb_read16(&board, 0x04000208) == 1);
    result = 0;
done:
    agb_recomp_free(&recomp);
    agb_map_destroy(&map, &board);
    agb_board_destroy(&board);
    free(audio);
    remove("agb_cpu_test.code.map");
    if (!result) puts("agb_cpu: ok");
    return result;
}
