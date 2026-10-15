/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the AGB DMA controller: four channels moving memory on demand, for the video modes, the sound FIFOs and
   everything else the CPU sets them up for. */
#include "board.h"

/* DMAxCNT_H: the control halfword the CPU writes. */
#define AGB_DMA_ENABLE 0x8000
#define AGB_DMA_IRQ    0x4000
#define AGB_DMA_REPEAT 0x0200
#define AGB_DMA_WORD   0x0400
#define AGB_DMA_TIMING 0x3000

static void dma_transfer(AgbBoard* board, int index, int fifo) {
    AgbDmaChannel* channel = &board->dma[index];
    uint32_t count = channel->count ? channel->count : (index == 3 ? 0x10000 : 0x4000);
    int word = channel->control & AGB_DMA_WORD, step = word ? 4 : 2;
    int source_mode = channel->control >> 7 & 3, destination_mode = channel->control >> 5 & 3;
    uint32_t source = channel->source_now, destination = channel->destination_now;
    if (fifo) {
        word = 1;
        step = 4;
        count = 4;
        destination_mode = 2;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (word) {
            uint32_t value = agb_read32(board, source);
            agb_write32(board, destination, value);
        } else {
            uint16_t value = agb_read16(board, source);
            agb_write16(board, destination, value);
        }
        switch (source_mode) {
            case 0: source += (uint32_t)step; break;
            case 1: source -= (uint32_t)step; break;
            case 3: source += (uint32_t)step; break;
            default: break;
        }
        switch (destination_mode) {
            case 0:
            case 3: destination += (uint32_t)step; break;
            case 1: destination -= (uint32_t)step; break;
            default: break;
        }
    }
    channel->source_now = source;
    if (channel->control & AGB_DMA_REPEAT) {
        /* the destination reloads in increment-reload mode, and never moves in FIFO mode */
        channel->destination_now = fifo                    ? channel->destination
                                   : destination_mode == 3 ? channel->destination
                                                           : destination;
    } else {
        channel->control &= (uint16_t)~AGB_DMA_ENABLE;
        channel->destination_now = destination;
    }
    if (channel->control & AGB_DMA_IRQ) agb_irq_request(board, AGB_IRQ_DMA(index));
}

void agb_dma_write(AgbBoard* board, uint32_t offset, uint16_t value) {
    int index = (int)((offset - 0xB0) >> 4), part = (int)(offset & 0xF);
    AgbDmaChannel* channel;
    if (index < 0 || index > 3) return;
    channel = &board->dma[index];
    switch (part) {
        case 0x0: channel->source = (channel->source & 0xFFFF0000) | value; return;
        case 0x2: channel->source = (channel->source & 0x0000FFFF) | (uint32_t)value << 16; return;
        case 0x4: channel->destination = (channel->destination & 0xFFFF0000) | value; return;
        case 0x6: channel->destination = (channel->destination & 0x0000FFFF) | (uint32_t)value << 16; return;
        case 0x8: channel->count = value; return;
        case 0xA: {
            int was = channel->control & AGB_DMA_ENABLE;
            channel->control = value;
            channel->source_now = channel->source;
            channel->destination_now = channel->destination;
            if (!was && (value & AGB_DMA_ENABLE)) {
                int timing = value >> 12 & 3;
                if (!timing) dma_transfer(board, index, 0);
            }
            return;
        }
        default: return;
    }
}

uint16_t agb_dma_read(AgbBoard* board, uint32_t offset) {
    int index = (int)((offset - 0xB0) >> 4), part = (int)(offset & 0xF);
    AgbDmaChannel* channel;
    if (index < 0 || index > 3) return 0;
    channel = &board->dma[index];
    if (part == 0xA) return channel->control;
    if (part == 0x8) return channel->count;
    return 0;
}

void agb_dma_event(AgbBoard* board, int event) {
    for (int index = 0; index < 4; ++index) {
        AgbDmaChannel* channel = &board->dma[index];
        int timing = channel->control >> 12 & 3;
        if (!(channel->control & AGB_DMA_ENABLE)) continue;
        if (event == AGB_DMA_VBLANK && timing == 1) dma_transfer(board, index, 0);
        if (event == AGB_DMA_HBLANK && timing == 2) dma_transfer(board, index, 0);
        if (timing == 3 && (index == 1 || index == 2)) {
            uint32_t target = channel->destination_now & 0x0FFFFFFC;
            if (event == AGB_DMA_FIFO_A && target == 0x040000A0) dma_transfer(board, index, 1);
            if (event == AGB_DMA_FIFO_B && target == 0x040000A4) dma_transfer(board, index, 1);
        }
    }
}
