/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* Translates a shader corpus (SWR_SHADER_CORPUS=dir in a game run) offline and checks the result: every shader is
   compiled to SPIR-V with glslang and, with --validate, checked with spirv-val.

     swr-shaderc CORPUS_DIR [--out DIR] [--validate] [--cache DIR] [--verbose]
     swr-shaderc --nvn DIR --cache DIR

   --out writes each shader's GLSL and .spv to DIR, to compare the translator's output between two builds. --cache goes
   through the runtime's SPIR-V cache in DIR, as the renderer does. Exit status 1 when a shader failed to compile or to
   validate, a corpus file could not be read or an output could not be written. Shaders the translator doesn't support
   are counted, not failures.

   --nvn fills the SPIR-V cache before the game first runs (scripts/recomp.py does it): DIR holds the game's own NVN
   shader files, as Catherine ships them in romfs. Their programs translate the way the renderer translates them on
   first use (fragment programs alone, a vertex shader's VertexA and VertexB merged), for draws without integer
   attributes, textures or outputs, so those shaders skip glslang then. */
#include "gpu/shader/shader.h"
#include "gpu/vk/spirv.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#define USAGE                                                                                                          \
    "usage: %s CORPUS_DIR [--out DIR] [--validate] [--cache DIR] [--verbose]\n"                                        \
    "       %s --nvn DIR --cache DIR [--verbose]\n"

static int write_file(const char* path, const void* p, size_t n) {
    FILE* f = fopen(path, "wb");
    int ok = f && fwrite(p, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    if (!ok) fprintf(stderr, "cannot write %s\n", path);
    return ok;
}

/* s as one argument of the shell popen() runs: in single quotes for sh, in double quotes for cmd.exe (a Windows path
   can't hold a double quote) */
static void shell_quote(char* out, size_t n, const char* s) {
#ifdef _WIN32
    snprintf(out, n, "\"%s\"", s);
#else
    size_t o = 0;
    out[o++] = '\'';
    for (; *s && o + 6 < n; s++) {
        if (*s == '\'') { /* close the quotes, an escaped quote, open them again */
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else
            out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = 0;
#endif
}

/* NVN shader files: a 0x19866891 magic, a control section, then the program (its header first) at 2896 bytes */
#define NVN_MAGIC 0x19866891u
#define NVN_CODE  2896
static int nvn_dir(const char* dir, const char* cache, int verbose) {
    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "cannot open %s\n", dir);
        return 2;
    }
    int total = 0, done = 0, failed = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        /* Room for twice the program: a VertexB found at offset o is read for 0x10000 bytes like any program
           (shader_hash, shader_merge_dual), up to o bytes past the file's part, which the zeroed second half holds */
        u8* buf = calloc(1, NVN_CODE + 2 * 0x10000);
        if (!buf) {
            fclose(f);
            continue;
        }
        size_t n = fread(buf, 1, 0x10000 + NVN_CODE, f);
        fclose(f);
        u32 magic, sph0;
        memcpy(&magic, buf, 4);
        memcpy(&sph0, buf + NVN_CODE, 4);
        if (n <= NVN_CODE + 0x50 || magic != NVN_MAGIC) {
            free(buf);
            continue;
        }
        /* the program header's type (bits 0-4: 1 vertex/geometry, 2 pixel) and shader type (bits 10-13: 0 VertexA,
           1 VertexB, 5 pixel) */
        u32 hdr = sph0 & 0x1F, type = (sph0 >> 10) & 0xF;
        int stage = hdr == 2 && type == 5 ? ST_FS : hdr == 1 && type <= 1 ? ST_VS : -1;
        if (stage < 0) {
            free(buf);
            continue;
        }
        u8* prog = buf + NVN_CODE;
        u8* merged = NULL;
        if (stage == ST_VS && type == 0) { /* VertexA: VertexB follows it, 256-byte aligned */
            u32 len;
            shader_hash(prog, 0x10000, &len);
            u8* b = NULL;
            for (u32 o = (len + 255) & ~255u; NVN_CODE + o + 0x50 < n && !b; o += 256) {
                u32 w;
                memcpy(&w, prog + o, 4);
                if ((w & 0x1F) == 1 && ((w >> 10) & 0xF) == 1) b = prog + o;
            }
            merged = b ? malloc(0x10000) : NULL;
            if (!merged || !shader_merge_dual(prog, b, merged, 0x10000)) {
                free(merged);
                free(buf);
                continue;
            }
            prog = merged;
        }
        total++;
        ShaderInfo si;
        memset(&si, 0, sizeof si);
        char* glsl = vk_glsl_post(shader_to_glsl(prog, 0x10000, stage, &si), stage);
        free(merged);
        free(buf);
        if (!glsl || si.unsupported) {
            if (verbose) printf("%s: not translated\n", e->d_name);
            free(glsl);
            continue;
        }
        char err[4096];
        size_t nwords = 0;
        u32* spv = vk_spirv_cached(glsl, stage, cache, &nwords, err, sizeof err);
        free(glsl);
        if (spv)
            done++;
        else {
            failed++;
            if (verbose) printf("%s: %s\n", e->d_name, err);
        }
        free(spv);
    }
    closedir(d);
    printf("%d shaders: %d in the SPIR-V cache, %d failed to compile\n", total, done, failed);
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    const char *dir = NULL, *out = NULL, *cache = NULL, *nvn = NULL;
    int validate = 0, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nvn") && i + 1 < argc)
            nvn = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            cache = argv[++i];
        else if (!strcmp(argv[i], "--validate"))
            validate = 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (argv[i][0] != '-' && !dir)
            dir = argv[i];
        else {
            fprintf(stderr, USAGE, argv[0], argv[0]);
            return 2;
        }
    }
    if (nvn) {
        if (!cache) {
            fprintf(stderr, "--nvn needs --cache DIR\n");
            return 2;
        }
        return nvn_dir(nvn, cache, verbose);
    }
    if (!dir) {
        fprintf(stderr, USAGE, argv[0], argv[0]);
        return 2;
    }
    /* Without --out, spirv-val reads each .spv from a file of this process's own in the temporary folder */
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
#ifdef _WIN32
    if (!tmpdir) tmpdir = ".";
#else
    if (!tmpdir) tmpdir = "/tmp";
#endif
    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "cannot open %s\n", dir);
        return 2;
    }
    int total = 0, bad_file = 0, untranslated = 0, compile_fail = 0, invalid = 0, ok = 0, unwritten = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        size_t nl = strlen(e->d_name);
        if (nl < 6 || strcmp(e->d_name + nl - 5, ".swrs")) continue;
        char path[2048], base[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(base, sizeof base, "%.*s", (int)(nl - 5), e->d_name);
        total++;
        u32 max_bytes;
        int stage;
        ShaderInfo si;
        u8* code = shader_corpus_load(path, &max_bytes, &stage, &si);
        if (!code) {
            bad_file++;
            if (verbose) printf("%s: not a corpus file for this build\n", base);
            continue;
        }
        char* glsl = shader_to_glsl(code, max_bytes, stage, &si);
        glsl = vk_glsl_post(glsl, stage); /* the GLSL the renderer compiles */
        free(code);
        if (!glsl || si.unsupported) {
            untranslated++;
            if (verbose)
                printf("%s: not translated (%d unsupported, first %s)\n", base, si.unsupported, si.first_unsupported);
            free(glsl);
            continue;
        }
        if (out) {
            char p[2200];
            snprintf(p, sizeof p, "%s/%s.glsl", out, base);
            unwritten += !write_file(p, glsl, strlen(glsl));
        }
        char err[4096];
        size_t nwords = 0;
        u32* spv = vk_spirv_cached(glsl, stage, cache, &nwords, err, sizeof err);
        free(glsl);
        if (!spv) {
            compile_fail++;
            printf("%s: %s\n", base, err);
            continue;
        }
        char spvpath[2200];
        if (out)
            snprintf(spvpath, sizeof spvpath, "%s/%s.spv", out, base);
        else
            snprintf(spvpath, sizeof spvpath, "%s/swr-shaderc-%d-%s.spv", tmpdir, (int)getpid(), base);
        int written = out || validate ? write_file(spvpath, spv, nwords * 4) : 1;
        free(spv);
        if (!written) {
            unwritten++;
            continue;
        }
        if (validate) {
            char quoted[4500], cmd[4600];
            shell_quote(quoted, sizeof quoted, spvpath);
            snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s 2>&1", quoted);
            FILE* p = popen(cmd, "r");
            char msg[1024] = "";
            size_t got = p ? fread(msg, 1, sizeof msg - 1, p) : 0;
            msg[got] = 0;
            int rc = p ? pclose(p) : -1;
            if (!out) remove(spvpath);
            if (rc != 0) {
                invalid++;
                printf("%s: spirv-val: %s\n", base, msg);
                continue;
            }
        }
        ok++;
    }
    closedir(d);
    printf("%d shaders: %d ok, %d not translated, %d failed to compile, %d invalid SPIR-V, %d unreadable\n", total, ok,
           untranslated, compile_fail, invalid, bad_file);
    if (unwritten) printf("%d files could not be written\n", unwritten);
    return (compile_fail || invalid || bad_file || unwritten) ? 1 : 0;
}
