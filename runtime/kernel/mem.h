/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the guest address space (mem.c) */
#ifndef SWR_MEM_H
#define SWR_MEM_H
#include "abi/cpu.h"
#include <string.h>

#define AS_SIZE    0x2000000000ull
#define AS_BASE    0x08000000ull
#define HEAP_BASE  0x100000000ull
#define HEAP_SIZE  0x200000000ull
#define ALIAS_BASE 0x300000000ull
#define ALIAS_SIZE 0x1000000000ull
#define STACK_BASE 0x1300000000ull
#define STACK_SIZE 0x100000000ull
#define KERN_BASE  0x1400000000ull /* TLS, kernel stacks, our own shmem */
#define KERN_END   0x1800000000ull

enum {
    MS_FREE = 0x00000000,
    MS_IO = 0x00182001,
    MS_STATIC = 0x00042002,
    MS_CODE = 0x04DC7E03,
    MS_CODEDATA = 0x07FEBD04,
    MS_NORMAL = 0x077EBD05,
    MS_SHARED = 0x04402006,
    MS_ALIASCODE = 0x04DD7E08,
    MS_ALIASCODEDATA = 0x07FFBD09,
    MS_IPC = 0x045C3C0A,
    MS_STACK = 0x045C3C0B,
    MS_TLS = 0x0400000C,
    MS_TRANSFERED = 0x055C3C0D,
    MS_SHAREDTRANSFERED = 0x045C380E,
    MS_SHAREDCODE = 0x0440380F,
    MS_INACCESSIBLE = 0x00000010,
    MS_KERNEL = 0x00000013,
    MS_GENCODE = 0x04402214,
    MS_CODEOUT = 0x04402015
};
enum { PERM_NONE = 0, PERM_R = 1, PERM_W = 2, PERM_RW = 3, PERM_X = 4, PERM_RX = 5, PERM_DONTCARE = 0x10000000 };

extern u8* g_mem;
typedef struct {
    u64 addr, size;
    u32 state, perm, attr, ipc, dev;
} MemBlock;
void mem_init(void);
int mem_map(u64 addr, u64 size, u32 state, u32 perm); /* 0 ok */
int mem_unmap(u64 addr, u64 size);
int mem_is_free(u64 addr, u64 size);
void mem_query(u64 addr, MemBlock* out);
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm);
int mem_set_attr(u64 addr, u64 size, u32 mask, u32 attr);
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align);
u64 kmem_alloc(u64 size, u32 state, u32 perm); /* reserved for the kernel */
u64 mem_heap_set(u64 size);
extern u64 g_heap_size, g_heap_max;
#define GP(a) (g_mem + (u64)(a))
static inline u32 gr32(u64 a) {
    u32 v;
    memcpy(&v, GP(a), 4);
    return v;
}
static inline u64 gr64(u64 a) {
    u64 v;
    memcpy(&v, GP(a), 8);
    return v;
}
static inline void gw32(u64 a, u32 v) { memcpy(GP(a), &v, 4); }
static inline void gw64(u64 a, u64 v) { memcpy(GP(a), &v, 8); }

#endif
