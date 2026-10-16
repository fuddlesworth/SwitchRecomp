/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Switch container extraction through hactool. Keys stay in a private temporary HOME. */
#include "extract.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *base, *update, *name, *out, *keys, *title_keys;
} ExtractOptions;

typedef struct TitleKey {
    char *name, *value;
    struct TitleKey* next;
} TitleKey;

typedef struct {
    const char *name, *value;
    size_t name_size, value_size;
} KeyLine;

typedef struct {
    char *base, *update, *out, *keys, *title_keys;
    char *hactool, *temp, *home, *base_dir, *update_dir;
    char *program, *patch, *exefs, *romfs;
    int made_out, base_xci, update_xci, has_keys;
    SnxU64 title_id;
    TitleKey* titles;
} ExtractState;

typedef struct ProgramNca {
    char* path;
    SnxU64 title_id, size;
    int patch;
    struct ProgramNca* next;
} ProgramNca;

static void extract_help(void) {
    puts("Usage: scarletnx -x BASE.xci|BASE.nsp|BASE.dxci|BASE.dnsp [options]\n"
         "  --update FILE       Apply a same-title update during extraction\n"
         "  --name NAME         Output name (default: base dump stem)\n"
         "  --out DIR           Output directory (default: REPO/work/NAME)\n"
         "  --keys FILE         prod.keys (default: REPO, then ~/.switch)\n"
         "  --title-keys FILE   title.keys (same discovery order; optional)\n"
         "  --help              Show this help");
    puts("REPO is the checkout, or for an installed build the user's data folder (scarletnx --help).");
#ifdef SNX_XDG
    puts("An installed build's work/ is in its cache folder, $XDG_CACHE_HOME/scarletnx (~/.cache/scarletnx).");
#endif
    puts("Decrypted content needs no keys. Refuses output containing exefs.");
}

static char* text_copy(const char* text, size_t size) {
    char* copy;
    if (size == (size_t)-1) return NULL;
    copy = (char*)malloc(size + 1);
    if (copy) {
        memcpy(copy, text, size);
        copy[size] = 0;
    }
    return copy;
}

static char* option_arg(const char* option, const char* value) {
    size_t a, b;
    char* text;
    a = strlen(option);
    b = strlen(value);
    if (a > (size_t)-1 - b - 1) return NULL;
    text = (char*)malloc(a + b + 1);
    if (text) {
        memcpy(text, option, a);
        memcpy(text + a, value, b + 1);
    }
    return text;
}

static int option_is(const char* arg, const char* name) {
    size_t size;
    size = strlen(name);
    return !strncmp(arg, name, size) && (!arg[size] || arg[size] == '=');
}

static int parse_options(int argc, char** argv, ExtractOptions* options) {
    int i, positional;
    const char **slot, *value;
    memset(options, 0, sizeof *options);
    positional = 0;
    for (i = 0; i < argc; i++) {
        if (!positional && (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))) {
            extract_help();
            return 2;
        }
        if (!positional && !strcmp(argv[i], "--")) {
            positional = 1;
            continue;
        }
        if (positional || argv[i][0] != '-' || !argv[i][1]) {
            if (options->base) return snx_error("Extraction accepts one base dump");
            options->base = argv[i];
            continue;
        }
        slot = NULL;
        if (option_is(argv[i], "--update"))
            slot = &options->update;
        else if (option_is(argv[i], "--name"))
            slot = &options->name;
        else if (option_is(argv[i], "--out"))
            slot = &options->out;
        else if (option_is(argv[i], "--keys"))
            slot = &options->keys;
        else if (option_is(argv[i], "--title-keys"))
            slot = &options->title_keys;
        if (!slot) return snx_error("Unknown extraction option: %s", argv[i]);
        value = strchr(argv[i], '=');
        if (value)
            value++;
        else {
            if (i + 1 == argc || !strncmp(argv[i + 1], "--", 2))
                return snx_error("Extraction option requires a value: %s", argv[i]);
            value = argv[++i];
        }
        if (!value[0]) return snx_error("Extraction option has an empty value");
        *slot = value;
    }
    if (!options->base) {
        extract_help();
        return 1;
    }
    return 0;
}

