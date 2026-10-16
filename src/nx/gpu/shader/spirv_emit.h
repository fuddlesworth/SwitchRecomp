/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A C SPIR-V writer. Instructions append words directly to their logical sections;
   spv_emit_finish orders the sections into a Vulkan shader module. */
#ifndef SNX_SPIRV_EMIT_H
#define SNX_SPIRV_EMIT_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t* words;
    size_t count, capacity;
} SpvWords;

typedef struct {
    uint32_t storage, pointee, id;
} SpvPointerType;

typedef struct {
    SpvWords capabilities, extensions, imports, entry_points, execution_modes, debug, decorations, types, functions;
    uint32_t next_id;
    SpvPointerType* pointer_types;
    size_t pointer_count, pointer_capacity;
    uint32_t sampled_image_types[96], image_types[96], sampled_image_pointee[3];
    uint32_t storage_image_keys[16][4], storage_image_types[16], storage_image_count; /* one OpTypeImage each */
    uint32_t* nonuniform; /* ids decorated NonUniform: texture array elements and what is made from them */
    size_t nonuniform_count, nonuniform_capacity;
    int texture_arrays; /* their capabilities declared */
} SpvEmitter;

typedef enum {
    SPV_STAGE_VERTEX = 0,
    SPV_STAGE_TESS_CONTROL = 1,
    SPV_STAGE_TESS_EVALUATION = 2,
    SPV_STAGE_GEOMETRY = 3,
    SPV_STAGE_FRAGMENT = 4,
    SPV_STAGE_COMPUTE = 5
} SpvShaderStage;
typedef enum {
    SPV_GLSL_ROUND_EVEN = 2,
    SPV_GLSL_TRUNC = 3,
    SPV_GLSL_FABS = 4,
    SPV_GLSL_FLOOR = 8,
    SPV_GLSL_CEIL = 9,
    SPV_GLSL_COS = 14,
    SPV_GLSL_SIN = 13,
    SPV_GLSL_EXP2 = 29,
    SPV_GLSL_LOG2 = 30,
    SPV_GLSL_SQRT = 31,
    SPV_GLSL_INVERSE_SQRT = 32,
    SPV_GLSL_FMIN = 37,
    SPV_GLSL_FMAX = 40,
    SPV_GLSL_FCLAMP = 43,
    SPV_GLSL_FMA = 50,
    SPV_GLSL_PACK_HALF2X16 = 58,
    SPV_GLSL_UNPACK_HALF2X16 = 62
} SpvGlsl450Op;

void spv_emit_init(SpvEmitter* e, size_t estimated_function_words);
void spv_emit_destroy(SpvEmitter* e);
uint32_t spv_emit_id(SpvEmitter* e);
uint32_t spv_emit_glsl450_import(SpvEmitter* e);
void spv_emit_shader_capability(SpvEmitter* e);
void spv_emit_float16_capability(SpvEmitter* e);
void spv_emit_float64_capability(SpvEmitter* e);
void spv_emit_draw_parameters_capability(SpvEmitter* e);
void spv_emit_layer_capability(SpvEmitter* e);
void spv_emit_clip_distance_capability(SpvEmitter* e);
uint32_t spv_emit_layer_output(SpvEmitter* e, uint32_t i32_type);
uint32_t spv_emit_clip_distance_output(SpvEmitter* e, uint32_t f32_type, uint32_t u32_type, uint32_t length);
uint32_t spv_emit_output_element_pointer(SpvEmitter* e, uint32_t f32_type, uint32_t array, uint32_t index);
void spv_emit_image_query_capability(SpvEmitter* e);
void spv_emit_subgroup_capabilities(SpvEmitter* e);
void spv_emit_image_gather_extended_capability(SpvEmitter* e);
void spv_emit_entry_str(SpvEmitter* e, SpvShaderStage stage, uint32_t function, const char* name,
                        const uint32_t* interfaces, size_t interface_count);
