/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* F7 recorder. H.264/mp4 through Media Foundation (uses QuickSync/NVENC/VCE when present).
   Frames arrive as NV12 already converted on the GPU; we just copy them into an MF buffer and
   a separate thread does the encoding so the game thread never blocks on it. */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <stdio.h>
#include "core/rt.h"

/* big queue, never drop. Dropped frames left holes that some players refused to play
   (laggy parts push several repeated frames at once) */
#define RQ 64
DEFINE_GUID(SWR_MF_TRANSCODE_CONTAINERTYPE, 0x150ff23f, 0x4abc, 0x478b, 0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca);
DEFINE_GUID(SWR_MFTranscodeContainerType_FMPEG4, 0x9ba876f1, 0x419f, 0x4b77, 0xa1, 0xe0, 0x35, 0x95, 0x9d, 0x9d, 0x40,
            0x04);
static IMFSample* g_q[RQ];
static volatile LONG g_qh, g_qt;
static HANDLE g_th, g_ev, g_ready_ev, g_space_ev;
static volatile int g_run, g_ok;
static int g_w, g_h, g_fps;
static u32 g_bitrate;
static wchar_t g_path[MAX_PATH];
static u64 g_frames_in, g_frames_drop;

static int q_count(void) { return (int)(g_qh - g_qt); }

