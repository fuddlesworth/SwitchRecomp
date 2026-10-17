# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The LLVM components the recompiler links (cmake/llvm.cmake), and the ones the LLVM builds of scripts/android-llvm.sh
# and scripts/apple-llvm.sh hold with what they need (scripts/llvm-libraries): one list for both
set(SNX_LLVM_COMPONENTS core native passes target analysis mcjit)
