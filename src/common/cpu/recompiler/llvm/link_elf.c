/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF: x86-64 or AArch64 ET_REL objects into an ET_DYN shared object for Linux, FreeBSD and Android.
   Layout: [R: headers, .hash .dynsym .dynstr .rela.dyn, read-only data] [RX: .text] [RW: .data .got .dynamic, .bss].
   Every symbol must be defined by one of the objects; the only dynamic relocations are R_*_RELATIVE, for absolute
   addresses in writable data and for GOT slots. The two machines' relocation numbers don't overlap (AArch64's start
   at 257), so one switch takes both. */
#include "link.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
int snx_link_elf(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                 const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    (void)inputs;
    (void)count;
    (void)exports;
    (void)export_count;
    (void)soname;
    (void)output;
    snprintf(error, error_size, "ELF output is not supported on Windows hosts");
    return -1;
}
#else
#include <elf.h>

/* names newer than some systems' <elf.h> (FreeBSD's calls the unwind section SHT_AMD64_UNWIND) */
#ifndef SHT_X86_64_UNWIND
#define SHT_X86_64_UNWIND 0x70000001
#endif
#ifndef R_X86_64_GOTPCRELX
#define R_X86_64_GOTPCRELX 41
#endif
#ifndef R_X86_64_REX_GOTPCRELX
#define R_X86_64_REX_GOTPCRELX 42
#endif
#ifndef R_X86_64_PLT32
#define R_X86_64_PLT32 4
#endif
#ifndef DT_RELACOUNT
#define DT_RELACOUNT 0x6ffffff9
#endif
#ifndef PT_GNU_STACK
#define PT_GNU_STACK 0x6474e551
#endif
#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
/* AArch64's, by number: not every <elf.h> has them */
enum {
    A64_ABS64 = 257,
    A64_PREL64 = 260,
    A64_PREL32 = 261,
    A64_ADR_PREL_LO21 = 274,
    A64_ADR_PREL_PG_HI21 = 275,
    A64_ADR_PREL_PG_HI21_NC = 276,
    A64_ADD_ABS_LO12_NC = 277,
    A64_LDST8_ABS_LO12_NC = 278,
    A64_TSTBR14 = 279,
    A64_CONDBR19 = 280,
    A64_JUMP26 = 282,
    A64_CALL26 = 283,
    A64_LDST16_ABS_LO12_NC = 284,
    A64_LDST32_ABS_LO12_NC = 285,
    A64_LDST64_ABS_LO12_NC = 286,
    A64_LDST128_ABS_LO12_NC = 299,
    A64_ADR_GOT_PAGE = 311,
    A64_LD64_GOT_LO12_NC = 312,
    A64_RELATIVE = 1027
};
/* a branch out of B's reach (128 MiB) goes through a veneer, ADRP x16 + ADD x16 + BR x16: the scratch register the
   procedure call standard gives them. Below this much code every branch reaches, and no object gets veneers */
#define VENEER      12
#define VENEER_TEXT (120ull << 20)

enum { K_RODATA, K_TEXT, K_DATA, K_BSS, K_COUNT };

typedef struct {
    const SnxLinkInput* input;
    const Elf64_Ehdr* eh;
    const Elf64_Shdr* sh;
    const Elf64_Sym* syms;
    size_t nsyms;
    const char* strtab;
    int* kind;          /* per section: output kind, -1 when not loaded */
    uint64_t* address;  /* per section: final virtual address */
    uint64_t* position; /* per section: offset inside its kind */
    size_t veneers;     /* its veneers, after its code at island (an offset in the code) */
    uint64_t island;
} Obj;

typedef struct {
    Obj* objs;
    size_t nobjs;
    SnxLinkNames globals;       /* name -> (object << 32 | symbol index) */
    SnxLinkNames got;           /* "object:symbol" of a symbol's definition -> GOT slot */
    SnxLinkNames veneer;        /* "caller:object:symbol" -> the caller's veneer to that definition */
    unsigned machine, relative; /* EM_X86_64 or EM_AARCH64, and its R_*_RELATIVE */
    uint64_t page;              /* segment alignment: AArch64 kernels may use 16 or 64 KiB pages */
    uint64_t kind_size[K_COUNT], kind_align[K_COUNT], kind_address[K_COUNT];
    uint64_t got_address;
    size_t got_count, relative_count;
    char* error;
    size_t error_size;
} Link;

static int fail(Link* l, const char* format, const char* a, const char* b) {
    snprintf(l->error, l->error_size, format, a, b);
    return -1;
}

static const char* sym_name(const Obj* o, const Elf64_Sym* s) {
    if (ELF64_ST_TYPE(s->st_info) == STT_SECTION) return "<section>";
    return o->strtab + s->st_name;
}

