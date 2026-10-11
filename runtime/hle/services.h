/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* registering the HLE services (each file registers its own) */
#ifndef SWR_SERVICES_H
#define SWR_SERVICES_H
#include "kernel/ipc.h"

void svc_core_init(void); /* services.c */
void svc_fs_init(void);
void svc_hid_init(void);
void svc_vi_init(void);
void svc_pl_init(void);                /* pl.c: the shared fonts */
SvcObj* svc_generic(const char* name); /* empty success reply, logged */

#endif
