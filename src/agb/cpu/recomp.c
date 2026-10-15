/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "recomp.h"
#include "board/board.h"
#include "common.h"
#include "cpu/arm/llvm.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define AGB_RECOMP_ABI 2 /* 1-based block indices */
#define AGB_BLOCK_MAX  256 /* words compiled per block at most */

void agb_recomp_init(AgbRecomp* recomp, const char* base, const char* cc) {
    memset(recomp, 0, sizeof(*recomp));
    recomp->base = (char*)malloc(strlen(base) + 1);
    if (recomp->base) strcpy(recomp->base, base);
    recomp->cc = cc;
}

void agb_recomp_free(AgbRecomp* recomp) {
    if (recomp->handle) {
#ifdef _WIN32
        FreeLibrary((HMODULE)recomp->handle);
#else
        dlclose(recomp->handle);
#endif
    }
    free(recomp->blocks);
    free(recomp->base);
    memset(recomp, 0, sizeof(*recomp));
}

static void block_name(char* out, size_t size, size_t index) { snprintf(out, size, "snx_agb_%zu", index); }

static void* module_symbol(AgbRecomp* recomp, size_t index) {
    char name[32];
    block_name(name, sizeof(name), index);
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)recomp->handle, name);
#else
    return dlsym(recomp->handle, name);
#endif
}

/* ---- block discovery ---- */

static AgbBlock* find(AgbRecomp* recomp, uint32_t pc, int thumb) {
    /* index 0 is none: the project's rule for indices */
    for (size_t i = 1; i <= recomp->count; ++i)
        if (recomp->blocks[i].pc == pc && recomp->blocks[i].thumb == (uint8_t)thumb) return &recomp->blocks[i];
    return NULL;
}

static AgbBlock* add(AgbRecomp* recomp, uint32_t pc, int thumb) {
    AgbBlock* block = find(recomp, pc, thumb);
    if (block) return block;
    if (recomp->count + 1 >= recomp->capacity) {
        size_t capacity = recomp->capacity ? recomp->capacity * 2 : 257;
        AgbBlock* grown = (AgbBlock*)realloc(recomp->blocks, capacity * sizeof(*grown));
        if (!grown) return NULL;
        recomp->blocks = grown;
        recomp->capacity = capacity;
    }
    block = &recomp->blocks[++recomp->count];
    memset(block, 0, sizeof(*block));
    block->pc = pc;
    block->thumb = (uint8_t)thumb;
    recomp->dirty = 1;
    return block;
}

static int32_t thumb_target(uint32_t pc, uint16_t hw1, uint16_t hw2, unsigned op) {
    if (op == BR_B) return (int32_t)(pc + 4 + (uint32_t)((int32_t)((hw1 & 0x7FF) << 21) >> 20));
    if (op == BR_BCOND) return (int32_t)(pc + 4 + (uint32_t)((int32_t)((hw1 & 0xFF) << 24) >> 23));
    if (op == BR_CBZ || op == BR_CBNZ)
        return (int32_t)(pc + 4 + ((((hw1 >> 9) & 1) << 5) | ((hw1 >> 3) & 31)) * 2);
    /* the Thumb32 forms spread the offset over both halfwords */
    {
        unsigned s = hw1 >> 10 & 1, i1 = (hw2 >> 13 & 1) ^ 1 ^ s, i2 = (hw2 >> 11 & 1) ^ 1 ^ s;
        uint32_t raw = s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1;
        return (int32_t)(pc + 4 + (uint32_t)((int32_t)(raw << 7) >> 7));
    }
}

/* The walk mirrors the recompiler's block ends: a control transfer (conditional ones too), a trap, or an
   undecodable instruction. Successors become block starts of their own. The walk is an explicit work
   list: the recursive form could chase a branch chain tail first without ever growing the stack. */