static int parse(Link* l, Obj* o, const SnxLinkInput* input) {
    size_t i;
    o->input = input;
    o->eh = (const Elf64_Ehdr*)input->data;
    if (input->size < sizeof(Elf64_Ehdr) || memcmp(o->eh->e_ident, ELFMAG, SELFMAG) ||
        o->eh->e_ident[EI_CLASS] != ELFCLASS64 || o->eh->e_ident[EI_DATA] != ELFDATA2LSB || o->eh->e_type != ET_REL ||
        (o->eh->e_machine != EM_X86_64 && o->eh->e_machine != EM_AARCH64) || o->eh->e_shentsize != sizeof(Elf64_Shdr) ||
        o->eh->e_shoff + (uint64_t)o->eh->e_shnum * sizeof(Elf64_Shdr) > input->size)
        return fail(l, "%s: not an x86-64 or AArch64 ELF relocatable object%s", input->name, "");
    if (!l->machine) l->machine = o->eh->e_machine;
    if (o->eh->e_machine != l->machine) return fail(l, "%s: another machine's object%s", input->name, "");
    o->sh = (const Elf64_Shdr*)(input->data + o->eh->e_shoff);
    /* one block, the 8-byte arrays first */
    o->address = (uint64_t*)calloc(o->eh->e_shnum, 2 * sizeof(uint64_t) + sizeof(int));
    if (!o->address) return fail(l, "out of memory%s%s", "", "");
    o->position = o->address + o->eh->e_shnum;
    o->kind = (int*)(o->position + o->eh->e_shnum);
    for (i = 0; i < o->eh->e_shnum; ++i) {
        const Elf64_Shdr* s = &o->sh[i];
        o->kind[i] = -1;
        if (s->sh_type != SHT_NOBITS && s->sh_offset + s->sh_size > input->size)
            return fail(l, "%s: truncated section%s", input->name, "");
        if (s->sh_type == SHT_SYMTAB) {
            o->syms = (const Elf64_Sym*)(input->data + s->sh_offset);
            o->nsyms = s->sh_size / sizeof(Elf64_Sym);
            if (s->sh_link >= o->eh->e_shnum) return fail(l, "%s: bad symbol table%s", input->name, "");
            o->strtab = (const char*)input->data + o->sh[s->sh_link].sh_offset;
        }
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_flags & SHF_TLS) return fail(l, "%s: thread-local data is not supported%s", input->name, "");
        if (s->sh_type == SHT_INIT_ARRAY || s->sh_type == SHT_FINI_ARRAY || s->sh_type == SHT_PREINIT_ARRAY)
            return fail(l, "%s: constructors are not supported%s", input->name, "");
        if (s->sh_type == SHT_NOBITS)
            o->kind[i] = K_BSS;
        else if (s->sh_type == SHT_PROGBITS || s->sh_type == SHT_X86_64_UNWIND)
            o->kind[i] = s->sh_flags & SHF_EXECINSTR ? K_TEXT : s->sh_flags & SHF_WRITE ? K_DATA : K_RODATA;
        else if (s->sh_type == SHT_NOTE || s->sh_type == SHT_GROUP)
            continue;
        else
            return fail(l, "%s: unsupported allocated section type%s", input->name, "");
    }
    if (!o->syms) return fail(l, "%s: no symbol table%s", input->name, "");
    return 0;
}

static int collect_globals(Link* l) {
    size_t i, j, found;
    for (i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
        for (j = 1; j < o->nsyms; ++j) {
            const Elf64_Sym* s = &o->syms[j];
            int bind = ELF64_ST_BIND(s->st_info);
            if ((bind != STB_GLOBAL && bind != STB_WEAK) || s->st_shndx == SHN_UNDEF) continue;
            if (s->st_shndx == SHN_COMMON) return fail(l, "%s: common symbol %s", o->input->name, sym_name(o, s));
            if (snx_link_find(&l->globals, sym_name(o, s), &found)) {
                const Obj* other = &l->objs[found >> 32];
                if (bind == STB_WEAK) continue;
                if (ELF64_ST_BIND(other->syms[found & 0xFFFFFFFFu].st_info) != STB_WEAK)
                    return fail(l, "duplicate symbol %s (%s)", sym_name(o, s), o->input->name);
            }
            snx_link_insert(&l->globals, sym_name(o, s), (i << 32) | j);
        }
    }
    return 0;
}

/* The defining object and symbol of object o's symbol index; *undefined_weak for an unresolved weak reference */
static int definition(Link* l, size_t object, size_t index, size_t* def_object, size_t* def_index,
                      int* undefined_weak) {
    const Obj* o = &l->objs[object];
    const Elf64_Sym* s;
    size_t found;
    *undefined_weak = 0;
    if (index >= o->nsyms) return fail(l, "%s: bad symbol index%s", o->input->name, "");
    s = &o->syms[index];
    if (s->st_shndx != SHN_UNDEF) {
        *def_object = object;
        *def_index = index;
        return 0;
    }
    if (snx_link_find(&l->globals, sym_name(o, s), &found)) {
        *def_object = found >> 32;
        *def_index = found & 0xFFFFFFFFu;
        return 0;
    }
    if (ELF64_ST_BIND(s->st_info) == STB_WEAK) {
        *undefined_weak = 1;
        return 0;
    }
    if (!strcmp(sym_name(o, s), "_GLOBAL_OFFSET_TABLE_")) {
        *def_object = (size_t)-1;
        return 0;
    }
    return fail(l, "undefined symbol %s (referenced by %s)", sym_name(o, s), o->input->name);
}

