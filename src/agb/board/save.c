/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "save.h"
#include "board.h"
#include <string.h>

/* The tags the SDK leaves in a ROM so the emulator knows the save kind. */
uint8_t agb_save_detect(const uint8_t* image, uint64_t size) {
    static const struct {
        const char* tag;
        uint8_t type;
    } tags[] = {
        {"FLASH1M_V", AGB_SAVE_FLASH128},
        {"FLASH512_V", AGB_SAVE_FLASH64},
        {"FLASH_V", AGB_SAVE_FLASH64},
        {"SRAM_V", AGB_SAVE_SRAM},
    };
    for (uint64_t i = 0; i + 16 < size; ++i)
        for (size_t t = 0; t < sizeof(tags) / sizeof(tags[0]); ++t)
            if (!memcmp(image + i, tags[t].tag, strlen(tags[t].tag))) return tags[t].type;
    return AGB_SAVE_NONE;
}

void agb_save_reset(AgbBoard* board, uint8_t type) {
    board->save.type = type;
    board->save.phase = 0;
    board->save.erase_armed = board->save.program_armed = board->save.bank_armed = 0;
    board->save.id_mode = 0;
    board->save.bank = 0;
}

static uint32_t save_offset(const AgbBoard* board, uint32_t address) {
    uint32_t offset = address & 0xFFFF; /* the window repeats every 64 KiB */
    if (board->save.type == AGB_SAVE_FLASH128) offset |= (uint32_t)board->save.bank << 16;
    return offset;
}

uint8_t agb_save_read8(AgbBoard* board, uint32_t address) {
    AgbSave* save = &board->save;
    if (save->id_mode) {
        switch (address & 0xFF) {
            case 0: return save->type == AGB_SAVE_FLASH128 ? 0x62 : 0x32; /* Sanyo or Panasonic */
            case 1: return save->type == AGB_SAVE_FLASH128 ? 0x13 : 0x1B;
            default: return 0xFF;
        }
    }
    return board->memory.sram[save_offset(board, address)];
}

static void flash_erase(AgbBoard* board, uint32_t address, int chip) {
    AgbSave* save = &board->save;
    uint32_t base = save->type == AGB_SAVE_FLASH128 ? (uint32_t)save->bank << 16 : 0;
    if (chip)
        memset(board->memory.sram + base, 0xFF, 0x10000);
    else
        memset(board->memory.sram + (base | (address & 0xF000)), 0xFF, 0x1000); /* 4 KiB sectors */
}

void agb_save_write8(AgbBoard* board, uint32_t address, uint8_t value) {
    AgbSave* save = &board->save;
    if (save->type == AGB_SAVE_NONE || save->type == AGB_SAVE_SRAM) {
        board->memory.sram[address & 0xFFFF] = value; /* the window repeats every 64 KiB */
        return;
    }
    /* flash: the command set is addressed by offsets inside the window */
    if (save->program_armed) {
        board->memory.sram[save_offset(board, address)] &= value; /* programming can only clear bits */
        save->program_armed = 0;
        save->phase = 0;
        return;
    }
    if (save->bank_armed) {
        if ((address & 0xFFFF) == 0) save->bank = value & 1;
        save->bank_armed = 0;
        save->phase = 0;
        return;
    }
    if ((address & 0xFFFF) == 0x5555 && value == 0xAA && save->phase == 0) {
        save->phase = 1;
        return;
    }
    if ((address & 0xFFFF) == 0x2AAA && value == 0x55 && save->phase == 1) {
        save->phase = 2;
        return;
    }
    if (save->erase_armed && value == 0x30) {
        flash_erase(board, address, 0);
        save->erase_armed = 0;
        save->phase = 0;
        return;
    }
    if (save->phase == 2 && (address & 0xFFFF) == 0x5555) {
        switch (value) {
            case 0x10: if (save->erase_armed) flash_erase(board, address, 1); save->erase_armed = 0; break;
            case 0x80: save->erase_armed = 1; break;
            case 0x90: save->id_mode = 1; break;
            case 0xF0: save->id_mode = 0; break;
            case 0xA0: save->program_armed = 1; break;
            case 0xB0: save->bank_armed = 1; break;
            default: break;
        }
        save->phase = 0;
        return;
    }
    save->phase = 0;
}
