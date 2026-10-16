/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/board/board.h"
#include "3ds/loader/ctr_loader.h"
#include "common/utilities/common.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SNX_SOURCE_DIR
#define SNX_SOURCE_DIR "."
#endif
#ifndef SNX_BUILD_DIR
#define SNX_BUILD_DIR NULL
#endif

static void usage(FILE* out) {
    fputs("Usage: scarletnx-3ds [--model old|new] [-x] [--output DIRECTORY] [--repo DIR] [--] game.cia\n"
          "       scarletnx-3ds [--model old|new] --board-info\n"
          "Stage 2a: decrypted application CIA extraction and memory controller.\n"
          "  -x, --extract   extract only (no guest execution)\n"
          "  --output DIR    extraction directory; matching completed output is reused\n"
          "                  default work/3ds/<title>-<sha256> of the data folder\n"
          "  --repo DIR      the data folder (default: the checkout, a portable copy's folder, else the\n"
          "                  user's data folder, as scarletnx picks it; an installed build's work/ is in\n"
          "                  its cache folder)\n"
          "ARM11 static compilation/execution and encrypted content are not implemented.\n",
          out);
}
static bool cia_path(const char* path) {
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' && tolower((unsigned char)path[n - 3]) == 'c' &&
           tolower((unsigned char)path[n - 2]) == 'i' && tolower((unsigned char)path[n - 1]) == 'a';
}
static bool visible_path(const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}
/* work/3ds of the data folder the Switch driver works in (snx_data_dir), which an installed build keeps in its cache
   folder (snx_work_dir); resolved, as the loader refuses a link on the way to its output */
static char* work_folder(const char* repo, const char* argv0) {
    char *exe = repo ? NULL : snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL;
    char* data = repo ? snx_strdup(repo) : snx_data_dir(SNX_SOURCE_DIR, SNX_BUILD_DIR, dir);
    char *work = data ? snx_work_dir(data) : snx_strdup("work"), *folder = snx_path_join(work, "3ds");
    char* resolved = snx_absolute(folder);
    free(folder);
    free(work);
    free(data);
    free(dir);
    free(exe);
    return resolved;
}
int main(int argc, char** argv) {
    SnxCtrModel model = SNX_CTR_OLD;
    const char *input = NULL, *output = NULL, *repo = NULL;
    bool info = false, positional = false, extract_only = false;
    SnxCtrBoard board;
    snx_tool_console();
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!positional && !strcmp(arg, "--"))
            positional = true;
        else if (!positional && (!strcmp(arg, "--help") || !strcmp(arg, "-h"))) {
            usage(stdout);
            return 0;
        } else if (!positional && !strcmp(arg, "--board-info"))
            info = true;
        else if (!positional && (!strcmp(arg, "-x") || !strcmp(arg, "--extract")))
            extract_only = true;
        else if (!positional && !strcmp(arg, "--output")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --output requires a directory\n", stderr);
                return 2;
            }
            output = argv[i];
        } else if (!positional && !strcmp(arg, "--repo")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --repo requires a directory\n", stderr);
                return 2;
            }
            repo = argv[i];
        } else if (!positional && !strcmp(arg, "--model")) {
            if (++i == argc || (strcmp(argv[i], "old") && strcmp(argv[i], "new"))) {
                fputs("scarletnx-3ds: --model requires old or new\n", stderr);
                return 2;
            }
            model = !strcmp(argv[i], "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
        } else if ((!positional && arg[0] == '-') || input) {
            fprintf(stderr, "scarletnx-3ds: unexpected argument: %s\n", arg);
            usage(stderr);
            return 2;
        } else
            input = arg;
    }
    if ((!input && !info) || (info && (input || extract_only || output || repo))) {
        usage(stderr);
        return 2;
    }
    if (!snx_ctr_board_init(&board, model)) return 1;
    if (info) {
        printf("3DS board: %s\nARM11 cores: %u\nBase clock: %" PRIu64 " Hz\nFCRAM: %u MiB\n", board.profile->name,
               board.profile->arm11_cores, SNX_CTR_BASE_CLOCK_HZ, board.profile->fcram_size / (1024u * 1024u));
        for (size_t i = 0; i < board.profile->region_count; ++i) {
            const SnxCtrRegion* r = &board.profile->regions[i];
            printf("  %-10s physical 0x%08" PRIx32 " size 0x%08" PRIx32 "\n", r->name, r->base, r->size);
        }
        snx_ctr_board_shutdown(&board);
        return 0;
    }
    snx_ctr_board_shutdown(&board);
    /* Do not let a host filename inject progress/launch banners into the GUI. */
    if (!visible_path(input) || (output && !visible_path(output))) {
        fputs("scarletnx-3ds: input/output paths may not contain control characters\n", stderr);
        return 2;
    }
    if (!cia_path(input) || !snx_is_file(input)) {
        fputs("scarletnx-3ds: expected a readable .cia file\n", stderr);
        return 2;
    }
    char error[512], *published = NULL, *work = output ? NULL : work_folder(repo, argv[0]);
    if (!output && !work) {
        fputs("scarletnx-3ds: cannot resolve the work folder\n", stderr);
        return 1;
    }
    printf("Extracting %s\n", input);
    SnxCtrExtractResult result = snx_ctr_extract(input, output, work, &published, error, sizeof error);
    free(work);
    if (result != SNX_CTR_EXTRACT_OK) {
        fprintf(stderr, "scarletnx-3ds: %s\n", error);
        return result == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
    }
    printf("Extracted %s\n", published);
    free(published);
    if (extract_only) return 0;
    /* Never silently substitute NX's A64 compiler or A32's MCJIT runner. */
    fputs("scarletnx-3ds: extraction complete; ARM11 static compilation/execution is not implemented (stage 2b).\n",
          stderr);
    return 3;
}