static void discover(AgbRecomp* recomp, AgbBoard* board, uint32_t pc, int thumb) {
    struct Work {
        uint32_t pc;
        int thumb;
    } work[1024];
    size_t count = 0, budget = 65536; /* the walk ends when the list empties or the budget runs out */
    work[count].pc = pc;
    work[count++].thumb = thumb;
    while (count > 0) {
        struct Work start = work[count - 1];
        --count;
        if (find(recomp, start.pc, start.thumb) || !add(recomp, start.pc, start.thumb)) continue;
        if (!--budget) return; /* whatever is left is compiled when it is first dispatched */
#define PUSH(to_pc, to_thumb)                                                                                        \
    do {                                                                                                             \
        if (count < sizeof(work) / sizeof(work[0])) {                                                                \
            work[count].pc = (uint32_t)(to_pc);                                                                      \
            work[count++].thumb = (to_thumb);                                                                        \
        }                                                                                                            \
    } while (0)
        if (start.thumb) {
            for (uint32_t p = start.pc, n = 0; n < AGB_BLOCK_MAX / 2; ++n) {
                uint16_t hw1 = agb_read16(board, p), hw2 = agb_read16(board, p + 2);
                SnxThumb t;
                unsigned len = snx_thumb_decode(hw1, hw2, &t);
                if (!len) break;
                if (t.shape == TH_BRANCH) {
                    int32_t target = thumb_target(p, hw1, hw2, t.op);
                    int fallthrough = t.op == BR_BL || t.op == BR_BLX_IMM || t.op == BR_BCOND || t.op == BR_CBZ ||
                                      t.op == BR_CBNZ;
                    if (t.op == BR_B || t.op == BR_BCOND || t.op == BR_CBZ || t.op == BR_CBNZ || t.op == BR_BL ||
                        t.op == BR_BLX_IMM || t.op == BR_BW)
                        PUSH(target, t.op != BR_BLX_IMM);
                    if (fallthrough) PUSH(p + len * 2, 1);
                    break;
                }
                if (t.shape == TH_TABLE || t.shape == TH_IT) break;
                p += len * 2;
            }
        } else {
            for (uint32_t p = start.pc, n = 0; n < AGB_BLOCK_MAX; ++n, p += 4) {
                uint32_t w = agb_read32(board, p);
                SnxA32Kind kind = snx_a32_decode(w);
                unsigned cond = w >> 28;
                if (kind == SNX_A32_BRANCH) {
                    uint32_t target = p + 8 + (uint32_t)((int32_t)(w << 8) >> 6);
                    PUSH(target, cond == 15); /* BLX lands in Thumb */
                    if ((w & (1u << 24)) || cond != 14) PUSH(p + 4, 0);
                    break;
                }
                if (kind == SNX_A32_BLX_IMM) {
                    uint32_t target = p + 8 + (uint32_t)((int32_t)(w << 8) >> 6) + (w >> 24 & 1) * 2;
                    PUSH(target & ~(uint32_t)3, 1);
                    break;
                }
                if (kind == SNX_A32_BX || kind == SNX_A32_SVC || kind == SNX_A32_BKPT || kind == SNX_A32_UNDEFINED) {
                    if (kind != SNX_A32_BX) PUSH(p + 4, 0);
                    break;
                }
                if ((kind == SNX_A32_DATA && (w >> 12 & 15) == 15) || (kind == SNX_A32_MULTIPLE && (w & 0x8000)) ||
                    (kind == SNX_A32_SINGLE && (w >> 12 & 15) == 15)) {
                    /* a write to the PC ends the block, and conditional ones continue after it */
                    if (cond != 14) PUSH(p + 4, 0);
                    break;
                }
            }
        }
#undef PUSH
    }
}

/* ---- linking ---- */

