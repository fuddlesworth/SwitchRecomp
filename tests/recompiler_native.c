/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic NRO -> CLI -> module -> AOT/interpreter execution. No game data.
   A fourth argument names the backend (llvm by default, gcc). */
#include "common/utilities/common.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/loader/module_abi.h"
#include "nx/runtime/driver/recompile.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#ifdef __linux__
#include <elf.h>
#endif

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
static void environment(const char* name, const char* value) {
#ifdef _WIN32
    check(!_putenv_s(name, value ? value : ""), "set test environment");
#else
    check(!(value ? setenv(name, value, 1) : unsetenv(name)), "set test environment");
#endif
}
static void check_empty(const char* dir) {
    char** names = NULL;
    size_t count = 0;
    check(!snx_list_dir(dir, &names, &count) && !count, "temporary build directory cleaned up");
    snx_free_strings(names, count);
}
static const char* backend;
static void check_sources(const char* dir, const char* suffix) {
    char** names = NULL;
    size_t count = 0, i, len;
    check(!snx_list_dir(dir, &names, &count) && count, "generated C retained on request");
    for (i = 0; i < count; ++i) {
        len = strlen(names[i]);
        check(len > strlen(suffix) && !strcmp(names[i] + len - strlen(suffix), suffix),
              "only generated sources retained");
    }
    snx_free_strings(names, count);
}
static void put32(unsigned char* p, size_t off, unsigned long n) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[off + i] = (unsigned char)(n >> (8 * i));
}
static void undefined(CPU* c, u64 pc, u32 word) {
    (void)c;
    (void)pc;
    (void)word;
    check(0, "unexpected UNDEF");
}
static void execute_module(const char* path, const unsigned char* image, int interp) {
    typedef const SnxModule* (*ModuleFn)(void);
    ModuleFn get;
    const SnxModule* mod;
    CPU cpu;
    RtAPI rt;
    size_t i;
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    check(handle != NULL, "load generated DLL");
    if (!handle) return;
    get = (ModuleFn)(void*)GetProcAddress(handle, "snx_module");
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) fprintf(stderr, "%s\n", dlerror());
    check(handle != NULL, "load generated DSO");
    if (!handle) return;
    get = (ModuleFn)dlsym(handle, "snx_module");
#endif
    check(get != NULL, "module export");
    if (!get) goto done;
    mod = get();
    check(mod->abi == SNX_ABI && mod->nmods == 1, "module ABI/metadata");
    check(interp ? mod->nentries == 0 : mod->nentries >= 1, "AOT/interpreter entry tables");
    memset(&cpu, 0, sizeof(cpu));
    memset(&rt, 0, sizeof(rt));
    rt.undef = undefined;
    cpu.rt = &rt;
    cpu.code_lo = mod->code_lo;
    cpu.code_hi = mod->code_hi;
    cpu.disp = calloc((size_t)(mod->code_hi - mod->code_lo) / 4, sizeof(*cpu.disp));
    cpu.mem = calloc(1, (size_t)mod->mods[0].base + 0x1000);
    check(cpu.mem && cpu.disp, "guest fixture allocation");
    if (cpu.mem && cpu.disp) {
        memcpy(cpu.mem + mod->mods[0].base, image, 0x200);
        for (i = 0; i < mod->nentries; ++i) cpu.disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
        if (interp)
            mod->interp(&cpu, mod->mods[0].base);
        else {
            check(cpu.disp[0] != NULL, "entry point discovered");
            if (cpu.disp[0]) cpu.disp[0](&cpu, mod->mods[0].base);
        }
        check(cpu.x[0] == 42 && cpu.pc == 0, "MOV/ADD/RET result");
    }
    free(cpu.mem);
    free(cpu.disp);
done:
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}
#ifdef __linux__
/* The module imports nothing (link.h; gcc: -nostdlib): no library and no undefined symbol, so linking it took none of
   the start files and libraries of the GCC libgccjit was built with, which a packaged libgccjit's system lacks */
