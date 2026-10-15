/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "apu.h"
#include "board/board.h"
#include <string.h>

/* The PSG runs at 4 MiHz and the mixer samples it every 512 cycles, 32768 times a second. The frame
   sequencer clocks the length counters at 256 Hz, the sweep at 128 Hz and the envelopes at 64 Hz. */
#define CYCLES_PER_SAMPLE 512
#define LENGTH_TICK       128
#define SWEEP_TICK        256
#define ENVELOPE_TICK     512

static const uint8_t duty_steps[4] = {1, 2, 4, 6}; /* how many of a duty cycle's eight positions are high */

static void trigger(AgbBoard* board, int channel) {
    AgbApu* apu = &board->apu;
    apu->enabled[channel] = 1;
    apu->phase[channel] = 0;
    if (channel < 2) apu->duty_position[channel] = 0;
    if (channel == 2) apu->wave_position = 0;
    if (channel == 3) apu->noise_lfsr = 0x7FFF;
    if (channel == 0) {
        apu->sweep_shadow[0] = apu->cnt_x[0] & 0x7FF;
        apu->sweep_timer[0] = (apu->cnt_l[0] >> 4) & 7;
    }
    /* the envelope reloads its volume and timer */
    apu->envelope_volume[channel] = (int)(apu->cnt_h[channel] >> 12);
    apu->envelope_direction[channel] = apu->cnt_h[channel] & 0x800 ? 1 : -1;
    apu->envelope_period[channel] = (apu->cnt_h[channel] >> 8) & 7;
    apu->envelope_timer[channel] = apu->envelope_period[channel];
    if (!apu->length[channel]) apu->length[channel] = channel == 2 ? 256 : 64;
}

/* ---- registers ---- */

uint16_t agb_apu_read(AgbBoard* board, uint32_t offset) {
    AgbApu* apu = &board->apu;
    switch (offset) {
        case 0x060: return apu->cnt_l[0] & 0x7F;
        case 0x062: return apu->cnt_h[0];
        case 0x068: return apu->cnt_h[1];
        case 0x064: return (uint16_t)(apu->cnt_x[0] & 0x4000);
        case 0x06C: return (uint16_t)(apu->cnt_x[1] & 0x4000);
        case 0x070: return apu->cnt_l[2] & 0xE0;
        case 0x072: return apu->cnt_h[2] & 0xE0FF;
        case 0x074: return (uint16_t)(apu->cnt_x[2] & 0x4000);
        case 0x078: return apu->cnt_h[3] & 0xFFC0;
        case 0x07C: return (uint16_t)(apu->cnt_x[3] & 0x40FF);
        case 0x080: return apu->soundcnt_l;
        case 0x082: return apu->soundcnt_h;
        case 0x084: {
            uint16_t status = apu->soundcnt_x & 0x80;
            for (int channel = 0; channel < 4; ++channel)
                if (apu->enabled[channel]) status |= 1 << channel;
            return status;
        }
        case 0x088: return apu->soundbias;
        default: break;
    }
    if (offset >= 0x090 && offset <= 0x09E) {
        uint32_t bank = offset >= 0x098, index = offset & 7;
        return (uint16_t)(apu->wave_ram[bank][index] | apu->wave_ram[bank][index + 1] << 8);
    }
    return 0;
}

