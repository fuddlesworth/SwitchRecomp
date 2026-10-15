/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "llvm.h"
#include "simd.h"
#include <limits.h>

/* All guest arithmetic is i32 without nsw/nuw. In particular address arithmetic
   wraps BEFORE extending to a host pointer. Never use inbounds GEP for guests. */
typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef b;
    LLVMValueRef fn, cpu, mem;
    LLVMTypeRef i1, i8, i16, i32, i64, ptr;
    uint32_t pc, word;
} ArmIR;

static LLVMValueRef c32(ArmIR* a, uint32_t n) { return LLVMConstInt(a->i32, n, 0); }
static LLVMValueRef c64(ArmIR* a, uint64_t n) { return LLVMConstInt(a->i64, n, 0); }
static LLVMValueRef add(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildAdd(a->b, x, y, ""); }
static LLVMValueRef sub(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildSub(a->b, x, y, ""); }
static LLVMValueRef band(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildAnd(a->b, x, y, ""); }
static LLVMValueRef bor(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildOr(a->b, x, y, ""); }
static LLVMValueRef bxor(ArmIR* a, LLVMValueRef x, LLVMValueRef y) { return LLVMBuildXor(a->b, x, y, ""); }
static LLVMValueRef shl(ArmIR* a, LLVMValueRef x, unsigned n) { return LLVMBuildShl(a->b, x, c32(a, n), ""); }
static LLVMValueRef lsr(ArmIR* a, LLVMValueRef x, unsigned n) { return LLVMBuildLShr(a->b, x, c32(a, n), ""); }
static LLVMValueRef cmp(ArmIR* a, LLVMIntPredicate p, LLVMValueRef x, LLVMValueRef y) {
    return LLVMBuildICmp(a->b, p, x, y, "");
}
static LLVMValueRef choose(ArmIR* a, LLVMValueRef c, LLVMValueRef t, LLVMValueRef f) {
    return LLVMBuildSelect(a->b, c, t, f, "");
}
static LLVMValueRef z32(ArmIR* a, LLVMValueRef x) { return LLVMBuildZExt(a->b, x, a->i32, ""); }
static LLVMValueRef z64(ArmIR* a, LLVMValueRef x) { return LLVMBuildZExt(a->b, x, a->i64, ""); }
static LLVMValueRef field(ArmIR* a, size_t offset) {
    LLVMValueRef i = c64(a, offset);
    return LLVMBuildGEP2(a->b, a->i8, a->cpu, &i, 1, "");
}
static LLVMValueRef get(ArmIR* a, size_t offset, LLVMTypeRef t) {
    return LLVMBuildLoad2(a->b, t, field(a, offset), "");
}
static void put(ArmIR* a, size_t offset, LLVMValueRef v) { LLVMBuildStore(a->b, v, field(a, offset)); }
#define GET(a, f)    get(a, offsetof(SnxArmCPU, f), (a)->i32)
#define PUT(a, f, v) put(a, offsetof(SnxArmCPU, f), v)
static LLVMValueRef reg(ArmIR* a, unsigned r) {
    return r == 15 ? c32(a, a->pc + 8) : get(a, offsetof(SnxArmCPU, r) + r * 4, a->i32);
}
static void setreg(ArmIR* a, unsigned r, LLVMValueRef v) { put(a, offsetof(SnxArmCPU, r) + r * 4, v); }
static LLVMValueRef flag(ArmIR* a, unsigned n) { return band(a, lsr(a, GET(a, cpsr), n), c32(a, 1)); }
static LLVMBasicBlockRef block(ArmIR* a, const char* name) {
    return LLVMAppendBasicBlockInContext(a->ctx, a->fn, name);
}
static void trap(ArmIR* a, SnxArmExit code, LLVMValueRef syndrome) {
    PUT(a, exit, c32(a, code));
    PUT(a, syndrome, syndrome);
    LLVMBuildRetVoid(a->b);
}
/* Continue only when valid. Fault paths do not reach host memory or write back. */
static void require(ArmIR* a, LLVMValueRef valid, SnxArmExit code, LLVMValueRef syndrome) {
    LLVMBasicBlockRef ok = block(a, "ok"), bad = block(a, "fault");
    LLVMBuildCondBr(a->b, valid, ok, bad);
    LLVMPositionBuilderAtEnd(a->b, bad);
    trap(a, code, syndrome);
    LLVMPositionBuilderAtEnd(a->b, ok);
}
static LLVMValueRef condition(ArmIR* a, unsigned cond) {
    LLVMValueRef n = flag(a, 31), z = flag(a, 30), c = flag(a, 29), v = flag(a, 28), r;
    switch (cond >> 1) {
        case 0: r = z; break;
        case 1: r = c; break;
        case 2: r = n; break;
        case 3: r = v; break;
        case 4: r = band(a, c, bxor(a, z, c32(a, 1))); break;
        case 5: r = z32(a, cmp(a, LLVMIntEQ, n, v)); break;
        case 6: r = band(a, bxor(a, z, c32(a, 1)), z32(a, cmp(a, LLVMIntEQ, n, v))); break;
        default: return LLVMConstInt(a->i1, 1, 0);
    }
    return cmp(a, (cond & 1) ? LLVMIntEQ : LLVMIntNE, r, c32(a, 0));
}
static void nz(ArmIR* a, LLVMValueRef value, LLVMValueRef carry, LLVMValueRef overflow) {
    LLVMValueRef f = GET(a, cpsr);
    uint32_t mask = 0xc0000000u | (carry ? 0x20000000u : 0) | (overflow ? 0x10000000u : 0);
    f = band(a, f, c32(a, ~mask));
    f = bor(a, f, band(a, value, c32(a, 0x80000000u)));
    f = bor(a, f, shl(a, z32(a, cmp(a, LLVMIntEQ, value, c32(a, 0))), 30));
    if (carry) f = bor(a, f, shl(a, carry, 29));
    if (overflow) f = bor(a, f, shl(a, overflow, 28));
    PUT(a, cpsr, f);
}
static LLVMValueRef ror(ArmIR* a, LLVMValueRef v, unsigned n) {
    return n ? bor(a, lsr(a, v, n), shl(a, v, 32 - n)) : v;
}
/* Shift_C, including register amounts 0,32,>32 and RRX. Mask every dynamic
   LLVM shift even in unselected arms: oversized shifts otherwise make poison. */
