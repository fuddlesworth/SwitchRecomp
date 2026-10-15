/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* the AGB cartridge: the ROM image the board runs and the header at its start */
#ifndef SNX_AGB_CARTRIDGE_H
#define SNX_AGB_CARTRIDGE_H
#include <stddef.h>
#include <stdint.h>

#define AGB_ROM_MIN 0xC0 /* the header at the start of every ROM */
#define AGB_ROM_MAX (32u * 1024 * 1024)

typedef struct {
    uint8_t* data;      /* the image as the cartridge delivers it: guest little endian */
    size_t size;        /* bytes; GBA ROM images hold at most 32 MiB */
    char title[13];     /* 0xA0: up to twelve title characters, NUL terminated */
    char game_code[5];  /* 0xAC: two maker characters and two game characters */
    char maker_code[3]; /* 0xB0 */
    uint8_t unit, device, version;
    uint32_t entry;    /* 0x00: the ARM branch word the header starts with */
    uint32_t entry_to; /* the branch target as a ROM offset */
} AgbCartridge;

/* Parses a ROM image kept in memory: returns 0, or -1 with a message when the image is not a GBA ROM. The image is
   copied. */
int agb_cartridge_load(AgbCartridge* cartridge, const void* image, size_t size, char* error, size_t error_size);
void agb_cartridge_unload(AgbCartridge* cartridge);
#endif
