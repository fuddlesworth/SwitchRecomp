/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The machine loop: recompiled blocks run on the flat map, software interrupts go to the HLE BIOS, and the
   privileged instructions the ARM recompiler refuses are interpreted here. */
#ifndef SNX_AGB_CPU_H
#define SNX_AGB_CPU_H
#include "cpu/arm/arm.h"
#include "recomp.h"
#include <stddef.h>
#include <stdint.h>

typedef struct AgbBoard AgbBoard;
typedef struct {
    SnxArmCPU arm;
    AgbBoard* board; /* the machine the bus hook serves */
    /* the banked registers of the ARM7TDMI's modes: usr and sys share one bank */
    uint32_t banked_r13[6], banked_r14[6], spsr[6];
    uint32_t fiq_r8[5], usr_r8[5]; /* the ARM7TDMI banks r8 to r12 in FIQ mode */
} AgbCpu;

void agb_cpu_init(AgbCpu* cpu, AgbBoard* board, uint8_t* map);
/* Runs the board for the given number of CPU cycles. audio collects interleaved stereo samples at
   AGB_APU_RATE, up to capacity. Returns 0, or -1 with a message when the machine stops. */
int agb_cpu_run(AgbBoard* board, AgbCpu* cpu, AgbRecomp* recomp, uint32_t cycles, int16_t* audio,
                uint32_t audio_capacity, uint32_t* audio_written, char* error, size_t error_size);
#endif
