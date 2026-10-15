/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AOT step for a GBA ROM: the ARM and Thumb code of the cartridge goes through the existing ARM
   recompiler (src/common/cpu/arm) into a native module, the way NX and GNU ABI prepare their code. */
#ifndef SNX_AGB_RECOMP_H
#define SNX_AGB_RECOMP_H
#include "cpu/arm/arm.h"
#include <stddef.h>
#include <stdint.h>

typedef struct AgbBoard AgbBoard;
typedef struct {
    uint32_t pc;
    uint16_t instructions;
    uint8_t thumb;
    SnxArmBlock fn;
} AgbBlock;

typedef struct {
    AgbBlock* blocks;
    size_t count, capacity;
    void* handle;   /* the loaded native module */
    char* base;     /* the cache path: <base>.so and <base>.map */
    const char* cc; /* the host compiler that links the emitted objects */
    int dirty;      /* blocks changed since the module was linked */
} AgbRecomp;

void agb_recomp_init(AgbRecomp* recomp, const char* base, const char* cc);
void agb_recomp_free(AgbRecomp* recomp);
/* The AOT step: discovers the code the ROM entry reaches and builds the module. */
int agb_recomp_build(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size);
/* Loads the module a previous build cached next to the ROM. Returns 0 on success. */
int agb_recomp_load_cache(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size);
/* The block at pc in the given ISA, compiling one if the cache does not hold it yet. */
const AgbBlock* agb_recomp_lookup(AgbRecomp* recomp, AgbBoard* board, uint32_t pc, int thumb, char* error,
                                  size_t error_size);
#endif
