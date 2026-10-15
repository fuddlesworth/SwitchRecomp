/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "board/board.h"
#include "ppu/ppu.h"
#include <stdio.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* The framebuffer holds 0xAABBGGRR, each palette channel's five bits widened to eight. */
#define WIDEN(v)     (((uint32_t)(v) << 3) | ((uint32_t)(v) >> 2))
#define PIX(r, g, b) (0xFF000000u | (WIDEN(b) << 16) | (WIDEN(g) << 8) | WIDEN(r))

static AgbBoard board;

static void run_frame(void) { agb_ppu_step(&board, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES); }
static uint32_t pixel(int x, int y) { return board.ppu.framebuffer[y * AGB_SCREEN_WIDTH + x]; }

int main(void) {
    agb_board_init(&board);

    /* timing: VCOUNT walks the visible lines into the blank, and DISPSTAT reports both */
    agb_ppu_write(&board, 0x004, 0x0008 | (160 << 8)); /* vblank interrupt on, match line 160 */
    agb_ppu_step(&board, 960);                         /* the first line's visible dots are over */
    CHECK(agb_ppu_read(&board, 0x006) == 0);
    CHECK(agb_ppu_read(&board, 0x004) & 2); /* the horizontal blank runs to the end of the line */
    agb_ppu_step(&board, AGB_SCANLINE_CYCLES * 159 + 272);
    CHECK(agb_ppu_read(&board, 0x006) == 160);
    CHECK(agb_ppu_read(&board, 0x004) & 1); /* the vertical blank */
    CHECK(agb_ppu_read(&board, 0x004) & 4); /* the line matches the target */
    CHECK(agb_read16(&board, 0x04000202) & AGB_IRQ_VBLANK);
    run_frame();
    CHECK(agb_ppu_read(&board, 0x006) == 160); /* a frame of lines from here wraps back */
    agb_ppu_step(&board, AGB_SCANLINE_CYCLES * 68);
    CHECK(agb_ppu_read(&board, 0x006) == 0);
    agb_io_write16(&board, 0x04000202, 0xFFFF);

    /* mode 3: the bitmap is the screen */
    agb_ppu_write(&board, 0x000, 0x0403); /* mode 3, BG2 on */
    agb_write16(&board, 0x06000000, 0x001F);
    agb_write16(&board, 0x06000000 + (239 + 159 * 240) * 2, 0x03E0);
    run_frame();
    CHECK(pixel(0, 0) == PIX(31, 0, 0));
    CHECK(pixel(239, 159) == PIX(0, 31, 0));
    CHECK(pixel(5, 5) == PIX(0, 0, 0));

    /* mode 4: a paletted bitmap, and DISPCNT picks the page */
    agb_write16(&board, 0x05000002, 0x7C00); /* palette 1: blue */
    agb_write16(&board, 0x05000004, 0x03E0); /* palette 2: green */
    agb_write16(&board, 0x06000000 + 10 + 20 * 240, 0x0201);
    agb_write16(&board, 0x0600A000 + 30 + 40 * 240, 1);
    agb_ppu_write(&board, 0x000, 0x0404);
    run_frame();
    CHECK(pixel(10, 20) == PIX(0, 0, 31));
    CHECK(pixel(11, 20) == PIX(0, 31, 0));
    CHECK(pixel(12, 20) == PIX(0, 0, 0));
    agb_ppu_write(&board, 0x000, 0x0414);
    run_frame();
    CHECK(pixel(30, 40) == PIX(0, 0, 31));
    CHECK(pixel(10, 20) == PIX(0, 0, 0));

    /* mode 0: one text background, every screen entry the same tile in palette bank 1 */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    memset(board.memory.palette, 0, AGB_PALETTE_SIZE);
    agb_write16(&board, 0x05000022, 0x001F); /* bank 1, entry 1: red */
    agb_write16(&board, 0x05000024, 0x03E0); /* bank 1, entry 2: green */
    board.memory.vram[0] = 0x11;             /* tile 0, row 0: pixels 0 1 2 3 are 1 1 2 1 */
    board.memory.vram[1] = 0x12;
    for (int i = 0; i < 1024; ++i) agb_write16(&board, 0x0600F800 + (uint32_t)i * 2, 0x1000);
    agb_ppu_write(&board, 0x008, 0x1F00); /* BG0: char base 0, screen base 31 */
    agb_ppu_write(&board, 0x000, 0x0100); /* mode 0, BG0 on */
    run_frame();
    CHECK(pixel(0, 0) == PIX(31, 0, 0));
    CHECK(pixel(2, 0) == PIX(0, 31, 0));
    CHECK(pixel(3, 0) == PIX(31, 0, 0));
    CHECK(pixel(4, 0) == PIX(0, 0, 0)); /* the tile's second half is transparent */
    CHECK(pixel(0, 8) == PIX(31, 0, 0));
    agb_ppu_write(&board, 0x010, 0x1FC); /* BG0HOFS: -4 */
    run_frame();
    CHECK(pixel(0, 0) == PIX(0, 0, 0));
    CHECK(pixel(4, 0) == PIX(31, 0, 0));
    agb_ppu_write(&board, 0x010, 0);

    /* OBJ: an 8x8 sprite over the background */
    agb_write16(&board, 0x05000202, 0x7C00); /* OBJ palette 1: blue */
    board.memory.vram[0x10000] = 0x01;       /* tile 0, row 0: only pixel 0 is set */
    agb_write16(&board, 0x07000000, 0x0000); /* attr0: y = 0, normal */
    agb_write16(&board, 0x07000002, 10);     /* attr1: x = 10 */
    agb_write16(&board, 0x07000004, 0x0000); /* attr2: tile 0, priority 0 */
    agb_ppu_write(&board, 0x000, 0x1100);    /* mode 0, BG0 and OBJ on */
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 0, 31)); /* the sprite's pixel */
    CHECK(pixel(9, 0) == PIX(31, 0, 0));  /* the background beside it */
    CHECK(pixel(10, 1) == PIX(0, 0, 0));  /* the next sprite row is transparent */
    /* priority 3 puts the sprite under BG0, which is priority 0 */
    agb_write16(&board, 0x07000004, 3 << 10);
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 31, 0)); /* the background tile's third pixel */
    agb_write16(&board, 0x07000004, 0);
    agb_ppu_write(&board, 0x000, 0x0100); /* OBJ off */
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 31, 0));

    /* WIN0 keeps only the layers its control selects */
    agb_ppu_write(&board, 0x000, 0x3100);        /* mode 0, BG0 and OBJ, WIN0 on */
    agb_ppu_write(&board, 0x040, (8 << 8) | 11); /* WIN0H: x 8 to 11 */
    agb_ppu_write(&board, 0x044, 4);             /* WIN0V: y 0 to 4 */
    agb_ppu_write(&board, 0x048, 0x0001);        /* inside WIN0: only BG0 */
    agb_ppu_write(&board, 0x04A, 0x0011);        /* outside: BG0 and OBJ */
    board.memory.vram[0x10000] = 0x11;           /* the sprite covers x 10 and 11 */
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 31, 0)); /* inside WIN0 the sprite is off */
    CHECK(pixel(11, 0) == PIX(0, 0, 31)); /* outside it draws */
    agb_ppu_write(&board, 0x000, 0x1100);
    agb_ppu_write(&board, 0x040, 0);
    agb_ppu_write(&board, 0x048, 0);
    agb_ppu_write(&board, 0x04A, 0);

    /* brightness: the first target lightens toward white */
    agb_ppu_write(&board, 0x050, 0x0010 | (2 << 6)); /* OBJ first target, brightness up */
    agb_ppu_write(&board, 0x054, 8);
    run_frame();
    /* blue (0,0,31) rises by (31-0)*8/16 in red and green */
    CHECK(pixel(10, 0) == PIX(15, 15, 31));
    CHECK(pixel(11, 0) == PIX(15, 15, 31));
    agb_ppu_write(&board, 0x050, 0);
    agb_ppu_write(&board, 0x054, 0);

    /* a semi transparent sprite alpha blends over its second target */
    agb_write16(&board, 0x07000000, 0x0400); /* attr0: mode 1, semi transparent */
    board.memory.vram[0x10000] = 0x01;       /* only x 10 of the sprite is set */
    agb_write16(&board, 0x05000024, 0x001F); /* the background pixel under it goes red */
    agb_ppu_write(&board, 0x050, 0x0100);    /* BG0 is the second target */
    agb_ppu_write(&board, 0x052, 8 | (8 << 8));
    run_frame();
    /* blue over red at eva = evb = 8: (0,0,15) and (15,0,0) */
    CHECK(pixel(10, 0) == PIX(15, 0, 15));
    agb_ppu_write(&board, 0x050, 0);
    agb_ppu_write(&board, 0x052, 0);

    agb_board_destroy(&board);
    puts("agb_ppu: ok");
    return 0;
}
