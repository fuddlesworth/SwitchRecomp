/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Direct Maxwell to SPIR-V translation. Unsupported instructions reject the entire module. */
#include "gpu/shader/spirv_emit.h"
#include "gpu/shader/translator.h"
#include <assert.h>
#include <stdlib.h>

typedef struct {
    SpvEmitter emit;
    u32 u32_type, f32_type, f16_type, f64_type, bool_type, zero, true_value, false_value, glsl;
    u32 reg[255];
    u32 pred[7];
    u32 cc_c, cc_o, cc_z, cc_s;
    u32 cbuf[18], cbuf_pointer[18];
    u32 scale_buffer, scale_pointer;
    u32 sampler[32], sampled_type[32];
    u32 texture_array, tex_index[32]; /* indexed textures: their array, and each slot's element (a variable) */
    u32 swapped;                      /* 1 + the slot whose sampler is an array element for this instruction */
    u32 image[8], image_type[8];
    u32 gbuf[16], gbuf_pointer, smap, subgroup_lane_input;
} DirectCtx;

int g_shader_texture_arrays;
int g_shader_stage_point_size;
int g_shader_output_layer = 1;
int g_shader_geometry = 1;
int g_shader_float16 = 1;

static u32 reg_read(DirectCtx* c, u32 r) {
    return r >= 255 ? c->zero : spv_emit_load(&c->emit, c->u32_type, c->reg[r]);
}

static void reg_write(DirectCtx* c, u32 r, u32 value) {
    if (r < 255) spv_emit_store(&c->emit, c->reg[r], value);
}

/* How a fragment shader's generic input interpolates (the SPH's imap: 1 constant, 2 perspective, 3 screen
   linear): as its first used component, and perspective when none is marked used, as Eden reads it */
static u32 ipa_interpolation(const u32* sph, u32 input) {
    u32 k, mode;
    for (k = 0; k < 4; k++) {
        mode = (sph[6 + input / 4] >> ((input % 4) * 8 + k * 2)) & 3;
        if (mode) return mode;
    }
    return 2;
}

/* a shared memory word index kept inside the array the shader declares: out of range is undefined on the console,
   and past an OpTypeArray in SPIR-V. A vertex of an input or output array is kept inside it the same way */
static u32 shared_index(DirectCtx* c, u32 index, u32 words) {
    u32 last = spv_emit_constant_u32(&c->emit, c->u32_type, words - 1);
    return spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, last, index), last, index);
}

/* a texture instruction's array layer: the low 16 bits of its register */
static u32 array_layer(DirectCtx* c, u32 r) {
    return spv_emit_convert_u_to_f(&c->emit, c->f32_type,
                                   spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, r),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff)));
}

static u32 texture_component(DirectCtx* c, u32 sampled, u32 scalar_type, u32 component) {
    u32 value = spv_emit_vec4_extract(&c->emit, scalar_type, sampled, component);
    return scalar_type == c->u32_type ? value : spv_emit_bitcast(&c->emit, c->u32_type, value);
}

static u32 vm_select(DirectCtx* c, u32 i32_type, u32 value, u32 select) {
    u32 lane = select & 7;
    if (lane >= 6) return value;
    u32 width = lane < 4 ? 8 : 16;
    u32 offset = lane < 4 ? lane * 8 : (lane - 4) * 16;
    u32 off_id = spv_emit_constant_u32(&c->emit, c->u32_type, offset);
    u32 width_id = spv_emit_constant_u32(&c->emit, c->u32_type, width);
    return select & 8 ? spv_emit_bitcast(&c->emit, c->u32_type,
                                         spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                    spv_emit_bitcast(&c->emit, i32_type, value), off_id,
                                                                    width_id))
                      : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, off_id, width_id);
}

static u32 xmad_half(DirectCtx* c, u32 i32_type, u32 value, int high, int is_signed) {
    u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, high ? 16 : 0);
    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
    if (is_signed) {
        u32 signed_value = spv_emit_bitcast(&c->emit, i32_type, value);
        u32 half = spv_emit_bitfield_sextract(&c->emit, i32_type, signed_value, offset, width);
        return spv_emit_bitcast(&c->emit, c->u32_type, half);
    }
    return spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, width);
}

static u32 global_atomic_opcode(u64 w, int reduction) {
    u32 size = reduction ? B(w, 20, 3) : B(w, 49, 3);
    u32 op = reduction ? B(w, 23, 3) : B(w, 52, 4);
    if (op == 10) op = 0; /* SAFEADD */
    if (size > 1 && size != 3) return 0;
    if (size == 3) return op == 8 ? 229 : op == 0 ? 0xffffffffu : 0;
    switch (op) {
        case 0: return 234;              /* IAdd */
        case 1: return size ? 236 : 237; /* SMin / UMin */
        case 2: return size ? 238 : 239; /* SMax / UMax */
        case 3:
        case 4: return 0xffffffffu; /* Inc / Dec use compare exchange. */
        case 5: return 240;         /* And */
        case 6: return 241;         /* Or */
        case 7: return 242;         /* Xor */
        case 8: return 229;         /* Exchange */
        default: return 0;
    }
}

static u32 shared_atomic_opcode(u64 w) {
    if (B(w, 28, 2) == 2 && B(w, 52, 4) == 8) return 0xfffffffeu; /* U64 exchange */
    if (B(w, 28, 2) > 1) return 0;
    switch (B(w, 52, 4)) {
        case 0: return 234;                     /* IAdd */
        case 1: return B(w, 28, 2) ? 236 : 237; /* SMin / UMin */
        case 2: return B(w, 28, 2) ? 238 : 239; /* SMax / UMax */
        case 3:
        case 4: return 0xffffffffu; /* Inc / Dec use compare exchange. */
        case 5: return 240;         /* And */
        case 6: return 241;         /* Or */
        case 7: return 242;         /* Xor */
        case 8: return 229;         /* Exchange */
        default: return 0;
    }
}

static u32 pred_read(DirectCtx* c, u32 p, int negative) {
    u32 value = p == 7 ? c->true_value : spv_emit_load(&c->emit, c->bool_type, c->pred[p]);
    return negative ? spv_emit_bool_not(&c->emit, c->bool_type, value) : value;
}

static void pred_write(DirectCtx* c, u32 p, u32 value) {
    if (p < 7) spv_emit_store(&c->emit, c->pred[p], value);
}

static u32 subgroup_ballot_word(DirectCtx* c, u32 vec4_u32, u32 lane, u32 predicate) {
    u32 mask = spv_emit_subgroup_ballot(&c->emit, vec4_u32, c->u32_type, predicate);
    u32 word = spv_emit_shift_right_u32(&c->emit, c->u32_type, lane, spv_emit_constant_u32(&c->emit, c->u32_type, 5));
    u32 result = spv_emit_vec4_extract(&c->emit, c->u32_type, mask, 0);
    for (u32 i = 1; i < 4; i++)
        result = spv_emit_select(
            &c->emit, c->u32_type,
            spv_emit_i_equal(&c->emit, c->bool_type, word, spv_emit_constant_u32(&c->emit, c->u32_type, i)),
            spv_emit_vec4_extract(&c->emit, c->u32_type, mask, i), result);
    return result;
}

static void flags_add(DirectCtx* c, u32 left, u32 right, u32 sum) {
    u32 carry = spv_emit_u_less(&c->emit, c->bool_type, sum, left);
    u32 signs = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, sum, left);
    u32 same = spv_emit_not_u32(&c->emit, c->u32_type, spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right));
    u32 bits = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, signs, same);
    u32 top = spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u);
    u32 overflow = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, bits, top), c->zero);
    u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, sum, c->zero);
    u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type, spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum, top),
                                    c->zero);
    spv_emit_store(&c->emit, c->cc_c, carry);
    spv_emit_store(&c->emit, c->cc_o, overflow);
    spv_emit_store(&c->emit, c->cc_z, zero);
    spv_emit_store(&c->emit, c->cc_s, sign);
}

static u32 integer_add(DirectCtx* c, u32 left, u32 right, u32 avg, int extended, int write_cc) {
    if (!extended && !write_cc) {
        if (avg == 2) left = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, left);
        if (avg == 1) right = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, right);
        u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, left, right);
        return avg == 3 ? spv_emit_iadd_u32(&c->emit, c->u32_type, sum, spv_emit_constant_u32(&c->emit, c->u32_type, 1))
                        : sum;
    }
    if (avg == 2) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
    if (avg == 1) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
    u32 sum0 = spv_emit_iadd_u32(&c->emit, c->u32_type, left, right);
    u32 carry0 = spv_emit_u_less(&c->emit, c->bool_type, sum0, left);
    u32 cin = avg ? spv_emit_constant_u32(&c->emit, c->u32_type, 1) : c->zero;
    if (extended) {
        u32 old_carry = spv_emit_load(&c->emit, c->bool_type, c->cc_c);
        u32 extra =
            spv_emit_select(&c->emit, c->u32_type, old_carry, spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
        cin = spv_emit_iadd_u32(&c->emit, c->u32_type, cin, extra);
    }
    u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, sum0, cin);
    if (write_cc) {
        u32 carry1 = spv_emit_u_less(&c->emit, c->bool_type, sum, sum0);
        u32 carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry0, carry1);
        u32 signs = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, sum, left);
        u32 same =
            spv_emit_not_u32(&c->emit, c->u32_type, spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right));
        u32 bits = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, signs, same);
        u32 top = spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u);
        u32 overflow = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, bits, top), c->zero);
        u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, sum, c->zero);
        if (extended)
            zero =
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, zero, spv_emit_load(&c->emit, c->bool_type, c->cc_z));
        u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum, top), c->zero);
        spv_emit_store(&c->emit, c->cc_c, carry);
        spv_emit_store(&c->emit, c->cc_o, overflow);
        spv_emit_store(&c->emit, c->cc_z, zero);
        spv_emit_store(&c->emit, c->cc_s, sign);
    }
    return sum;
}

static void flags_logic(DirectCtx* c, u32 value) {
    u32 sign = spv_emit_i_not_equal(&c->emit, c->bool_type,
                                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, value,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000u)),
                                    c->zero);
    spv_emit_store(&c->emit, c->cc_c, c->false_value);
    spv_emit_store(&c->emit, c->cc_o, c->false_value);
    spv_emit_store(&c->emit, c->cc_z, spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero));
    spv_emit_store(&c->emit, c->cc_s, sign);
}

static u32 bool_combine(DirectCtx* c, u32 op, u32 left, u32 right) {
    return op < 3 ? spv_emit_bool_binary(&c->emit, c->bool_type, op, left, right) : left;
}

static int cc_valid(u32 code) {
    return code <= 14 || code == 15 || (code >= 16 && code <= 23) || code == 30 || code == 31;
}
static int flow_has_cc(int op) {
    return op == O_BRA || op == O_BRX || op == O_SYNC || op == O_BRK || op == O_CONT || op == O_EXIT || op == O_RET ||
           op == O_KIL;
}
static u32 cc_not(DirectCtx* c, u32 x) { return spv_emit_bool_not(&c->emit, c->bool_type, x); }
static u32 cc_and(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 0, x, y); }
static u32 cc_or(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 1, x, y); }
static u32 cc_xor(DirectCtx* c, u32 x, u32 y) { return spv_emit_bool_binary(&c->emit, c->bool_type, 2, x, y); }
static u32 cc_test(DirectCtx* c, u32 code) {
    if (code == 0 || code == 15) return code == 15 ? c->true_value : c->false_value;
    u32 s = spv_emit_load(&c->emit, c->bool_type, c->cc_s);
    u32 z = spv_emit_load(&c->emit, c->bool_type, c->cc_z);
    u32 carry = spv_emit_load(&c->emit, c->bool_type, c->cc_c);
    u32 overflow = spv_emit_load(&c->emit, c->bool_type, c->cc_o);
    switch (code) {
        case 1: return cc_xor(c, cc_and(c, s, cc_not(c, z)), overflow);
        case 2: return cc_and(c, cc_not(c, s), z);
        case 3: return cc_xor(c, s, cc_or(c, z, overflow));
        case 4: return cc_not(c, cc_or(c, cc_xor(c, s, overflow), z));
        case 5: return cc_not(c, z);
        case 6: return cc_not(c, cc_xor(c, s, overflow));
        case 7: return cc_not(c, cc_and(c, s, z));
        case 8: return cc_and(c, s, z);
        case 9: return cc_xor(c, s, overflow);
        case 10: return z;
        case 11: return cc_or(c, cc_xor(c, s, overflow), z);
        case 12: return cc_xor(c, cc_not(c, s), cc_or(c, overflow, z));
        case 13: return cc_or(c, s, cc_not(c, z));
        case 14: return cc_xor(c, cc_or(c, cc_not(c, s), z), overflow);
        case 16: return cc_not(c, overflow);
        case 17: return cc_not(c, carry);
        case 18: return cc_not(c, s);
        case 19: return cc_or(c, z, cc_not(c, carry));
        case 20: return cc_and(c, carry, cc_not(c, z));
        case 21: return s;
        case 22: return carry;
        case 23: return overflow;
        case 30: return cc_or(c, s, z);
        case 31: return cc_not(c, cc_or(c, s, z));
        default: return c->false_value;
    }
}

static u32 integer_compare(DirectCtx* c, u32 i32_type, u64 w, u32 left, u32 right) {
    u32 cmp = B(w, 49, 3);
    if (cmp == 0 || cmp == 7) return cmp == 7 ? c->true_value : c->false_value;
    if (B(w, 43, 1)) {
        /* the high words decide unless equal: LE and GE take LT and GT there, equality goes to the carry */
        u64 plain = w & ~(1ull << 43);
        u64 strict = cmp == 3 ? 1 : cmp == 6 ? 4 : cmp;
        u32 base = integer_compare(c, i32_type, (plain & ~(7ull << 49)) | (strict << 49), left, right);
        u32 equal = integer_compare(c, i32_type, (plain & ~(7ull << 49)) | (2ull << 49), left, right);
        u32 carry = spv_emit_load(&c->emit, c->bool_type, c->cc_c);
        u32 zero = spv_emit_load(&c->emit, c->bool_type, c->cc_z);
        u32 no_carry = spv_emit_bool_not(&c->emit, c->bool_type, carry);
        if (cmp == 1 || cmp == 6)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal, cmp == 1 ? no_carry : carry));
        if (cmp == 2) return spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal, zero);
        if (cmp == 3)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal,
                                     spv_emit_bool_binary(&c->emit, c->bool_type, 1, zero, no_carry)));
        if (cmp == 4)
            return spv_emit_bool_binary(
                &c->emit, c->bool_type, 1, base,
                spv_emit_bool_binary(&c->emit, c->bool_type, 0, equal,
                                     spv_emit_bool_binary(&c->emit, c->bool_type, 0,
                                                          spv_emit_bool_not(&c->emit, c->bool_type, zero), carry)));
        if (cmp == 5)
            return spv_emit_bool_binary(&c->emit, c->bool_type, 1, base,
                                        spv_emit_bool_not(&c->emit, c->bool_type, zero));
    }
    if (cmp == 2) return spv_emit_i_equal(&c->emit, c->bool_type, left, right);
    if (cmp == 5) return spv_emit_i_not_equal(&c->emit, c->bool_type, left, right);
    u32 less, greater;
    if (B(w, 48, 1)) {
        u32 lhs = spv_emit_bitcast(&c->emit, i32_type, left);
        u32 rhs = spv_emit_bitcast(&c->emit, i32_type, right);
        less = spv_emit_s_greater(&c->emit, c->bool_type, rhs, lhs);
        greater = spv_emit_s_greater(&c->emit, c->bool_type, lhs, rhs);
    } else {
        less = spv_emit_u_less(&c->emit, c->bool_type, left, right);
        greater = spv_emit_u_less(&c->emit, c->bool_type, right, left);
    }
    if (cmp == 1) return less;
    if (cmp == 4) return greater;
    return spv_emit_bool_not(&c->emit, c->bool_type, cmp == 3 ? greater : less);
}

static u32 float_mod(DirectCtx* c, u32 value, int absolute, int negative) {
    if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FABS, value);
    if (negative) value = spv_emit_fnegate(&c->emit, c->f32_type, value);
    return value;
}

static u32 double_read(DirectCtx* c, u32 vec2_u32, u32 index) {
    u32 low = reg_read(c, index);
    u32 high = reg_read(c, index < 254 ? index + 1 : 255);
    u32 bits = spv_emit_vec2_construct(&c->emit, vec2_u32, low, high);
    return spv_emit_bitcast(&c->emit, c->f64_type, bits);
}

static u32 double_mod(DirectCtx* c, u32 value, int absolute, int negative) {
    if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_FABS, value);
    if (negative) value = spv_emit_fnegate(&c->emit, c->f64_type, value);
    return value;
}

static void double_write(DirectCtx* c, u32 vec2_u32, u32 index, u32 value) {
    u32 bits = spv_emit_bitcast(&c->emit, vec2_u32, value);
    reg_write(c, index, spv_emit_vec4_extract(&c->emit, c->u32_type, bits, 0));
    if (index < 254) reg_write(c, index + 1, spv_emit_vec4_extract(&c->emit, c->u32_type, bits, 1));
}

static u32 half_pair(DirectCtx* c, u32 vec2, u32 raw, u32 swizzle) {
    if (swizzle == 1) {
        u32 scalar = spv_emit_bitcast(&c->emit, c->f32_type, raw);
        return spv_emit_vec2_construct(&c->emit, vec2, scalar, scalar);
    }
    u32 unpacked = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_UNPACK_HALF2X16, raw);
    if (swizzle == 0) return unpacked;
    u32 scalar = spv_emit_vec4_extract(&c->emit, c->f32_type, unpacked, swizzle == 3);
    return spv_emit_vec2_construct(&c->emit, vec2, scalar, scalar);
}

static u32 half_pair_mod(DirectCtx* c, u32 vec2, u32 pair, int absolute, int negative) {
    if (absolute) pair = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_FABS, pair);
    if (negative) pair = spv_emit_fnegate(&c->emit, vec2, pair);
    return pair;
}

static u32 half_pair_result(DirectCtx* c, u32 vec2, u32 pair, u32 old, u32 merge, int saturate) {
    if (saturate) {
        u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
        u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
        pair = spv_emit_glsl_ternary(&c->emit, vec2, c->glsl, SPV_GLSL_FCLAMP, pair,
                                     spv_emit_vec2_construct(&c->emit, vec2, zero, zero),
                                     spv_emit_vec2_construct(&c->emit, vec2, one, one));
    }
    if (merge == 1)
        return spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_vec4_extract(&c->emit, c->f32_type, pair, 0));
    u32 packed = spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair);
    if (merge == 2 || merge == 3) {
        u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, merge == 2 ? 0xffffu : 0xffff0000u);
        return spv_emit_bitwise_or_u32(
            &c->emit, c->u32_type,
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, old, spv_emit_not_u32(&c->emit, c->u32_type, mask)),
            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed, mask));
    }
    return packed;
}

static u32 float_imm20(DirectCtx* c, u64 w) {
    return spv_emit_constant_u32(&c->emit, c->f32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
}

static u32 float_imm32(DirectCtx* c, u64 w) { return spv_emit_constant_u32(&c->emit, c->f32_type, B(w, 20, 32)); }

static u32 float_saturate(DirectCtx* c, u32 value, int saturate) {
    if (!saturate) return value;
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
    return spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FCLAMP, value, zero, one);
}

static void alpha_test(DirectCtx* c, u32 push, u32 push_pointer, u32 alpha) {
    u32 index[3];
    for (u32 i = 0; i < 3; i++) index[i] = spv_emit_constant_u32(&c->emit, c->u32_type, i);
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[0], c->f32_type);
    u32 function = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[1], c->f32_type);
    u32 reference = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[2], c->f32_type);
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 active = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled, zero);
    u32 pass =
        spv_emit_fcompare(&c->emit, c->bool_type, 2, function, spv_emit_constant_f32(&c->emit, c->f32_type, 7.0f));
    for (u32 mode = 1; mode <= 6; mode++) {
        u32 tag = spv_emit_constant_f32(&c->emit, c->f32_type, (float)mode);
        u32 selected = spv_emit_fcompare(&c->emit, c->bool_type, 2, function, tag);
        u32 comparison = spv_emit_fcompare(&c->emit, c->bool_type, mode, alpha, reference);
        u32 result = spv_emit_bool_binary(&c->emit, c->bool_type, 0, selected, comparison);
        pass = spv_emit_bool_binary(&c->emit, c->bool_type, 1, pass, result);
    }
    u32 fail = spv_emit_bool_not(&c->emit, c->bool_type, pass);
    fail = spv_emit_bool_binary(&c->emit, c->bool_type, 0, active, fail);
    u32 discard = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, fail, discard, merge);
    spv_emit_label(&c->emit, discard);
    spv_emit_kill(&c->emit);
    spv_emit_label(&c->emit, merge);
}

static void vertex_depth_fix(DirectCtx* c, u32 position, u32 push, u32 push_pointer, u32 vec4_type) {
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, member, component, c->f32_type);
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 test = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled, zero);
    u32 adjust = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, test, adjust, merge);
    spv_emit_label(&c->emit, adjust);
    u32 value = spv_emit_load(&c->emit, vec4_type, position);
    u32 z = spv_emit_vec4_extract(&c->emit, c->f32_type, value, 2);
    u32 w = spv_emit_vec4_extract(&c->emit, c->f32_type, value, 3);
    u32 sum = spv_emit_fadd(&c->emit, c->f32_type, z, w);
    spv_emit_no_contraction(&c->emit, sum);
    u32 half = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
    u32 depth = spv_emit_fmul(&c->emit, c->f32_type, sum, half);
    spv_emit_no_contraction(&c->emit, depth);
    value = spv_emit_vec4_insert(&c->emit, vec4_type, value, depth, 2);
    spv_emit_store(&c->emit, position, value);
    spv_emit_branch(&c->emit, merge);
    spv_emit_label(&c->emit, merge);
}

/* A constant buffer word is loaded as an integer: a float load may flush the bits of an integer that look like a
   denormal (Adreno does, in a shader that declares DenormFlushToZero), and a jump table of small offsets reads 0. */
static u32 cbuf_word(DirectCtx* c, u64 w) {
    u32 idx = B(w, 34, 5), off = B(w, 20, 14) * 4;
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 element = spv_emit_constant_u32(&c->emit, c->u32_type, off >> 4);
    u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (off >> 2) & 3);
    return spv_emit_uniform_component(&c->emit, c->cbuf_pointer[idx], c->cbuf[idx], member, element, component,
                                      c->u32_type);
}
static u32 cbuf_read(DirectCtx* c, u64 w) { return spv_emit_bitcast(&c->emit, c->f32_type, cbuf_word(c, w)); }
static u32 integer_right(DirectCtx* c, u64 w, int kind) {
    if (kind == 0) return reg_read(c, B(w, 20, 8));
    if (kind == 1) return cbuf_word(c, w);
    u32 imm = B(w, 20, 19);
    return spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 56, 1) ? imm | 0xfff80000u : imm);
}
/* Two's-complement signed operands correct the unsigned high word by subtracting
   the other operand when their sign bit is set. The low word needs no correction. */
static u32 integer_mul_high(DirectCtx* c, u32 lhs, u32 rhs, int signed_lhs, int signed_rhs) {
    u32 high = spv_emit_umul_high_u32(&c->emit, c->u32_type, lhs, rhs);
    u32 sign = spv_emit_constant_u32(&c->emit, c->u32_type, 31);
    if (signed_lhs)
        high = spv_emit_isub_u32(
            &c->emit, c->u32_type, high,
            spv_emit_imul_u32(&c->emit, c->u32_type, rhs, spv_emit_shift_right_u32(&c->emit, c->u32_type, lhs, sign)));
    if (signed_rhs)
        high = spv_emit_isub_u32(
            &c->emit, c->u32_type, high,
            spv_emit_imul_u32(&c->emit, c->u32_type, lhs, spv_emit_shift_right_u32(&c->emit, c->u32_type, rhs, sign)));
    return high;
}
static int gbuf_tag(int stage, u32 index, u32 offset) {
    u32 end = stage == ST_CS ? 0x410 : 0x830;
    /* The vertex system buffer also supplies a bare 64-bit output pointer at 0xb0. */
    if (stage == ST_VS && index == 0 && offset == 0xb0) return 0xb1;
    return index == 0 && offset >= 0x110 && offset < end && !(offset & 15) ? (int)offset + 1 : 0;
}
static int texs_outputs(u64 w, u32 regs[4], u32 comps[4]) {
    u32 da = B(w, 0, 8), db = B(w, 28, 8), sw = B(w, 50, 3);
    int n = 0;
    int i;
    if (db == 255) {
        static const int tab[8][2] = {{0, -1}, {1, -1}, {2, -1}, {3, -1}, {0, 1}, {0, 3}, {1, 3}, {2, 3}};
        for (i = 0; i < 2; i++)
            if (tab[sw][i] >= 0) {
                regs[n] = da == 255 ? 255 : da + (u32)n;
                comps[n++] = (u32)tab[sw][i];
            }
    } else {
        static const int tab[5][4] = {{0, 1, 2, -1}, {0, 1, 3, -1}, {0, 2, 3, -1}, {1, 2, 3, -1}, {0, 1, 2, 3}};
        if (sw > 4) sw = 4;
        for (i = 0; i < 4; i++)
            if (tab[sw][i] >= 0) {
                regs[n] = n < 2 ? da + (u32)n : db + (u32)(n - 2);
                comps[n++] = (u32)tab[sw][i];
            }
    }
    return n;
}
static u32 direct_mask_count(u32 mask) {
    u32 count = 0;
    for (; mask; mask &= mask - 1) count++;
    return count;
}
/* Whether the hardware writes reg when it runs w. Backward scans for a register's
   definition need the whole range: loads, 64-bit results and masked texture results
   write registers after the one in bits 0-7, and stores and predicate setters keep a
   source there instead. Forms the translator rejects are described too, so a scan
   never skips over a write. */
int shader_writes_reg(u64 w, int op, u32 reg) {
    u32 d = B(w, 0, 8), count = 1;
    if (reg == 255) return 0;
    switch (op) {
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_PSETP:
        case O_CSETP:
        case O_VSETP:
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I:
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_R2B:
        case O_STG:
        case O_STS:
        case O_STL:
        case O_ST:
        case O_STP:
        case O_RED:
        case O_AST:
        case O_SUST:
        case O_SURED:
        case O_BRA:
        case O_BRX:
        case O_SSY:
        case O_SYNC:
        case O_PBK:
        case O_BRK:
        case O_PCNT:
        case O_CONT:
        case O_EXIT:
        case O_KIL:
        case O_JMP:
        case O_CAL:
        case O_RET:
        case O_PRET:
        case O_PEXIT:
        case O_NOP:
        case O_BAR:
        case O_MEMBAR:
        case O_DEPBAR:
        case O_VOTE_VTG:
        case O_CCTL:
        case O_CCTLL:
        case O_BPT:
        case O_IDE:
        case O_SETCRSPTR: return 0;
        case O_TLD4S: {
            /* Two destination pairs, the second named in bits 28-35, or one packed register for each. */
            u32 second = B(w, 28, 8);
            if (B(w, 55, 1)) return d == reg || second == reg;
            return (d != 255 && reg - d < 2) || (second != 255 && reg - second < 2);
        }
        case O_TEXS:
        case O_TLDS: {
            u32 regs[4], comps[4];
            int outputs = texs_outputs(w, regs, comps);
            int k;
            if (!B(w, 59, 1)) return outputs && (d == reg || (outputs > 2 && B(w, 28, 8) == reg));
            for (k = 0; k < outputs; k++)
                if (regs[k] == reg) return 1;
            return 0;
        }
        case O_TEX:
        case O_TEX_B:
        case O_TLD:
        case O_TLD_B:
        case O_TLD4:
        case O_TLD4_B:
        case O_TMML:
        case O_TXD: count = direct_mask_count(B(w, 31, 4) ? B(w, 31, 4) : 15); break;
        case O_TXQ:
        case O_TXQ_B: count = direct_mask_count(B(w, 31, 4)); break;
        case O_SULD:
            count = !B(w, 52, 1) ? direct_mask_count(B(w, 20, 4)) : B(w, 20, 3) == 5 ? 2 : B(w, 20, 3) >= 6 ? 4 : 1;
            break;
        case O_LDG:
        case O_LDS:
        case O_LDL:
        case O_LD: count = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1; break;
        case O_LDC: count = B(w, 48, 3) == 5 ? 2 : 1; break;
        case O_ALD: count = B(w, 47, 2) + 1; break;
        case O_ATOM: count = B(w, 49, 3) == 2 || B(w, 49, 3) == 5 ? 2 : 1; break;
        case O_ATOMS: count = B(w, 28, 2) >= 2 ? 2 : 1; break;
        case O_SUATOM: count = B(w, 51, 3) == 2 || B(w, 51, 3) == 3 || B(w, 51, 3) == 7 ? 2 : 1; break;
        case O_ATOM_CAS:
        case O_ATOMS_CAS:
        case O_SUATOM_CAS:
        case O_CS2R: count = 2; break;
        case O_DADD_R:
        case O_DADD_C:
        case O_DADD_I:
        case O_DMUL_R:
        case O_DMUL_C:
        case O_DMUL_I:
        case O_DFMA_R:
        case O_DFMA_RC:
        case O_DFMA_CR:
        case O_DFMA_I:
        case O_DMNMX_R:
        case O_DMNMX_C:
        case O_DMNMX_I: count = 2; break;
        case O_F2F_R:
        case O_F2F_C:
        case O_F2F_I:
        case O_F2I_R:
        case O_F2I_C:
        case O_F2I_I:
        case O_I2F_R:
        case O_I2F_C:
        case O_I2F_I:
        case O_I2I_R:
        case O_I2I_C:
        case O_I2I_I: count = B(w, 8, 2) == 3 ? 2 : 1; break;
        default: break;
    }
    return d != 255 && reg >= d && reg - d < count;
}

/* Whether w may write a predicate register. LOP, LOP3 and LEA name theirs in bits 48-50, PT for none. */
static int direct_writes_pred(u64 w, int op) {
    switch (op) {
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I:
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I:
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I:
        case O_PSETP:
        case O_CSETP:
        case O_VSETP:
        case O_FCHK_R:
        case O_FCHK_C:
        case O_FCHK_I:
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_SHFL:
        case O_VOTE: return 1;
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I:
        case O_LOP3_R:
        case O_LOP3_C:
        case O_LOP3_I:
        case O_LEA_HR:
        case O_LEA_HC:
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI: return B(w, 48, 3) != 7;
        default: return 0;
    }
}

/* Where control can arrive from the flow instruction at k, besides the next instruction: a branch, SSY, PBK or PCNT
   target, a jump table's entries, a CAL's callee and the return point after it, a PRET's return point. -1 when that
   can't be known (a JMP, or a jump table that isn't found). elsewhere[t] is set for arrivals that come from other
   instructions than k: a PCNT's target takes CONTs from later in the loop, and a return point takes RETs. */
static int direct_flow_targets(const u64* insn, u32 n, u32 k, u32 targets[64], u8 elsewhere[64]) {
    const Op* flow = decode(insn[k]);
    if (!flow) return 0;
    switch (flow->id) {
        case O_JMP: return -1;
        case O_BRX: {
            int count = shader_brx_targets(insn, k, n, targets, 64);
            int t;
            for (t = 0; t < count; t++) elsewhere[t] = 0;
            return count;
        }
        case O_CAL:
            targets[0] = bra_target(insn[k], k);
            targets[1] = k + 1 + ((k + 1 - 10) % 4 == 0);
            elsewhere[0] = 0;
            elsewhere[1] = 1;
            return 2;
        case O_PRET:
            targets[0] = bra_target(insn[k], k);
            elsewhere[0] = 1;
            return 1;
        case O_BRA:
        case O_SSY:
        case O_PBK:
        case O_PCNT:
            targets[0] = bra_target(insn[k], k);
            elsewhere[0] = flow->id == O_PCNT;
            return 1;
        default: return 0;
    }
}

/* Whether every path to use passes def and then stays in (def, use], which the caller has checked writes nothing
   of interest. Paths enter the region only by falling through from def, unless control arrives in it from before
   def (a branch, or the SYNC or BRK of an SSY or PBK set up before def), from a loop or a jump after use, or from
   instructions that aren't known (CONTs, RETs). */
static int direct_def_dominates(const u64* insn, u32 n, u32 def, u32 use) {
    u32 k;
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int jump;
        int t;
        if (!IDX_OK(k)) continue;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) return 0;
        jump = decode(insn[k])->id != O_SSY && decode(insn[k])->id != O_PBK;
        for (t = 0; t < count; t++) {
            if (targets[t] <= def || targets[t] > use) continue;
            if (elsewhere[t] || k < def || (jump && k > use)) return 0;
        }
    }
    return 1;
}

/* The instruction that last wrote reg before index, as its first destination, or -1. In index's basic block, a
   write under a predicate reaches only an instruction under the same one, with no predicate written between them:
   the compiler loads a handle and uses it under the guard of one side of a branch it made predicated. Across flow,
   the write has to be unconditional and dominate index. */
static int direct_reaching_def(const u64* insn, u32 n, u32 index, u32 reg) {
    u32 guard;
    int pred_written = 0, crossed = 0;
    /* where index's basic block starts: the last place control can arrive at before it */
    u32 block_start = 11;
    u32 k;
    u32 j;
    if (reg == 255) return -1;
    guard = B(insn[index], 16, 4);
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        if (!IDX_OK(k)) continue;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) return -1;
        for (t = 0; t < count; t++)
            if (targets[t] > block_start && targets[t] <= index) block_start = targets[t];
    }
    for (j = index; j-- > 11;) {
        const Op* op;
        if (!IDX_OK(j)) continue;
        op = decode(insn[j]);
        if (!op) return -1;
        if (j < block_start || op->id == O_BRA || op->id == O_BRX || op->id == O_CAL || op->id == O_RET ||
            op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT || op->id == O_EXIT || op->id == O_KIL)
            crossed = 1;
        if (!shader_writes_reg(insn[j], op->id, reg)) {
            pred_written |= direct_writes_pred(insn[j], op->id);
            continue;
        }
        /* A write that reaches reg from a lower destination leaves no single source. */
        if (B(insn[j], 0, 8) != reg) return -1;
        if (B(insn[j], 16, 4) != 7 && (crossed || B(insn[j], 16, 4) != guard || pred_written)) return -1;
        return !crossed || direct_def_dominates(insn, n, j, index) ? (int)j : -1;
    }
    return -1;
}

static int direct_track_const(const u64* insn, u32 n, u32 index, u32 reg, u32* value, int depth) {
    int j = depth > 6 ? -1 : direct_reaching_def(insn, n, index, reg);
    u64 w;
    int op;
    if (j < 0) return 0;
    w = insn[j];
    op = decode(w)->id;
    if (op == O_MOV32I) {
        *value = B(w, 20, 32);
        return 1;
    }
    if (op == O_MOV_R) return direct_track_const(insn, n, (u32)j, B(w, 20, 8), value, depth + 1);
    return 0;
}

/* Resolve a register supplied image handle to the constant buffer word read in this basic block. */
static int direct_track_image_handle(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, int depth) {
    int j = depth > 6 ? -1 : direct_reaching_def(insn, n, index, reg);
    u64 w;
    int op;
    if (j < 0) return 0;
    w = insn[j];
    op = decode(w)->id;
    if (op == O_MOV_C) {
        *bank = B(w, 34, 5);
        *off = B(w, 20, 14);
        return *bank < 18;
    }
    if (op == O_LDC && B(w, 48, 3) >= 4 && B(w, 48, 3) <= 5) {
        i32 byte_off = (i16)B(w, 20, 16);
        *bank = B(w, 36, 5);
        *off = (u32)byte_off / 4;
        /* Keep the legacy first-descriptor behavior for indexed handles.
           Runtime descriptor selection for arbitrary indices needs a separate path. */
        return *bank < 18 && byte_off >= 0 && !(byte_off & 3);
    }
    if (op == O_MOV_R) return direct_track_image_handle(insn, n, (u32)j, B(w, 20, 8), bank, off, depth + 1);
    return 0;
}

