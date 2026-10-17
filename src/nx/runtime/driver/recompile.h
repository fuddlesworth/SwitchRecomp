/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_RECOMPILE_H
#define SNX_RECOMPILE_H
#include "common/utilities/common.h"
/* the ABI stamped into the games this build recompiles */
unsigned snx_game_abi(void);
/* what a game this build recompiled has in its <name>.abi, owned: the ABI, then the build's commit, a line each. A
   game whose stamp says anything else is from another version, and is recompiled before it runs */
char* snx_game_stamp(void);
int snx_recompile(int argc, char** argv, const SnxPaths* paths);
#endif
