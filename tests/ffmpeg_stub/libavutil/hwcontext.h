/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Minimal test double, NOT an FFmpeg ABI header. Only the nvdec_packets target includes this directory. */
#ifndef SNX_TEST_HWCONTEXT_H
#define SNX_TEST_HWCONTEXT_H
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
enum AVHWDeviceType {
    AV_HWDEVICE_TYPE_NONE   = 0,
    AV_HWDEVICE_TYPE_VULKAN = 13,
};
int av_hwdevice_ctx_create(AVBufferRef** ctx, enum AVHWDeviceType type, const char* name, void* opts, int flags);
int av_hwframe_transfer_data(AVFrame* dst, const AVFrame* src, int flags);
#endif
