# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
"""SwitchRecomp: static recompiler for Switch games, AArch64 -> C -> native.

The game code is translated to C ahead of time (plus a fallback interpreter built from the same instruction templates)
and compiled into <Game>_code.so or .dll, which the runtime (runtime/, built with CMake) loads. Run it as
`python3 -m switchrecomp`, or through scripts/recomp.py.

    nxloader   NSO/NRO and NPDM loading, symbols
    a64        ARM64 instruction decoding and C templates
    codegen    function discovery and C emission
    module     the game module's C files (functions, entry tables, the module descriptor)
    toolchain  compiling and linking the game module, building the runtime with CMake
    data       the game's data folder next to it (exefs, romfs)
    cli        the command line

LEGAL: does not include or download games, keys or firmware. Use your own copy.
"""

import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(REPO, 'runtime')
# the headers and helpers the generated code is compiled with: the ABI between game module and runtime
ABI_DIR = os.path.join(RUNTIME, 'abi')


def abi_version():
    """SWR_ABI from cpu.h: the runtime refuses a game module built against another one."""
    with open(os.path.join(ABI_DIR, 'cpu.h')) as f:
        m = re.search(r'#define\s+SWR_ABI\s+(\d+)', f.read())
    if not m:
        raise SystemExit('cpu.h has no SWR_ABI')
    return int(m.group(1))