void spv_emit_compute_entry_str(SpvEmitter* e, uint32_t function, const char* name, const uint32_t* interfaces,
                                size_t interface_count);
void spv_emit_fragment_origin_upper_left(SpvEmitter* e, uint32_t function);
void spv_emit_fragment_depth_replacing(SpvEmitter* e, uint32_t function);
void spv_emit_float_controls(SpvEmitter* e, uint32_t function, int flush_denorms, int preserve_infnan);
void spv_emit_local_size(SpvEmitter* e, uint32_t function, uint32_t x, uint32_t y, uint32_t z);
void spv_emit_name_str(SpvEmitter* e, uint32_t id, const char* name);
uint32_t spv_emit_type_void(SpvEmitter* e);
uint32_t spv_emit_type_bool(SpvEmitter* e);
uint32_t spv_emit_type_u32(SpvEmitter* e);
uint32_t spv_emit_type_i32(SpvEmitter* e);
uint32_t spv_emit_convert_f_to_s(SpvEmitter* e, uint32_t i32_type, uint32_t value);
uint32_t spv_emit_convert_f_to_u(SpvEmitter* e, uint32_t u32_type, uint32_t value);
uint32_t spv_emit_convert_s_to_f(SpvEmitter* e, uint32_t f32_type, uint32_t value);
uint32_t spv_emit_convert_u_to_f(SpvEmitter* e, uint32_t f32_type, uint32_t value);
uint32_t spv_emit_convert_f_to_f(SpvEmitter* e, uint32_t result_type, uint32_t value);
uint32_t spv_emit_type_f16(SpvEmitter* e);
uint32_t spv_emit_type_f32(SpvEmitter* e);
uint32_t spv_emit_type_f64(SpvEmitter* e);
uint32_t spv_emit_type_vec4(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_vec2(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_vec3(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_runtime_array(SpvEmitter* e, uint32_t element_type, uint32_t stride);
uint32_t spv_emit_type_struct_one(SpvEmitter* e, uint32_t member_type);
uint32_t spv_emit_type_storage_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_function_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_workgroup_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_array_u32(SpvEmitter* e, uint32_t u32_type, uint32_t length);
uint32_t spv_emit_workgroup_variable(SpvEmitter* e, uint32_t pointer_type);
uint32_t spv_emit_function_element_pointer(SpvEmitter* e, uint32_t pointer_type, uint32_t array, uint32_t index);
uint32_t spv_emit_type_function_void(SpvEmitter* e, uint32_t void_type);
uint32_t spv_emit_constant_u32(SpvEmitter* e, uint32_t type, uint32_t value);
uint32_t spv_emit_constant_bool(SpvEmitter* e, uint32_t type, int value);
uint32_t spv_emit_constant_f32(SpvEmitter* e, uint32_t type, float value);
uint32_t spv_emit_constant_vec4(SpvEmitter* e, uint32_t type, const uint32_t components[4]);
uint32_t spv_emit_constant_vec2(SpvEmitter* e, uint32_t type, uint32_t x, uint32_t y);
uint32_t spv_emit_constant_vec3(SpvEmitter* e, uint32_t type, uint32_t x, uint32_t y, uint32_t z);
uint32_t spv_emit_storage_buffer(SpvEmitter* e, uint32_t struct_type, uint32_t set, uint32_t binding);
uint32_t spv_emit_uniform_vec4_array(SpvEmitter* e, uint32_t vec4_type, uint32_t component_type, uint32_t u32_type,
                                     uint32_t set, uint32_t binding, uint32_t elements,
                                     uint32_t* component_pointer_type);
uint32_t spv_emit_uniform_component(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member,
                                    uint32_t element, uint32_t component, uint32_t component_type);
uint32_t spv_emit_push_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t f32_type, uint32_t offset,
                            uint32_t* component_pointer_type);
uint32_t spv_emit_push_component_f32(SpvEmitter* e, uint32_t pointer_type, uint32_t block, uint32_t member,
                                     uint32_t component, uint32_t f32_type);
uint32_t spv_emit_output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location);
uint32_t spv_emit_position_output(SpvEmitter* e, uint32_t vec4_type);
uint32_t spv_emit_point_size_output(SpvEmitter* e, uint32_t f32_type);
uint32_t spv_emit_frag_depth_output(SpvEmitter* e, uint32_t f32_type);
/* interpolation: 0 perspective, 1 flat, 2 no perspective */
uint32_t spv_emit_input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location, int interpolation);
uint32_t spv_emit_fragcoord_input(SpvEmitter* e, uint32_t vec4_type);
uint32_t spv_emit_builtin_input_vec3(SpvEmitter* e, uint32_t vec3_type, uint32_t builtin);
uint32_t spv_emit_builtin_input_u32(SpvEmitter* e, uint32_t u32_type, uint32_t builtin);
void spv_emit_flat(SpvEmitter* e, uint32_t variable);
void spv_emit_capability(SpvEmitter* e, uint32_t capability);
/* An Input (1) or Output (3) variable of type, or of an array of count of it when count isn't 0, decorated BuiltIn
   builtin, or Location location when builtin is SPV_NOT_BUILTIN, and Patch when patch is set */
