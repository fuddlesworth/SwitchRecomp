/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ir.h"
#include "cpu.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- setup ---- */

SnxIrFeatures snx_ir_host;
static const SnxIrFeatures generic_features;

static int has_feature(const char* features, const char* name) {
    size_t n = strlen(name);
    const char* p;
    for (p = features; (p = strstr(p, name)) != NULL; p += n)
        if (p > features && p[-1] == '+' && (p[n] == ',' || !p[n])) return 1;
    return 0;
}

int snx_ir_init(char* error, size_t error_size) {
    char* triple;
    int x86, arm;
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        snprintf(error, error_size, "LLVM has no code generator for this host");
        return -1;
    }
    /* x86-64 and AArch64 hosts: the guest's barriers, acquires, releases and exclusives are fences and seq_cst atomics
       (cpu.h), so its plain accesses need no ordering of their own from the host */
    triple = LLVMGetDefaultTargetTriple();
    x86 = !strncmp(triple, "x86_64", 6);
    arm = !strncmp(triple, "aarch64", 7);
    if (!x86 && !arm)
        snprintf(error, error_size, "the LLVM backend targets x86-64 and AArch64 hosts, this one is %s", triple);
    LLVMDisposeMessage(triple);
    if (x86 && !snx_ir_host.x86) {
        char* f = LLVMGetHostCPUFeatures();
        snx_ir_host.sse42 = has_feature(f, "sse4.2");
        snx_ir_host.crc32 = snx_ir_host.sse42 && has_feature(f, "crc32");
        snx_ir_host.pclmul = has_feature(f, "pclmul");
        snx_ir_host.aes = has_feature(f, "aes");
        snx_ir_host.gfni = has_feature(f, "gfni");
        snx_ir_host.fma = has_feature(f, "fma");
        snx_ir_host.avx2 = has_feature(f, "avx2");
        snx_ir_host.avx512bw = has_feature(f, "avx512bw");
        snx_ir_host.avx512vl = has_feature(f, "avx512vl");
        snx_ir_host.avx512bitalg = has_feature(f, "avx512bitalg");
        LLVMDisposeMessage(f);
        snx_ir_host.x86 = 1; /* last: snx_ir_init may run again, the features stay as read */
    }
    return x86 || arm ? 0 : -1;
}

static LLVMCodeGenOptLevel codegen_level(char opt) {
    switch (opt) {
        case '0': return LLVMCodeGenLevelNone;
        case '1': return LLVMCodeGenLevelLess;
        case '3': return LLVMCodeGenLevelAggressive;
        default: return LLVMCodeGenLevelDefault;
    }
}

SnxIr* snx_ir_new(const char* name, char opt, char* error, size_t error_size) {
    return snx_ir_new_target(name, opt, NULL, error, error_size);
}

SnxIr* snx_ir_new_target(const char* name, char opt, const char* target_triple, char* error, size_t error_size) {
    SnxIr* ir;
    char *triple, *cpu, *features, *message = NULL, *layout;
    LLVMTargetRef target;
    LLVMTargetDataRef data;
    LLVMTypeRef params[2];
    ir = (SnxIr*)calloc(1, sizeof(*ir));
    if (!ir) return NULL;
    triple = target_triple ? LLVMCreateMessage(target_triple) : LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(triple, &target, &message)) {
        snprintf(error, error_size, "LLVM target %s: %s", triple, message ? message : "unavailable");
        LLVMDisposeMessage(message);
        LLVMDisposeMessage(triple);
        free(ir);
        return NULL;
    }
    /* -march=native -mtune=native, -fPIC (toolchain.c) */
    cpu = target_triple ? LLVMCreateMessage("x86-64") : LLVMGetHostCPUName();
    features = target_triple ? LLVMCreateMessage("+cx16") : LLVMGetHostCPUFeatures();
    ir->tm =
        LLVMCreateTargetMachine(target, triple, cpu, features, codegen_level(opt), LLVMRelocPIC, LLVMCodeModelDefault);
    ir->windows = strstr(triple, "windows") != NULL || strstr(triple, "mingw") != NULL;
    ir->features = target_triple ? &generic_features : &snx_ir_host;
    ir->ctx = LLVMContextCreate();
    ir->mod = LLVMModuleCreateWithNameInContext(name, ir->ctx);
    LLVMSetTarget(ir->mod, triple);
    data = LLVMCreateTargetDataLayout(ir->tm);
    layout = LLVMCopyStringRepOfTargetData(data);
    LLVMSetDataLayout(ir->mod, layout);
    LLVMDisposeMessage(layout);
    LLVMDisposeTargetData(data);
    LLVMDisposeMessage(triple);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    ir->b = LLVMCreateBuilderInContext(ir->ctx);
    ir->t_void = LLVMVoidTypeInContext(ir->ctx);
    ir->t_i1 = LLVMInt1TypeInContext(ir->ctx);
    ir->t_i8 = LLVMInt8TypeInContext(ir->ctx);
    ir->t_i16 = LLVMInt16TypeInContext(ir->ctx);
    ir->t_i32 = LLVMInt32TypeInContext(ir->ctx);
    ir->t_i64 = LLVMInt64TypeInContext(ir->ctx);
    ir->t_i128 = LLVMInt128TypeInContext(ir->ctx);
    ir->t_ptr = LLVMPointerTypeInContext(ir->ctx, 0);
    params[0] = ir->t_ptr;
    params[1] = ir->t_i64;
    ir->t_guest = LLVMFunctionType(ir->t_void, params, 2, 0);
    {
        /* !alias.scope/!noalias: one domain, two scopes, uniqued by name */
        LLVMMetadataRef domain, cpu[2], guest[2], list;
        LLVMMetadataRef name = LLVMMDStringInContext2(ir->ctx, "snx.domain", 10);
        domain = LLVMMDNodeInContext2(ir->ctx, &name, 1);
        cpu[0] = LLVMMDStringInContext2(ir->ctx, "snx.cpu", 7);
        cpu[1] = domain;
        guest[0] = LLVMMDStringInContext2(ir->ctx, "snx.guest", 9);
        guest[1] = domain;
        list = LLVMMDNodeInContext2(ir->ctx, cpu, 2);
        ir->scope_cpu = LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, &list, 1));
        list = LLVMMDNodeInContext2(ir->ctx, guest, 2);
        ir->scope_guest = LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, &list, 1));
        ir->kind_scope = LLVMGetMDKindIDInContext(ir->ctx, "alias.scope", 11);
        ir->kind_noalias = LLVMGetMDKindIDInContext(ir->ctx, "noalias", 7);
    }
    return ir;
}

void snx_ir_free(SnxIr* ir) {
    if (!ir) return;
    LLVMDisposeBuilder(ir->b);
    LLVMDisposeModule(ir->mod);
    LLVMContextDispose(ir->ctx);
    LLVMDisposeTargetMachine(ir->tm);
    free(ir);
}

int snx_ir_object(SnxIr* ir, char opt, unsigned char** data, size_t* size, char* error, size_t error_size) {
    char pipeline[16], *message = NULL;
    LLVMPassBuilderOptionsRef options;
    LLVMErrorRef failure;
    LLVMMemoryBufferRef buffer;
    *data = NULL;
    *size = 0;
    if (LLVMVerifyModule(ir->mod, LLVMReturnStatusAction, &message)) {
        snprintf(error, error_size, "invalid IR: %.900s", message ? message : "");
        LLVMDisposeMessage(message);
        return -1;
    }
    LLVMDisposeMessage(message);
    sprintf(pipeline, "default<O%c>", opt == 's' ? 's' : opt == 'z' ? 'z' : opt);
    options = LLVMCreatePassBuilderOptions();
    failure = LLVMRunPasses(ir->mod, pipeline, ir->tm, options);
    LLVMDisposePassBuilderOptions(options);
    if (failure) {
        message = LLVMGetErrorMessage(failure);
        snprintf(error, error_size, "LLVM passes: %s", message);
        LLVMDisposeErrorMessage(message);
        return -1;
    }
    message = NULL;
    if (LLVMTargetMachineEmitToMemoryBuffer(ir->tm, ir->mod, LLVMObjectFile, &message, &buffer)) {
        snprintf(error, error_size, "LLVM code generation: %s", message ? message : "");
        LLVMDisposeMessage(message);
        return -1;
    }
    *size = LLVMGetBufferSize(buffer);
    *data = (unsigned char*)malloc(*size ? *size : 1);
    if (*data) memcpy(*data, LLVMGetBufferStart(buffer), *size);
    LLVMDisposeMemoryBuffer(buffer);
    if (!*data) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    return 0;
}