/* final address of a definition; 0 for an absolute symbol's value */
static uint64_t symbol_address(Link* l, size_t object, size_t index, int* absolute) {
    const Obj* o;
    const Elf64_Sym* s;
    *absolute = 0;
    if (object == (size_t)-1) return l->got_address;
    o = &l->objs[object];
    s = &o->syms[index];
    if (s->st_shndx == SHN_ABS) {
        *absolute = 1;
        return s->st_value;
    }
    return o->address[s->st_shndx] + s->st_value;
}

static int got_reloc(unsigned type) {
    return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX ||
           type == A64_ADR_GOT_PAGE || type == A64_LD64_GOT_LO12_NC;
}

static void got_key(char* key, size_t object, size_t index) {
    sprintf(key, "%lu:%lu", (unsigned long)object, (unsigned long)index);
}
static void veneer_key(char* key, size_t caller, size_t object, size_t index) {
    sprintf(key, "%lu:%lu:%lu", (unsigned long)caller, (unsigned long)object, (unsigned long)index);
}

/* text before veneers, which only a module this large needs */
static uint64_t text_size(const Link* l) {
    uint64_t size = 0;
    size_t i, j;
    for (i = 0; i < l->nobjs; ++i)
        for (j = 0; j < l->objs[i].eh->e_shnum; ++j)
            if (l->objs[i].kind[j] == K_TEXT) size += l->objs[i].sh[j].sh_size + l->objs[i].sh[j].sh_addralign;
    return size;
}

/* first pass over relocations: GOT slots, the R_*_RELATIVE count and the veneers each object may need */
static int scan_relocations(Link* l) {
    size_t i, j, k, def_object, def_index, slot;
    int weak, far = text_size(l) >= VENEER_TEXT;
    char key[72];
    for (i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = &o->sh[j];
            const Elf64_Rela* r;
            size_t n;
            if (s->sh_type == SHT_REL) return fail(l, "%s: REL relocations are not supported%s", o->input->name, "");
            if (s->sh_type != SHT_RELA || s->sh_info >= o->eh->e_shnum || o->kind[s->sh_info] < 0) continue;
            r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            n = s->sh_size / sizeof(Elf64_Rela);
            for (k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                if (definition(l, i, ELF64_R_SYM(r[k].r_info), &def_object, &def_index, &weak)) return -1;
                if (got_reloc(type)) {
                    if (weak) return fail(l, "%s: GOT reference to an undefined weak symbol%s", o->input->name, "");
                    got_key(key, def_object, def_index);
                    if (!snx_link_find(&l->got, key, &slot)) {
                        snx_link_insert(&l->got, key, l->got_count++);
                        ++l->relative_count;
                    }
                } else if ((type == A64_CALL26 || type == A64_JUMP26) && far && !weak && def_object != i &&
                           def_object != (size_t)-1) {
                    veneer_key(key, i, def_object, def_index);
                    if (!snx_link_find(&l->veneer, key, &slot)) snx_link_insert(&l->veneer, key, o->veneers++);
                } else if ((type == R_X86_64_64 || type == A64_ABS64) && !weak) {
                    if (o->kind[s->sh_info] != K_DATA)
                        return fail(l, "%s: absolute address in read-only section %s", o->input->name,
                                    (const char*)o->input->data + o->sh[o->eh->e_shstrndx].sh_offset +
                                        o->sh[s->sh_info].sh_name);
                    ++l->relative_count;
                }
            }
        }
    }
    return 0;
}

