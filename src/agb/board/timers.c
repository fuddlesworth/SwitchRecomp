/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the AGB timers: four 16 bit counters at the CPU clock, the first two of them feeding the sound FIFOs. */
#include "board.h"

#define AGB_TIMER_ENABLE  0x0080
#define AGB_TIMER_IRQ     0x0040
#define AGB_TIMER_CASCADE 0x0004

static const uint32_t prescalers[4] = {1, 64, 256, 1024};

static void timer_overflow(AgbBoard* board, int index) {
    AgbTimer* timer = &board->timer[index];
    timer->counter = timer->reload;
    if (timer->control & AGB_TIMER_IRQ) agb_irq_request(board, AGB_IRQ_TIMER(index));
    if (index < 3 && (board->timer[index + 1].control & AGB_TIMER_ENABLE) &&
        (board->timer[index + 1].control & AGB_TIMER_CASCADE)) {
        AgbTimer* next = &board->timer[index + 1];
        if (++next->counter >= 0x10000) timer_overflow(board, index + 1);
    }
}

void agb_timer_write(AgbBoard* board, uint32_t offset, uint16_t value) {
    int index = (int)((offset - 0x100) >> 2), part = (int)(offset & 3);
    AgbTimer* timer;
    if (index < 0 || index > 3) return;
    timer = &board->timer[index];
    if (!part) {
        timer->reload = value;
        return;
    }
    if ((value & AGB_TIMER_ENABLE) && !(timer->control & AGB_TIMER_ENABLE)) {
        timer->counter = timer->reload;
        timer->cycles = 0;
    }
    timer->control = value & 0x00C7;
}

uint16_t agb_timer_read(AgbBoard* board, uint32_t offset) {
    int index = (int)((offset - 0x100) >> 2), part = (int)(offset & 3);
    if (index < 0 || index > 3) return 0;
    return part ? board->timer[index].control : (uint16_t)board->timer[index].counter;
}

void agb_timer_step(AgbBoard* board, uint32_t cycles) {
    for (int index = 0; index < 4; ++index) {
        AgbTimer* timer = &board->timer[index];
        uint32_t step;
        if (!(timer->control & AGB_TIMER_ENABLE) || (timer->control & AGB_TIMER_CASCADE)) continue;
        step = prescalers[timer->control & 3];
        timer->cycles += cycles;
        while (timer->cycles >= step) {
            timer->cycles -= step;
            if (++timer->counter >= 0x10000) timer_overflow(board, index);
        }
    }
}
