/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "cartridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The complement check covers the title through the software version. */
static uint8_t header_checksum(const uint8_t* header) {
    unsigned sum = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i) sum += header[i];
    return (uint8_t)(0u - sum - 0x19u);
}

static int fail(char* error, size_t error_size, const char* what) {
    snprintf(error, error_size, "%s", what);
    return -1;
}

int agb_cartridge_load(AgbCartridge* cartridge, const void* image, size_t size, char* error, size_t error_size) {
    const uint8_t* bytes = (const uint8_t*)image;
    uint8_t* copy;
    if (error_size) error[0] = 0;
    memset(cartridge, 0, sizeof(*cartridge));
    if (size < AGB_ROM_MIN) {
        snprintf(error, error_size, "ROM image is %zu bytes: too small for a GBA header", size);
        return -1;
    }
    if (size > AGB_ROM_MAX) {
        snprintf(error, error_size, "ROM image is %zu bytes: larger than the 32 MiB a GBA cartridge holds", size);
        return -1;
    }
    cartridge->entry =
        (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    if ((cartridge->entry & 0xFF000000u) != 0xEA000000u)
        return fail(error, error_size, "ROM header does not start with an ARM branch");
    /* the branch target as a ROM offset: 8 past the branch word, plus the sign extended imm24 shifted left 2 */
    cartridge->entry_to = 8u + (uint32_t)((int32_t)(cartridge->entry << 8) >> 6);
    if (bytes[0xB2] != 0x96) return fail(error, error_size, "ROM header is missing the fixed 0x96 byte");
    if (header_checksum(bytes) != bytes[0xBD])
        return fail(error, error_size, "ROM header checksum does not match its title block");
    copy = (uint8_t*)malloc(size);
    if (!copy) return fail(error, error_size, "out of memory for the ROM image");
    memcpy(copy, image, size);
    cartridge->data = copy;
    cartridge->size = size;
    memcpy(cartridge->title, bytes + 0xA0, 12);
    cartridge->title[12] = 0;
    memcpy(cartridge->game_code, bytes + 0xAC, 4);
    cartridge->game_code[4] = 0;
    memcpy(cartridge->maker_code, bytes + 0xB0, 2);
    cartridge->maker_code[2] = 0;
    cartridge->unit = bytes[0xB3];
    cartridge->device = bytes[0xB4];
    cartridge->version = bytes[0xBC];
    return 0;
}

void agb_cartridge_unload(AgbCartridge* cartridge) {
    free(cartridge->data);
    memset(cartridge, 0, sizeof(*cartridge));
}