static int ascii_name(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int ascii_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int has_suffix(const char* text, const char* suffix) {
    size_t a, b, i;
    a = strlen(text);
    b = strlen(suffix);
    if (a < b) return 0;
    for (i = 0; i < b; i++)
        if (ascii_lower(text[a - b + i]) != ascii_lower(suffix[i])) return 0;
    return 1;
}

static int container_kind(const char* path, int* xci) {
    unsigned char header[0x104];
    size_t size;
    FILE* file;
    int failed;
    file = snx_fopen(path, "rb");
    if (!file) return snx_error("Cannot read container header: %s", path);
    size = fread(header, 1, sizeof header, file);
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) return snx_error("Cannot read container header: %s", path);
    if (size >= 4 && !memcmp(header, "PFS0", 4)) {
        *xci = 0;
        return 0;
    }
    if (size == sizeof header && (!memcmp(header + 0x100, "HEAD", 4) || !memcmp(header + 0x100, "DXCI", 4))) {
        *xci = 1;
        return 0;
    }
    return snx_error("Unsupported container header. Expected NSP/DNSP (PFS0) or XCI/DXCI (HEAD/DXCI)");
}

static int nca_needs_keys(const char* path) {
    unsigned char header[0x204];
    size_t size;
    FILE* file;
    int failed;
    file = snx_fopen(path, "rb");
    if (!file) return -1;
    size = fread(header, 1, sizeof header, file);
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) return -1;
    return size != sizeof header || (memcmp(header + 0x200, "DNCA", 4) && memcmp(header + 0x200, "NCA3", 4) &&
                                     memcmp(header + 0x200, "NCA2", 4));
}

static char* path_parent(const char* path) {
    const char *p, *last;
    last = NULL;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    if (!last) return text_copy(".", 1);
    if (last == path || (last == path + 2 && path[1] == ':')) last++;
    return text_copy(path, (size_t)(last - path));
}

static char* dump_name(const char* path) {
    const char *p, *base, *dot;
    base = path;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    if (!dot || dot == base) dot = base + strlen(base);
    return text_copy(base, (size_t)(dot - base));
}

static char* find_keys(const char* given, const char* name, const SnxPaths* paths) {
    const char* home;
    char *in_repo, *directory, *in_home, *absolute;
    if (given) return snx_absolute(given);
    in_repo = snx_path_join(paths->repo, name);
    if (!in_repo || snx_is_file(in_repo)) return in_repo;
#if defined(_WIN32)
    home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home || !home[0]) return in_repo;
    directory = snx_path_join(home, ".switch");
    if (!directory) {
        free(in_repo);
        return NULL;
    }
    in_home = snx_path_join(directory, name);
    free(directory);
    if (!in_home) {
        free(in_repo);
        return NULL;
    }
    if (!snx_is_file(in_home)) {
        free(in_home);
        return in_repo;
    }
    absolute = snx_absolute(in_home);
    free(in_home);
    free(in_repo);
    return absolute;
}

static int key_line(const char* text, size_t size, KeyLine* line) {
    size_t i, start;
    i = 0;
    while (i < size && isspace((unsigned char)text[i])) i++;
    start = i;
    while (i < size && ascii_name((unsigned char)text[i])) i++;
    if (i == start) return 0;
    line->name = text + start;
    line->name_size = i - start;
    while (i < size && isspace((unsigned char)text[i])) i++;
    if (i == size || text[i++] != '=') return 0;
    while (i < size && isspace((unsigned char)text[i])) i++;
    start = i;
    while (i < size && ascii_hex((unsigned char)text[i])) i++;
    if (i == start) return 0;
    line->value = text + start;
    line->value_size = i - start;
    while (i < size && isspace((unsigned char)text[i])) i++;
    return i == size;
}

static void secret_free(void* memory, size_t size) {
    volatile unsigned char* p;
    p = (volatile unsigned char*)memory;
    if (!p) return;
    while (size) {
        *p++ = 0;
        size--;
    }
    free(memory);
}

