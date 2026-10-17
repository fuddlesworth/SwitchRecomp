/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVDEC recorder: job files round-trip. */
#include "nx/gpu/video/nvdec_int.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static u8 filebuf[8192];
static int readback(const char* path, u32* len) {
    FILE* f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (unsigned long)n > sizeof filebuf) {
        fclose(f);
        return -1;
    }
    if (n > 0 && fread(filebuf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *len = (u32)n;
    return 0;
}

int main(void) {
    static u32 regs[1024];
    for (unsigned i = 0; i < 1024; i++) regs[i] = 0x01010101u * i;
    static const u8 ps[] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const u8 bs[] = {9, 10, 11, 12};
    /* NULL data blobs are skipped, never touched */
    const NvdecRecBlob blobs[3] = {{"ps", ps, (u32)sizeof ps}, {"nil", NULL, 0}, {"bs", bs, (u32)sizeof bs}};
    const char* dir = "nvdec-rec-tmp";
    u32 len = 0;
    check(nvdec_rec_job(dir, 7, 3, regs, blobs, 3) == 0, "record written");
    check(readback("nvdec-rec-tmp/job-00007/regs.bin", &len) == 0 && len == sizeof regs &&
              memcmp(filebuf, regs, len) == 0,
          "regs round-trip");
    check(readback("nvdec-rec-tmp/job-00007/ps.bin", &len) == 0 && len == sizeof ps && memcmp(filebuf, ps, len) == 0,
          "ps round-trip");
    check(readback("nvdec-rec-tmp/job-00007/bs.bin", &len) == 0 && len == sizeof bs && memcmp(filebuf, bs, len) == 0,
          "bs round-trip");
    check(readback("nvdec-rec-tmp/job-00007/meta.txt", &len) == 0 && len > 0, "meta written");
    remove("nvdec-rec-tmp/job-00007/regs.bin");
    remove("nvdec-rec-tmp/job-00007/ps.bin");
    remove("nvdec-rec-tmp/job-00007/bs.bin");
    remove("nvdec-rec-tmp/job-00007/meta.txt");
    remove("nvdec-rec-tmp/job-00007");
    remove("nvdec-rec-tmp");
    /* unwritable target: nonzero, no crash */
    check(nvdec_rec_job("/proc/nvdec-rec-test", 0, 3, regs, blobs, 3) != 0, "bad dir refused");
    if (failures) return 1;
    printf("recorder: round-trip ok\n");
    return 0;
}
