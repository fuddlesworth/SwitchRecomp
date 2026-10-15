/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "board/board.h"
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

#define ROM_SIZE 0x200

/* A ROM image with a valid header: an ARM branch over the header, then the title block. */
static void fix_checksum(uint8_t* rom) {
    unsigned sum = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i) sum += rom[i];
    rom[0xBD] = (uint8_t)(0u - sum - 0x19u);
}
static void make_rom(uint8_t* rom) {
    memset(rom, 0, ROM_SIZE);
    rom[0] = 0x2E;
    rom[3] = 0xEA; /* B 0xC0: the entry branch every GBA ROM starts with */
    memcpy(rom + 0xA0, "TESTGAME", 8);
    memcpy(rom + 0xAC, "BZVE", 4);
    memcpy(rom + 0xB0, "01", 2);
    rom[0xB2] = 0x96;
    rom[0xB3] = 0x00;
    rom[0xB4] = 0x00;
    rom[0xBC] = 3;
    fix_checksum(rom);
}

int main(void) {
    uint8_t* rom = (uint8_t*)malloc(ROM_SIZE);
    char error[256];
    CHECK(rom);
    make_rom(rom);

    AgbBoard board;
    agb_board_init(&board);
    CHECK(!agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(board.has_cartridge);
    const AgbCartridge* cartridge = &board.cartridge;
    CHECK(cartridge->size == ROM_SIZE);
    CHECK(!strcmp(cartridge->title, "TESTGAME"));
    CHECK(!strcmp(cartridge->game_code, "BZVE"));
    CHECK(!strcmp(cartridge->maker_code, "01"));
    CHECK(cartridge->version == 3 && cartridge->unit == 0 && cartridge->device == 0);
    CHECK(cartridge->entry == 0xEA00002E && cartridge->entry_to == 0xC0);
    CHECK(!memcmp(cartridge->data, rom, ROM_SIZE));
    CHECK(cartridge->data != rom); /* the cartridge keeps its own copy */

    /* titles run to the first NUL */
    make_rom(rom);
    rom[0xA4] = 0;
    fix_checksum(rom);
    CHECK(!agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(!strcmp(board.cartridge.title, "TEST"));

    /* each header violation names the byte it failed on */
    make_rom(rom);
    CHECK(agb_board_insert(&board, rom, AGB_ROM_MIN - 1, error, sizeof error));
    CHECK(strstr(error, "too small"));
    CHECK(agb_board_insert(&board, rom, (size_t)AGB_ROM_MAX + 4, error, sizeof error));
    CHECK(strstr(error, "32 MiB"));
    make_rom(rom);
    rom[3] = 0x00;
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "branch"));
    make_rom(rom);
    rom[0xB2] = 0x00;
    fix_checksum(rom);
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "0x96"));
    make_rom(rom);
    rom[0xBD] ^= 0x40;
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "checksum"));

    /* a rejected insert leaves the accepted cartridge in place */
    CHECK(board.has_cartridge && !strcmp(board.cartridge.title, "TEST"));
    agb_board_destroy(&board);
    CHECK(!board.has_cartridge && !board.cartridge.data);

    free(rom);
    puts("agb_board: ok");
    return 0;
}