static int clean_prod_keys(const char* source, const char* dest) {
    unsigned char* data;
    size_t size, start, end;
    unsigned long dropped;
    FILE* file;
    KeyLine line;
    int failed;
    data = snx_read_file(source, &size);
    if (!data) return snx_error("Cannot read prod.keys");
    file = snx_fopen(dest, "wb");
    if (!file) {
        secret_free(data, size);
        return snx_error("Cannot write private prod.keys");
    }
    start = 0;
    dropped = 0;
    failed = 0;
    while (start < size) {
        end = start;
        while (end < size && data[end] != '\n') end++;
        if (key_line((const char*)data + start, end - start, &line)) {
            if (line.value_size % 32)
                dropped++;
            else if (fwrite(line.name, 1, line.name_size, file) != line.name_size || fputs(" = ", file) == EOF ||
                     fwrite(line.value, 1, line.value_size, file) != line.value_size || fputc('\n', file) == EOF) {
                failed = 1;
                break;
            }
        }
        start = end < size ? end + 1 : end;
    }
    if (fclose(file)) failed = 1;
    secret_free(data, size);
    if (failed) return snx_error("Cannot write private prod.keys");
    if (dropped) printf("Left out %lu malformed key lines\n", dropped);
    return 0;
}

static int title_add(TitleKey** list, const KeyLine* line) {
    TitleKey *item, **last;
    char* name;
    size_t i;
    name = text_copy(line->name, line->name_size);
    if (!name) return snx_error("Out of memory merging title keys");
    for (i = 0; i < line->name_size; i++) name[i] = ascii_lower(name[i]);
    last = list;
    while (*last) {
        if (!strcmp((*last)->name, name)) {
            free(name);
            return 0;
        }
        last = &(*last)->next;
    }
    item = (TitleKey*)calloc(1, sizeof *item);
    if (!item) {
        free(name);
        return snx_error("Out of memory merging title keys");
    }
    item->name = name;
    item->value = text_copy(line->value, line->value_size);
    if (!item->value) {
        free(name);
        free(item);
        return snx_error("Out of memory merging title keys");
    }
    *last = item;
    return 0;
}

static int read_title_keys(const char* source, TitleKey** list) {
    unsigned char* data;
    size_t size, start, end;
    KeyLine line;
    int failed;
    if (!snx_is_file(source)) return 0;
    data = snx_read_file(source, &size);
    if (!data) return snx_error("Cannot read title.keys");
    start = 0;
    failed = 0;
    while (start < size) {
        end = start;
        while (end < size && data[end] != '\n') end++;
        if (key_line((const char*)data + start, end - start, &line) && title_add(list, &line)) {
            failed = 1;
            break;
        }
        start = end < size ? end + 1 : end;
    }
    secret_free(data, size);
    return failed;
}

static int ticket_key(const char* path, TitleKey** list) {
    unsigned char data[1024];
    char name[33], value[33];
    static const char digits[] = "0123456789abcdef";
    SnxU32 signature;
    size_t size, offset, i;
    FILE* file;
    KeyLine line;
    int failed;
    file = snx_fopen(path, "rb");
    if (!file) return snx_error("Cannot read an extracted ticket");
    size = fread(data, 1, sizeof data, file);
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) return snx_error("Cannot read an extracted ticket");
    if (size < 4) return 0;
    signature = (SnxU32)data[0] | ((SnxU32)data[1] << 8) | ((SnxU32)data[2] << 16) | ((SnxU32)data[3] << 24);
    switch (signature) {
        case 0x10000UL:
        case 0x10003UL: offset = 4 + 0x200 + 0x3c; break;
        case 0x10001UL:
        case 0x10004UL: offset = 4 + 0x100 + 0x3c; break;
        case 0x10002UL:
        case 0x10005UL: offset = 4 + 0x3c + 0x40; break;
        case 0x10006UL: offset = 4 + 0x14 + 0x28; break;
        default: return 0;
    }
    if (size < offset + 0x170 || data[offset + 0x141]) return 0;
    for (i = 0; i < 16; i++) {
        name[i * 2] = digits[data[offset + 0x160 + i] >> 4];
        name[i * 2 + 1] = digits[data[offset + 0x160 + i] & 15];
        value[i * 2] = digits[data[offset + 0x40 + i] >> 4];
        value[i * 2 + 1] = digits[data[offset + 0x40 + i] & 15];
    }
    name[32] = value[32] = 0;
    line.name = name;
    line.name_size = 32;
    line.value = value;
    line.value_size = 32;
    return title_add(list, &line);
}