static void check_no_imports(const char* path) {
    size_t size = 0, i, j;
    unsigned char* file = snx_read_file(path, &size);
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)file;
    int needed = 0, undefined = 0;
    if (!file || size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf64_Phdr) > size ||
        eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) > size) {
        check(0, "module is a 64-bit ELF");
        free(file);
        return;
    }
    for (i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(file + eh->e_phoff) + i;
        if (ph->p_type != PT_DYNAMIC || ph->p_offset + ph->p_filesz > size) continue;
        for (j = 0; (j + 1) * sizeof(Elf64_Dyn) <= ph->p_filesz; ++j) {
            const Elf64_Dyn* d = (const Elf64_Dyn*)(file + ph->p_offset) + j;
            if (d->d_tag == DT_NULL) break;
            needed |= d->d_tag == DT_NEEDED;
        }
    }
    for (i = 0; i < eh->e_shnum; ++i) {
        const Elf64_Shdr* sh = (const Elf64_Shdr*)(file + eh->e_shoff) + i;
        if (sh->sh_type != SHT_DYNSYM || sh->sh_offset + sh->sh_size > size) continue;
        for (j = 1; (j + 1) * sizeof(Elf64_Sym) <= sh->sh_size; ++j) /* 0 is the null symbol */
            undefined |= ((const Elf64_Sym*)(file + sh->sh_offset))[j].st_shndx == SHN_UNDEF;
    }
    check(!needed, "module needs no library");
    check(!undefined, "module has no undefined symbol");
    free(file);
}
#endif
int main(int argc, char** argv) {
    unsigned char image[0x200];
    const char* args[24];
    char *root, *input, *out, *self, *module, *text = NULL, *data_file;
    char *cached, *source, *nested, *temp, *build;
    size_t cached_size;
    int rc, pass, k;
    if (argc != 4 && argc != 5) return 2;
    backend = argc == 5 ? argv[4] : "llvm";
    root = snx_temp_dir(NULL);
    check(root != NULL, "temp fixture");
    if (!root) return 1;
    input = snx_path_join(root, "program with spaces.nro");
    out = snx_path_join(root, "output with spaces");
    self = snx_executable(argv[0]);
    temp = snx_path_join(root, "temporary files with spaces");
    build = snx_path_join(out, "Native Fixture/Native Fixture_build");
    check(!snx_mkdirs(temp), "temporary build parent");
#ifdef _WIN32
    environment("TMP", temp);
    environment("TEMP", temp);
#else
    environment("TMPDIR", temp);
#endif
    memset(image, 0, sizeof(image));
    put32(image, 0, 0x14000020); /* B +0x80 */
    memcpy(image + 0x10, "NRO0", 4);
    put32(image, 0x18, sizeof(image));
    put32(image, 0x20, 0);
    put32(image, 0x24, 0x100);
    put32(image, 0x28, 0x100);
    put32(image, 0x2c, 0x80);
    put32(image, 0x30, 0x180);
    put32(image, 0x34, 0x80);
    put32(image, 0x80, 0xd28004e0); /* MOV X0,#39 */
    put32(image, 0x84, 0x91000c00); /* ADD X0,X0,#3 */
    put32(image, 0x88, 0xd65f03c0); /* RET */
    check(!snx_write_file(input, image, sizeof(image)), "write NRO");
    for (pass = 0; pass < 6; ++pass) {
        if (pass == 1) {
            data_file = snx_path_join(out, "Native Fixture/Native Fixture_data/romfs.bin");
            check(!snx_write_file(data_file, "old RomFS", 9), "stale NRO assets fixture");
            free(data_file);
        }
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--repo";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = input;
        args[k++] = "Native Fixture";
        args[k++] = "--outdir";
        args[k++] = out;
        args[k++] = "--backend";
        args[k++] = backend;
        args[k++] = "--runtime";
        args[k++] = self;
        args[k++] = "--jobs";
        args[k++] = "2";
        args[k++] = "--chunk";
        args[k++] = "2";
        if (pass == 2) args[k++] = "--interp-only";
        if (pass == 5) args[k++] = "--keep-ir";
        args[k] = NULL;
        rc = snx_run(args, root, NULL, &text);
        if (rc && pass != 4) fprintf(stderr, "%s\n", text);
        if (pass == 4)
            check(rc != 0, "link output replacement failure");
        else
            check(!rc, pass == 2 ? "interpreter CLI build" : "AOT CLI build");
        data_file = snx_path_join(out, "Native Fixture/Native Fixture.abi");
        check(snx_is_file(data_file) == !rc, "ABI checkpoint published only after a successful build");
        if (!rc) { /* what the driver takes for this build's: the ABI and the commit */
            char* stamp = snx_game_stamp();
            cached = (char*)snx_read_file(data_file, &cached_size);
            check(cached && cached_size == strlen(stamp) && !memcmp(cached, stamp, cached_size) &&
                      strchr(stamp, '\n') < stamp + strlen(stamp) - 1,
                  "the ABI checkpoint names the ABI and the build");
            free(cached);
            free(stamp);
        }
        free(data_file);
        if (pass == 1) {
            data_file = snx_path_join(out, "Native Fixture/Native Fixture_data/romfs.bin");
            check(!snx_is_file(data_file), "asset-free NRO removes stale RomFS");
            free(data_file);
        }
        check_empty(temp);
        if (pass == 5)
            check_sources(build, !strcmp(backend, "llvm") ? ".ll" : ".c"); /* gcc: libgccjit's dump */
        else
            check(!snx_is_dir(build), "no persistent build directory by default");
        free(text);
        text = NULL;
#ifdef _WIN32
        module = snx_path_join(out, "Native Fixture/Native Fixture_code.dll");
#else
        module = snx_path_join(out, "Native Fixture/Native Fixture_code.so");
#endif
        if (!rc) execute_module(module, image, pass == 2);
#ifdef __linux__
        if (!rc) check_no_imports(module);
#endif
        /* Force a link publish failure without relying on compiler-specific flags. */
        if (pass == 3) {
            check(!snx_remove_tree(module), "remove previous module");
            check(!snx_mkdirs(module), "block link output with a directory");
        } else if (pass == 4)
            check(!snx_remove_tree(module), "remove link output blocker");
        free(module);
    }
    check(!snx_remove_tree(build), "remove kept sources before no-build test");
    data_file = snx_path_join(out, "Native Fixture/Native Fixture_data/exefs/main.nro");
    cached = (char*)snx_read_file(data_file, &cached_size);
    check(cached && cached_size == sizeof(image) && !memcmp(cached, image, sizeof(image)), "NRO staging");
    free(cached);
    free(data_file);
    /* A previously staged NSO must not silently shadow the new NRO. */
    data_file = snx_path_join(out, "Native Fixture/Native Fixture_data/exefs/main");
    check(!snx_write_file(data_file, "old NSO", 7), "conflicting format fixture");
    k = 0;
    args[k++] = argv[1];
    args[k++] = "--repo";
    args[k++] = argv[2];
    args[k++] = "--recomp";
    args[k++] = input;
    args[k++] = "--out=Native Fixture";
    args[k++] = "--outdir";
    args[k++] = out;
    args[k++] = "--no-build";
    args[k++] = "-O2";
    args[k] = NULL;
    check(snx_run(args, root, NULL, &text) != 0, "reject mixed NSO/NRO cache");
    check_empty(temp);
    free(text);
    text = NULL;
    check(!snx_remove_tree(data_file), "remove conflict fixture");
    free(data_file);
    check(!snx_run(args, root, NULL, &text), "legacy aliases and attached optimization level");
    check(!snx_is_dir(build), "--no-build stages data only, no generated code");
    check_empty(temp);
    free(text);
    text = NULL;
    { /* --keep-c, --keep-ir's old name: still taken, and said to be deprecated */
        const char* old[] = {argv[1], "--repo", argv[2], "--recomp", "/nonexistent/old.nro", "--keep-c", NULL};
        check(snx_run(old, root, NULL, &text) != 0 && strstr(text, "--keep-c is deprecated: use --keep-ir"),
              "--keep-c said to be deprecated");
        free(text);
        text = NULL;
    }
    { /* no name given: the name the NRO's control.nacp gives it, shown as it is recompiled and in its settings' head,
         its control.nacp in its data for the launcher; its files by the file's name, as an NRO has no title ID */
        static unsigned char named[0x200 + 0x40 + 0x300];
        char *nro = snx_path_join(root, "named nro.nro"), *nacp, *ini;
        size_t size;
        memcpy(named, image, sizeof image);
        put32(named, 0x18, sizeof image); /* the NRO itself; its assets after it */
        memcpy(named + 0x200, "ASET", 4);
        put32(named, 0x200 + 0x18, 0x40); /* the NACP's offset in the assets, and its size */
        put32(named, 0x200 + 0x20, 0x300);
        memcpy(named + 0x240, "Named by NACP", 13);
        check(!snx_write_file(nro, named, sizeof named), "NRO with a NACP");
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--repo";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = nro;
        args[k++] = "--outdir";
        args[k++] = out;
        args[k++] = "--no-build";
        args[k] = NULL;
        check(!snx_run(args, root, NULL, &text) && strstr(text, "Recompiling Named by NACP"), "shown by its NACP");
        free(text);
        text = NULL;
        nacp = snx_path_join(out, "named nro/named nro_data/control.nacp");
        cached = (char*)snx_read_file(nacp, &size);
        check(cached && size == 0x300 && !memcmp(cached, named + 0x240, 0x300), "its NACP in its data");
        free(cached);
        ini = snx_path_join(out, "named nro/named nro.ini");
        cached = (char*)snx_read_file(ini, &size);
        check(cached && !strncmp(cached, "; ScarletNX settings for Named by NACP\n", 39), "its settings' head");
        free(cached);
        free(ini);
        free(nacp);
        free(nro);
    }
    k = 0;
    args[k++] = argv[1];
    args[k++] = "--repo";
    args[k++] = argv[2];
    args[k++] = "-r";
    args[k++] = input;
    args[k++] = "--jobs";
    args[k++] = "0";
    args[k] = NULL;
    check(snx_run(args, root, NULL, &text) != 0, "reject zero jobs");
    free(text);
    text = NULL;
#ifndef _WIN32 /* a Windows child without PATH can't load libgccjit's DLLs, which are in the toolchain's folder */
    if (!strcmp(backend, "gcc")) { /* without binutils it says so, where GCC's driver would end the process */
        char* path = getenv("PATH") ? snx_strdup(getenv("PATH")) : NULL;
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--repo";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = input;
        args[k++] = "--outdir";
        args[k++] = out;
        args[k++] = "--backend";
        args[k++] = "gcc";
        args[k] = NULL;
        environment("PATH", "");
        rc = snx_run(args, root, NULL, &text);
        environment("PATH", path);
        check(rc != 0 && text && strstr(text, "PATH has no as and ld: install binutils"), "missing binutils named");
        check_empty(temp);
        free(path);
        free(text);
        text = NULL;
    }
#endif
    source = snx_path_join(root, "copy source");
    nested = snx_path_join(source, "nested/output");
    check(!snx_mkdirs(source), "copy source fixture");
    check(snx_copy_tree(source, nested) != 0 && !snx_is_dir(nested), "reject recursive output inside source");
    free(source);
    free(nested);
    free(input);
    free(out);
    free(self);
    free(temp);
    free(build);
    check(!snx_remove_tree(root), "fixture cleanup");
    free(root);
    printf("Native recompiler: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