/* Split texture and sampler handles can be ORed or added after separate cbuf
   reads, the second one either through a register or as the instruction's own
   constant buffer operand. Keep both sources so descriptor binding reproduces
   the guest handle. */
static int direct_track_image_handle_pair(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, u32* bank2,
                                          u32* off2, int depth) {
    int j;
    u64 w;
    int op;
    int plain_or;
    int plain_add;
    u32 lhs;
    u32 rhs;
    *bank2 = *off2 = 0;
    if (direct_track_image_handle(insn, n, index, reg, bank, off, depth)) return 1;
    j = depth > 6 ? -1 : direct_reaching_def(insn, n, index, reg);
    if (j < 0) return 0;
    w = insn[j];
    op = decode(w)->id;
    if (op == O_MOV_R)
        return direct_track_image_handle_pair(insn, n, (u32)j, B(w, 20, 8), bank, off, bank2, off2, depth + 1);
    plain_or = (op == O_LOP_R || op == O_LOP_C) && B(w, 41, 2) == 1 && !B(w, 39, 2) && !B(w, 47, 1);
    plain_add = (op == O_IADD_R || op == O_IADD_C) && !B(w, 43, 1) && !B(w, 47, 1) && !B(w, 48, 2);
    if (!plain_or && !plain_add) return 0;
    lhs = B(w, 8, 8);
    if (op == O_LOP_C || op == O_IADD_C) {
        u32 cbank = B(w, 34, 5), coff = B(w, 20, 14);
        if (cbank >= 18) return 0;
        if (lhs == 255) {
            *bank = cbank;
            *off = coff;
            return 1;
        }
        *bank2 = cbank;
        *off2 = coff;
        return direct_track_image_handle(insn, n, (u32)j, lhs, bank, off, depth + 1) ? 2 : 0;
    }
    rhs = B(w, 20, 8);
    if (lhs == 255) return direct_track_image_handle_pair(insn, n, (u32)j, rhs, bank, off, bank2, off2, depth + 1);
    if (rhs == 255) return direct_track_image_handle_pair(insn, n, (u32)j, lhs, bank, off, bank2, off2, depth + 1);
    return direct_track_image_handle(insn, n, (u32)j, lhs, bank, off, depth + 1) &&
                   direct_track_image_handle(insn, n, (u32)j, rhs, bank2, off2, depth + 1)
               ? 2
               : 0;
}
static u32 cbuf_read_dynamic(DirectCtx* c, u32 bank, u32 address) {
    u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
    u32 two = spv_emit_constant_u32(&c->emit, c->u32_type, 2);
    u32 three = spv_emit_constant_u32(&c->emit, c->u32_type, 3);
    u32 element = spv_emit_shift_right_u32(&c->emit, c->u32_type, address, four);
    u32 component = spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                             spv_emit_shift_right_u32(&c->emit, c->u32_type, address, two), three);
    return spv_emit_uniform_component(&c->emit, c->cbuf_pointer[bank], c->cbuf[bank], c->zero, element, component,
                                      c->u32_type);
}

static u32 double_source(DirectCtx* c, u32 vec2_u32, u64 w, int kind) {
    if (kind == 0) return double_read(c, vec2_u32, B(w, 20, 8));
    u32 low = c->zero, high;
    if (kind == 1) {
        u32 bank = B(w, 34, 5), offset = B(w, 20, 14) * 4;
        if (!(offset & 4)) low = cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset));
        high = cbuf_read_dynamic(c, bank, spv_emit_constant_u32(&c->emit, c->u32_type, offset | 4u));
    } else
        high = spv_emit_constant_u32(&c->emit, c->u32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
    return spv_emit_bitcast(&c->emit, c->f64_type, spv_emit_vec2_construct(&c->emit, vec2_u32, low, high));
}

static u32 smap_word(DirectCtx* c, u32 index) {
    u32 pointer = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->smap, c->zero, index);
    return spv_emit_load(&c->emit, c->u32_type, pointer);
}

static u32 gbuf_word_index(DirectCtx* c, int stage, u32 slot, u32 word) {
    /* Compute dispatches bind each guest buffer as one contiguous storage buffer.
       The graphics page table descriptor is not present in their descriptor set. */
    if (stage == ST_CS) return word;
    u32 h = smap_word(c, spv_emit_constant_u32(&c->emit, c->u32_type, slot));
    u32 no_map = spv_emit_i_equal(&c->emit, c->bool_type, h, spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu));
    u32 offset = smap_word(c, spv_emit_constant_u32(&c->emit, c->u32_type, 16 + slot));
    u32 count = smap_word(c, spv_emit_constant_u32(&c->emit, c->u32_type, 32 + slot));
    u32 valid = spv_emit_u_less(&c->emit, c->bool_type, word, count);
    u32 mapped_word = spv_emit_iadd_u32(&c->emit, c->u32_type, word, offset);
    u32 page =
        spv_emit_shift_right_u32(&c->emit, c->u32_type, mapped_word, spv_emit_constant_u32(&c->emit, c->u32_type, 10));
    u32 safe_h = spv_emit_select(&c->emit, c->u32_type, no_map, c->zero, h);
    u32 safe_page = spv_emit_select(&c->emit, c->u32_type, no_map, c->zero, page);
    u32 page_ix = spv_emit_iadd_u32(&c->emit, c->u32_type, safe_h, safe_page);
    u32 mapped_page = smap_word(c, page_ix);
    u32 high =
        spv_emit_shift_left_u32(&c->emit, c->u32_type, mapped_page, spv_emit_constant_u32(&c->emit, c->u32_type, 10));
    u32 low = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mapped_word,
                                       spv_emit_constant_u32(&c->emit, c->u32_type, 1023));
    u32 mapped = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, high, low);
    mapped = spv_emit_select(&c->emit, c->u32_type, valid, mapped,
                             spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu));
    return spv_emit_select(&c->emit, c->u32_type, no_map, word, mapped);
}

static u32 next_instruction(u32 index, u32 limit) {
    do { index++; } while (index < limit && !IDX_OK(index));
    return index;
}

static int texture_sample_supported(u64 w, int bindless) {
    u32 type = B(w, 28, 3);
    u32 offset = B(w, bindless ? 36 : 54, 1);
    /* Type 5 denotes a 3D array, which Vulkan cannot sample. Other dimensions
       share the same LOD and bias fields in the bound and bindless encodings. */
    return type != 5 && !(type == 4 && B(w, 50, 1)) && (!offset || type <= 4);
}

static int supported(u64 w, int op) {
    switch (op) {
        case O_EXIT: return cc_valid(B(w, 0, 5));
        case O_NOP:
        case O_DEPBAR:
        case O_MEMBAR:
        case O_VOTE_VTG: return 1;
        case O_VOTE: return B(w, 48, 2) < 3;
        case O_FLO_R:
        case O_FLO_C:
        case O_FLO_I: return 1;
        case O_POPC_R:
        case O_POPC_C:
        case O_POPC_I: return 1;
        case O_SHFL: return 1;
        case O_FSWZADD: return 1;
        case O_VMNMX: return 1;
        case O_VMAD:
            return (((w >> 48) & ~3ull) == 0x5f04 || ((w >> 48) & ~3ull) == 0x5f00) && !B(w, 47, 1) && !B(w, 51, 5);
        case O_VSETP: return (w >> 48) == 0x50f4;
        case O_RED: return global_atomic_opcode(w, 1) != 0;
        case O_ATOM: return global_atomic_opcode(w, 0) != 0;
        case O_ATOMS: return shared_atomic_opcode(w) != 0;
        case O_LDL:
        case O_STL: return B(w, 48, 3) <= 7;
        case O_LDS:
        case O_STS: return B(w, 48, 3) <= 7;
        /* BAR.SYNC 0 for the whole workgroup: arrivals, reductions and thread counts are other barriers */
        case O_BAR:
            return (w & 0x0000009B00000000ull) == 0x0000008000000000ull && B(w, 43, 1) && !B(w, 8, 8) && B(w, 44, 1) &&
                   !B(w, 20, 12);
        case O_S2R: {
            u32 sr = B(w, 20, 8);
            return sr == 0 || sr == 0x11 || sr == 0x12 || sr == 0x13 || sr == 0x1d || sr == 0x1e || sr == 0x1f ||
                   (sr >= 0x21 && sr <= 0x23) || (sr >= 0x25 && sr <= 0x27) || sr == 0x38 || sr == 0x39 || sr == 0x3a ||
                   sr == 0x3b || sr == 0x3c || sr == 0x50 || sr == 0x52;
        }
        /* ALD and AST: a patch's attributes (bit 31) are its tessellation levels and 30 vectors from 0x20. Which stage
           has which is stage_has's. Bit 32 of ALD is ignored, as Eden does: evaluation programs set it to read the
           tessellation coordinate */
        case O_ALD: {
            u32 addr = B(w, 20, 10), count = B(w, 47, 2) + 1;
            if (B(w, 8, 8) != 255 || B(w, 0, 8) + count > 255) return 0;
            if (B(w, 31, 1)) return addr + count * 4 <= 0x200;
            return (addr >= 0x70 && addr + count * 4 <= 0x280) ||
                   ((addr == 0x60 || addr == 0x2f4 || addr == 0x2f8 || addr == 0x2fc) && count == 1) ||
                   (addr == 0x2f0 && count <= 2);
        }
        case O_AST: {
            u32 addr = B(w, 20, 10), count = B(w, 47, 2) + 1;
            if (B(w, 8, 8) != 255 || !(B(w, 0, 8) + count <= 255 || (B(w, 0, 8) == 255 && count == 1))) return 0;
            if (B(w, 31, 1)) return addr + count * 4 <= 0x200;
            return ((addr == 0x64 || addr == 0x6c) && count == 1) || (addr >= 0x70 && addr + count * 4 <= 0x80) ||
                   (addr >= 0x80 && addr + count * 4 <= 0x280) || (addr >= 0x2c0 && addr + count * 4 <= 0x2e0);
        }
        /* OUT emits and ends geometry primitives on stream 0, its third operand */
        case O_OUT_R: return B(w, 20, 8) == 255 && (B(w, 39, 1) || B(w, 40, 1));
        case O_OUT_I: return !B(w, 20, 2) && (B(w, 39, 1) || B(w, 40, 1));
        /* ISBERD in its plain form (no skew, output read or mode, so its size doesn't matter) passes the index it reads
           (the vertex) through, as Eden translates it */
        case O_ISBERD: return B(w, 8, 8) != 255 && !B(w, 24, 8) && !B(w, 31, 4) && !B(w, 47, 2);
        case O_KIL: return cc_valid(B(w, 0, 5));
        case O_BRA: return cc_valid(B(w, 0, 5));
        case O_CAL: return 1;
        case O_PRET: return 1;
        case O_RET: return cc_valid(B(w, 0, 5));
        case O_BRX: return cc_valid(B(w, 0, 5));
        case O_SSY:
        case O_PBK:
        case O_PCNT: return 1;
        case O_SYNC:
        case O_BRK:
        case O_CONT: return cc_valid(B(w, 0, 5));
        case O_MOV32I: return B(w, 12, 4) == 1 || B(w, 12, 4) == 15;
        case O_MOV_R:
        case O_MOV_C:
        case O_MOV_I: return 1;
        /* saturating adds, IMNMX's XLO/XHI, 64-bit conversions and MUFU's RCP64H/RSQ64H aren't translated */
        case O_IADD32I: return !B(w, 54, 1);
        case O_IMUL_R:
        case O_IMUL_C:
        case O_IMUL_I:
        case O_IMUL32I:
        case O_IMAD_R:
        case O_IMAD_RC:
        case O_IMAD_CR:
        case O_IMAD_I:
        case O_IMAD32I: return 1;
        case O_IADD3_R: return 1;
        case O_IADD3_C:
        case O_IADD3_I: return 1;
        case O_IMNMX_R:
        case O_IMNMX_C:
        case O_IMNMX_I: return !B(w, 43, 2);
        case O_IADD_R:
        case O_IADD_C:
        case O_IADD_I: return !B(w, 50, 1);
        case O_LDG:
        case O_STG: return B(w, 48, 3) <= 7;
        case O_TXQ:
        case O_TXQ_B: return B(w, 22, 6) == 1;
        case O_TLD4S: return 1;
        case O_TLD4:
            return (B(w, 28, 3) <= 3 || B(w, 28, 3) >= 6) &&
                   (!B(w, 54, 2) || (B(w, 54, 2) <= 2 && (B(w, 28, 3) >> 1) == 1));
        case O_TLD4_B:
            return (B(w, 28, 3) <= 3 || B(w, 28, 3) >= 6) &&
                   (!B(w, 36, 2) || (B(w, 36, 2) <= 2 && (B(w, 28, 3) >> 1) == 1));
        case O_I2I_R:
        case O_I2I_C:
        case O_I2I_I:
            return B(w, 8, 2) <= 2 && B(w, 10, 2) <= 2 && B(w, 41, 3) <= 3 &&
                   (B(w, 10, 2) != 1 || !(B(w, 41, 3) & 1)) && (B(w, 10, 2) != 2 || !B(w, 41, 3));
        case O_LOP32I: return !B(w, 52, 1);
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I: return !B(w, 47, 1);
        case O_LOP3_R:
        case O_LOP3_C:
        case O_LOP3_I: return 1;
        case O_SEL_R:
        case O_SEL_C:
        case O_SEL_I: return 1;
        case O_XMAD_R: return (B(w, 50, 3) <= 2 || B(w, 50, 3) == 4) && !B(w, 47, 1) && !B(w, 38, 1);
        case O_XMAD_RC: return B(w, 50, 2) <= 2 && !B(w, 47, 1) && !B(w, 54, 1) && !B(w, 48, 2);
        case O_XMAD_CR: return B(w, 50, 2) <= 2 && !B(w, 47, 1) && !B(w, 54, 1);
        case O_XMAD_I: return (B(w, 50, 3) <= 2 || B(w, 50, 3) == 4) && !B(w, 47, 1) && !B(w, 38, 1) && !B(w, 48, 2);
        /* the plain addressing mode: IS, IL and ISL (bits 44-45) take the bank or address from the register */
        case O_LDC:
            return B(w, 48, 3) <= 5 && B(w, 36, 5) < 18 && (B(w, 48, 3) != 5 || B(w, 0, 8) < 254) && !B(w, 44, 2);
        case O_SHF_LR:
        case O_SHF_LI:
        case O_SHF_RR:
        case O_SHF_RI: return B(w, 37, 2) != 1 && !B(w, 47, 1) && !B(w, 48, 2);
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI:
        case O_LEA_HR:
        case O_LEA_HC: return 1;
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I: return B(w, 45, 2) < 3;
        case O_ICMP_R:
        case O_ICMP_RC:
        case O_ICMP_CR:
        case O_ICMP_I: return 1;
        case O_FCMP_R:
        case O_FCMP_RC:
        case O_FCMP_CR:
        case O_FCMP_I: return 1;
        case O_BFI_R:
        case O_BFI_RC:
        case O_BFI_CR:
        case O_BFI_I: return 1;
        case O_F2F_R:
        case O_F2F_C:
        case O_F2F_I:
        case O_F2I_R:
        case O_F2I_C:
        case O_F2I_I: return B(w, 8, 2) != 3 && B(w, 10, 2) != 3;
        case O_I2F_R:
        case O_I2F_C:
        case O_I2F_I:
            return (B(w, 8, 2) == 1 || B(w, 8, 2) == 2 || B(w, 8, 2) == 3) && B(w, 10, 2) != 3 &&
                   (B(w, 10, 2) != 1 || !(B(w, 41, 2) & 1));
        case O_ISCADD_R:
        case O_ISCADD_C:
        case O_ISCADD_I: return 1;
        case O_BFE_R:
        case O_BFE_C:
        case O_BFE_I: return 1;
        case O_SHL_R:
        case O_SHL_C:
        case O_SHL_I: return 1;
        case O_SHR_R:
        case O_SHR_C:
        case O_SHR_I: return 1;
        case O_ISET_R:
        case O_ISET_C:
        case O_ISET_I: return B(w, 45, 2) < 3;
        case O_FADD_R:
        case O_FADD_C:
        case O_FADD_I: return !B(w, 47, 1);
        case O_FADD32I: return !B(w, 52, 1);
        case O_FMUL_R:
        case O_FMUL_C:
        case O_FMUL_I: return !B(w, 47, 1) && B(w, 41, 3) < 7;
        case O_FMUL32I: return B(w, 53, 2) <= 1 && !B(w, 52, 1);
        case O_FFMA_R:
        case O_FFMA_RC:
        case O_FFMA_CR:
        case O_FFMA_I: return !B(w, 47, 1);
        case O_FFMA32I: return !B(w, 52, 1);
        case O_FMNMX_R:
        case O_FMNMX_C:
        case O_FMNMX_I: return !B(w, 47, 1);
        case O_IPA: {
            u32 addr = B(w, 30, 8) * 4;
            return ((addr >= 0x80 && addr < 0x280) || (addr >= 0x70 && addr < 0x80) || addr == 0x64 || addr == 0x3fc) &&
                   !B(w, 38, 1);
        }
        case O_PSETP: return B(w, 24, 2) < 3 && B(w, 45, 2) < 3;
        case O_PSET: return B(w, 24, 2) < 3 && B(w, 45, 2) < 3;
        case O_CSETP:
        case O_CSET: return cc_valid(B(w, 8, 5)) && B(w, 45, 2) < 3;
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I:
        case O_P2R_R:
        case O_P2R_C:
        case O_P2R_I: return 1;
        case O_MUFU: return B(w, 20, 4) <= 5 || B(w, 20, 4) == 8;
        case O_RRO_R:
        case O_RRO_C: return 1;
        case O_TEXS:
            return (B(w, 53, 4) == 0 || B(w, 53, 4) == 1 || B(w, 53, 4) == 2 || B(w, 53, 4) == 3 || B(w, 53, 4) == 4 ||
                    B(w, 53, 4) == 7 || B(w, 53, 4) == 8 || B(w, 53, 4) == 10 || B(w, 53, 4) == 11 ||
                    B(w, 53, 4) == 12 || B(w, 53, 4) == 13);
        /* not 6, a multisample fetch: its sample index would be dropped */
        case O_TLDS:
            return (B(w, 53, 4) == 0 || B(w, 53, 4) == 1 || B(w, 53, 4) == 2 || B(w, 53, 4) == 4 || B(w, 53, 4) == 5 ||
                    B(w, 53, 4) == 7 || B(w, 53, 4) == 8 || B(w, 53, 4) == 12);
        case O_TLD: return B(w, 28, 3) <= 4 && !B(w, 50, 1) && (!B(w, 35, 1) || B(w, 28, 3) == 2);
        case O_TLD_B:
            return B(w, 28, 3) <= 4 && (!B(w, 50, 1) || B(w, 28, 3) == 2 || B(w, 28, 3) == 3) &&
                   (!B(w, 35, 1) || B(w, 28, 3) == 2 && !B(w, 50, 1));
        /* 2D surfaces, and buffer images as storage texel buffers: their raw 32-bit loads and formatted stores */
        case O_SULD:
            return (B(w, 33, 3) == 3 && (!B(w, 52, 1) || B(w, 20, 4) == 4)) ||
                   (B(w, 33, 3) == 1 && B(w, 52, 1) && B(w, 20, 4) == 4);
        case O_SUST: return !B(w, 52, 1) && (B(w, 33, 3) == 3 || B(w, 33, 3) == 1);
        /* 32-bit atomics other than INC and DEC, which wrap at the operand. Bindless is bit 54 here. */
        case O_SUATOM:
            return (B(w, 33, 3) == 3 || B(w, 33, 3) == 1) && (B(w, 51, 3) <= 1 || B(w, 51, 3) == 6) &&
                   B(w, 29, 4) <= 8 && B(w, 29, 4) != 3 && B(w, 29, 4) != 4 && !B(w, 49, 2);
        case O_SURED: return B(w, 33, 3) == 3 && B(w, 20, 3) == 0 && B(w, 24, 3) <= 2 && !B(w, 23, 1) && !B(w, 49, 2);
        case O_HADD2_R:
        case O_HADD2_C:
        case O_HADD2_I:
        case O_HMUL2_R:
        case O_HMUL2_C:
        case O_HMUL2_I:
        case O_HFMA2_R:
        case O_HFMA2_RC:
        case O_HFMA2_CR:
        case O_HFMA2_I: return 1;
        case O_HSET2_R:
        case O_HSET2_C:
        case O_HSET2_I: return 1;
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I: return 1;
        case O_DSET_R:
        case O_DSET_C:
        case O_DSET_I:
        case O_DSETP_R:
        case O_DSETP_C:
        case O_DSETP_I: return 1;
        case O_DADD_R:
        case O_DADD_C:
        case O_DADD_I:
        case O_DMUL_R:
        case O_DMUL_C:
        case O_DMUL_I: return !B(w, 47, 1) && !B(w, 39, 2);
        case O_DFMA_R:
        case O_DFMA_RC:
        case O_DFMA_CR:
        case O_DFMA_I: return !B(w, 47, 1) && !B(w, 50, 2);
        case O_DMNMX_R:
        case O_DMNMX_C:
        case O_DMNMX_I: return !B(w, 47, 1);
        case O_TEX: return texture_sample_supported(w, 0);
        case O_TEX_B: return texture_sample_supported(w, 1);
        case O_TMML: return B(w, 28, 3) == 2;
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I: return 1;
        case O_FSET_R:
        case O_FSET_C:
        case O_FSET_I: return 1;
        default: return 0;
    }
}

/* The storage image slot for a surface operation: images of one handle share a slot when they have the same
   dimension (3 a 2D image, 1 a buffer) and are both R32ui (atomics, and loads from buffers) or both not. */
static int direct_image_slot(const ShaderInfo* info, u32 off, int bindless, u32 bank, int r32ui, int type) {
    int slot = 0;
    while (slot < info->nimg && (info->img[slot].cbuf_off != off || info->img[slot].bindless != bindless ||
                                 (info->img[slot].kind == 1) != r32ui || info->img[slot].type != type ||
                                 (bindless && info->img[slot].bl_cbuf != bank)))
        slot++;
    return slot;
}

/* The attribute instructions each stage has. Vertex, tessellation and geometry programs read their inputs and store
   their outputs, the stages after the vertex one per vertex of their input primitive (its position too).
   Tessellation control programs store the patch's attributes, which tessellation evaluation programs read with the
   tessellation coordinate. Only geometry programs emit vertices. */
static int stage_has(u64 w, int op, int stage) {
    int between = stage == ST_TCS || stage == ST_TES || stage == ST_GS;
    u32 addr = B(w, 20, 10);
    if (op == O_ALD) {
        if (stage == ST_FS || stage == ST_CS) return 0;
        if (B(w, 31, 1)) return stage == ST_TCS || stage == ST_TES;
        if (addr == 0x2f8 || addr == 0x2fc) return stage == ST_VS;
        if (addr == 0x2f0 || addr == 0x2f4) return stage == ST_TES;
        return addr >= 0x80 || between;
    }
    if (op == O_AST) return stage != ST_FS && stage != ST_CS && (!B(w, 31, 1) || stage == ST_TCS);
    if (op == O_OUT_R || op == O_OUT_I) return stage == ST_GS;
    if (op == O_ISBERD) return between;
    if (op == O_S2R && B(w, 20, 8) == 0x11) return stage == ST_TCS || stage == ST_GS;
    if (op == O_BAR) return stage == ST_CS || stage == ST_TCS;
    return 1;
}

/* An indexed handle: the LDC that loads it, through MOVs, takes its address from a register. Its flags
   (SNX_TEX_INDEXED and the stride's shift: the SHL that made the address, or 2 for words) and *ldc, that LDC's
   instruction plus one. 0 when the handle isn't indexed, or can't be: the device has no texture arrays, or the
   stage is compute, which binds none. */
static u32 direct_indexed_handle(const u64* insn, u32 n, u32 index, u32 reg, int stage, u32* ldc) {
    int j = -1, k, depth, op = 0;
    u64 w = 0;
    u32 shift = 2;
    if (!g_shader_texture_arrays || stage == ST_CS) return 0;
    for (depth = 0; depth < 6; depth++) {
        j = direct_reaching_def(insn, n, index, reg);
        if (j < 0) return 0;
        w = insn[j];
        op = decode(w)->id;
        if (op != O_MOV_R) break;
        index = (u32)j;
        reg = B(w, 20, 8);
    }
    if (depth == 6 || op != O_LDC || B(w, 8, 8) == 255 || B(w, 48, 3) != 4) return 0;
    k = direct_reaching_def(insn, n, (u32)j, B(w, 8, 8));
    if (k >= 0 && decode(insn[k])->id == O_SHL_I && B(insn[k], 20, 5) >= 2 && B(insn[k], 20, 5) <= 15)
        shift = B(insn[k], 20, 5);
    *ldc = (u32)j + 1;
    return SNX_TEX_INDEXED | shift << 4;
}

/* Whether a bindless slot reads the handle a tracker found: its bank and second read, or for an indexed handle the
   same LDC (bl2_off holds its instruction plus one, as no second read is kept). */
static int direct_bindless_matches(const ShTex* t, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed,
                                   u32 ldc) {
    if ((u32)(t->flags & 0xF4) != indexed || t->bl_cbuf != bank) return 0;
    if (indexed) return t->bl2_off == ldc;
    return t->bl2 == (handle_count == 2) && (handle_count != 2 || (t->bl2_cbuf == bank2 && t->bl2_off == off2));
}

/* The 2D texture slot a TXQ queries, bound or by a tracked handle: TXQ doesn't say what the texture is, and the
   query is translated for a 2D one like the bound form's. */