static LLVMValueRef shift(ArmIR* a, LLVMValueRef v, unsigned type, LLVMValueRef amount, int immediate, unsigned imm,
                          LLVMValueRef* carry) {
    LLVMValueRef old = flag(a, 29), result, c;
    if (immediate) {
        if (!imm && type == 0) {
            *carry = old;
            return v;
        }
        if (!imm && type == 3) {
            *carry = band(a, v, c32(a, 1));
            return bor(a, shl(a, old, 31), lsr(a, v, 1));
        }
        if (!imm) imm = 32;
        if (type == 0) {
            c = band(a, lsr(a, v, 32 - imm), c32(a, 1));
            result = shl(a, v, imm);
        } else if (type == 1) {
            c = band(a, lsr(a, v, imm - 1), c32(a, 1));
            result = imm == 32 ? c32(a, 0) : lsr(a, v, imm);
        } else if (type == 2) {
            c = band(a, lsr(a, v, imm - 1), c32(a, 1));
            result = LLVMBuildAShr(a->b, v, c32(a, imm == 32 ? 31 : imm), "");
        } else {
            result = ror(a, v, imm);
            c = lsr(a, result, 31);
        }
    } else {
        LLVMValueRef k = band(a, amount, c32(a, 255));
        LLVMValueRef k5 = band(a, k, c32(a, 31));
        LLVMValueRef zero = cmp(a, LLVMIntEQ, k, c32(a, 0));
        LLVMValueRef small = cmp(a, LLVMIntULT, k, c32(a, 32));
        LLVMValueRef atmost = cmp(a, LLVMIntULE, k, c32(a, 32));
        LLVMValueRef km1 = band(a, sub(a, k, c32(a, 1)), c32(a, 31));
        LLVMValueRef negk = band(a, sub(a, c32(a, 0), k), c32(a, 31));
        if (type == 0) {
            result = choose(a, small, LLVMBuildShl(a->b, v, k5, ""), c32(a, 0));
            c = choose(a, atmost, band(a, LLVMBuildLShr(a->b, v, negk, ""), c32(a, 1)), c32(a, 0));
        } else if (type == 1) {
            result = choose(a, small, LLVMBuildLShr(a->b, v, k5, ""), c32(a, 0));
            c = choose(a, atmost, band(a, LLVMBuildLShr(a->b, v, km1, ""), c32(a, 1)), c32(a, 0));
        } else if (type == 2) {
            result = LLVMBuildAShr(a->b, v, choose(a, small, k5, c32(a, 31)), "");
            c = choose(a, small, band(a, LLVMBuildLShr(a->b, v, km1, ""), c32(a, 1)), lsr(a, v, 31));
        } else {
            result = bor(a, LLVMBuildLShr(a->b, v, k5, ""), LLVMBuildShl(a->b, v, negk, ""));
            c = lsr(a, result, 31);
        }
        result = choose(a, zero, v, result);
        c = choose(a, zero, old, c);
    }
    *carry = c;
    return result;
}
static LLVMValueRef operand2(ArmIR* a, uint32_t w, LLVMValueRef* carry) {
    if (w & (1u << 25)) {
        unsigned rot = ((w >> 8) & 15) * 2;
        uint32_t x = w & 255;
        if (rot) x = (x >> rot) | (x << (32 - rot));
        *carry = rot ? c32(a, x >> 31) : flag(a, 29);
        return c32(a, x);
    }
    return shift(a, reg(a, w & 15), (w >> 5) & 3, reg(a, (w >> 8) & 15), !(w & 16), (w >> 7) & 31, carry);
}
static void branch_exchange(ArmIR* a, LLVMValueRef target) {
    LLVMValueRef thumb = band(a, target, c32(a, 1));
    /* No alignment check here: a misaligned ARM target faults at the next
       instruction fetch (the dispatcher's job), not at the branch itself. */
    PUT(a, cpsr, bor(a, band(a, GET(a, cpsr), c32(a, ~SNX_ARM_T)), shl(a, thumb, 5)));
    setreg(a, 15, band(a, target, c32(a, ~1u)));
}
static LLVMValueRef arithmetic(ArmIR* a, LLVMValueRef x, LLVMValueRef y, LLVMValueRef cin, int flags) {
    LLVMValueRef wide = add(a, add(a, z64(a, x), z64(a, y)), z64(a, cin));
    LLVMValueRef result = LLVMBuildTrunc(a->b, wide, a->i32, "");
    if (flags) {
        LLVMValueRef c = LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, wide, c64(a, 32), ""), a->i32, "");
        LLVMValueRef v = lsr(a, band(a, LLVMBuildNot(a->b, bxor(a, x, y), ""), bxor(a, x, result)), 31);
        nz(a, result, c, v);
    }
    return result;
}
static void data(ArmIR* a) {
    uint32_t w = a->word;
    unsigned op = (w >> 21) & 15, d = (w >> 12) & 15;
    int flags = !!(w & (1u << 20)), logic = 0;
    LLVMValueRef carry, x = reg(a, (w >> 16) & 15), y = operand2(a, w, &carry), r;
    switch (op) {
        case 0:
        case 8:
            r = band(a, x, y);
            logic = 1;
            break;
        case 1:
        case 9:
            r = bxor(a, x, y);
            logic = 1;
            break;
        case 2:
        case 10: r = arithmetic(a, x, LLVMBuildNot(a->b, y, ""), c32(a, 1), flags); break;
        case 3: r = arithmetic(a, y, LLVMBuildNot(a->b, x, ""), c32(a, 1), flags); break;
        case 4:
        case 11: r = arithmetic(a, x, y, c32(a, 0), flags); break;
        case 5: r = arithmetic(a, x, y, flag(a, 29), flags); break;
        case 6: r = arithmetic(a, x, LLVMBuildNot(a->b, y, ""), flag(a, 29), flags); break;
        case 7: r = arithmetic(a, y, LLVMBuildNot(a->b, x, ""), flag(a, 29), flags); break;
        case 12:
            r = bor(a, x, y);
            logic = 1;
            break;
        case 13:
            r = y;
            logic = 1;
            break;
        case 14:
            r = band(a, x, LLVMBuildNot(a->b, y, ""));
            logic = 1;
            break;
        default:
            r = LLVMBuildNot(a->b, y, "");
            logic = 1;
            break;
    }
    if (flags && logic) nz(a, r, carry, NULL);
    if (op >= 8 && op <= 11) return;
    if (d == 15)
        branch_exchange(a, r);
    else
        setreg(a, d, r);
}

