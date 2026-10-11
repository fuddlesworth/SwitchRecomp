/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the file system services and the game's romfs (fs.c) */
#ifndef SWR_FS_H
#define SWR_FS_H
#include "abi/cpu.h"

void romfs_init(void);
/* Android: the romfs comes from the APK (chunks of a file) or a folder */
int romfs_add_chunk(int fd, u64 base, u64 len);
extern char g_romfs_dir_override[1024];
u64 romfs_size(void);
int romfs_read(u64 off, void* dst, u64 size);

#endif