char* snx_ir_text(SnxIr* ir) { return LLVMPrintModuleToString(ir->mod); }

/* ---- values ---- */

LLVMValueRef ir_i8(SnxIr* ir, unsigned v) { return LLVMConstInt(ir->t_i8, v & 0xFF, 0); }
LLVMValueRef ir_i16(SnxIr* ir, unsigned v) { return LLVMConstInt(ir->t_i16, v & 0xFFFF, 0); }
LLVMValueRef ir_i32(SnxIr* ir, SnxU32 v) { return LLVMConstInt(ir->t_i32, v, 0); }
LLVMValueRef ir_i64(SnxIr* ir, SnxU64 v) { return LLVMConstInt(ir->t_i64, v, 0); }

LLVMValueRef ir_cast(SnxIr* ir, LLVMValueRef v, LLVMTypeRef to, int is_signed) {
    unsigned from = LLVMGetIntTypeWidth(LLVMTypeOf(v)), width = LLVMGetIntTypeWidth(to);
    if (from == width) return v;
    if (from > width) return LLVMBuildTrunc(ir->b, v, to, "");
    return is_signed ? LLVMBuildSExt(ir->b, v, to, "") : LLVMBuildZExt(ir->b, v, to, "");
}
LLVMValueRef ir_u32(SnxIr* ir, LLVMValueRef v) { return ir_cast(ir, v, ir->t_i32, 0); }
LLVMValueRef ir_u64(SnxIr* ir, LLVMValueRef v) { return ir_cast(ir, v, ir->t_i64, 0); }
LLVMValueRef ir_s64(SnxIr* ir, LLVMValueRef v) { return ir_cast(ir, v, ir->t_i64, 1); }
LLVMValueRef ir_bool32(SnxIr* ir, LLVMValueRef condition) { return LLVMBuildZExt(ir->b, condition, ir->t_i32, ""); }

/* ---- the CPU ---- */

LLVMValueRef ir_cpu(SnxIr* ir, size_t offset) {
    LLVMValueRef index = ir_i64(ir, offset);
    if (!offset) return ir->c;
    return LLVMBuildInBoundsGEP2(ir->b, ir->t_i8, ir->c, &index, 1, "");
}
LLVMValueRef ir_load(SnxIr* ir, LLVMTypeRef type, LLVMValueRef pointer) {
    return LLVMBuildLoad2(ir->b, type, pointer, "");
}
void ir_store(SnxIr* ir, LLVMValueRef value, LLVMValueRef pointer) { LLVMBuildStore(ir->b, value, pointer); }
/* The guest's memory is its own allocation, never the CPU structure: telling LLVM lets it keep guest registers in
   host registers across guest loads and stores (and loops), which it can't prove from CPU* noalias alone once c is
   passed to a call */
LLVMValueRef ir_tag_cpu(SnxIr* ir, LLVMValueRef access) {
    LLVMSetMetadata(access, ir->kind_scope, ir->scope_cpu);
    LLVMSetMetadata(access, ir->kind_noalias, ir->scope_guest);
    return access;
}
LLVMValueRef ir_tag_guest(SnxIr* ir, LLVMValueRef access) {
    LLVMSetMetadata(access, ir->kind_scope, ir->scope_guest);
    LLVMSetMetadata(access, ir->kind_noalias, ir->scope_cpu);
    return access;
}
LLVMValueRef ir_cpu_load(SnxIr* ir, LLVMTypeRef type, size_t offset) {
    return ir_tag_cpu(ir, ir_load(ir, type, ir_cpu(ir, offset)));
}
void ir_cpu_store(SnxIr* ir, LLVMValueRef value, size_t offset) {
    ir_tag_cpu(ir, LLVMBuildStore(ir->b, value, ir_cpu(ir, offset)));
}

size_t ir_off_x(unsigned n) { return offsetof(CPU, x) + 8 * (size_t)n; }
size_t ir_off_v(unsigned n) { return offsetof(CPU, v) + 16 * (size_t)n; }
size_t ir_off_pc(void) { return offsetof(CPU, pc); }
size_t ir_off_cf(void) { return offsetof(CPU, cf); }
size_t ir_off_excl_on(void) { return offsetof(CPU, excl_on); }
size_t ir_off_fpcr(void) { return offsetof(CPU, fpcr); }
size_t ir_off_mem(void) { return offsetof(CPU, mem); }

LLVMValueRef ir_x(SnxIr* ir, unsigned n) { return ir_cpu_load(ir, ir->t_i64, ir_off_x(n)); }
void ir_set_x(SnxIr* ir, unsigned n, LLVMValueRef value) { ir_cpu_store(ir, ir_u64(ir, value), ir_off_x(n)); }
LLVMValueRef ir_rx(SnxIr* ir, unsigned n) { return n == 31 ? ir_i64(ir, 0) : ir_x(ir, n); }
void ir_setx(SnxIr* ir, unsigned n, LLVMValueRef value) {
    /* the value is computed even for XZR: it may set flags or touch memory */
    if (n != 31) ir_set_x(ir, n, value);
}
LLVMValueRef ir_vd(SnxIr* ir, unsigned n, unsigned lane) {
    return ir_cpu_load(ir, ir->t_i64, ir_off_v(n) + 8 * (size_t)lane);
}
void ir_set_vd(SnxIr* ir, unsigned n, unsigned lane, LLVMValueRef value) {
    ir_cpu_store(ir, ir_u64(ir, value), ir_off_v(n) + 8 * (size_t)lane);
}
static LLVMTypeRef lane_type(SnxIr* ir, unsigned size) {
    return size == 0 ? ir->t_i8 : size == 1 ? ir->t_i16 : size == 2 ? ir->t_i32 : ir->t_i64;
}
LLVMValueRef ir_ve(SnxIr* ir, unsigned n, unsigned size, unsigned index) {
    return ir_cpu_load(ir, lane_type(ir, size), ir_off_v(n) + ((size_t)index << size));
}
void ir_set_ve(SnxIr* ir, unsigned n, unsigned size, unsigned index, LLVMValueRef value) {
    ir_cpu_store(ir, ir_cast(ir, value, lane_type(ir, size), 0), ir_off_v(n) + ((size_t)index << size));
}
LLVMValueRef ir_pc(SnxIr* ir) { return ir_cpu_load(ir, ir->t_i64, offsetof(CPU, pc)); }
void ir_set_pc(SnxIr* ir, LLVMValueRef value) { ir_cpu_store(ir, value, offsetof(CPU, pc)); }

/* ---- guest memory ---- */

LLVMValueRef ir_mp(SnxIr* ir, LLVMValueRef address) {
    LLVMValueRef index = ir_u64(ir, address);
    return LLVMBuildGEP2(ir->b, ir->t_i8, ir->M, &index, 1, "");
}
static LLVMTypeRef int_type(SnxIr* ir, unsigned bits) { return LLVMIntTypeInContext(ir->ctx, bits); }
/* Guest addresses carry no alignment the host may rely on: the C dereferences of RDn/WRn compile to the same plain
   x86 moves as these align-1 accesses */
/* Every guest access is volatile: it happens exactly as the guest wrote it, as on the hardware. LLVM may otherwise
   hoist a load out of a loop that polls memory another thread writes (a relaxed atomic is a plain LDR), or sink a
   store out of one, now that the alias scopes let it move everything else */