static void bounds(ArmIR* a, LLVMValueRef addr, unsigned bytes, unsigned alignment) {
    LLVMValueRef end, size;
    if (alignment > 1)
        require(a, cmp(a, LLVMIntEQ, band(a, addr, c32(a, alignment - 1)), c32(a, 0)), SNX_ARM_ALIGNMENT, addr);
    end = add(a, z64(a, addr), c64(a, bytes));
    size = get(a, offsetof(SnxArmCPU, mem_size), a->i64);
    require(a, band(a, cmp(a, LLVMIntULE, end, size), cmp(a, LLVMIntULE, end, c64(a, UINT64_C(1) << 32))),
            SNX_ARM_DATA_ABORT, addr);
}
static LLVMValueRef memptr(ArmIR* a, LLVMValueRef addr) {
    LLVMValueRef offset = z64(a, addr);
    return LLVMBuildGEP2(a->b, a->i8, a->mem, &offset, 1, "");
}
static LLVMValueRef host_order(ArmIR* a, LLVMValueRef v, unsigned bytes);
/* The bus hook (SnxArmCPU.io): the I/O window, the palette, OAM and the save memory have bus rules a
   plain access cannot follow (mirrors finer than a page, byte write widths). The runtime registers a
   hook the way the A64 side registers a fetch callback; without one every access is direct. */
static LLVMValueRef io_route(ArmIR* a, LLVMValueRef addr, LLVMValueRef fn) {
    LLVMValueRef top = lsr(a, addr, 24), route = z32(a, cmp(a, LLVMIntEQ, top, c32(a, 4)));
    route = bor(a, route, z32(a, cmp(a, LLVMIntEQ, top, c32(a, 5))));
    route = bor(a, route, z32(a, cmp(a, LLVMIntEQ, top, c32(a, 6))));
    route = bor(a, route, z32(a, cmp(a, LLVMIntEQ, top, c32(a, 7))));
    route = bor(a, route, z32(a, cmp(a, LLVMIntEQ, top, c32(a, 14))));
    return band(a, route, z32(a, cmp(a, LLVMIntNE, fn, LLVMConstNull(a->ptr))));
}
static LLVMTypeRef io_type(ArmIR* a) {
    LLVMTypeRef params[4] = {a->ptr, a->i32, a->i32, a->i32};
    return LLVMFunctionType(a->i32, params, 4, 0);
}
/* The GBA's ARM7TDMI rotates a misaligned load's aligned data into place in the 32 bit register instead
   of reading across the boundary (ARMv8-A would read the unaligned bytes directly). The zero
   misalignment selects the value unchanged through the shift arithmetic itself. */
static LLVMValueRef rotated_load(ArmIR* a, LLVMValueRef value, LLVMValueRef addr, unsigned bytes) {
    LLVMValueRef mis = band(a, addr, c32(a, bytes - 1));
    LLVMValueRef amount = shl(a, mis, 3);
    LLVMValueRef top = LLVMBuildLShr(a->b, value, amount, "");
    LLVMValueRef bottom = LLVMBuildShl(a->b, value, band(a, sub(a, c32(a, 32), amount), c32(a, 31)), "");
    return bor(a, top, bottom);
}
/* Normal memory permits unaligned single-register transfers. Do not give LLVM
   its default ABI alignment, and do not split aligned words into byte stores. */
