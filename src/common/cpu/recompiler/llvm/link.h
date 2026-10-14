/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend's linker: relocatable objects (as LLVM and Clang write them: x86-64 or AArch64 ELF on Unix and
   Android, x86-64 COFF on Windows) into the game module the runtime loads, an ELF shared object or a PE DLL. The
   module defines everything it uses (rtlib.c), so the linker resolves every symbol itself and writes no imports: only
   the relocations position-independent code needs (ELF R_*_RELATIVE, PE base relocations) and an export table */
#ifndef SNX_RECOMPILER_LLVM_LINK_H
#define SNX_RECOMPILER_LLVM_LINK_H
#include <stddef.h>

typedef struct {
    const unsigned char* data;
    size_t size;
    const char* name; /* for diagnostics */
} SnxLinkInput;

typedef struct {
    unsigned char* data; /* malloc'd */
    size_t size;
} SnxLinkOutput;

/* Links by the format of the first input. exports name defined functions the shared library exports. soname is the
   library's own file name (the PE export directory records it). 0, or -1 with the reason in error */
int snx_link(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
             const char* soname, SnxLinkOutput* output, char* error, size_t error_size);
int snx_link_elf(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                 const char* soname, SnxLinkOutput* output, char* error, size_t error_size);
int snx_link_coff(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                  const char* soname, SnxLinkOutput* output, char* error, size_t error_size);

/* shared by both linkers: a growable byte buffer and a name -> value map */
typedef struct {
    unsigned char* data;
    size_t size, capacity;
} SnxLinkBuffer;
void snx_link_reserve(SnxLinkBuffer* buffer, size_t size); /* grows to size bytes, zero-filled */
size_t snx_link_append(SnxLinkBuffer* buffer, const void* data, size_t size);
void snx_link_align(SnxLinkBuffer* buffer, size_t alignment);

typedef struct {
    char** keys;
    size_t* values;
    size_t count, capacity;
} SnxLinkNames;
/* 1 and *value when present */
int snx_link_find(const SnxLinkNames* names, const char* key, size_t* value);
void snx_link_insert(SnxLinkNames* names, const char* key, size_t value);
void snx_link_names_free(SnxLinkNames* names);

#endif
