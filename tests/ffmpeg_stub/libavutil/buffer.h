/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Minimal test double, NOT an FFmpeg ABI header. Only the nvdec_packets target includes this directory. */
#ifndef SNX_TEST_BUFFER_H
#define SNX_TEST_BUFFER_H
typedef struct AVBufferRef {
    void* data;
} AVBufferRef;
AVBufferRef* av_buffer_ref(const AVBufferRef* b);
void av_buffer_unref(AVBufferRef** b);
#endif