static LLVMValueRef readmem(ArmIR* a, LLVMValueRef addr, unsigned bytes, int sign) {
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef fn = get(a, offsetof(SnxArmCPU, io), a->ptr);
    LLVMValueRef aligned = band(a, addr, c32(a, ~(uint32_t)(bytes - 1)));
    LLVMBasicBlockRef io = block(a, "io"), mem = block(a, "mem"), done = block(a, "read");
    LLVMValueRef incoming[2], joined;
    LLVMBasicBlockRef from[2];
    LLVMBuildCondBr(a->b, cmp(a, LLVMIntNE, io_route(a, addr, fn), c32(a, 0)), io, mem);
    LLVMPositionBuilderAtEnd(a->b, mem);
    {
        LLVMValueRef value = LLVMBuildLoad2(a->b, type, memptr(a, aligned), "");
        LLVMSetAlignment(value, 1);
        value = host_order(a, value, bytes);
        if (sign && bytes == 2) {
            /* ARMv4T: a misaligned LDRSH reads a sign extended byte instead */
            LLVMValueRef wide = LLVMBuildSExt(a->b, value, a->i32, "");
            LLVMValueRef narrow =
                LLVMBuildSExt(a->b, LLVMBuildLoad2(a->b, a->i8, memptr(a, addr), ""), a->i32, "");
            incoming[0] = LLVMBuildSelect(a->b, cmp(a, LLVMIntNE, band(a, addr, c32(a, 1)), c32(a, 0)), narrow, wide,
                                         "");
        } else
            incoming[0] = bytes < 4 ? z32(a, value) : value;
        from[0] = mem;
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, io);
    {
        LLVMValueRef args[4] = {a->cpu, aligned, c32(a, (sign && bytes == 2 ? 0x200 : 0) | bytes), c32(a, 0)};
        incoming[1] = LLVMBuildCall2(a->b, io_type(a), fn, args, 4, "");
        from[1] = io;
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, done);
    joined = LLVMBuildPhi(a->b, a->i32, "");
    LLVMAddIncoming(joined, incoming, from, 2);
    if (sign && bytes == 2) return joined; /* the quirk value is already the result */
    /* both paths carry the aligned data zero extended; a signed load narrows back and widens again */
    if (bytes > 1) joined = rotated_load(a, joined, addr, bytes);
    return sign && bytes < 4 ? LLVMBuildSExt(a->b, LLVMBuildTrunc(a->b, joined, type, ""), a->i32, "") : joined;
}
static void writemem(ArmIR* a, LLVMValueRef addr, unsigned bytes, LLVMValueRef value) {
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef fn = get(a, offsetof(SnxArmCPU, io), a->ptr);
    LLVMBasicBlockRef io = block(a, "io"), mem = block(a, "mem"), done = block(a, "written");
    /* A local ordinary store may clear the monitor. Clearing on every local
       store is a permitted spurious exclusive failure. */
    PUT(a, exclusive_valid, c32(a, 0));
    LLVMBuildCondBr(a->b, cmp(a, LLVMIntNE, io_route(a, addr, fn), c32(a, 0)), io, mem);
    LLVMPositionBuilderAtEnd(a->b, mem);
    {
        LLVMValueRef narrow = bytes < 4 ? LLVMBuildTrunc(a->b, value, type, "") : value;
        /* a misaligned store lands in the aligned unit on the GBA, without the load rotation */
        LLVMValueRef store =
            LLVMBuildStore(a->b, host_order(a, narrow, bytes), memptr(a, band(a, addr, c32(a, ~(uint32_t)(bytes - 1)))));
        LLVMSetAlignment(store, 1);
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, io);
    {
        /* the hook sees the raw address: the save memory's 8 bit bus picks the lane from it */
        LLVMValueRef args[4] = {a->cpu, addr, c32(a, 0x100 | bytes), value};
        LLVMBuildCall2(a->b, io_type(a), fn, args, 4, "");
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, done);
}
/* LDM and STM move raw words at the exact addresses: the GBA's single transfer rotation and
   alignment do not apply to the list transfers. The bus hook still sees them. */
