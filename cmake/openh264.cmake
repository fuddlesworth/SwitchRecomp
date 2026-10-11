# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# OpenH264's decoder as a static library (target openh264), built from the source list of its Makefile
# (codec/common/targets.mk and codec/decoder/targets.mk), with its x86-64 assembly when NASM is there. The runtime only
# decodes, so the encoder and the processing library are left out.
set(src "${openh264_SOURCE_DIR}")
set(common common_tables copy_mb cpu crt_util_safe_x deblocking_common expand_pic intra_pred_common mc memory_align
           sad_common utils welsCodecTrace WelsTaskThread WelsThread WelsThreadLib WelsThreadPool)
set(decoder au_parser bit_stream cabac_decoder deblocking decode_mb_aux decode_slice decoder decoder_core
            decoder_data_tables error_concealment fmo get_intra_predictor manage_dec_ref memmgr_nal_unit mv_pred
            parse_mb_syn_cabac parse_mb_syn_cavlc pic_queue rec_mb wels_decoder_thread)
list(TRANSFORM common PREPEND "${src}/codec/common/src/")
list(TRANSFORM common APPEND .cpp)
list(TRANSFORM decoder PREPEND "${src}/codec/decoder/core/src/")
list(TRANSFORM decoder APPEND .cpp)

add_library(openh264 STATIC ${common} ${decoder} "${src}/codec/decoder/plus/src/welsDecoderExt.cpp")
# The API headers are included as "wels/codec_api.h". BEFORE and not SYSTEM, so a system OpenH264 found through an
# -I in CFLAGS can't stand in for them (cmake/dependencies.cmake says why)
target_include_directories(openh264 BEFORE PUBLIC "${src}/codec/api")
target_include_directories(openh264 PRIVATE "${src}/codec/api/wels" "${src}/codec/common/inc"
                                            "${src}/codec/decoder/core/inc" "${src}/codec/decoder/plus/inc")
target_compile_definitions(openh264 PRIVATE NDEBUG)
target_compile_options(openh264 PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-w;-fno-strict-aliasing>"
                                        "$<$<COMPILE_LANGUAGE:ASM_NASM>:-w-all>")
# C++11 like its Makefile: later standards reject code it still has (register)
set_target_properties(openh264 PROPERTIES CXX_STANDARD 11 CXX_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)

# The assembly is NASM for x86-64. Without NASM the C versions of the same functions are used, which work everywhere
# but decode more slowly
include(CheckLanguage)
check_language(ASM_NASM)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND CMAKE_ASM_NASM_COMPILER)
    enable_language(ASM_NASM)
    set(asm cpuid dct deblock expand_picture intra_pred_com mb_copy mc_chroma mc_luma satd_sad vaa)
    list(TRANSFORM asm PREPEND "${src}/codec/common/x86/")
    list(TRANSFORM asm APPEND .asm)
    list(APPEND asm "${src}/codec/decoder/core/x86/dct.asm" "${src}/codec/decoder/core/x86/intra_pred.asm")
    target_sources(openh264 PRIVATE ${asm})
    target_compile_definitions(openh264 PRIVATE X86_ASM HAVE_AVX2)
    if(WIN32)
        set(platform WIN64)
    else()
        set(platform UNIX64)
    endif()
    # CMake picks the object format (elf64, win64) from the target; the defines say which calling convention
    set_source_files_properties(${asm} PROPERTIES COMPILE_OPTIONS "-I${src}/codec/common/x86/;-D${platform};-DHAVE_AVX2")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(WARNING "NASM was not found: OpenH264 is built without its assembly and decodes movies more slowly")
endif()