static int read_tickets(const char* folder, TitleKey** list) {
    char **names, *path;
    size_t count, i;
    int failed;
    names = NULL;
    count = 0;
    if (snx_list_dir(folder, &names, &count)) return snx_error("Cannot enumerate extracted tickets");
    failed = 0;
    for (i = 0; i < count; i++) {
        if (!has_suffix(names[i], ".tik")) continue;
        path = snx_path_join(folder, names[i]);
        if (!path) {
            failed = snx_error("Out of memory reading tickets");
            break;
        }
        if (snx_is_file(path)) failed = ticket_key(path, list);
        free(path);
        if (failed) break;
    }
    snx_free_strings(names, count);
    return failed;
}

static int write_title_keys(const char* path, const TitleKey* list) {
    FILE* file;
    int failed;
    file = snx_fopen(path, "wb");
    if (!file) return snx_error("Cannot write private title.keys");
    failed = 0;
    while (list) {
        if (fprintf(file, "%s = %s\n", list->name, list->value) < 0) {
            failed = 1;
            break;
        }
        list = list->next;
    }
    if (fclose(file)) failed = 1;
    return failed ? snx_error("Cannot write private title.keys") : 0;
}

/* Hactool warnings can print secret values. Never forward its captured output, including on failure. */
static int run_hactool(const ExtractState* state, const char* const* args, size_t count, char** information) {
    const char* command[12];
    char* output;
    size_t i;
    int result;
    if (count > 8) return snx_error("Internal hactool argument limit exceeded");
    command[0] = state->hactool;
    command[1] = "--disablekeywarns";
    for (i = 0; i < count; i++) command[i + 2] = args[i];
    command[count + 2] = NULL;
    output = NULL;
    result = snx_run(command, NULL, state->home, &output);
    if (result) {
        if (output) secret_free(output, strlen(output));
        return snx_error(
            "hactool failed; diagnostic output withheld because it can contain keys. Check the dump and keys.");
    }
    if (information)
        *information = output;
    else if (output)
        secret_free(output, strlen(output));
    return 0;
}

static int unpack(const ExtractState* state, const char* container, const char* directory, int xci) {
    const char* command[3];
    char* output_option;
    int result;
    output_option = option_arg(xci ? "--securedir=" : "--outdir=", directory);
    if (!output_option) return snx_error("Out of memory unpacking container");
    command[0] = xci ? "-txci" : "-tpfs0";
    command[1] = output_option;
    command[2] = container;
    printf("Unpacking %s\n", container);
    fflush(stdout);
    result = run_hactool(state, command, 3, NULL);
    free(output_option);
    if (!result && !snx_is_dir(directory))
        result = snx_error("hactool did not create the extracted container directory");
    return result;
}

static int program_type(const char* information) {
    const char* p;
    if (!information) return 0;
    p = strstr(information, "Content Type:");
    if (!p) return 0;
    p += strlen("Content Type:");
    while (*p && isspace((unsigned char)*p)) p++;
    return !strncmp(p, "Program", 7) && !ascii_name((unsigned char)p[7]);
}

static int program_title_id(const char* information, SnxU64* title_id) {
    const char* p;
    size_t i;
    unsigned digit;
    SnxU64 value;
    p = information ? strstr(information, "Title ID:") : NULL;
    if (!p) return 0;
    p += strlen("Title ID:");
    while (*p && isspace((unsigned char)*p)) p++;
    value = 0;
    for (i = 0; i < 16; i++) {
        if (!ascii_hex((unsigned char)p[i])) return 0;
        digit = p[i] <= '9' ? (unsigned)(p[i] - '0') : (unsigned)(ascii_lower(p[i]) - 'a' + 10);
        value = (value << 4) | digit;
    }
    if (ascii_hex((unsigned char)p[16])) return 0;
    *title_id = value;
    return 1;
}

