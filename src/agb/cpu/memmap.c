/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "memmap.h"
#include "board/board.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

#ifndef AGB_MAP_SIZE
#define AGB_MAP_SIZE 0x10000000u
#endif

/* The console mirrors every region across its window: the unit is the region's own size. The palette and
   OAM are smaller than a host page, so their mirrors repeat every page. VRAM is 96 KiB in a 128 KiB
   window whose last 32 KiB repeat the 32 KiB above the bitmap region. */
typedef struct {
    uint32_t address, window, unit;
    int backing;
} AgbMirror;

static void* map_reserve(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void map_release(void* p, size_t size) {
#ifdef _WIN32
    (void)size;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
#else
    if (p) munmap(p, size);
#endif
}

#ifdef _WIN32
static int map_backing(AgbMap* map, size_t size) {
    (void)map;
    (void)size;
    return -1; /* the window is plain RAM and the mirrors are copies */
}
#else
static int map_backing(AgbMap* map, size_t size) {
    int fd = -1;
    void* p;
    if (map->backing_count == (int)(sizeof(map->backings) / sizeof(map->backings[0]))) return -1;
#if defined(__linux__)
    fd = (int)syscall(SYS_memfd_create, "agb", 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size)) {
        close(fd);
        return -1;
    }
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
#else
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (p == MAP_FAILED) {
#if defined(__linux__)
        close(fd);
#endif
        return -1;
    }
    map->backings[map->backing_count] = p;
    map->backing_sizes[map->backing_count] = size;
    map->fds[map->backing_count] = fd;
    ++map->backing_count;
    return map->backing_count - 1;
}
#endif

/* The mirrors only exist where the regions have shared backings. */
static int aliased(AgbMap* map) { return map->backing_count > 0 && map->fds[0] >= 0; }

static int map_alias(AgbMap* map, uint32_t offset, size_t size, size_t backing, size_t within) {
#ifdef _WIN32
    (void)map;
    (void)offset;
    (void)size;
    (void)backing;
    (void)within;
    return -1;
#else
    return mmap(map->base + offset, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, map->fds[backing],
                (off_t)within) == MAP_FAILED
               ? -1
               : 0;
#endif
}

/* The mirrors of one region, one shared backing mapped again and again. */
static int mirror_region(AgbMap* map, uint32_t address, uint32_t window, uint32_t unit, int backing) {
    if (!aliased(map)) return 0; /* no shared backing: the window itself is the storage */
    for (uint32_t offset = 0; offset < window; offset += unit) {
        uint32_t within = offset % (uint32_t)map->backing_sizes[backing];
        if (map_alias(map, address + offset, unit, (size_t)backing, within)) return -1;
    }
    return 0;
}

static int mirror_vram(AgbMap* map, uint32_t base, int backing) {
    if (!aliased(map)) return 0;
    for (uint32_t offset = 0; offset < 0x1000000; offset += 0x20000) {
        for (uint32_t page = 0; page < 0x20000; page += 0x1000) {
            /* the window's last 32 KiB repeat the 32 KiB above the bitmap region */
            uint32_t within = page >= 0x18000 ? page - 0x8000 : page;
            if (map_alias(map, base + 0x06000000 + offset + page, 0x1000, (size_t)backing, within)) return -1;
        }
    }
    return 0;
}

