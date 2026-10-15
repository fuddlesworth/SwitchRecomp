/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* the AGB sound hardware: the four programmable sound channels, the two direct sound FIFOs and the mixer
   that turns them into the stereo stream the host plays. */
#ifndef SNX_AGB_APU_H
#define SNX_AGB_APU_H
#include <stdint.h>

#define AGB_APU_RATE      32768 /* the mixer's output rate in samples per second */
#define AGB_APU_CHANNELS  6     /* square 1, square 2, wave and noise, then FIFO A and FIFO B */
#define AGB_FIFO_CAPACITY 32

typedef struct AgbBoard AgbBoard;
typedef struct {
    /* the PSG channels: registers, then the running state */
    uint16_t cnt_l[4], cnt_h[4], cnt_x[4]; /* one group per channel: SOUNDnCNT_L, _H and _X */
    uint8_t duty_position[2], wave_position;
    uint16_t noise_lfsr;
    uint32_t phase[AGB_APU_CHANNELS]; /* 16.16 phase accumulators */
    int envelope_volume[4], envelope_direction[4], envelope_period[4], envelope_timer[4];
    int length[4], length_enabled[4], sweep_timer[4], sweep_shadow[4];
    int enabled[4];
    uint16_t soundcnt_l, soundcnt_h, soundcnt_x, soundbias;
    uint8_t wave_ram[2][16];
    /* the FIFOs: each holds queued 8 bit signed samples */
    int8_t fifo[2][AGB_FIFO_CAPACITY];
    uint8_t fifo_read[2], fifo_write[2], fifo_count[2];
    uint32_t cycles; /* cycles into the current output sample */
    uint32_t ticks;  /* output samples since startup, for the frame sequencer */
} AgbApu;

/* The register window at 0x04000060 to 0x040000A7, halfword addressed. */
uint16_t agb_apu_read(AgbBoard* board, uint32_t offset);
void agb_apu_write(AgbBoard* board, uint32_t offset, uint16_t value);
/* Runs the sound hardware for the given CPU cycles, appending interleaved stereo samples at
   AGB_APU_RATE to the buffer. Returns the number of stereo samples appended. */
uint32_t agb_apu_run(AgbBoard* board, uint32_t cycles, int16_t* stereo, uint32_t capacity);
/* Queues the bytes of a FIFO write (the direct sound DMA stores here). */
void agb_apu_fifo_write(AgbBoard* board, int fifo, const void* bytes, uint32_t size);
#endif