static DWORD WINAPI enc_thread(LPVOID arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMFSinkWriter* wr = NULL;
    IMFMediaType* out = NULL;
    IMFMediaType* in = NULL;
    IMFAttributes* at = NULL;
    DWORD si = 0;
    HRESULT hr;
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&at, 3);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(at, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(at, &MF_SINK_WRITER_DISABLE_THROTTLING, FALSE);
    /* fragmented mp4 (win10+) so the file is still valid if we crash mid-recording */
    if (SUCCEEDED(hr) && !getenv("SWR_REC_MP4_CLASICO")) {
        IMFAttributes_SetGUID(at, &SWR_MF_TRANSCODE_CONTAINERTYPE, &SWR_MFTranscodeContainerType_FMPEG4);
        hr = MFCreateSinkWriterFromURL(g_path, NULL, at, &wr);
        if (FAILED(hr)) {
            IMFAttributes_DeleteItem(at, &SWR_MF_TRANSCODE_CONTAINERTYPE);
            hr = S_OK;
            wr = NULL;
        } else
            LOG("recorder: fragmented mp4 (not corrupted if the game closes)\n");
    }
    if (SUCCEEDED(hr) && !wr) hr = MFCreateSinkWriterFromURL(g_path, NULL, at, &wr);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
        IMFMediaType_SetUINT32(out, &MF_MT_AVG_BITRATE, g_bitrate);
        IMFMediaType_SetUINT32(out, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        IMFMediaType_SetUINT32(out, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        IMFMediaType_SetUINT32(out, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        IMFMediaType_SetUINT32(out, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        IMFMediaType_SetUINT32(out, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        IMFMediaType_SetUINT32(out, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        IMFMediaType_SetUINT64(out, &MF_MT_FRAME_SIZE, ((UINT64)g_w << 32) | (UINT32)g_h);
        IMFMediaType_SetUINT64(out, &MF_MT_FRAME_RATE, ((UINT64)g_fps << 32) | 1);
        IMFMediaType_SetUINT64(out, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
        hr = IMFSinkWriter_AddStream(wr, out, &si);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
        IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        IMFMediaType_SetUINT32(in, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        IMFMediaType_SetUINT32(in, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        IMFMediaType_SetUINT32(in, &MF_MT_DEFAULT_STRIDE, (UINT32)g_w);
        IMFMediaType_SetUINT64(in, &MF_MT_FRAME_SIZE, ((UINT64)g_w << 32) | (UINT32)g_h);
        IMFMediaType_SetUINT64(in, &MF_MT_FRAME_RATE, ((UINT64)g_fps << 32) | 1);
        IMFMediaType_SetUINT64(in, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
        hr = IMFSinkWriter_SetInputMediaType(wr, si, in, NULL);
    }
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_BeginWriting(wr);
    if (in) IMFMediaType_Release(in);
    if (out) IMFMediaType_Release(out);
    if (at) IMFAttributes_Release(at);
    if (FAILED(hr)) {
        WARN("recorder: Windows could not open the H.264 encoder (error %08lx)\n", (unsigned long)hr);
        if (wr) IMFSinkWriter_Release(wr);
        MFShutdown();
        CoUninitialize();
        g_ok = 0;
        SetEvent(g_ready_ev);
        return 0;
    }
    g_ok = 1;
    SetEvent(g_ready_ev);
    for (;;) {
        while (q_count() > 0) {
            IMFSample* s = g_q[g_qt % RQ];
            InterlockedIncrement(&g_qt);
            SetEvent(g_space_ev);
            hr = IMFSinkWriter_WriteSample(wr, si, s);
            IMFSample_Release(s);
            if (FAILED(hr)) {
                static int said;
                if (said < 5) {
                    said++;
                    WARN("recorder: failed to encode a frame (error %08lx)\n", (unsigned long)hr);
                }
            }
        }
        if (!g_run) break;
        WaitForSingleObject(g_ev, 100);
    }
    hr = IMFSinkWriter_Finalize(wr);
    if (FAILED(hr)) WARN("recorder: could not finalize the .mp4 (error %08lx)\n", (unsigned long)hr);
    IMFSinkWriter_Release(wr);
    MFShutdown();
    CoUninitialize();
    return 0;
}

static FILE* g_raw; /* SWR_REC_RAW=1 dumps raw NV12, handy to test capture without MF */
int rec_open(const wchar_t* path, int w, int h, int fps) {
    if (g_th || g_raw) return 0;
    g_w = w;
    g_h = h;
    g_fps = fps;
    wcsncpy(g_path, path, MAX_PATH - 1);
    if (getenv("SWR_REC_RAW")) {
        wchar_t p2[MAX_PATH];
        _snwprintf(p2, MAX_PATH, L"%ls.nv12", path);
        g_raw = _wfopen(p2, L"wb");
        g_frames_in = 0;
        return g_raw != NULL;
    }
    /* ~0.12 bpp, 1080p60 ends up around 15 Mb/s */
    double br = (double)w * h * fps * 0.12;
    if (br < 8e6) br = 8e6;
    if (br > 40e6) br = 40e6;
    g_bitrate = (u32)br;
    g_qh = g_qt = 0;
    g_frames_in = g_frames_drop = 0;
    g_run = 1;
    g_ok = 0;
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ready_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_space_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_th = CreateThread(NULL, 0, enc_thread, NULL, 0, NULL);
    if (!g_th) {
        CloseHandle(g_ev);
        CloseHandle(g_ready_ev);
        return 0;
    }
    WaitForSingleObject(g_ready_ev, 10000);
    if (!g_ok) {
        g_run = 0;
        WaitForSingleObject(g_th, 5000);
        CloseHandle(g_th);
        g_th = NULL;
        CloseHandle(g_ev);
        CloseHandle(g_ready_ev);
        DeleteFileW(g_path);
        return 0;
    }
    LOG("recorder: recording %dx%d at %d FPS, %.0f Mb/s\n", w, h, fps, g_bitrate / 1e6);
    return 1;
}

/* NV12 frame: Y plane then interleaved UV at half res. t is in 100ns units */
void rec_frame(const u8* nv12, i64 t100, i64 dur100) {
    if (g_raw) {
        fwrite(nv12, 1, (size_t)g_w * g_h * 3 / 2, g_raw);
        g_frames_in++;
        return;
    }
    if (!g_th || !g_ok) return;
    g_frames_in++;
    /* encoder can't keep up: wait. smooth mode already slows the game down, so this is rare */
    {
        DWORD t0 = GetTickCount();
        while (q_count() >= RQ && g_run) {
            WaitForSingleObject(g_space_ev, 50);
            if (GetTickCount() - t0 > 20000) {
                g_frames_drop++;
                return;
            }
        }
    }
    DWORD n = (DWORD)((u64)g_w * g_h * 3 / 2);
    IMFMediaBuffer* b = NULL;
    IMFSample* s = NULL;
    BYTE* p = NULL;
    if (FAILED(MFCreateMemoryBuffer(n, &b))) return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
        memcpy(p, nv12, n);
        IMFMediaBuffer_Unlock(b);
    }
    IMFMediaBuffer_SetCurrentLength(b, n);
    if (FAILED(MFCreateSample(&s))) {
        IMFMediaBuffer_Release(b);
        return;
    }
    IMFSample_AddBuffer(s, b);
    IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, t100);
    IMFSample_SetSampleDuration(s, dur100);
    g_q[g_qh % RQ] = s;
    InterlockedIncrement(&g_qh);
    SetEvent(g_ev);
}

void rec_close(void) {
    if (g_raw) {
        fclose(g_raw);
        g_raw = NULL;
        LOG("recorder: %llu raw frames\n", (unsigned long long)g_frames_in);
        return;
    }
    if (!g_th) return;
    g_run = 0;
    SetEvent(g_ev);
    WaitForSingleObject(g_th, INFINITE);
    CloseHandle(g_th);
    g_th = NULL;
    CloseHandle(g_ev);
    CloseHandle(g_ready_ev);
    CloseHandle(g_space_ev);
    LOG("recorder: saved %ls (%llu frames, %llu dropped)\n", g_path, (unsigned long long)g_frames_in,
        (unsigned long long)g_frames_drop);
}