LLVMValueRef ir_rd(SnxIr* ir, LLVMValueRef address, unsigned bits) {
    LLVMValueRef load = LLVMBuildLoad2(ir->b, int_type(ir, bits), ir_mp(ir, address), "");
    LLVMSetAlignment(load, 1);
    LLVMSetVolatile(load, 1);
    return ir_tag_guest(ir, load);
}
LLVMValueRef ir_rd_bytes(SnxIr* ir, LLVMValueRef address, unsigned bytes) {
    LLVMValueRef load = LLVMBuildLoad2(ir->b, LLVMVectorType(ir->t_i8, bytes), ir_mp(ir, address), "");
    LLVMSetAlignment(load, 1);
    LLVMSetVolatile(load, 1);
    return ir_tag_guest(ir, load);
}
void ir_wr_bytes(SnxIr* ir, LLVMValueRef address, LLVMValueRef value) {
    LLVMValueRef store = LLVMBuildStore(ir->b, value, ir_mp(ir, address));
    LLVMSetAlignment(store, 1);
    LLVMSetVolatile(store, 1);
    ir_tag_guest(ir, store);
}
void ir_wr(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value) {
    LLVMValueRef pointer = ir_mp(ir, address);
    LLVMValueRef store = LLVMBuildStore(ir->b, ir_cast(ir, value, int_type(ir, bits), 0), pointer);
    LLVMSetAlignment(store, 1);
    LLVMSetVolatile(store, 1);
    ir_tag_guest(ir, store);
}
void ir_rdq(SnxIr* ir, LLVMValueRef destination, LLVMValueRef address) {
    LLVMValueRef store = LLVMBuildStore(ir->b, ir_rd_bytes(ir, address, 16), destination);
    LLVMSetAlignment(store, 1);
}
void ir_wrq(SnxIr* ir, LLVMValueRef address, LLVMValueRef source) {
    LLVMValueRef load = LLVMBuildLoad2(ir->b, LLVMVectorType(ir->t_i8, 16), source, "");
    LLVMSetAlignment(load, 1);
    ir_wr_bytes(ir, address, load);
}
/* __atomic_load_n/__atomic_store_n on T* (cpu.h LDACQ/STREL): seq_cst, naturally aligned as the C type says */
LLVMValueRef ir_ldacq(SnxIr* ir, LLVMValueRef address, unsigned bits) {
    LLVMValueRef load = LLVMBuildLoad2(ir->b, int_type(ir, bits), ir_mp(ir, address), "");
    LLVMSetOrdering(load, LLVMAtomicOrderingSequentiallyConsistent);
    LLVMSetAlignment(load, bits / 8);
    return ir_tag_guest(ir, load);
}
void ir_strel(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value) {
    LLVMValueRef pointer = ir_mp(ir, address);
    LLVMValueRef store = LLVMBuildStore(ir->b, ir_cast(ir, value, int_type(ir, bits), 0), pointer);
    LLVMSetOrdering(store, LLVMAtomicOrderingSequentiallyConsistent);
    LLVMSetAlignment(store, bits / 8);
    ir_tag_guest(ir, store);
}

/* ---- functions and calls ---- */

static void attribute(SnxIr* ir, LLVMValueRef function, LLVMAttributeIndex index, const char* name) {
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    LLVMAddAttributeAtIndex(function, index, LLVMCreateEnumAttribute(ir->ctx, kind, 0));
}

LLVMValueRef ir_call(SnxIr* ir, LLVMValueRef function, LLVMValueRef* args, unsigned count) {
    return LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(function), function, args, count, "");
}

LLVMValueRef ir_extern(SnxIr* ir, const char* name, LLVMTypeRef ret, LLVMTypeRef* params, unsigned count) {
    LLVMValueRef function = LLVMGetNamedFunction(ir->mod, name);
    if (function) return function;
    function = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ret, params, count, 0));
    /* -fvisibility=hidden: everything the module links is local to it */
    LLVMSetVisibility(function, LLVMHiddenVisibility);
    attribute(ir, function, LLVMAttributeFunctionIndex, "nounwind");
    return function;
}

static LLVMValueRef rt_pointer(SnxIr* ir) { return ir_cpu_load(ir, ir->t_ptr, offsetof(CPU, rt)); }
size_t ir_rt_svc(void) { return offsetof(RtAPI, svc); }
size_t ir_rt_undef(void) { return offsetof(RtAPI, undef); }
size_t ir_rt_note_hint(void) { return offsetof(RtAPI, note_hint); }
size_t ir_rt_brk(void) { return offsetof(RtAPI, brk); }
size_t ir_rt_sev(void) { return offsetof(RtAPI, sev); }
size_t ir_rt_wfe(void) { return offsetof(RtAPI, wfe); }
size_t ir_rt_poll(void) { return offsetof(RtAPI, poll); }
size_t ir_off_poll(void) { return offsetof(CPU, poll); }

LLVMValueRef ir_rt_call(SnxIr* ir, size_t offset, LLVMTypeRef ret, LLVMTypeRef* params, LLVMValueRef* args,
                        unsigned count) {
    LLVMValueRef index = ir_i64(ir, offset), slot, callee;
    slot = LLVMBuildInBoundsGEP2(ir->b, ir->t_i8, rt_pointer(ir), &index, 1, "");
    callee = ir_load(ir, ir->t_ptr, slot);
    return LLVMBuildCall2(ir->b, LLVMFunctionType(ret, params, count, 0), callee, args, count, "");
}

void ir_undef(SnxIr* ir, SnxU64 pc, SnxU32 word) {
    LLVMTypeRef params[3];
    LLVMValueRef args[3];
    params[0] = ir->t_ptr;
    params[1] = ir->t_i64;
    params[2] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir_i64(ir, pc);
    args[2] = ir_i32(ir, word);
    ir_rt_call(ir, ir_rt_undef(), ir->t_void, params, args, 3);
}

void ir_fence(SnxIr* ir) { LLVMBuildFence(ir->b, LLVMAtomicOrderingSequentiallyConsistent, 0, ""); }

/* CBAR(): asm volatile("" ::: "memory") */
static void compiler_barrier(SnxIr* ir) {
    LLVMTypeRef type = LLVMFunctionType(ir->t_void, NULL, 0, 0);
    LLVMValueRef barrier = LLVMGetInlineAsm(type, "", 0, "~{memory}", 9, 1, 0, LLVMInlineAsmDialectATT, 0);
    LLVMBuildCall2(ir->b, type, barrier, NULL, 0, "");
}
void ir_yield(SnxIr* ir) {
    /* cpu.h: __builtin_ia32_pause() under __x86_64__, the compiler barrier alone elsewhere */
    if (!strncmp(LLVMGetTarget(ir->mod), "x86_64", 6)) { /* PAUSE is baseline x86-64: no CPUID gate */
        static const char pause[] = "llvm.x86.sse2.pause";
        unsigned id = LLVMLookupIntrinsicID(pause, strlen(pause));
        LLVMValueRef function = LLVMGetIntrinsicDeclaration(ir->mod, id, NULL, 0);
        LLVMBuildCall2(ir->b, LLVMIntrinsicGetType(ir->ctx, id, NULL, 0), function, NULL, 0, "");
    }
    compiler_barrier(ir);
}

void ir_unlikely(SnxIr* ir, LLVMValueRef branch) {
    LLVMMetadataRef md[3];
    unsigned kind = LLVMGetMDKindIDInContext(ir->ctx, "prof", 4);
    md[0] = LLVMMDStringInContext2(ir->ctx, "branch_weights", 14);
    md[1] = LLVMValueAsMetadata(ir_i32(ir, 1));
    md[2] = LLVMValueAsMetadata(ir_i32(ir, 2000));
    LLVMSetMetadata(branch, kind, LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, md, 3)));
}

LLVMValueRef ir_intrinsic(SnxIr* ir, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned count) {
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    LLVMValueRef function = LLVMGetIntrinsicDeclaration(ir->mod, id, type ? &type : NULL, type ? 1 : 0);
    return LLVMBuildCall2(ir->b, LLVMIntrinsicGetType(ir->ctx, id, type ? &type : NULL, type ? 1 : 0), function, args,
                          count, "");
}

/* the "memory" attribute: 2 bits per location (argument memory, inaccessible memory, other), each none/read/write */
static void memory_effects(SnxIr* ir, LLVMValueRef function, unsigned value) {
    unsigned kind = LLVMGetEnumAttributeKindForName("memory", 6);
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ir->ctx, kind, value));
    kind = LLVMGetEnumAttributeKindForName("willreturn", 10);
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ir->ctx, kind, 0));
}
void ir_pure(SnxIr* ir, LLVMValueRef function) { memory_effects(ir, function, 0); }
void ir_argmem(SnxIr* ir, LLVMValueRef function) { memory_effects(ir, function, 3); }

LLVMBasicBlockRef ir_block(SnxIr* ir, const char* name) { return LLVMAppendBasicBlockInContext(ir->ctx, ir->fn, name); }

LLVMValueRef ir_local(SnxIr* ir, LLVMTypeRef type) {
    LLVMBasicBlockRef here = LLVMGetInsertBlock(ir->b), entry = LLVMGetEntryBasicBlock(ir->fn);
    LLVMValueRef first = LLVMGetFirstInstruction(entry), slot;
    if (first)
        LLVMPositionBuilderBefore(ir->b, first);
    else
        LLVMPositionBuilderAtEnd(ir->b, entry);
    slot = LLVMBuildAlloca(ir->b, type, "");
    LLVMSetAlignment(slot, 8);
    LLVMPositionBuilderAtEnd(ir->b, here);
    return slot;
}

LLVMTypeRef ir_v128(SnxIr* ir) { return LLVMArrayType(ir->t_i8, 16); }

static LLVMValueRef intrinsic(SnxIr* ir, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned count) {
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    LLVMValueRef function = LLVMGetIntrinsicDeclaration(ir->mod, id, &type, 1);
    return LLVMBuildCall2(ir->b, LLVMIntrinsicGetType(ir->ctx, id, &type, 1), function, args, count, "");
}

