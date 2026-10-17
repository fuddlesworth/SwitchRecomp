/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NVDEC job recorder: one directory per job (see NvdecRecBlob in nvdec_int.h) */
#include "nx/gpu/video/nvdec_int.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define rec_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define rec_mkdir(p) mkdir(p, 0700)
#endif

static int write_file(const char* path, const void* data, u32 len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f) != 0 ? -1 : 0;
}

int nvdec_rec_job(const char* dir, unsigned n, u32 app, const u32* regs, const NvdecRecBlob* blobs, unsigned nblobs) {
    char sub[4096], path[4096];
    unsigned i;
    FILE* m;
    int err = 0;
    if (!dir || !regs) return -1;
    if (snprintf(sub, sizeof sub, "%s/job-%05u", dir, n) >= (int)sizeof sub) return -1;
    rec_mkdir(dir);
    rec_mkdir(sub); /* exists already is fine; real failures surface below */
    if (snprintf(path, sizeof path, "%s/regs.bin", sub) >= (int)sizeof path) return -1;
    /* register words as bytes, little-endian like the rest of the video code assumes */
    if (write_file(path, regs, 4096)) return -1;
    if (snprintf(path, sizeof path, "%s/meta.txt", sub) >= (int)sizeof path) return -1;
    m = fopen(path, "w");
    if (!m) return -1;
    if (fprintf(m, "app %u\nregs 4096\n", app) < 0) err = 1;
    for (i = 0; i < nblobs; i++) {
        if (!blobs[i].data) continue;
        if (snprintf(path, sizeof path, "%s/%s.bin", sub, blobs[i].name) >= (int)sizeof path) {
            err = 1;
            break;
        }
        if (write_file(path, blobs[i].data, blobs[i].len))
            err = 1;
        else if (fprintf(m, "%s %u\n", blobs[i].name, blobs[i].len) < 0)
            err = 1;
    }
    if (fclose(m) != 0) err = 1;
    return err ? -1 : 0;
}
