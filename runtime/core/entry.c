/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* executable entry point */
#include "core/main.h"
#if defined(_WIN32)
#include <windows.h>
int main(int argc, char** argv) { return swr_main(argc, argv); }
#else
int main(int argc, char** argv) { return swr_main(argc, argv); }
#endif