static LLVMValueRef readmem_list(ArmIR* a, LLVMValueRef addr, unsigned bytes) {
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef fn = get(a, offsetof(SnxArmCPU, io), a->ptr);
    LLVMBasicBlockRef io = block(a, "io"), mem = block(a, "mem"), done = block(a, "read");
    LLVMValueRef incoming[2], joined;
    LLVMBasicBlockRef from[2];
    LLVMBuildCondBr(a->b, cmp(a, LLVMIntNE, io_route(a, addr, fn), c32(a, 0)), io, mem);
    LLVMPositionBuilderAtEnd(a->b, mem);
    {
        LLVMValueRef value = LLVMBuildLoad2(a->b, type, memptr(a, addr), "");
        LLVMSetAlignment(value, 1);
        incoming[0] = host_order(a, value, bytes);
        from[0] = mem;
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, io);
    {
        LLVMValueRef args[4] = {a->cpu, addr, c32(a, bytes), c32(a, 0)};
        incoming[1] = LLVMBuildCall2(a->b, io_type(a), fn, args, 4, "");
        from[1] = io;
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, done);
    joined = LLVMBuildPhi(a->b, a->i32, "");
    LLVMAddIncoming(joined, incoming, from, 2);
    return bytes < 4 ? z32(a, joined) : joined;
}
static void writemem_list(ArmIR* a, LLVMValueRef addr, unsigned bytes, LLVMValueRef value) {
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef fn = get(a, offsetof(SnxArmCPU, io), a->ptr);
    LLVMBasicBlockRef io = block(a, "io"), mem = block(a, "mem"), done = block(a, "written");
    LLVMBuildCondBr(a->b, cmp(a, LLVMIntNE, io_route(a, addr, fn), c32(a, 0)), io, mem);
    LLVMPositionBuilderAtEnd(a->b, mem);
    {
        LLVMValueRef narrow = bytes < 4 ? LLVMBuildTrunc(a->b, value, type, "") : value;
        LLVMValueRef store = LLVMBuildStore(a->b, host_order(a, narrow, bytes), memptr(a, addr));
        LLVMSetAlignment(store, 1);
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, io);
    {
        LLVMValueRef args[4] = {a->cpu, addr, c32(a, 0x100 | bytes), value};
        LLVMBuildCall2(a->b, io_type(a), fn, args, 4, "");
    }
    LLVMBuildBr(a->b, done);
    LLVMPositionBuilderAtEnd(a->b, done);
}
static void memory(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15, op = (w >> 5) & 3;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23));
    int wb = !pre || !!(w & (1u << 21)), load = !!(w & (1u << 20));
    unsigned bytes = 4;
    int sign = 0, pair = 0;
    LLVMValueRef base = reg(a, n), offset, adjusted, addr, value, second = NULL, unused;
    if (kind == SNX_A32_SINGLE) {
        bytes = w & (1u << 22) ? 1 : 4;
        offset =
            w & (1u << 25) ? shift(a, reg(a, w & 15), (w >> 5) & 3, NULL, 1, (w >> 7) & 31, &unused) : c32(a, w & 4095);
    } else {
        offset = w & (1u << 22) ? c32(a, ((w >> 4) & 0xf0) | (w & 15)) : reg(a, w & 15);
        if (!load && op >= 2) {
            pair = 1;
            load = op == 2;
        } else {
            bytes = op == 2 ? 1 : 2;
            sign = op >= 2;
        }
    }
    adjusted = up ? add(a, base, offset) : sub(a, base, offset);
    addr = pre ? adjusted : base;
    bounds(a, addr, pair ? 8 : bytes, pair || (load && d == 15) ? 4 : 1);
    if (load) {
        value = readmem(a, addr, bytes, sign);
        if (pair) second = readmem(a, add(a, addr, c32(a, 4)), 4, 0);
        /* Check a loaded PC before committing writeback. */
        if (d == 15)
            branch_exchange(a, value);
        else
            setreg(a, d, value);
        if (pair) setreg(a, d + 1, second);
    } else {
        value = reg(a, d); /* ARM7TDMI's PCStoreValue is instruction address + 12 */
        if (d == 15) value = c32(a, a->pc + 12);
        if (pair) second = reg(a, d + 1);
        writemem(a, addr, bytes, value);
        if (pair) writemem(a, add(a, addr, c32(a, 4)), 4, second);
    }
    if (wb) setreg(a, n, adjusted);
}
static void multiple(ArmIR* a) {
    uint32_t w = a->word;
    unsigned n = (w >> 16) & 15, count = 0, i, index = 0;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23)), load = !!(w & (1u << 20));
    LLVMValueRef base = reg(a, n), start, adjusted, values[16] = {0};
    for (i = 0; i < 16; ++i) count += (w >> i) & 1;
    adjusted = up ? add(a, base, c32(a, count * 4)) : sub(a, base, c32(a, count * 4));
    start = up ? add(a, base, c32(a, pre ? 4 : 0)) : add(a, adjusted, c32(a, pre ? 0 : 4));
    /* The GBA's ARM7TDMI transfers at an unaligned base instead of faulting */
    bounds(a, start, count * 4, 1);
    /* Snapshot all sources before updates, particularly SP and the PC. */
    for (i = 0; i < 16; ++i)
        if (w & (1u << i)) {
            /* the ARM7TDMI stores the written back base when the base is not the first in the list */
            int first = !(w & ((1u << i) - 1));
            values[i] = load ? readmem_list(a, add(a, start, c32(a, index * 4)), 4)
                             : (i == 15   ? c32(a, a->pc + 12)
                                : i == n ? ((w & (1u << 21)) && !first ? adjusted : reg(a, i))
                                         : reg(a, i));
            ++index;
        }
    if (load && values[15]) branch_exchange(a, values[15]);
    index = 0;
    for (i = 0; i < 16; ++i)
        if (values[i]) {
            if (load) {
                if (i != 15) setreg(a, i, values[i]);
            } else
                writemem_list(a, add(a, start, c32(a, index * 4)), 4, values[i]);
            ++index;
        }
    if (w & (1u << 21)) setreg(a, n, adjusted);
}

static LLVMValueRef intrinsic(ArmIR* a, const char* name, size_t len, LLVMTypeRef type, LLVMValueRef* args,
                              unsigned count) {
    unsigned id = LLVMLookupIntrinsicID(name, len);
    LLVMValueRef fn = LLVMGetIntrinsicDeclaration(a->mod, id, &type, 1);
    return LLVMBuildCall2(a->b, LLVMGlobalGetValueType(fn), fn, args, count, "");
}
static LLVMValueRef host_order(ArmIR* a, LLVMValueRef v, unsigned bytes) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if (bytes > 1) return intrinsic(a, "llvm.bswap", 10, LLVMTypeOf(v), &v, 1);
#else
    (void)a;
    (void)bytes;
