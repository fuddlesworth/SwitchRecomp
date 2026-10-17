/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* rEden's A64 cases (a64_ported_cases.h) on the AOT step and the interpreter. Arguments: C compiler, source tree */
#include "common/cpu/aarch64/a64.h"
#include "a64_ported_cases.h"
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void emit(FILE* out) {
    const size_t ops = sizeof(snx_a64p_script) / sizeof(snx_a64p_script[0]);
    const size_t words = sizeof(snx_a64p_minmax_words) / sizeof(snx_a64p_minmax_words[0]);
    char name[48];
    size_t i, address = 0;
    fputs("#include \"cpu.h\"\n#include \"cpu_helpers.c\"\n#include \"a64_audit_state.h\"\nGuestFn snx_interp_entry;\n",
          out);
    /* each case's code from address 0: a branch's static step resolves its target from its own address */
    for (i = 0; i < ops; ++i) {
        if (snx_a64p_script[i].op == SNX_A64P_OP_CASE) address = 0;
        if (snx_a64p_script[i].op != SNX_A64P_OP_CODE) continue;
        sprintf(name, "snx_a64p_s%lu", (unsigned long)i);
        snx_a64_static_step(out, (SnxU32)snx_a64p_script[i].a, address, name);
        address += 4;
    }
    for (i = 0; i < words; ++i) {
        sprintf(name, "snx_a64p_m%lu", (unsigned long)i);
        snx_a64_static_step(out, snx_a64p_minmax_words[i].word, 0, name);
    }
    snx_a64_interp(out, 1);
    fputs("typedef u64 (*SnxA64pStep)(CPU*);\nstatic const SnxA64pStep snx_a64p_aot[] = {", out);
    for (i = 0; i < ops; ++i) {
        if (snx_a64p_script[i].op == SNX_A64P_OP_CODE)
            fprintf(out, "%ssnx_a64p_s%lu", i ? "," : "", (unsigned long)i);
        else
            fputs(i ? ",0" : "0", out);
    }
    fputs("};\nstatic const SnxA64pStep snx_a64p_minmax_aot[] = {", out);
    for (i = 0; i < words; ++i) fprintf(out, "%ssnx_a64p_m%lu", i ? "," : "", (unsigned long)i);
    /* the words these steps were emitted for: the runner reads the cases from the source tree, which may be newer */
    fputs("};\nstatic const u32 snx_a64p_emitted[] = {", out);
    for (i = 0; i < ops; ++i)
        fprintf(out, "%s0x%08lXu", i ? "," : "",
                snx_a64p_script[i].op == SNX_A64P_OP_CODE ? (unsigned long)snx_a64p_script[i].a : 0ul);
    fputs("};\nstatic const u32 snx_a64p_emitted_minmax[] = {", out);
    for (i = 0; i < words; ++i) fprintf(out, "%s0x%08lXu", i ? "," : "", (unsigned long)snx_a64p_minmax_words[i].word);
    fputs("};\n#include \"a64_ported_run.inc\"\n", out);
}

int main(int argc, char** argv) {
    const char* args[20];
    char *directory, *source, *binary, *cpu, *tests, *root, *output = NULL;
    FILE* file;
    int n = 0, status;
    if (argc != 3) {
        fputs("usage: snx-test-a64-ported CC SOURCE_DIR\n", stderr);
        return 2;
    }
    directory = snx_temp_dir(NULL);
    check(directory != NULL, "create the test directory");
    if (!directory) return 1;
    source = snx_path_join(directory, "a64_ported.c");
#ifdef _WIN32
    binary = snx_path_join(directory, "a64_ported.exe");
#else
    binary = snx_path_join(directory, "a64_ported");
#endif
    cpu = snx_path_join(argv[2], "src/common/cpu/aarch64");
    tests = snx_path_join(argv[2], "tests");
    root = snx_path_join(argv[2], "src"); /* what cpu.h includes */
    file = snx_fopen(source, "wb");
    check(file != NULL, "open the generated source");
    if (file) {
        emit(file);
        check(!fclose(file), "write the generated source");
        /* the code generation flags toolchain.c gives game modules */
        args[n++] = argv[1];
        args[n++] = "-O2";
        args[n++] = "-fno-strict-aliasing";
        args[n++] = "-fwrapv";
#if defined(__x86_64__) || defined(_M_X64)
        args[n++] = "-mcx16";
#endif
        args[n++] = "-Wno-unused-label";
        args[n++] = "-I";
        args[n++] = cpu;
        args[n++] = "-I";
        args[n++] = tests;
        args[n++] = "-I";
        args[n++] = root;
        args[n++] = source;
#ifndef _WIN32
        args[n++] = "-lm";
#endif
        args[n++] = "-o";
        args[n++] = binary;
        args[n] = NULL;
        status = snx_run(args, directory, NULL, &output);
        if (status && output) fputs(output, stderr);
        check(!status, "compile the emitted steps with the runner");
        free(output);
        output = NULL;
        if (!status) {
            args[0] = binary;
            args[1] = NULL;
            status = snx_run(args, directory, NULL, &output);
            if (output) fputs(output, status ? stderr : stdout);
            check(!status, "every ported case on both paths");
        }
    }
    free(output);
    check(!snx_remove_tree(directory), "remove the test directory");
    free(directory);
    free(source);
    free(binary);
    free(cpu);
    free(tests);
    free(root);
    return failures != 0;
}
