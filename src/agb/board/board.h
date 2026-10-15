/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* the AGB board: the console the recompiled ROM runs on. It holds the cartridge, the memory map and the
   devices behind the I/O registers. */
#ifndef SNX_AGB_BOARD_H
#define SNX_AGB_BOARD_H
#include "../apu/apu.h"
#include "../cartridge/cartridge.h"
#include "../ppu/ppu.h"
#include "memory.h"
#include "save.h"

/* The interrupt lines devices raise, the bits of the interrupt controller's registers. */
enum {
    AGB_IRQ_VBLANK = 0x001,
    AGB_IRQ_HBLANK = 0x002,
    AGB_IRQ_VCOUNT = 0x004,
    AGB_IRQ_SERIAL = 0x080,
    AGB_IRQ_KEYPAD = 0x1000,
    AGB_IRQ_GAMEPAK = 0x2000
};
#define AGB_IRQ_TIMER(n) (0x008u << (n))
#define AGB_IRQ_DMA(n)   (0x100u << (n))

/* The DMA trigger lines from the PPU and the FIFOs. */
enum { AGB_DMA_VBLANK, AGB_DMA_HBLANK, AGB_DMA_FIFO_A, AGB_DMA_FIFO_B };

typedef struct {
    uint32_t source, destination, source_now, destination_now;
    uint16_t count, control;
} AgbDmaChannel;

typedef struct {
    uint16_t reload, control;
    uint32_t counter, cycles;
} AgbTimer;

typedef struct AgbBoard {
    AgbCartridge cartridge;
    AgbMemory memory;
    AgbPpu ppu;
    AgbApu apu;
    AgbSave save;
    AgbDmaChannel dma[4];
    AgbTimer timer[4];
    uint16_t irq_ie, irq_if, irq_ime;
    uint32_t cpu_halted;       /* the CPU waits for an interrupt (Halt) */
    uint16_t io[AGB_IO_COUNT]; /* the I/O register file; devices claim registers here */
    int has_cartridge;
} AgbBoard;

void agb_board_init(AgbBoard* board);
/* Takes a copy of the image: returns 0, or -1 with a message when the image is not a GBA ROM. */
int agb_board_insert(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size);
void agb_board_destroy(AgbBoard* board);

/* The I/O window at 0x04000000: the devices own their registers here. Recompiled code reaches them
   through the bus hook of AgbCpu, which routes every access to these. */
uint16_t agb_io_read16(AgbBoard* board, uint32_t address);
void agb_io_write16(AgbBoard* board, uint32_t address, uint16_t value);
/* Devices request interrupts here. */
void agb_irq_request(AgbBoard* board, uint32_t mask);

/* The DMA controller and the timers (dma.c, timers.c). */
void agb_dma_write(AgbBoard* board, uint32_t offset, uint16_t value);
uint16_t agb_dma_read(AgbBoard* board, uint32_t offset);
void agb_dma_event(AgbBoard* board, int event);
void agb_timer_write(AgbBoard* board, uint32_t offset, uint16_t value);
uint16_t agb_timer_read(AgbBoard* board, uint32_t offset);
void agb_timer_step(AgbBoard* board, uint32_t cycles);
#endif