static int program_nca(const ExtractState* state, const char* folder, int update, SnxU64 expected_title,
                       char** selected, char** embedded_patch, SnxU64* selected_title) {
    char **names, *path, *information;
    const char* args[2];
    size_t count, i;
    SnxU64 title_id;
    ProgramNca *programs, *item, *next, *base, *patch;
    int failed, needs_keys;
    names = NULL;
    count = 0;
    programs = base = patch = NULL;
    *selected = NULL;
    if (embedded_patch) *embedded_patch = NULL;
    if (snx_list_dir(folder, &names, &count)) return snx_error("Cannot enumerate extracted NCAs");
    failed = 0;
    for (i = 0; i < count; i++) {
        if (!has_suffix(names[i], ".nca") || has_suffix(names[i], ".cnmt.nca")) continue;
        path = snx_path_join(folder, names[i]);
        if (!path) {
            failed = snx_error("Out of memory selecting Program NCA");
            break;
        }
        if (!snx_is_file(path)) {
            free(path);
            continue;
        }
        needs_keys = nca_needs_keys(path);
        if (needs_keys < 0)
            failed = snx_error("Cannot read an extracted NCA header");
        else if (needs_keys && !state->has_keys)
            failed = snx_error("Encrypted content needs prod.keys. Provide --keys, or put it at %s or in ~/.switch",
                               state->keys);
        if (failed) {
            free(path);
            break;
        }
        args[0] = "-i";
        args[1] = path;
        information = NULL;
        failed = run_hactool(state, args, 2, &information);
        if (!failed && program_type(information)) {
            if (!program_title_id(information, &title_id))
                failed = snx_error("hactool did not report a valid Program NCA title ID");
            else {
                item = (ProgramNca*)calloc(1, sizeof *item);
                if (!item)
                    failed = snx_error("Out of memory selecting Program NCA");
                else {
                    item->path = path;
                    path = NULL;
                    item->title_id = title_id;
                    item->size = snx_file_size(item->path);
                    item->patch = (title_id & 0x800UL) != 0 || strstr(information, "Patch RomFS") != NULL;
                    item->next = programs;
                    programs = item;
                }
            }
        }
        if (information) secret_free(information, strlen(information));
        free(path);
        if (failed) break;
    }
    snx_free_strings(names, count);
    if (!failed) {
        for (item = programs; item; item = item->next) {
            if (update) {
                if (item->patch && (item->title_id & ~(SnxU64)0x800UL) == expected_title &&
                    (!base || item->size > base->size))
                    base = item;
            } else if (!item->patch && (!base || item->size > base->size))
                base = item;
        }
        if (!base)
            failed =
                snx_error(update ? "Update has no Program NCA matching the selected base game"
                                 : "Container has no base Program NCA (possibly an update or DLC). Supply a base game");
        else {
            if (embedded_patch) {
                for (item = programs; item; item = item->next)
                    if (item->patch && (item->title_id & ~(SnxU64)0x800UL) == base->title_id &&
                        (!patch || item->size > patch->size))
                        patch = item;
                if (patch) {
                    *embedded_patch = patch->path;
                    patch->path = NULL;
                }
            }
            *selected = base->path;
            base->path = NULL;
            if (selected_title) *selected_title = base->title_id;
        }
    }
    for (item = programs; item; item = next) {
        next = item->next;
        free(item->path);
        free(item);
    }
    return failed;
}

static char* locate_hactool(const SnxPaths* paths) {
    const char *tool_name, *command[9];
    char *path, *parent, *cmake, *output, *extract_build;
    int result;
#if defined(_WIN32)
    tool_name = "hactool.exe";
#elif defined(__ANDROID__)
    tool_name = "libhactool.so"; /* the app's libraries folder takes no other name (cmake/hactool.cmake) */
#else
    tool_name = "hactool";
#endif
    path = snx_path_join(paths->build, tool_name);
    if (!path || snx_is_file(path)) return path;
    free(path);
    parent = path_parent(paths->executable);
    if (!parent) return NULL;
    path = snx_path_join(parent, tool_name);
    free(parent);
    if (!path || snx_is_file(path)) return path;
    free(path);
    extract_build = snx_path_join(paths->repo, "build/extract");
    if (!extract_build) return NULL;
    path = snx_path_join(extract_build, tool_name);
    if (!path || snx_is_file(path)) {
        free(extract_build);
        return path;
    }
    free(path);
    cmake = snx_find_tool("cmake");
    if (!cmake) {
        free(extract_build);
        snx_error("hactool is missing, and CMake is unavailable to build it");
        return NULL;
    }
    puts("Building hactool");
    fflush(stdout);
    command[0] = cmake;
    command[1] = "--preset";
    command[2] = "default";
    command[3] = "-B";
    command[4] = extract_build;
    command[5] = "-DSNX_HACTOOL_ONLY=ON";
    command[6] = NULL;
    output = NULL;
    result = snx_run(command, paths->repo, NULL, &output);
    free(output);
    if (!result) {
        command[1] = "--build";
        command[2] = extract_build;
        command[3] = "--target";
        command[4] = "hactool";
        command[5] = NULL;
        output = NULL;
        result = snx_run(command, paths->repo, NULL, &output);
        free(output);
    }
    free(cmake);
    path = snx_path_join(extract_build, tool_name);
    free(extract_build);
    if (result || !path || !snx_is_file(path)) {
        free(path);
        snx_error("CMake could not build hactool");
        return NULL;
    }
    return path;
}