static int direct_query_slot(const ShTex* textures, int count, u32 off, int bindless, u32 bank, int handle_count,
                             u32 bank2, u32 off2, u32 indexed, u32 ldc) {
    int slot = 0;
    while (slot < count &&
           (textures[slot].cbuf_off != off || textures[slot].type != 1 || textures[slot].array ||
            textures[slot].shadow || (textures[slot].flags & 3) || textures[slot].bindless != bindless ||
            (bindless && !direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
        slot++;
    return slot;
}

/* For this instruction, an indexed texture's sampler is the array element its LDC chose, clamped to the array */
static void direct_swap_element(DirectCtx* c, int slot) {
    u32 limit = spv_emit_constant_u32(&c->emit, c->u32_type, SNX_TEX_ARRAY - 1);
    u32 index = spv_emit_load(&c->emit, c->u32_type, c->tex_index[slot]);
    index = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, limit, index), limit, index);
    c->sampler[slot] = spv_emit_sampler_array_element(&c->emit, c->sampled_type[slot], c->texture_array, index);
    c->swapped = (u32)slot + 1;
}

static u32* direct_reject(ShaderInfo* si, const char* reason) {
    si->unsupported = 1;
    snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", reason);
    return NULL;
}

u32* shader_to_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count, int can_flush,
                     int can_preserve, int can_subgroup) {
    u32 clip;
    if (!code || !si || !word_count || max_bytes < 96 || stage < ST_VS || stage > ST_CS) return NULL;
    if (stage == ST_CS) {
        ShaderInfo original = *si;
        u32* simple = shader_compute_to_spirv(code, max_bytes, si, word_count, can_flush, can_preserve);
        if (simple) return simple;
        *si = original;
        if (!si->cs_local[0] || !si->cs_local[1] || !si->cs_local[2]) return direct_reject(si, "compute local size");
    }
    if (!ops_ready) ops_init();
    u32 len;
    shader_hash(code, max_bytes, &len);
    u32 n = len / 8;
    const u64* insn = (const u64*)code;
    u32 sph[20];
    memcpy(sph, code, sizeof sph);
    u32 target = stage == ST_CS ? 0 : sph[18];
    if (stage == ST_FS && (sph[19] & ~2u)) {
        snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", "depth or sample-mask output");
        si->unsupported = 1;
        return NULL;
    } /* depth and sample-mask outputs follow the colours */
    u8 used[255] = {0};
    u8 pred_used[7] = {0};
    u8 inputs_used[32] = {0};
    u8 outputs_used[32] = {0};
    int position_used = 0, layer_output_used = 0, layer_input_used = 0, point_size_used = 0;
    /* the stages between the vertex and fragment ones: their input arrays' length (32 for tessellation, the input
       primitive's vertices for geometry), the control program's output vertices, and what they read and write */
    int between = stage == ST_TCS || stage == ST_TES || stage == ST_GS;
    u32 in_vertices = stage == ST_GS ? si->rt_int_mask : 32, out_vertices = stage == ST_TCS ? sph[2] >> 24 : 0;
    u8 patch_used[30] = {0};
    int levels_used = 0, position_in_used = 0, primitive_id_used = 0, tess_coord_used = 0, invocation_used = 0;
    int last = stage != ST_TCS && !(si->clip_enable & SNX_SH_NOT_LAST); /* before rasterization */
    u32 clip_used = 0;
    u8 cbuf_used[18] = {0};
    u32 cbuf_max[18] = {0};
    ShTex textures[32] = {0};
    int texture_count = 0;
    ShaderInfo image_info = {0};
    int needs_fragcoord = 0;
    int needs_frontface = 0;
    int needs_image_query = 0;
    int needs_instance = 0;
    int needs_local_id = 0, needs_group_id = 0;
    int needs_scale = 0;
    int needs_glsl = 0, needs_f16 = 0, needs_f64 = 0;
    int has_flow = 0, has_calls = 0, has_barrier = 0, converge;
    u32 wait_label = 0;
    int first_indexed = -1, k_indexed;
    int has_stack = 0;
    int needs_local = 0, needs_shared = 0, needs_subgroup = 0, needs_atomic_cas = 0;
    int needs_gather_extended = 0;
    int fp32_flush = 0, fp32_preserve = 0;
    u32 out_reg = 0;
    if (stage == ST_FS)
        for (int rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                if (out_reg > 251) return direct_reject(si, "fragment output registers");
                for (int k = 0; k < 4; k++)
                    if (target & (1u << (rt * 4 + k))) used[out_reg + k] = 1;
                out_reg += 4;
            }
    if (stage == ST_FS && (sph[19] & 2u)) {
        if (out_reg >= 254) return direct_reject(si, "fragment depth register");
        used[out_reg + 1] = 1;
    }
    int has_exit = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(insn[i]);
        if (!op || !supported(insn[i], op->id) || !stage_has(insn[i], op->id, stage) ||
            (stage != ST_FS && op->id == O_TMML) || (stage != ST_FS && op->id == O_IPA) ||
            (stage != ST_FS && op->id == O_KIL) ||
            (stage != ST_CS && op->id == O_S2R &&
             ((B(insn[i], 20, 8) >= 0x21 && B(insn[i], 20, 8) <= 0x23) ||
              (B(insn[i], 20, 8) >= 0x25 && B(insn[i], 20, 8) <= 0x27)))) {
            if (op && op->id == O_TEXS)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TEXS encoding %u, fp32 %u",
                         B(insn[i], 53, 4), B(insn[i], 59, 1));
            else if (op && op->id == O_TLDS)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TLDS encoding %u mask %u fp32 %u",
                         B(insn[i], 53, 4), B(insn[i], 50, 3), B(insn[i], 59, 1));
            else if (op && (op->id == O_HADD2_R || op->id == O_HADD2_C || op->id == O_HADD2_I || op->id == O_HADD2_32I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "HADD2 kind %d merge %u swa %u swb %u sat %u", op->id - O_HADD2_R, B(insn[i], 49, 2),
                         B(insn[i], 47, 2), B(insn[i], 28, 2), B(insn[i], 32, 1));
            else if (op && op->id >= O_XMAD_R && op->id <= O_XMAD_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "XMAD %d m%u c%u x%u s%u%u h%u p%u g%u",
                         op->id - O_XMAD_R, B(insn[i], 50, 3), B(insn[i], 47, 1),
                         op->id == O_XMAD_CR || op->id == O_XMAD_RC ? B(insn[i], 54, 1) : B(insn[i], 38, 1),
                         B(insn[i], 48, 1), B(insn[i], 49, 1), B(insn[i], 53, 1), B(insn[i], 36, 1), B(insn[i], 37, 1));
            else if (op && op->id == O_LDC)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "LDC size %u index %u addr %u",
                         B(insn[i], 48, 3), B(insn[i], 36, 5), B(insn[i], 8, 8));
            else if (op && op->id >= O_ISETP_R && op->id <= O_ISETP_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "ISETP kind %d cmp %u uns %u x %u cc %u",
                         op->id - O_ISETP_R, B(insn[i], 49, 3), B(insn[i], 48, 1), B(insn[i], 43, 1),
                         B(insn[i], 47, 1));
            else if (op && op->id >= O_F2F_R && op->id <= O_F2F_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "F2F kind %d dst %u src %u rnd %u int %u sat %u", op->id - O_F2F_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 39, 2), B(insn[i], 42, 1), B(insn[i], 50, 1));
            else if (op && op->id >= O_F2I_R && op->id <= O_F2I_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "F2I kind %d dst %u src %u rnd %u signed %u", op->id - O_F2I_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 39, 2), B(insn[i], 12, 1));
            else if (op && op->id >= O_I2F_R && op->id <= O_I2F_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "I2F kind %d dst %u src %u signed %u sel %u", op->id - O_I2F_R, B(insn[i], 8, 2),
                         B(insn[i], 10, 2), B(insn[i], 13, 1), B(insn[i], 41, 2));
            else if (op && op->id >= O_ISCADD_R && op->id <= O_ISCADD_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "ISCADD kind %d avg %u shift %u cc %u",
                         op->id - O_ISCADD_R, B(insn[i], 49, 2), B(insn[i], 39, 5), B(insn[i], 47, 1));
            else if (op && op->id == O_ALD)
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "ALD addr %u count %u index %u bit32 %u dst %u", B(insn[i], 20, 10), B(insn[i], 47, 2) + 1,
                         B(insn[i], 8, 8), B(insn[i], 32, 1), B(insn[i], 0, 8));
            else if (op && op->id == O_IPA)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IPA addr %u bit38 %u bit51 %u mode %u",
                         B(insn[i], 30, 8) * 4, B(insn[i], 38, 1), B(insn[i], 51, 1), B(insn[i], 54, 2));
            else if (op && op->id == O_IADD32I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IADD32I po %u x %u cc %u sat %u",
                         B(insn[i], 55, 2), B(insn[i], 53, 1), B(insn[i], 52, 1), B(insn[i], 54, 1));
            else if (op && (op->id == O_IADD_R || op->id == O_IADD_C || op->id == O_IADD_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "IADD kind %d avg %u x %u cc %u",
                         op->id - O_IADD_R, B(insn[i], 48, 2), B(insn[i], 43, 1), B(insn[i], 47, 1));
            else if (op && (op->id == O_I2I_R || op->id == O_I2I_C || op->id == O_I2I_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "I2I kind %d dst %u src %u signed %u%u sel %u neg %u abs %u", op->id - O_I2I_R,
                         B(insn[i], 8, 2), B(insn[i], 10, 2), B(insn[i], 12, 1), B(insn[i], 13, 1), B(insn[i], 41, 2),
                         B(insn[i], 45, 1), B(insn[i], 49, 1));
            else if (op && (op->id == O_SHR_R || op->id == O_SHR_C || op->id == O_SHR_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "SHR kind %d sign %u wrap %u reverse %u",
                         op->id - O_SHR_R, B(insn[i], 48, 1), B(insn[i], 39, 1), B(insn[i], 40, 1));
            else if (op && (op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "ISET kind %d cmp %u uns %u x %u cc %u bool %u", op->id - O_ISET_R, B(insn[i], 49, 3),
                         B(insn[i], 48, 1), B(insn[i], 43, 1), B(insn[i], 47, 1), B(insn[i], 45, 2));
            else if (op && (op->id == O_LOP_R || op->id == O_LOP_C || op->id == O_LOP_I))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "LOP kind %d op %u ia %u ib %u cc %u pred %u", op->id - O_LOP_R, B(insn[i], 41, 2),
                         B(insn[i], 39, 1), B(insn[i], 40, 1), B(insn[i], 47, 1), B(insn[i], 48, 3));
            else if (op && (op->id == O_TXQ || op->id == O_TXQ_B))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s q%u mask%u off%u", op->name,
                         B(insn[i], 22, 6), B(insn[i], 31, 4), B(insn[i], 36, 13));
            else if (op && op->id == O_TLD)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "TLD ty%u lod%u toff%u ms%u mask%u",
                         B(insn[i], 28, 3), B(insn[i], 55, 1), B(insn[i], 35, 1), B(insn[i], 50, 1), B(insn[i], 31, 4));
            else if (op && (op->id == O_TLD4 || op->id == O_TLD4_B || op->id == O_TLD4S))
                snprintf(si->first_unsupported, sizeof si->first_unsupported,
                         "%s dc%u ao%u comp%u fp16%u da%u db%u mask%u off%u toff%u", op->name, B(insn[i], 50, 1),
                         B(insn[i],
                           op->id == O_TLD4S    ? 51
                           : op->id == O_TLD4_B ? 36
                                                : 54,
                           op->id == O_TLD4S ? 1 : 2),
                         B(insn[i],
                           op->id == O_TLD4S    ? 52
                           : op->id == O_TLD4_B ? 38
                                                : 56,
                           2),
                         B(insn[i], 55, 1), B(insn[i], 0, 8), B(insn[i], 28, 8), B(insn[i], 31, 4), B(insn[i], 36, 13),
                         B(insn[i], 54, 2));
            else if (op && op->id == O_AST)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "AST addr %u count %u idxreg %u reg %u",
                         B(insn[i], 20, 10), B(insn[i], 47, 2) + 1, B(insn[i], 8, 8), B(insn[i], 0, 8));
            else if (op && op->id >= O_BFE_R && op->id <= O_BFE_I)
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "BFE kind %d signed %u reverse %u",
                         op->id - O_BFE_R, B(insn[i], 48, 1), B(insn[i], 40, 1));
            else if (op && (op->id == O_TEX || op->id == O_TEX_B))
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s ty%u lod%u dc%u mask%u off%u aoffi%u",
                         op->name, B(insn[i], 28, 3), B(insn[i], op->id == O_TEX_B ? 37 : 55, 3), B(insn[i], 50, 1),
                         B(insn[i], 31, 4), B(insn[i], 36, 13), B(insn[i], op->id == O_TEX_B ? 36 : 54, 1));
            else /* the word too, to decode which form of it is missing */
                snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s %016llx",
                         op ? op->name : "unknown opcode", (unsigned long long)insn[i]);
            si->unsupported = 1;
            return NULL;
        }
        if (op->id == O_BRA || op->id == O_CAL || op->id == O_PRET) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return direct_reject(si, "branch target");
            has_flow = 1;
            if (op->id != O_BRA) has_calls = 1;
        }
        if (op->id == O_RET) has_flow = has_calls = 1; /* a RET with no CAL pops an empty stack: it ends the program */
        if ((op->id == O_RED || op->id == O_ATOM) && global_atomic_opcode(insn[i], op->id == O_RED) == 0xffffffffu)
            needs_atomic_cas = 1;
        if (op->id == O_ATOMS && shared_atomic_opcode(insn[i]) == 0xffffffffu) needs_atomic_cas = 1;
        if (op->id == O_BRX) has_flow = 1;
        if (op->id == O_S2R && stage == ST_CS) {
            u32 sr = B(insn[i], 20, 8);
            needs_local_id |= sr >= 0x21 && sr <= 0x23;
            needs_group_id |= sr >= 0x25 && sr <= 0x27;
        }
        if (op->id == O_SSY || op->id == O_PBK || op->id == O_PCNT) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return direct_reject(si, "control stack target");
            has_flow = has_stack = 1;
        }
        if (op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT) has_flow = has_stack = 1;
        if (((op->id >= O_FADD_R && op->id <= O_FFMA_I) || op->id == O_FFMA32I) && op->id != O_FMNMX_R &&
            op->id != O_FMNMX_C && op->id != O_FMNMX_I) {
            u32 mode = op->id == O_FADD32I                        ? B(insn[i], 55, 1)
                       : op->id >= O_FADD_R && op->id <= O_FADD_I ? B(insn[i], 44, 1)
                       : op->id >= O_FMUL_R && op->id <= O_FMUL_I ? B(insn[i], 44, 2)
                                                                  : B(insn[i], 53, 2);
            if (mode)
                fp32_flush = 1;
            else
                fp32_preserve = 1;
        }
        u32 d = B(insn[i], 0, 8), a = B(insn[i], 8, 8), b = B(insn[i], 20, 8);
        u32 guard = B(insn[i], 16, 3);
        if (guard < 7) pred_used[guard] = 1;
        if (d < 255 && op->id != O_EXIT && op->id != O_KIL && op->id != O_BRA && op->id != O_BRX && op->id != O_SSY &&
            op->id != O_PBK && op->id != O_PCNT && op->id != O_SYNC && op->id != O_BRK && op->id != O_CONT &&
            op->id != O_AST && op->id != O_NOP && op->id != O_VOTE_VTG && op->id != O_DEPBAR && op->id != O_MEMBAR &&
            op->id != O_CAL &&
            op->id != O_PRET && op->id != O_RET && op->id != O_FSETP_R && op->id != O_FSETP_C && op->id != O_FSETP_I &&
            op->id != O_HSETP2_R && op->id != O_HSETP2_C && op->id != O_HSETP2_I && op->id != O_DSETP_R &&
            op->id != O_PSETP && op->id != O_CSETP && op->id != O_R2P_I)
            used[d] = 1;
        if (op->id == O_ALD || op->id == O_AST) {
            u32 addr = B(insn[i], 20, 10), width = B(insn[i], 47, 2) + 1, vertex = B(insn[i], 39, 8);
            int input = op->id == O_ALD;
            if (between && vertex < 255) used[vertex] = 1;
            if (stage == ST_TCS && op->id == O_AST) invocation_used = 1; /* the invocation's output vertex */
            for (u32 k = 0; k < width; k++) {
                if (d + k < 255) used[d + k] = 1;
                u32 location = addr + k * 4;
                if (B(insn[i], 31, 1)) { /* the levels' 6 words and 2 of padding, then the patch's vectors */
                    if (location >= 0x20)
                        patch_used[(location - 0x20) >> 4] = 1;
                    else if (location < 0x18)
                        levels_used |= location < 0x10 ? 1 : 2;
                } else if (input && location >= 0x2f8 && location <= 0x2fc)
                    needs_instance = 1;
                else if (input && (location == 0x2f0 || location == 0x2f4))
                    tess_coord_used = 1;
                else if (input && location == 0x60)
                    primitive_id_used = 1;
                else if (input && location < 0x80)
                    position_in_used = 1;
                else if (input)
                    inputs_used[(location - 0x80) >> 4] = 1;
                else if (location == 0x64)
                    layer_output_used = 1;
                else if (location == 0x6c)
                    point_size_used = 1;
                else if (location >= 0x2c0 && location < 0x2e0) {
                    u32 bit = 1u << ((location - 0x2c0) / 4);
                    if (si->clip_enable & bit) clip_used |= bit;
                } else if (location >= 0x80)
                    outputs_used[(location - 0x80) >> 4] = 1;
                else
                    position_used = 1;
            }
        }
        if (op->id == O_ISBERD) used[a] = 1;
        if (op->id == O_S2R && B(insn[i], 20, 8) == 0x11) invocation_used = 1;
        if (op->id == O_IMUL_R || op->id == O_IMAD_R || op->id == O_IMAD_RC) {
            if (b < 255 && op->id != O_IMAD_RC) used[b] = 1;
            if (op->id == O_IMAD_RC && B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
        }
        if (op->id == O_IMAD_R || op->id == O_IMAD_CR || op->id == O_IMAD_I)
            if (B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_IMAD32I && d < 255) used[d] = 1;
        if ((op->id >= O_IMUL_R && op->id <= O_IMAD32I) && a < 255) used[a] = 1;
        if (op->id == O_MOV_R || op->id == O_LOP3_R || op->id == O_IMNMX_R || op->id == O_XMAD_R || op->id == O_BFE_R ||
            op->id == O_ICMP_R || op->id == O_IADD3_R || op->id == O_FADD_R || op->id == O_FMUL_R ||
            op->id == O_FFMA_R || op->id == O_FMNMX_R || op->id == O_FSET_R || op->id == O_RRO_R || op->id == O_FLO_R ||
            op->id == O_POPC_R) {
            if (b < 255) used[b] = 1;
        }
        if ((op->id == O_FADD_R || op->id == O_FADD_C || op->id == O_FADD_I || op->id == O_FADD32I ||
             op->id == O_FFMA32I || op->id == O_FMUL_R || op->id == O_FMUL_C || op->id == O_FMUL_I ||
             op->id == O_FMUL32I || op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR ||
             op->id == O_FFMA_I || op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I ||
             op->id == O_FSET_R || op->id == O_FSET_C || op->id == O_FSET_I || op->id == O_MUFU) &&
            a < 255)
            used[a] = 1;
        if ((op->id == O_BRX || (op->id >= O_P2R_R && op->id <= O_R2P_I) || op->id == O_LOP3_R || op->id == O_LOP3_C ||
             op->id == O_LOP3_I || op->id == O_IMNMX_R || op->id == O_IMNMX_C || op->id == O_IMNMX_I ||
             op->id == O_XMAD_CR || op->id == O_XMAD_RC || op->id == O_XMAD_R || op->id == O_IADD3_R ||
             op->id == O_IADD3_C || op->id == O_IADD3_I || op->id == O_ICMP_R || op->id == O_ICMP_RC ||
             op->id == O_ICMP_CR || op->id == O_ICMP_I || op->id == O_FCMP_R || op->id == O_FCMP_RC ||
             op->id == O_FCMP_CR || op->id == O_FCMP_I || op->id == O_BFI_R || op->id == O_BFI_RC ||
             op->id == O_BFI_CR || op->id == O_BFI_I || op->id == O_IADD32I || op->id == O_LOP32I ||
             op->id == O_XMAD_I || op->id == O_IADD_R || op->id == O_IADD_C || op->id == O_IADD_I || op->id == O_LDG ||
             op->id == O_STG || op->id == O_RED || op->id == O_ATOM || op->id == O_LOP_R || op->id == O_LOP_C ||
             op->id == O_LOP_I || op->id == O_SEL_R || op->id == O_SEL_C || op->id == O_SEL_I || op->id == O_ISCADD_R ||
             op->id == O_ISCADD_C || op->id == O_ISCADD_I || (op->id >= O_BFE_R && op->id <= O_BFE_I) ||
             op->id == O_SHL_R || op->id == O_SHL_C || op->id == O_SHL_I || op->id == O_SHR_R || op->id == O_SHR_C ||
             op->id == O_SHR_I || op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I) &&
            a < 255)
            used[a] = 1;
        if ((op->id == O_ISCADD_R || op->id == O_IADD_R || op->id == O_LOP_R || op->id == O_SEL_R ||
             op->id == O_P2R_R || op->id == O_R2P_R) &&
            b < 255)
            used[b] = 1;
        if (op->id == O_SEL_R || op->id == O_SEL_C || op->id == O_SEL_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
        }
        if ((op->id == O_XMAD_I || op->id == O_XMAD_R || op->id == O_XMAD_RC || op->id == O_XMAD_CR) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_F2F_R || op->id == O_F2I_R || op->id == O_I2F_R) && b < 255) used[b] = 1;
        if (op->id >= O_I2F_R && op->id <= O_I2F_I && B(insn[i], 8, 2) == 3) {
            needs_f64 = 1;
            if (d < 254) used[d + 1] = 1;
        }
        if (op->id == O_I2I_R && b < 255) used[b] = 1;
        if ((op->id == O_ICMP_R || op->id == O_ICMP_RC || op->id == O_ICMP_CR || op->id == O_ICMP_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_FCMP_R || op->id == O_FCMP_RC || op->id == O_FCMP_CR || op->id == O_FCMP_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_FCMP_R || op->id == O_FCMP_RC) && b < 255) used[b] = 1;
        if ((op->id == O_BFI_R || op->id == O_BFI_RC || op->id == O_BFI_CR || op->id == O_BFI_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_BFI_R || op->id == O_BFI_RC) && b < 255) used[b] = 1;
        if ((op->id == O_IADD3_R || op->id == O_IADD3_C || op->id == O_IADD3_I) && B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if ((op->id == O_LOP3_R || op->id == O_LOP3_C || op->id == O_LOP3_I) && B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_LDC) {
            if (a < 255) used[a] = 1;
            if (B(insn[i], 48, 3) == 5 && d + 1 < 255) used[d + 1] = 1;
            u32 bank = B(insn[i], 36, 5);
            cbuf_used[bank] = 1;
            cbuf_max[bank] = 65536;
        }
        if (op->id == O_LDG || op->id == O_STG) {
            u32 count = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + count > 256) return direct_reject(si, "global memory register range");
            for (u32 k = 0; k < count; k++)
                if (d + k < 255) used[d + k] = 1;
        }
        if (op->id == O_RED && d < 255) used[d] = 1;
        if (op->id == O_ATOM && b < 255) used[b] = 1;
        if (op->id == O_LDL || op->id == O_STL) {
            needs_local = 1;
            if (a < 255) used[a] = 1;
            u32 words = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + words > 255) return direct_reject(si, "local memory register range");
            for (u32 k = 0; k < words; k++)
                if (d + k < 255) used[d + k] = 1;
        }
        if (op->id == O_LDS || op->id == O_STS) {
            needs_shared = 1;
            if (a < 255) used[a] = 1;
            u32 words = B(insn[i], 48, 3) == 5 ? 2 : B(insn[i], 48, 3) >= 6 ? 4 : 1;
            if (d != 255 && d + words > 255) return direct_reject(si, "shared memory register range");
            for (u32 k = 0; k < words; k++)
                if (d + k < 255) used[d + k] = 1;
        }
        if (op->id == O_ATOMS) {
            needs_shared = 1;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (shared_atomic_opcode(insn[i]) == 0xfffffffeu) {
                if (stage != ST_CS || (u64)si->cs_local[0] * si->cs_local[1] * si->cs_local[2] != 1)
                    return direct_reject(si, "64-bit shared exchange requires one invocation");
                if (b + 1 < 255) used[b + 1] = 1;
                if (d + 1 < 255) used[d + 1] = 1;
            }
        }
        if (op->id == O_BAR && stage != ST_CS && stage != ST_TCS) return direct_reject(si, "barrier outside compute");
        if (op->id == O_BAR) has_barrier = 1;
        if (op->id == O_ISETP_R || op->id == O_ISETP_C || op->id == O_ISETP_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_ISETP_R && b < 255) used[b] = 1;
            u32 pa = B(insn[i], 3, 3), pb = B(insn[i], 0, 3), pc = B(insn[i], 39, 3);
            if (pa < 7) pred_used[pa] = 1;
            if (pb < 7) pred_used[pb] = 1;
            if (pc < 7) pred_used[pc] = 1;
        }
        if (op->id == O_PSETP) {
            u32 pp[] = {B(insn[i], 12, 3), B(insn[i], 29, 3), B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 5; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if (op->id == O_PSET) {
            u32 pred_ids[3] = {B(insn[i], 12, 3), B(insn[i], 29, 3), B(insn[i], 39, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pred_ids[k] < 7) pred_used[pred_ids[k]] = 1;
        }
        if (op->id == O_CSETP || op->id == O_CSET) {
            u32 pp[] = {B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if (op->id == O_VOTE) {
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (B(insn[i], 45, 3) < 7) pred_used[B(insn[i], 45, 3)] = 1;
        }
        if (op->id == O_SHFL) {
            if (a < 255) used[a] = 1;
            if (!B(insn[i], 28, 1) && b < 255) used[b] = 1;
            if (!B(insn[i], 29, 1) && B(insn[i], 39, 8) < 255) used[B(insn[i], 39, 8)] = 1;
            if (B(insn[i], 48, 3) < 7) pred_used[B(insn[i], 48, 3)] = 1;
        }
        if (op->id == O_IMNMX_R || op->id == O_IMNMX_C || op->id == O_IMNMX_I)
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
        if (op->id >= O_P2R_R && op->id <= O_R2P_I && !B(insn[i], 40, 1))
            for (u32 p = 0; p < 7; p++) pred_used[p] = 1;
        if ((op->id == O_LOP_R || op->id == O_LOP_C || op->id == O_LOP_I) && B(insn[i], 48, 3) < 7)
            pred_used[B(insn[i], 48, 3)] = 1;
        if (op->id == O_LOP3_R && B(insn[i], 48, 3) < 7) pred_used[B(insn[i], 48, 3)] = 1;
        if (op->id == O_SHL_R && b < 255) used[b] = 1;
        if (op->id == O_SHF_LR || op->id == O_SHF_LI || op->id == O_SHF_RR || op->id == O_SHF_RI) {
            if (a < 255) used[a] = 1;
            if ((op->id == O_SHF_LR || op->id == O_SHF_RR) && b < 255) used[b] = 1;
            u32 hi = B(insn[i], 39, 8);
            if (hi < 255) used[hi] = 1;
        }
        if (op->id == O_LEA_LR || op->id == O_LEA_LC || op->id == O_LEA_LI || op->id == O_LEA_HR ||
            op->id == O_LEA_HC) {
            if (a < 255) used[a] = 1;
            if ((op->id == O_LEA_LR || op->id == O_LEA_HR) && b < 255) used[b] = 1;
            if (op->id == O_LEA_HR || op->id == O_LEA_HC) {
                u32 hi = B(insn[i], 39, 8);
                if (hi < 255) used[hi] = 1;
            } else if (B(insn[i], 48, 3) < 7)
                pred_used[B(insn[i], 48, 3)] = 1;
        }
        if ((op->id == O_SHR_R || op->id == O_ISET_R) && b < 255) used[b] = 1;
        if (op->id == O_ISET_R || op->id == O_ISET_C || op->id == O_ISET_I) {
            u32 pc = B(insn[i], 39, 3);
            if (pc < 7) pred_used[pc] = 1;
        }
        if ((op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FFMA_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_MUFU || op->id == O_RRO_R || op->id == O_RRO_C || op->id == O_FFMA32I || op->id == O_FFMA_R ||
            op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FFMA_I || op->id == O_FMNMX_R ||
            op->id == O_FMNMX_C || op->id == O_FMNMX_I ||
            ((op->id == O_FADD_R || op->id == O_FADD_C || op->id == O_FADD_I) &&
             (B(insn[i], 46, 1) || B(insn[i], 49, 1) || B(insn[i], 50, 1))) ||
            (op->id == O_FADD32I && (B(insn[i], 54, 1) || B(insn[i], 57, 1))) ||
            ((op->id == O_FMUL_R || op->id == O_FMUL_C || op->id == O_FMUL_I) && B(insn[i], 50, 1)) ||
            (op->id == O_FMUL32I && B(insn[i], 55, 1)) || (op->id >= O_F2F_R && op->id <= O_F2F_I) ||
            (op->id >= O_F2I_R && op->id <= O_F2I_I) || (op->id >= O_I2F_R && op->id <= O_I2F_I))
            needs_glsl = 1;
        if (op->id == O_IMUL_C || op->id == O_IMAD_RC || op->id == O_IMAD_CR) {
            u32 idx = B(insn[i], 34, 5), off = B(insn[i], 20, 14) * 4;
            if (idx >= 18) return direct_reject(si, "constant buffer index");
            cbuf_used[idx] = 1;
            if ((off | 15u) + 1 > cbuf_max[idx]) cbuf_max[idx] = (off | 15u) + 1;
        }
        if (op->id == O_LOP3_C || op->id == O_IMNMX_C || op->id == O_XMAD_CR || op->id == O_XMAD_RC ||
            op->id == O_BFE_C || op->id == O_IADD3_C || op->id == O_LEA_LC || op->id == O_LEA_HC || op->id == O_F2I_C ||
            op->id == O_I2F_C || op->id == O_I2I_C || op->id == O_F2F_C || op->id == O_FLO_C || op->id == O_POPC_C ||
            op->id == O_P2R_C || op->id == O_R2P_C || op->id == O_ICMP_RC || op->id == O_ICMP_CR ||
            op->id == O_FCMP_RC || op->id == O_FCMP_CR || op->id == O_BFI_RC || op->id == O_BFI_CR ||
            op->id == O_MOV_C || op->id == O_IADD_C || op->id == O_ISCADD_C || op->id == O_LOP_C || op->id == O_SEL_C ||
            op->id == O_HADD2_C || op->id == O_HMUL2_C || op->id == O_HFMA2_RC || op->id == O_HFMA2_CR ||
            op->id == O_ISETP_C || op->id == O_ISET_C || op->id == O_SHL_C || op->id == O_SHR_C || op->id == O_FADD_C ||
            op->id == O_FMUL_C || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FMNMX_C ||
            op->id == O_RRO_C || op->id == O_FSETP_C || op->id == O_FSET_C || op->id == O_HSET2_C ||
            op->id == O_HSETP2_C || op->id == O_DADD_C || op->id == O_DMUL_C || op->id == O_DFMA_RC ||
            op->id == O_DFMA_CR || op->id == O_DMNMX_C || op->id == O_DSET_C || op->id == O_DSETP_C) {
            u32 idx = B(insn[i], 34, 5), off = B(insn[i], 20, 14) * 4;
            if (idx >= 18) return direct_reject(si, "constant buffer index");
            cbuf_used[idx] = 1;
            if ((off | 15u) + 1 > cbuf_max[idx]) cbuf_max[idx] = (off | 15u) + 1;
        }
        if (op->id == O_FSETP_R || op->id == O_FSETP_C || op->id == O_FSETP_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_FSETP_R && b < 255) used[b] = 1;
            u32 pc = B(insn[i], 39, 3), pa = B(insn[i], 3, 3), pb = B(insn[i], 0, 3);
            if (pc < 7) pred_used[pc] = 1;
            if (pa < 7) pred_used[pa] = 1;
            if (pb < 7) pred_used[pb] = 1;
            needs_glsl = 1;
        }
        if (op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
        }
        if (op->id == O_FSET_R || op->id == O_FSET_C || op->id == O_FSET_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
            if (B(insn[i], 54, 1) || B(insn[i], 44, 1)) needs_glsl = 1;
        }
        if (op->id == O_TEXS) {
            u32 off = B(insn[i], 36, 13);
            u32 encoding = B(insn[i], 53, 4);
            int shadow = encoding == 4, type = encoding == 12 || encoding == 13   ? 3
                                               : encoding == 10 || encoding == 11 ? 2
                                               : encoding == 0                    ? 0
                                                                                  : 1;
            int array = encoding == 7 || encoding == 8;
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].shadow == shadow && textures[slot].type == type &&
                    textures[slot].array == array && !textures[slot].bindless && !textures[slot].flags)
                    break;
            if (slot == texture_count) {
                if (texture_count >= 32) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].shadow = (u8)shadow;
                textures[slot].array = (u8)array;
                texture_count++;
            }
            if (!B(insn[i], 59, 1)) needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if ((shadow || encoding == 3 || encoding == 10 || encoding == 11 || encoding == 12 || encoding == 13 ||
                 array) &&
                a + 1 < 255)
                used[a + 1] = 1;
            if (encoding == 13 && b + 1 < 255) used[b + 1] = 1;
            u32 regs[4], comps[4];
            int outputs = texs_outputs(insn[i], regs, comps);
            if (B(insn[i], 59, 1)) {
                for (int k = 0; k < outputs; k++)
                    if (regs[k] < 255) used[regs[k]] = 1;
            } else {
                if (outputs && d < 255) used[d] = 1;
                if (outputs > 2) {
                    u32 second = B(insn[i], 28, 8);
                    if (second == 255) second = d + 1;
                    if (second < 255) used[second] = 1;
                }
            }
        }
        if (op->id == O_TLD || op->id == O_TLD_B) {
            u32 indexed = 0, ldc = 0;
            u32 off = B(insn[i], 36, 13), ty = B(insn[i], 28, 3);
            u32 bank = 0, bank2 = 0, off2 = 0;
            int bindless = op->id == O_TLD_B;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            int type = ty >> 1, array = ty & 1, multisample = op->id == O_TLD_B && B(insn[i], 50, 1), slot = 0;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                    textures[slot].shadow || textures[slot].bindless != bindless ||
                    !!(textures[slot].flags & 2) != multisample ||
                    (bindless &&
                     !direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
                slot++;
            if (slot == texture_count) {
                if (slot >= 32) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].array = (u8)array;
                textures[slot].bindless = (u8)bindless;
                textures[slot].flags = multisample ? 2 : 0;
                textures[slot].bl_cbuf = (u8)bank;
                textures[slot].bl2 = handle_count == 2;
                textures[slot].bl2_cbuf = (u8)bank2;
                textures[slot].bl2_off = off2;
                if (indexed) {
                    textures[slot].flags |= (u8)indexed;
                    textures[slot].bl2_off = ldc;
                }
                texture_count++;
            }
            u32 coordinate_count = (type == 0 ? 1u : type == 1 ? 2u : 3u) + (u32)array;
            for (u32 k = 0; k < coordinate_count; k++)
                if (a + k < 255) used[a + k] = 1;
            if (bindless && b < 255) used[b] = 1;
            u32 meta = b + bindless;
            if (B(insn[i], 55, 1) && meta < 255) used[meta++] = 1;
            if (B(insn[i], 35, 1) && meta < 255) used[meta++] = 1;
            if (multisample && meta < 255) used[meta] = 1;
            u32 target = d, mask = B(insn[i], 31, 4);
            if (!mask) mask = 15;
            for (u32 k = 0; k < 4; k++)
                if (mask & (1u << k)) {
                    if (target < 255) used[target] = 1;
                    target++;
                }
        }
        if (op->id == O_SULD || op->id == O_SUST || op->id == O_SURED || op->id == O_SUATOM) {
            int buffer = B(insn[i], 33, 3) == 1;
            u32 bank, off, handle;
            int bindless;
            if (buffer && stage != ST_CS) return direct_reject(si, "buffer surface outside compute");
            if (stage != ST_CS && (stage != ST_FS || op->id == O_SULD))
                return direct_reject(si, "surface operation outside supported stage");
            bank = 0;
            off = B(insn[i], 36, 13);
            handle = B(insn[i], 39, 8);
            bindless = op->id == O_SUATOM ? (int)B(insn[i], 54, 1) : !B(insn[i], 51, 1);
            if (bindless && !direct_track_image_handle(insn, n, i, handle, &bank, &off, 0))
                return direct_reject(si, "untracked surface handle");
            if (a < 255) used[a] = 1;
            if (!buffer && a + 1 < 255) used[a + 1] = 1;
            if (bindless && handle < 255) used[handle] = 1;
            int raw = op->id == O_SULD && B(insn[i], 52, 1);
            u32 mask = raw ? 1u : B(insn[i], 20, 4), target = d;
            if (op->id == O_SURED) {
                if (target < 255) used[target] = 1;
            } else if (op->id == O_SUATOM) {
                if (d < 255) used[d] = 1;
                if (b < 255) used[b] = 1;
            } else
                for (u32 q = 0; q < 4; q++)
                    if (mask & (1u << q)) {
                        if (target < 255) used[target] = 1;
                        target++;
                    }
            /* buffer loads read the storage texel buffer: a sampled 1D image of a buffer is one row of at most
               16384 texels, and the dispatch's own stores and atomics wouldn't reach it */
            if (op->id != O_SULD || buffer) {
                int r32ui = op->id != O_SUST;
                int type = buffer ? 1 : 3;
                int slot = direct_image_slot(&image_info, off, bindless, bank, r32ui, type);
                if (slot == image_info.nimg) {
                    if (slot >= 8) return direct_reject(si, "storage image descriptor count");
                    int different_format = !!(si->rt_int_mask & (1u << (8 + slot)));
                    int kind = r32ui ? 1 : (bindless != different_format ? 2 : 0);
                    image_info.img[slot].cbuf_off = off;
                    image_info.img[slot].type = (u8)type;
                    image_info.img[slot].bindless = (u8)bindless;
                    image_info.img[slot].bl_cbuf = (u8)bank;
                    image_info.img[slot].kind = (u8)kind;
                    image_info.nimg++;
                }
                if (bindless) {
                    cbuf_used[bank] = 1;
                    if (off * 4 + 16 > cbuf_max[bank]) cbuf_max[bank] = ((off * 4) | 15u) + 1;
                }
            } else {
                int slot = 0;
                while (slot < texture_count &&
                       (textures[slot].cbuf_off != off || textures[slot].type != 1 || textures[slot].array ||
                        textures[slot].shadow || textures[slot].bindless != bindless ||
                        !!(textures[slot].flags & 1) != raw || (bindless && textures[slot].bl_cbuf != bank)))
                    slot++;
                if (slot == texture_count) {
                    if (slot >= 32) return direct_reject(si, "texture descriptor count");
                    textures[slot].cbuf_off = off;
                    textures[slot].type = 1;
                    textures[slot].bindless = (u8)bindless;
                    textures[slot].flags = raw ? 1 : 0;
                    textures[slot].bl_cbuf = (u8)bank;
                    texture_count++;
                }
            }
        }
        if (op->id == O_HADD2_R || op->id == O_HADD2_C || op->id == O_HADD2_I || op->id == O_HMUL2_R ||
            op->id == O_HMUL2_C || op->id == O_HMUL2_I || op->id == O_HFMA2_R || op->id == O_HFMA2_RC ||
            op->id == O_HFMA2_CR || op->id == O_HFMA2_I) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if ((op->id == O_HADD2_R || op->id == O_HMUL2_R || op->id == O_HFMA2_R) && b < 255) used[b] = 1;
            if ((op->id == O_HFMA2_R || op->id == O_HFMA2_RC || op->id == O_HFMA2_CR || op->id == O_HFMA2_I) &&
                B(insn[i], 39, 8) < 255)
                used[B(insn[i], 39, 8)] = 1;
        }
        if (op->id == O_HSET2_R || op->id == O_HSET2_C || op->id == O_HSET2_I) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (op->id == O_HSET2_R && b < 255) used[b] = 1;
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
        }
        if (op->id == O_HSETP2_R || op->id == O_HSETP2_C || op->id == O_HSETP2_I) {
            needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (op->id == O_HSETP2_R && b < 255) used[b] = 1;
            u32 pp[] = {B(insn[i], 39, 3), B(insn[i], 3, 3), B(insn[i], 0, 3)};
            for (u32 k = 0; k < 3; k++)
                if (pp[k] < 7) pred_used[pp[k]] = 1;
        }
        if ((op->id >= O_DADD_R && op->id <= O_DADD_I) || (op->id >= O_DMUL_R && op->id <= O_DMUL_I) ||
            (op->id >= O_DFMA_R && op->id <= O_DFMA_I) || (op->id >= O_DMNMX_R && op->id <= O_DMNMX_I) ||
            (op->id >= O_DSET_R && op->id <= O_DSET_I) || (op->id >= O_DSETP_R && op->id <= O_DSETP_I)) {
            int right_reg;
            needs_f64 = needs_glsl = 1;
            if (a < 255) used[a] = 1;
            if (a < 254) used[a + 1] = 1;
            right_reg = op->id == O_DADD_R || op->id == O_DMUL_R || op->id == O_DFMA_R || op->id == O_DFMA_RC ||
                        op->id == O_DMNMX_R || op->id == O_DSET_R || op->id == O_DSETP_R;
            if (right_reg && b < 255) used[b] = 1;
            if (right_reg && b < 254) used[b + 1] = 1;
            if ((op->id >= O_DADD_R && op->id <= O_DADD_I) || (op->id >= O_DMUL_R && op->id <= O_DMUL_I) ||
                (op->id >= O_DFMA_R && op->id <= O_DFMA_I) || (op->id >= O_DMNMX_R && op->id <= O_DMNMX_I)) {
                if (d < 254) used[d + 1] = 1;
                if ((op->id >= O_DFMA_R && op->id <= O_DFMA_I) && B(insn[i], 39, 8) < 255) {
                    used[B(insn[i], 39, 8)] = 1;
                    if (B(insn[i], 39, 8) < 254) used[B(insn[i], 39, 8) + 1] = 1;
                }
            }
            if ((op->id >= O_DMNMX_R && op->id <= O_DMNMX_I) || (op->id >= O_DSET_R && op->id <= O_DSET_I) ||
                (op->id >= O_DSETP_R && op->id <= O_DSETP_I))
                if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (op->id >= O_DSETP_R && op->id <= O_DSETP_I) {
                if (B(insn[i], 3, 3) < 7) pred_used[B(insn[i], 3, 3)] = 1;
                if (B(insn[i], 0, 3) < 7) pred_used[B(insn[i], 0, 3)] = 1;
            }
        }
        if (op->id == O_FSWZADD) {
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
        }
        if (op->id == O_VMNMX) {
            if (a < 255) used[a] = 1;
            if (B(insn[i], 50, 1) && b < 255) used[b] = 1;
            u32 third = B(insn[i], 39, 8);
            if (third < 255) used[third] = 1;
        }
        if (op->id == O_VMAD) {
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            u32 third = B(insn[i], 39, 8);
            if (third < 255) used[third] = 1;
        }
        if (op->id == O_VSETP) {
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (B(insn[i], 39, 3) < 7) pred_used[B(insn[i], 39, 3)] = 1;
            if (B(insn[i], 3, 3) < 7) pred_used[B(insn[i], 3, 3)] = 1;
            if (B(insn[i], 0, 3) < 7) pred_used[B(insn[i], 0, 3)] = 1;
        }
        if (op->id == O_TLDS) {
            u32 off = B(insn[i], 36, 13);
            u32 enc = B(insn[i], 53, 4);
            int array = enc == 8, type = enc == 0 || enc == 1 ? 0 : enc == 7 ? 2 : 1;
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].type == type && textures[slot].array == array &&
                    !textures[slot].shadow && !textures[slot].bindless && !textures[slot].flags)
                    break;
            if (slot == texture_count) {
                if (texture_count >= 32) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].array = (u8)array;
                texture_count++;
            }
            if (a < 255) used[a] = 1;
            if ((enc == 4 || enc == 5 || enc == 7 || enc == 12) && a + 1 < 255) used[a + 1] = 1;
            if (b < 255) used[b] = 1;
            if ((enc == 12 || array) && b + 1 < 255) used[b + 1] = 1;
            u32 regs[4], comps[4];
            int outputs = texs_outputs(insn[i], regs, comps);
            if (!B(insn[i], 59, 1)) {
                /* fp16 results pack component pairs into the destination and the second register */
                needs_f16 = 1;
                if (d < 255) used[d] = 1;
                if (outputs > 2 && B(insn[i], 28, 8) < 255) used[B(insn[i], 28, 8)] = 1;
            } else
                for (int k = 0; k < outputs; k++)
                    if (regs[k] < 255) used[regs[k]] = 1;
        }
        if (op->id == O_TEX || op->id == O_TEX_B) {
            int bindless = op->id == O_TEX_B;
            u32 mode = bindless ? B(insn[i], 37, 3) : B(insn[i], 55, 3);
            u32 offset_enabled = B(insn[i], bindless ? 36 : 54, 1);
            u32 source = b + bindless;
            u32 has_level = mode == 2 || mode == 3 || mode == 6 || mode == 7;
            u32 offset_source = source + has_level;
            u32 off, bank, bank2, off2, indexed = 0, ldc = 0;
            int handle_count;
            if (offset_enabled) {
                u32 packed;
                int known = direct_track_const(insn, n, i, offset_source, &packed, 0);
                if (!known) needs_image_query = 1; /* the offset moves the coordinates by the texture's size */
            }
            off = B(insn[i], 36, 13);
            bank = 0;
            bank2 = 0;
            off2 = 0;
            handle_count = bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            int type = B(insn[i], 28, 3) >> 1;
            int array = B(insn[i], 28, 1), shadow = B(insn[i], 50, 1);
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].shadow == shadow && textures[slot].type == type &&
                    textures[slot].array == array && textures[slot].bindless == bindless &&
                    !(textures[slot].flags & 3) &&
                    (!bindless ||
                     direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc)))
                    break;
            if (slot == texture_count) {
                if (texture_count >= 32) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].array = (u8)array;
                textures[slot].shadow = (u8)shadow;
                textures[slot].bindless = (u8)bindless;
                textures[slot].bl_cbuf = (u8)bank;
                textures[slot].bl2 = handle_count == 2;
                textures[slot].bl2_cbuf = (u8)bank2;
                textures[slot].bl2_off = off2;
                if (indexed) {
                    textures[slot].flags |= (u8)indexed;
                    textures[slot].bl2_off = ldc;
                }
                texture_count++;
            }
            if (a < 255) used[a] = 1;
            if ((type != 0 || array) && a + 1 < 255) used[a + 1] = 1;
            if ((type >= 2 || array) && a + 2 < 255) used[a + 2] = 1;
            if (type == 3 && array && a + 3 < 255) used[a + 3] = 1;
            if (b < 255) used[b] = 1;
            if (has_level && source < 255) used[source] = 1;
            if (offset_enabled && offset_source < 255) used[offset_source] = 1;
            if (shadow && offset_source + offset_enabled < 255) used[offset_source + offset_enabled] = 1;
            u32 mask = B(insn[i], 31, 4), dst = d;
            if (!mask) mask = 15;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component)) {
                    if (dst < 255) used[dst] = 1;
                    if (dst < 255) dst++;
                }
        }
        if (op->id == O_TMML) {
            u32 off = B(insn[i], 36, 13);
            int slot = direct_query_slot(textures, texture_count, off, 0, 0, 0, 0, 0, 0, 0);
            if (slot == texture_count) {
                if (texture_count >= 32) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = 1;
                texture_count++;
            }
            if (a < 255) used[a] = 1;
            if (a + 1 < 255) used[a + 1] = 1;
            needs_image_query = 1;
        }
        if (op->id == O_TXQ || op->id == O_TXQ_B) {
            /* TXQ.B takes the handle in Ra and the level in Ra + 1 */
            int bindless = op->id == O_TXQ_B;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
            int slot;
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, a, stage, &ldc);
            slot = direct_query_slot(textures, texture_count, off, bindless, bank, handle_count, bank2, off2, indexed,
                                     ldc);
            if (slot == texture_count) {
                if (texture_count >= 32) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = 1;
                textures[slot].bindless = (u8)bindless;
                textures[slot].bl_cbuf = (u8)bank;
                textures[slot].bl2 = handle_count == 2;
                textures[slot].bl2_cbuf = (u8)bank2;
                textures[slot].bl2_off = off2;
                if (indexed) {
                    textures[slot].flags |= (u8)indexed;
                    textures[slot].bl2_off = ldc;
                }
                texture_count++;
            }
            needs_image_query = 1;
            if (a < 255) used[a] = 1;
            if (bindless && a + 1 < 255) used[a + 1] = 1;
            u32 target = d;
            for (u32 k = 0; k < 4; k++)
                if (B(insn[i], 31 + k, 1)) {
                    if (target < 255) used[target] = 1;
                    target++;
                }
        }
        if (op->id == O_TLD4S || op->id == O_TLD4 || op->id == O_TLD4_B) {
            u32 indexed = 0, ldc = 0;
            int bindless = op->id == O_TLD4_B;
            u32 off = B(insn[i], 36, 13), bank = 0, bank2 = 0, off2 = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (bindless && !handle_count) return direct_reject(si, "untracked texture handle");
            u32 ty = op->id == O_TLD4S ? 2 : B(insn[i], 28, 3);
            u32 type = ty >> 1, array = ty & 1;
            u32 shadow = B(insn[i], 50, 1);
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].type == type && textures[slot].array == array &&
                    textures[slot].shadow == shadow && textures[slot].bindless == bindless &&
                    (!bindless ||
                     direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc)))
                    break;
            if (slot == texture_count) {
                if (texture_count >= 32) return direct_reject(si, "texture descriptor count");
                textures[slot].cbuf_off = off;
                textures[slot].type = (u8)type;
                textures[slot].array = (u8)array;
                textures[slot].shadow = (u8)shadow;
                textures[slot].bindless = (u8)bindless;
                textures[slot].bl_cbuf = (u8)bank;
                textures[slot].bl2 = handle_count == 2;
                textures[slot].bl2_cbuf = (u8)bank2;
                textures[slot].bl2_off = off2;
                if (indexed) {
                    textures[slot].flags |= (u8)indexed;
                    textures[slot].bl2_off = ldc;
                }
                texture_count++;
            }
            if (a < 255) used[a] = 1;
            if (op->id == O_TLD4S) {
                if (b < 255) used[b] = 1;
                if (B(insn[i], 51, 1)) needs_gather_extended = 1;
                if (B(insn[i], 55, 1)) needs_glsl = 1;
                if (B(insn[i], 50, 1) || B(insn[i], 51, 1)) {
                    if (a + 1 < 255) used[a + 1] = 1;
                    if (B(insn[i], 50, 1) && B(insn[i], 51, 1) && b + 1 < 255) used[b + 1] = 1;
                }
                u32 db = B(insn[i], 28, 8);
                if (!B(insn[i], 55, 1) && d < 254) used[d + 1] = 1;
                if (db < 255) used[db] = 1;
                if (!B(insn[i], 55, 1) && db < 254) used[db + 1] = 1;
            } else {
                u32 coords = (type == 0 ? 1u : type == 1 ? 2u : 3u) + array;
                for (u32 k = 1; k < coords; k++)
                    if (a + k < 255) used[a + k] = 1;
                u32 extra = b + bindless;
                if (B(insn[i], bindless ? 36 : 54, 2) == 1) {
                    if (extra < 255) used[extra] = 1;
                    extra++;
                    needs_gather_extended = 1;
                } else if (B(insn[i], bindless ? 36 : 54, 2) == 2) {
                    if (extra < 255) used[extra] = 1;
                    if (extra + 1 < 255) used[extra + 1] = 1;
                    extra += 2;
                    needs_gather_extended = 1;
                }
                if (shadow && extra < 255) used[extra] = 1;
                u32 mask = B(insn[i], 31, 4), dst = d;
                if (!mask) mask = 15;
                for (u32 component = 0; component < 4; component++)
                    if (mask & (1u << component) && dst < 255) used[dst++] = 1;
            }
        }
        if (op->id == O_IPA) {
            if (B(insn[i], 51, 1)) needs_glsl = 1;
            u32 addr = B(insn[i], 30, 8) * 4;
            if (addr == 0x64) {
                layer_input_used = 1;
            } else if (addr == 0x3fc) {
                needs_frontface = 1;
            } else if (addr >= 0x80) {
                u32 input = (addr - 0x80) >> 4;
                u32 interpolation = ipa_interpolation(sph, input);
                inputs_used[input] = 1;
                if (interpolation == 2) needs_fragcoord = 1;
            } else
                needs_fragcoord = 1;
            if (addr == 0x70 || addr == 0x74) needs_scale = 1;
            if (B(insn[i], 54, 2) == 1 && b < 255) used[b] = 1;
        }
        if (op->id == O_EXIT && flow_always(insn[i])) has_exit = 1;
        if (op->id == O_VOTE || op->id == O_SHFL) needs_subgroup = 1;
        /* the lane and its masks come from the subgroup, where VOTE and SHFL see the lanes */
        if (op->id == O_S2R && (B(insn[i], 20, 8) == 0 || (B(insn[i], 20, 8) >= 0x38 && B(insn[i], 20, 8) <= 0x3c)))
            needs_subgroup = 1;
    }
    if (!has_exit) return direct_reject(si, "missing exit");
    if (needs_subgroup && !can_subgroup) return direct_reject(si, "subgroup ballot/shuffle unavailable");
    /* The flow loop runs one block per iteration, so lanes that took the two sides of a branch reach the join on
       different iterations, and VOTE, SHFL and BAR would see part of the warp. Each iteration runs only the lanes
       at the subgroup's lowest pc, as the hardware reconverges: lanes ahead wait for the others to reach them. */
    converge = has_flow && can_subgroup && (needs_subgroup || has_barrier);
    if (needs_shared && (stage != ST_CS || si->shared_size <= 0)) return direct_reject(si, "shared memory size");
    for (int i = 0; i < texture_count; i++)
        if (textures[i].shadow && (si->tex_int_mask & (1u << i))) return direct_reject(si, "integer shadow texture");
    u32* pop_targets = has_stack ? calloc(n, sizeof *pop_targets) : NULL;
    if (has_stack && !pop_targets) return NULL;
    int dynamic_stack = has_stack && !shader_resolve_crs(insn, n, pop_targets);

    u8* gb_at = calloc(n, 1);
    if (!gb_at) {
        free(pop_targets);
        return NULL;
    }
    int gtags[255] = {0}, ngbuf = 0;
    u32 gbuf_wmask = 0;
    u32 gbuf_off[16] = {0};
    for (u32 i = 11; i < n; i++) {
        u32 second;
        u32 r;
        if (!IDX_OK(i)) continue;
        u64 w = insn[i];
        int op = decode(w)->id;
        u32 d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8);
        int tag = 0;
        if (op == O_MOV_C)
            tag = gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
        else if (op == O_LDC && a == 255 && (B(w, 48, 3) == 4 || B(w, 48, 3) == 5))
            tag = gbuf_tag(stage, B(w, 36, 5), (u32)(i32)(i16)B(w, 20, 16));
        else if (op == O_MOV_R)
            tag = b == 255 ? 0 : gtags[b];
        else if (op == O_IADD32I && a < 255)
            tag = gtags[a];
        else if (op == O_IADD_I && a < 255 && B(w, 48, 2) != 1)
            tag = gtags[a];
        else if (op == O_IADD_C) {
            tag = gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
            if (!tag && B(w, 48, 2) != 2 && a < 255) tag = gtags[a];
            if (B(w, 48, 2) == 1) tag = 0;
        } else if (op == O_IADD_R) {
            tag = B(w, 48, 2) == 1      ? (a < 255 ? gtags[a] : 0)
                  : B(w, 48, 2) == 2    ? (b < 255 ? gtags[b] : 0)
                  : a < 255 && gtags[a] ? gtags[a]
                  : b < 255             ? gtags[b]
                                        : 0;
        } else if (op == O_ISCADD_C && B(w, 48, 2) != 1)
            tag = gbuf_tag(stage, B(w, 34, 5), B(w, 20, 14) * 4);
        else if (op == O_ISCADD_R && B(w, 48, 2) != 1 && b < 255)
            tag = gtags[b];
        if (tag == 0xb1 && op != O_MOV_C && op != O_LDC && op != O_MOV_R) tag = 0;
        if (op == O_LDG || op == O_STG || op == O_RED || op == O_ATOM) {
            int current = a < 255 ? gtags[a] : 0, slot = 0;
            if (!current) {
                free(gb_at);
                free(pop_targets);
                return direct_reject(si, "global memory base not tracked");
            }
            if (current == 0xb1) {
                i32 byte_offset = (i32)(B(w, 20, 24) << 8) >> 8;
                u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
                if ((op != O_LDG && op != O_STG) || byte_offset < 0 || (u32)byte_offset + words * 4 > 16) {
                    free(gb_at);
                    free(pop_targets);
                    return direct_reject(si, "vertex pointer access exceeds known range");
                }
            }
            u32 descriptor = (u32)(current - 1);
            for (; slot < ngbuf && gbuf_off[slot] != descriptor; slot++) {}
            if (slot == ngbuf) {
                if (ngbuf == 16) {
                    free(gb_at);
                    free(pop_targets);
                    return direct_reject(si, "global buffer count");
                }
                gbuf_off[ngbuf++] = descriptor;
            }
            gb_at[i] = (u8)(slot + 1);
            if (op != O_LDG) gbuf_wmask |= 1u << slot;
            cbuf_used[0] = 1;
            if ((descriptor | 15u) + 1 > cbuf_max[0]) cbuf_max[0] = (descriptor | 15u) + 1;
        }
        /* every register the instruction writes stops being a pointer, then its destination takes the new tag */
        second = op == O_TEXS || op == O_TLDS || op == O_TLD4S ? B(w, 28, 8) : 255;
        for (r = d; r < d + 4 && r < 255; r++)
            if (shader_writes_reg(w, op, r)) gtags[r] = 0;
        for (r = second; r < second + 2 && r < 255; r++)
            if (shader_writes_reg(w, op, r)) gtags[r] = 0;
        if (shader_writes_reg(w, op, d)) gtags[d] = tag;
    }

    /* the primitives of the stages between: the control program's output patch (the SPH's threads per input
       primitive), the evaluation program's domain, spacing and output (ShaderInfo.rt_int_mask), and the geometry
       program's input and output primitives */
    if (stage == ST_TCS && (!out_vertices || out_vertices > 32)) return direct_reject(si, "patch output vertices");
    if (stage == ST_TCS && (clip_used || layer_output_used || point_size_used))
        return direct_reject(si, "clip distance, layer or point size in tessellation control");
    if (stage == ST_TES && ((si->rt_int_mask & 3) == 3 || ((si->rt_int_mask >> 4) & 3) == 3))
        return direct_reject(si, "tessellation domain or spacing");
    if (stage == ST_GS && in_vertices != 1 && in_vertices != 2 && in_vertices != 3 && in_vertices != 4 &&
        in_vertices != 6)
        return direct_reject(si, "geometry input primitive");
    if (stage == ST_GS && ((sph[3] >> 24) & 15) != 1 && ((sph[3] >> 24) & 15) != 6 && ((sph[3] >> 24) & 15) != 7)
        return direct_reject(si, "geometry output topology");
    if (stage == ST_GS && ((sph[4] & 0xfff) > 256 || (sph[2] >> 24) > 32))
        return direct_reject(si, "geometry output vertices or invocations");
    /* every indexed texture reads the one array binding: one table of handles, one kind of texture */
    for (k_indexed = 0; k_indexed < texture_count; k_indexed++) {
        const ShTex* t = &textures[k_indexed];
        if (!(t->flags & SNX_TEX_INDEXED)) continue;
        if (t->flags & 3) return direct_reject(si, "indexed multisample texture"); /* the array is single-sampled */
        if (first_indexed < 0) {
            first_indexed = k_indexed;
            continue;
        }
        if (t->bl_cbuf != textures[first_indexed].bl_cbuf || t->cbuf_off != textures[first_indexed].cbuf_off ||
            t->flags != textures[first_indexed].flags || t->type != textures[first_indexed].type ||
            t->array != textures[first_indexed].array || t->shadow != textures[first_indexed].shadow)
            return direct_reject(si, "two indexed texture tables");
    }
    DirectCtx* c = calloc(1, sizeof *c);
    if (!c) {
        free(gb_at);
        free(pop_targets);
        return NULL;
    }
    spv_emit_init(&c->emit, (size_t)n * 20 + 128);
    spv_emit_shader_capability(&c->emit);
    if (stage == ST_TCS || stage == ST_TES) spv_emit_capability(&c->emit, 3); /* Tessellation */
    if (stage == ST_GS) spv_emit_capability(&c->emit, 2);                     /* Geometry */
    if (needs_f16) spv_emit_float16_capability(&c->emit);
    if (needs_f64) spv_emit_float64_capability(&c->emit);
    if (needs_subgroup || converge) spv_emit_subgroup_capabilities(&c->emit);
    if (converge) spv_emit_subgroup_arithmetic_capability(&c->emit);
    if (needs_gather_extended) spv_emit_image_gather_extended_capability(&c->emit);
    if (needs_instance) spv_emit_draw_parameters_capability(&c->emit);
    if (needs_image_query) spv_emit_image_query_capability(&c->emit);
    if (needs_glsl) c->glsl = spv_emit_glsl450_import(&c->emit);
    u32 fn = spv_emit_id(&c->emit), label = spv_emit_id(&c->emit);
    u32 void_type = spv_emit_type_void(&c->emit);
    c->u32_type = spv_emit_type_u32(&c->emit);
    u32 i32_type = spv_emit_type_i32(&c->emit);
    c->f32_type = spv_emit_type_f32(&c->emit);
    if (needs_f16) c->f16_type = spv_emit_type_f16(&c->emit);
    if (needs_f64) c->f64_type = spv_emit_type_f64(&c->emit);
    c->bool_type = spv_emit_type_bool(&c->emit);
    c->true_value = spv_emit_constant_bool(&c->emit, c->bool_type, 1);
    c->false_value = spv_emit_constant_bool(&c->emit, c->bool_type, 0);
    u32 vec4 = spv_emit_type_vec4(&c->emit, c->f32_type);
    u32 vec4_u32 = spv_emit_type_vec4(&c->emit, c->u32_type);
    u32 vec4_i32 = spv_emit_type_vec4(&c->emit, i32_type);
    u32 vec2 = spv_emit_type_vec2(&c->emit, c->f32_type);
    u32 vec2f16 = needs_f16 ? spv_emit_type_vec2(&c->emit, c->f16_type) : 0;
    u32 vec2_u32 = spv_emit_type_vec2(&c->emit, c->u32_type);
    u32 vec2_i32 = spv_emit_type_vec2(&c->emit, i32_type);
    u32 vec3 = spv_emit_type_vec3(&c->emit, c->f32_type);
    u32 vec3_u32 = spv_emit_type_vec3(&c->emit, c->u32_type);
    u32 vec3_i32 = spv_emit_type_vec3(&c->emit, i32_type);
    u32 fn_type = spv_emit_type_function_void(&c->emit, void_type);
    u32 reg_ptr = spv_emit_type_function_pointer(&c->emit, c->u32_type);
    c->zero = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 zero_f = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    /* Maximum VS interface: point size, position, 32 outputs, 32 inputs, 18 buffers, 32 samplers, push constants. */
    u32 outputs[8], inputs[32] = {0}, input_vec_type[32] = {0}, input_component_type[32] = {0};
    u32 output_attrs[32] = {0}, position = 0, point_size = 0, layer_output = 0, layer_input = 0;
    u32 clip_output = 0, clip_count = 0;
    u32 depth_output = 0, interfaces[256], count = 0;
    /* the stages between: the levels and vectors of a patch, and the inputs of each vertex */
    u32 tess_outer = 0, tess_inner = 0, patch_vars[30] = {0}, position_in = 0, primitive_id = 0, tess_coord = 0;
    u32 invocation_id = 0, patch_vertices = 0;
    int depth_fix = position_used && last && stage != ST_FS && stage != ST_CS;
    /* BuiltIn Layer takes the Geometry capability (a geometry program has it; the device's geometryShader) or
       ShaderLayer (its shaderOutputLayer), which a vertex or tessellation program writing the layer needs. A fragment
       program reads it with either. Without one a write is dropped and a read is 0 */
    if (layer_output_used && stage != ST_GS && !g_shader_output_layer) layer_output_used = 0;
    if (layer_input_used && !g_shader_geometry && !g_shader_output_layer) layer_input_used = 0;
    if ((layer_output_used && stage != ST_GS) || (layer_input_used && !g_shader_geometry))
        spv_emit_layer_capability(&c->emit);
    else if (layer_input_used && stage != ST_GS)
        spv_emit_capability(&c->emit, 2); /* Geometry */
    if (clip_used) spv_emit_clip_distance_capability(&c->emit);
    if (stage == ST_FS)
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                u32 color_type =
                    (si->rt_int_mask & (1u << rt)) ? (si->rt_sint_mask & (1u << rt)) ? vec4_i32 : vec4_u32 : vec4;
                outputs[count] = spv_emit_output_vec4(&c->emit, color_type, rt);
                interfaces[count] = outputs[count];
                count++;
            }
    if (stage == ST_FS && (sph[19] & 2u)) {
        depth_output = spv_emit_frag_depth_output(&c->emit, c->f32_type);
        interfaces[count++] = depth_output;
    }
    if (stage != ST_FS && stage != ST_CS) {
        /* a tessellation control program's outputs are arrays of its output vertices */
        u32 per_vertex = stage == ST_TCS ? out_vertices : 0;
        if (stage == ST_VS || (stage != ST_TCS && g_shader_stage_point_size)) {
            if (stage == ST_TES) spv_emit_capability(&c->emit, 23); /* TessellationPointSize */
            if (stage == ST_GS) spv_emit_capability(&c->emit, 24);  /* GeometryPointSize */
            point_size = spv_emit_point_size_output(&c->emit, c->f32_type);
            interfaces[count++] = point_size;
        }
        if (layer_output_used) {
            layer_output = spv_emit_layer_output(&c->emit, i32_type);
            interfaces[count++] = layer_output;
        }
        if (clip_used) {
            u32 i;
            for (i = 0; i < 8; i++)
                if (clip_used & (1u << i)) clip_count = i + 1;
            clip_output = spv_emit_clip_distance_output(&c->emit, c->f32_type, c->u32_type, clip_count);
            interfaces[count++] = clip_output;
        }
        if (position_used) {
            position = per_vertex ? spv_emit_interface_variable(&c->emit, 3, vec4, c->u32_type, per_vertex, 0, 0, 0)
                                  : spv_emit_position_output(&c->emit, vec4);
            interfaces[count++] = position;
        }
        for (u32 i = 0; i < 32; i++)
            if (outputs_used[i]) {
                output_attrs[i] = per_vertex ? spv_emit_interface_variable(&c->emit, 3, vec4, c->u32_type, per_vertex,
                                                                           SPV_NOT_BUILTIN, i, 0)
                                             : spv_emit_output_vec4(&c->emit, vec4, i);
                interfaces[count++] = output_attrs[i];
            }
    }
    if (stage == ST_TCS || stage == ST_TES) {
        /* a patch's levels and vectors: the control program's outputs, the evaluation program's inputs */
        u32 storage = stage == ST_TCS ? 3 : 1, i;
        if (levels_used & 1) {
            tess_outer = spv_emit_interface_variable(&c->emit, storage, c->f32_type, c->u32_type, 4, 11, 0, 1);
            interfaces[count++] = tess_outer;
        }
        if (levels_used & 2) {
            tess_inner = spv_emit_interface_variable(&c->emit, storage, c->f32_type, c->u32_type, 2, 12, 0, 1);
            interfaces[count++] = tess_inner;
        }
        for (i = 0; i < 30; i++)
            if (patch_used[i]) {
                patch_vars[i] =
                    spv_emit_interface_variable(&c->emit, storage, vec4, c->u32_type, 0, SPV_NOT_BUILTIN, i, 1);
                interfaces[count++] = patch_vars[i];
            }
        patch_vertices = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 14, 0, 0);
        interfaces[count++] = patch_vertices;
    }
    if (between) {
        if (position_in_used) {
            position_in = spv_emit_interface_variable(&c->emit, 1, vec4, c->u32_type, in_vertices, 0, 0, 0);
            interfaces[count++] = position_in;
        }
        if (primitive_id_used) {
            primitive_id = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 7, 0, 0);
            interfaces[count++] = primitive_id;
        }
        if (tess_coord_used) {
            tess_coord = spv_emit_interface_variable(&c->emit, 1, vec3, c->u32_type, 0, 13, 0, 0);
            interfaces[count++] = tess_coord;
        }
        if (invocation_used) {
            invocation_id = spv_emit_interface_variable(&c->emit, 1, c->u32_type, c->u32_type, 0, 8, 0, 0);
            interfaces[count++] = invocation_id;
        }
    }
    for (u32 i = 0; i < 32; i++) {
        if (!inputs_used[i]) continue;
        u32 interpolation = 0;
        if (stage == ST_FS) {
            u32 mode_byte = ((const u8*)sph)[0x18 + i];
            u32 k = 0;
            while (k < 3 && !((mode_byte >> (k * 2)) & 3)) k++;
            u32 mode = (mode_byte >> (k * 2)) & 3;
            interpolation = mode == 1 ? 1 : mode == 3 ? 2 : 0;
        }
        u32 component_type = c->f32_type;
        u32 vector_type = vec4;
        if (stage == ST_VS && (si->attr_int_mask & (1u << i))) {
            int signed_input = (si->attr_sint_mask & (1u << i)) != 0;
            component_type = signed_input ? i32_type : c->u32_type;
            vector_type = signed_input ? vec4_i32 : vec4_u32;
        }
        input_vec_type[i] = vector_type;
        input_component_type[i] = component_type;
        inputs[i] = between ? spv_emit_interface_variable(&c->emit, 1, vector_type, c->u32_type, in_vertices,
                                                          SPV_NOT_BUILTIN, i, 0)
                            : spv_emit_input_vec4(&c->emit, vector_type, i, interpolation);
        interfaces[count++] = inputs[i];
    }
    u32 fragcoord = 0;
    if (layer_input_used) {
        layer_input = spv_emit_builtin_input_u32(&c->emit, i32_type, 9);
        interfaces[count++] = layer_input;
    }
    u32 frontface = 0;
    if (needs_frontface) {
        frontface = spv_emit_builtin_input_u32(&c->emit, c->bool_type, 17);
        interfaces[count++] = frontface;
    }
    u32 instance_index = 0, base_instance = 0, vertex_index = 0;
    u32 local_input = 0, group_input = 0;
    if (needs_local_id) {
        local_input = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 27);
        interfaces[count++] = local_input;
    }
    if (needs_group_id) {
        group_input = spv_emit_builtin_input_vec3(&c->emit, vec3_u32, 26);
        interfaces[count++] = group_input;
    }
    if (needs_subgroup) {
        c->subgroup_lane_input = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 41);
        if (stage == ST_FS) spv_emit_flat(&c->emit, c->subgroup_lane_input);
        interfaces[count++] = c->subgroup_lane_input;
    }
    if (needs_instance) {
        instance_index = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 43);
        base_instance = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 4425);
        vertex_index = spv_emit_builtin_input_u32(&c->emit, c->u32_type, 42);
        interfaces[count++] = instance_index;
        interfaces[count++] = base_instance;
        interfaces[count++] = vertex_index;
    }
    if (needs_fragcoord) {
        fragcoord = spv_emit_fragcoord_input(&c->emit, vec4);
        interfaces[count++] = fragcoord;
    }
    if (needs_scale) {
        c->scale_buffer =
            spv_emit_uniform_vec4_array(&c->emit, vec4, c->f32_type, c->u32_type, 1, 74, 1, &c->scale_pointer);
        interfaces[count++] = c->scale_buffer;
    }
    for (u32 i = 0; i < 18; i++) {
        if (!cbuf_used[i]) continue;
        c->cbuf[i] = spv_emit_uniform_vec4_array(&c->emit, vec4_u32, c->u32_type, c->u32_type, shader_vk_set(stage), i,
                                                 4096, &c->cbuf_pointer[i]);
        interfaces[count++] = c->cbuf[i];
    }
    u32 texture_scalar[32], texture_vec4[32];
    for (int i = 0; i < texture_count; i++) {
        u32 scalar = (textures[i].flags & 1)          ? c->u32_type
                     : (si->tex_int_mask & (1u << i)) ? (si->tex_sint_mask & (1u << i)) ? i32_type : c->u32_type
                                                      : c->f32_type;
        texture_scalar[i] = scalar;
        texture_vec4[i] = scalar == i32_type ? vec4_i32 : scalar == c->u32_type ? vec4_u32 : vec4;
        if (textures[i].flags & SNX_TEX_INDEXED) {
            if (i == first_indexed) {
                c->texture_array =
                    spv_emit_sampler_image_array(&c->emit, scalar, c->u32_type, shader_vk_set(stage), SNX_VKB_TEXARRAY,
                                                 textures[i].type == 0 ? 1 : textures[i].type, textures[i].array,
                                                 textures[i].shadow, SNX_TEX_ARRAY, &c->sampled_type[i]);
                interfaces[count++] = c->texture_array;
            } else
                c->sampled_type[i] = c->sampled_type[first_indexed];
            c->sampler[i] = c->texture_array;
            continue;
        }
        c->sampler[i] = spv_emit_sampler_image(&c->emit, scalar, shader_vk_set(stage), 34 + (u32)i,
                                               textures[i].type == 0 ? 1 : textures[i].type, textures[i].array,
                                               textures[i].shadow, !!(textures[i].flags & 2), &c->sampled_type[i]);
        interfaces[count++] = c->sampler[i];
    }
    /* A buffer's formatted stores are typeless: compute binds a view in the buffer's own format, or in the integer
       format of the same bits */
    for (int i = 0; i < image_info.nimg; i++) {
        int integer = image_info.img[i].kind != 0, buffer = image_info.img[i].type == 1;
        c->image[i] =
            spv_emit_storage_image(&c->emit, integer ? c->u32_type : c->f32_type, shader_vk_set(stage),
                                   (buffer ? SNX_VKB_IMGBUF : SNX_VKB_IMG) + (u32)i, buffer ? 5 : 1, 0,
                                   integer && (!buffer || image_info.img[i].kind == 1) ? 33 : 0, &c->image_type[i]);
        interfaces[count++] = c->image[i];
    }
    if (ngbuf) {
        u32 array = spv_emit_type_runtime_array(&c->emit, c->u32_type, 4);
        u32 structure = spv_emit_type_struct_one(&c->emit, array);
        c->gbuf_pointer = spv_emit_type_storage_pointer(&c->emit, c->u32_type);
        for (int i = 0; i < ngbuf; i++) {
            c->gbuf[i] = spv_emit_storage_buffer(&c->emit, structure, shader_vk_set(stage), SNX_VKB_SSBO + (u32)i);
            interfaces[count++] = c->gbuf[i];
        }
        if (stage != ST_CS) {
            c->smap = spv_emit_storage_buffer(&c->emit, structure, shader_vk_set(stage), SNX_VKB_SMAP);
            interfaces[count++] = c->smap;
        }
    }
    u32 shared_array = 0, shared_element_pointer = 0, shared_words = 0;
    if (needs_shared) {
        shared_words = ((u32)si->shared_size + 3) / 4;
        u32 array_type = spv_emit_type_array_u32(&c->emit, c->u32_type, shared_words);
        u32 pointer_type = spv_emit_type_workgroup_pointer(&c->emit, array_type);
        shared_element_pointer = spv_emit_type_workgroup_pointer(&c->emit, c->u32_type);
        shared_array = spv_emit_workgroup_variable(&c->emit, pointer_type);
        interfaces[count++] = shared_array;
    }
    u32 push = 0, push_pointer = 0;
    if ((stage == ST_FS && (target & 15)) || depth_fix) {
        push = spv_emit_push_vec4(&c->emit, vec4, c->f32_type, stage == ST_FS ? 16 : 0, &push_pointer);
        interfaces[count++] = push;
    }
    spv_emit_entry_str(&c->emit, (SpvShaderStage)stage, fn, "main", interfaces, count); /* ST_ is SPIR-V's model */
    if (stage == ST_TCS) spv_emit_execution_mode(&c->emit, fn, 26, 1, out_vertices);    /* OutputVertices */
    if (stage == ST_TES) {
        /* 0xC8: the domain (isolines, triangles, quads), the spacing (equal, fractional odd and even) and the output
           (points, lines, triangles clockwise and counterclockwise) */
        u32 domain = si->rt_int_mask & 3, spacing = (si->rt_int_mask >> 4) & 3, output = (si->rt_int_mask >> 8) & 3;
        spv_emit_execution_mode(&c->emit, fn, domain == 0 ? 25 : domain == 1 ? 22 : 24, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, spacing == 0 ? 1 : spacing == 1 ? 3 : 2, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, output == 2 ? 4 : 5, 0, 0); /* VertexOrderCw, VertexOrderCcw */
        if (output == 0) spv_emit_execution_mode(&c->emit, fn, 10, 0, 0); /* PointMode */
    }
    if (stage == ST_GS) {
        /* the input primitive, the SPH's output topology and vertices, and its threads per input primitive */
        u32 topology = (sph[3] >> 24) & 15, invocations = sph[2] >> 24;
        spv_emit_execution_mode(&c->emit, fn,
                                in_vertices == 1   ? 19
                                : in_vertices == 2 ? 20
                                : in_vertices == 4 ? 21
                                : in_vertices == 6 ? 23
                                                   : 22,
                                0, 0);
        spv_emit_execution_mode(&c->emit, fn, topology == 1 ? 27 : topology == 6 ? 28 : 29, 0, 0);
        spv_emit_execution_mode(&c->emit, fn, 26, 1, (sph[4] & 0xfff) ? sph[4] & 0xfff : 1);
        spv_emit_execution_mode(&c->emit, fn, 0, 1, invocations ? invocations : 1); /* Invocations */
    }
    if (stage == ST_CS) spv_emit_local_size(&c->emit, fn, si->cs_local[0], si->cs_local[1], si->cs_local[2]);
    if (stage == ST_FS) spv_emit_fragment_origin_upper_left(&c->emit, fn);
    if (depth_output) spv_emit_fragment_depth_replacing(&c->emit, fn);
    spv_emit_float_controls(&c->emit, fn, fp32_flush && !fp32_preserve && can_flush, can_preserve);
    spv_emit_function_void(&c->emit, void_type, fn, fn_type);
    spv_emit_label(&c->emit, label);
    u32 local_array = 0, local_element_pointer = 0, local_words = 0;
    if (needs_local) {
        u32 bytes = (sph[1] & 0xffffffu) + (sph[2] & 0xffffffu);
        local_words = 1024;
        if (bytes) {
            local_words = 1;
            while (local_words < 65536 && local_words * 4 < bytes) local_words <<= 1;
        }
        u32 array_type = spv_emit_type_array_u32(&c->emit, c->u32_type, local_words);
        u32 array_pointer = spv_emit_type_function_pointer(&c->emit, array_type);
        local_element_pointer = spv_emit_type_function_pointer(&c->emit, c->u32_type);
        local_array = spv_emit_function_variable(&c->emit, array_pointer);
    }
    for (u32 r = 0; r < 255; r++)
        if (used[r]) c->reg[r] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    for (k_indexed = 0; k_indexed < texture_count; k_indexed++)
        if (textures[k_indexed].flags & SNX_TEX_INDEXED)
            c->tex_index[k_indexed] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    u32 pred_ptr = spv_emit_type_function_pointer(&c->emit, c->bool_type);
    for (u32 p = 0; p < 7; p++)
        if (pred_used[p]) c->pred[p] = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_c = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_o = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_z = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    c->cc_s = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    u32 atomic_old_var = needs_atomic_cas ? spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero) : 0;
    u32 stack_ptr = 0, stack[32] = {0};
    if (dynamic_stack) {
        stack_ptr = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
        for (u32 k = 0; k < 32; k++) stack[k] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    }
    u32 call_ptr = 0, call_stack[32] = {0};
    if (has_calls) {
        call_ptr = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
        for (u32 k = 0; k < 32; k++) call_stack[k] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    }
    u32 pc = 0, loop_header = 0, loop_dispatch = 0, dispatch_default = 0, dispatch_merge = 0, loop_continue = 0,
        loop_merge = 0;
    u32 *case_labels = NULL, *case_literals = NULL;
    u8* block_start = NULL;
    size_t case_count = 0;
    /* every OpVariable comes before the first block's other instructions, the clip distances' stores included */
    if (has_flow)
        pc = spv_emit_function_variable_init(&c->emit, reg_ptr, spv_emit_constant_u32(&c->emit, c->u32_type, 11));
    for (clip = 0; clip < clip_count; clip++) {
        u32 index = spv_emit_constant_u32(&c->emit, c->u32_type, clip);
        u32 ptr = spv_emit_output_element_pointer(&c->emit, c->f32_type, clip_output, index);
        spv_emit_store(&c->emit, ptr, zero_f);
    }
    if (has_flow) {
        loop_header = spv_emit_id(&c->emit);
        loop_dispatch = spv_emit_id(&c->emit);
        dispatch_default = spv_emit_id(&c->emit);
        dispatch_merge = spv_emit_id(&c->emit);
        loop_continue = spv_emit_id(&c->emit);
        loop_merge = spv_emit_id(&c->emit);
        /* one case for each instruction, and the case of lanes waiting to reconverge */
        case_labels = calloc(n + 1, sizeof *case_labels + sizeof *case_literals + 1);
        if (!case_labels) abort();
        case_literals = case_labels + n + 1;
        block_start = (u8*)(case_literals + n + 1);
        block_start[11] = 1;
        int unknown_brx = 0;
        for (u32 i = 11; i < n; i++) {
            if (!IDX_OK(i)) continue;
            int id = decode(insn[i])->id;
            if (id == O_BRX) {
                u32 targets[64];
                int count = shader_brx_targets(insn, i, n, targets, 64);
                if (count < 0)
                    unknown_brx = 1;
                else
                    for (int k = 0; k < count; k++) block_start[targets[k]] = 1;
            }
            if (id == O_BRA || id == O_CAL || id == O_PRET || id == O_SSY || id == O_PBK || id == O_PCNT)
                block_start[bra_target(insn[i], i)] = 1;
            if (!dynamic_stack && (id == O_SYNC || id == O_BRK || id == O_CONT)) block_start[pop_targets[i]] = 1;
            if (id == O_BRA || id == O_CAL || id == O_RET || id == O_BRX || id == O_SYNC || id == O_BRK ||
                id == O_CONT || id == O_EXIT || id == O_KIL) {
                u32 next = next_instruction(i, n);
                if (next < n) block_start[next] = 1;
            }
        }
        /* Unknown jump tables retain a case for every possible instruction target. */
        if (unknown_brx)
            for (u32 i = 11; i < n; i++)
                if (IDX_OK(i)) block_start[i] = 1;
        for (u32 i = 11; i < n; i++)
            if (IDX_OK(i) && block_start[i]) {
                case_literals[case_count] = i;
                case_labels[case_count++] = spv_emit_id(&c->emit);
            }
        if (converge) {
            wait_label = spv_emit_id(&c->emit);
            case_literals[case_count] = UINT32_MAX - 1;
            case_labels[case_count++] = wait_label;
        }
        if (stage == ST_VS || stage == ST_TES || stage == ST_GS) {
            u32 one_f = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
            if (point_size) spv_emit_store(&c->emit, point_size, one_f);
            u32 components[4] = {zero_f, zero_f, zero_f, one_f};
            u32 empty = spv_emit_constant_vec4(&c->emit, vec4, components);
            if (position) spv_emit_store(&c->emit, position, empty);
            for (u32 i = 0; i < 32; i++)
                if (output_attrs[i]) spv_emit_store(&c->emit, output_attrs[i], empty);
        }
        spv_emit_branch(&c->emit, loop_header);
        spv_emit_label(&c->emit, loop_header);
        spv_emit_loop_merge(&c->emit, loop_merge, loop_continue);
        spv_emit_branch(&c->emit, loop_dispatch);
        spv_emit_label(&c->emit, loop_dispatch);
        u32 selector = spv_emit_load(&c->emit, c->u32_type, pc);
        if (converge) {
            u32 lowest = spv_emit_subgroup_umin_u32(&c->emit, c->u32_type, selector);
            selector =
                spv_emit_select(&c->emit, c->u32_type, spv_emit_i_equal(&c->emit, c->bool_type, selector, lowest),
                                selector, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX - 1));
        }
        spv_emit_selection_merge(&c->emit, dispatch_merge);
        spv_emit_switch_u32(&c->emit, selector, dispatch_default, case_literals, case_labels, case_count);
    }
    if ((stage == ST_VS || stage == ST_TES || stage == ST_GS) && !has_flow) {
        u32 one_f = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
        if (point_size) spv_emit_store(&c->emit, point_size, one_f);
        u32 components[4] = {zero_f, zero_f, zero_f, one_f};
        u32 empty = spv_emit_constant_vec4(&c->emit, vec4, components);
        if (position) spv_emit_store(&c->emit, position, empty);
        for (u32 i = 0; i < 32; i++)
            if (output_attrs[i]) spv_emit_store(&c->emit, output_attrs[i], empty);
    }

    size_t case_index = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        if (c->swapped) { /* the array element an indexed texture took for the instruction before */
            c->sampler[c->swapped - 1] = c->texture_array;
            c->swapped = 0;
        }
        if (has_flow && block_start[i]) { spv_emit_label(&c->emit, case_labels[case_index++]); }
        if (has_flow && (next_instruction(i, n) >= n || block_start[next_instruction(i, n)])) {
            u32 following = spv_emit_constant_u32(&c->emit, c->u32_type, next_instruction(i, n));
            spv_emit_store(&c->emit, pc, following);
        }
        u64 w = insn[i];
        int op = decode(w)->id;
        u32 d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8);
        u32 guard_merge = 0;
        if (op != O_SSY && op != O_PBK && op != O_PCNT && op != O_CAL &&
            (B(w, 16, 3) != 7 || B(w, 19, 1) || (flow_has_cc(op) && B(w, 0, 5) != 15))) {
            u32 execute = spv_emit_id(&c->emit);
            guard_merge = spv_emit_id(&c->emit);
            u32 condition = pred_read(c, B(w, 16, 3), B(w, 19, 1));
            if (flow_has_cc(op) && B(w, 0, 5) != 15) condition = cc_and(c, condition, cc_test(c, B(w, 0, 5)));
            spv_emit_selection_merge(&c->emit, guard_merge);
            spv_emit_branch_conditional(&c->emit, condition, execute, guard_merge);
            spv_emit_label(&c->emit, execute);
        }
        if (op == O_CAL || op == O_PRET) {
            /* CAL pushes the instruction after it and jumps. PRET only pushes its target, where a later RET returns */
            u32 sp = spv_emit_load(&c->emit, c->u32_type, call_ptr);
            u32 room = spv_emit_u_less(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
            u32 address =
                spv_emit_constant_u32(&c->emit, c->u32_type, op == O_PRET ? bra_target(w, i) : next_instruction(i, n));
            for (u32 k = 0; k < 32; k++) {
                u32 at = spv_emit_i_equal(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, k));
                u32 old = spv_emit_load(&c->emit, c->u32_type, call_stack[k]);
                spv_emit_store(&c->emit, call_stack[k], spv_emit_select(&c->emit, c->u32_type, at, address, old));
            }
            spv_emit_store(
                &c->emit, call_ptr,
                spv_emit_iadd_u32(&c->emit, c->u32_type, sp,
                                  spv_emit_select(&c->emit, c->u32_type, room,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero)));
            if (op == O_CAL)
                spv_emit_store(&c->emit, pc,
                               spv_emit_select(&c->emit, c->u32_type, room,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, bra_target(w, i)),
                                               spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX)));
        } else if (op == O_RET) {
            u32 sp = spv_emit_load(&c->emit, c->u32_type, call_ptr);
            u32 has_entry = spv_emit_u_less(&c->emit, c->bool_type, c->zero, sp);
            u32 index = spv_emit_isub_u32(&c->emit, c->u32_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
            u32 destination = spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX);
            for (u32 k = 0; k < 32; k++) {
                u32 at =
                    spv_emit_i_equal(&c->emit, c->bool_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, k));
                u32 old = spv_emit_load(&c->emit, c->u32_type, call_stack[k]);
                destination = spv_emit_select(&c->emit, c->u32_type, at, old, destination);
            }
            spv_emit_store(&c->emit, call_ptr, spv_emit_select(&c->emit, c->u32_type, has_entry, index, c->zero));
            spv_emit_store(&c->emit, pc, destination);
        } else if (dynamic_stack && (op == O_SSY || op == O_PBK || op == O_PCNT)) {
            u32 sp = spv_emit_load(&c->emit, c->u32_type, stack_ptr);
            u32 token = spv_emit_constant_u32(&c->emit, c->u32_type, bra_target(w, i) | (u32)CRS_KIND(op) << 16);
            for (u32 k = 0; k < 32; k++) {
                u32 at = spv_emit_i_equal(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, k));
                u32 old = spv_emit_load(&c->emit, c->u32_type, stack[k]);
                spv_emit_store(&c->emit, stack[k], spv_emit_select(&c->emit, c->u32_type, at, token, old));
            }
            u32 room = spv_emit_u_less(&c->emit, c->bool_type, sp, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
            u32 inc =
                spv_emit_select(&c->emit, c->u32_type, room, spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
            spv_emit_store(&c->emit, stack_ptr, spv_emit_iadd_u32(&c->emit, c->u32_type, sp, inc));
        } else if (dynamic_stack && (op == O_SYNC || op == O_BRK || op == O_CONT)) {
            u32 sp = spv_emit_load(&c->emit, c->u32_type, stack_ptr);
            u32 found = c->false_value;
            u32 destination = spv_emit_load(&c->emit, c->u32_type, pc);
            u32 new_sp = sp;
            u32 kind = spv_emit_constant_u32(&c->emit, c->u32_type, CRS_KIND(op));
            u32 shift = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
            u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff);
            for (int k = 31; k >= 0; k--) {
                u32 index = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)k);
                u32 in_range = spv_emit_u_less(&c->emit, c->bool_type, index, sp);
                u32 token = spv_emit_load(&c->emit, c->u32_type, stack[k]);
                u32 matches = spv_emit_i_equal(&c->emit, c->bool_type,
                                               spv_emit_shift_right_u32(&c->emit, c->u32_type, token, shift), kind);
                u32 choose = spv_emit_bool_binary(&c->emit, c->bool_type, 0, in_range, matches);
                choose = spv_emit_bool_binary(&c->emit, c->bool_type, 0, choose,
                                              spv_emit_bool_not(&c->emit, c->bool_type, found));
                destination =
                    spv_emit_select(&c->emit, c->u32_type, choose,
                                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, token, mask), destination);
                new_sp = spv_emit_select(&c->emit, c->u32_type, choose, index, new_sp);
                found = spv_emit_bool_binary(&c->emit, c->bool_type, 1, found, choose);
            }
            spv_emit_store(&c->emit, pc, destination);
            spv_emit_store(&c->emit, stack_ptr, new_sp);
        } else if (op == O_BRA || op == O_SYNC || op == O_BRK || op == O_CONT) {
            u32 target_index = op == O_BRA ? bra_target(w, i) : pop_targets ? pop_targets[i] : 0;
            u32 destination = spv_emit_constant_u32(&c->emit, c->u32_type, target_index);
            spv_emit_store(&c->emit, pc, destination);
        } else if (op == O_BRX) {
            u32 offset = spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
            u32 signed_index =
                spv_emit_iadd_u32(&c->emit, i32_type, spv_emit_bitcast(&c->emit, i32_type, reg_read(c, a)), offset);
            u32 words =
                spv_emit_sdiv_i32(&c->emit, i32_type, signed_index, spv_emit_constant_u32(&c->emit, i32_type, 8));
            u32 target = spv_emit_iadd_u32(&c->emit, c->u32_type, spv_emit_bitcast(&c->emit, c->u32_type, words),
                                           spv_emit_constant_u32(&c->emit, c->u32_type, i + 1));
            u32 after_ten =
                spv_emit_isub_u32(&c->emit, c->u32_type, target, spv_emit_constant_u32(&c->emit, c->u32_type, 10));
            u32 aligned = spv_emit_i_equal(&c->emit, c->bool_type,
                                           spv_emit_bitwise_and_u32(&c->emit, c->u32_type, after_ten,
                                                                    spv_emit_constant_u32(&c->emit, c->u32_type, 3)),
                                           c->zero);
            u32 in_range = spv_emit_bool_not(
                &c->emit, c->bool_type,
                spv_emit_u_less(&c->emit, c->bool_type, target, spv_emit_constant_u32(&c->emit, c->u32_type, 10)));
            u32 needs_skip = spv_emit_bool_binary(&c->emit, c->bool_type, 0, aligned, in_range);
            target = spv_emit_iadd_u32(&c->emit, c->u32_type, target,
                                       spv_emit_select(&c->emit, c->u32_type, needs_skip,
                                                       spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
            spv_emit_store(&c->emit, pc, target);
        } else if (op == O_S2R) {
            u32 sr = B(w, 20, 8);
            u32 value = c->zero;
            if (stage == ST_CS && ((sr >= 0x21 && sr <= 0x23) || (sr >= 0x25 && sr <= 0x27))) {
                u32 local = sr <= 0x23;
                u32 group = spv_emit_load(&c->emit, vec3_u32, local ? local_input : group_input);
                value = spv_emit_vec4_extract(&c->emit, c->u32_type, group, local ? sr - 0x21 : sr - 0x25);
            } else if (sr == 0 || (sr >= 0x38 && sr <= 0x3c)) {
                /* SR_LANEID, and the EQ, LT, LE, GT and GE lane masks of a 32-lane warp */
                u32 lane = spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                                    spv_emit_load(&c->emit, c->u32_type, c->subgroup_lane_input),
                                                    spv_emit_constant_u32(&c->emit, c->u32_type, 31));
                u32 one = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
                u32 eq = spv_emit_shift_left_u32(&c->emit, c->u32_type, one, lane);
                u32 lt = spv_emit_isub_u32(&c->emit, c->u32_type, eq, one);
                u32 le = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, lt, eq);
                value = sr == 0      ? lane
                        : sr == 0x38 ? eq
                        : sr == 0x39 ? lt
                        : sr == 0x3a ? le
                        : sr == 0x3b ? spv_emit_not_u32(&c->emit, c->u32_type, le)
                                     : spv_emit_not_u32(&c->emit, c->u32_type, lt);
            } else if (sr == 0x11) /* SR_INVOCATION_ID */
                value = spv_emit_load(&c->emit, c->u32_type, invocation_id);
            else if (sr == 0x1d && between) {
                /* SR_INVOCATION_INFO: the vertices of the input primitive in bits 16-23, with the primitive's index
                   in the batch (bits 0-7) 0, which makes the index the program builds for ISBERD the vertex's */
                u32 vertices = stage == ST_GS ? spv_emit_constant_u32(&c->emit, c->u32_type, in_vertices)
                                              : spv_emit_load(&c->emit, c->u32_type, patch_vertices);
                value = spv_emit_shift_left_u32(&c->emit, c->u32_type, vertices,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 16));
            } else
                value = sr == 0x12                     ? spv_emit_constant_u32(&c->emit, c->u32_type, 0x3f800000u)
                        : sr == 0x1d && stage != ST_FS ? spv_emit_constant_u32(&c->emit, c->u32_type, 0x00ff0000u)
                                                       : c->zero;
            reg_write(c, d, value);
        } else if (op == O_ALD && (between || B(w, 31, 1))) {
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1, vertex = 0, k;
            int patch = B(w, 31, 1);
            if (!patch) vertex = shared_index(c, reg_read(c, B(w, 39, 8)), in_vertices); /* kept inside the arrays */
            for (k = 0; k < width; k++) {
                u32 location = addr + k * 4, value = c->zero, pointer = 0, storage;
                u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (location >> 2) & 3);
                if (patch) { /* the control program reads back what it wrote */
                    storage = stage == ST_TCS ? 3 : 1;
                    if (location >= 0x20)
                        pointer = spv_emit_interface_scalar_pointer(&c->emit, storage, c->f32_type,
                                                                    patch_vars[(location - 0x20) >> 4], 0, component);
                    else if (location < 0x18)
                        pointer = spv_emit_interface_scalar_pointer(
                            &c->emit, storage, c->f32_type, location < 0x10 ? tess_outer : tess_inner, 0,
                            spv_emit_constant_u32(&c->emit, c->u32_type, (location & 0xf) / 4));
                } else if (location == 0x2f0 || location == 0x2f4)
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, 1, c->f32_type, tess_coord, 0,
                        spv_emit_constant_u32(&c->emit, c->u32_type, (location - 0x2f0) / 4));
                else if (location == 0x60)
                    value = spv_emit_load(&c->emit, c->u32_type, primitive_id);
                else
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, 1, c->f32_type, location < 0x80 ? position_in : inputs[(location - 0x80) >> 4],
                        vertex, component);
                if (pointer)
                    value = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_load(&c->emit, c->f32_type, pointer));
                reg_write(c, d + k, value);
            }
        } else if (op == O_ALD) {
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1;
            for (u32 k = 0; k < width; k++) {
                u32 location = addr + k * 4, index = (location - 0x80) >> 4;
                if (location == 0x2f8 || location == 0x2fc) {
                    u32 instance;
                    if (location == 0x2f8) {
                        instance = spv_emit_load(&c->emit, c->u32_type, instance_index);
                        u32 base = spv_emit_load(&c->emit, c->u32_type, base_instance);
                        instance = spv_emit_isub_u32(&c->emit, c->u32_type, instance, base);
                    } else
                        instance = spv_emit_load(&c->emit, c->u32_type, vertex_index);
                    reg_write(c, d + k, instance);
                } else {
                    u32 vector = spv_emit_load(&c->emit, input_vec_type[index], inputs[index]);
                    u32 value =
                        spv_emit_vec4_extract(&c->emit, input_component_type[index], vector, (location >> 2) & 3);
                    reg_write(c, d + k,
                              input_component_type[index] == c->u32_type
                                  ? value
                                  : spv_emit_bitcast(&c->emit, c->u32_type, value));
                }
            }
        } else if (op == O_AST && (stage == ST_TCS || B(w, 31, 1))) {
            /* a control program stores its invocation's output vertex, and the patch's levels and vectors */
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1, k;
            u32 vertex = B(w, 31, 1) ? 0 : spv_emit_load(&c->emit, c->u32_type, invocation_id);
            for (k = 0; k < width; k++) {
                u32 location = addr + k * 4, pointer = 0;
                u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (location >> 2) & 3);
                if (!B(w, 31, 1))
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, 3, c->f32_type, location < 0x80 ? position : output_attrs[(location - 0x80) >> 4],
                        vertex, component);
                else if (location >= 0x20)
                    pointer = spv_emit_interface_scalar_pointer(&c->emit, 3, c->f32_type,
                                                                patch_vars[(location - 0x20) >> 4], 0, component);
                else if (location < 0x18)
                    pointer = spv_emit_interface_scalar_pointer(
                        &c->emit, 3, c->f32_type, location < 0x10 ? tess_outer : tess_inner, 0,
                        spv_emit_constant_u32(&c->emit, c->u32_type, (location & 0xf) / 4));
                if (pointer)
                    spv_emit_store(&c->emit, pointer, spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, d + k)));
            }
        } else if (op == O_AST) {
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1;
            for (u32 k = 0; k < width; k++) {
                u32 location = addr + k * 4;
                if (location == 0x6c) {
                    if (point_size)
                        spv_emit_store(&c->emit, point_size,
                                       spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, d + k)));
                    continue;
                }
                if (location == 0x64) {
                    if (layer_output) /* none: the device can't take this stage's layer */
                        spv_emit_store(&c->emit, layer_output,
                                       spv_emit_bitcast(&c->emit, i32_type, reg_read(c, d + k)));
                    continue;
                }
                if (location >= 0x2c0) {
                    u32 index = (location - 0x2c0) / 4;
                    if (clip_used & (1u << index)) {
                        u32 ptr = spv_emit_output_element_pointer(&c->emit, c->f32_type, clip_output,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, index));
                        spv_emit_store(&c->emit, ptr, spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, d + k)));
                    }
                    continue;
                }
                u32 variable = location < 0x80 ? position : output_attrs[(location - 0x80) >> 4];
                u32 vector = spv_emit_load(&c->emit, vec4, variable);
                u32 value = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, d + k));
                vector = spv_emit_vec4_insert(&c->emit, vec4, vector, value, (location >> 2) & 3);
                spv_emit_store(&c->emit, variable, vector);
            }
        } else if (op == O_OUT_R || op == O_OUT_I) {
            /* the vertex is complete: the depth mode applies to it, as at the end of the other stages */
            if (B(w, 39, 1)) {
                if (depth_fix) vertex_depth_fix(c, position, push, push_pointer, vec4);
                spv_emit_emit_vertex(&c->emit);
                /* outputs are undefined after it: the next vertex gets the default point size, unless the program
                   writes its own */
                if (point_size && !point_size_used)
                    spv_emit_store(&c->emit, point_size, spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f));
            }
            if (B(w, 40, 1)) spv_emit_end_primitive(&c->emit);
            reg_write(c, d, c->zero); /* the output handle, which the host doesn't have */
        } else if (op == O_ISBERD) {
            reg_write(c, d, reg_read(c, a));
        } else if (op == O_MOV32I) {
            u32 value = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
            reg_write(c, d, value);
        } else if (op == O_IMUL_R || op == O_IMUL_C || op == O_IMUL_I || op == O_IMUL32I || op == O_IMAD_R ||
                   op == O_IMAD_RC || op == O_IMAD_CR || op == O_IMAD_I || op == O_IMAD32I) {
            int immediate32 = op == O_IMUL32I || op == O_IMAD32I;
            int mad = op >= O_IMAD_R && op <= O_IMAD32I;
            u32 lhs = reg_read(c, a);
            u32 rhs = immediate32       ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32))
                      : op == O_IMAD_RC ? reg_read(c, B(w, 39, 8))
                      : op == O_IMAD_CR ? cbuf_word(c, w)
                                        : integer_right(c, w,
                                                        op == O_IMUL_R || op == O_IMAD_R ? 0
                                                        : op == O_IMUL_C                 ? 1
                                                                                         : 2);
            u32 addend = c->zero;
            int high;
            int signed_lhs;
            int signed_rhs;
            u32 product;
            if (mad) {
                u32 mode;
                addend = op == O_IMAD32I   ? reg_read(c, d)
                         : op == O_IMAD_RC ? cbuf_word(c, w)
                                           : reg_read(c, B(w, 39, 8));
                mode = B(w, immediate32 ? 55 : 51, 2);
                if (mode == 2) rhs = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, rhs);
                if (mode == 1) addend = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, addend);
                if (mode == 3)
                    addend = spv_emit_iadd_u32(&c->emit, c->u32_type, addend,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                if (!immediate32 && B(w, 49, 1))
                    addend = spv_emit_iadd_u32(
                        &c->emit, c->u32_type, addend,
                        spv_emit_select(&c->emit, c->u32_type, spv_emit_load(&c->emit, c->bool_type, c->cc_c),
                                        spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
            }
            high = B(w, immediate32 ? 53 : mad ? 54 : 39, 1);
            signed_lhs = B(w, immediate32 ? 54 : mad ? 48 : 40, 1);
            signed_rhs = B(w, immediate32 ? mad ? 57 : 55 : mad ? 53 : 41, 1);
            product = high ? integer_mul_high(c, lhs, rhs, signed_lhs, signed_rhs)
                           : spv_emit_imul_u32(&c->emit, c->u32_type, lhs, rhs);
            reg_write(c, d, mad ? spv_emit_iadd_u32(&c->emit, c->u32_type, product, addend) : product);
        } else if (op == O_VOTE) {
            u32 value = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 lane = spv_emit_load(&c->emit, c->u32_type, c->subgroup_lane_input);
            u32 mask = subgroup_ballot_word(c, vec4_u32, lane, value);
            u32 mode = B(w, 48, 2);
            if (mode < 3) {
                u32 any = spv_emit_i_not_equal(&c->emit, c->bool_type, mask, c->zero);
                u32 result = any;
                if (mode != 1) {
                    u32 active = subgroup_ballot_word(c, vec4_u32, lane, c->true_value);
                    u32 all = spv_emit_i_equal(&c->emit, c->bool_type, mask, active);
                    result = mode == 0 ? all
                                       : spv_emit_bool_binary(&c->emit, c->bool_type, 1, all,
                                                              spv_emit_bool_not(&c->emit, c->bool_type, any));
                }
                pred_write(c, B(w, 45, 3), result);
            }
            reg_write(c, d, mask);
        } else if (op == O_FLO_R || op == O_FLO_C || op == O_FLO_I) {
            u32 value = integer_right(c, w, op - O_FLO_R);
            if (B(w, 40, 1)) value = spv_emit_not_u32(&c->emit, c->u32_type, value);
            if (B(w, 48, 1)) {
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, value));
                value = spv_emit_select(&c->emit, c->u32_type, negative, spv_emit_not_u32(&c->emit, c->u32_type, value),
                                        value);
            }
            u32 spread = value;
            for (u32 shift = 1; shift <= 16; shift <<= 1)
                spread = spv_emit_bitwise_or_u32(
                    &c->emit, c->u32_type, spread,
                    spv_emit_shift_right_u32(&c->emit, c->u32_type, spread,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, shift)));
            u32 index = spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_bit_count(&c->emit, c->u32_type, spread),
                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1));
            if (B(w, 41, 1)) {
                u32 found = spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
                index = spv_emit_select(
                    &c->emit, c->u32_type, found,
                    spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 31), index),
                    index);
            }
            reg_write(c, d, index);
        } else if (op == O_POPC_R || op == O_POPC_C || op == O_POPC_I) {
            u32 value = integer_right(c, w, op - O_POPC_R);
            if (B(w, 40, 1)) value = spv_emit_not_u32(&c->emit, c->u32_type, value);
            reg_write(c, d, spv_emit_bit_count(&c->emit, c->u32_type, value));
        } else if (op == O_SHFL) {
            u32 index = B(w, 28, 1) ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 5)) : reg_read(c, b);
            u32 mask =
                B(w, 29, 1) ? spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 34, 13)) : reg_read(c, B(w, 39, 8));
            u32 lane_full = spv_emit_load(&c->emit, c->u32_type, c->subgroup_lane_input);
            u32 lane = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, lane_full,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 seg = spv_emit_bitwise_and_u32(
                &c->emit, c->u32_type,
                spv_emit_shift_right_u32(&c->emit, c->u32_type, mask, spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 min_lane = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, lane, seg);
            u32 max_lane = spv_emit_bitwise_or_u32(
                &c->emit, c->u32_type, min_lane,
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mask,
                                         spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                                                  spv_emit_not_u32(&c->emit, c->u32_type, seg),
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 31))));
            u32 mode = B(w, 30, 2), source_lane, valid;
            if (mode == 0) {
                source_lane =
                    spv_emit_bitwise_or_u32(&c->emit, c->u32_type, min_lane,
                                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, index,
                                                                     spv_emit_not_u32(&c->emit, c->u32_type, seg)));
            } else if (mode == 1) {
                source_lane = spv_emit_isub_u32(&c->emit, c->u32_type, lane, index);
            } else if (mode == 2) {
                source_lane = spv_emit_iadd_u32(&c->emit, c->u32_type, lane, index);
            } else {
                source_lane = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, lane, index);
            }
            valid = mode == 1 ? spv_emit_bool_not(
                                    &c->emit, c->bool_type,
                                    spv_emit_u_less(&c->emit, c->bool_type,
                                                    spv_emit_isub_u32(&c->emit, c->u32_type, lane, min_lane), index))
                              : spv_emit_bool_not(&c->emit, c->bool_type,
                                                  spv_emit_u_less(&c->emit, c->bool_type, max_lane, source_lane));
            u32 host_lane =
                spv_emit_bitwise_or_u32(&c->emit, c->u32_type,
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, lane_full,
                                                                 spv_emit_constant_u32(&c->emit, c->u32_type, ~31u)),
                                        spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source_lane,
                                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 31)));
            u32 value = reg_read(c, a);
            u32 shuffled = spv_emit_subgroup_shuffle_u32(&c->emit, c->u32_type, value, host_lane);
            pred_write(c, B(w, 48, 3), valid);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, valid, shuffled, value));
        } else if (op == O_MOV_R || op == O_MOV_C || op == O_MOV_I) {
            if (B(w, 39, 4) == 1 || B(w, 39, 4) == 15) reg_write(c, d, integer_right(c, w, op - O_MOV_R));
        } else if (op == O_IADD32I) {
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
            reg_write(c, d, integer_add(c, reg_read(c, a), immediate, B(w, 55, 2), B(w, 53, 1), B(w, 52, 1)));
        } else if (op == O_IADD3_R || op == O_IADD3_C || op == O_IADD3_I) {
            u32 first = reg_read(c, a);
            u32 second = integer_right(c, w, op - O_IADD3_R);
            u32 third = reg_read(c, B(w, 39, 8));
            if (op == O_IADD3_R) {
                u32 values[3] = {first, second, third};
                for (u32 k = 0; k < 3; k++) {
                    u32 half = B(w, 35 - 2 * k, 2);
                    if (half == 1)
                        values[k] = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, values[k],
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff));
                    if (half == 2)
                        values[k] = spv_emit_shift_right_u32(&c->emit, c->u32_type, values[k],
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 16));
                }
                first = values[0];
                second = values[1];
                third = values[2];
            }
            if (B(w, 51, 1)) first = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, first);
            if (B(w, 50, 1)) second = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, second);
            if (B(w, 49, 1)) third = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, third);
            u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, first, second);
            u32 carry0 = spv_emit_u_less(&c->emit, c->bool_type, sum, first);
            u32 shift = op == O_IADD3_R ? B(w, 37, 2) : 0;
            if (shift == 1) /* the 33-bit sum shifted: its carry becomes bit 16 */
                sum = spv_emit_bitwise_or_u32(
                    &c->emit, c->u32_type,
                    spv_emit_shift_right_u32(&c->emit, c->u32_type, sum,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 16)),
                    spv_emit_select(&c->emit, c->u32_type, carry0,
                                    spv_emit_constant_u32(&c->emit, c->u32_type, 0x10000), c->zero));
            if (shift == 2)
                sum = spv_emit_shift_left_u32(&c->emit, c->u32_type, sum,
                                              spv_emit_constant_u32(&c->emit, c->u32_type, 16));
            u32 sum2 = spv_emit_iadd_u32(&c->emit, c->u32_type, sum, third);
            u32 carry1 = spv_emit_u_less(&c->emit, c->bool_type, sum2, sum);
            u32 carry2 = c->false_value;
            if (B(w, 48, 1)) {
                u32 cin = spv_emit_select(&c->emit, c->u32_type, spv_emit_load(&c->emit, c->bool_type, c->cc_c),
                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero);
                u32 final = spv_emit_iadd_u32(&c->emit, c->u32_type, sum2, cin);
                carry2 = spv_emit_u_less(&c->emit, c->bool_type, final, sum2);
                sum2 = final;
            }
            if (B(w, 47, 1)) {
                u32 carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry1, carry2);
                if (shift != 1 && shift != 2) carry = spv_emit_bool_binary(&c->emit, c->bool_type, 1, carry, carry0);
                u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, sum2, c->zero);
                if (B(w, 48, 1))
                    zero = spv_emit_bool_binary(&c->emit, c->bool_type, 0, zero,
                                                spv_emit_load(&c->emit, c->bool_type, c->cc_z));
                u32 sign = spv_emit_i_not_equal(
                    &c->emit, c->bool_type,
                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum2,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0x80000000)),
                    c->zero);
                spv_emit_store(&c->emit, c->cc_c, carry);
                spv_emit_store(&c->emit, c->cc_z, zero);
                spv_emit_store(&c->emit, c->cc_s, sign);
            }
            reg_write(c, d, sum2);
        } else if (op == O_IADD_R || op == O_IADD_C || op == O_IADD_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_IADD_R);
            reg_write(c, d, integer_add(c, left, right, B(w, 48, 2), B(w, 43, 1), B(w, 47, 1)));
        } else if (op == O_LOP32I) {
            u32 left = reg_read(c, a);
            u32 right = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
            if (B(w, 55, 1)) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
            if (B(w, 56, 1)) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
            u32 value;
            switch (B(w, 53, 2)) {
                case 0: value = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, left, right); break;
                case 1: value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right); break;
                case 2: value = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right); break;
                default: value = right; break;
            }
            reg_write(c, d, value);
        } else if (op == O_LOP_R || op == O_LOP_C || op == O_LOP_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_LOP_R);
            if (B(w, 39, 1)) left = spv_emit_not_u32(&c->emit, c->u32_type, left);
            if (B(w, 40, 1)) right = spv_emit_not_u32(&c->emit, c->u32_type, right);
            u32 value;
            switch (B(w, 41, 2)) {
                case 0: value = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, left, right); break;
                case 1: value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right); break;
                case 2: value = spv_emit_bitwise_xor_u32(&c->emit, c->u32_type, left, right); break;
                default: value = right; break;
            }
            reg_write(c, d, value);
            if (B(w, 48, 3) < 7) {
                u32 mode = B(w, 44, 2);
                u32 predicate = mode == 0   ? c->false_value
                                : mode == 1 ? c->true_value
                                : mode == 2 ? spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero)
                                            : spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
                pred_write(c, B(w, 48, 3), predicate);
            }
        } else if (op == O_LOP3_R || op == O_LOP3_C || op == O_LOP3_I) {
            u32 first = reg_read(c, a), second = integer_right(c, w, op - O_LOP3_R);
            u32 third = reg_read(c, B(w, 39, 8));
            u32 lut = op == O_LOP3_R ? B(w, 28, 8) : B(w, 48, 8);
            u32 value = c->zero;
            for (u32 bit = 0; bit < 8; bit++) {
                if (!(lut & (1u << bit))) continue;
                u32 x = bit & 4 ? first : spv_emit_not_u32(&c->emit, c->u32_type, first);
                u32 y = bit & 2 ? second : spv_emit_not_u32(&c->emit, c->u32_type, second);
                u32 z = bit & 1 ? third : spv_emit_not_u32(&c->emit, c->u32_type, third);
                u32 term = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, x, y);
                term = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, term, z);
                value = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, value, term);
            }
            reg_write(c, d, value);
            if (op == O_LOP3_R && B(w, 48, 3) < 7) {
                u32 mode = B(w, 36, 2);
                u32 predicate = mode == 0   ? c->false_value
                                : mode == 1 ? c->true_value
                                : mode == 2 ? spv_emit_i_equal(&c->emit, c->bool_type, value, c->zero)
                                            : spv_emit_i_not_equal(&c->emit, c->bool_type, value, c->zero);
                pred_write(c, B(w, 48, 3), predicate);
            }
        } else if (op == O_SEL_R || op == O_SEL_C || op == O_SEL_I) {
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 rhs = integer_right(c, w, op - O_SEL_R);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, condition, reg_read(c, a), rhs));
        } else if (op == O_XMAD_RC) {
            u32 half = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
            u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff);
            u32 lhs = B(w, 53, 1) ? spv_emit_shift_right_u32(&c->emit, c->u32_type, reg_read(c, a), half)
                                  : spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a), mask);
            u32 source = reg_read(c, B(w, 39, 8));
            u32 rhs = B(w, 52, 1) ? spv_emit_shift_right_u32(&c->emit, c->u32_type, source, half)
                                  : spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source, mask);
            u32 third = cbuf_word(c, w);
            if (B(w, 50, 2) == 1)
                third = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, third, mask);
            else if (B(w, 50, 2) == 2)
                third = spv_emit_shift_right_u32(&c->emit, c->u32_type, third, half);
            reg_write(
                c, d,
                spv_emit_iadd_u32(&c->emit, c->u32_type, spv_emit_imul_u32(&c->emit, c->u32_type, lhs, rhs), third));
        } else if (op == O_XMAD_CR) {
            u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff);
            u32 sixteen = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
            /* the constant buffer form keeps a 2-bit select at 50 (no CBCC), B's half at 52, PSL at 55, MRG at 56 */
            u32 lhs = xmad_half(c, i32_type, reg_read(c, a), B(w, 53, 1), B(w, 48, 1));
            u32 source = cbuf_word(c, w);
            u32 rhs = xmad_half(c, i32_type, source, B(w, 52, 1), B(w, 49, 1));
            u32 product = spv_emit_imul_u32(&c->emit, c->u32_type, lhs, rhs);
            if (B(w, 55, 1)) product = spv_emit_shift_left_u32(&c->emit, c->u32_type, product, sixteen);
            u32 third = reg_read(c, B(w, 39, 8));
            if (B(w, 50, 2) == 1)
                third = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, third, mask);
            else if (B(w, 50, 2) == 2)
                third = spv_emit_shift_right_u32(&c->emit, c->u32_type, third, sixteen);
            u32 value = spv_emit_iadd_u32(&c->emit, c->u32_type, product, third);
            if (B(w, 56, 1))
                value = spv_emit_bitwise_or_u32(
                    &c->emit, c->u32_type, spv_emit_bitwise_and_u32(&c->emit, c->u32_type, value, mask),
                    spv_emit_shift_left_u32(&c->emit, c->u32_type,
                                            spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source, mask), sixteen));
            reg_write(c, d, value);
        } else if (op == O_XMAD_R) {
            u32 source_b = reg_read(c, b);
            u32 left = xmad_half(c, i32_type, reg_read(c, a), B(w, 53, 1), B(w, 48, 1));
            u32 right = xmad_half(c, i32_type, source_b, B(w, 35, 1), B(w, 49, 1));
            u32 sixteen = spv_emit_constant_u32(&c->emit, c->u32_type, 16);
            u32 product = spv_emit_imul_u32(&c->emit, c->u32_type, left, right);
            if (B(w, 36, 1)) product = spv_emit_shift_left_u32(&c->emit, c->u32_type, product, sixteen);
            u32 third = reg_read(c, B(w, 39, 8));
            if (B(w, 50, 3) == 1)
                third = xmad_half(c, i32_type, third, 0, 0);
            else if (B(w, 50, 3) == 2)
                third = xmad_half(c, i32_type, third, 1, 0);
            else if (B(w, 50, 3) == 4)
                third = spv_emit_iadd_u32(&c->emit, c->u32_type,
                                          spv_emit_shift_left_u32(&c->emit, c->u32_type, source_b, sixteen), third);
            u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, product, third);
            if (B(w, 37, 1)) {
                u32 low_mask = spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff);
                u32 high = spv_emit_shift_left_u32(&c->emit, c->u32_type,
                                                   spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source_b, low_mask),
                                                   sixteen);
                sum = spv_emit_bitwise_or_u32(&c->emit, c->u32_type,
                                              spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum, low_mask), high);
            }
            reg_write(c, d, sum);
        } else if (op == O_XMAD_I) {
            u32 half = B(w, 53, 1) ? spv_emit_shift_right_u32(&c->emit, c->u32_type, reg_read(c, a),
                                                              spv_emit_constant_u32(&c->emit, c->u32_type, 16))
                                   : spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a),
                                                              spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff));
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16));
            u32 product = spv_emit_imul_u32(&c->emit, c->u32_type, half, immediate);
            if (B(w, 36, 1))
                product = spv_emit_shift_left_u32(&c->emit, c->u32_type, product,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 16));
            u32 third = reg_read(c, B(w, 39, 8));
            if (B(w, 50, 3) == 1)
                third = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, third,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff));
            else if (B(w, 50, 3) == 2)
                third = spv_emit_shift_right_u32(&c->emit, c->u32_type, third,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 16));
            else if (B(w, 50, 3) == 4)
                third = spv_emit_iadd_u32(&c->emit, c->u32_type,
                                          spv_emit_shift_left_u32(&c->emit, c->u32_type, immediate,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 16)),
                                          third);
            u32 sum = spv_emit_iadd_u32(&c->emit, c->u32_type, product, third);
            if (B(w, 37, 1))
                sum = spv_emit_bitwise_or_u32(
                    &c->emit, c->u32_type,
                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, sum,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff)),
                    spv_emit_shift_left_u32(&c->emit, c->u32_type, immediate,
                                            spv_emit_constant_u32(&c->emit, c->u32_type, 16)));
            reg_write(c, d, sum);
        } else if (op == O_LDC) {
            u32 off = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)(i32)(i16)B(w, 20, 16));
            u32 address = a == 255 ? off : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), off);
            u32 bank = B(w, 36, 5);
            u32 value = cbuf_read_dynamic(c, bank, address);
            u32 size = B(w, 48, 3);
            /* the element an indexed texture whose handle this loads picks: the address register over the stride */
            for (k_indexed = 0; k_indexed < texture_count; k_indexed++)
                if ((textures[k_indexed].flags & SNX_TEX_INDEXED) && textures[k_indexed].bl2_off == i + 1)
                    spv_emit_store(&c->emit, c->tex_index[k_indexed],
                                   spv_emit_shift_right_u32(
                                       &c->emit, c->u32_type, reg_read(c, a),
                                       spv_emit_constant_u32(&c->emit, c->u32_type, textures[k_indexed].flags >> 4)));
            if (size < 4) {
                u32 byte_offset = spv_emit_bitwise_and_u32(
                    &c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 3 : 2));
                u32 bit_offset = spv_emit_shift_left_u32(&c->emit, c->u32_type, byte_offset,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 8 : 16);
                value = size & 1
                            ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, value),
                                                                          bit_offset, width))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, bit_offset, width);
            }
            reg_write(c, d, value);
            if (size == 5) {
                u32 next =
                    spv_emit_iadd_u32(&c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                reg_write(c, d + 1, cbuf_read_dynamic(c, bank, next));
            }
        } else if (op == O_ATOMS) {
            i32 relative = (i32)(B(w, 30, 22) << 10) >> 10;
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)(relative * 4));
            u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
            u32 index = spv_emit_shift_right_u32(&c->emit, c->u32_type, address,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2));
            index = shared_index(c, index, shared_words);
            u32 pointer = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, index);
            u32 operation = shared_atomic_opcode(w);
            if (operation == 0xfffffffeu) {
                u32 next_index = shared_index(
                    c, spv_emit_iadd_u32(&c->emit, c->u32_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, 1)),
                    shared_words);
                u32 high =
                    spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, next_index);
                u32 old_low = spv_emit_load(&c->emit, c->u32_type, pointer);
                u32 old_high = spv_emit_load(&c->emit, c->u32_type, high);
                spv_emit_store(&c->emit, pointer, reg_read(c, b));
                spv_emit_store(&c->emit, high, reg_read(c, b + 1));
                reg_write(c, d, old_low);
                reg_write(c, d + 1, old_high);
            } else if (operation != 0xffffffffu) {
                reg_write(c, d, spv_emit_atomic_shared_u32(&c->emit, c->u32_type, operation, pointer, reg_read(c, b)));
            } else {
                u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                u32 cont = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
                spv_emit_store(&c->emit, atomic_old_var,
                               spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer));
                spv_emit_branch(&c->emit, header);
                spv_emit_label(&c->emit, header);
                spv_emit_loop_merge(&c->emit, merge, cont);
                spv_emit_branch(&c->emit, body);
                spv_emit_label(&c->emit, body);
                u32 old = spv_emit_load(&c->emit, c->u32_type, atomic_old_var);
                u32 value = reg_read(c, b);
                u32 next;
                if (B(w, 52, 4) == 3) {
                    u32 increment =
                        spv_emit_iadd_u32(&c->emit, c->u32_type, old, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                    next = spv_emit_select(&c->emit, c->u32_type,
                                           cc_not(c, spv_emit_u_less(&c->emit, c->bool_type, old, value)), c->zero,
                                           increment);
                } else {
                    u32 decrement =
                        spv_emit_isub_u32(&c->emit, c->u32_type, old, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                    u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, old, c->zero);
                    u32 greater = spv_emit_u_less(&c->emit, c->bool_type, value, old);
                    next = spv_emit_select(&c->emit, c->u32_type, cc_or(c, zero, greater), value, decrement);
                }
                u32 observed = spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, next, old);
                spv_emit_store(&c->emit, atomic_old_var, observed);
                spv_emit_branch_conditional(&c->emit, spv_emit_i_equal(&c->emit, c->bool_type, observed, old), merge,
                                            cont);
                spv_emit_label(&c->emit, cont);
                spv_emit_branch(&c->emit, header);
                spv_emit_label(&c->emit, merge);
                reg_write(c, d, spv_emit_load(&c->emit, c->u32_type, atomic_old_var));
            }
        } else if (op == O_LDS || op == O_STS) {
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
            u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
            u32 first = spv_emit_shift_right_u32(&c->emit, c->u32_type, address,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2));
            u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
            u32 size = B(w, 48, 3);
            for (u32 k = 0; k < words; k++) {
                u32 index =
                    k ? spv_emit_iadd_u32(&c->emit, c->u32_type, first, spv_emit_constant_u32(&c->emit, c->u32_type, k))
                      : first;
                index = shared_index(c, index, shared_words);
                u32 pointer = spv_emit_function_element_pointer(&c->emit, shared_element_pointer, shared_array, index);
                if (size < 4) {
                    u32 bit_offset = spv_emit_bitwise_and_u32(
                        &c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 3 : 2));
                    bit_offset = spv_emit_shift_left_u32(&c->emit, c->u32_type, bit_offset,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 8 : 16);
                    if (op == O_STS) {
                        u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                        u32 retry = spv_emit_id(&c->emit), done = spv_emit_id(&c->emit);
                        spv_emit_branch(&c->emit, header);
                        spv_emit_label(&c->emit, header);
                        spv_emit_loop_merge(&c->emit, done, retry);
                        spv_emit_branch(&c->emit, body);
                        spv_emit_label(&c->emit, body);
                        u32 old = spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer);
                        u32 desired =
                            spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width);
                        u32 actual =
                            spv_emit_atomic_compare_exchange_shared_u32(&c->emit, c->u32_type, pointer, desired, old);
                        u32 success = spv_emit_i_equal(&c->emit, c->bool_type, actual, old);
                        spv_emit_branch_conditional(&c->emit, success, done, retry);
                        spv_emit_label(&c->emit, retry);
                        spv_emit_branch(&c->emit, header);
                        spv_emit_label(&c->emit, done);
                    } else {
                        u32 old = spv_emit_atomic_load_shared_u32(&c->emit, c->u32_type, pointer);
                        u32 value =
                            size & 1
                                ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                   spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                              spv_emit_bitcast(&c->emit, i32_type, old),
                                                                              bit_offset, width))
                                : spv_emit_bitfield_uextract(&c->emit, c->u32_type, old, bit_offset, width);
                        reg_write(c, d, value);
                    }
                } else if (op == O_STS)
                    spv_emit_store(&c->emit, pointer, reg_read(c, d + k));
                else
                    reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, pointer));
            }
        } else if (op == O_BAR) {
            if (stage == ST_TCS)
                spv_emit_patch_barrier(&c->emit, c->u32_type);
            else
                spv_emit_control_barrier(&c->emit, c->u32_type);
        } else if (op == O_MEMBAR) {
            /* a memory fence only, never execution: CTA orders workgroup memory, the rest device memory */
            spv_emit_memory_barrier(&c->emit, c->u32_type, B(w, 8, 2) == 0 ? 2 : 1, 0xD48);
        } else if (op == O_LDL || op == O_STL) {
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
            u32 address = a == 255 ? immediate : spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
            u32 first = spv_emit_shift_right_u32(&c->emit, c->u32_type, address,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2));
            u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
            u32 size = B(w, 48, 3);
            for (u32 k = 0; k < words; k++) {
                u32 index =
                    k ? spv_emit_iadd_u32(&c->emit, c->u32_type, first, spv_emit_constant_u32(&c->emit, c->u32_type, k))
                      : first;
                index = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, index,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, local_words - 1));
                u32 pointer = spv_emit_function_element_pointer(&c->emit, local_element_pointer, local_array, index);
                if (size < 4) {
                    u32 bit_offset = spv_emit_bitwise_and_u32(
                        &c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 3 : 2));
                    bit_offset = spv_emit_shift_left_u32(&c->emit, c->u32_type, bit_offset,
                                                         spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 8 : 16);
                    u32 old = spv_emit_load(&c->emit, c->u32_type, pointer);
                    if (op == O_STL)
                        spv_emit_store(
                            &c->emit, pointer,
                            spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width));
                    else {
                        u32 value =
                            size & 1
                                ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                   spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                              spv_emit_bitcast(&c->emit, i32_type, old),
                                                                              bit_offset, width))
                                : spv_emit_bitfield_uextract(&c->emit, c->u32_type, old, bit_offset, width);
                        reg_write(c, d, value);
                    }
                } else if (op == O_STL)
                    spv_emit_store(&c->emit, pointer, reg_read(c, d + k));
                else
                    reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, pointer));
            }
        } else if (op == O_LDG || op == O_STG) {
            u32 slot = gb_at[i] - 1;
            u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, gbuf_off[slot]);
            u32 base = cbuf_read_dynamic(c, 0, offset);
            u32 immediate = spv_emit_constant_u32(&c->emit, c->u32_type, (u32)((i32)(B(w, 20, 24) << 8) >> 8));
            u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a), immediate);
            address = spv_emit_isub_u32(&c->emit, c->u32_type, address, base);
            u32 index = spv_emit_shift_right_u32(&c->emit, c->u32_type, address,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2));
            u32 words = B(w, 48, 3) == 5 ? 2 : B(w, 48, 3) >= 6 ? 4 : 1;
            u32 size = B(w, 48, 3);
            for (u32 k = 0; k < words; k++) {
                u32 word =
                    k ? spv_emit_iadd_u32(&c->emit, c->u32_type, index, spv_emit_constant_u32(&c->emit, c->u32_type, k))
                      : index;
                word = gbuf_word_index(c, stage, slot, word);
                u32 ptr = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero, word);
                if (size < 4) {
                    u32 byte_offset = spv_emit_bitwise_and_u32(
                        &c->emit, c->u32_type, address, spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 3 : 2));
                    u32 bit_offset = spv_emit_shift_left_u32(&c->emit, c->u32_type, byte_offset,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 3));
                    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size < 2 ? 8 : 16);
                    if (op == O_STG) {
                        u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                        u32 retry = spv_emit_id(&c->emit), done = spv_emit_id(&c->emit);
                        spv_emit_branch(&c->emit, header);
                        spv_emit_label(&c->emit, header);
                        spv_emit_loop_merge(&c->emit, done, retry);
                        spv_emit_branch(&c->emit, body);
                        spv_emit_label(&c->emit, body);
                        u32 old = spv_emit_atomic_load_u32(&c->emit, c->u32_type, ptr);
                        u32 desired =
                            spv_emit_bitfield_insert(&c->emit, c->u32_type, old, reg_read(c, d), bit_offset, width);
                        u32 actual = spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, ptr, desired, old);
                        u32 success = spv_emit_i_equal(&c->emit, c->bool_type, actual, old);
                        spv_emit_branch_conditional(&c->emit, success, done, retry);
                        spv_emit_label(&c->emit, retry);
                        spv_emit_branch(&c->emit, header);
                        spv_emit_label(&c->emit, done);
                    } else {
                        u32 old = spv_emit_atomic_load_u32(&c->emit, c->u32_type, ptr);
                        u32 value =
                            size & 1
                                ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                   spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                              spv_emit_bitcast(&c->emit, i32_type, old),
                                                                              bit_offset, width))
                                : spv_emit_bitfield_uextract(&c->emit, c->u32_type, old, bit_offset, width);
                        reg_write(c, d, value);
                    }
                } else if (op == O_STG)
                    spv_emit_store(&c->emit, ptr, reg_read(c, d + k));
                else
                    reg_write(c, d + k, spv_emit_load(&c->emit, c->u32_type, ptr));
            }
        } else if (op == O_RED || op == O_ATOM) {
            u32 slot = gb_at[i] - 1;
            u32 base = cbuf_read_dynamic(c, 0, spv_emit_constant_u32(&c->emit, c->u32_type, gbuf_off[slot]));
            /* a signed offset from a register, an absolute address from RZ, for ATOM and RED alike */
            u32 immediate = a == 255 ? B(w, 28, 20) : (u32)((i32)(B(w, 28, 20) << 12) >> 12);
            u32 address = spv_emit_iadd_u32(&c->emit, c->u32_type, reg_read(c, a),
                                            spv_emit_constant_u32(&c->emit, c->u32_type, immediate));
            address = spv_emit_isub_u32(&c->emit, c->u32_type, address, base);
            u32 index = spv_emit_shift_right_u32(&c->emit, c->u32_type, address,
                                                 spv_emit_constant_u32(&c->emit, c->u32_type, 2));
            index = gbuf_word_index(c, stage, slot, index);
            u32 pointer = spv_emit_storage_access_u32(&c->emit, c->gbuf_pointer, c->gbuf[slot], c->zero, index);
            u32 value = reg_read(c, op == O_RED ? d : b);
            u32 atomic_opcode = global_atomic_opcode(w, op == O_RED);
            u32 prior;
            if (atomic_opcode != 0xffffffffu)
                prior = spv_emit_atomic_u32(&c->emit, c->u32_type, atomic_opcode, pointer, value);
            else {
                u32 operation = op == O_RED ? B(w, 23, 3) : B(w, 52, 4);
                u32 size = op == O_RED ? B(w, 20, 3) : B(w, 49, 3);
                u32 header = spv_emit_id(&c->emit), body = spv_emit_id(&c->emit);
                u32 cont = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
                spv_emit_store(&c->emit, atomic_old_var, spv_emit_load(&c->emit, c->u32_type, pointer));
                spv_emit_branch(&c->emit, header);
                spv_emit_label(&c->emit, header);
                spv_emit_loop_merge(&c->emit, merge, cont);
                spv_emit_branch(&c->emit, body);
                spv_emit_label(&c->emit, body);
                u32 old = spv_emit_load(&c->emit, c->u32_type, atomic_old_var), desired;
                if (size == 3) {
                    u32 sum = spv_emit_fadd(&c->emit, c->f32_type, spv_emit_bitcast(&c->emit, c->f32_type, old),
                                            spv_emit_bitcast(&c->emit, c->f32_type, value));
                    desired = spv_emit_bitcast(&c->emit, c->u32_type, sum);
                } else if (operation == 3) {
                    u32 increment =
                        spv_emit_iadd_u32(&c->emit, c->u32_type, old, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                    u32 greater_equal = cc_not(c, spv_emit_u_less(&c->emit, c->bool_type, old, value));
                    desired = spv_emit_select(&c->emit, c->u32_type, greater_equal, c->zero, increment);
                } else {
                    u32 decrement =
                        spv_emit_isub_u32(&c->emit, c->u32_type, old, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
                    u32 zero = spv_emit_i_equal(&c->emit, c->bool_type, old, c->zero);
                    u32 greater = spv_emit_u_less(&c->emit, c->bool_type, value, old);
                    desired = spv_emit_select(&c->emit, c->u32_type, cc_or(c, zero, greater), value, decrement);
                }
                u32 observed = spv_emit_atomic_compare_exchange_u32(&c->emit, c->u32_type, pointer, desired, old);
                spv_emit_store(&c->emit, atomic_old_var, observed);
                u32 done = spv_emit_i_equal(&c->emit, c->bool_type, observed, old);
                spv_emit_branch_conditional(&c->emit, done, merge, cont);
                spv_emit_label(&c->emit, cont);
                spv_emit_branch(&c->emit, header);
                spv_emit_label(&c->emit, merge);
                prior = spv_emit_load(&c->emit, c->u32_type, atomic_old_var);
            }
            if (op == O_ATOM) reg_write(c, d, prior);
        } else if (op == O_PSET) {
            u32 pa = pred_read(c, B(w, 12, 3), B(w, 15, 1));
            u32 pb = pred_read(c, B(w, 29, 3), B(w, 32, 1));
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 test = bool_combine(c, B(w, 45, 2), bool_combine(c, B(w, 24, 2), pa, pb), pc);
            u32 value = spv_emit_select(
                &c->emit, c->u32_type, test,
                spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu), c->zero);
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_PSETP) {
            u32 pa = pred_read(c, B(w, 12, 3), B(w, 15, 1));
            u32 pb = pred_read(c, B(w, 29, 3), B(w, 32, 1));
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 first = bool_combine(c, B(w, 24, 2), pa, pb);
            u32 second = bool_combine(c, B(w, 24, 2), spv_emit_bool_not(&c->emit, c->bool_type, pa), pb);
            pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), first, pc));
            pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), second, pc));
        } else if (op == O_CSETP || op == O_CSET) {
            u32 test = cc_test(c, B(w, 8, 5));
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 yes = bool_combine(c, B(w, 45, 2), test, condition);
            if (op == O_CSET) {
                u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu);
                u32 result = spv_emit_select(&c->emit, c->u32_type, yes, truth, c->zero);
                if (B(w, 47, 1)) flags_logic(c, result);
                reg_write(c, d, result);
            } else {
                pred_write(c, B(w, 3, 3), yes);
                pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), cc_not(c, test), condition));
            }
        } else if (op >= O_P2R_R && op <= O_P2R_I) {
            u32 mask = integer_right(c, w, op - O_P2R_R);
            if (op != O_P2R_I)
                mask = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mask,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 255));
            u32 bits = c->zero;
            u32 flags[4] = {c->cc_z, c->cc_s, c->cc_c, c->cc_o};
            for (u32 p = 0; p < (B(w, 40, 1) ? 4u : 7u); p++) {
                u32 value = B(w, 40, 1) ? spv_emit_load(&c->emit, c->bool_type, flags[p]) : pred_read(c, p, 0);
                u32 bit = spv_emit_select(&c->emit, c->u32_type, value,
                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p), c->zero);
                bits = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, bits, bit);
            }
            u32 shift = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 2) * 8);
            u32 shifted_mask = spv_emit_shift_left_u32(&c->emit, c->u32_type, mask, shift);
            u32 base = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reg_read(c, a),
                                                spv_emit_not_u32(&c->emit, c->u32_type, shifted_mask));
            u32 picked = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, bits, mask);
            u32 shifted = spv_emit_shift_left_u32(&c->emit, c->u32_type, picked, shift);
            reg_write(c, d, spv_emit_bitwise_or_u32(&c->emit, c->u32_type, base, shifted));
        } else if (op >= O_R2P_R && op <= O_R2P_I) {
            u32 source = reg_read(c, a);
            if (B(w, 41, 2))
                source = spv_emit_shift_right_u32(&c->emit, c->u32_type, source,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 2) * 8));
            u32 mask = integer_right(c, w, op - O_R2P_R);
            u32 flags[4] = {c->cc_z, c->cc_s, c->cc_c, c->cc_o};
            for (u32 p = 0; p < (B(w, 40, 1) ? 4u : 7u); p++) {
                u32 enabled = spv_emit_i_not_equal(
                    &c->emit, c->bool_type,
                    spv_emit_bitwise_and_u32(&c->emit, c->u32_type, mask,
                                             spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p)),
                    c->zero);
                u32 bit = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, source,
                                                   spv_emit_constant_u32(&c->emit, c->u32_type, 1u << p));
                u32 value = spv_emit_i_not_equal(&c->emit, c->bool_type, bit, c->zero);
                u32 ptr = B(w, 40, 1) ? flags[p] : c->pred[p];
                spv_emit_store(&c->emit, ptr,
                               spv_emit_select(&c->emit, c->bool_type, enabled, value,
                                               spv_emit_load(&c->emit, c->bool_type, ptr)));
            }
        } else if (op == O_ISETP_R || op == O_ISETP_C || op == O_ISETP_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_ISETP_R);
            u32 test = integer_compare(c, i32_type, w, left, right);
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 yes = bool_combine(c, B(w, 45, 2), test, condition);
            u32 no = bool_combine(c, B(w, 45, 2), spv_emit_bool_not(&c->emit, c->bool_type, test), condition);
            pred_write(c, B(w, 3, 3), yes);
            pred_write(c, B(w, 0, 3), no);
        } else if (op == O_ISET_R || op == O_ISET_C || op == O_ISET_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_ISET_R);
            u32 test = integer_compare(c, i32_type, w, left, right);
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            test = bool_combine(c, B(w, 45, 2), test, condition);
            u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 44, 1) ? 0x3f800000u : 0xffffffffu);
            u32 result = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
            if (B(w, 47, 1)) flags_logic(c, result);
            reg_write(c, d, result);
        } else if (op == O_IMNMX_R || op == O_IMNMX_C || op == O_IMNMX_I) {
            u32 left = reg_read(c, a), right = integer_right(c, w, op - O_IMNMX_R);
            u32 less = B(w, 48, 1)
                           ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, right),
                                                spv_emit_bitcast(&c->emit, i32_type, left))
                           : spv_emit_u_less(&c->emit, c->bool_type, left, right);
            u32 min_value = spv_emit_select(&c->emit, c->u32_type, less, left, right);
            u32 max_value = spv_emit_select(&c->emit, c->u32_type, less, right, left);
            u32 choose_min = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 value = spv_emit_select(&c->emit, c->u32_type, choose_min, min_value, max_value);
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_VSETP) {
            u32 left = vm_select(c, i32_type, reg_read(c, a), B(w, 36, 3));
            u32 right = vm_select(c, i32_type, reg_read(c, b), B(w, 28, 3));
            u32 less = spv_emit_u_less(&c->emit, c->bool_type, left, right);
            u32 greater = spv_emit_u_less(&c->emit, c->bool_type, right, left);
            u32 equal = spv_emit_i_equal(&c->emit, c->bool_type, left, right);
            u32 cmp = B(w, 43, 2) | (B(w, 47, 1) << 2);
            u32 result = cmp == 0   ? c->false_value
                         : cmp == 1 ? less
                         : cmp == 2 ? equal
                         : cmp == 3 ? cc_not(c, greater)
                         : cmp == 4 ? greater
                         : cmp == 5 ? cc_not(c, equal)
                         : cmp == 6 ? cc_not(c, less)
                                    : c->true_value;
            u32 condition = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), result, condition));
            pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), cc_not(c, result), condition));
        } else if (op == O_VMAD) {
            u32 left = vm_select(c, i32_type, reg_read(c, a), B(w, 36, 3) | (B(w, 48, 1) << 3));
            u32 right = B(w, 50, 1) ? vm_select(c, i32_type, reg_read(c, b), B(w, 28, 3) | (B(w, 49, 1) << 3))
                                    : vm_select(c, i32_type, spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 16)),
                                                4 | (B(w, 49, 1) << 3));
            u32 product = spv_emit_imul_u32(&c->emit, c->u32_type, left, right);
            reg_write(c, d, spv_emit_iadd_u32(&c->emit, c->u32_type, product, reg_read(c, B(w, 39, 8))));
        } else if (op == O_VMNMX) {
            u32 asel = B(w, 36, 3) | (B(w, 48, 1) << 3);
            u32 bsel = B(w, 28, 3) | (B(w, 49, 1) << 3);
            int sa = asel >> 3, sb = bsel >> 3, maximum = B(w, 56, 1);
            u32 va = vm_select(c, i32_type, reg_read(c, a), asel);
            u32 vb = B(w, 50, 1) ? vm_select(c, i32_type, reg_read(c, b), bsel)
                                 : spv_emit_constant_u32(&c->emit, c->u32_type,
                                                         sb ? (u32)(i32)(i16)B(w, 20, 16) : B(w, 20, 16));
            u32 less;
            if (sa == sb) {
                less = sa ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, vb),
                                               spv_emit_bitcast(&c->emit, i32_type, va))
                          : spv_emit_u_less(&c->emit, c->bool_type, va, vb);
            } else if (sa) {
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, va));
                less = spv_emit_bool_binary(&c->emit, c->bool_type, 1, negative,
                                            spv_emit_u_less(&c->emit, c->bool_type, va, vb));
            } else {
                u32 nonnegative = spv_emit_bool_not(&c->emit, c->bool_type,
                                                    spv_emit_s_greater(&c->emit, c->bool_type,
                                                                       spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                                       spv_emit_bitcast(&c->emit, i32_type, vb)));
                less = spv_emit_bool_binary(&c->emit, c->bool_type, 0, nonnegative,
                                            spv_emit_u_less(&c->emit, c->bool_type, va, vb));
            }
            u32 result = spv_emit_select(&c->emit, c->u32_type, less, maximum ? vb : va, maximum ? va : vb);
            int result_signed = sa == sb ? sa : !maximum;
            int signed_format = B(w, 54, 1);
            if (B(w, 55, 1)) {
                if (signed_format && !result_signed) {
                    u32 cap = spv_emit_constant_u32(&c->emit, c->u32_type, 0x7fffffffu);
                    u32 above = spv_emit_u_less(&c->emit, c->bool_type, cap, result);
                    result = spv_emit_select(&c->emit, c->u32_type, above, cap, result);
                } else if (!signed_format && result_signed) {
                    u32 negative =
                        spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                           spv_emit_bitcast(&c->emit, i32_type, result));
                    result = spv_emit_select(&c->emit, c->u32_type, negative, c->zero, result);
                }
            }
            u32 third = reg_read(c, B(w, 39, 8));
            u32 mode = B(w, 51, 3);
            if (mode < 4) {
                u32 off = mode == 0 || mode == 3 ? 16 : 0;
                u32 width = mode == 2 || mode == 3 ? 8 : 16;
                result = spv_emit_bitfield_insert(&c->emit, c->u32_type, third, result,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, off),
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, width));
            } else if (mode == 4) {
                result = spv_emit_iadd_u32(&c->emit, c->u32_type, result, third);
            } else if (mode == 5 || mode == 6) {
                u32 cmp = signed_format
                              ? spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, result),
                                                   spv_emit_bitcast(&c->emit, i32_type, third))
                              : spv_emit_u_less(&c->emit, c->bool_type, third, result);
                result =
                    spv_emit_select(&c->emit, c->u32_type, cmp, mode == 5 ? third : result, mode == 5 ? result : third);
            }
            reg_write(c, d, result);
        } else if (op == O_ICMP_R || op == O_ICMP_RC || op == O_ICMP_CR || op == O_ICMP_I) {
            u32 third = reg_read(c, B(w, 39, 8));
            u32 right = op == O_ICMP_RC || op == O_ICMP_CR ? cbuf_word(c, w)
                        : op == O_ICMP_I                   ? integer_right(c, w, 2)
                                                           : reg_read(c, b);
            u32 comparison = op == O_ICMP_RC ? right : third;
            u32 alternative = op == O_ICMP_RC ? third : right;
            /* ICMP has no .X: bit 43 is part of Rc */
            u32 test = integer_compare(c, i32_type, w & ~(1ull << 43), comparison, c->zero);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, test, reg_read(c, a), alternative));
        } else if (op == O_FCMP_R || op == O_FCMP_RC || op == O_FCMP_CR || op == O_FCMP_I) {
            u32 third = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, B(w, 39, 8)));
            u32 comparison = op == O_FCMP_RC ? cbuf_read(c, w) : third;
            u32 alternative = op == O_FCMP_RC   ? reg_read(c, B(w, 39, 8))
                              : op == O_FCMP_CR ? cbuf_word(c, w)
                              : op == O_FCMP_I  ? spv_emit_bitcast(&c->emit, c->u32_type, float_imm20(c, w))
                                                : reg_read(c, b);
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), comparison, zero_f);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, test, reg_read(c, a), alternative));
        } else if (op == O_BFI_R || op == O_BFI_RC || op == O_BFI_CR || op == O_BFI_I) {
            u32 base = op == O_BFI_RC ? cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
            u32 packed = op == O_BFI_CR   ? cbuf_word(c, w)
                         : op == O_BFI_RC ? reg_read(c, B(w, 39, 8))
                         : op == O_BFI_I  ? integer_right(c, w, 2)
                                          : reg_read(c, b);
            u32 mask = spv_emit_constant_u32(&c->emit, c->u32_type, 255);
            u32 offset = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed, mask);
            u32 width =
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                         spv_emit_shift_right_u32(&c->emit, c->u32_type, packed,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
                                         mask);
            u32 thirty_two = spv_emit_constant_u32(&c->emit, c->u32_type, 32);
            u32 valid = spv_emit_u_less(&c->emit, c->bool_type, offset, thirty_two);
            u32 safe_offset = spv_emit_select(&c->emit, c->u32_type, valid, offset, c->zero);
            u32 remaining = spv_emit_isub_u32(&c->emit, c->u32_type, thirty_two, safe_offset);
            u32 fits = spv_emit_u_less(&c->emit, c->bool_type, width, remaining);
            width = spv_emit_select(&c->emit, c->u32_type, fits, width, remaining);
            u32 value = spv_emit_bitfield_insert(&c->emit, c->u32_type, base, reg_read(c, a), safe_offset, width);
            value = spv_emit_select(&c->emit, c->u32_type, valid, value, base);
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_F2F_R || op == O_F2F_C || op == O_F2F_I) {
            u32 raw = op == O_F2F_I ? spv_emit_bitcast(&c->emit, c->u32_type, float_imm20(c, w))
                                    : integer_right(c, w, op - O_F2F_R);
            u32 src;
            if (B(w, 10, 2) == 1) {
                u32 unpacked = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_UNPACK_HALF2X16, raw);
                src = spv_emit_vec4_extract(&c->emit, c->f32_type, unpacked, B(w, 41, 1));
            } else
                src = spv_emit_bitcast(&c->emit, c->f32_type, raw);
            src = float_mod(c, src, B(w, 49, 1), B(w, 45, 1));
            /* Between two floats of one size, bit 42 makes it an integer rounding (39-40: even, floor, ceil, trunc),
               F16 as well as F32: Unity's half-precision frac() is an F2F.F16.F16.FLOOR. Between sizes, 39-40 is
               the conversion's rounding, which this translation leaves to the host */
            static const SpvGlsl450Op rounds[] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
            if (B(w, 8, 2) == B(w, 10, 2) && B(w, 42, 1))
                src = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, rounds[B(w, 39, 2)], src);
            src = float_saturate(c, src, B(w, 50, 1));
            u32 result;
            if (B(w, 8, 2) == 1) {
                u32 pair =
                    spv_emit_vec2_construct(&c->emit, vec2, src, spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
                result = spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair);
            } else
                result = spv_emit_bitcast(&c->emit, c->u32_type, src);
            reg_write(c, d, result);
        } else if (op == O_F2I_R || op == O_F2I_C || op == O_F2I_I) {
            u32 raw = op == O_F2I_I ? spv_emit_bitcast(&c->emit, c->u32_type, float_imm20(c, w))
                                    : integer_right(c, w, op - O_F2I_R);
            u32 src;
            if (B(w, 10, 2) == 1) {
                u32 unpacked = spv_emit_glsl_unary(&c->emit, vec2, c->glsl, SPV_GLSL_UNPACK_HALF2X16, raw);
                src = spv_emit_vec4_extract(&c->emit, c->f32_type, unpacked, B(w, 41, 1));
            } else
                src = spv_emit_bitcast(&c->emit, c->f32_type, raw);
            src = float_mod(c, src, B(w, 49, 1), B(w, 45, 1));
            static const SpvGlsl450Op rounds[] = {SPV_GLSL_ROUND_EVEN, SPV_GLSL_FLOOR, SPV_GLSL_CEIL, SPV_GLSL_TRUNC};
            src = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, rounds[B(w, 39, 2)], src);
            if (!B(w, 12, 1)) {
                u32 low = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
                u32 high = spv_emit_constant_f32(&c->emit, c->f32_type, 4294967040.0f);
                u32 bounded = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FCLAMP, src, low, high);
                u32 result = spv_emit_convert_f_to_u(&c->emit, c->u32_type, bounded);
                u32 overflow = spv_emit_fcompare(&c->emit, c->bool_type, 6, src,
                                                 spv_emit_constant_f32(&c->emit, c->f32_type, 4294967296.0f));
                result = spv_emit_select(&c->emit, c->u32_type, overflow,
                                         spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu), result);
                result = spv_emit_select(&c->emit, c->u32_type, spv_emit_isnan(&c->emit, c->bool_type, src), c->zero,
                                         result);
                if (B(w, 8, 2) == 1) {
                    u32 bound = spv_emit_constant_u32(&c->emit, c->u32_type, 65535);
                    u32 large = spv_emit_u_less(&c->emit, c->bool_type, bound, result);
                    result = spv_emit_select(&c->emit, c->u32_type, large, bound, result);
                }
                reg_write(c, d, result);
            } else {
                u32 low = spv_emit_constant_f32(&c->emit, c->f32_type, -2147483648.0f);
                u32 high = spv_emit_constant_f32(&c->emit, c->f32_type, 2147483520.0f);
                u32 bounded = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FCLAMP, src, low, high);
                u32 result = spv_emit_convert_f_to_s(&c->emit, i32_type, bounded);
                u32 overflow = spv_emit_fcompare(&c->emit, c->bool_type, 6, src,
                                                 spv_emit_constant_f32(&c->emit, c->f32_type, 2147483648.0f));
                result = spv_emit_select(&c->emit, i32_type, overflow,
                                         spv_emit_constant_u32(&c->emit, i32_type, 0x7fffffff), result);
                result = spv_emit_select(&c->emit, i32_type, spv_emit_isnan(&c->emit, c->bool_type, src),
                                         spv_emit_constant_u32(&c->emit, i32_type, 0), result);
                if (B(w, 8, 2) == 1) { /* S16 saturates like U16 */
                    u32 top = spv_emit_constant_u32(&c->emit, i32_type, 32767);
                    u32 bottom = spv_emit_constant_u32(&c->emit, i32_type, 0xffff8000u);
                    result = spv_emit_select(&c->emit, i32_type,
                                             spv_emit_s_greater(&c->emit, c->bool_type, result, top), top, result);
                    result = spv_emit_select(
                        &c->emit, i32_type, spv_emit_s_greater(&c->emit, c->bool_type, bottom, result), bottom, result);
                }
                reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, result));
            }
        } else if (op == O_I2I_R || op == O_I2I_C || op == O_I2I_I) {
            u32 value = integer_right(c, w, op - O_I2I_R);
            u32 src_size = B(w, 10, 2), dst_size = B(w, 8, 2);
            if (src_size < 2) {
                u32 offset = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 41, 3) * 8);
                u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 8u << src_size);
                value = B(w, 13, 1)
                            ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, value),
                                                                          offset, width))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, width);
            }
            if (B(w, 49, 1)) {
                u32 negative = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                  spv_emit_bitcast(&c->emit, i32_type, value));
                u32 absolute = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, value);
                value = spv_emit_select(&c->emit, c->u32_type, negative, absolute, value);
            }
            if (B(w, 45, 1)) value = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, value);
            if (B(w, 50, 1) && dst_size < 2) {
                u32 high = (B(w, 12, 1) ? 0x7fffffffu : 0xffffffffu) >> (32 - (8u << dst_size));
                u32 hi = spv_emit_constant_u32(&c->emit, c->u32_type, high);
                if (B(w, 13, 1)) {
                    u32 negative =
                        spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_constant_u32(&c->emit, i32_type, 0),
                                           spv_emit_bitcast(&c->emit, i32_type, value));
                    if (B(w, 12, 1)) {
                        u32 lo = spv_emit_constant_u32(&c->emit, c->u32_type, ~high);
                        u32 below = spv_emit_s_greater(&c->emit, c->bool_type, spv_emit_bitcast(&c->emit, i32_type, lo),
                                                       spv_emit_bitcast(&c->emit, i32_type, value));
                        value = spv_emit_select(&c->emit, c->u32_type, below, lo, value);
                    } else
                        value = spv_emit_select(&c->emit, c->u32_type, negative, c->zero, value);
                }
                u32 above = B(w, 13, 1) && B(w, 12, 1) ? spv_emit_s_greater(&c->emit, c->bool_type,
                                                                            spv_emit_bitcast(&c->emit, i32_type, value),
                                                                            spv_emit_bitcast(&c->emit, i32_type, hi))
                                                       : spv_emit_u_less(&c->emit, c->bool_type, hi, value);
                value = spv_emit_select(&c->emit, c->u32_type, above, hi, value);
            } else if (B(w, 50, 1) && B(w, 12, 1) != B(w, 13, 1)) {
                /* 32 bits saturate only across a change of signedness */
                if (B(w, 13, 1))
                    value = spv_emit_select(&c->emit, c->u32_type,
                                            spv_emit_s_greater(&c->emit, c->bool_type,
                                                               spv_emit_constant_u32(&c->emit, i32_type, 0),
                                                               spv_emit_bitcast(&c->emit, i32_type, value)),
                                            c->zero, value);
                else {
                    u32 top = spv_emit_constant_u32(&c->emit, c->u32_type, 0x7fffffffu);
                    value = spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, top, value),
                                            top, value);
                }
            }
            if (dst_size < 2) {
                u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 8u << dst_size);
                value = spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, c->zero, width);
            }
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_I2F_R || op == O_I2F_C || op == O_I2F_I) {
            u32 value = integer_right(c, w, op - O_I2F_R);
            u32 size = B(w, 10, 2);
            if (size < 2) {
                u32 offset =
                    spv_emit_constant_u32(&c->emit, c->u32_type, size == 0 ? B(w, 41, 2) * 8 : (B(w, 41, 2) & 2) * 8);
                u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, size == 0 ? 8 : 16);
                value = B(w, 13, 1)
                            ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, value),
                                                                          offset, width))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, value, offset, width);
            }
            u32 target_type = B(w, 8, 2) == 3 ? c->f64_type : c->f32_type;
            u32 result = B(w, 13, 1) ? spv_emit_convert_s_to_f(&c->emit, target_type,
                                                               spv_emit_bitcast(&c->emit, i32_type, value))
                                     : spv_emit_convert_u_to_f(&c->emit, target_type, value);
            if (B(w, 8, 2) == 3) {
                result = double_mod(c, result, B(w, 49, 1), B(w, 45, 1));
                double_write(c, vec2_u32, d, result);
            } else if (B(w, 8, 2) == 1) {
                result = float_mod(c, result, B(w, 49, 1), B(w, 45, 1));
                u32 pair =
                    spv_emit_vec2_construct(&c->emit, vec2, result, spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f));
                reg_write(c, d, spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, pair));
            } else {
                result = float_mod(c, result, B(w, 49, 1), B(w, 45, 1));
                reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, result));
            }
        } else if (op == O_ISCADD_R || op == O_ISCADD_C || op == O_ISCADD_I) {
            u32 left = reg_read(c, a);
            u32 avg = B(w, 48, 2);
            u32 shifted = spv_emit_shift_left_u32(&c->emit, c->u32_type, left,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 39, 5)));
            u32 rhs = integer_right(c, w, op - O_ISCADD_R);
            reg_write(c, d, integer_add(c, shifted, rhs, avg, 0, B(w, 47, 1)));
        } else if (op >= O_BFE_R && op <= O_BFE_I) {
            u32 packed = integer_right(c, w, op - O_BFE_R);
            u32 source = reg_read(c, a);
            if (B(w, 40, 1)) source = spv_emit_bit_reverse_u32(&c->emit, c->u32_type, source);
            u32 raw_offset = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, packed,
                                                      spv_emit_constant_u32(&c->emit, c->u32_type, 255));
            u32 thirty_one = spv_emit_constant_u32(&c->emit, c->u32_type, 31);
            u32 offset =
                spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, thirty_one, raw_offset),
                                thirty_one, raw_offset);
            u32 raw_width =
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type,
                                         spv_emit_shift_right_u32(&c->emit, c->u32_type, packed,
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 8)),
                                         spv_emit_constant_u32(&c->emit, c->u32_type, 255));
            u32 remaining =
                spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 32), offset);
            u32 width =
                spv_emit_select(&c->emit, c->u32_type, spv_emit_u_less(&c->emit, c->bool_type, remaining, raw_width),
                                remaining, raw_width);
            u32 nonzero = spv_emit_i_not_equal(&c->emit, c->bool_type, width, c->zero);
            u32 safe_width =
                spv_emit_select(&c->emit, c->u32_type, nonzero, width, spv_emit_constant_u32(&c->emit, c->u32_type, 1));
            u32 extracted =
                B(w, 48, 1) ? spv_emit_bitcast(&c->emit, c->u32_type,
                                               spv_emit_bitfield_sextract(&c->emit, i32_type,
                                                                          spv_emit_bitcast(&c->emit, i32_type, source),
                                                                          offset, safe_width))
                            : spv_emit_bitfield_uextract(&c->emit, c->u32_type, source, offset, safe_width);
            u32 value = spv_emit_select(&c->emit, c->u32_type, nonzero, extracted, c->zero);
            reg_write(c, d, value);
        } else if (op == O_SHL_R || op == O_SHL_C || op == O_SHL_I) {
            u32 shift = integer_right(c, w, op - O_SHL_R);
            u32 safe = B(w, 39, 1) ? spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift,
                                                              spv_emit_constant_u32(&c->emit, c->u32_type, 31))
                                   : shift;
            u32 value = spv_emit_shift_left_u32(&c->emit, c->u32_type, reg_read(c, a), safe);
            if (!B(w, 39, 1)) {
                u32 bound = spv_emit_constant_u32(&c->emit, c->u32_type, 32);
                u32 valid = spv_emit_u_less(&c->emit, c->bool_type, shift, bound);
                value = spv_emit_select(&c->emit, c->u32_type, valid, value, c->zero);
            }
            reg_write(c, d, value);
        } else if (op == O_LEA_LR || op == O_LEA_LC || op == O_LEA_LI || op == O_LEA_HR || op == O_LEA_HC) {
            int high_form = op == O_LEA_HR || op == O_LEA_HC;
            u32 shift = high_form ? B(w, op == O_LEA_HR ? 28 : 51, 5) : B(w, 39, 5);
            int negative = high_form ? B(w, op == O_LEA_HR ? 37 : 56, 1) : B(w, 45, 1);
            u32 source = reg_read(c, a);
            u32 rhs = op == O_LEA_LR || op == O_LEA_HR ? reg_read(c, b)
                      : op == O_LEA_LI                 ? integer_right(c, w, 2)
                                                       : integer_right(c, w, 1);
            u32 value;
            int extended;
            u32 old_carry;
            u32 sum;
            if (high_form) {
                u32 hi = reg_read(c, B(w, 39, 8));
                value = shift ? spv_emit_bitwise_or_u32(
                                    &c->emit, c->u32_type,
                                    spv_emit_shift_right_u32(&c->emit, c->u32_type, source,
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 32 - shift)),
                                    spv_emit_shift_left_u32(&c->emit, c->u32_type, hi,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, shift)))
                              : hi;
                if (negative) {
                    u32 carry =
                        spv_emit_i_equal(&c->emit, c->bool_type,
                                         spv_emit_shift_left_u32(&c->emit, c->u32_type, source,
                                                                 spv_emit_constant_u32(&c->emit, c->u32_type, shift)),
                                         c->zero);
                    value =
                        spv_emit_iadd_u32(&c->emit, c->u32_type, spv_emit_not_u32(&c->emit, c->u32_type, value),
                                          spv_emit_select(&c->emit, c->u32_type, carry,
                                                          spv_emit_constant_u32(&c->emit, c->u32_type, 1), c->zero));
                }
            } else {
                if (negative) source = spv_emit_isub_u32(&c->emit, c->u32_type, c->zero, source);
                value = spv_emit_shift_left_u32(&c->emit, c->u32_type, source,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, shift));
            }
            /* .X adds the carry in (LO at 46, HI at 38 or 57), .CC writes the flags, and LO's predicate takes the
               add's carry out: LEA.LO ... P0 then LEA.HI.X is how a 64-bit address is formed */
            extended = B(w, op == O_LEA_HR ? 38 : op == O_LEA_HC ? 57 : 46, 1);
            old_carry = extended ? spv_emit_load(&c->emit, c->bool_type, c->cc_c) : 0;
            sum = integer_add(c, value, rhs, 0, extended, B(w, 47, 1));
            reg_write(c, d, sum);
            if (!high_form && B(w, 48, 3) < 7) {
                u32 carry = spv_emit_u_less(&c->emit, c->bool_type, sum, value);
                if (extended)
                    carry = spv_emit_bool_binary(
                        &c->emit, c->bool_type, 1, carry,
                        spv_emit_bool_binary(&c->emit, c->bool_type, 0, old_carry,
                                             spv_emit_i_equal(&c->emit, c->bool_type, sum, value)));
                pred_write(c, B(w, 48, 3), carry);
            }
        } else if (op == O_SHF_LR || op == O_SHF_LI || op == O_SHF_RR || op == O_SHF_RI) {
            int left_shift = op == O_SHF_LR || op == O_SHF_LI;
            u32 shift = (op == O_SHF_LR || op == O_SHF_RR) ? reg_read(c, b)
                                                           : spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 19));
            u32 longest = B(w, 37, 2) >= 2;
            u32 limit = spv_emit_constant_u32(&c->emit, c->u32_type, longest ? 63 : 32);
            u32 wrapped = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift,
                                                   spv_emit_constant_u32(&c->emit, c->u32_type, longest ? 62 : 31));
            u32 safe = B(w, 50, 1)
                           ? wrapped
                           : spv_emit_select(&c->emit, c->u32_type,
                                             spv_emit_u_less(&c->emit, c->bool_type, limit, shift), limit, shift);
            u32 low = reg_read(c, a), high = reg_read(c, B(w, 39, 8));
            u32 small =
                spv_emit_bitwise_and_u32(&c->emit, c->u32_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 left = spv_emit_shift_left_u32(&c->emit, c->u32_type, high, small);
            u32 reverse =
                spv_emit_isub_u32(&c->emit, c->u32_type, spv_emit_constant_u32(&c->emit, c->u32_type, 32), safe);
            reverse = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, reverse,
                                               spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 right = spv_emit_shift_right_u32(&c->emit, c->u32_type, low, reverse);
            u32 combined = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, left, right);
            if (left_shift)
                combined = spv_emit_select(&c->emit, c->u32_type,
                                           spv_emit_i_equal(&c->emit, c->bool_type, safe, c->zero), high, combined);
            else {
                u32 low_part = spv_emit_shift_right_u32(&c->emit, c->u32_type, low, small);
                u32 high_part = spv_emit_shift_left_u32(&c->emit, c->u32_type, high, reverse);
                combined = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, low_part, high_part);
                combined = spv_emit_select(&c->emit, c->u32_type,
                                           spv_emit_i_equal(&c->emit, c->bool_type, safe, c->zero), low, combined);
            }
            u32 long_shift =
                spv_emit_isub_u32(&c->emit, c->u32_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
            long_shift = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, long_shift,
                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 long_value =
                left_shift ? spv_emit_shift_left_u32(&c->emit, c->u32_type, low, long_shift)
                : B(w, 37, 2) == 3
                    ? spv_emit_bitcast(&c->emit, c->u32_type,
                                       spv_emit_shift_right_s32(&c->emit, i32_type,
                                                                spv_emit_bitcast(&c->emit, i32_type, high), long_shift))
                    : spv_emit_shift_right_u32(&c->emit, c->u32_type, high, long_shift);
            u32 result = spv_emit_select(
                &c->emit, c->u32_type,
                spv_emit_u_less(&c->emit, c->bool_type, safe, spv_emit_constant_u32(&c->emit, c->u32_type, 32)),
                combined, long_value);
            reg_write(c, d, result);
        } else if (op == O_SHR_R || op == O_SHR_C || op == O_SHR_I) {
            u32 shift = integer_right(c, w, op - O_SHR_R);
            u32 source = reg_read(c, a);
            if (B(w, 40, 1)) source = spv_emit_bit_reverse_u32(&c->emit, c->u32_type, source);
            u32 safe = spv_emit_bitwise_and_u32(&c->emit, c->u32_type, shift,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
            u32 value;
            if (B(w, 48, 1)) {
                if (!B(w, 39, 1)) {
                    u32 below = spv_emit_u_less(&c->emit, c->bool_type, shift,
                                                spv_emit_constant_u32(&c->emit, c->u32_type, 31));
                    safe = spv_emit_select(&c->emit, c->u32_type, below, shift,
                                           spv_emit_constant_u32(&c->emit, c->u32_type, 31));
                }
                value = spv_emit_bitcast(
                    &c->emit, c->u32_type,
                    spv_emit_shift_right_s32(&c->emit, i32_type, spv_emit_bitcast(&c->emit, i32_type, source), safe));
            } else {
                value = spv_emit_shift_right_u32(&c->emit, c->u32_type, source, safe);
            }
            if (!B(w, 48, 1) && !B(w, 39, 1)) {
                u32 valid =
                    spv_emit_u_less(&c->emit, c->bool_type, shift, spv_emit_constant_u32(&c->emit, c->u32_type, 32));
                value = spv_emit_select(&c->emit, c->u32_type, valid, value, c->zero);
            }
            reg_write(c, d, value);
        } else if (op == O_FADD_R || op == O_FADD_C || op == O_FADD_I || op == O_FADD32I || op == O_FMUL_R ||
                   op == O_FMUL_C || op == O_FMUL_I || op == O_FMUL32I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 right = op == O_FADD_C || op == O_FMUL_C     ? cbuf_read(c, w)
                        : op == O_FADD_I || op == O_FMUL_I   ? float_imm20(c, w)
                        : op == O_FADD32I || op == O_FMUL32I ? float_imm32(c, w)
                                                             : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            if (op == O_FADD_R || op == O_FADD_C || op == O_FADD_I) {
                left = float_mod(c, left, B(w, 46, 1), B(w, 48, 1));
                right = float_mod(c, right, B(w, 49, 1), B(w, 45, 1));
            } else if (op == O_FADD32I) {
                left = float_mod(c, left, B(w, 54, 1), B(w, 56, 1));
                right = float_mod(c, right, B(w, 57, 1), B(w, 53, 1));
            } else if (op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I) {
                right = float_mod(c, right, 0, B(w, 48, 1));
            }
            u32 result = op == O_FADD_R || op == O_FADD_C || op == O_FADD_I || op == O_FADD32I
                             ? spv_emit_fadd(&c->emit, c->f32_type, left, right)
                             : spv_emit_fmul(&c->emit, c->f32_type, left, right);
            spv_emit_no_contraction(&c->emit, result);
            if (op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I) {
                if (B(w, 44, 2) == 2) {
                    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
                    u32 left_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, left, zero);
                    u32 right_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, right, zero);
                    u32 either = spv_emit_bool_binary(&c->emit, c->bool_type, 1, left_zero, right_zero);
                    result = spv_emit_select(&c->emit, c->f32_type, either, zero, result);
                }
                static const float scale[] = {1.0f, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f};
                u32 sc = B(w, 41, 3);
                if (sc) {
                    u32 factor = spv_emit_constant_f32(&c->emit, c->f32_type, scale[sc]);
                    result = spv_emit_fmul(&c->emit, c->f32_type, result, factor);
                    spv_emit_no_contraction(&c->emit, result);
                }
            }
            int sat = op == O_FADD32I ? 0 : op == O_FMUL32I ? B(w, 55, 1) : B(w, 50, 1);
            result = float_saturate(c, result, sat);
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, result));
        } else if (op == O_FFMA_R || op == O_FFMA_RC || op == O_FFMA_CR || op == O_FFMA_I || op == O_FFMA32I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 right = op == O_FFMA32I   ? float_imm32(c, w)
                        : op == O_FFMA_CR ? cbuf_read(c, w)
                        : op == O_FFMA_I
                            ? float_imm20(c, w)
                            : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, op == O_FFMA_RC ? B(w, 39, 8) : b));
            u32 addend = op == O_FFMA_RC
                             ? cbuf_read(c, w)
                             : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, op == O_FFMA32I ? d : B(w, 39, 8)));
            if (op == O_FFMA32I) {
                if (B(w, 56, 1)) left = spv_emit_fnegate(&c->emit, c->f32_type, left);
                if (B(w, 57, 1)) addend = spv_emit_fnegate(&c->emit, c->f32_type, addend);
            } else {
                if (B(w, 48, 1)) right = spv_emit_fnegate(&c->emit, c->f32_type, right);
                if (B(w, 49, 1)) addend = spv_emit_fnegate(&c->emit, c->f32_type, addend);
            }
            u32 result = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMA, left, right, addend);
            if (B(w, 53, 2) == 2) {
                u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
                u32 left_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, left, zero);
                u32 right_zero = spv_emit_fcompare(&c->emit, c->bool_type, 2, right, zero);
                result = spv_emit_select(&c->emit, c->f32_type, cc_or(c, left_zero, right_zero), addend, result);
            }
            result = float_saturate(c, result, op == O_FFMA32I ? B(w, 55, 1) : B(w, 50, 1));
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, result));
        } else if (op == O_FMNMX_R || op == O_FMNMX_C || op == O_FMNMX_I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 right = op == O_FMNMX_C   ? cbuf_read(c, w)
                        : op == O_FMNMX_I ? float_imm20(c, w)
                                          : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            left = float_mod(c, left, B(w, 46, 1), B(w, 48, 1));
            right = float_mod(c, right, B(w, 49, 1), B(w, 45, 1));
            u32 is_left_nan = spv_emit_isnan(&c->emit, c->bool_type, left);
            u32 is_right_nan = spv_emit_isnan(&c->emit, c->bool_type, right);
            u32 minimum = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMIN, left, right);
            u32 maximum = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMAX, left, right);
            u32 choose_min = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 value = spv_emit_select(&c->emit, c->f32_type, choose_min, minimum, maximum);
            value = spv_emit_select(&c->emit, c->f32_type, is_right_nan, left, value);
            value = spv_emit_select(&c->emit, c->f32_type, is_left_nan, right, value);
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, value));
        } else if (op == O_FSETP_R || op == O_FSETP_C || op == O_FSETP_I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            left = float_mod(c, left, B(w, 7, 1), B(w, 43, 1));
            u32 right;
            if (op == O_FSETP_C)
                right = cbuf_read(c, w);
            else if (op == O_FSETP_I)
                right = spv_emit_constant_u32(&c->emit, c->f32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
            else
                right = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            right = float_mod(c, right, B(w, 44, 1), B(w, 6, 1));
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 op_bool = B(w, 45, 2);
            u32 yes = bool_combine(c, op_bool, test, pc);
            u32 inverse = spv_emit_bool_not(&c->emit, c->bool_type, test);
            u32 no = bool_combine(c, op_bool, inverse, pc);
            pred_write(c, B(w, 3, 3), yes);
            pred_write(c, B(w, 0, 3), no);
        } else if ((op >= O_DADD_R && op <= O_DADD_I) || (op >= O_DMUL_R && op <= O_DMUL_I) ||
                   (op >= O_DFMA_R && op <= O_DFMA_I) || (op >= O_DMNMX_R && op <= O_DMNMX_I)) {
            u32 left = double_read(c, vec2_u32, a);
            u32 right = op == O_DFMA_RC                      ? double_read(c, vec2_u32, B(w, 39, 8))
                        : op == O_DFMA_CR                    ? double_source(c, vec2_u32, w, 1)
                        : op >= O_DFMA_R && op <= O_DFMA_I   ? double_source(c, vec2_u32, w, op == O_DFMA_I ? 2 : 0)
                        : (op >= O_DADD_R && op <= O_DADD_I) ? double_source(c, vec2_u32, w, op - O_DADD_R)
                        : (op >= O_DMUL_R && op <= O_DMUL_I) ? double_source(c, vec2_u32, w, op - O_DMUL_R)
                                                             : double_source(c, vec2_u32, w, op - O_DMNMX_R);
            u32 value;
            if (op >= O_DADD_R && op <= O_DADD_I) {
                left = double_mod(c, left, B(w, 46, 1), B(w, 48, 1));
                right = double_mod(c, right, B(w, 49, 1), B(w, 45, 1));
                value = spv_emit_fadd(&c->emit, c->f64_type, left, right);
                spv_emit_no_contraction(&c->emit, value);
            } else if (op >= O_DMUL_R && op <= O_DMUL_I) {
                left = double_mod(c, left, 0, B(w, 48, 1));
                value = spv_emit_fmul(&c->emit, c->f64_type, left, right);
                spv_emit_no_contraction(&c->emit, value);
            } else if (op >= O_DFMA_R && op <= O_DFMA_I) {
                u32 third;
                right = double_mod(c, right, 0, B(w, 48, 1));
                third = op == O_DFMA_RC ? double_source(c, vec2_u32, w, 1) : double_read(c, vec2_u32, B(w, 39, 8));
                u32 addend = double_mod(c, third, 0, B(w, 49, 1));
                value = spv_emit_glsl_ternary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_FMA, left, right, addend);
            } else {
                left = double_mod(c, left, B(w, 46, 1), B(w, 48, 1));
                right = double_mod(c, right, B(w, 49, 1), B(w, 45, 1));
                u32 min = spv_emit_glsl_binary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_FMIN, left, right);
                u32 max = spv_emit_glsl_binary(&c->emit, c->f64_type, c->glsl, SPV_GLSL_FMAX, left, right);
                value = spv_emit_select(&c->emit, c->f64_type, pred_read(c, B(w, 39, 3), B(w, 42, 1)), min, max);
            }
            double_write(c, vec2_u32, d, value);
        } else if ((op >= O_DSET_R && op <= O_DSET_I) || (op >= O_DSETP_R && op <= O_DSETP_I)) {
            int is_set = op >= O_DSET_R && op <= O_DSET_I;
            u32 left = double_mod(c, double_read(c, vec2_u32, a), is_set ? B(w, 54, 1) : B(w, 7, 1), B(w, 43, 1));
            u32 right = double_mod(c, double_source(c, vec2_u32, w, is_set ? op - O_DSET_R : op - O_DSETP_R),
                                   B(w, 44, 1), is_set ? B(w, 53, 1) : B(w, 6, 1));
            u32 comparison = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
            u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            if (is_set) {
                u32 test = bool_combine(c, B(w, 45, 2), comparison, predicate);
                u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 52, 1) ? 0x3f800000u : 0xffffffffu);
                u32 value = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
                if (B(w, 47, 1)) flags_logic(c, value);
                reg_write(c, d, value);
            } else {
                u32 inverse = spv_emit_bool_not(&c->emit, c->bool_type, comparison);
                pred_write(c, B(w, 3, 3), bool_combine(c, B(w, 45, 2), comparison, predicate));
                pred_write(c, B(w, 0, 3), bool_combine(c, B(w, 45, 2), inverse, predicate));
            }
        } else if (op == O_FSET_R || op == O_FSET_C || op == O_FSET_I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 right = op == O_FSET_C   ? cbuf_read(c, w)
                        : op == O_FSET_I ? float_imm20(c, w)
                                         : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            left = float_mod(c, left, B(w, 54, 1), B(w, 43, 1));
            right = float_mod(c, right, B(w, 44, 1), B(w, 53, 1));
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            test = bool_combine(c, B(w, 45, 2), test, pc);
            u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 52, 1) ? 0x3f800000u : 0xffffffffu);
            u32 value = spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero);
            if (B(w, 47, 1)) flags_logic(c, value);
            reg_write(c, d, value);
        } else if (op == O_IPA) {
            u32 addr = B(w, 30, 8) * 4;
            u32 component = (addr >> 2) & 3;
            u32 value;
            u32 interpolation = 0;
            if (addr == 0x64) {
                value = layer_input ? spv_emit_bitcast(&c->emit, c->f32_type,
                                                       spv_emit_bitcast(&c->emit, c->u32_type,
                                                                        spv_emit_load(&c->emit, i32_type, layer_input)))
                                    : spv_emit_bitcast(&c->emit, c->f32_type, c->zero); /* no capability: layer 0 */
            } else if (addr == 0x3fc) {
                u32 facing = spv_emit_load(&c->emit, c->bool_type, frontface);
                u32 bits = spv_emit_select(&c->emit, c->u32_type, facing,
                                           spv_emit_constant_u32(&c->emit, c->u32_type, 0xffffffffu), c->zero);
                value = spv_emit_bitcast(&c->emit, c->f32_type, bits);
            } else if (addr >= 0x80) {
                u32 input = (addr - 0x80) >> 4;
                value = spv_emit_load(&c->emit, vec4, inputs[input]);
                value = spv_emit_vec4_extract(&c->emit, c->f32_type, value, component);
                interpolation = ipa_interpolation(sph, input);
            }
            if (addr >= 0x70 && addr < 0x80) {
                u32 coord = spv_emit_load(&c->emit, vec4, fragcoord);
                value = spv_emit_vec4_extract(&c->emit, c->f32_type, coord, component);
                /* a[0x7c] is the interpolated 1/w, FragCoord.w as it is: shaders take its reciprocal for
                   IPA.MUL */
                if (addr != 0x7c && needs_scale && component < 2) {
                    u32 zero_index = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
                    u32 scale = spv_emit_uniform_component(&c->emit, c->scale_pointer, c->scale_buffer, zero_index,
                                                           zero_index, zero_index, c->f32_type);
                    value = spv_emit_fmul(&c->emit, c->f32_type, value, scale);
                    spv_emit_no_contraction(&c->emit, value);
                }
            } else if (interpolation == 2) {
                /* The hardware interpolates attr/w for a perspective input, and the shader multiplies by w
                   (IPA.MUL with the reciprocal of a[0x7c]). Vulkan's input is attr itself: times the
                   interpolated 1/w, FragCoord.w, it is what the hardware gives. */
                u32 coord = spv_emit_load(&c->emit, vec4, fragcoord);
                u32 wcoord = spv_emit_vec4_extract(&c->emit, c->f32_type, coord, 3);
                value = spv_emit_fmul(&c->emit, c->f32_type, value, wcoord);
                spv_emit_no_contraction(&c->emit, value);
            }
            if (B(w, 54, 2) == 1) {
                u32 rhs = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
                value = spv_emit_fmul(&c->emit, c->f32_type, value, rhs);
                spv_emit_no_contraction(&c->emit, value);
            }
            value = float_saturate(c, value, B(w, 51, 1));
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, value));
        } else if (op == O_MUFU || op == O_RRO_R || op == O_RRO_C) {
            u32 source = op == O_MUFU ? a : b;
            u32 value = op == O_RRO_C ? cbuf_read(c, w) : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, source));
            u32 absolute = op == O_MUFU ? B(w, 46, 1) : B(w, 49, 1);
            u32 negative = op == O_MUFU ? B(w, 48, 1) : B(w, 45, 1);
            if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FABS, value);
            if (negative) value = spv_emit_fnegate(&c->emit, c->f32_type, value);
            if (op == O_MUFU) {
                SpvGlsl450Op fn = 0;
                switch (B(w, 20, 4)) {
                    case 0: fn = SPV_GLSL_COS; break;
                    case 1: fn = SPV_GLSL_SIN; break;
                    case 2: fn = SPV_GLSL_EXP2; break;
                    case 3: fn = SPV_GLSL_LOG2; break;
                    case 5: fn = SPV_GLSL_INVERSE_SQRT; break;
                    case 8: fn = SPV_GLSL_SQRT; break;
                    case 4: {
                        u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                        value = spv_emit_fdiv(&c->emit, c->f32_type, one, value);
                        break;
                    }
                    default: break;
                }
                if (fn) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, fn, value);
                if (B(w, 50, 1)) {
                    u32 fzero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
                    u32 fone = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                    value = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FCLAMP, value, fzero, fone);
                }
            }
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, value));
        } else if (op == O_FSWZADD) {
            u32 quad = B(w, 28, 8);
            u32 lhs = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 rhs = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            u32 result;
            if (quad == 0x99 || quad == 0x66 || quad == 0xa5 || quad == 0x5a) {
                result = spv_emit_derivative(&c->emit, c->f32_type, rhs, quad == 0xa5 || quad == 0x5a);
                if (quad == 0x66 || quad == 0x5a) result = spv_emit_fnegate(&c->emit, c->f32_type, result);
            } else
                result = spv_emit_fadd(&c->emit, c->f32_type, lhs, rhs);
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, result));
        } else if (op == O_HADD2_R || op == O_HADD2_C || op == O_HADD2_I || op == O_HMUL2_R || op == O_HMUL2_C ||
                   op == O_HMUL2_I || op == O_HFMA2_R || op == O_HFMA2_RC || op == O_HFMA2_CR || op == O_HFMA2_I) {
            int reg_b = op == O_HADD2_R || op == O_HMUL2_R || op == O_HFMA2_R || op == O_HFMA2_RC;
            int cbuf_b = op == O_HADD2_C || op == O_HMUL2_C || op == O_HFMA2_CR;
            int fma = op == O_HFMA2_R || op == O_HFMA2_RC || op == O_HFMA2_CR || op == O_HFMA2_I;
            int multiply = op == O_HMUL2_R || op == O_HMUL2_C || op == O_HMUL2_I;
            u32 raw_b;
            u32 lhs;
            int rc, cr, imm_c;
            u32 swizzle_b;
            int abs_b;
            int neg_b;
            u32 rhs;
            if (reg_b)
                raw_b = reg_read(c, op == O_HFMA2_RC ? B(w, 39, 8) : b);
            else if (cbuf_b)
                raw_b = cbuf_word(c, w);
            else {
                u32 lo = (B(w, 20, 9) << 6) | (B(w, 29, 1) << 15);
                u32 hi = (B(w, 30, 9) << 6) | (B(w, 56, 1) << 15);
                raw_b = spv_emit_constant_u32(&c->emit, c->u32_type, lo | (hi << 16));
            }
            lhs = half_pair_mod(c, vec2, half_pair(c, vec2, reg_read(c, a), B(w, 47, 2)), fma ? 0 : B(w, 44, 1),
                                fma                   ? 0
                                : !multiply || !reg_b ? B(w, 43, 1)
                                                      : 0);
            /* A constant buffer operand is one F32 for both halves. The HFMA2 forms with one keep B's (RC) or C's
               (CR) register swizzle at 53, neg_b at 56, neg_c at 51 and saturate at 52. The immediate form (I) has
               C's at 53 and neg_c at 51 too; its B carries its own signs (29 and 56), so it has no neg_b. */
            rc = op == O_HFMA2_RC;
            cr = op == O_HFMA2_CR;
            imm_c = op == O_HFMA2_I;
            swizzle_b = cbuf_b ? 1 : rc ? B(w, 53, 2) : reg_b ? B(w, 28, 2) : 0;
            abs_b = fma ? 0 : reg_b ? (int)B(w, 30, 1) : cbuf_b ? (int)B(w, 54, 1) : 0;
            neg_b = rc || cr || op == O_HADD2_C ? (int)B(w, 56, 1) : reg_b ? (int)B(w, 31, 1) : 0;
            rhs = half_pair_mod(c, vec2, half_pair(c, vec2, raw_b, swizzle_b), abs_b, neg_b);
            u32 pair;
            if (fma) {
                u32 add_raw = rc ? cbuf_word(c, w) : reg_read(c, B(w, 39, 8));
                u32 swizzle_c = rc ? 1 : cr || imm_c ? B(w, 53, 2) : B(w, 35, 2);
                u32 addend = half_pair_mod(c, vec2, half_pair(c, vec2, add_raw, swizzle_c), 0,
                                           rc || cr || imm_c ? B(w, 51, 1) : B(w, 30, 1));
                pair = spv_emit_glsl_ternary(&c->emit, vec2, c->glsl, SPV_GLSL_FMA, lhs, rhs, addend);
            } else
                pair = multiply ? spv_emit_fmul(&c->emit, vec2, lhs, rhs) : spv_emit_fadd(&c->emit, vec2, lhs, rhs);
            spv_emit_no_contraction(&c->emit, pair);
            reg_write(
                c, d,
                half_pair_result(c, vec2, pair, reg_read(c, d), B(w, 49, 2), reg_b && !rc ? B(w, 32, 1) : B(w, 52, 1)));
        } else if (op == O_HSET2_R || op == O_HSET2_C || op == O_HSET2_I) {
            u32 raw_b;
            u32 lhs;
            u32 rhs;
            if (op == O_HSET2_R)
                raw_b = reg_read(c, b);
            else if (op == O_HSET2_C)
                raw_b = cbuf_word(c, w);
            else {
                u32 low = (B(w, 20, 9) << 6) | (B(w, 29, 1) << 15);
                u32 high = (B(w, 30, 9) << 6) | (B(w, 56, 1) << 15);
                raw_b = spv_emit_constant_u32(&c->emit, c->u32_type, low | (high << 16));
            }
            lhs = half_pair_mod(c, vec2, half_pair(c, vec2, reg_read(c, a), B(w, 47, 2)), B(w, 44, 1), B(w, 43, 1));
            rhs = half_pair_mod(c, vec2,
                                half_pair(c, vec2, raw_b,
                                          op == O_HSET2_R   ? B(w, 28, 2)
                                          : op == O_HSET2_C ? 1
                                                            : 0),
                                op == O_HSET2_R && B(w, 30, 1),
                                op == O_HSET2_R ? B(w, 31, 1) : op == O_HSET2_C && B(w, 56, 1));
            u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 compare = B(w, op == O_HSET2_R ? 35 : 49, 4);
            u32 result = c->zero;
            for (u32 lane = 0; lane < 2; lane++) {
                u32 left = spv_emit_vec4_extract(&c->emit, c->f32_type, lhs, lane);
                u32 right = spv_emit_vec4_extract(&c->emit, c->f32_type, rhs, lane);
                u32 test = spv_emit_fcompare(&c->emit, c->bool_type, compare, left, right);
                test = bool_combine(c, B(w, 45, 2), test, predicate);
                u32 truth = B(w, op == O_HSET2_R ? 49 : 53, 1) ? 0x3c00u : 0xffffu;
                u32 bits = spv_emit_select(&c->emit, c->u32_type, test,
                                           spv_emit_constant_u32(&c->emit, c->u32_type, truth << (lane * 16)), c->zero);
                result = spv_emit_bitwise_or_u32(&c->emit, c->u32_type, result, bits);
            }
            reg_write(c, d, result);
        } else if (op == O_HSETP2_R || op == O_HSETP2_C || op == O_HSETP2_I) {
            u32 raw_b;
            u32 lhs;
            u32 rhs;
            if (op == O_HSETP2_R)
                raw_b = reg_read(c, b);
            else if (op == O_HSETP2_C)
                raw_b = cbuf_word(c, w);
            else {
                u32 low = (B(w, 20, 9) << 6) | (B(w, 29, 1) << 15);
                u32 high = (B(w, 30, 9) << 6) | (B(w, 56, 1) << 15);
                raw_b = spv_emit_constant_u32(&c->emit, c->u32_type, low | (high << 16));
            }
            lhs = half_pair_mod(c, vec2, half_pair(c, vec2, reg_read(c, a), B(w, 47, 2)), B(w, 44, 1), B(w, 43, 1));
            rhs = half_pair_mod(c, vec2,
                                half_pair(c, vec2, raw_b,
                                          op == O_HSETP2_R   ? B(w, 28, 2)
                                          : op == O_HSETP2_C ? 1
                                                             : 0),
                                op == O_HSETP2_C && B(w, 54, 1) || op == O_HSETP2_R && B(w, 30, 1),
                                op == O_HSETP2_R ? B(w, 31, 1) : op == O_HSETP2_C && B(w, 56, 1));
            u32 predicate = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 compare = B(w, op == O_HSETP2_R ? 35 : 49, 4);
            u32 outcomes[2];
            for (u32 lane = 0; lane < 2; lane++) {
                u32 left = spv_emit_vec4_extract(&c->emit, c->f32_type, lhs, lane);
                u32 right = spv_emit_vec4_extract(&c->emit, c->f32_type, rhs, lane);
                u32 test = spv_emit_fcompare(&c->emit, c->bool_type, compare, left, right);
                outcomes[lane] = bool_combine(c, B(w, 45, 2), test, predicate);
            }
            if (B(w, op == O_HSETP2_R ? 49 : 53, 1)) {
                u32 both = spv_emit_bool_binary(&c->emit, c->bool_type, 0, outcomes[0], outcomes[1]);
                pred_write(c, B(w, 3, 3), both);
                pred_write(c, B(w, 0, 3), spv_emit_bool_not(&c->emit, c->bool_type, both));
            } else {
                pred_write(c, B(w, 3, 3), outcomes[0]);
                pred_write(c, B(w, 0, 3), outcomes[1]);
            }
        } else if (op == O_TLD || op == O_TLD_B) {
            u32 indexed = 0, ldc = 0;
            u32 off = B(w, 36, 13), ty = B(w, 28, 3);
            u32 bank = 0, bank2 = 0, off2 = 0;
            int handle_count =
                op == O_TLD_B ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            if (op == O_TLD_B && !handle_count) return direct_reject(si, "untracked texture handle");
            int type = ty >> 1, array = ty & 1, multisample = op == O_TLD_B && B(w, 50, 1), slot = 0;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                    textures[slot].shadow || textures[slot].bindless != (op == O_TLD_B) ||
                    !!(textures[slot].flags & 2) != multisample ||
                    (op == O_TLD_B &&
                     !direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
                slot++;
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            u32 x = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, a + array));
            u32 y = type == 0 ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                              : spv_emit_bitcast(&c->emit, i32_type, reg_read(c, a + array + 1));
            if (B(w, 35, 1)) {
                u32 packed = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, b + (op == O_TLD_B) + B(w, 55, 1)));
                u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
                u32 ox = spv_emit_bitfield_sextract(&c->emit, i32_type, packed, c->zero, four);
                u32 oy = spv_emit_bitfield_sextract(&c->emit, i32_type, packed, four, four);
                x = spv_emit_iadd_u32(&c->emit, i32_type, x, ox);
                y = spv_emit_iadd_u32(&c->emit, i32_type, y, oy);
            }
            u32 coord;
            if (type == 0 && !array || type == 1 && !array)
                coord = spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
            else {
                u32 z = type == 0 || array ? spv_emit_bitcast(&c->emit, i32_type,
                                                              spv_emit_bitwise_and_u32(
                                                                  &c->emit, c->u32_type, reg_read(c, a),
                                                                  spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff)))
                                           : spv_emit_bitcast(&c->emit, i32_type, reg_read(c, a + 2));
                coord = spv_emit_vec3_construct(&c->emit, vec3_i32, x, y, z);
            }
            u32 sample = multisample
                             ? spv_emit_image_fetch_sample(
                                   &c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot], coord,
                                   spv_emit_bitcast(&c->emit, i32_type, reg_read(c, b + 1 + B(w, 55, 1) + B(w, 35, 1))))
                             : spv_emit_image_fetch(
                                   &c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot], coord,
                                   B(w, 55, 1) ? spv_emit_bitcast(&c->emit, i32_type, reg_read(c, b + (op == O_TLD_B)))
                                               : spv_emit_constant_u32(&c->emit, i32_type, 0));
            u32 mask = B(w, 31, 4), target = d;
            if (!mask) mask = 15;
            for (u32 k = 0; k < 4; k++)
                if (mask & (1u << k)) {
                    reg_write(c, target, texture_component(c, sample, texture_scalar[slot], k));
                    if (target < 255) target++;
                }
        } else if (op == O_SULD || op == O_SUST || op == O_SURED || op == O_SUATOM) {
            int buffer = B(w, 33, 3) == 1;
            u32 bank = 0, off = B(w, 36, 13);
            int bindless = op == O_SUATOM ? (int)B(w, 54, 1) : !B(w, 51, 1);
            if (bindless) direct_track_image_handle(insn, n, i, B(w, 39, 8), &bank, &off, 0);
            u32 x = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, a));
            u32 coord = buffer ? x
                               : spv_emit_vec2_construct(&c->emit, vec2_i32, x,
                                                         spv_emit_bitcast(&c->emit, i32_type, reg_read(c, a + 1)));
            u32 mask = op == O_SULD && B(w, 52, 1) ? 1u : B(w, 20, 4), source = d;
            if (op != O_SULD || buffer) {
                int slot = direct_image_slot(&image_info, off, bindless, bank, op != O_SUST, buffer ? 1 : 3);
                int kind = image_info.img[slot].kind;
                if (op == O_SULD) {
                    u32 texel = spv_emit_image_read(&c->emit, vec4_u32, c->image_type[slot], c->image[slot], coord);
                    reg_write(c, d, spv_emit_vec4_extract(&c->emit, c->u32_type, texel, 0));
                } else if (kind == 1) {
                    u32 opcode;
                    if (op == O_SURED) {
                        u32 operation = B(w, 24, 3);
                        opcode = operation == 0   ? 234
                                 : operation == 1 ? (B(w, 20, 3) ? 236 : 237)
                                                  : (B(w, 20, 3) ? 238 : 239);
                    } else {
                        /* ADD, MIN, MAX, -, -, AND, OR, XOR, EXCH; only S32 compares signed */
                        static const u32 opcodes[9] = {234, 237, 239, 0, 0, 240, 241, 242, 229};
                        u32 operation = B(w, 29, 4);
                        opcode = opcodes[operation];
                        if (B(w, 51, 3) == 1 && (operation == 1 || operation == 2)) opcode--;
                    }
                    u32 pointer = spv_emit_image_texel_pointer(&c->emit, c->u32_type, c->image[slot], coord, c->zero);
                    u32 prior = spv_emit_atomic_u32(&c->emit, c->u32_type, opcode, pointer,
                                                    reg_read(c, op == O_SUATOM ? b : source));
                    if (op == O_SUATOM) reg_write(c, d, prior);
                } else {
                    u32 components[4];
                    for (u32 q = 0; q < 4; q++) {
                        components[q] = kind == 2 ? spv_emit_constant_u32(&c->emit, c->u32_type, q == 3 ? 1 : 0)
                                                  : spv_emit_constant_f32(&c->emit, c->f32_type, q == 3 ? 1.0f : 0.0f);
                        if (mask & (1u << q)) {
                            components[q] = kind == 2 ? reg_read(c, source)
                                                      : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, source));
                            if (source < 255) source++;
                        }
                    }
                    u32 texel = spv_emit_vec4_construct(&c->emit, kind == 2 ? vec4_u32 : vec4, components);
                    spv_emit_image_write(&c->emit, c->image_type[slot], c->image[slot], coord, texel);
                }
            } else {
                int slot = 0;
                while (slot < texture_count &&
                       (textures[slot].cbuf_off != off || textures[slot].type != 1 || textures[slot].array ||
                        textures[slot].shadow || textures[slot].bindless != bindless ||
                        !!(textures[slot].flags & 1) != !!B(w, 52, 1) || (bindless && textures[slot].bl_cbuf != bank)))
                    slot++;
                u32 sample = spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                                  coord, spv_emit_constant_u32(&c->emit, i32_type, 0));
                for (u32 q = 0; q < 4; q++)
                    if (mask & (1u << q)) {
                        reg_write(c, source, texture_component(c, sample, texture_scalar[slot], q));
                        if (source < 255) source++;
                    }
            }
        } else if (op == O_TLDS) {
            u32 off = B(w, 36, 13);
            u32 encoding = B(w, 53, 4);
            int array = encoding == 8, type = encoding == 0 || encoding == 1 ? 0 : encoding == 7 ? 2 : 1;
            int slot = 0;
            u32 x;
            u32 y;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                    textures[slot].shadow || textures[slot].bindless || textures[slot].flags))
                slot++;
            /* the 2D array form takes x and y from Rb and Rb + 1, the layer from Ra's low 16 bits */
            x = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, array ? b : a));
            y = type == 0 ? spv_emit_constant_u32(&c->emit, i32_type, 0)
                          : spv_emit_bitcast(
                                &c->emit, i32_type,
                                reg_read(c, array                                                               ? b + 1
                                            : encoding == 4 || encoding == 5 || encoding == 7 || encoding == 12 ? a + 1
                                                                                                                : b));
            if (encoding == 4 || encoding == 12) {
                u32 packed = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, b + (encoding == 12)));
                u32 four = spv_emit_constant_u32(&c->emit, c->u32_type, 4);
                u32 ox = spv_emit_bitfield_sextract(&c->emit, i32_type, packed, c->zero, four);
                u32 oy = spv_emit_bitfield_sextract(&c->emit, i32_type, packed, four, four);
                x = spv_emit_iadd_u32(&c->emit, i32_type, x, ox);
                y = spv_emit_iadd_u32(&c->emit, i32_type, y, oy);
            }
            u32 coord = type == 2 ? spv_emit_vec3_construct(&c->emit, vec3_i32, x, y,
                                                            spv_emit_bitcast(&c->emit, i32_type, reg_read(c, b)))
                        : array   ? spv_emit_vec3_construct(
                                        &c->emit, vec3_i32, x, y,
                                        spv_emit_bitcast(&c->emit, i32_type,
                                                         spv_emit_bitwise_and_u32(
                                                             &c->emit, c->u32_type, reg_read(c, a),
                                                             spv_emit_constant_u32(&c->emit, c->u32_type, 0xffff))))
                                  : spv_emit_vec2_construct(&c->emit, vec2_i32, x, y);
            u32 lod = encoding == 1 || encoding == 5 || encoding == 12 ? reg_read(c, b) : c->zero;
            u32 sample =
                spv_emit_image_fetch(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot], coord, lod);
            u32 regs[4], comps[4];
            int outputs = texs_outputs(w, regs, comps);
            if (!B(w, 59, 1)) {
                /* fp16 surfaces sample as f32; convert each component and pack pairs into halves */
                u32 halves[4];
                int pairs = 0;
                for (int k = 0; k < outputs; k++)
                    halves[pairs++] = spv_emit_vec4_extract(&c->emit, c->f32_type, sample, comps[k]);
                u32 lo = spv_emit_vec2_construct(&c->emit, vec2, pairs > 0 ? halves[0] : zero_f,
                                                 pairs > 1 ? halves[1] : zero_f);
                lo = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_f(&c->emit, vec2f16, lo));
                if (d < 255) reg_write(c, d, lo);
                if (outputs > 2) {
                    u32 db = B(w, 28, 8);
                    u32 hi = spv_emit_vec2_construct(&c->emit, vec2, pairs > 2 ? halves[2] : zero_f,
                                                     pairs > 3 ? halves[3] : zero_f);
                    hi = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_f(&c->emit, vec2f16, hi));
                    if (db < 255) reg_write(c, db, hi);
                }
            } else
                for (int k = 0; k < outputs; k++)
                    if (regs[k] < 255) reg_write(c, regs[k], texture_component(c, sample, texture_scalar[slot], comps[k]));
        } else if (op == O_TEXS) {
            u32 off = B(w, 36, 13);
            u32 encoding = B(w, 53, 4);
            int shadow = encoding == 4, type = encoding == 12 || encoding == 13   ? 3
                                               : encoding == 10 || encoding == 11 ? 2
                                               : encoding == 0                    ? 0
                                                                                  : 1;
            int array = encoding == 7 || encoding == 8;
            int slot = 0;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].shadow != shadow || textures[slot].type != type ||
                    textures[slot].array != array || textures[slot].bindless || textures[slot].flags))
                slot++;
            u32 x = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, array ? a + 1 : a));
            u32 yreg = shadow || encoding == 3 || encoding == 10 || encoding == 11 || encoding == 12 || encoding == 13
                           ? a + 1
                           : b;
            u32 y = encoding == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                                  : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, yreg));
            u32 uv = array ? spv_emit_vec3_construct(&c->emit, vec3, x, y, array_layer(c, a))
                     : encoding == 10 || encoding == 11 || encoding == 12 || encoding == 13
                         ? spv_emit_vec3_construct(&c->emit, vec3, x, y,
                                                   spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b)))
                         : spv_emit_vec2_construct(&c->emit, vec2, x, y);
            u32 dref = shadow ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b)) : 0;
            u32 sample;
            /* LZ (0, 2, 8, 11) and LL (3, 13) take an explicit level */
            if (encoding == 0 || encoding == 2 || encoding == 3 || encoding == 8 || encoding == 11 || encoding == 13) {
                u32 lod = encoding == 13  ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b + 1))
                          : encoding == 3 ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b))
                                          : zero_f;
                sample = spv_emit_sample2d_lod(&c->emit, texture_vec4[slot], c->sampled_type[slot], c->sampler[slot],
                                               uv, 0, lod);
            } else if (stage != ST_FS) {
                sample = spv_emit_sample2d_lod(&c->emit, shadow ? c->f32_type : texture_vec4[slot],
                                               c->sampled_type[slot], c->sampler[slot], uv, dref, zero_f);
            } else {
                sample = spv_emit_sample2d(&c->emit, shadow ? c->f32_type : texture_vec4[slot], c->sampled_type[slot],
                                           c->sampler[slot], uv, dref);
            }
            u32 regs[4], comps[4];
            int outputs = texs_outputs(w, regs, comps);
            if (B(w, 59, 1)) {
                for (int k = 0; k < outputs; k++) {
                    u32 component;
                    if (regs[k] >= 255) continue;
                    /* a depth compare returns its result in x, y and z, and 1.0 in alpha */
                    component =
                        shadow ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                  comps[k] == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f)
                                                                : sample)
                               : texture_component(c, sample, texture_scalar[slot], comps[k]);
                    reg_write(c, regs[k], component);
                }
            } else {
                for (int k = 0; k < outputs; k += 2) {
                    u32 pair[2];
                    for (int lane = 0; lane < 2; lane++) {
                        u32 value =
                            k + lane >= outputs ? zero_f
                            : shadow
                                ? (comps[k + lane] == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f) : sample)
                                : spv_emit_vec4_extract(&c->emit, texture_scalar[slot], sample, comps[k + lane]);
                        pair[lane] = texture_scalar[slot] == c->f32_type || shadow || k + lane >= outputs ? value
                                     : texture_scalar[slot] == i32_type
                                         ? spv_emit_convert_s_to_f(&c->emit, c->f32_type, value)
                                         : spv_emit_convert_u_to_f(&c->emit, c->f32_type, value);
                    }
                    u32 packed = spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16,
                                                     spv_emit_vec2_construct(&c->emit, vec2, pair[0], pair[1]));
                    u32 target = k == 0 ? d : B(w, 28, 8) == 255 ? d + 1 : B(w, 28, 8);
                    if (target < 255) reg_write(c, target, packed);
                }
            }
        } else if (op == O_TEX || op == O_TEX_B) {
            int bindless = op == O_TEX_B;
            u32 mode = bindless ? B(w, 37, 3) : B(w, 55, 3);
            u32 offset_enabled = B(w, bindless ? 36 : 54, 1);
            u32 source = b + bindless;
            u32 has_level = mode == 2 || mode == 3 || mode == 6 || mode == 7;
            u32 offset_source = source + has_level;
            u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            int type = B(w, 28, 3) >> 1;
            int array = B(w, 28, 1), shadow = B(w, 50, 1);
            int slot = 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            while (slot < texture_count && (textures[slot].cbuf_off != off || textures[slot].shadow != shadow ||
                                            textures[slot].type != type || textures[slot].array != array ||
                                            textures[slot].bindless != bindless || (textures[slot].flags & 3) ||
                                            (bindless && !direct_bindless_matches(&textures[slot], bank, handle_count,
                                                                                  bank2, off2, indexed, ldc))))
                slot++;
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            u32 x = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a + array));
            u32 y = type == 0 ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                              : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a + array + 1));
            u32 coords =
                type == 3 && array ? spv_emit_vec4_construct(
                                         &c->emit, vec4,
                                         (u32[4]){x, y, spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a + 3)),
                                                  array_layer(c, a)})
                : array            ? spv_emit_vec3_construct(&c->emit, vec3, x, y, array_layer(c, a))
                : type >= 2 ? spv_emit_vec3_construct(&c->emit, vec3, x, y,
                                                      spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a + 2)))
                            : spv_emit_vec2_construct(&c->emit, vec2, x, y);
            u32 level = mode == 1   ? zero_f
                        : has_level ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, source))
                                    : 0;
            u32 dref =
                shadow ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, offset_source + offset_enabled)) : 0;
            u32 offset = 0;
            int dynamic_offset = 0;
            if (offset_enabled) {
                u32 packed = 0;
                int known = direct_track_const(insn, n, i, offset_source, &packed, 0);
                u32 bits;
                dynamic_offset = !known;
                bits = known ? 0 : reg_read(c, offset_source);
                if (!known) bits = spv_emit_bitcast(&c->emit, i32_type, bits);
                u32 ox = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 28) >> 28))
                               : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 0),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                u32 oy = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 24) >> 28))
                               : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 4),
                                                            spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                if (type == 2) {
                    u32 oz = known ? spv_emit_constant_u32(&c->emit, i32_type, (u32)((i32)(packed << 20) >> 28))
                                   : spv_emit_bitfield_sextract(&c->emit, i32_type, bits,
                                                                spv_emit_constant_u32(&c->emit, c->u32_type, 8),
                                                                spv_emit_constant_u32(&c->emit, c->u32_type, 4));
                    offset = known ? spv_emit_constant_vec3(&c->emit, vec3_i32, ox, oy, oz)
                                   : spv_emit_vec3_construct(&c->emit, vec3_i32, ox, oy, oz);
                } else {
                    if (type == 0) oy = spv_emit_constant_u32(&c->emit, i32_type, 0);
                    offset = known ? spv_emit_constant_vec2(&c->emit, vec2_i32, ox, oy)
                                   : spv_emit_vec2_construct(&c->emit, vec2_i32, ox, oy);
                }
            }
            /* Vulkan takes a non-constant Offset on gathers only: a sample moves its coordinates instead, by the
               offset over the level's size (1D textures are 2D images one texel high, their y offset 0) */
            if (dynamic_offset) {
                u32 moved = type == 2 ? 3 : 2, components = moved + (u32)array;
                u32 lod_index = spv_emit_convert_f_to_s(&c->emit, i32_type, level ? level : zero_f);
                u32 dimensions = spv_emit_image_query_size_lod(&c->emit, components == 3 ? vec3_i32 : vec2_i32,
                                                               c->sampled_type[slot], c->sampler[slot], lod_index);
                u32 adjusted[3];
                u32 component;
                for (component = 0; component < components; component++) {
                    u32 coord = spv_emit_vec4_extract(&c->emit, c->f32_type, coords, component);
                    if (component >= moved) { /* the array layer */
                        adjusted[component] = coord;
                        continue;
                    }
                    u32 off = spv_emit_vec4_extract(&c->emit, i32_type, offset, component);
                    u32 size = spv_emit_vec4_extract(&c->emit, i32_type, dimensions, component);
                    u32 delta =
                        spv_emit_fdiv(&c->emit, c->f32_type, spv_emit_convert_s_to_f(&c->emit, c->f32_type, off),
                                      spv_emit_convert_s_to_f(&c->emit, c->f32_type, size));
                    adjusted[component] = spv_emit_fadd(&c->emit, c->f32_type, coord, delta);
                }
                coords = components == 3
                             ? spv_emit_vec3_construct(&c->emit, vec3, adjusted[0], adjusted[1], adjusted[2])
                             : spv_emit_vec2_construct(&c->emit, vec2, adjusted[0], adjusted[1]);
                offset = 0;
                dynamic_offset = 0;
            }
            /* outside the fragment stage there are no derivatives: LOD 0, or the bias as the level */
            u32 sample_mode = mode == 1 || mode == 3 || mode == 7 || stage != ST_FS ? 1
                              : mode == 2 || mode == 6                              ? 2
                                                                                    : 0;
            if (sample_mode == 1 && !level) level = zero_f;
            u32 sample =
                spv_emit_sample_image(&c->emit, shadow ? c->f32_type : texture_vec4[slot], c->sampled_type[slot],
                                      c->sampler[slot], coords, dref, sample_mode, level, offset, dynamic_offset);
            u32 mask = B(w, 31, 4), dst = d;
            if (!mask) mask = 15;
            for (u32 component = 0; component < 4; component++)
                if (mask & (1u << component)) {
                    u32 value =
                        shadow ? spv_emit_bitcast(&c->emit, c->u32_type,
                                                  component == 3 ? spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f)
                                                                 : sample)
                               : texture_component(c, sample, texture_scalar[slot], component);
                    reg_write(c, dst, value);
                    if (dst < 255) dst++;
                }
        } else if (op == O_TXQ || op == O_TXQ_B) {
            int bindless = op == O_TXQ_B;
            u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0, indexed = 0, ldc = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, a, &bank, &off, &bank2, &off2, 0) : 0;
            int slot;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, a, stage, &ldc);
            slot = direct_query_slot(textures, texture_count, off, bindless, bank, handle_count, bank2, off2, indexed,
                                     ldc);
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            u32 dimensions = spv_emit_image_query_size_lod(&c->emit, vec2_u32, c->sampled_type[slot], c->sampler[slot],
                                                           reg_read(c, a + (u32)bindless));
            u32 values[4] = {
                spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 0),
                spv_emit_vec4_extract(&c->emit, c->u32_type, dimensions, 1),
                spv_emit_constant_u32(&c->emit, c->u32_type, 1),
                spv_emit_image_query_levels(&c->emit, c->u32_type, c->sampled_type[slot], c->sampler[slot])};
            u32 dst = d;
            for (u32 k = 0; k < 4; k++)
                if (B(w, 31 + k, 1)) {
                    reg_write(c, dst, values[k]);
                    if (dst < 255) dst++;
                }
        } else if (op == O_TMML) {
            u32 off = B(w, 36, 13);
            int slot = direct_query_slot(textures, texture_count, off, 0, 0, 0, 0, 0, 0, 0);
            u32 x = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 y = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a + 1));
            u32 coords = spv_emit_vec2_construct(&c->emit, vec2, x, y);
            u32 lod = spv_emit_image_query_lod(&c->emit, vec2, c->sampled_type[slot], c->sampler[slot], coords);
            u32 mask = B(w, 31, 4), dst = d;
            if (!mask) mask = 15;
            for (u32 component = 0; component < 4; component++) {
                if (!(mask & (1u << component))) continue;
                u32 value = c->zero;
                if (component < 2) {
                    u32 f = spv_emit_vec4_extract(&c->emit, c->f32_type, lod, 1 - component);
                    f = spv_emit_fmul(&c->emit, c->f32_type, f, spv_emit_constant_f32(&c->emit, c->f32_type, 256.0f));
                    value = spv_emit_bitcast(&c->emit, c->u32_type, spv_emit_convert_f_to_s(&c->emit, i32_type, f));
                }
                reg_write(c, dst, value);
                if (dst < 255) dst++;
            }
        } else if (op == O_TLD4S || op == O_TLD4 || op == O_TLD4_B) {
            u32 indexed = 0, ldc = 0;
            int bindless = op == O_TLD4_B;
            u32 off = B(w, 36, 13), bank = 0, bank2 = 0, off2 = 0;
            int handle_count =
                bindless ? direct_track_image_handle_pair(insn, n, i, b, &bank, &off, &bank2, &off2, 0) : 0;
            if (handle_count == 1) indexed = direct_indexed_handle(insn, n, i, b, stage, &ldc);
            u32 ty = op == O_TLD4S ? 2 : B(w, 28, 3);
            u32 type = ty >> 1, array = ty & 1;
            u32 shadow = B(w, 50, 1);
            int slot = 0;
            while (slot < texture_count &&
                   (textures[slot].cbuf_off != off || textures[slot].type != type || textures[slot].array != array ||
                    textures[slot].shadow != shadow || textures[slot].bindless != bindless ||
                    (bindless &&
                     !direct_bindless_matches(&textures[slot], bank, handle_count, bank2, off2, indexed, ldc))))
                slot++;
            if (textures[slot].flags & SNX_TEX_INDEXED) direct_swap_element(c, slot);
            u32 x = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a + array));
            u32 y = type == 0
                        ? spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f)
                        : spv_emit_bitcast(&c->emit, c->f32_type,
                                           reg_read(c, op == O_TLD4S && !shadow && !B(w, 51, 1) ? b : a + array + 1));
            u32 coords;
            if (!array && type != 3)
                coords = spv_emit_vec2_construct(&c->emit, vec2, x, y);
            else {
                u32 z =
                    type == 3 ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a + array + 2)) : array_layer(c, a);
                if (type == 3 && array) {
                    u32 four[4];
                    four[0] = x;
                    four[1] = y;
                    four[2] = z;
                    four[3] = array_layer(c, a);
                    coords = spv_emit_vec4_construct(&c->emit, vec4, four);
                } else
                    coords = spv_emit_vec3_construct(&c->emit, vec3, x, y, z);
            }
            u32 component = spv_emit_constant_u32(&c->emit, c->u32_type,
                                                  B(w,
                                                    op == O_TLD4S ? 52
                                                    : bindless    ? 38
                                                                  : 56,
                                                    2));
            u32 offset = 0, dref = 0;
            int per_tap_offset = 0;
            u32 ptp_source = 0;
            if (op != O_TLD4S) {
                u32 source = b + bindless;
                u32 offset_mode = B(w, bindless ? 36 : 54, 2);
                if (offset_mode == 1) {
                    u32 raw = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, source++));
                    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 6);
                    u32 ox = spv_emit_bitfield_sextract(&c->emit, i32_type, raw, c->zero, width);
                    u32 oy = spv_emit_bitfield_sextract(&c->emit, i32_type, raw,
                                                        spv_emit_constant_u32(&c->emit, c->u32_type, 8), width);
                    offset = spv_emit_vec2_construct(&c->emit, vec2_i32, ox, oy);
                } else if (offset_mode == 2) {
                    per_tap_offset = 1;
                    ptp_source = source;
                    source += 2;
                }
                if (shadow) dref = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, source));
            } else {
                if (B(w, 51, 1)) {
                    u32 raw = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, b));
                    u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 6);
                    u32 ox = spv_emit_bitfield_sextract(&c->emit, i32_type, raw, c->zero, width);
                    u32 oy = spv_emit_bitfield_sextract(&c->emit, i32_type, raw,
                                                        spv_emit_constant_u32(&c->emit, c->u32_type, 8), width);
                    offset = spv_emit_vec2_construct(&c->emit, vec2_i32, ox, oy);
                }
                if (shadow) dref = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b + B(w, 51, 1)));
            }
            u32 gathered;
            if (per_tap_offset) {
                u32 taps[4];
                u32 width = spv_emit_constant_u32(&c->emit, c->u32_type, 6);
                /* tap k's offset is (x, y) in bytes 2(k % 2) and 2(k % 2) + 1 of register k / 2, and its texel is the
                   i0j0 one of a gather at that offset, its w */
                for (u32 tap = 0; tap < 4; tap++) {
                    u32 raw = spv_emit_bitcast(&c->emit, i32_type, reg_read(c, ptp_source + tap / 2));
                    u32 xoff = spv_emit_bitfield_sextract(
                        &c->emit, i32_type, raw, spv_emit_constant_u32(&c->emit, c->u32_type, (tap & 1) * 16), width);
                    u32 yoff = spv_emit_bitfield_sextract(
                        &c->emit, i32_type, raw, spv_emit_constant_u32(&c->emit, c->u32_type, (tap & 1) * 16 + 8),
                        width);
                    u32 tap_offset = spv_emit_vec2_construct(&c->emit, vec2_i32, xoff, yoff);
                    u32 sampled = spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                           c->sampler[slot], coords, component, dref, tap_offset);
                    taps[tap] = spv_emit_vec4_extract(&c->emit, texture_scalar[slot], sampled, 3);
                }
                gathered = spv_emit_vec4_construct(&c->emit, texture_vec4[slot], taps);
            } else {
                gathered = spv_emit_image_gather_ex(&c->emit, texture_vec4[slot], c->sampled_type[slot],
                                                    c->sampler[slot], coords, component, dref, offset);
            }
            if (op == O_TLD4S) {
                u32 db = B(w, 28, 8);
                if (B(w, 55, 1)) {
                    u32 xy = spv_emit_vec2_construct(&c->emit, vec2,
                                                     spv_emit_vec4_extract(&c->emit, c->f32_type, gathered, 0),
                                                     spv_emit_vec4_extract(&c->emit, c->f32_type, gathered, 1));
                    u32 zw = spv_emit_vec2_construct(&c->emit, vec2,
                                                     spv_emit_vec4_extract(&c->emit, c->f32_type, gathered, 2),
                                                     spv_emit_vec4_extract(&c->emit, c->f32_type, gathered, 3));
                    reg_write(c, d, spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, xy));
                    reg_write(c, db, spv_emit_glsl_unary(&c->emit, c->u32_type, c->glsl, SPV_GLSL_PACK_HALF2X16, zw));
                } else {
                    u32 regs[4] = {d, d < 254 ? d + 1 : 255, db, db < 254 ? db + 1 : 255};
                    for (u32 k = 0; k < 4; k++)
                        if (regs[k] < 255)
                            reg_write(c, regs[k], texture_component(c, gathered, texture_scalar[slot], k));
                }
            } else {
                u32 mask = B(w, 31, 4), dst = d;
                if (!mask) mask = 15;
                for (u32 k = 0; k < 4; k++)
                    if (mask & (1u << k)) {
                        reg_write(c, dst, texture_component(c, gathered, texture_scalar[slot], k));
                        if (dst < 255) dst++;
                    }
            }
        } else if (op == O_KIL) {
            spv_emit_kill(&c->emit);
        } else if (op == O_EXIT) {
            if (depth_fix && stage != ST_GS) { /* a geometry program's vertices are fixed as it emits them */
                vertex_depth_fix(c, position, push, push_pointer, vec4);
            } else if (stage == ST_FS) {
                u32 reg = 0, output = 0, alpha = zero_f;
                for (u32 rt = 0; rt < 8; rt++) {
                    u32 mask = (target >> (rt * 4)) & 15;
                    if (!mask) continue;
                    u32 components[4];
                    int integer = !!(si->rt_int_mask & (1u << rt));
                    int signed_color = !!(si->rt_sint_mask & (1u << rt));
                    u32 color_type = integer ? signed_color ? vec4_i32 : vec4_u32 : vec4;
                    for (u32 k = 0; k < 4; k++, reg++) {
                        u32 raw = mask & (1u << k) ? reg_read(c, reg) : c->zero;
                        components[k] = integer ? signed_color ? spv_emit_bitcast(&c->emit, i32_type, raw) : raw
                                        : mask & (1u << k) ? spv_emit_bitcast(&c->emit, c->f32_type, raw)
                                                           : zero_f;
                        if (rt == 0 && k == 3) alpha = spv_emit_bitcast(&c->emit, c->f32_type, raw);
                    }
                    u32 color = spv_emit_vec4_construct(&c->emit, color_type, components);
                    spv_emit_store(&c->emit, outputs[output++], color);
                }
                if (depth_output)
                    spv_emit_store(&c->emit, depth_output,
                                   spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, reg + 1)));
                if (push) alpha_test(c, push, push_pointer, alpha);
            }
            spv_emit_return(&c->emit);
        }
        if (guard_merge) {
            if (op != O_EXIT && op != O_KIL) spv_emit_branch(&c->emit, guard_merge);
            spv_emit_label(&c->emit, guard_merge);
        } else if ((op == O_EXIT || op == O_KIL) && !has_flow)
            break;
        u32 next = next_instruction(i, n);
        if (has_flow && (next >= n || block_start[next]) && (guard_merge || (op != O_EXIT && op != O_KIL)))
            spv_emit_branch(&c->emit, dispatch_merge);
    }
    if (has_flow) {
        /* A branch outside the decoded instruction range must terminate. A switch
           default that simply loops would leave a GPU invocation spinning forever. */
        if (converge) { /* lanes ahead of the subgroup's lowest pc wait for this iteration */
            spv_emit_label(&c->emit, wait_label);
            spv_emit_branch(&c->emit, dispatch_merge);
        }
        spv_emit_label(&c->emit, dispatch_default);
        spv_emit_store(&c->emit, pc, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX));
        spv_emit_branch(&c->emit, dispatch_merge);
        spv_emit_label(&c->emit, dispatch_merge);
        u32 next_pc = spv_emit_load(&c->emit, c->u32_type, pc);
        u32 finished =
            spv_emit_i_equal(&c->emit, c->bool_type, next_pc, spv_emit_constant_u32(&c->emit, c->u32_type, UINT32_MAX));
        spv_emit_branch_conditional(&c->emit, finished, loop_merge, loop_continue);
        spv_emit_label(&c->emit, loop_continue);
        spv_emit_branch(&c->emit, loop_header);
        spv_emit_label(&c->emit, loop_merge);
        if (depth_fix && stage != ST_GS) vertex_depth_fix(c, position, push, push_pointer, vec4);
        if (stage == ST_FS && depth_output)
            spv_emit_store(&c->emit, depth_output, spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, out_reg + 1)));
        spv_emit_return(&c->emit);
    }
    spv_emit_function_end(&c->emit);
    u32* result = spv_emit_finish(&c->emit, word_count);
    spv_emit_destroy(&c->emit);
    free(c);
    free(case_labels); /* the literals and block starts too */
    free(pop_targets);
    free(gb_at);
    ShaderInfo next = {0};
    next.stage = stage;
    memcpy(next.cs_local, si->cs_local, sizeof next.cs_local);
    next.shared_size = si->shared_size;
    next.attr_int_mask = si->attr_int_mask;
    next.attr_sint_mask = si->attr_sint_mask;
    next.clip_enable = si->clip_enable;
    next.uses_clip_mask = clip_used;
    next.rt_int_mask = si->rt_int_mask;
    next.rt_sint_mask = si->rt_sint_mask;
    next.tex_int_mask = si->tex_int_mask;
    next.tex_sint_mask = si->tex_sint_mask;
    next.ntex = texture_count;
    next.nimg = image_info.nimg;
    memcpy(next.img, image_info.img, (size_t)image_info.nimg * sizeof image_info.img[0]);
    next.ngbuf = ngbuf;
    next.gbuf_wmask = gbuf_wmask;
    memcpy(next.gbuf_cbuf_off, gbuf_off, (size_t)ngbuf * sizeof gbuf_off[0]);
    next.fp32_flush = fp32_flush;
    next.fp32_preserve = fp32_preserve;
    next.uses_position_in = needs_fragcoord;
    memcpy(next.tex, textures, (size_t)texture_count * sizeof textures[0]);
    for (u32 i = 0; i < 18; i++)
        if (cbuf_used[i]) {
            next.cbuf_mask |= 1u << i;
            next.cbuf_max[i] = cbuf_max[i];
        }
    for (u32 i = 0; i < 32; i++)
        if (inputs_used[i]) next.in_mask |= 1u << i;
    memcpy(next.sph, sph, sizeof sph);
    *si = next;
    return result;
}