#define SPV_NOT_BUILTIN 0xffffffffu
uint32_t spv_emit_interface_variable(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t u32_type, uint32_t count,
                                     uint32_t builtin, uint32_t location, int patch);
/* a pointer to one scalar of an interface variable: its element (an id, or 0 when the variable isn't an array), then
   its component (an id) */
uint32_t spv_emit_interface_scalar_pointer(SpvEmitter* e, uint32_t storage, uint32_t scalar_type, uint32_t variable,
                                           uint32_t element, uint32_t component);
void spv_emit_execution_mode(SpvEmitter* e, uint32_t function, uint32_t mode, int has_operand, uint32_t operand);
void spv_emit_emit_vertex(SpvEmitter* e);
void spv_emit_end_primitive(SpvEmitter* e);
/* a tessellation control barrier: the patch's invocations, and its outputs written before it */
void spv_emit_patch_barrier(SpvEmitter* e, uint32_t u32_type);
uint32_t spv_emit_sampler_image_array(SpvEmitter* e, uint32_t scalar_type, uint32_t u32_type, uint32_t set,
                                      uint32_t binding, uint32_t type, int array, int shadow, uint32_t count,
                                      uint32_t* sampled_type);
uint32_t spv_emit_sampler_array_element(SpvEmitter* e, uint32_t sampled_type, uint32_t array_variable, uint32_t index);
uint32_t spv_emit_sampler2d(SpvEmitter* e, uint32_t f32_type, uint32_t set, uint32_t binding, int shadow,
                            uint32_t* sampled_type);
uint32_t spv_emit_sampler_image(SpvEmitter* e, uint32_t f32_type, uint32_t set, uint32_t binding, uint32_t type,
                                int array, int shadow, int multisample, uint32_t* sampled_type);
uint32_t spv_emit_storage_image(SpvEmitter* e, uint32_t scalar_type, uint32_t set, uint32_t binding, uint32_t type,
                                int array, uint32_t format, uint32_t* image_type);
void spv_emit_image_write(SpvEmitter* e, uint32_t image_type, uint32_t image_variable, uint32_t coordinates,
                          uint32_t texel);
uint32_t spv_emit_image_read(SpvEmitter* e, uint32_t result_type, uint32_t image_type, uint32_t image_variable,
                             uint32_t coordinates);
uint32_t spv_emit_image_texel_pointer(SpvEmitter* e, uint32_t scalar_type, uint32_t image_variable,
                                      uint32_t coordinates, uint32_t sample);