void agb_apu_write(AgbBoard* board, uint32_t offset, uint16_t value) {
    AgbApu* apu = &board->apu;
    int channel;
    switch (offset) {
        case 0x060: apu->cnt_l[0] = value & 0x7F; return;
        case 0x062:
        case 0x068:
            channel = offset == 0x062 ? 0 : 1;
            apu->cnt_h[channel] = value;
            apu->length[channel] = 64 - (value & 63);
            apu->length_enabled[channel] = value & 0x4000 ? 1 : 0;
            if (value & 0x8000) trigger(board, channel);
            return;
        case 0x072:
            apu->cnt_h[2] = value & 0xE0FF;
            apu->length[2] = 256 - (value & 0xFF);
            apu->length_enabled[2] = value & 0x4000 ? 1 : 0;
            return;
        case 0x078:
            apu->cnt_h[3] = value;
            apu->length[3] = 64 - (value & 63);
            apu->length_enabled[3] = value & 0x4000 ? 1 : 0;
            return;
        case 0x064:
        case 0x06C:
        case 0x074:
            channel = offset == 0x064 ? 0 : offset == 0x06C ? 1 : 2;
            apu->cnt_x[channel] = value & 0x47FF;
            apu->length_enabled[channel] = value & 0x4000 ? 1 : 0;
            if (value & 0x8000) trigger(board, channel);
            return;
        case 0x07C:
            apu->cnt_x[3] = value & 0xC0FF;
            apu->length_enabled[3] = value & 0x4000 ? 1 : 0;
            if (value & 0x8000) trigger(board, 3);
            return;
        case 0x070:
            /* bit 7 turns the wave channel on; bit 5 selects the bank writes go to */
            apu->cnt_l[2] = value & 0xE0;
            if (!(value & 0x80)) apu->enabled[2] = 0;
            return;
        case 0x080: apu->soundcnt_l = value & 0xFF77; return;
        case 0x082: {
            apu->soundcnt_h = value;
            /* bits 13 and 15 clear the FIFOs as the write lands */
            if (value & 0x2000) {
                apu->fifo_count[0] = apu->fifo_read[0] = apu->fifo_write[0] = 0;
                apu->soundcnt_h &= (uint16_t)~0x2000;
            }
            if (value & 0x8000) {
                apu->fifo_count[1] = apu->fifo_read[1] = apu->fifo_write[1] = 0;
                apu->soundcnt_h &= (uint16_t)~0x8000;
            }
            return;
        }
        case 0x084:
            apu->soundcnt_x = value & 0x80;
            if (!(value & 0x80)) {
                memset(apu->enabled, 0, sizeof(apu->enabled));
                memset(apu->length, 0, sizeof(apu->length));
            }
            return;
        case 0x088: apu->soundbias = value & 0xC3FF; return;
        default: break;
    }
    if (offset >= 0x090 && offset <= 0x09E) {
        uint32_t bank = apu->cnt_l[2] & 0x20 ? 1 : 0, index = offset & 7;
        if (apu->cnt_l[2] & 0x80) return; /* wave RAM is not writable while the channel plays */
        apu->wave_ram[bank][index] = (uint8_t)value;
        apu->wave_ram[bank][index + 1] = (uint8_t)(value >> 8);
        return;
    }
    if (offset == 0x0A0 || offset == 0x0A2) {
        uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
        agb_apu_fifo_write(board, 0, bytes, 2);
    } else if (offset == 0x0A4 || offset == 0x0A6) {
        uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
        agb_apu_fifo_write(board, 1, bytes, 2);
    }
}

void agb_apu_fifo_write(AgbBoard* board, int fifo, const void* bytes, uint32_t size) {
    AgbApu* apu = &board->apu;
    const uint8_t* p = (const uint8_t*)bytes;
    for (uint32_t i = 0; i < size; ++i) {
        if (apu->fifo_count[fifo] == AGB_FIFO_CAPACITY) break;
        apu->fifo[fifo][apu->fifo_write[fifo]] = (int8_t)p[i];
        apu->fifo_write[fifo] = (uint8_t)((apu->fifo_write[fifo] + 1) % AGB_FIFO_CAPACITY);
        ++apu->fifo_count[fifo];
    }
}

static int fifo_pop(AgbApu* apu, int fifo) {
    int8_t sample;
    if (!apu->fifo_count[fifo]) return 0;
    sample = apu->fifo[fifo][apu->fifo_read[fifo]];
    apu->fifo_read[fifo] = (uint8_t)((apu->fifo_read[fifo] + 1) % AGB_FIFO_CAPACITY);
    --apu->fifo_count[fifo];
    return sample;
}