int agb_map_init(AgbMap* map, AgbBoard* board, char* error, size_t error_size) {
    size_t rom = board->has_cartridge ? board->cartridge.size : AGB_ROM_MIN;
    size_t rom_backing_size = (rom + 0xFFF) & ~(size_t)0xFFF; /* host pages, or reads past the image fault */
    int bios = -1, ewram = -1, iwram = -1, io = -1, palette = -1, vram = -1, oam = -1, sram = -1;
    memset(map, 0, sizeof(*map));
    for (int i = 0; i < (int)(sizeof(map->fds) / sizeof(map->fds[0])); ++i) map->fds[i] = -1;
    map->base = (uint8_t*)map_reserve(AGB_MAP_SIZE);
    if (!map->base) {
        snprintf(error, error_size, "cannot reserve the guest memory window");
        return -1;
    }
    if ((bios = map_backing(map, 0x4000)) < 0 || (ewram = map_backing(map, AGB_EWRAM_SIZE)) < 0 ||
        (iwram = map_backing(map, AGB_IWRAM_SIZE)) < 0 || (io = map_backing(map, 0x10000)) < 0 ||
        (palette = map_backing(map, 0x1000)) < 0 || (vram = map_backing(map, 0x20000)) < 0 ||
        (oam = map_backing(map, 0x1000)) < 0 || (sram = map_backing(map, AGB_SRAM_SIZE)) < 0) {
        /* plain RAM everywhere: the mirrors are copies and diverge from their originals */
        for (int i = 0; i < map->backing_count; ++i) {
#ifndef _WIN32
            if (map->fds[i] >= 0) close(map->fds[i]);
#endif
            map->fds[i] = -1;
        }
        bios = ewram = iwram = io = palette = vram = oam = sram = -1;
    }
    if (aliased(map)) {
        AgbMirror mirrors[] = {
            {0x00000000, 0x2000000, 0x4000, bios},          {0x02000000, 0x1000000, AGB_EWRAM_SIZE, ewram},
            {0x03000000, 0x1000000, AGB_IWRAM_SIZE, iwram}, {0x04000000, 0x1000000, 0x10000, io},
            {0x05000000, 0x1000000, 0x1000, palette},       {0x07000000, 0x1000000, 0x1000, oam},
            {0x0E000000, 0x2000000, AGB_SRAM_SIZE, sram},
        };
        for (uint32_t base = 0;; base = 0x80000000u) {
            for (size_t i = 0; i < sizeof(mirrors) / sizeof(mirrors[0]); ++i) {
                if (mirror_region(map, base + mirrors[i].address, mirrors[i].window, mirrors[i].unit,
                                  mirrors[i].backing)) {
                    snprintf(error, error_size, "cannot mirror the guest memory regions");
                    agb_map_destroy(map, board);
                    return -1;
                }
            }
            if (mirror_vram(map, base, vram)) {
                snprintf(error, error_size, "cannot mirror the guest video RAM");
                agb_map_destroy(map, board);
                return -1;
            }
            if (base) break;
        }
    }
    if (board->has_cartridge) {
        int backing = -1;
        if (aliased(map)) {
            backing = map_backing(map, rom_backing_size);
            if (backing >= 0) {
                memcpy(map->backings[backing], board->cartridge.data, rom);
                if (mirror_region(map, 0x08000000, 0x6000000, (uint32_t)rom_backing_size, backing) ||
                    mirror_region(map, 0x88000000, 0x6000000, (uint32_t)rom_backing_size, backing))
                    backing = -1;
            }
        }
        if (backing < 0) memcpy(map->base + 0x08000000, board->cartridge.data, rom); /* the window is RAM */
    }
    /* the board's devices read and write the regions through these pointers */
    agb_memory_free(&board->memory);
    memset(map->base + 0x0E000000, 0xFF, AGB_SRAM_SIZE); /* save memory reads 0xFF until written */
    board->memory.ewram = map->base + 0x02000000;
    board->memory.iwram = map->base + 0x03000000;
    board->memory.palette = map->base + 0x05000000;
    board->memory.vram = map->base + 0x06000000;
    board->memory.oam = map->base + 0x07000000;
    board->memory.sram = map->base + 0x0E000000;
    board->memory.owned = 0;
    map->adopted = 1;
    return 0;
}

void agb_map_destroy(AgbMap* map, AgbBoard* board) {
    for (int i = 0; i < map->backing_count; ++i) {
#ifndef _WIN32
        if (map->fds[i] >= 0) close(map->fds[i]);
        munmap(map->backings[i], map->backing_sizes[i]);
#endif
    }
#ifdef _WIN32
    (void)board;
#endif
    if (map->base) {
        map_release(map->base, AGB_MAP_SIZE);
        if (map->adopted) agb_memory_init(&board->memory); /* the devices fall back to plain buffers */
    } else
        (void)board;
    memset(map, 0, sizeof(*map));
    for (int i = 0; i < (int)(sizeof(map->fds) / sizeof(map->fds[0])); ++i) map->fds[i] = -1;
}