/* ---- cpu.h's inline helpers ----
   Each mirrors its C body. They are internal and always inlined, as SNX_INLINE asks, so a constant argument
   (a condition, a shift type, an access size) folds away as it does in the templates' C compiled with a constant word. */

typedef struct {
    LLVMBasicBlockRef block;
    LLVMValueRef fn, c, M;
} Saved;

static LLVMValueRef helper_begin(SnxIr* ir, const char* name, LLVMTypeRef ret, LLVMTypeRef* params, unsigned count,
                                 Saved* saved) {
    LLVMValueRef function = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ret, params, count, 0));
    LLVMSetLinkage(function, LLVMInternalLinkage);
    attribute(ir, function, LLVMAttributeFunctionIndex, "alwaysinline");
    attribute(ir, function, LLVMAttributeFunctionIndex, "nounwind");
    saved->block = LLVMGetInsertBlock(ir->b);
    saved->fn = ir->fn;
    saved->c = ir->c;
    saved->M = ir->M;
    ir->fn = function;
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, function, "entry"));
    return function;
}
static void helper_end(SnxIr* ir, Saved* saved) {
    ir->fn = saved->fn;
    ir->c = saved->c;
    ir->M = saved->M;
    if (saved->block) LLVMPositionBuilderAtEnd(ir->b, saved->block);
}
#define P(i) LLVMGetParam(function, i)
#define B    ir->b

static void store_flag(SnxIr* ir, LLVMValueRef value, size_t offset) { ir_cpu_store(ir, ir_u32(ir, value), offset); }

/* adds64/adds32(c,a,b,cin) */
static void build_adds(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, unsigned bits) {
    LLVMValueRef a = P(1), b = P(2), cin = P(3), r, cf, vf;
    r = LLVMBuildAdd(B, LLVMBuildAdd(B, a, b, ""), ir_cast(ir, cin, t, 0), "");
    store_flag(ir, LLVMBuildLShr(B, r, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, n));
    store_flag(ir, LLVMBuildZExt(B, LLVMBuildICmp(B, LLVMIntEQ, r, LLVMConstInt(t, 0, 0), ""), t, ""),
               offsetof(CPU, z));
    cf = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, cin, ir_i32(ir, 0), ""), LLVMBuildICmp(B, LLVMIntULE, r, a, ""),
                         LLVMBuildICmp(B, LLVMIntULT, r, a, ""), "");
    ir_cpu_store(ir, ir_bool32(ir, cf), offsetof(CPU, cf));
    vf = LLVMBuildAnd(B, LLVMBuildNot(B, LLVMBuildXor(B, a, b, ""), ""), LLVMBuildXor(B, a, r, ""), "");
    store_flag(ir, LLVMBuildLShr(B, vf, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, vf));
    LLVMBuildRet(B, r);
}
/* subs64/subs32(c,a,b) */
static void build_subs(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, unsigned bits) {
    LLVMValueRef a = P(1), b = P(2), r, vf;
    r = LLVMBuildSub(B, a, b, "");
    store_flag(ir, LLVMBuildLShr(B, r, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, n));
    ir_cpu_store(ir, ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, r, LLVMConstInt(t, 0, 0), "")), offsetof(CPU, z));
    ir_cpu_store(ir, ir_bool32(ir, LLVMBuildICmp(B, LLVMIntUGE, a, b, "")), offsetof(CPU, cf));
    vf = LLVMBuildAnd(B, LLVMBuildXor(B, a, b, ""), LLVMBuildXor(B, a, r, ""), "");
    store_flag(ir, LLVMBuildLShr(B, vf, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, vf));
    LLVMBuildRet(B, r);
}
/* logic64/logic32(c,r) */
static void build_logic(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, unsigned bits) {
    LLVMValueRef r = P(1);
    store_flag(ir, LLVMBuildLShr(B, r, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, n));
    ir_cpu_store(ir, ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, r, LLVMConstInt(t, 0, 0), "")), offsetof(CPU, z));
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, cf));
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, vf));
    LLVMBuildRetVoid(B);
}
/* cond_ok(c,cond) */
static void build_cond_ok(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef cond = P(1), n, z, cf, vf, k, r, nv, zero = ir_i32(ir, 0), result;
    LLVMBasicBlockRef always = ir_block(ir, "always"), test = ir_block(ir, "test");
    n = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, n));
    z = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, z));
    cf = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, cf));
    vf = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, vf));
    k = LLVMBuildLShr(B, cond, ir_i32(ir, 1), "");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 7), ""), always, test);
    LLVMPositionBuilderAtEnd(B, always);
    LLVMBuildRet(B, ir_i32(ir, 1));
    LLVMPositionBuilderAtEnd(B, test);
    nv = ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, n, vf, ""));
    /* case 6: !z && n==vf, case 5: n==vf, case 4: cf && !z, cases 3..0: vf, n, cf, z */
    r = ir_bool32(
        ir, LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntEQ, z, zero, ""), LLVMBuildICmp(B, LLVMIntEQ, n, vf, ""), ""));
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 5), ""), nv, r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 4), ""),
                        ir_bool32(ir, LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntNE, cf, zero, ""),
                                                   LLVMBuildICmp(B, LLVMIntEQ, z, zero, ""), "")),
                        r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 3), ""), vf, r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 2), ""), n, r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 1), ""), cf, r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 0), ""), z, r, "");
    result = LLVMBuildSelect(B, LLVMBuildTrunc(B, cond, ir->t_i1, ""),
                             ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, r, zero, "")), r, "");
    LLVMBuildRet(B, result);
}
/* set_nzcv(c,f) */
static void build_set_nzcv(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef f = P(1), one = ir_i32(ir, 1);
    ir_cpu_store(ir, LLVMBuildAnd(B, LLVMBuildLShr(B, f, ir_i32(ir, 3), ""), one, ""), offsetof(CPU, n));
    ir_cpu_store(ir, LLVMBuildAnd(B, LLVMBuildLShr(B, f, ir_i32(ir, 2), ""), one, ""), offsetof(CPU, z));
    ir_cpu_store(ir, LLVMBuildAnd(B, LLVMBuildLShr(B, f, one, ""), one, ""), offsetof(CPU, cf));
    ir_cpu_store(ir, LLVMBuildAnd(B, f, one, ""), offsetof(CPU, vf));
    LLVMBuildRetVoid(B);
}
/* ror64/ror32(x,s): s&=w-1; s ? (x>>s)|(x<<(w-s)) : x */
static void build_ror(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, unsigned bits) {
    LLVMValueRef x = P(0), s = LLVMBuildAnd(B, P(1), ir_i32(ir, bits - 1), ""), st, r;
    st = ir_cast(ir, s, t, 0);
    r = LLVMBuildOr(B, LLVMBuildLShr(B, x, st, ""),
                    LLVMBuildShl(B, x, LLVMBuildSub(B, LLVMConstInt(t, bits, 0), st, ""), ""), "");
    LLVMBuildRet(B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, s, ir_i32(ir, 0), ""), r, x, ""));
}
/* shf64/shf32(x,t,s) */
static void build_shf(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, int ror) {
    LLVMValueRef x = P(0), type = P(1), s = ir_cast(ir, P(2), t, 0), args[2], r;
    args[0] = x;
    args[1] = P(2);
    r = ir_call(ir, ir_helper(ir, ror), args, 2);
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, type, ir_i32(ir, 2), ""), LLVMBuildAShr(B, x, s, ""), r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, type, ir_i32(ir, 1), ""), LLVMBuildLShr(B, x, s, ""), r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, type, ir_i32(ir, 0), ""), LLVMBuildShl(B, x, s, ""), r, "");
    LLVMBuildRet(B, r);
}
/* ext64(x,opt,sh) */
static void build_ext64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef x = P(0), opt = P(1), r = x;
    static const struct {
        unsigned opt, bits;
        int is_signed;
    } kinds[] = {{6, 32, 1}, {5, 16, 1}, {4, 8, 1}, {2, 32, 0}, {1, 16, 0}, {0, 8, 0}};
    size_t i;
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        LLVMValueRef narrow = LLVMBuildTrunc(B, x, int_type(ir, kinds[i].bits), "");
        r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, opt, ir_i32(ir, kinds[i].opt), ""),
                            ir_cast(ir, narrow, ir->t_i64, kinds[i].is_signed), r, "");
    }
    LLVMBuildRet(B, LLVMBuildShl(B, r, ir_u64(ir, P(2)), ""));
}
/* clz64/clz32(x): x ? __builtin_clz(x) : width, which is ctlz defined at zero */
static void build_clz(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t) {
    LLVMValueRef args[2];
    args[0] = P(0);
    args[1] = LLVMConstInt(ir->t_i1, 0, 0);
    LLVMBuildRet(B, ir_u32(ir, intrinsic(ir, "llvm.ctlz", t, args, 2)));
}
/* cls64/cls32(x): clz(x ^ (x >> 1 arithmetic)) - 1 */
static void build_cls(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, int clz) {
    LLVMValueRef x = P(0), arg;
    arg = LLVMBuildXor(B, x, LLVMBuildAShr(B, x, LLVMConstInt(t, 1, 0), ""), "");
    LLVMBuildRet(B, LLVMBuildSub(B, ir_call(ir, ir_helper(ir, clz), &arg, 1), ir_i32(ir, 1), ""));
}
static LLVMValueRef swap_pairs(SnxIr* ir, LLVMValueRef x, unsigned shift, SnxU64 mask) {
    LLVMValueRef m = ir_i64(ir, mask), s = ir_i64(ir, shift);
    return LLVMBuildOr(B, LLVMBuildAnd(B, LLVMBuildLShr(B, x, s, ""), m, ""),
                       LLVMBuildShl(B, LLVMBuildAnd(B, x, m, ""), s, ""), "");
}
/* rbit64(x): cpu.h swaps bits, pairs and nibbles, then bswap64 - a bit reversal, which LLVM lowers to the host's
   best sequence (GF2P8AFFINEQB and BSWAP on x86 with GFNI, RBIT on AArch64) */
