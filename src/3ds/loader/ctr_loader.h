/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_LOADER_H
#define SNX_CTR_LOADER_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    SNX_CTR_EXTRACT_OK,
    SNX_CTR_EXTRACT_IO,
    SNX_CTR_EXTRACT_FORMAT,
    SNX_CTR_EXTRACT_UNSUPPORTED,
    SNX_CTR_EXTRACT_LIMIT,
    SNX_CTR_EXTRACT_EXISTS
} SnxCtrExtractResult;
/* Decrypted application CIAs only. RSA/ECDSA trust and encrypted layers are not
   implemented. Hash checks are integrity checks, NOT signature authentication.
   New output is published only after complete extraction. NULL
   destination selects <work>/<title-id>-<full-input-sha256>, work/3ds when work
   is NULL. Existing outputs are reused only with a matching completed format-1
   manifest, intact code hash and extraction directories; otherwise refused. Parent
   directory and input must be trusted and not concurrently modified. published
   is owned by caller on OK. */
SnxCtrExtractResult snx_ctr_extract(const char* input, const char* destination, const char* work, char** published,
                                    char* error, size_t error_size);
/* ExeFS backward LZSS/BLZ, exact bounded expansion; output_size is authoritative.
   The input and output buffers must not overlap. */
bool snx_ctr_blz(const unsigned char* input, size_t input_size, unsigned char* output, size_t output_size);
#endif
