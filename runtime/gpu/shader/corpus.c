/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the shader corpus (SWR_SHADER_CORPUS): saving and loading translator inputs for tools/swr_shaderc.c */
#include "gpu/shader/translator.h"
/* ============================================================ shader corpus */
typedef struct {
    char magic[4]; /* SWRS */
    u32 version, si_size, stage, len, max_bytes;
} CorpusHeader;
int shader_corpus_save(const char* dir, const u8* code, u32 len, u32 max_bytes, int stage, const ShaderInfo* si,
                       u64 key) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%016llx_%d.swrs", dir, (unsigned long long)key, stage);
    FILE* f = fopen(path, "rb");
    if (f) { /* already there */
        fclose(f);
        return 1;
    }
    f = fopen(path, "wb");
    if (!f) return 0;
    CorpusHeader h = {{'S', 'W', 'R', 'S'}, 1, (u32)sizeof(ShaderInfo), (u32)stage, len, max_bytes};
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(si, sizeof *si, 1, f) == 1 && fwrite(code, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) remove(path); /* a cut-off file would stay and stop the next save */
    return ok;
}
u8* shader_corpus_load(const char* path, u32* max_bytes, int* stage, ShaderInfo* si) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    CorpusHeader h;
    u8* code = NULL;
    if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "SWRS", 4) && h.version == 1 &&
        h.si_size == sizeof(ShaderInfo) && h.stage <= ST_CS && h.len <= h.max_bytes && h.max_bytes <= (1u << 20) &&
        fread(si, sizeof *si, 1, f) == 1) {
        code = calloc(1, h.max_bytes);
        if (fread(code, 1, h.len, f) != h.len) {
            free(code);
            code = NULL;
        } else {
            *max_bytes = h.max_bytes;
            *stage = (int)h.stage;
        }
    }
    fclose(f);
    return code;
}
