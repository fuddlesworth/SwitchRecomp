/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The cartridge's save memory: plain SRAM or flash with its command set. All of it sits on an 8 bit
   bus, so the wider accesses the CPU makes are one byte each on the way in and replicated on the way
   out (board/memory.c applies those rules). */
#ifndef SNX_AGB_SAVE_H
#define SNX_AGB_SAVE_H
#include <stdint.h>

typedef struct AgbBoard AgbBoard;
enum { AGB_SAVE_NONE, AGB_SAVE_SRAM, AGB_SAVE_FLASH64, AGB_SAVE_FLASH128 };

typedef struct {
    uint8_t type;
    uint8_t phase;        /* the unlock sequence: 0xAA, then 0x55 */
    uint8_t erase_armed, program_armed, bank_armed, id_mode, bank;
} AgbSave;

/* The save kind the ROM's tag names, or AGB_SAVE_NONE. */
uint8_t agb_save_detect(const uint8_t* image, uint64_t size);
void agb_save_reset(AgbBoard* board, uint8_t type);
uint8_t agb_save_read8(AgbBoard* board, uint32_t address);
void agb_save_write8(AgbBoard* board, uint32_t address, uint8_t value);
#endif
