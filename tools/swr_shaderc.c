/* Translates a shader corpus (SWR_SHADER_CORPUS=dir in a game run) offline and checks the result:
   Vulkan mode compiles every shader to SPIR-V with glslang and, with --validate, checks it with spirv-val.

     swr-shaderc CORPUS_DIR [--out DIR] [--validate] [--gl] [--cache DIR] [--verbose]

   --out writes each shader's GLSL (and .spv in Vulkan mode) to DIR. --cache goes through the runtime's SPIR-V cache in
   DIR, as the renderer does. --gl translates for OpenGL instead and only
   writes GLSL, to compare the translator's GL output between two builds. Exit status 1 when anything failed. */
#include "shader.h"
#include "vk_shader.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_texoff_emul; /* gpu_gl.c's, read by the translator */

static int write_file(const char* path, const void* p, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(p, 1, n, f) == n;
    fclose(f);
    return ok;
}

int main(int argc, char** argv) {
    const char *dir = NULL, *out = NULL, *cache = NULL;
    int validate = 0, gl = 0, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            cache = argv[++i];
        else if (!strcmp(argv[i], "--validate"))
            validate = 1;
        else if (!strcmp(argv[i], "--gl"))
            gl = 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else if (argv[i][0] != '-' && !dir)
            dir = argv[i];
        else {
            fprintf(stderr, "usage: %s CORPUS_DIR [--out DIR] [--validate] [--gl] [--cache DIR] [--verbose]\n", argv[0]);
            return 2;
        }
    }
    if (!dir) {
        fprintf(stderr, "usage: %s CORPUS_DIR [--out DIR] [--validate] [--gl] [--cache DIR] [--verbose]\n", argv[0]);
        return 2;
    }
    g_shader_vulkan = !gl;
    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "cannot open %s\n", dir);
        return 2;
    }
    int total = 0, bad_file = 0, untranslated = 0, compile_fail = 0, invalid = 0, ok = 0;
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
        free(code);
        if (!glsl || si.unsupported) {
            untranslated++;
            if (verbose) printf("%s: not translated (%d unsupported, first %s)\n", base, si.unsupported, si.first_unsupported);
            free(glsl);
            continue;
        }
        if (out) {
            char p[2200];
            snprintf(p, sizeof p, "%s/%s.glsl", out, base);
            write_file(p, glsl, strlen(glsl));
        }
        if (gl) {
            ok++;
            free(glsl);
            continue;
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
        snprintf(spvpath, sizeof spvpath, "%s/%s.spv", out ? out : "/tmp", base);
        if (out || validate) write_file(spvpath, spv, nwords * 4);
        free(spv);
        if (validate) {
            char cmd[2400];
            snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 '%s' 2>&1", spvpath);
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
    return (compile_fail || invalid || bad_file) ? 1 : 0;
}