/* ---- the frame sequencer ---- */

static void tick_length(AgbApu* apu) {
    for (int channel = 0; channel < 4; ++channel) {
        if (!apu->length_enabled[channel] || !apu->length[channel]) continue;
        if (--apu->length[channel]) continue;
        apu->enabled[channel] = 0;
    }
}

static void tick_envelope(AgbApu* apu) {
    for (int channel = 0; channel < 4; ++channel) {
        if (!apu->envelope_period[channel] || !apu->enabled[channel]) continue;
        if (--apu->envelope_timer[channel]) continue;
        apu->envelope_timer[channel] = apu->envelope_period[channel];
        {
            int volume = apu->envelope_volume[channel] + apu->envelope_direction[channel];
            if (volume >= 0 && volume <= 15) apu->envelope_volume[channel] = volume;
        }
    }
}

static void tick_sweep(AgbApu* apu) {
    uint32_t shift, period = (apu->cnt_l[0] >> 4) & 7;
    int shadow, frequency;
    if (!apu->enabled[0] || !period) return;
    if (--apu->sweep_timer[0]) return;
    apu->sweep_timer[0] = period;
    shift = apu->cnt_l[0] & 7;
    if (!shift) return;
    shadow = apu->sweep_shadow[0];
    frequency = apu->cnt_l[0] & 0x8 ? shadow - (shadow >> shift) : shadow + (shadow >> shift);
    if (frequency > 0x7FF) {
        apu->enabled[0] = 0;
        return;
    }
    apu->sweep_shadow[0] = frequency;
    apu->cnt_x[0] = (uint16_t)((apu->cnt_x[0] & ~(uint16_t)0x7FF) | frequency);
}

/* ---- channels ---- */

/* Each channel's DAC centers its levels on zero: a playing square is high or low, the wave channel
   plays its four bit digits, and a stopped channel contributes nothing at all. */
static int channel_sample(AgbBoard* board, int channel) {
    AgbApu* apu = &board->apu;
    uint32_t frequency;
    if (!apu->enabled[channel] || !(apu->soundcnt_x & 0x80)) return 0;
    if (channel < 2) {
        int position = apu->duty_position[channel] & 7;
        int high = position < duty_steps[apu->cnt_h[channel] >> 6 & 3];
        frequency = (uint32_t)(apu->cnt_x[channel] & 0x7FF);
        /* eight duty positions per wave cycle, 131072 Hz at the middle frequency */
        apu->phase[channel] += 0x10000u * 32 / (2048u - frequency);
        apu->duty_position[channel] = (uint8_t)((apu->duty_position[channel] + (apu->phase[channel] >> 16)) & 7);
        apu->phase[channel] &= 0xFFFF;
        return high ? apu->envelope_volume[channel] : -apu->envelope_volume[channel];
    }
    if (channel == 2) {
        uint32_t bank = apu->cnt_l[2] & 0x20;
        int sample, centered;
        /* one bank mode plays the selected bank; two bank mode plays the other one */
        if (apu->cnt_l[2] & 0x40) bank = !bank;
        {
            uint8_t byte = apu->wave_ram[bank][(apu->wave_position >> 1) & 15];
            sample = apu->wave_position & 1 ? byte >> 4 : byte & 15;
        }
        frequency = (uint32_t)(apu->cnt_x[2] & 0x7FF);
        /* 32 four bit digits per wave cycle, one quarter of the square's rate */
        apu->phase[2] += 0x10000u * 64 / (2048u - frequency);
        apu->wave_position = (uint8_t)((apu->wave_position + (apu->phase[2] >> 16)) & 31);
        apu->phase[2] &= 0xFFFF;
        centered = sample * 2 - 15;
        switch (apu->cnt_h[2] >> 13 & 3) {
            case 0: return 0;
            case 2: return centered >> 1;
            case 3: return centered >> 2;
            default: return centered;
        }
    }
    /* the noise channel clocks a linear feedback register at its own rate */
    {
        uint32_t code = (uint32_t)(apu->cnt_x[3] & 7), shift = (uint32_t)(apu->cnt_x[3] >> 4 & 15);
        uint32_t rate = code ? 524288u / (code << shift) : 1048576u >> shift;
        int high;
        apu->phase[3] += rate;
        while (apu->phase[3] >= AGB_APU_RATE) {
            uint32_t bit = (apu->noise_lfsr & 1) ^ ((apu->noise_lfsr >> 1) & 1);
            apu->noise_lfsr = (uint16_t)((apu->noise_lfsr >> 1) | (bit << 14));
            if (apu->cnt_x[3] & 0x80) apu->noise_lfsr = (uint16_t)((apu->noise_lfsr & ~(1u << 6)) | (bit << 6));
            apu->phase[3] -= AGB_APU_RATE;
        }
        high = !(apu->noise_lfsr & 1);
        return high ? apu->envelope_volume[3] : -apu->envelope_volume[3];
    }
}

