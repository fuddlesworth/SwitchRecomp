/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* scarletnx-agb: the AGB (Game Boy Advance) system type. A ROM goes through the ARM recompiler into a
   native module, then runs on the emulated board, the way the NX and GNU ABI system types prepare their
   code before playing. */
#include "board/board.h"
#include "cpu/agb_cpu.h"
#include "cpu/memmap.h"
#include "cpu/recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SNX_AGB_CC
#define SNX_AGB_CC "cc"
#endif

static void usage(void) {
    puts("Usage: scarletnx-agb [OPTIONS] ROM.gba\n"
         "Recompiles a Game Boy Advance ROM and runs it on the AGB board.\n"
         "\n"
         "  -r, --recompile   rebuild the module even when the cache is current\n"
         "  --frames=N        run N frames and exit (the default runs until interrupted)\n"
         "  --shot=FILE       write the final frame as a PPM image\n"
         "\n"
         "The recompiled module is cached beside the ROM as <ROM>.code.so.");
}

/* The frame the host presents: 240x160 RGB, straight out of the display device. */
static int write_shot(const char* path, const uint32_t* framebuffer) {
    FILE* file = fopen(path, "wb");
    if (!file) return -1;
    fprintf(file, "P6\n%d %d\n255\n", AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT);
    for (int i = 0; i < AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT; ++i) {
        uint8_t pixel[3] = {(uint8_t)framebuffer[i], (uint8_t)(framebuffer[i] >> 8), (uint8_t)(framebuffer[i] >> 16)};
        fwrite(pixel, 1, 3, file);
    }
    fclose(file);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *path = NULL, *shot = NULL;
    long frames = -1;
    int recompile = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--recompile"))
            recompile = 1;
        else if (!strncmp(argv[i], "--frames=", 9))
            frames = strtol(argv[i] + 9, NULL, 0);
        else if (!strncmp(argv[i], "--shot=", 7))
            shot = argv[i] + 7;
        else if (!strcmp(argv[i], "--"))
            continue;
        else if (!path && argv[i][0] != '-')
             path = argv[i];
         else {
             fprintf(stderr, "scarletnx-agb: unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!path) {
        usage();
        return 2;
    }
    {
        AgbBoard board;
        AgbMap map;
        AgbCpu cpu;
        AgbRecomp recomp;
        char error[512] = {0};
        char* base = NULL;
        uint8_t* image = NULL;
        long length = 0;
        int16_t* audio = NULL;
        int status = 1;
        FILE* file = fopen(path, "rb");
        if (!file) {
            fprintf(stderr, "scarletnx-agb: cannot open %s\n", path);
            return 1;
        }
        fseek(file, 0, SEEK_END);
        length = ftell(file);
        rewind(file);
        image = (uint8_t*)malloc(length > 0 ? (size_t)length : 1);
        if (!image || (length > 0 && fread(image, 1, (size_t)length, file) != (size_t)length)) {
            fprintf(stderr, "scarletnx-agb: cannot read %s\n", path);
            free(image);
            fclose(file);
            return 1;
        }
        fclose(file);
        agb_board_init(&board);
        if (agb_board_insert(&board, image, (size_t)length, error, sizeof error)) {
            fprintf(stderr, "scarletnx-agb: %s: %s\n", path, error);
            free(image);
            agb_board_destroy(&board);
            return 1;
        }
        free(image);
        printf("ScarletNX - %s (AGB)\n", board.cartridge.title);
        if (agb_map_init(&map, &board, error, sizeof error)) {
            fprintf(stderr, "scarletnx-agb: %s\n", error);
            agb_board_destroy(&board);
            return 1;
        }
        agb_cpu_init(&cpu, &board, map.base);
        base = (char*)malloc(strlen(path) + 16);
        if (base) sprintf(base, "%s.code", path);
        agb_recomp_init(&recomp, base ? base : "agb-code", SNX_AGB_CC);
        free(base);
        if (recompile || agb_recomp_load_cache(&recomp, &board, error, sizeof error)) {
            error[0] = 0;
            printf("Recompiling %s\n", path);
            if (agb_recomp_build(&recomp, &board, error, sizeof error)) {
                fprintf(stderr, "scarletnx-agb: %s\n", error);
                goto done;
            }
            printf("Built %zu blocks\n", recomp.count);
        }
        audio = (int16_t*)malloc(2 * sizeof(int16_t) * AGB_APU_RATE);
        if (!audio) goto done;
        for (long frame = 0; frames < 0 || frame < frames; ++frame) {
            uint32_t written = 0;
            if (agb_cpu_run(&board, &cpu, &recomp, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES, audio, AGB_APU_RATE, &written,
                            error, sizeof error)) {
                fprintf(stderr, "scarletnx-agb: %s\n", error);
                goto done;
            }
        }
        if (shot && write_shot(shot, board.ppu.framebuffer)) fprintf(stderr, "scarletnx-agb: cannot write %s\n", shot);
        status = 0;
    done:
        free(audio);
        agb_recomp_free(&recomp);
        agb_map_destroy(&map, &board);
        agb_board_destroy(&board);
        return status;
    }
}