#endif
    return v;
}
static void exclusive(ArmIR* a) {
    uint32_t w = a->word;
    unsigned op = (w >> 20) & 7, n = (w >> 16) & 15, d = (w >> 12) & 15, t = w & 15;
    static const unsigned widths[] = {4, 8, 1, 2};
    unsigned bytes = widths[op >> 1];
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    LLVMValueRef addr = reg(a, n), p, value;
    bounds(a, addr, bytes, bytes);
    p = memptr(a, addr);
    if (op & 1) {
        value = LLVMBuildLoad2(a->b, type, p, "");
        LLVMSetOrdering(value, LLVMAtomicOrderingSequentiallyConsistent);
        LLVMSetAlignment(value, bytes);
        value = host_order(a, value, bytes);
        if (bytes < 8) value = LLVMBuildZExt(a->b, value, a->i64, "");
        put(a, offsetof(SnxArmCPU, exclusive_value), value);
        PUT(a, exclusive_address, addr);
        PUT(a, exclusive_size, c32(a, bytes));
        PUT(a, exclusive_valid, c32(a, 1));
        setreg(a, d, LLVMBuildTrunc(a->b, value, a->i32, ""));
        if (bytes == 8) setreg(a, d + 1, LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, value, c64(a, 32), ""), a->i32, ""));
    } else {
        LLVMBasicBlockRef attempt = block(a, "exclusive.try"), fail = block(a, "exclusive.fail"),
                          join = block(a, "exclusive.end");
        LLVMValueRef valid = cmp(a, LLVMIntNE, GET(a, exclusive_valid), c32(a, 0)), expected, pair, result;
        LLVMValueRef incoming[2];
        LLVMBasicBlockRef from[2];
        valid = band(a, valid, cmp(a, LLVMIntEQ, GET(a, exclusive_address), addr));
        valid = band(a, valid, cmp(a, LLVMIntEQ, GET(a, exclusive_size), c32(a, bytes)));
        PUT(a, exclusive_valid, c32(a, 0));
        LLVMBuildCondBr(a->b, valid, attempt, fail);
        LLVMPositionBuilderAtEnd(a->b, attempt);
        value = reg(a, t);
        if (bytes == 8)
            value = bor(a, z64(a, value), LLVMBuildShl(a->b, z64(a, reg(a, t + 1)), c64(a, 32), ""));
        else if (bytes < 4)
            value = LLVMBuildTrunc(a->b, value, type, "");
        expected = get(a, offsetof(SnxArmCPU, exclusive_value), a->i64);
        if (bytes < 8) expected = LLVMBuildTrunc(a->b, expected, type, "");
        pair = LLVMBuildAtomicCmpXchg(a->b, p, host_order(a, expected, bytes), host_order(a, value, bytes),
                                      LLVMAtomicOrderingSequentiallyConsistent,
                                      LLVMAtomicOrderingSequentiallyConsistent, 0);
        LLVMSetAlignment(pair, bytes);
        incoming[0] = bxor(a, z32(a, LLVMBuildExtractValue(a->b, pair, 1, "")), c32(a, 1));
        from[0] = LLVMGetInsertBlock(a->b);
        LLVMBuildBr(a->b, join);
        LLVMPositionBuilderAtEnd(a->b, fail);
        incoming[1] = c32(a, 1);
        from[1] = fail;
        LLVMBuildBr(a->b, join);
        LLVMPositionBuilderAtEnd(a->b, join);
        result = LLVMBuildPhi(a->b, a->i32, "");
        LLVMAddIncoming(result, incoming, from, 2);
        setreg(a, d, result);
    }
}
static void acquire_release(ArmIR* a) {
    unsigned op = (a->word >> 20) & 7, bytes = op >= 6 ? 2 : op >= 4 ? 1 : 4;
    LLVMValueRef addr = reg(a, (a->word >> 16) & 15), value, access;
    LLVMTypeRef type = LLVMIntTypeInContext(a->ctx, bytes * 8);
    bounds(a, addr, bytes, bytes);
    if (op & 1) {
        access = LLVMBuildLoad2(a->b, type, memptr(a, addr), "");
        LLVMSetOrdering(access, LLVMAtomicOrderingSequentiallyConsistent);
        LLVMSetAlignment(access, bytes);
        value = host_order(a, access, bytes);
        if (bytes < 4) value = z32(a, value);
        setreg(a, (a->word >> 12) & 15, value);
    } else {
        value = reg(a, a->word & 15);
        if (bytes < 4) value = LLVMBuildTrunc(a->b, value, type, "");
        access = LLVMBuildStore(a->b, host_order(a, value, bytes), memptr(a, addr));
        LLVMSetOrdering(access, LLVMAtomicOrderingSequentiallyConsistent);
        LLVMSetAlignment(access, bytes);
        PUT(a, exclusive_valid, c32(a, 0));
    }
}
static void multiply(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15;
    LLVMValueRef x = reg(a, w & 15), y = reg(a, (w >> 8) & 15), r;
    if (kind == SNX_A32_MULL) {
        if (w & (1u << 22)) {
            x = LLVMBuildSExt(a->b, x, a->i64, "");
            y = LLVMBuildSExt(a->b, y, a->i64, "");
        } else {
            x = z64(a, x);
            y = z64(a, y);
        }
        r = LLVMBuildMul(a->b, x, y, "");
        if (w & (1u << 21))
            r = add(a, r, bor(a, z64(a, reg(a, d)), LLVMBuildShl(a->b, z64(a, reg(a, n)), c64(a, 32), "")));
        setreg(a, d, LLVMBuildTrunc(a->b, r, a->i32, ""));
        setreg(a, n, LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, r, c64(a, 32), ""), a->i32, ""));
        if (w & (1u << 20)) {
            LLVMValueRef f = band(a, GET(a, cpsr), c32(a, 0x3fffffffu));
            LLVMValueRef sign = LLVMBuildTrunc(a->b, LLVMBuildLShr(a->b, r, c64(a, 32), ""), a->i32, "");
            f = bor(a, f, band(a, sign, c32(a, 0x80000000u)));
            f = bor(a, f, shl(a, z32(a, cmp(a, LLVMIntEQ, r, c64(a, 0))), 30));
            PUT(a, cpsr, f);
        }
    } else {
        r = LLVMBuildMul(a->b, x, y, "");
        if (kind == SNX_A32_MLS)
            r = sub(a, reg(a, d), r);
        else if (w & (1u << 21))
            r = add(a, r, reg(a, d));
        if (w & (1u << 20)) nz(a, r, NULL, NULL);
        setreg(a, n, r);
    }
}
static void miscellaneous(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned d = (w >> 12) & 15, n = (w >> 16) & 15;
    LLVMValueRef x = reg(a, w & 15), r = x;
    if (kind == SNX_A32_MOVW || kind == SNX_A32_MOVT) {
        uint32_t imm = ((w >> 4) & 0xf000) | (w & 0xfff);
        r = kind == SNX_A32_MOVW ? c32(a, imm) : bor(a, band(a, reg(a, d), c32(a, 65535)), c32(a, imm << 16));
    } else if (kind == SNX_A32_DIV) {
        LLVMValueRef y = reg(a, (w >> 8) & 15), zero = cmp(a, LLVMIntEQ, y, c32(a, 0));
        y = choose(a, zero, c32(a, 1), y);
        if (w & (1u << 21))
            r = LLVMBuildUDiv(a->b, x, y, "");
        else {
            r = LLVMBuildSDiv(a->b, LLVMBuildSExt(a->b, x, a->i64, ""), LLVMBuildSExt(a->b, y, a->i64, ""), "");
            r = LLVMBuildTrunc(a->b, r, a->i32, "");
        }
        r = choose(a, zero, c32(a, 0), r);
        d = n;
    } else if (kind == SNX_A32_CLZ) {
        LLVMValueRef args[] = {x, LLVMConstInt(a->i1, 0, 0)};
        r = intrinsic(a, "llvm.ctlz", 9, a->i32, args, 2);
    } else if (kind == SNX_A32_REV) {
        if ((w & 0x00400080) == 0x00400000)
            r = intrinsic(a, "llvm.bitreverse", 15, a->i32, &x, 1);
        else if (!(w & 0x80))
            r = intrinsic(a, "llvm.bswap", 10, a->i32, &x, 1);
        else {
            r = bor(a, band(a, lsr(a, x, 8), c32(a, 0x00ff00ff)), shl(a, band(a, x, c32(a, 0x00ff00ff)), 8));
            if (w & (1u << 22)) r = LLVMBuildSExt(a->b, LLVMBuildTrunc(a->b, r, a->i16, ""), a->i32, "");
        }
    } else if (kind == SNX_A32_EXTEND) {
        LLVMTypeRef type = w & (1u << 20) ? a->i16 : a->i8;
        r = LLVMBuildTrunc(a->b, ror(a, x, ((w >> 10) & 3) * 8), type, "");
        r = w & (1u << 22) ? z32(a, r) : LLVMBuildSExt(a->b, r, a->i32, "");
        if (n != 15) r = add(a, r, reg(a, n));
    } else if (kind == SNX_A32_BITFIELD) {
        unsigned lsb = (w >> 7) & 31, width;
        uint32_t mask;
        if ((w & 0x0fe00070) == 0x07c00010) {
            width = ((w >> 16) & 31) - lsb + 1;
            mask = UINT32_MAX >> (32 - width);
            r = (w & 15) == 15 ? c32(a, 0) : shl(a, band(a, x, c32(a, mask)), lsb);
            r = bor(a, band(a, reg(a, d), c32(a, ~(mask << lsb))), r);
        } else {
            width = ((w >> 16) & 31) + 1;
            mask = UINT32_MAX >> (32 - width);
            r = band(a, lsr(a, x, lsb), c32(a, mask));
            if (!(w & (1u << 22)) && width < 32) r = LLVMBuildAShr(a->b, shl(a, r, 32 - width), c32(a, 32 - width), "");
        }
    } else if (kind == SNX_A32_MRS)
        r = GET(a, cpsr);
    else if (kind == SNX_A32_MSR) {
        LLVMValueRef unused;
        uint32_t mask = (n & 8 ? 0xf8000000u : 0) | (n & 4 ? 0x000f0000u : 0);
        r = w & (1u << 25) ? operand2(a, w, &unused) : x;
        PUT(a, cpsr, bor(a, band(a, GET(a, cpsr), c32(a, ~mask)), band(a, r, c32(a, mask))));
        return;
    }
    setreg(a, d, r);
}
#include "llvm_vector.inc"
#include "llvm_thumb.inc"