static int emit_module(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    static int initialized;
    char *object = NULL, *library = NULL, *message = NULL, *triple;
    const char* args[10];
    LLVMContextRef ctx = NULL;
    LLVMModuleRef module = NULL;
    LLVMTargetMachineRef machine = NULL;
    LLVMTargetDataRef layout = NULL;
    LLVMPassBuilderOptionsRef options = NULL;
    LLVMTargetRef target;
    size_t n = 0;
    int result = -1;
    if (!initialized) {
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
        initialized = 1;
    }
    ctx = LLVMContextCreate();
    module = LLVMModuleCreateWithNameInContext("agb-code", ctx);
    triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(triple, &target, &message)) goto done;
    machine = LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC,
                                      LLVMCodeModelDefault);
    LLVMSetTarget(module, triple);
    layout = LLVMCreateTargetDataLayout(machine);
    LLVMSetModuleDataLayout(module, layout);
    for (size_t i = 1; i <= recomp->count; ++i) {
        AgbBlock* block = &recomp->blocks[i];
        char name[32];
        size_t consumed = 0;
        LLVMValueRef fn;
        block_name(name, sizeof(name), i);
        if (block->thumb) {
            uint16_t words[AGB_BLOCK_MAX];
            for (uint32_t j = 0; j < AGB_BLOCK_MAX; ++j) words[j] = agb_read16(board, block->pc + j * 2);
            fn = snx_thumb_llvm_block(module, name, words, AGB_BLOCK_MAX, block->pc, &consumed);
        } else {
            uint32_t words[AGB_BLOCK_MAX];
            for (uint32_t j = 0; j < AGB_BLOCK_MAX; ++j) words[j] = agb_read32(board, block->pc + j * 4);
            fn = snx_arm_llvm_block(module, name, words, AGB_BLOCK_MAX, block->pc, &consumed);
        }
        if (!fn || !consumed) {
            snprintf(error, error_size, "the ARM recompiler rejected the block at 0x%08X", block->pc);
            goto done;
        }
        block->instructions = (uint16_t)(consumed > 0xFFFF ? 0xFFFF : consumed);
    }
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) goto done;
    options = LLVMCreatePassBuilderOptions();
    {
        LLVMErrorRef pass = LLVMRunPasses(module, "default<O2>", machine, options);
        if (pass) {
            message = LLVMGetErrorMessage(pass);
            goto done;
        }
    }
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) goto done;
    object = (char*)malloc(strlen(recomp->base) + 8);
    library = (char*)malloc(strlen(recomp->base) + 8);
    if (!object || !library) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    sprintf(object, "%s.obj", recomp->base);
    sprintf(library, "%s.so", recomp->base);
    if (LLVMTargetMachineEmitToFile(machine, module, object, LLVMObjectFile, &message)) goto done;
    args[n++] = recomp->cc;
    args[n++] = "-shared";
    args[n++] = "-o";
    args[n++] = library;
    args[n++] = object;
#ifndef _WIN32
    args[n++] = "-lm";
#endif
    args[n] = NULL;
    {
        char* output = NULL;
        if (snx_run(args, NULL, NULL, &output)) {
            snprintf(error, error_size, "linking the recompiled ROM failed: %.300s", output ? output : "");
            free(output);
            goto done;
        }
        free(output);
    }
    remove(object);
    result = 0;
done:
    if (result && message && !error[0]) snprintf(error, error_size, "%.300s", message);
    free(object);
    free(library);
    if (options) LLVMDisposePassBuilderOptions(options);
    if (layout) LLVMDisposeTargetData(layout);
    if (machine) LLVMDisposeTargetMachine(machine);
    if (module) LLVMDisposeModule(module);
    if (ctx) LLVMContextDispose(ctx);
    free(triple);
    return result;
}

static int open_module(AgbRecomp* recomp, char* error, size_t error_size) {
    size_t length = strlen(recomp->base);
    char* library = (char*)malloc(length + 12);
    if (!library) return -1;
    /* a name without a directory would not be looked up in the working directory */
    if (strchr(recomp->base, '/') || strchr(recomp->base, '\\'))
        sprintf(library, "%s.so", recomp->base);
    else
        sprintf(library, "./%s.so", recomp->base);
#ifdef _WIN32
    recomp->handle = (void*)LoadLibraryA(library);
#else
    recomp->handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
#endif
    free(library);
    if (!recomp->handle) {
#ifndef _WIN32
        const char* why = dlerror();
        snprintf(error, error_size, "cannot load the recompiled ROM module: %s", why ? why : "unknown");
#else
        snprintf(error, error_size, "cannot load the recompiled ROM module");
#endif
        return -1;
    }
    for (size_t i = 1; i <= recomp->count; ++i) recomp->blocks[i].fn = (SnxArmBlock)module_symbol(recomp, i);
    return 0;
}