/* The mixer applies the PSG volume ratio and the master volumes, then adds the FIFOs at their own
   volume settings, panning everything with the SOUNDCNT_L enables. */
static void mix_sample(AgbBoard* board, int16_t* left, int16_t* right) {
    AgbApu* apu = &board->apu;
    int psg_left = 0, psg_right = 0, l = 0, r = 0, ratio;
    if (!(apu->soundcnt_x & 0x80)) {
        *left = *right = 0;
        return;
    }
    for (int channel = 0; channel < 4; ++channel) {
        int sample = channel_sample(board, channel);
        if (apu->soundcnt_l & (0x100 << channel)) psg_right += sample;
        if (apu->soundcnt_l & (0x1000 << channel)) psg_left += sample;
    }
    ratio = 1 << (apu->soundcnt_h & 3); /* 25, 50 or 100 percent */
    psg_left = (psg_left * ratio) >> 2;
    psg_right = (psg_right * ratio) >> 2;
    l = psg_left * (int)((apu->soundcnt_l >> 4) & 7) / 7;
    r = psg_right * (int)(apu->soundcnt_l & 7) / 7;
    for (int fifo = 0; fifo < 2; ++fifo) {
        int sample = fifo_pop(apu, fifo);
        if (apu->fifo_count[fifo] <= 16) agb_dma_event(board, fifo ? AGB_DMA_FIFO_B : AGB_DMA_FIFO_A);
        if (!(apu->soundcnt_h & (4 << fifo))) sample >>= 1;     /* the 50 percent volume setting */
        if (apu->soundcnt_h & (0x100 << fifo * 2)) r += sample; /* bits 8 and 10: to the right */
        if (apu->soundcnt_h & (0x200 << fifo * 2)) l += sample; /* bits 9 and 11: to the left */
    }
    *left = (int16_t)(l * 64);
    *right = (int16_t)(r * 64);
}

uint32_t agb_apu_run(AgbBoard* board, uint32_t cycles, int16_t* stereo, uint32_t capacity) {
    AgbApu* apu = &board->apu;
    uint32_t written = 0;
    apu->cycles += cycles;
    while (apu->cycles >= CYCLES_PER_SAMPLE && written < capacity) {
        apu->cycles -= CYCLES_PER_SAMPLE;
        mix_sample(board, stereo + written * 2, stereo + written * 2 + 1);
        ++written;
        if (++apu->ticks % LENGTH_TICK == 0) tick_length(apu);
        if (apu->ticks % SWEEP_TICK == 0) tick_sweep(apu);
        if (apu->ticks % ENVELOPE_TICK == 0) tick_envelope(apu);
    }
    return written;
}