/* Returns 1 for instructions which end the block, with the next PC stored. */
static int instruction(ArmIR* a, SnxA32Kind kind) {
    uint32_t w = a->word;
    unsigned d = (w >> 12) & 15;
    switch (kind) {
        case SNX_A32_VECTOR: vector_instruction(a); break;
        case SNX_A32_DATA: data(a); return d == 15;
        case SNX_A32_SINGLE:
        case SNX_A32_EXTRA: memory(a, kind); return d == 15 && (w & (1u << 20));
        case SNX_A32_MULTIPLE: multiple(a); return (w & (1u << 15)) && (w & (1u << 20));
        case SNX_A32_BRANCH:
        case SNX_A32_BLX_IMM: {
            uint32_t offset = (w & 0xffffff) << 2;
            if (offset & 0x02000000) offset |= 0xfc000000;
            if ((w & (1u << 24)) || kind == SNX_A32_BLX_IMM) setreg(a, 14, c32(a, a->pc + 4));
            if (kind == SNX_A32_BLX_IMM) {
                offset |= (w >> 23) & 2;
                PUT(a, cpsr, bor(a, GET(a, cpsr), c32(a, SNX_ARM_T)));
            }
            setreg(a, 15, c32(a, a->pc + 8 + offset));
            return 1;
        }
        case SNX_A32_BX: {
            LLVMValueRef target = reg(a, w & 15);
            /* Target can be LR, so read it before writing the return address. */
            branch_exchange(a, target);
            if (w & 0x20) setreg(a, 14, c32(a, a->pc + 4));
            return 1;
        }
        case SNX_A32_MUL:
        case SNX_A32_MULL:
        case SNX_A32_MLS: multiply(a, kind); break;
        case SNX_A32_EXCLUSIVE: exclusive(a); break;
        case SNX_A32_ACQUIRE_RELEASE: acquire_release(a); break;
        case SNX_A32_CLREX: PUT(a, exclusive_valid, c32(a, 0)); break;
        case SNX_A32_BARRIER:
            /* Stronger than each valid DMB/DSB scope, never just a compiler barrier. */
            LLVMBuildFence(a->b, LLVMAtomicOrderingSequentiallyConsistent, 0, "");
            break;
        case SNX_A32_HINT: break; /* hints may complete without waiting */
        case SNX_A32_TLS:
            if (w & (1u << 20))
                setreg(a, d, w & 0x20 ? GET(a, tpidruro) : GET(a, tpidrurw));
            else
                PUT(a, tpidrurw, reg(a, d));
            break;
        case SNX_A32_SVC:
        case SNX_A32_BKPT:
            setreg(a, 15, c32(a, a->pc + 4));
            trap(a, kind == SNX_A32_SVC ? SNX_ARM_SVC : SNX_ARM_BREAKPOINT,
                 c32(a, kind == SNX_A32_SVC ? w & 0xffffff : ((w >> 4) & 0xfff0) | (w & 15)));
            return 1;
        case SNX_A32_UNDEFINED: trap(a, SNX_ARM_UNDEFINED, c32(a, w)); return 1;
        default: miscellaneous(a, kind); break;
    }
    return 0;
}