static void build_rbit64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef x = P(0);
    if (ir->features->x86 && ir->features->gfni) {
        /* wunkolo, "gf2p8affineqb: Bit reversal": the matrix 0x8040201008040201 reverses each byte's bits, BSWAP the
           bytes. Explicit, whatever the LLVM version lowers llvm.bitreverse to */
        LLVMTypeRef v2 = LLVMVectorType(ir->t_i64, 2), v16 = LLVMVectorType(ir->t_i8, 16);
        LLVMValueRef bytes[16], args[3], v;
        unsigned i;
        static const unsigned char matrix[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
        for (i = 0; i < 16; ++i) bytes[i] = ir_i8(ir, matrix[i & 7]);
        v = LLVMBuildInsertElement(B, LLVMGetUndef(v2), x, ir_i32(ir, 0), "");
        args[0] = LLVMBuildBitCast(B, v, v16, "");
        args[1] = LLVMConstVector(bytes, 16);
        args[2] = ir_i8(ir, 0);
        v = LLVMBuildBitCast(B, ir_intrinsic(ir, "llvm.x86.vgf2p8affineqb.128", NULL, args, 3), v2, "");
        x = LLVMBuildExtractElement(B, v, ir_i32(ir, 0), "");
        LLVMBuildRet(B, intrinsic(ir, "llvm.bswap", ir->t_i64, &x, 1));
        return;
    }
    LLVMBuildRet(B, intrinsic(ir, "llvm.bitreverse", ir->t_i64, &x, 1));
}
/* rbit32(x): (u32)(rbit64(x) >> 32) */
static void build_rbit32(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef x = ir_u64(ir, P(0));
    x = ir_call(ir, ir_helper(ir, SNX_IR_RBIT64), &x, 1);
    LLVMBuildRet(B, ir_u32(ir, LLVMBuildLShr(B, x, ir_i64(ir, 32), "")));
}
static void build_rev16_64(SnxIr* ir, LLVMValueRef function) {
    LLVMBuildRet(B, swap_pairs(ir, P(0), 8, ((SnxU64)0x00FF00FFUL << 32) | 0x00FF00FFUL));
}
static void build_rev32_64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef x = P(0), y = ir_call(ir, ir_helper(ir, SNX_IR_BSWAP64), &x, 1);
    LLVMBuildRet(B,
                 LLVMBuildOr(B, LLVMBuildLShr(B, y, ir_i64(ir, 32), ""), LLVMBuildShl(B, y, ir_i64(ir, 32), ""), ""));
}
static void build_bswap(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t) {
    LLVMValueRef x = P(0);
    LLVMBuildRet(B, intrinsic(ir, "llvm.bswap", t, &x, 1));
}
/* udiv(a,b): b ? a/b : 0 */
static void build_udiv(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t) {
    LLVMValueRef a = P(0), b = P(1), zero = LLVMConstInt(t, 0, 0);
    LLVMBasicBlockRef divide = ir_block(ir, "divide"), none = ir_block(ir, "zero");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntNE, b, zero, ""), divide, none);
    LLVMPositionBuilderAtEnd(B, divide);
    LLVMBuildRet(B, LLVMBuildUDiv(B, a, b, ""));
    LLVMPositionBuilderAtEnd(B, none);
    LLVMBuildRet(B, zero);
}
/* sdiv(a,b): !b -> 0, b==-1 -> 0-a (no INT_MIN/-1 trap), else a/b */
static void build_sdiv(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t) {
    LLVMValueRef a = P(0), b = P(1), zero = LLVMConstInt(t, 0, 0);
    LLVMBasicBlockRef none = ir_block(ir, "zero"), test = ir_block(ir, "test"), negate = ir_block(ir, "negate"),
                      divide = ir_block(ir, "divide");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, b, zero, ""), none, test);
    LLVMPositionBuilderAtEnd(B, none);
    LLVMBuildRet(B, zero);
    LLVMPositionBuilderAtEnd(B, test);
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, b, LLVMConstAllOnes(t), ""), negate, divide);
    LLVMPositionBuilderAtEnd(B, negate);
    LLVMBuildRet(B, LLVMBuildSub(B, zero, a, ""));
    LLVMPositionBuilderAtEnd(B, divide);
    LLVMBuildRet(B, LLVMBuildSDiv(B, a, b, ""));
}
/* umulh64(a,b): (u64)(((u128)a * b) >> 64) */
static void build_umulh64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = ir_cast(ir, P(0), ir->t_i128, 0), b = ir_cast(ir, P(1), ir->t_i128, 0), r;
    r = LLVMBuildLShr(B, LLVMBuildMul(B, a, b, ""), LLVMConstInt(ir->t_i128, 64, 0), "");
    LLVMBuildRet(B, LLVMBuildTrunc(B, r, ir->t_i64, ""));
}
/* smulh64(a,b): umulh64, minus b when a < 0, minus a when b < 0 */
static void build_smulh64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(0), b = P(1), args[2], r, zero = ir_i64(ir, 0);
    args[0] = a;
    args[1] = b;
    r = ir_call(ir, ir_helper(ir, SNX_IR_UMULH64), args, 2);
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, a, zero, ""), LLVMBuildSub(B, r, b, ""), r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, b, zero, ""), LLVMBuildSub(B, r, a, ""), r, "");
    LLVMBuildRet(B, r);
}
/* bmask(n): n >= 64 ? ~0 : (1<<n)-1 */
static void build_bmask(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef n = P(0), mask;
    mask = LLVMBuildSub(B, LLVMBuildShl(B, ir_i64(ir, 1), ir_u64(ir, n), ""), ir_i64(ir, 1), "");
    LLVMBuildRet(
        B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntUGE, n, ir_i32(ir, 64), ""), ir_i64(ir, ~(SnxU64)0), mask, ""));
}
static LLVMValueRef call_bmask(SnxIr* ir, LLVMValueRef n) { return ir_call(ir, ir_helper(ir, SNX_IR_BMASK), &n, 1); }
/* ubfm_(src,immr,imms,ds) */
static void build_ubfm(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef src = P(0), immr = P(1), imms = P(2), ds = P(3), one = ir_i32(ir, 1);
    LLVMBasicBlockRef low = ir_block(ir, "low"), high = ir_block(ir, "high");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntUGE, imms, immr, ""), low, high);
    LLVMPositionBuilderAtEnd(B, low);
    LLVMBuildRet(B, LLVMBuildAnd(B, LLVMBuildLShr(B, src, ir_u64(ir, immr), ""),
                                 call_bmask(ir, LLVMBuildAdd(B, LLVMBuildSub(B, imms, immr, ""), one, "")), ""));
    LLVMPositionBuilderAtEnd(B, high);
    LLVMBuildRet(B, LLVMBuildShl(B, LLVMBuildAnd(B, src, call_bmask(ir, LLVMBuildAdd(B, imms, one, "")), ""),
                                 ir_u64(ir, LLVMBuildSub(B, ds, immr, "")), ""));
}
/* sbfm_(src,immr,imms,ds) */
static void build_sbfm(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef src = P(0), immr = P(1), imms = P(2), ds = P(3), one = ir_i32(ir, 1), r1, r2, top1, top2, phis[2];
    LLVMValueRef r, top, values[2];
    LLVMBasicBlockRef low = ir_block(ir, "low"), high = ir_block(ir, "high"), join = ir_block(ir, "join"),
                      sign = ir_block(ir, "sign"), done = ir_block(ir, "done"), blocks[2];
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntUGE, imms, immr, ""), low, high);
    LLVMPositionBuilderAtEnd(B, low);
    r1 = LLVMBuildAnd(B, LLVMBuildLShr(B, src, ir_u64(ir, immr), ""),
                      call_bmask(ir, LLVMBuildAdd(B, LLVMBuildSub(B, imms, immr, ""), one, "")), "");
    top1 = LLVMBuildSub(B, imms, immr, "");
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, high);
    r2 = LLVMBuildShl(B, LLVMBuildAnd(B, src, call_bmask(ir, LLVMBuildAdd(B, imms, one, "")), ""),
                      ir_u64(ir, LLVMBuildSub(B, ds, immr, "")), "");
    top2 = LLVMBuildAdd(B, LLVMBuildSub(B, ds, immr, ""), imms, "");
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, join);
    blocks[0] = low;
    blocks[1] = high;
    r = LLVMBuildPhi(B, ir->t_i64, "");
    values[0] = r1;
    values[1] = r2;
    LLVMAddIncoming(r, values, blocks, 2);
    top = LLVMBuildPhi(B, ir->t_i32, "");
    values[0] = top1;
    values[1] = top2;
    LLVMAddIncoming(top, values, blocks, 2);
    LLVMBuildCondBr(B, LLVMBuildTrunc(B, LLVMBuildLShr(B, r, ir_u64(ir, top), ""), ir->t_i1, ""), sign, done);
    LLVMPositionBuilderAtEnd(B, sign);
    r1 = LLVMBuildOr(B, r, LLVMBuildNot(B, call_bmask(ir, LLVMBuildAdd(B, top, one, "")), ""), "");
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
    phis[0] = LLVMBuildPhi(B, ir->t_i64, "");
    values[0] = r;
    values[1] = r1;
    blocks[0] = join;
    blocks[1] = sign;
    LLVMAddIncoming(phis[0], values, blocks, 2);
    LLVMBuildRet(B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, ds, ir_i32(ir, 32), ""),
                                    ir_u64(ir, ir_u32(ir, phis[0])), phis[0], ""));
}
/* bfm_(dst,src,immr,imms,ds) */
static void build_bfm(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef dst = P(0), src = P(1), immr = P(2), imms = P(3), ds = P(4), one = ir_i32(ir, 1), m, r1, r2, r,
                 values[2];
    LLVMBasicBlockRef low = ir_block(ir, "low"), high = ir_block(ir, "high"), join = ir_block(ir, "join"), blocks[2];
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntUGE, imms, immr, ""), low, high);
    LLVMPositionBuilderAtEnd(B, low);
    m = call_bmask(ir, LLVMBuildAdd(B, LLVMBuildSub(B, imms, immr, ""), one, ""));
    r1 = LLVMBuildOr(B, LLVMBuildAnd(B, dst, LLVMBuildNot(B, m, ""), ""),
                     LLVMBuildAnd(B, LLVMBuildLShr(B, src, ir_u64(ir, immr), ""), m, ""), "");
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, high);
    {
        LLVMValueRef p = ir_u64(ir, LLVMBuildSub(B, ds, immr, ""));
        m = LLVMBuildShl(B, call_bmask(ir, LLVMBuildAdd(B, imms, one, "")), p, "");
        r2 = LLVMBuildOr(B, LLVMBuildAnd(B, dst, LLVMBuildNot(B, m, ""), ""),
                         LLVMBuildAnd(B, LLVMBuildShl(B, src, p, ""), m, ""), "");
    }
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, join);
    r = LLVMBuildPhi(B, ir->t_i64, "");
    values[0] = r1;
    values[1] = r2;
    blocks[0] = low;
    blocks[1] = high;
    LLVMAddIncoming(r, values, blocks, 2);
    LLVMBuildRet(
        B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, ds, ir_i32(ir, 32), ""), ir_u64(ir, ir_u32(ir, r)), r, ""));
}