static int prepare_paths(ExtractState* state, const ExtractOptions* options, const SnxPaths* paths) {
    char *name, *work, *target, *parent;
    SnxU64 base_size, update_size, needed, maximum;
    int made_out;
    state->base = snx_absolute(options->base);
    if (options->update) state->update = snx_absolute(options->update);
    state->keys = find_keys(options->keys, "prod.keys", paths);
    state->title_keys = find_keys(options->title_keys, "title.keys", paths);
    if (!state->base || (options->update && !state->update) || !state->keys || !state->title_keys)
        return snx_error("Cannot resolve extraction input paths");
    if (!snx_is_file(state->base) || (state->update && !snx_is_file(state->update)))
        return snx_error("Base or update dump is missing or is not a file");
    if (container_kind(state->base, &state->base_xci) ||
        (state->update && container_kind(state->update, &state->update_xci)))
        return 1;
    state->has_keys = snx_is_file(state->keys);
    if (options->keys && !state->has_keys) return snx_error("Specified prod.keys file is missing");
    name = options->name ? text_copy(options->name, strlen(options->name)) : dump_name(state->base);
    if (!name) return snx_error("Out of memory resolving extraction output");
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strpbrk(name, "/\\:")) {
        free(name);
        return snx_error("Extraction name must be a single nonempty filename");
    }
    /* only when --out names none: play_dump names one, and has said where an older build's extractions are */
    work = options->out ? NULL : snx_work_dir(paths->repo);
    target = work ? snx_path_join(work, name) : NULL;
    free(work);
    free(name);
    if (!options->out && !target) return snx_error("Out of memory resolving extraction output");
    state->out = snx_absolute(options->out ? options->out : target);
    free(target);
    if (!state->out) return snx_error("Cannot resolve extraction output");
    state->exefs = snx_path_join(state->out, "exefs");
    state->romfs = snx_path_join(state->out, "romfs");
    if (!state->exefs || !state->romfs) return snx_error("Out of memory resolving extraction output");
    if (snx_is_file(state->exefs) || snx_is_dir(state->exefs) || snx_is_link(state->exefs))
        return snx_error("Output already contains exefs: %s", state->out);
    if (snx_is_file(state->out)) return snx_error("Extraction output is a file");
    base_size = update_size = 0;
    base_size = snx_file_size(state->base);
    if (state->update) update_size = snx_file_size(state->update);
    maximum = (SnxU64)-1;
    needed = base_size > maximum - update_size ? maximum : base_size + update_size;
    needed = needed > maximum - needed / 2 ? maximum : needed + needed / 2;
    made_out = !snx_is_dir(state->out);
    if (snx_mkdirs(state->out)) return snx_error("Cannot create extraction output");
    state->made_out = made_out;
    state->temp = snx_temp_dir(NULL);
    if (state->temp && snx_free_space(state->temp) < needed) {
        if (snx_remove_tree(state->temp)) return snx_error("Cannot remove the first extraction temp directory");
        free(state->temp);
        state->temp = NULL;
    }
    if (!state->temp) {
        parent = path_parent(state->out);
        if (!parent) return snx_error("Out of memory selecting extraction temp directory");
        state->temp = snx_temp_dir(parent);
        free(parent);
    }
    if (!state->temp) return snx_error("Cannot create extraction temp directory");
    state->home = snx_path_join(state->temp, "home");
    state->base_dir = snx_path_join(state->temp, "base");
    if (state->update) state->update_dir = snx_path_join(state->temp, "update");
    if (!state->home || !state->base_dir || (state->update && !state->update_dir))
        return snx_error("Out of memory preparing extraction temp directory");
    return 0;
}