LLVMValueRef snx_arm_llvm_block(LLVMModuleRef module, const char* name, const uint32_t* words, size_t count,
                                uint32_t pc, size_t* consumed) {
    ArmIR a;
    LLVMTypeRef ft;
    size_t i;
    if (consumed) *consumed = 0;
    if (!module || !name || !*name || !words || !count || count > 65536 || (pc & 3) ||
        LLVMGetNamedFunction(module, name) || LLVMGetNamedGlobal(module, name))
        return NULL;
    a.ctx = LLVMGetModuleContext(module);
    a.mod = module;
    a.b = LLVMCreateBuilderInContext(a.ctx);
    a.i1 = LLVMInt1TypeInContext(a.ctx);
    a.i8 = LLVMInt8TypeInContext(a.ctx);
    a.i16 = LLVMInt16TypeInContext(a.ctx);
    a.i32 = LLVMInt32TypeInContext(a.ctx);
    a.i64 = LLVMInt64TypeInContext(a.ctx);
    a.ptr = LLVMPointerTypeInContext(a.ctx, 0);
    ft = LLVMFunctionType(LLVMVoidTypeInContext(a.ctx), &a.ptr, 1, 0);
    a.fn = LLVMAddFunction(module, name, ft);
    a.cpu = LLVMGetParam(a.fn, 0);
    LLVMPositionBuilderAtEnd(a.b, block(&a, "entry"));
    PUT(&a, exit, c32(&a, SNX_ARM_OK));
    PUT(&a, syndrome, c32(&a, 0));
    require(&a, cmp(&a, LLVMIntEQ, get(&a, offsetof(SnxArmCPU, r) + 60, a.i32), c32(&a, pc)), SNX_ARM_BAD_ENTRY,
            c32(&a, pc));
    require(&a, cmp(&a, LLVMIntEQ, band(&a, GET(&a, cpsr), c32(&a, SNX_ARM_T)), c32(&a, 0)), SNX_ARM_BAD_ENTRY,
            c32(&a, words[0]));
    require(&a, cmp(&a, LLVMIntEQ, band(&a, GET(&a, cpsr), c32(&a, SNX_ARM_E)), c32(&a, 0)), SNX_ARM_ENDIAN_UNSUPPORTED,
            c32(&a, words[0]));
    a.mem = get(&a, offsetof(SnxArmCPU, mem), a.ptr);
    for (i = 0; i < count; ++i) {
        LLVMBasicBlockRef execute = block(&a, "execute"), skip = block(&a, "skip"), next = block(&a, "next");
        int stop;
        a.pc = pc + (uint32_t)i * 4;
        a.word = words[i];
        setreg(&a, 15, c32(&a, a.pc));
        LLVMBuildCondBr(a.b, condition(&a, a.word >> 28), execute, skip);
        LLVMPositionBuilderAtEnd(a.b, execute);
        stop = instruction(&a, snx_a32_decode(a.word));
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(a.b))) {
            if (!stop) setreg(&a, 15, c32(&a, a.pc + 4));
            LLVMBuildBr(a.b, next);
        }
        LLVMPositionBuilderAtEnd(a.b, skip);
        setreg(&a, 15, c32(&a, a.pc + 4));
        LLVMBuildBr(a.b, next);
        LLVMPositionBuilderAtEnd(a.b, next);
        if (stop) {
            ++i;
            break;
        }
    }
    LLVMBuildRetVoid(a.b);
    LLVMDisposeBuilder(a.b);
    if (consumed) *consumed = i;
    return a.fn;
}