uint32_t spv_emit_vec3_construct(SpvEmitter* e, uint32_t vec3_type, uint32_t x, uint32_t y, uint32_t z);
void spv_emit_function_void(SpvEmitter* e, uint32_t void_type, uint32_t function, uint32_t function_type);
void spv_emit_label(SpvEmitter* e, uint32_t label);
uint32_t spv_emit_function_variable(SpvEmitter* e, uint32_t pointer_type);
uint32_t spv_emit_function_variable_init(SpvEmitter* e, uint32_t pointer_type, uint32_t initial_value);
uint32_t spv_emit_load(SpvEmitter* e, uint32_t result_type, uint32_t pointer);
uint32_t spv_emit_bitcast(SpvEmitter* e, uint32_t result_type, uint32_t value);
uint32_t spv_emit_fadd(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_fmul(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_i_equal(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_i_not_equal(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_u_less(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_s_greater(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_isub_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_shift_left_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t shift);
uint32_t spv_emit_shift_right_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t shift);
uint32_t spv_emit_shift_right_s32(SpvEmitter* e, uint32_t i32_type, uint32_t value, uint32_t shift);
uint32_t spv_emit_sdiv_i32(SpvEmitter* e, uint32_t i32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_bitwise_and_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_bitwise_or_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_bitwise_xor_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_not_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value);
uint32_t spv_emit_bitfield_uextract(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t offset, uint32_t count);
uint32_t spv_emit_bit_reverse_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value);
uint32_t spv_emit_atomic_u32(SpvEmitter* e, uint32_t u32_type, uint32_t opcode, uint32_t pointer, uint32_t value);
uint32_t spv_emit_atomic_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t opcode, uint32_t pointer,
                                    uint32_t value);
uint32_t spv_emit_atomic_load_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer);
uint32_t spv_emit_atomic_load_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer);
uint32_t spv_emit_atomic_compare_exchange_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer,
                                                     uint32_t desired, uint32_t expected);
uint32_t spv_emit_atomic_compare_exchange_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer, uint32_t desired,
                                              uint32_t expected);
void spv_emit_control_barrier(SpvEmitter* e, uint32_t u32_type);
void spv_emit_memory_barrier(SpvEmitter* e, uint32_t u32_type, uint32_t scope, uint32_t semantics);
uint32_t spv_emit_bitfield_insert(SpvEmitter* e, uint32_t u32_type, uint32_t base, uint32_t insert, uint32_t offset,
                                  uint32_t count);
uint32_t spv_emit_bit_count(SpvEmitter* e, uint32_t u32_type, uint32_t value);
uint32_t spv_emit_bitfield_sextract(SpvEmitter* e, uint32_t i32_type, uint32_t value, uint32_t offset, uint32_t count);
uint32_t spv_emit_fcompare(SpvEmitter* e, uint32_t bool_type, uint32_t condition, uint32_t left, uint32_t right);
uint32_t spv_emit_bool_not(SpvEmitter* e, uint32_t bool_type, uint32_t value);
uint32_t spv_emit_bool_binary(SpvEmitter* e, uint32_t bool_type, uint32_t op, uint32_t left, uint32_t right);
uint32_t spv_emit_fdiv(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_fnegate(SpvEmitter* e, uint32_t f32_type, uint32_t value);
uint32_t spv_emit_derivative(SpvEmitter* e, uint32_t f32_type, uint32_t value, int y_axis);
uint32_t spv_emit_glsl_unary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t value);
uint32_t spv_emit_glsl_binary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t left,
                              uint32_t right);
uint32_t spv_emit_glsl_ternary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t a,
                               uint32_t b, uint32_t c);