static int extract_files(ExtractState* state) {
    const char* command[4];
    char *switch_dir, *prod_path, *title_path, *exefs_arg, *romfs_arg, *base_arg, *main_path;
    int result;
    switch_dir = snx_path_join(state->home, ".switch");
    prod_path = switch_dir ? snx_path_join(switch_dir, "prod.keys") : NULL;
    title_path = switch_dir ? snx_path_join(switch_dir, "title.keys") : NULL;
    exefs_arg = option_arg("--exefsdir=", state->exefs);
    romfs_arg = option_arg("--romfsdir=", state->romfs);
    main_path = snx_path_join(state->exefs, "main");
    base_arg = NULL;
    result = 1;
    if (!switch_dir || !prod_path || !title_path || !exefs_arg || !romfs_arg || !main_path) {
        snx_error("Out of memory preparing extraction");
        goto done;
    }
    if (snx_mkdirs(switch_dir)) {
        snx_error("Cannot create private key directory");
        goto done;
    }
    if (state->has_keys) {
        if (clean_prod_keys(state->keys, prod_path)) goto done;
    } else if (snx_write_file(prod_path, "", 0)) {
        snx_error("Cannot write private prod.keys");
        goto done;
    }
    if (unpack(state, state->base, state->base_dir, state->base_xci)) goto done;
    if (state->update && unpack(state, state->update, state->update_dir, state->update_xci)) goto done;
    if (read_title_keys(state->title_keys, &state->titles) || read_tickets(state->base_dir, &state->titles)) goto done;
    if (state->update_dir && read_tickets(state->update_dir, &state->titles)) goto done;
    if (write_title_keys(title_path, state->titles) ||
        program_nca(state, state->base_dir, 0, 0, &state->program, state->update_dir ? NULL : &state->patch,
                    &state->title_id))
        goto done;
    if (state->update_dir) {
        if (program_nca(state, state->update_dir, 1, state->title_id, &state->patch, NULL, NULL)) goto done;
    }
    if (state->patch) {
        base_arg = option_arg("--basenca=", state->program);
        if (!base_arg) {
            snx_error("Out of memory preparing update extraction");
            goto done;
        }
        command[0] = base_arg;
        command[1] = exefs_arg;
        command[2] = romfs_arg;
        command[3] = state->patch;
        puts("Extracting the base with the update applied");
        fflush(stdout);
        if (run_hactool(state, command, 4, NULL)) goto done;
    } else {
        command[0] = exefs_arg;
        command[1] = romfs_arg;
        command[2] = state->program;
        puts("Extracting the base game");
        fflush(stdout);
        if (run_hactool(state, command, 3, NULL)) goto done;
    }
    if (!snx_is_file(main_path)) {
        snx_error("Extracted exefs has no main; verify that the update matches the base game");
        goto done;
    }
    result = 0;
done:
    free(switch_dir);
    free(prod_path);
    free(title_path);
    free(exefs_arg);
    free(romfs_arg);
    free(base_arg);
    free(main_path);
    return result;
}

static int cleanup(ExtractState* state, int result) {
    TitleKey *item, *next;
    int incomplete;
    incomplete = result;
    if (state->temp && snx_remove_tree(state->temp))
        result = snx_error("Could not remove extraction temp directory: %s", state->temp);
    if (incomplete && state->made_out && state->out && snx_remove_tree(state->out))
        snx_error("Could not remove incomplete extraction output: %s", state->out);
    item = state->titles;
    while (item) {
        next = item->next;
        secret_free(item->name, strlen(item->name));
        secret_free(item->value, strlen(item->value));
        free(item);
        item = next;
    }
    if (!result) printf("Done: %s\n", state->out);
    free(state->base);
    free(state->update);
    free(state->out);
    free(state->keys);
    free(state->title_keys);
    free(state->hactool);
    free(state->temp);
    free(state->home);
    free(state->base_dir);
    free(state->update_dir);
    free(state->program);
    free(state->patch);
    free(state->exefs);
    free(state->romfs);
    return result;
}

int snx_extract(int argc, char** argv, const SnxPaths* paths) {
    ExtractOptions options;
    ExtractState state;
    int result;
    result = parse_options(argc, argv, &options);
    if (result) return result == 2 ? 0 : 1;
    if (!paths || !paths->repo || !paths->build || !paths->executable)
        return snx_error("Extraction requires repository, build and executable paths");
    memset(&state, 0, sizeof state);
    result = prepare_paths(&state, &options, paths);
    if (!result) {
        state.hactool = locate_hactool(paths);
        if (!state.hactool) result = 1;
    }
    if (!result) result = extract_files(&state);
    return cleanup(&state, result);
}
