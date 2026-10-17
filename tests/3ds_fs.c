/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__) /* Darwin defines no __unix__ */
#include <sys/resource.h>
#include <sys/stat.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif
#define CMD (SNX_CTR_INITIAL_TLS + 0x80)
static void word(SnxCtrProcess* p, uint32_t address, uint32_t v) {
    unsigned char b[4];
    for (unsigned i = 0; i < 4; ++i) b[i] = (unsigned char)(v >> (i * 8));
    assert(snx_ctr_write_ram(p->space, address, b, 4) == SNX_CTR_MEM_OK);
}
static uint32_t get(SnxCtrProcess* p, unsigned n) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, CMD + 4 * n, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void request(SnxCtrProcess* p, uint32_t h, const uint32_t* w, unsigned n, SnxCtrSvcResult state,
                    uint32_t transport) {
    unsigned char zero[256] = {0};
    assert(snx_ctr_write_ram(p->space, CMD, zero, 256) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < n; ++i) word(p, CMD + i * 4, w[i]);
    p->cpu.state.cpu.r[0] = h;
    assert(snx_ctr_svc(p, 0x32) == state);
    assert(p->cpu.state.cpu.r[0] == (state == SNX_CTR_SVC_RETURN ? transport : h));
}
static bool no_mmio(void* context, uint32_t address, const void* bytes, size_t size) {
    (void)context;
    (void)address;
    (void)bytes;
    (void)size;
    assert(0 && "FS buffers must never invoke MMIO");
    return false;
}
static unsigned objects(SnxCtrProcess* p) {
    unsigned n = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) n += !!p->objects[i].type;
    return n;
}
static void archives(SnxCtrProcess* p, uint32_t fs, bool absent) {
    uint32_t a[] = {0x80c00c2, 3, 1, 1, 0x4002, 0x300020};
    unsigned before = objects(p);
    uint64_t serial = p->archive_sequence;
    a[5] = 0x304000;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
    assert(p->archive_sequence == serial && objects(p) == before);
    a[5] = 0x300020;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    if (absent) {
        assert(get(p, 1) == 0xc8804464 && !p->archive_sequence);
        return;
    }
    assert(get(p, 0) == 0x80c00c0 && !get(p, 1));
    uint64_t token = (uint64_t)get(p, 2) | (uint64_t)get(p, 3) << 32;
    assert(token && objects(p) == before);
    uint32_t open[] = {0x80201c2, 0, (uint32_t)token, (uint32_t)(token >> 32), 2, 12, 1, 0, 0x30002, 0x300000};
    request(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 0) == 0x8020042 && !get(p, 1) && get(p, 2) == 0x10);
    uint32_t file = get(p, 3);
    uint32_t close[] = {0x80e0080, (uint32_t)token, (uint32_t)(token >> 32)};
    request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804465);
    request(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804465);
    uint32_t size[] = {0x8040000};
    request(p, file, size, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && get(p, 2));
    uint32_t finish[] = {0x8080000};
    request(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && !snx_ctr_handle_close(p, file));
    uint64_t tokens[CTR_SELF_ARCHIVES];
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
        request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        tokens[i] = (uint64_t)get(p, 2) | (uint64_t)get(p, 3) << 32;
        assert(tokens[i] > token);
    }
    serial = p->archive_sequence;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xd0401834 && p->archive_sequence == serial && objects(p) == before);
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) {
        close[1] = (uint32_t)tokens[i];
        close[2] = (uint32_t)(tokens[i] >> 32);
        request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
    }
    p->archive_sequence = UINT32_MAX;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && !get(p, 2) && get(p, 3) == 1);
    open[2] = 0;
    open[3] = 1;
    request(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    file = get(p, 3);
    close[1] = 0;
    close[2] = 1;
    request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    request(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && !snx_ctr_handle_close(p, file));
    serial = p->archive_sequence;
    p->archive_sequence = UINT64_MAX;
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xd0401834 && p->archive_sequence == UINT64_MAX);
    for (unsigned i = 0; i < CTR_SELF_ARCHIVES; ++i) assert(!p->self_archives[i].token);
    p->archive_sequence = serial;
}
/* The backing file's real host modification time, computed independently here
   from the host stat/FILETIME and compared with both the ControlArchive wire
   reply and the RomFS API result. */
