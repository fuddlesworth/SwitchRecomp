/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_BUILD_INFO_H
#define SNX_BUILD_INFO_H
/* "ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)", for the window titles */
const char* snx_build_info(void);
/* the commit in it: a short hash, "-dirty" after it when the tree had changes, "unknown" outside a git checkout */
const char* snx_build_commit(void);
#endif
