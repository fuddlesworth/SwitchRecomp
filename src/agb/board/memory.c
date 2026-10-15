/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "memory.h"
#include "board.h"
#include "save.h"
#include <stdlib.h>
#include <string.h>

void agb_memory_init(AgbMemory* memory) {
    memset(memory, 0, sizeof(*memory));
    memory->ewram = (uint8_t*)calloc(1, AGB_EWRAM_SIZE);
    memory->iwram = (uint8_t*)calloc(1, AGB_IWRAM_SIZE);
    memory->palette = (uint8_t*)calloc(1, AGB_PALETTE_SIZE);
    memory->vram = (uint8_t*)calloc(1, AGB_VRAM_SIZE);
    memory->oam = (uint8_t*)calloc(1, AGB_OAM_SIZE);
    memory->sram = (uint8_t*)calloc(1, AGB_SRAM_SIZE);
    /* save memory reads 0xFF until the game writes it */
    if (memory->sram) memset(memory->sram, 0xFF, AGB_SRAM_SIZE);
    memory->owned = 1;
}

void agb_memory_free(AgbMemory* memory) {
    if (!memory->owned) return;
    free(memory->ewram);
    free(memory->iwram);
    free(memory->palette);
    free(memory->vram);
    free(memory->oam);
    free(memory->sram);
    memset(memory, 0, sizeof(*memory));
}

static uint16_t load16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t load32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void store16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}
static void store32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t vram_offset(uint32_t address) {
    uint32_t offset = address & 0x1FFFF; /* the console wraps VRAM in 128 KiB windows */
    /* the window's last 32 KiB mirror the 32 KiB above the bitmap region */
    if (offset >= AGB_VRAM_SIZE) offset -= 0x8000;
    return offset;
}

static uint8_t* region8(AgbBoard* board, uint32_t address) {
    switch (address >> 24) {
        case 0x02: return board->memory.ewram + (address & (AGB_EWRAM_SIZE - 1));
        case 0x03: return board->memory.iwram + (address & (AGB_IWRAM_SIZE - 1));
        case 0x05: return board->memory.palette + (address & (AGB_PALETTE_SIZE - 1));
        case 0x06: return board->memory.vram + vram_offset(address);
        case 0x07: return board->memory.oam + (address & (AGB_OAM_SIZE - 1));
        case 0x0E:
        case 0x0F: return board->memory.sram + (address & (AGB_SRAM_SIZE - 1));
        default: return NULL;
    }
}

/* The cartridge image at a wait state offset. The 32 MiB window repeats the image, byte addressed so a
   multi-byte access near the image's end wraps around it. */
static const uint8_t* rom_bytes(const AgbBoard* board, uint32_t address, uint32_t width, uint32_t* offset) {
    uint32_t size;
    if (!board->has_cartridge) return NULL;
    size = (uint32_t)board->cartridge.size;
    if (!size || width > size) return NULL;
    *offset = (address & 0x01FFFFFF) % size;
    return board->cartridge.data;
}

uint8_t agb_read8(AgbBoard* board, uint32_t address) {
    uint32_t offset;
    const uint8_t* rom;
    uint8_t* p;
    if ((address >> 24) == 0x04) return (uint8_t)(agb_io_read16(board, address & 0x0FFFFFFE) >> ((address & 1) * 8));
    if ((address >> 24) >= 0x0E) return agb_save_read8(board, address);
    p = region8(board, address);
    if (p) return *p;
    rom = rom_bytes(board, address, 1, &offset);
    return rom ? rom[offset] : 0;
}

uint16_t agb_read16(AgbBoard* board, uint32_t address) {
    uint32_t offset, size;
    const uint8_t* rom;
    uint8_t* p;
    address &= 0x0FFFFFFE;
    if ((address >> 24) == 0x04) return agb_io_read16(board, address);
    if ((address >> 24) >= 0x0E) {
        /* the save memory sits on an 8 bit bus: a wider read returns the byte twice */
        uint8_t byte = agb_save_read8(board, address);
        return (uint16_t)(byte * 0x0101u);
    }
    p = region8(board, address);
    if (p) return load16(p);
    rom = rom_bytes(board, address, 2, &offset);
    if (!rom) return 0;
    size = (uint32_t)board->cartridge.size;
    return (uint16_t)(rom[offset] | rom[(offset + 1) % size] << 8);
}

uint32_t agb_read32(AgbBoard* board, uint32_t address) {
    uint32_t offset, size;
    const uint8_t* rom;
    uint8_t* p;
    address &= 0x0FFFFFFC;
    if ((address >> 24) == 0x04)
        return agb_io_read16(board, address) | (uint32_t)agb_io_read16(board, address + 2) << 16;
    if ((address >> 24) >= 0x0E) {
        uint8_t byte = agb_save_read8(board, address);
        return byte * 0x01010101u;
    }
    p = region8(board, address);
    if (p) return load32(p);
    rom = rom_bytes(board, address, 4, &offset);
    if (!rom) return 0;
    size = (uint32_t)board->cartridge.size;
    return (uint32_t)rom[offset] | (uint32_t)rom[(offset + 1) % size] << 8 | (uint32_t)rom[(offset + 2) % size] << 16 |
           (uint32_t)rom[(offset + 3) % size] << 24;
}

void agb_write8(AgbBoard* board, uint32_t address, uint8_t value) {
    uint8_t* p;
    if ((address >> 24) == 0x04) {
        uint32_t halfword = address & 0x0FFFFFFE;
        uint16_t old = agb_io_read16(board, halfword);
        agb_io_write16(board, halfword, (uint16_t)(address & 1 ? (old & 0x00FF) | value << 8 : (old & 0xFF00) | value));
        return;
    }
    if ((address >> 24) >= 0x0E) {
        agb_save_write8(board, address, value);
        return;
    }
    p = region8(board, address);
    if (!p) return;
    switch (address >> 24) {
        /* byte writes reach these halfword wide buses as the byte duplicated in both halves */
        case 0x05:
        case 0x06: {
            /* the OBJ half of VRAM has no byte writes at all, and OAM is halfword wide only */
            uint32_t object_base = (board->ppu.dispcnt & 7) >= 3 ? 0x14000 : 0x10000;
            if (address >> 24 == 0x06 && vram_offset(address) >= object_base) return;
            store16(p - (address & 1), (uint16_t)(value * 0x0101u));
            break;
        }
        case 0x07: return;
        default: *p = value; break;
    }
}

void agb_write16(AgbBoard* board, uint32_t address, uint16_t value) {
    uint8_t* p;
    if ((address >> 24) >= 0x0E) {
        /* the save memory is 8 bits wide: a wider store puts one lane's byte down */
        agb_save_write8(board, address, (uint8_t)(value >> ((address & 1) * 8)));
        return;
    }
    address &= 0x0FFFFFFE;
    if ((address >> 24) == 0x04) {
        agb_io_write16(board, address, value);
        return;
    }
    p = region8(board, address);
    if (p) store16(p, value);
}

void agb_write32(AgbBoard* board, uint32_t address, uint32_t value) {
    uint8_t* p;
    if ((address >> 24) >= 0x0E) {
        agb_save_write8(board, address, (uint8_t)(value >> ((address & 3) * 8)));
        return;
    }
    address &= 0x0FFFFFFC;
    if ((address >> 24) == 0x04) {
        agb_io_write16(board, address, (uint16_t)value);
        agb_io_write16(board, address + 2, (uint16_t)(value >> 16));
        return;
    }
    p = region8(board, address);
    if (p) store32(p, value);
}