static uint64_t host_datetime(const char* path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA data;
    assert(GetFileAttributesExA(path, GetFileExInfoStandard, &data));
    ULARGE_INTEGER time = {.LowPart = data.ftLastWriteTime.dwLowDateTime,
                           .HighPart = data.ftLastWriteTime.dwHighDateTime};
    assert(time.QuadPart >= UINT64_C(116444736000000000));
    return (time.QuadPart - UINT64_C(116444736000000000)) / 10000 + UINT64_C(2208988800000);
#else
    struct stat st;
    assert(!stat(path, &st) && st.st_mtime >= 0);
#if defined(__APPLE__) /* as fs/tree.c reads it */
    return (uint64_t)st.st_mtime * 1000 + (uint64_t)st.st_mtimespec.tv_nsec / 1000000 + UINT64_C(2208988800000);
#else
    return (uint64_t)st.st_mtime * 1000 + (uint64_t)st.st_mtim.tv_nsec / 1000000 + UINT64_C(2208988800000);
#endif
#endif
}
static uint64_t get64(SnxCtrProcess* p, uint32_t address) {
    unsigned char b[8];
    assert(snx_ctr_read_ram(p->space, address, b, sizeof b) == SNX_CTR_MEM_OK);
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)b[i] << (i * 8);
    return v;
}
static void enc16(unsigned char* b, const uint16_t* units, unsigned bytes) {
    assert(bytes <= 256);
    for (unsigned i = 0; i < bytes; ++i) b[i] = (unsigned char)(units[i / 2] >> ((i & 1) * 8));
}
static void utf16(SnxCtrProcess* p, uint32_t address, const uint16_t* units, unsigned bytes) {
    unsigned char b[256];
    enc16(b, units, bytes);
    assert(snx_ctr_write_ram(p->space, address, b, bytes) == SNX_CTR_MEM_OK);
}
static void timestamps(SnxCtrProcess* p, uint32_t fs, const char* directory) {
    uint32_t a[] = {0x80c00c2, 3, 1, 1, 0x4002, 0x300020};
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    uint32_t lo = get(p, 2), hi = get(p, 3);
    char* hello_path = snx_path_join(directory, "romfs/hello.txt");
    char* root_path = snx_path_join(directory, "romfs");
    uint64_t hello = host_datetime(hello_path), root = host_datetime(root_path);
    free(hello_path);
    free(root_path);
    static const uint16_t hello16[] = {'h', 'e', 'l', 'l', 'o', '.', 't', 'x', 't', 0};
    static const uint16_t slashed16[] = {'\\', 'h', 'e', 'l', 'l', 'o', '.', 't', 'x', 't', 0};
    static const uint16_t root16[] = {0};
    static const uint16_t missing16[] = {'n', 'o', 'p', 'e', 0};
    static const uint16_t bad16[] = {0xd800, 0};
    uint32_t cmd[] = {0x80d0144, lo, hi, 1, 20, 8, (20u << 4) | 10, 0x300400, (8u << 4) | 12, 0x300500};
    utf16(p, 0x300400, hello16, 20);
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 0) == 0x080d0040 && !get(p, 1));
    assert(get64(p, 0x300500) == hello && hello > 2208988800000ull);
    uint64_t api = 0;
    unsigned char raw[256];
    enc16(raw, hello16, 20);
    assert(snx_ctr_romfs_time(p->romfs, raw, 20, &api) == SNX_CTR_TIME_OK && api == hello);
    utf16(p, 0x300400, slashed16, 22);
    cmd[4] = 22;
    cmd[6] = (22u << 4) | 10;
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && get64(p, 0x300500) == hello); /* \ separator resolves the same backing file */
    utf16(p, 0x300400, root16, 2);
    cmd[4] = 2;
    cmd[6] = (2u << 4) | 10;
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && get64(p, 0x300500) == root);
    enc16(raw, root16, 2);
    assert(snx_ctr_romfs_time(p->romfs, raw, 2, &api) == SNX_CTR_TIME_OK && api == root);
    word(p, 0x300500, 0xdeadbeef);
    utf16(p, 0x300400, missing16, 10);
    cmd[4] = 10;
    cmd[6] = (10u << 4) | 10;
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804478 && get(p, 0) == 0x080d0040 && (uint32_t)get64(p, 0x300500) == 0xdeadbeef);
    enc16(raw, missing16, 10);
    assert(snx_ctr_romfs_time(p->romfs, raw, 10, &api) == SNX_CTR_TIME_NOTFOUND);
    utf16(p, 0x300400, bad16, 4);
    cmd[4] = 4;
    cmd[6] = (4u << 4) | 10;
    request(p, fs, cmd, 10, SNX_CTR_SVC_UNSUPPORTED, 0); /* unpaired surrogate is not a path */
    enc16(raw, bad16, 4);
    assert(snx_ctr_romfs_time(p->romfs, raw, 4, &api) == SNX_CTR_TIME_INVALID);
    cmd[3] = 0;
    request(p, fs, cmd, 10, SNX_CTR_SVC_UNSUPPORTED, 0); /* save commit needs a save archive */
    cmd[3] = 1;
    cmd[4] = 3;
    cmd[6] = (3u << 4) | 10;
    request(p, fs, cmd, 10, SNX_CTR_SVC_UNSUPPORTED, 0); /* odd path size */
    cmd[4] = 20;
    cmd[6] = (20u << 4) | 10;
    request(p, fs, cmd, 10, SNX_CTR_SVC_UNSUPPORTED, 0); /* stale bytes are not the earlier path */
    cmd[6] = (20u << 4) | 11;
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
    cmd[6] = (20u << 4) | 10;
    cmd[7] = 0x304000;
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0xe0e01bf5); /* unmapped path input */
    cmd[7] = 0x300400;
    cmd[9] = 0x100000;
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0xe0e01bf5); /* read-only output span */
    cmd[9] = 0x300500;
    cmd[1] = lo + 0x1000;
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804465); /* stale archive token */
    cmd[1] = lo;
    uint32_t close[] = {0x80e0080, lo, hi};
    request(p, fs, close, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    request(p, fs, cmd, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804465); /* timestamps need a live binding */
}
/* The sdmc area is read-write, backed by the shared host tree like NX: real
   create/write/read/resize/rename/delete, with host files as the durable
   store. Directory probes match the devoptab's stat fallback. */
