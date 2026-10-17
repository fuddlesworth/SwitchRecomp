/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the platform layer: threads, locks, time, virtual memory, files, libraries (plat_posix.c or plat_win32.c), logging
   and fatal errors (plat.c) */
#ifndef SNX_PLAT_H
#define SNX_PLAT_H
#include "common/cpu/aarch64/cpu.h"
#include "common/filesystem/host.h"
#include <stddef.h>
#include <stdio.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX /* windows.h's min and max macros would break any later min( or max( in the runtime */
#endif
#include <windows.h>
typedef struct {
    CONDITION_VARIABLE c;
} PCond;
#else
#include <pthread.h>
typedef struct {
    pthread_cond_t c;
} PCond;
#endif
void pc_init(PCond* c);
void pc_signal(PCond* c);
void pc_broadcast(PCond* c);
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns); /* 0 woken, 1 timed out */
typedef void (*PThreadFn)(void* arg);
int plat_thread(PThreadFn fn, void* arg, size_t stack);
void plat_thread_exit(void);
/* ends the process now, with code: stdio flushed, no atexit handlers, the other threads not joined */
void plat_exit_now(int code) __attribute__((noreturn));
void plat_sleep_ns(i64 ns);
/* Host text entry for the standard software keyboard: 0 accepted, 1 cancelled, -1 unavailable. */
int plat_text_input(char* out, size_t cap, const char* initial);
/* An interleaved signed 16-bit PCM output, initially paused. NULL means no host audio device (headless too).
   queue copies the samples before returning, so the guest may reuse a released buffer. */
typedef struct PAudio PAudio;
PAudio* plat_audio_open(u32 rate, u32 channels);
void plat_audio_close(PAudio* a);
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes);
void plat_audio_start(PAudio* a, int started);
void plat_audio_clear(PAudio* a);
u32 plat_audio_queued(PAudio* a); /* bytes queued and not yet played; 0 without a device */
void plat_audio_volume(PAudio* a, float volume);
void plat_yield(void);
u64 plat_ns(void); /* monotonic */
i64 plat_unix_time(void);
i64 plat_tz_offset(void); /* local - UTC, seconds */
u8* plat_vm_reserve(u64 size);
/* written-page tracking by range: calls cb for each run of pages in [p, p+size) written since the range was last
   scanned, and starts tracking them again. -1 when the platform can't (then everything must be checked by content).
   Linux: userfaultfd write protection (async) and PAGEMAP_SCAN; Windows: GetWriteWatch. SNX_NO_WW turns either off */
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size));
int plat_vm_commit(u8* p, u64 size);
void plat_vm_decommit(u8* p, u64 size);
void* plat_dl_open(const char* path);
void* plat_dl_sym(void* h, const char* name);
const char* plat_dl_error(void); /* why the last plat_dl_open or plat_dl_sym failed */
int plat_is_dir(const char* path);
typedef struct {
    void* h;
    int first;
    char name[512];
    int is_dir;
    u64 size;
} PDir;
int plat_dir_open(PDir* d, const char* path);
int plat_dir_next(PDir* d);
void plat_dir_close(PDir* d);
u64 plat_file_size(const char* path);
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off); /* bytes read (fewer at the end of the file), -1 on error */
/* a whole file mapped read-only. An empty file maps to data NULL with size 0 (and succeeds) */
typedef struct {
    const u8* data;
    u64 size;
    int borrowed; /* a view of a file shared for the whole run (fs_map_file): plat_file_unmap only clears it */
} PFileMap;
int plat_file_map(PFileMap* m, const char* path); /* 0, or -1 when the file can't be opened or mapped */
/* An indexed regular file: use the recorded length without querying host metadata. */
int plat_file_map_sized(PFileMap* m, const char* path, u64 size);
void plat_file_unmap(PFileMap* m);
/* A read-only file kept for the whole run: its fd stays open and it is mapped MAP_SHARED (on Windows a PAGE_READONLY
   section, shared by nature), so its pages are the host's page cache, shared by every running instance and read only
   when touched. size is the indexed length; an empty file keeps its fd with data NULL. 0, or -1 with *fd -1 when it
   can't be opened or mapped, or when keeping it open would leave too few descriptors for the files opened on demand */
/* replaces dst with tmp in one step (rename), so a reader never sees dst missing or half written. On failure dst is
   kept and tmp removed */
int plat_replace_file(const char* tmp, const char* dst);
void plat_install_crash_handler(void);
extern __thread void* tls_thread;
void plat_prof_start(void); /* the sampling profiler (profile option), a no-op outside Windows */
#if defined(_WIN32)
void plat_prof_add_thread(HANDLE h);
#endif

#endif
