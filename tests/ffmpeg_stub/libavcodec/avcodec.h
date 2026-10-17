/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Minimal test double, NOT an FFmpeg ABI header. Only the nvdec_packets target includes this directory. */
#ifndef SNX_TEST_AVCODEC_H
#define SNX_TEST_AVCODEC_H
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define AVERROR(e)                   (-(e))
#define AVERROR_INVALIDDATA          (-1094995529)
#define AVERROR_EOF                  (-541478725)
#define AV_ERROR_MAX_STRING_SIZE     64
#define AV_LOG_WARNING               24
#define AV_CODEC_ID_H264             27
#define AV_CODEC_ID_VP9              167
#define FF_THREAD_SLICE              2
enum AVPixelFormat {
    AV_PIX_FMT_NONE     = -1,
    AV_PIX_FMT_YUV420P  = 0,
    AV_PIX_FMT_YUVJ420P = 12,
    AV_PIX_FMT_NV12     = 23,
    AV_PIX_FMT_VULKAN   = 199,
};
typedef struct AVCodec {
    int unused;
} AVCodec;
typedef struct AVPacket {
    uint8_t* data;
    int size;
    void* buf;
} AVPacket;
typedef struct AVFrame {
    uint8_t* data[8];
    int linesize[8];
    int width, height, format;
    void* buf[8];
} AVFrame;
typedef struct AVCodecContext {
    void* opaque;
    int thread_count, thread_type;
    int (*get_buffer2)(struct AVCodecContext*, AVFrame*, int);
    void* hw_device_ctx; /* the double never makes one: the hardware attempt lands on the CPU path */
    enum AVPixelFormat (*get_format)(struct AVCodecContext*, const enum AVPixelFormat*);
} AVCodecContext;
const AVCodec* avcodec_find_decoder(int id);
AVCodecContext* avcodec_alloc_context3(const AVCodec* codec);
int avcodec_open2(AVCodecContext* c, const AVCodec* codec, void* options);
void avcodec_free_context(AVCodecContext** c);
int avcodec_default_get_buffer2(AVCodecContext* c, AVFrame* f, int flags);
int avcodec_send_packet(AVCodecContext* c, const AVPacket* p);
int avcodec_receive_frame(AVCodecContext* c, AVFrame* f);
AVPacket* av_packet_alloc(void);
void av_packet_free(AVPacket** p);
void av_packet_unref(AVPacket* p);
int av_new_packet(AVPacket* p, int size);
void av_shrink_packet(AVPacket* p, int size);
AVFrame* av_frame_alloc(void);
void av_frame_free(AVFrame** f);
void av_frame_unref(AVFrame* f);
int av_frame_ref(AVFrame* dst, const AVFrame* src);
const char* av_version_info(void);
int av_strerror(int err, char* dst, size_t size);
int av_log_get_level(void);
void av_log_set_level(int level);
void av_log_set_callback(void (*cb)(void*, int, const char*, va_list));
int av_log_format_line2(void* c, int level, const char* fmt, va_list args, char* line, int size, int* prefix);
#endif