/* a switch over an access size 0..3 (u8..u64): one block per size, joined with a phi of i64 results */
typedef struct {
    LLVMBasicBlockRef blocks[4], join;
    LLVMValueRef values[4];
} SizeSwitch;
static void size_switch(SnxIr* ir, LLVMValueRef size, SizeSwitch* s) {
    LLVMValueRef sw;
    unsigned i;
    for (i = 0; i < 4; ++i) s->blocks[i] = ir_block(ir, "size");
    s->join = ir_block(ir, "join");
    sw = LLVMBuildSwitch(B, size, s->blocks[3], 3); /* default: the 64-bit case */
    for (i = 0; i < 3; ++i) LLVMAddCase(sw, ir_i32(ir, i), s->blocks[i]);
}
static LLVMValueRef size_join(SnxIr* ir, SizeSwitch* s, LLVMTypeRef type) {
    LLVMValueRef phi;
    LLVMPositionBuilderAtEnd(B, s->join);
    phi = LLVMBuildPhi(B, type, "");
    LLVMAddIncoming(phi, s->values, s->blocks, 4);
    return phi;
}
/* ldx(c,M,a,sz) */
static void build_ldx(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(2), sz = P(3), v;
    SizeSwitch s;
    unsigned i;
    ir->c = P(0);
    ir->M = P(1);
    size_switch(ir, sz, &s);
    for (i = 0; i < 4; ++i) {
        LLVMPositionBuilderAtEnd(B, s.blocks[i]);
        s.values[i] = ir_u64(ir, ir_ldacq(ir, a, 8u << i));
        LLVMBuildBr(B, s.join);
    }
    v = size_join(ir, &s, ir->t_i64);
    ir_cpu_store(ir, a, offsetof(CPU, excl_addr));
    ir_cpu_store(ir, v, offsetof(CPU, excl_val));
    ir_cpu_store(ir, sz, offsetof(CPU, excl_size));
    ir_cpu_store(ir, ir_i32(ir, 1), offsetof(CPU, excl_on));
    LLVMBuildRet(B, v);
}
/* snx_cas8..64(p,o,n): __atomic_compare_exchange_n, strong, seq_cst */
static LLVMValueRef cas(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef expected, LLVMValueRef value) {
    LLVMTypeRef t = int_type(ir, bits);
    LLVMValueRef x =
        LLVMBuildAtomicCmpXchg(B, ir_mp(ir, address), ir_cast(ir, expected, t, 0), ir_cast(ir, value, t, 0),
                               LLVMAtomicOrderingSequentiallyConsistent, LLVMAtomicOrderingSequentiallyConsistent, 0);
    LLVMSetAlignment(x, bits / 8);
    return ir_bool32(ir, LLVMBuildExtractValue(B, x, 1, ""));
}
static LLVMValueRef exclusive_match(SnxIr* ir, LLVMValueRef a, LLVMValueRef size) {
    LLVMValueRef on = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, excl_on));
    LLVMValueRef address = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_addr));
    LLVMValueRef have = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, excl_size));
    return LLVMBuildAnd(B,
                        LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntNE, on, ir_i32(ir, 0), ""),
                                     LLVMBuildICmp(B, LLVMIntEQ, address, a, ""), ""),
                        LLVMBuildICmp(B, LLVMIntEQ, have, size, ""), "");
}
/* stx(c,M,a,sz,v) */
static void build_stx(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(2), sz = P(3), v = P(4), ok, values[2], expected;
    LLVMBasicBlockRef start, attempt = NULL, finish, blocks[2];
    SizeSwitch s;
    unsigned i;
    ir->c = P(0);
    ir->M = P(1);
    start = LLVMGetInsertBlock(B);
    attempt = ir_block(ir, "attempt");
    finish = ir_block(ir, "finish");
    LLVMBuildCondBr(B, exclusive_match(ir, a, sz), attempt, finish);
    LLVMPositionBuilderAtEnd(B, attempt);
    size_switch(ir, sz, &s);
    for (i = 0; i < 4; ++i) {
        LLVMPositionBuilderAtEnd(B, s.blocks[i]);
        expected = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_val));
        s.values[i] = cas(ir, a, 8u << i, expected, v);
        LLVMBuildBr(B, s.join);
    }
    ok = size_join(ir, &s, ir->t_i32);
    LLVMBuildBr(B, finish);
    LLVMPositionBuilderAtEnd(B, finish);
    values[0] = ir_i32(ir, 0);
    values[1] = ok;
    blocks[0] = start;
    blocks[1] = s.join;
    ok = LLVMBuildPhi(B, ir->t_i32, "");
    LLVMAddIncoming(ok, values, blocks, 2);
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, excl_on));
    LLVMBuildRet(
        B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, ok, ir_i32(ir, 0), ""), ir_i32(ir, 0), ir_i32(ir, 1), ""));
}
/* ldxp(c,M,a,sz,&v1,&v2) */
static void build_ldxp(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(2), sz = P(3), v1 = P(4), v2 = P(5), t, first, second;
    LLVMBasicBlockRef pair32 = ir_block(ir, "pair32"), pair64 = ir_block(ir, "pair64"), done = ir_block(ir, "done");
    ir->c = P(0);
    ir->M = P(1);
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, sz, ir_i32(ir, 2), ""), pair32, pair64);
    LLVMPositionBuilderAtEnd(B, pair32);
    t = ir_ldacq(ir, a, 64);
    ir_store(ir, ir_u64(ir, ir_u32(ir, t)), v1);
    ir_store(ir, LLVMBuildLShr(B, t, ir_i64(ir, 32), ""), v2);
    ir_cpu_store(ir, t, offsetof(CPU, excl_val));
    ir_cpu_store(ir, ir_i64(ir, 0), offsetof(CPU, excl_val2));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, pair64);
    first = ir_ldacq(ir, a, 64);
    ir_store(ir, first, v1);
    second = ir_ldacq(ir, LLVMBuildAdd(B, a, ir_i64(ir, 8), ""), 64);
    ir_store(ir, second, v2);
    ir_cpu_store(ir, ir_load(ir, ir->t_i64, v1), offsetof(CPU, excl_val));
    ir_cpu_store(ir, ir_load(ir, ir->t_i64, v2), offsetof(CPU, excl_val2));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
    ir_cpu_store(ir, a, offsetof(CPU, excl_addr));
    ir_cpu_store(ir, LLVMBuildOr(B, sz, ir_i32(ir, 0x10), ""), offsetof(CPU, excl_size));
    ir_cpu_store(ir, ir_i32(ir, 1), offsetof(CPU, excl_on));
    LLVMBuildRetVoid(B);
}
/* stxp(c,M,a,sz,v1,v2) */
static void build_stxp(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(2), sz = P(3), v1 = P(4), v2 = P(5), ok32, ok128, ok, values[3], args[5], packed;
    LLVMTypeRef params[5];
    LLVMBasicBlockRef start, attempt = ir_block(ir, "attempt"), pair32 = ir_block(ir, "pair32"),
                             pair64 = ir_block(ir, "pair64"), finish = ir_block(ir, "finish"), blocks[3];
    ir->c = P(0);
    ir->M = P(1);
    start = LLVMGetInsertBlock(B);
    LLVMBuildCondBr(B, exclusive_match(ir, a, LLVMBuildOr(B, sz, ir_i32(ir, 0x10), "")), attempt, finish);
    LLVMPositionBuilderAtEnd(B, attempt);
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, sz, ir_i32(ir, 2), ""), pair32, pair64);
    LLVMPositionBuilderAtEnd(B, pair32);
    packed = LLVMBuildOr(B, ir_u64(ir, ir_u32(ir, v1)), LLVMBuildShl(B, v2, ir_i64(ir, 32), ""), "");
    ok32 = cas(ir, a, 64, ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_val)), packed);
    LLVMBuildBr(B, finish);
    LLVMPositionBuilderAtEnd(B, pair64);
    params[0] = ir->t_ptr;
    params[1] = params[2] = params[3] = params[4] = ir->t_i64;
    args[0] = ir_mp(ir, a);
    args[1] = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_val));
    args[2] = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_val2));
    args[3] = v1;
    args[4] = v2;
    ok128 = ir_call(ir, ir_extern(ir, "snx_cas128", ir->t_i32, params, 5), args, 5);
    LLVMBuildBr(B, finish);
    LLVMPositionBuilderAtEnd(B, finish);
    ok = LLVMBuildPhi(B, ir->t_i32, "");
    values[0] = ir_i32(ir, 0);
    values[1] = ok32;
    values[2] = ok128;
    blocks[0] = start;
    blocks[1] = pair32;
    blocks[2] = pair64;
    LLVMAddIncoming(ok, values, blocks, 3);
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, excl_on));
    LLVMBuildRet(
        B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, ok, ir_i32(ir, 0), ""), ir_i32(ir, 0), ir_i32(ir, 1), ""));
}
/* snx_lookup(c,pc) */
static void build_lookup(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef pc = P(1), lo, hi, o, inside, f, index, entry;
    LLVMBasicBlockRef table = ir_block(ir, "table"), found = ir_block(ir, "found"), none = ir_block(ir, "none");
    ir->c = P(0);
    lo = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, code_lo));
    hi = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, code_hi));
    o = LLVMBuildSub(B, pc, lo, "");
    inside = LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntULT, o, LLVMBuildSub(B, hi, lo, ""), ""),
                          LLVMBuildICmp(B, LLVMIntEQ, LLVMBuildAnd(B, pc, ir_i64(ir, 3), ""), ir_i64(ir, 0), ""), "");
    LLVMBuildCondBr(B, inside, table, none);
    LLVMPositionBuilderAtEnd(B, table);
    index = LLVMBuildLShr(B, o, ir_i64(ir, 2), "");
    f = ir_load(ir, ir->t_ptr,
                LLVMBuildGEP2(B, ir->t_ptr, ir_cpu_load(ir, ir->t_ptr, offsetof(CPU, disp)), &index, 1, ""));
    LLVMBuildCondBr(B, LLVMBuildIsNotNull(B, f, ""), found, none);
    LLVMPositionBuilderAtEnd(B, found);
    LLVMBuildRet(B, f);
    LLVMPositionBuilderAtEnd(B, none);
    entry = LLVMGetNamedGlobal(ir->mod, "snx_interp_entry");
    if (!entry) {
        entry = LLVMAddGlobal(ir->mod, ir->t_ptr, "snx_interp_entry");
        LLVMSetVisibility(entry, LLVMHiddenVisibility);
    }
    LLVMBuildRet(B, ir_load(ir, ir->t_ptr, entry));
}
/* dc_zva(M,a): memset(MP(a & ~63), 0, 64) */
static void build_dc_zva(SnxIr* ir, LLVMValueRef function) {
    ir->M = P(0);
    {
        LLVMValueRef a = LLVMBuildAnd(B, P(1), ir_i64(ir, ~(SnxU64)63), ""),
                     zero = LLVMConstNull(LLVMVectorType(ir->t_i8, 16));
        unsigned i;
        for (i = 0; i < 4; ++i) ir_wr_bytes(ir, LLVMBuildAdd(B, a, ir_i64(ir, 16 * i), ""), zero);
    }
    LLVMBuildRetVoid(B);
}
/* vfp_imm32(i) / vfp_imm64(i) */
static void build_vfp_imm(SnxIr* ir, LLVMValueRef function, int wide) {
    LLVMTypeRef t = wide ? ir->t_i64 : ir->t_i32;
    LLVMValueRef i = ir_cast(ir, P(0), t, 0), b6, e, r;
#define K(v) LLVMConstInt(t, v, 0)
    b6 = LLVMBuildAnd(B, LLVMBuildLShr(B, i, K(6), ""), K(1), "");
    e = LLVMBuildOr(B, LLVMBuildShl(B, LLVMBuildXor(B, b6, K(1), ""), K(wide ? 10 : 7), ""),
                    LLVMBuildOr(B,
                                LLVMBuildShl(B,
                                             LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, b6, K(0), ""),
                                                             K(wide ? 0xFF : 0x1F), K(0), ""),
                                             K(2), ""),
                                LLVMBuildAnd(B, LLVMBuildLShr(B, i, K(4), ""), K(3), ""), ""),
                    "");
    r = LLVMBuildOr(B, LLVMBuildShl(B, LLVMBuildLShr(B, i, K(7), ""), K(wide ? 63 : 31), ""),
                    LLVMBuildOr(B, LLVMBuildShl(B, e, K(wide ? 52 : 23), ""),
                                LLVMBuildShl(B, LLVMBuildAnd(B, i, K(15), ""), K(wide ? 48 : 19), ""), ""),
                    "");