uint32_t spv_emit_isnan(SpvEmitter* e, uint32_t bool_type, uint32_t value);
uint32_t spv_emit_select(SpvEmitter* e, uint32_t result_type, uint32_t condition, uint32_t yes, uint32_t no);
uint32_t spv_emit_vec4_extract(SpvEmitter* e, uint32_t component_type, uint32_t vector, uint32_t component);
uint32_t spv_emit_subgroup_ballot(SpvEmitter* e, uint32_t vec4_type, uint32_t u32_type, uint32_t predicate);
uint32_t spv_emit_subgroup_umin_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value);
void spv_emit_subgroup_arithmetic_capability(SpvEmitter* e);
uint32_t spv_emit_subgroup_shuffle_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t lane);
uint32_t spv_emit_vec4_insert(SpvEmitter* e, uint32_t vec4_type, uint32_t vector, uint32_t component_value,
                              uint32_t component);
uint32_t spv_emit_vec4_construct(SpvEmitter* e, uint32_t vec4_type, const uint32_t components[4]);
uint32_t spv_emit_vec2_construct(SpvEmitter* e, uint32_t vec2_type, uint32_t x, uint32_t y);
uint32_t spv_emit_sample2d(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                           uint32_t coordinates, uint32_t depth_reference);
uint32_t spv_emit_sample2d_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                               uint32_t coordinates, uint32_t depth_reference, uint32_t lod);
uint32_t spv_emit_sample_lod_offset(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                    uint32_t coordinates, uint32_t lod, uint32_t offset, int dynamic_offset);
uint32_t spv_emit_image_query_size_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                       uint32_t lod);
uint32_t spv_emit_image_query_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates);
uint32_t spv_emit_image_query_levels(SpvEmitter* e, uint32_t u32_type, uint32_t sampled_type, uint32_t sampler);
uint32_t spv_emit_image_fetch(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                              uint32_t coordinates, uint32_t lod);
uint32_t spv_emit_image_fetch_sample(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                     uint32_t coordinates, uint32_t sample);
uint32_t spv_emit_image_gather(SpvEmitter* e, uint32_t vec4_type, uint32_t sampled_type, uint32_t sampler,
                               uint32_t coordinates, uint32_t component);
uint32_t spv_emit_image_gather_ex(SpvEmitter* e, uint32_t vec4_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates, uint32_t component, uint32_t dref, uint32_t offset);
uint32_t spv_emit_sample2d_offset(SpvEmitter* e, uint32_t vec4_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates, uint32_t offset, int dynamic_offset);
uint32_t spv_emit_sample2d_bias(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                uint32_t coordinates, uint32_t bias);
/* mode: 0 implicit LOD, 1 explicit LOD, 2 bias. Optional depth and texel offset. */
uint32_t spv_emit_sample_image(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                               uint32_t coordinates, uint32_t depth_reference, uint32_t mode, uint32_t level,
                               uint32_t offset, int dynamic_offset);
void spv_emit_no_contraction(SpvEmitter* e, uint32_t result);
uint32_t spv_emit_storage_access_u32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member_index,
                                     uint32_t element_index);
uint32_t spv_emit_iadd_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_imul_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_umul_high_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
void spv_emit_store_u32(SpvEmitter* e, uint32_t pointer, uint32_t value);
void spv_emit_store(SpvEmitter* e, uint32_t pointer, uint32_t value);
void spv_emit_return(SpvEmitter* e);
void spv_emit_kill(SpvEmitter* e);
void spv_emit_selection_merge(SpvEmitter* e, uint32_t merge_label);
void spv_emit_branch_conditional(SpvEmitter* e, uint32_t condition, uint32_t yes_label, uint32_t no_label);
void spv_emit_branch(SpvEmitter* e, uint32_t label);
void spv_emit_loop_merge(SpvEmitter* e, uint32_t merge_label, uint32_t continue_label);
void spv_emit_switch_u32(SpvEmitter* e, uint32_t selector, uint32_t default_label, const uint32_t* literals,
                         const uint32_t* labels, size_t count);
void spv_emit_function_end(SpvEmitter* e);
/* The caller owns the returned words. A module is assembled once after all functions are emitted. */
uint32_t* spv_emit_finish(const SpvEmitter* e, size_t* word_count);
#endif