static void layout_kinds(Link* l) {
    size_t i, j, k;
    for (k = 0; k < K_COUNT; ++k) l->kind_align[k] = 1;
    for (k = 0; k < K_COUNT; ++k)
        for (i = 0; i < l->nobjs; ++i) {
            Obj* o = &l->objs[i];
            for (j = 0; j < o->eh->e_shnum; ++j) {
                uint64_t align = o->sh[j].sh_addralign ? o->sh[j].sh_addralign : 1;
                if (o->kind[j] != (int)k) continue;
                l->kind_size[k] = (l->kind_size[k] + align - 1) / align * align;
                o->position[j] = l->kind_size[k];
                l->kind_size[k] += o->sh[j].sh_size;
                if (align > l->kind_align[k]) l->kind_align[k] = align;
            }
            if (k == K_TEXT && o->veneers) { /* right after the object's code, in reach of all of it */
                o->island = (l->kind_size[k] + 3) & ~(uint64_t)3;
                l->kind_size[k] = o->island + VENEER * o->veneers;
            }
        }
}

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static unsigned long elf_hash(const char* name) {
    unsigned long h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000UL;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static void put64(unsigned char* p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(unsigned char* p, uint32_t v) { memcpy(p, &v, 4); }
/* an AArch64 instruction with value in its bits from shift, width wide */
static void field(unsigned char* p, uint64_t value, unsigned shift, unsigned width) {
    uint32_t insn, mask = (uint32_t)(((uint64_t)1 << width) - 1) << shift;
    memcpy(&insn, p, 4);
    insn = (insn & ~mask) | ((uint32_t)(value << shift) & mask);
    memcpy(p, &insn, 4);
}
/* ADR and ADRP's 21-bit immediate: its low two bits at 29, the rest at 5 */
static void adr_field(unsigned char* p, uint64_t value) {
    field(p, value & 3, 29, 2);
    field(p, value >> 2, 5, 19);
}
static uint64_t page_of(uint64_t address) { return address & ~(uint64_t)0xFFF; }
/* whether value fits a signed field of bits */
static int fits(int64_t value, unsigned bits) {
    return value >= -((int64_t)1 << (bits - 1)) && value < ((int64_t)1 << (bits - 1));
}

/* the output's section headers, for tools: debuggers and objdump find the code and the symbols */
enum {
    S_NULL,
    S_HASH,
    S_DYNSYM,
    S_DYNSTR,
    S_RELA,
    S_RODATA,
    S_TEXT,
    S_DATA,
    S_GOT,
    S_DYNAMIC,
    S_BSS,
    S_SYMTAB,
    S_STRTAB,
    S_SHSTRTAB,
    S_COUNT
};
static const char* const section_names[S_COUNT] = {"",        ".hash",   ".dynsym", ".dynstr",  ".rela.dyn",
                                                   ".rodata", ".text",   ".data",   ".got",     ".dynamic",
                                                   ".bss",    ".symtab", ".strtab", ".shstrtab"};
static const int kind_section[K_COUNT] = {S_RODATA, S_TEXT, S_DATA, S_BSS};

int snx_link_elf(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                 const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    Link l;
    SnxLinkBuffer out, dynstr, strtab, symtab, rela;
    Elf64_Shdr shdr[S_COUNT];
    Elf64_Phdr ph[5];
    Elf64_Ehdr eh;
    uint64_t hash_off, dynsym_off, dynstr_off, rela_off, rodata_off, text_off, data_off, got_off, dynamic_off, bss_off,
        end_r, end_data, nbucket, *export_address;
    size_t i, j, k, nsyms = export_count + 1, ndyn = 0, symbol_count = 0;
    int rc = -1;
    unsigned char* p;
    Elf64_Dyn dyn[16];
    (void)soname;
    memset(&l, 0, sizeof(l));
    memset(&out, 0, sizeof(out));
    memset(&dynstr, 0, sizeof(dynstr));
    memset(&strtab, 0, sizeof(strtab));
    memset(&symtab, 0, sizeof(symtab));
    memset(&rela, 0, sizeof(rela));
    memset(output, 0, sizeof(*output));
    l.error = error;
    l.error_size = error_size;
    l.nobjs = count;
    l.objs = (Obj*)calloc(count, sizeof(Obj));
    export_address = (uint64_t*)calloc(export_count + 1, sizeof(uint64_t));
    if (!l.objs || !export_address) goto done;
    for (i = 0; i < count; ++i)
        if (parse(&l, &l.objs[i], &inputs[i])) goto done;
    l.relative = l.machine == EM_AARCH64 ? A64_RELATIVE : R_X86_64_RELATIVE;
    l.page = l.machine == EM_AARCH64 ? 0x10000 : 0x1000;
    if (collect_globals(&l) || scan_relocations(&l)) goto done;
    layout_kinds(&l);
    /* dynamic symbol names */
    snx_link_append(&dynstr, "", 1);
    for (i = 0; i < export_count; ++i) snx_link_append(&dynstr, exports[i], strlen(exports[i]) + 1);
    nbucket = export_count ? export_count : 1;
    /* addresses */
    hash_off = align_up(sizeof(Elf64_Ehdr) + sizeof(ph), 8);
    dynsym_off = align_up(hash_off + 4 * (2 + nbucket + nsyms), 8);
    dynstr_off = dynsym_off + nsyms * sizeof(Elf64_Sym);
    rela_off = align_up(dynstr_off + dynstr.size, 8);
    rodata_off = align_up(rela_off + l.relative_count * sizeof(Elf64_Rela), l.kind_align[K_RODATA]);
    end_r = rodata_off + l.kind_size[K_RODATA];
    text_off = align_up(end_r, l.page);
    data_off = align_up(align_up(text_off + l.kind_size[K_TEXT], l.page), l.kind_align[K_DATA]);
    got_off = align_up(data_off + l.kind_size[K_DATA], 8);
    dynamic_off = got_off + 8 * l.got_count;
    end_data = dynamic_off + sizeof(dyn);
    bss_off = align_up(end_data, l.kind_align[K_BSS]);
    l.kind_address[K_RODATA] = rodata_off;
    l.kind_address[K_TEXT] = text_off;
    l.kind_address[K_DATA] = data_off;
    l.kind_address[K_BSS] = bss_off;
    l.got_address = got_off;
    for (i = 0; i < count; ++i)
        for (j = 0; j < l.objs[i].eh->e_shnum; ++j)
            if (l.objs[i].kind[j] >= 0)
                l.objs[i].address[j] = l.kind_address[l.objs[i].kind[j]] + l.objs[i].position[j];
    snx_link_reserve(&out, end_data);
    /* section contents */
    for (i = 0; i < count; ++i) {
        Obj* o = &l.objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j)
            if (o->kind[j] >= 0 && o->kind[j] != K_BSS && o->sh[j].sh_size)
                memcpy(out.data + o->address[j], o->input->data + o->sh[j].sh_offset, o->sh[j].sh_size);
    }
    /* GOT slots: the address of their symbol, relocated by the loader */
    for (i = 0; i < l.got.capacity; ++i)
        if (l.got.keys[i]) {
            unsigned long object, index;
            int absolute;
            uint64_t value, slot = got_off + 8 * l.got.values[i];
            Elf64_Rela r;
            sscanf(l.got.keys[i], "%lu:%lu", &object, &index);
            value = symbol_address(&l, object, index, &absolute);
            put64(out.data + slot, value);
            r.r_offset = slot;
            r.r_info = ELF64_R_INFO((Elf64_Xword)0, l.relative);
            r.r_addend = (int64_t)value;
            snx_link_append(&rela, &r, sizeof(r));
        }
    /* relocations */
    for (i = 0; i < count; ++i) {
        Obj* o = &l.objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = &o->sh[j];
            const Elf64_Rela* r;
            size_t n, def_object = 0, def_index = 0;
            if (s->sh_type != SHT_RELA || o->kind[s->sh_info] < 0) continue;
            r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            n = s->sh_size / sizeof(Elf64_Rela);
            for (k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                uint64_t P = o->address[s->sh_info] + r[k].r_offset, S = 0, value;
                int64_t A = r[k].r_addend, relative;
                int weak, absolute = 1;
                char key[72];
                size_t slot;
                uint64_t got;
                if (type != R_X86_64_NONE && r[k].r_offset + 4 > o->sh[s->sh_info].sh_size) goto bad;
                if (definition(&l, i, ELF64_R_SYM(r[k].r_info), &def_object, &def_index, &weak)) goto done;
                if (!weak) S = symbol_address(&l, def_object, def_index, &absolute);
                p = out.data + P;
                switch (type) {
                    case R_X86_64_NONE: break;
                    case R_X86_64_64:
                    case A64_ABS64:
                        value = S + (uint64_t)A;
                        put64(p, value);
                        if (!weak && !absolute) {
                            Elf64_Rela d;
                            d.r_offset = P;
                            d.r_info = ELF64_R_INFO((Elf64_Xword)0, l.relative);
                            d.r_addend = (int64_t)value;
                            snx_link_append(&rela, &d, sizeof(d));
                        }
                        break;
                    case R_X86_64_PC32:
                    case R_X86_64_PLT32:
                        relative = (int64_t)(S + (uint64_t)A - P);
                        if (relative != (int32_t)relative) goto range;
                        put32(p, (uint32_t)relative);
                        break;
                    case R_X86_64_PC64: put64(p, S + (uint64_t)A - P); break;
                    case R_X86_64_GOTPCREL:
                    case R_X86_64_GOTPCRELX:
                    case R_X86_64_REX_GOTPCRELX:
                        got_key(key, def_object, def_index);
                        snx_link_find(&l.got, key, &slot);
                        relative = (int64_t)(got_off + 8 * slot + (uint64_t)A - P);
                        if (relative != (int32_t)relative) goto range;
                        put32(p, (uint32_t)relative);
                        break;
                    case R_X86_64_GOTOFF64: put64(p, S + (uint64_t)A - got_off); break;
                    case R_X86_64_GOTPC32:
                        relative = (int64_t)(got_off + (uint64_t)A - P);
                        if (relative != (int32_t)relative) goto range;
                        put32(p, (uint32_t)relative);
                        break;
                    case R_X86_64_GOTPC64: put64(p, got_off + (uint64_t)A - P); break;
                    case A64_PREL64: put64(p, S + (uint64_t)A - P); break;
                    case A64_PREL32:
                        relative = (int64_t)(S + (uint64_t)A - P);
                        if (relative != (int32_t)relative) goto range;
                        put32(p, (uint32_t)relative);
                        break;
                    case A64_ADR_PREL_LO21:
                        relative = (int64_t)(S + (uint64_t)A - P);
                        if (!fits(relative, 21)) goto range;
                        adr_field(p, (uint64_t)relative);
                        break;
                    case A64_ADR_PREL_PG_HI21:
                    case A64_ADR_PREL_PG_HI21_NC:
                        relative = (int64_t)(page_of(S + (uint64_t)A) - page_of(P)) >> 12;
                        if (type == A64_ADR_PREL_PG_HI21 && !fits(relative, 21)) goto range;
                        adr_field(p, (uint64_t)relative);
                        break;
                    case A64_ADD_ABS_LO12_NC: field(p, (S + (uint64_t)A) & 0xFFF, 10, 12); break;
                    case A64_LDST8_ABS_LO12_NC:
                    case A64_LDST16_ABS_LO12_NC:
                    case A64_LDST32_ABS_LO12_NC:
                    case A64_LDST64_ABS_LO12_NC:
                    case A64_LDST128_ABS_LO12_NC: {
                        /* the offset is scaled by the access's size, which the address must be a multiple of */
                        unsigned shift = type == A64_LDST8_ABS_LO12_NC    ? 0
                                         : type == A64_LDST16_ABS_LO12_NC ? 1
                                         : type == A64_LDST32_ABS_LO12_NC ? 2
                                         : type == A64_LDST64_ABS_LO12_NC ? 3
                                                                          : 4;
                        value = (S + (uint64_t)A) & 0xFFF;
                        if (value & ((1u << shift) - 1)) goto misaligned;
                        field(p, value >> shift, 10, 12);
                        break;
                    }
                    case A64_ADR_GOT_PAGE:
                    case A64_LD64_GOT_LO12_NC:
                        got_key(key, def_object, def_index);
                        snx_link_find(&l.got, key, &slot);
                        got = got_off + 8 * slot;
                        if (type == A64_LD64_GOT_LO12_NC) {
                            field(p, (got & 0xFFF) >> 3, 10, 12);
                            break;
                        }
                        relative = (int64_t)(page_of(got + (uint64_t)A) - page_of(P)) >> 12;
                        if (!fits(relative, 21)) goto range;
                        adr_field(p, (uint64_t)relative);
                        break;
                    case A64_CALL26:
                    case A64_JUMP26:
                        relative = (int64_t)(S + (uint64_t)A - P);
                        if (!fits(relative, 28)) {
                            /* the caller's veneer, which it reaches, jumps there: ADRP x16, ADD x16, BR x16 */
                            uint64_t target = S + (uint64_t)A, at;
                            unsigned char* v;
                            veneer_key(key, i, def_object, def_index);
                            if (weak || !snx_link_find(&l.veneer, key, &slot)) goto range;
                            at = l.kind_address[K_TEXT] + o->island + VENEER * slot;
                            v = out.data + at;
                            put32(v, 0x90000010u);
                            adr_field(v, (uint64_t)((int64_t)(page_of(target) - page_of(at)) >> 12));
                            put32(v + 4, 0x91000210u);
                            field(v + 4, target & 0xFFF, 10, 12);
                            put32(v + 8, 0xD61F0200u);
                            relative = (int64_t)(at - P);
                            if (!fits(relative, 28)) goto range;
                        }
                        field(p, (uint64_t)relative >> 2, 0, 26);
                        break;
                    case A64_CONDBR19:
                        relative = (int64_t)(S + (uint64_t)A - P);
                        if (!fits(relative, 21)) goto range;
                        field(p, (uint64_t)relative >> 2, 5, 19);
                        break;
                    case A64_TSTBR14:
                        relative = (int64_t)(S + (uint64_t)A - P);
                        if (!fits(relative, 16)) goto range;
                        field(p, (uint64_t)relative >> 2, 5, 14);
                        break;
                    default:
                        snprintf(error, error_size, "%s: unsupported relocation type %u", o->input->name, type);
                        goto done;
                }
                continue;
            range:
                snprintf(error, error_size, "%s: relocation %u out of range (the module is too large)", o->input->name,
                         type);
                goto done;
            misaligned:
                snprintf(error, error_size, "%s: relocation %u to a misaligned address", o->input->name, type);
                goto done;
            bad:
                snprintf(error, error_size, "%s: relocation outside its section", o->input->name);
                goto done;
            }
        }
    }
    /* exports */
    for (i = 0; i < export_count; ++i) {
        size_t found;
        int absolute;
        if (!snx_link_find(&l.globals, exports[i], &found)) {
            snprintf(error, error_size, "exported symbol %s is not defined", exports[i]);
            goto done;
        }
        export_address[i] = symbol_address(&l, found >> 32, found & 0xFFFFFFFFu, &absolute);
    }
    /* .hash: nbucket, nchain, buckets, chains */
    p = out.data + hash_off;
    put32(p, (uint32_t)nbucket);
    put32(p + 4, (uint32_t)nsyms);
    for (i = 0; i < export_count; ++i) {
        uint32_t* bucket = (uint32_t*)(void*)(p + 8 + 4 * (elf_hash(exports[i]) % nbucket));
        uint32_t* chain = (uint32_t*)(void*)(p + 8 + 4 * nbucket + 4 * (i + 1));
        uint32_t previous;
        memcpy(&previous, bucket, 4);
        memcpy(chain, &previous, 4);
        put32((unsigned char*)bucket, (uint32_t)(i + 1));
    }
    /* .dynsym */
    for (i = 0, k = 1; i < export_count; ++i) {
        Elf64_Sym s;
        memset(&s, 0, sizeof(s));
        s.st_name = (uint32_t)k;
        k += strlen(exports[i]) + 1;
        s.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
        s.st_shndx = S_TEXT;
        s.st_value = export_address[i];
        memcpy(out.data + dynsym_off + (i + 1) * sizeof(Elf64_Sym), &s, sizeof(s));
    }
    memcpy(out.data + dynstr_off, dynstr.data, dynstr.size);
    memcpy(out.data + rela_off, rela.data, rela.size);
    /* .dynamic */
#define DYN(tag, value)                                                                                                \
    do {                                                                                                               \
        dyn[ndyn].d_tag = (tag);                                                                                       \
        dyn[ndyn++].d_un.d_val = (value);                                                                              \
    } while (0)
    DYN(DT_HASH, hash_off);
    DYN(DT_STRTAB, dynstr_off);
    DYN(DT_SYMTAB, dynsym_off);
    DYN(DT_STRSZ, dynstr.size);
    DYN(DT_SYMENT, sizeof(Elf64_Sym));
    if (rela.size) {
        DYN(DT_RELA, rela_off);
        DYN(DT_RELASZ, rela.size);
        DYN(DT_RELAENT, sizeof(Elf64_Rela));
        DYN(DT_RELACOUNT, rela.size / sizeof(Elf64_Rela));
    }
    while (ndyn < sizeof(dyn) / sizeof(dyn[0])) DYN(DT_NULL, 0);
#undef DYN
    memcpy(out.data + dynamic_off, dyn, sizeof(dyn));
    /* program headers */
    memset(ph, 0, sizeof(ph));
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R;
    ph[0].p_filesz = ph[0].p_memsz = end_r;
    ph[0].p_align = l.page;
    ph[1].p_type = PT_LOAD;
    ph[1].p_flags = PF_R | PF_X;
    ph[1].p_offset = ph[1].p_vaddr = ph[1].p_paddr = text_off;
    ph[1].p_filesz = ph[1].p_memsz = l.kind_size[K_TEXT];
    ph[1].p_align = l.page;
    ph[2].p_type = PT_LOAD;
    ph[2].p_flags = PF_R | PF_W;
    ph[2].p_offset = ph[2].p_vaddr = ph[2].p_paddr = data_off;
    ph[2].p_filesz = end_data - data_off;
    ph[2].p_memsz = bss_off + l.kind_size[K_BSS] - data_off;
    ph[2].p_align = l.page;
    ph[3].p_type = PT_DYNAMIC;
    ph[3].p_flags = PF_R | PF_W;
    ph[3].p_offset = ph[3].p_vaddr = ph[3].p_paddr = dynamic_off;
    ph[3].p_filesz = ph[3].p_memsz = sizeof(dyn);
    ph[3].p_align = 8;
    ph[4].p_type = PT_GNU_STACK; /* without it, the loader makes the stack executable */
    ph[4].p_flags = PF_R | PF_W;
    ph[4].p_align = 16;
    memcpy(out.data + sizeof(Elf64_Ehdr), ph, sizeof(ph));
    /* .symtab: every named function and object, for debuggers and crash reports; the exports last, global */
    snx_link_append(&strtab, "", 1);
    {
        Elf64_Sym s;
        memset(&s, 0, sizeof(s));
        snx_link_append(&symtab, &s, sizeof(s));
    }
    for (i = 0; i < count; ++i) {
        Obj* o = &l.objs[i];
        for (j = 1; j < o->nsyms; ++j) {
            const Elf64_Sym* src = &o->syms[j];
            int type = ELF64_ST_TYPE(src->st_info);
            Elf64_Sym s;
            size_t found;
            if ((type != STT_FUNC && type != STT_OBJECT) || src->st_shndx == SHN_UNDEF ||
                src->st_shndx >= SHN_LORESERVE || o->kind[src->st_shndx] < 0 || !o->strtab[src->st_name])
                continue;
            if (ELF64_ST_BIND(src->st_info) != STB_LOCAL && snx_link_find(&l.globals, sym_name(o, src), &found) &&
                found != ((i << 32) | j))
                continue; /* an overridden weak definition */
            memset(&s, 0, sizeof(s));
            s.st_name = (uint32_t)snx_link_append(&strtab, sym_name(o, src), strlen(sym_name(o, src)) + 1);
            s.st_info = ELF64_ST_INFO(STB_LOCAL, type);
            s.st_other = STV_DEFAULT;
            s.st_shndx = (uint16_t)kind_section[o->kind[src->st_shndx]];
            s.st_value = o->address[src->st_shndx] + src->st_value;
            s.st_size = src->st_size;
            snx_link_append(&symtab, &s, sizeof(s));
        }
    }
    symbol_count = symtab.size / sizeof(Elf64_Sym);
    for (i = 0; i < export_count; ++i) {
        Elf64_Sym s;
        memset(&s, 0, sizeof(s));
        s.st_name = (uint32_t)snx_link_append(&strtab, exports[i], strlen(exports[i]) + 1);
        s.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
        s.st_shndx = S_TEXT;
        s.st_value = export_address[i];
        snx_link_append(&symtab, &s, sizeof(s));
    }
    /* trailing, unloaded: symbol table, strings, section names, section headers */
    memset(shdr, 0, sizeof(shdr));
    snx_link_align(&out, 8);
    shdr[S_SYMTAB].sh_offset = snx_link_append(&out, symtab.data, symtab.size);
    shdr[S_STRTAB].sh_offset = snx_link_append(&out, strtab.data, strtab.size);
    {
        SnxLinkBuffer names;
        memset(&names, 0, sizeof(names));
        for (i = 0; i < S_COUNT; ++i)
            shdr[i].sh_name = (uint32_t)snx_link_append(&names, section_names[i], strlen(section_names[i]) + 1);
        shdr[S_SHSTRTAB].sh_offset = snx_link_append(&out, names.data, names.size);
        shdr[S_SHSTRTAB].sh_size = names.size;
        free(names.data);
    }
#define SECTION(i, type, flags, address, size, link, info, align, entsize)                                             \
    do {                                                                                                               \
        shdr[i].sh_type = (type);                                                                                      \
        shdr[i].sh_flags = (flags);                                                                                    \
        shdr[i].sh_addr = (address);                                                                                   \
        if ((type) != SHT_SYMTAB && (type) != SHT_STRTAB) shdr[i].sh_offset = (address);                               \
        if (i == S_STRTAB || i == S_SYMTAB || i == S_SHSTRTAB) shdr[i].sh_addr = 0;                                    \
        shdr[i].sh_size = (size);                                                                                      \
        shdr[i].sh_link = (link);                                                                                      \
        shdr[i].sh_info = (info);                                                                                      \
        shdr[i].sh_addralign = (align);                                                                                \
        shdr[i].sh_entsize = (entsize);                                                                                \
    } while (0)
    SECTION(S_HASH, SHT_HASH, SHF_ALLOC, hash_off, 4 * (2 + nbucket + nsyms), S_DYNSYM, 0, 8, 4);
    SECTION(S_DYNSYM, SHT_DYNSYM, SHF_ALLOC, dynsym_off, nsyms * sizeof(Elf64_Sym), S_DYNSTR, 1, 8, sizeof(Elf64_Sym));
    SECTION(S_DYNSTR, SHT_STRTAB, SHF_ALLOC, dynstr_off, dynstr.size, 0, 0, 1, 0);
    shdr[S_DYNSTR].sh_offset = dynstr_off;
    SECTION(S_RELA, SHT_RELA, SHF_ALLOC, rela_off, rela.size, S_DYNSYM, 0, 8, sizeof(Elf64_Rela));
    SECTION(S_RODATA, SHT_PROGBITS, SHF_ALLOC, rodata_off, l.kind_size[K_RODATA], 0, 0, l.kind_align[K_RODATA], 0);
    SECTION(S_TEXT, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, text_off, l.kind_size[K_TEXT], 0, 0, 16, 0);
    SECTION(S_DATA, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, data_off, l.kind_size[K_DATA], 0, 0, l.kind_align[K_DATA], 0);
    SECTION(S_GOT, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, got_off, 8 * l.got_count, 0, 0, 8, 8);
    SECTION(S_DYNAMIC, SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, dynamic_off, sizeof(dyn), S_DYNSTR, 0, 8, sizeof(Elf64_Dyn));
    SECTION(S_BSS, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, bss_off, l.kind_size[K_BSS], 0, 0, l.kind_align[K_BSS], 0);
    shdr[S_BSS].sh_offset = end_data;
    SECTION(S_SYMTAB, SHT_SYMTAB, 0, 0, symtab.size, S_STRTAB, (uint32_t)symbol_count, 8, sizeof(Elf64_Sym));
    SECTION(S_STRTAB, SHT_STRTAB, 0, 0, strtab.size, 0, 0, 1, 0);
    shdr[S_SHSTRTAB].sh_type = SHT_STRTAB;
    shdr[S_SHSTRTAB].sh_addralign = 1;
#undef SECTION
    snx_link_align(&out, 8);
    /* ELF header */
    memset(&eh, 0, sizeof(eh));
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_ident[EI_OSABI] = ELFOSABI_NONE;
    eh.e_type = ET_DYN;
    eh.e_machine = (Elf64_Half)l.machine;
    eh.e_version = EV_CURRENT;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = sizeof(ph) / sizeof(ph[0]);
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = S_COUNT;
    eh.e_shstrndx = S_SHSTRTAB;
    eh.e_shoff = snx_link_append(&out, shdr, sizeof(shdr));
    memcpy(out.data, &eh, sizeof(eh));
    output->data = out.data;
    output->size = out.size;
    out.data = NULL;
    rc = 0;
done:
    for (i = 0; l.objs && i < count; ++i) {
        free(l.objs[i].address);
    }
    free(l.objs);
    free(export_address);
    snx_link_names_free(&l.globals);
    snx_link_names_free(&l.got);
    snx_link_names_free(&l.veneer);
    free(out.data);
    free(dynstr.data);
    free(strtab.data);
    free(symtab.data);
    free(rela.data);
    return rc;
}
#endif