#undef K
    LLVMBuildRet(B, r);
}

LLVMValueRef ir_helper(SnxIr* ir, int id) {
    LLVMTypeRef i32 = ir->t_i32, i64 = ir->t_i64, ptr = ir->t_ptr, p[6];
    LLVMValueRef function;
    Saved saved;
    if (ir->helpers[id]) return ir->helpers[id];
    switch (id) {
#define BEGIN(name, ret, n) function = helper_begin(ir, name, ret, p, n, &saved)
        case SNX_IR_ADDS64:
            p[0] = ptr, p[1] = i64, p[2] = i64, p[3] = i32;
            BEGIN("adds64", i64, 4);
            ir->c = P(0);
            build_adds(ir, function, i64, 64);
            break;
        case SNX_IR_ADDS32:
            p[0] = ptr, p[1] = i32, p[2] = i32, p[3] = i32;
            BEGIN("adds32", i32, 4);
            ir->c = P(0);
            build_adds(ir, function, i32, 32);
            break;
        case SNX_IR_SUBS64:
            p[0] = ptr, p[1] = i64, p[2] = i64;
            BEGIN("subs64", i64, 3);
            ir->c = P(0);
            build_subs(ir, function, i64, 64);
            break;
        case SNX_IR_SUBS32:
            p[0] = ptr, p[1] = i32, p[2] = i32;
            BEGIN("subs32", i32, 3);
            ir->c = P(0);
            build_subs(ir, function, i32, 32);
            break;
        case SNX_IR_LOGIC64:
            p[0] = ptr, p[1] = i64;
            BEGIN("logic64", ir->t_void, 2);
            ir->c = P(0);
            build_logic(ir, function, i64, 64);
            break;
        case SNX_IR_LOGIC32:
            p[0] = ptr, p[1] = i32;
            BEGIN("logic32", ir->t_void, 2);
            ir->c = P(0);
            build_logic(ir, function, i32, 32);
            break;
        case SNX_IR_COND_OK:
            p[0] = ptr, p[1] = i32;
            BEGIN("cond_ok", i32, 2);
            ir->c = P(0);
            build_cond_ok(ir, function);
            break;
        case SNX_IR_SET_NZCV:
            p[0] = ptr, p[1] = i32;
            BEGIN("set_nzcv", ir->t_void, 2);
            ir->c = P(0);
            build_set_nzcv(ir, function);
            break;
        case SNX_IR_ROR64:
            p[0] = i64, p[1] = i32;
            BEGIN("ror64", i64, 2);
            build_ror(ir, function, i64, 64);
            break;
        case SNX_IR_ROR32:
            p[0] = i32, p[1] = i32;
            BEGIN("ror32", i32, 2);
            build_ror(ir, function, i32, 32);
            break;
        case SNX_IR_SHF64:
            p[0] = i64, p[1] = i32, p[2] = i32;
            BEGIN("shf64", i64, 3);
            build_shf(ir, function, i64, SNX_IR_ROR64);
            break;
        case SNX_IR_SHF32:
            p[0] = i32, p[1] = i32, p[2] = i32;
            BEGIN("shf32", i32, 3);
            build_shf(ir, function, i32, SNX_IR_ROR32);
            break;
        case SNX_IR_EXT64:
            p[0] = i64, p[1] = i32, p[2] = i32;
            BEGIN("ext64", i64, 3);
            build_ext64(ir, function);
            break;
        case SNX_IR_CLZ64:
            p[0] = i64;
            BEGIN("clz64", i32, 1);
            build_clz(ir, function, i64);
            break;
        case SNX_IR_CLZ32:
            p[0] = i32;
            BEGIN("clz32", i32, 1);
            build_clz(ir, function, i32);
            break;
        case SNX_IR_CLS64:
            p[0] = i64;
            BEGIN("cls64", i32, 1);
            build_cls(ir, function, i64, SNX_IR_CLZ64);
            break;
        case SNX_IR_CLS32:
            p[0] = i32;
            BEGIN("cls32", i32, 1);
            build_cls(ir, function, i32, SNX_IR_CLZ32);
            break;
        case SNX_IR_RBIT64:
            p[0] = i64;
            BEGIN("rbit64", i64, 1);
            build_rbit64(ir, function);
            break;
        case SNX_IR_RBIT32:
            p[0] = i32;
            BEGIN("rbit32", i32, 1);
            build_rbit32(ir, function);
            break;
        case SNX_IR_REV16_64:
            p[0] = i64;
            BEGIN("rev16_64", i64, 1);
            build_rev16_64(ir, function);
            break;
        case SNX_IR_REV32_64:
            p[0] = i64;
            BEGIN("rev32_64", i64, 1);
            build_rev32_64(ir, function);
            break;
        case SNX_IR_BSWAP32:
            p[0] = i32;
            BEGIN("bswap32", i32, 1);
            build_bswap(ir, function, i32);
            break;
        case SNX_IR_BSWAP64:
            p[0] = i64;
            BEGIN("bswap64", i64, 1);
            build_bswap(ir, function, i64);
            break;
        case SNX_IR_UDIV64:
            p[0] = i64, p[1] = i64;
            BEGIN("udiv64", i64, 2);
            build_udiv(ir, function, i64);
            break;
        case SNX_IR_UDIV32:
            p[0] = i32, p[1] = i32;
            BEGIN("udiv32", i32, 2);
            build_udiv(ir, function, i32);
            break;
        case SNX_IR_SDIV64:
            p[0] = i64, p[1] = i64;
            BEGIN("sdiv64", i64, 2);
            build_sdiv(ir, function, i64);
            break;
        case SNX_IR_SDIV32:
            p[0] = i32, p[1] = i32;
            BEGIN("sdiv32", i32, 2);
            build_sdiv(ir, function, i32);
            break;
        case SNX_IR_UMULH64:
            p[0] = i64, p[1] = i64;
            BEGIN("umulh64", i64, 2);
            build_umulh64(ir, function);
            break;
        case SNX_IR_SMULH64:
            p[0] = i64, p[1] = i64;
            BEGIN("smulh64", i64, 2);
            build_smulh64(ir, function);
            break;
        case SNX_IR_BMASK:
            p[0] = i32;
            BEGIN("bmask", i64, 1);
            build_bmask(ir, function);
            break;
        case SNX_IR_UBFM:
            p[0] = i64, p[1] = i32, p[2] = i32, p[3] = i32;
            BEGIN("ubfm_", i64, 4);
            build_ubfm(ir, function);
            break;
        case SNX_IR_SBFM:
            p[0] = i64, p[1] = i32, p[2] = i32, p[3] = i32;
            BEGIN("sbfm_", i64, 4);
            build_sbfm(ir, function);
            break;
        case SNX_IR_BFM:
            p[0] = i64, p[1] = i64, p[2] = i32, p[3] = i32, p[4] = i32;
            BEGIN("bfm_", i64, 5);
            build_bfm(ir, function);
            break;
        case SNX_IR_LDX:
            p[0] = ptr, p[1] = ptr, p[2] = i64, p[3] = i32;
            BEGIN("ldx", i64, 4);
            build_ldx(ir, function);
            break;
        case SNX_IR_STX:
            p[0] = ptr, p[1] = ptr, p[2] = i64, p[3] = i32, p[4] = i64;
            BEGIN("stx", i32, 5);
            build_stx(ir, function);
            break;
        case SNX_IR_LDXP:
            p[0] = ptr, p[1] = ptr, p[2] = i64, p[3] = i32, p[4] = ptr, p[5] = ptr;
            BEGIN("ldxp", ir->t_void, 6);
            build_ldxp(ir, function);
            break;
        case SNX_IR_STXP:
            p[0] = ptr, p[1] = ptr, p[2] = i64, p[3] = i32, p[4] = i64, p[5] = i64;
            BEGIN("stxp", i32, 6);
            build_stxp(ir, function);
            break;
        case SNX_IR_LOOKUP:
            p[0] = ptr, p[1] = i64;
            BEGIN("snx_lookup", ptr, 2);
            build_lookup(ir, function);
            break;
        case SNX_IR_DC_ZVA:
            p[0] = ptr, p[1] = i64;
            BEGIN("dc_zva", ir->t_void, 2);
            build_dc_zva(ir, function);
            break;
        case SNX_IR_VFP_IMM32:
            p[0] = i32;
            BEGIN("vfp_imm32", i32, 1);
            build_vfp_imm(ir, function, 0);
            break;
        case SNX_IR_VFP_IMM64:
            p[0] = i32;
            BEGIN("vfp_imm64", i64, 1);
            build_vfp_imm(ir, function, 1);
            break;
        default: return NULL;
#undef BEGIN
    }
    helper_end(ir, &saved);
    ir->helpers[id] = function;
    return function;
}

LLVMValueRef ir_call_helper(SnxIr* ir, int id, LLVMValueRef* args, unsigned count) {
    return ir_call(ir, ir_helper(ir, id), args, count);
}
