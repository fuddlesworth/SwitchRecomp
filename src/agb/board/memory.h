/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* the AGB memory map: the console's address space and its mirrors, with the board's I/O window at 0x04000000.
   The regions are plain buffers until the recompiler's flat map takes them over (cpu/memmap.c). */
#ifndef SNX_AGB_MEMORY_H
#define SNX_AGB_MEMORY_H
#include <stddef.h>
#include <stdint.h>

#define AGB_EWRAM_SIZE (256u * 1024) /* 0x02000000: work RAM on the board */
#define AGB_IWRAM_SIZE (32u * 1024)  /* 0x03000000: work RAM on the CPU's bus */
#define AGB_PALETTE_SIZE 0x400u      /* 0x05000000: background and object palette */
#define AGB_VRAM_SIZE 0x18000u       /* 0x06000000: video RAM */
#define AGB_OAM_SIZE 0x400u          /* 0x07000000: object attribute memory */
#define AGB_SRAM_SIZE (128u * 1024) /* 0x0E000000: the cartridge's save memory, up to the 128 KiB flash */
#define AGB_IO_COUNT 0x200u          /* 0x04000000: register halfwords */
#define AGB_MAP_SIZE 0x100000000ull /* the flat window: the 256 MiB map twice, for the bit 31 mirror */

typedef struct AgbBoard AgbBoard;
typedef struct {
   uint8_t *ewram, *iwram, *palette, *vram, *oam, *sram;
   int owned; /* the buffers are this board's, not the flat map's */
} AgbMemory;

void agb_memory_init(AgbMemory* memory);
void agb_memory_free(AgbMemory* memory);

/* Addresses fold through the region mirrors before the access, down to the palette's and OAM's 1 KiB
   repeats. Reads of unmapped space and writes to the cartridge ROM are dropped and return zero. */
uint8_t agb_read8(AgbBoard* board, uint32_t address);
uint16_t agb_read16(AgbBoard* board, uint32_t address);
uint32_t agb_read32(AgbBoard* board, uint32_t address);
void agb_write8(AgbBoard* board, uint32_t address, uint8_t value);
void agb_write16(AgbBoard* board, uint32_t address, uint16_t value);
void agb_write32(AgbBoard* board, uint32_t address, uint32_t value);
#endif