static void sd_area(SnxCtrProcess* p, uint32_t fs, const char* directory) {
    uint32_t a[] = {0x80c00c2, 9, 1, 1, 0x4002, 0x300020};
    request(p, fs, a, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    uint32_t lo = get(p, 2), hi = get(p, 3);
    static const uint16_t dir16[] = {'d', 0};
    static const uint16_t file16[] = {'d', '/', 'f', '.', 'b', 'i', 'n', 0};
    static const uint16_t moved16[] = {'d', '/', 'm', '.', 'b', 'i', 'n', 0};
    utf16(p, 0x300400, dir16, 4);
    uint32_t mkdir[] = {0x8090182, 0, lo, hi, 4, 4, 0, 0x10002, 0x300400};
    request(p, fs, mkdir, 9, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    request(p, fs, mkdir, 9, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc88044be); /* already exists */
    utf16(p, 0x300400, file16, 16);
    uint32_t mkfile[] = {0x8080202, 0, lo, hi, 4, 16, 0, 4, 0, 0x40002, 0x300400};
    request(p, fs, mkfile, 11, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    uint32_t open[] = {0x80201c2, 0, lo, hi, 4, 16, 3, 0, 0x40002, 0x300400};
    request(p, fs, open, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 0) == 0x8020042 && !get(p, 1) && get(p, 2) == 0x10);
    uint32_t file = get(p, 3);
    word(p, 0x300600, 0x64636261); /* "abcd" */
    uint32_t write[] = {0x8030102, 0, 0, 4, 0, (4u << 4) | 10, 0x300600};
    request(p, file, write, 7, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && get(p, 2) == 4);
    uint32_t size[] = {0x8040000};
    request(p, file, size, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && get(p, 2) == 4 && !get(p, 3));
    uint32_t grow[] = {0x8050080, 6, 0};
    request(p, file, grow, 3, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    word(p, 0x300500, 0);
    word(p, 0x300504, 0);
    uint32_t read[] = {0x80200c2, 0, 0, 6, (6u << 4) | 12, 0x300500};
    request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && get(p, 2) == 6);
    unsigned char bytes[8];
    assert(snx_ctr_read_ram(p->space, 0x300500, bytes, 6) == SNX_CTR_MEM_OK);
    assert(!memcmp(bytes, "abcd\0\0", 6)); /* grown with zeros */
    uint32_t finish[] = {0x8080000};
    request(p, file, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && !snx_ctr_handle_close(p, file));
    /* stat-style probes: a directory is not a file, then opens as directory */
    utf16(p, 0x300400, dir16, 4);
    uint32_t probe[] = {0x80201c2, 0, lo, hi, 4, 4, 1, 0, 0x10002, 0x300400};
    request(p, fs, probe, 10, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804702);
    utf16(p, 0x300400, dir16, 4);
    uint32_t opend[] = {0x80b0102, lo, hi, 4, 4, 0x10002, 0x300400};
    request(p, fs, opend, 7, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 0) == 0x80b0042 && !get(p, 1) && get(p, 2) == 0x10);
    uint32_t handle = get(p, 3);
    request(p, handle, finish, 1, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1) && !snx_ctr_handle_close(p, handle));
    /* rename through the wire, then delete */
    utf16(p, 0x300400, file16, 16);
    utf16(p, 0x300700, moved16, 16);
    uint32_t rename[] = {0x8050244, 0, lo, hi, 4, 16, lo, hi, 4, 16, 0x40002, 0x300400, (16u << 14) | (1u << 11) | 2,
                         0x300700};
    request(p, fs, rename, 14, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    /* the host file under the shared sdmc tree carries the written bytes */
    char* host = snx_path_join(directory, "sdmc/d/m.bin");
    FILE* disk = fopen(host, "rb");
    assert(disk);
    char content[8] = {0};
    assert(fread(content, 1, 6, disk) == 6 && !memcmp(content, "abcd\0\0", 6));
    assert(!fclose(disk));
    free(host);
    uint32_t remove[] = {0x8040142, 0, lo, hi, 4, 16, 0x40002, 0x300700};
    request(p, fs, remove, 8, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    request(p, fs, remove, 8, SNX_CTR_SVC_RETURN, 0);
    assert(get(p, 1) == 0xc8804478); /* gone */
    utf16(p, 0x300400, dir16, 4);
    uint32_t rmdir[] = {0x8060142, 0, lo, hi, 4, 4, 0x10002, 0x300400};
    request(p, fs, rmdir, 8, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
    /* save commit on the SD area succeeds: writes already landed on the host */
    uint32_t commit[] = {0x80d0144, lo, hi, 0, 12, 8, (12u << 4) | 10, 0x300400, (8u << 4) | 12, 0x300500};
    request(p, fs, commit, 10, SNX_CTR_SVC_RETURN, 0);
    assert(!get(p, 1));
}
int main(int argc, char** argv) {
    assert(argc == 4);
#if defined(__unix__) || defined(__APPLE__)
    if (!strcmp(argv[3], "fallback")) {
        struct rlimit limit = {64, 64};
        assert(!setrlimit(RLIMIT_NOFILE, &limit));
    }
#endif
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .bss_size = 0x3000,
                         .code = code,
                         .code_size = sizeof code};
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        bool absent = !strcmp(argv[3], "absent");
        assert(snx_ctr_process_mount_fs(p, argv[1]) && (!!p->romfs != absent));
        if (!absent) assert(!snx_ctr_process_mount_fs(p, argv[1]));
        uint32_t fs;
        assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, &fs));
        uint32_t open[] = {0x8030204, 0, 3, 1, 1, 2, 12, 1, 0, 0x4802, 0x300020, 0x30002, 0x300000};
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == SNX_CTR_IPC_INVALID_STATE);
        uint32_t init[] = {0x8010002, 0x20, 0xffffffff};
        request(p, fs, init, 3, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        unsigned before = objects(p);
        archives(p, fs, absent);
        if (!absent) timestamps(p, fs, argv[1]);
        sd_area(p, fs, argv[1]);
        if (absent) {
            request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
            assert(get(p, 1) == 0xc8804464 && objects(p) == before);
            snx_ctr_process_free(p);
            continue;
        }
        open[9] = 0x8802;
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        assert(objects(p) == before && get(p, 0) == open[0]);
        open[9] = 0x4802;
        open[10] = 0x304000;
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(objects(p) == before);
        open[10] = 0x300020;
        open[7] = 3;
        request(p, fs, open, 13, SNX_CTR_SVC_UNSUPPORTED, 0);
        assert(objects(p) == before);
        open[7] = 1;
        uint32_t aliases[CTR_HANDLES], n = 0, unused;
        while (n < CTR_HANDLES && !snx_ctr_handle_add(p, 1, &aliases[n])) ++n;
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd8600413 && objects(p) == before);
        for (unsigned i = 0; i < n; ++i) assert(!snx_ctr_handle_close(p, aliases[i]));
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x8030042 && !get(p, 1) && get(p, 2) == 0x10);
        uint32_t file = get(p, 3), id = snx_ctr_handle_object(p, file), duplicate;
        assert(id && p->objects[id].type == CTR_OBJ_FILE && !snx_ctr_handle_add(p, id, &duplicate));
        uint32_t size_cmd[] = {0x8040000};
        request(p, file, size_cmd, 1, SNX_CTR_SVC_RETURN, 0);
        uint64_t size = (uint64_t)get(p, 2) | (uint64_t)get(p, 3) << 32;
        assert(size && size < 12000);
        assert(get(p, 0) == 0x80400c0 && size == snx_ctr_romfs_size(p->romfs));
        /* the fixed 256-byte negative-read patterns below need 256 bytes even
           when a tiny fixture's whole RomFS view is smaller */
        size_t alloc = size < 256 ? 256 : (size_t)size;
        unsigned char *expected = malloc(alloc), *actual = malloc(alloc);
        assert(expected && actual);
        assert(snx_ctr_romfs_read(p->romfs, 0, expected, (size_t)size));
        uint32_t read[] = {0x80200c2, 0, 0, (uint32_t)size, (uint32_t)size << 4 | 12, 0x300800};
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 0) == 0x8020082 && !get(p, 1) && get(p, 2) == size && get(p, 3) == read[4] &&
               get(p, 4) == read[5]);
        assert(snx_ctr_read_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK &&
               !memcmp(actual, expected, (size_t)size));
        if (!model) assert(!snx_write_file(argv[2], actual, (size_t)size));
        for (uint32_t off = 0; off < size; off += 137) {
            size_t n = size - off < 139 ? (size_t)(size - off) : 139;
            assert(snx_ctr_romfs_read(p->romfs, off, actual, n) && !memcmp(actual, expected + off, n));
        }
        if (!strcmp(argv[3], "fallback")) {
            char* source = snx_path_join(argv[1], "romfs/hello.txt");
            char* moved = snx_path_join(argv[1], "romfs/hello.moved");
            assert(source && moved && !rename(source, moved));
            memset(actual, 0x55, (size_t)size);
            assert(snx_ctr_write_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK);
            request(p, file, read, 6, SNX_CTR_SVC_UNSUPPORTED, 0);
            assert(get(p, 0) == read[0]);
            assert(snx_ctr_read_ram(p->space, read[5], actual, (size_t)size) == SNX_CTR_MEM_OK);
            for (uint64_t i = 0; i < size; ++i) assert(actual[i] == 0x55);
            assert(!rename(moved, source));
            free(source);
            free(moved);
        }
        read[1] = (uint32_t)size - 2;
        read[3] = 32;
        read[4] = 32 << 4 | 12;
        request(p, duplicate, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 2) == 2);
        read[1] = (uint32_t)size + 1;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 2));
        read[1] = read[2] = UINT32_MAX;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 2));
        read[1] = read[2] = 0;
        read[3] = 512;
        read[4] = 512 << 4 | 12;
        read[5] = 0x303f00;
        unsigned char fill[256];
        memset(fill, 0x55, sizeof fill);
        assert(snx_ctr_write_ram(p->space, read[5], fill, 256) == SNX_CTR_MEM_OK);
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(get(p, 0) == read[0]);
        assert(snx_ctr_read_ram(p->space, read[5], actual, 256) == SNX_CTR_MEM_OK && !memcmp(fill, actual, 256));
        read[5] = 0x200000;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        assert(snx_ctr_map(p->space, 0x50000000, 0x10100000, 4096, 3) == SNX_CTR_MEM_OK);
        snx_ctr_memory_mmio(p->memory, NULL, NULL, no_mmio);
        read[5] = 0x50000000;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, 0xe0e01bf5);
        read[5] = 0x300800;
        read[4] = 16 << 4 | 12;
        request(p, file, read, 6, SNX_CTR_SVC_RETURN, SNX_CTR_IPC_BAD_DESCRIPTOR);
        read[4] = 512 << 4 | 10;
        request(p, file, read, 6, SNX_CTR_SVC_UNSUPPORTED, 0);
        uint32_t unknown[] = {0x8090000};
        request(p, file, unknown, 1, SNX_CTR_SVC_UNSUPPORTED, 0);
        uint32_t close[] = {0x8080000};
        request(p, file, close, 1, SNX_CTR_SVC_RETURN, 0);
        assert(!get(p, 1));
        request(p, duplicate, size_cmd, 1, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == SNX_CTR_IPC_INVALID_STATE);
        assert(!snx_ctr_handle_close(p, file) && p->objects[id].refs == 1 && !snx_ctr_handle_close(p, duplicate) &&
               !p->objects[id].type);
        request(p, file, size_cmd, 1, SNX_CTR_SVC_RETURN, 0xd8e007f7);
        for (n = 0; n < 64; ++n) {
            request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
            assert(!get(p, 1));
            aliases[n] = get(p, 3);
        }
        before = objects(p);
        request(p, fs, open, 13, SNX_CTR_SVC_RETURN, 0);
        assert(get(p, 1) == 0xd0401834 && objects(p) == before);
        for (n = 0; n < 64; ++n) assert(!snx_ctr_handle_close(p, aliases[n]));
        assert(!snx_ctr_handle_add(p, 1, &unused) && !snx_ctr_handle_close(p, unused));
        free(expected);
        free(actual);
        snx_ctr_process_free(p);
    }
    puts("CTR RomFS: real files, shared/fallback reads, checked buffers, quotas, close/duplicate lifetime passed");
    return 0;
}
