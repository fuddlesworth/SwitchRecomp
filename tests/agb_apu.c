/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "board/board.h"
#include <stdio.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static AgbBoard board;
static int16_t out[2 * 20480];

/* Every section starts from a silent board with the master switch on. */
static void reset(void) {
    agb_board_destroy(&board);
    agb_board_init(&board);
    agb_apu_write(&board, 0x084, 0x0080);
}

/* One stereo pair of samples per 512 cycles. */
static uint32_t run_samples(uint32_t count) {
    CHECK(count * 2 <= sizeof(out) / sizeof(out[0]));
    memset(out, 0, sizeof(out));
    return agb_apu_run(&board, count * 512, out, count);
}

int main(void) {
    /* square 2: 50 percent duty at 128 Hz, envelope volume 15 held constant */
    reset();
    agb_apu_write(&board, 0x080, 0x2277); /* both master volumes 7, square 2 to both sides */
    agb_apu_write(&board, 0x082, 0x0002); /* PSG at 100 percent */
    agb_apu_write(&board, 0x068, 0xF080);
    agb_apu_write(&board, 0x06C, 0x8400); /* frequency 1024 and trigger */
    CHECK(run_samples(256) == 256);
    CHECK(out[0] == 960 && out[1] == 960); /* 15 at full volume, scaled to 16 bits */
    CHECK(out[2 * 127] == 960);
    CHECK(out[2 * 128] == -960 && out[2 * 200] == -960); /* the duty cycle's second half */

    /* the envelope steps down at 64 Hz */
    reset();
    agb_apu_write(&board, 0x080, 0x2277);
    agb_apu_write(&board, 0x082, 0x0002);
    agb_apu_write(&board, 0x068, 0xF180); /* direction down, period 1 */
    agb_apu_write(&board, 0x06C, 0x8400);
    CHECK(run_samples(600) == 600);
    CHECK(out[2 * 300] == 960); /* still 15 before the first step */
    CHECK(out[2 * 512] == 896); /* volume 14 after it */

    /* the length counter stops the channel */
    reset();
    agb_apu_write(&board, 0x080, 0x2277);
    agb_apu_write(&board, 0x082, 0x0002);
    agb_apu_write(&board, 0x068, 0xF0A0); /* length load 32 */
    agb_apu_write(&board, 0x06C, 0xC400); /* frequency 1024, length on, trigger */
    CHECK(run_samples(4200) == 4200);
    CHECK(out[2 * 3900] == 960);
    CHECK(out[2 * 4100] == 0); /* the counter ran out at 32 * 128 samples */

    /* the wave channel plays its 4 bit digits */
    reset();
    agb_apu_write(&board, 0x070, 0x00); /* wave RAM writes go to bank 0 */
    for (int i = 0; i < 8; ++i) agb_apu_write(&board, 0x090 + (uint32_t)i * 2, 0xFFFF);
    agb_apu_write(&board, 0x080, 0x4477); /* wave to both sides */
    agb_apu_write(&board, 0x082, 0x0002); /* PSG at 100 percent */
    agb_apu_write(&board, 0x070, 0x80);   /* playing */
    agb_apu_write(&board, 0x072, 0x2000); /* full volume */
    agb_apu_write(&board, 0x074, 0x8400); /* frequency 1024 and trigger */
    CHECK(run_samples(64) == 64);
    CHECK(out[0] == 960 && out[2 * 63] == 960); /* every digit is 15 */

    /* the FIFOs play the bytes the direct sound DMA queues */
    reset();
    agb_apu_write(&board, 0x082, 0x0304); /* FIFO A at full volume to both sides */
    agb_apu_write(&board, 0x0A0, 0x9C64); /* the bytes 100 and -100 */
    CHECK(run_samples(4) == 4);
    CHECK(out[0] == 6400 && out[1] == 6400);
    CHECK(out[2] == -6400 && out[3] == -6400);
    CHECK(out[4] == 0 && out[5] == 0); /* the queue is empty after the two samples */

    /* FIFO B at half volume follows the same path */
    agb_apu_write(&board, 0x082, 0x0C04);
    agb_apu_write(&board, 0x0A4, 100);
    CHECK(run_samples(1) == 1);
    CHECK(out[0] == 3200 && out[1] == 3200);

    /* panning: a channel sent to one side leaves the other silent */
    reset();
    agb_apu_write(&board, 0x082, 0x0002);
    agb_apu_write(&board, 0x080, 0x027F); /* master volumes 7 and 7, square 2 right only */
    agb_apu_write(&board, 0x068, 0xF080);
    agb_apu_write(&board, 0x06C, 0x8400);
    CHECK(run_samples(8) == 8);
    CHECK(out[1] == 960 && out[0] == 0);

    /* the master switch silences everything */
    agb_apu_write(&board, 0x084, 0x0000);
    CHECK(run_samples(8) == 8);
    CHECK(out[0] == 0 && out[1] == 0);

    puts("agb_apu: ok");
    return 0;
}