static int relink(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    if (recomp->handle) {
#ifdef _WIN32
        FreeLibrary((HMODULE)recomp->handle);
#else
        dlclose(recomp->handle);
#endif
        recomp->handle = NULL;
    }
    if (emit_module(recomp, board, error, error_size)) return -1;
    if (open_module(recomp, error, error_size)) return -1;
    recomp->dirty = 0;
    return 0;
}

/* The cache beside the ROM: the module plus a map of the blocks it holds. */
static int write_cache(AgbRecomp* recomp) {
    size_t length = strlen(recomp->base);
    char* map_path = (char*)malloc(length + 8);
    FILE* file;
    if (!map_path) return -1;
    sprintf(map_path, "%s.map", recomp->base);
    file = fopen(map_path, "w");
    free(map_path);
    if (!file) return -1;
    fprintf(file, "snx-agb-code %d %u %u\n", AGB_RECOMP_ABI, SNX_ARM_ABI, 0);
    for (size_t i = 1; i <= recomp->count; ++i)
        fprintf(file, "%08X %u %u snx_agb_%zu\n", recomp->blocks[i].pc, recomp->blocks[i].thumb,
                recomp->blocks[i].instructions, i);
    fclose(file);
    return 0;
}

int agb_recomp_load_cache(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    size_t length = strlen(recomp->base);
    char* map_path = (char*)malloc(length + 8);
    char line[128];
    FILE* file;
    int abi = 0;
    unsigned arm_abi = 0, size = 0;
    (void)board;
    if (!map_path) return -1;
    sprintf(map_path, "%s.map", recomp->base);
    file = fopen(map_path, "r");
    free(map_path);
    if (!file) return -1;
    if (!fgets(line, sizeof(line), file) ||
        sscanf(line, "snx-agb-code %d %u %u", &abi, &arm_abi, &size) != 3 || abi != AGB_RECOMP_ABI ||
        arm_abi != SNX_ARM_ABI) {
        fclose(file);
        return -1;
    }
    while (fgets(line, sizeof(line), file)) {
        unsigned pc, thumb, instructions;
        if (sscanf(line, "%X %u %u", &pc, &thumb, &instructions) != 3 || !instructions) continue;
        if (!add(recomp, pc, (int)thumb)) {
            fclose(file);
            return -1;
        }
        recomp->blocks[recomp->count - 1].instructions = (uint16_t)instructions;
    }
    fclose(file);
    recomp->dirty = 0;
    return open_module(recomp, error, error_size);
}

int agb_recomp_build(AgbRecomp* recomp, AgbBoard* board, char* error, size_t error_size) {
    if (!recomp->base || !recomp->count) discover(recomp, board, 0x080000C0, 0);
    if (!recomp->count) {
        snprintf(error, error_size, "no code found at the ROM entry");
        return -1;
    }
    if (recomp->dirty || !recomp->handle)
        if (relink(recomp, board, error, error_size)) return -1;
    write_cache(recomp);
    return 0;
}

const AgbBlock* agb_recomp_lookup(AgbRecomp* recomp, AgbBoard* board, uint32_t pc, int thumb, char* error,
                                  size_t error_size) {
    AgbBlock* block = find(recomp, pc, thumb);
    if (block && block->fn && block->instructions) return block;
    if (!block) {
        block = add(recomp, pc, thumb);
        if (!block) {
            snprintf(error, error_size, "out of memory");
            return NULL;
        }
    }
    /* a block the discovery step did not reach: compile it and link again */
    if (relink(recomp, board, error, error_size)) return NULL;
    block = find(recomp, pc, thumb);
    return block && block->fn && block->instructions ? block : NULL;
}
