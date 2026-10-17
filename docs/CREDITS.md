<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Licenses and credits

**SwitchRecomp by MZ Labs**, copyright 2026 MZ Labs, is ScarletNX's upstream.
Fork: [GPL-3.0-or-later](../LICENSE). MZ Labs code retains its
[Apache-2.0](../LICENSE.apache) headers and [NOTICE](../NOTICE).
Generated modules include runtime ABI code. Game assets, keys and firmware are excluded.

| Component | License |
| --- | --- |
| FFmpeg | LGPL-2.1-or-later |
| hactool / cJSON / ConvertUTF | ISC / MIT / Unicode license |
| LibHac storage code, ported into hactool by `cmake/hactool-libhac.patch` | MIT |
| OpenSSL | Apache 2.0 |
| LZ4 (the library, `lib/`) | BSD-2-Clause |
| Vulkan-Headers | Apache 2.0 |
| volk / Vulkan Memory Allocator | MIT |
| SDL3 | zlib |
| lunasvg / plutovg (the splash's SVGs) | MIT / MIT, with FreeType's rasterizer and stroker (FreeType License) and stb (MIT or public domain) |
| GUI VGA BIOS font (SeaBIOS `vgafont16`, originally Joseph Gil's font collection) | Public domain |

The GUI font's original notice and pinned source are preserved in
[`src/gui/FONT-NOTICE.txt`](../src/gui/FONT-NOTICE.txt).

Code ported into the repository, and assets taken from other projects. In these places, the files whose header names
a source keep that source's copyright lines and license, most with the revision they came from; the drawables of
`dist/controls/` have theirs in [its README](../dist/controls/README.md):

| Where | From | License |
| --- | --- | --- |
| `cmake/hactool-libhac.patch` (the storages it adds to hactool) | LibHac, as Ryubing maintains it | MIT |
| `src/3ds/gpu/`, `src/3ds/audio/`, `src/3ds/services/dsp.c` | Azahar (Citra) | GPL-2.0-or-later |
| `src/3ds/gpu/shader.c` and `shader.h` (the instruction fields) | nihstro | BSD-3-Clause |
| `src/common/network/`, `src/nx/hle/services/network/` | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/video/vp9_head.c`, `vp9_head.h`, `vp9_head_coef.h` | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `tests/ipc_message.c` (the HIPC, CMIF and TIPC request builders) | libnx | ISC |
| `dist/controls/*.xml`, and `controls.png` made from them | yuzu's Android overlay, as Eden and rEden carry it | GPL-3.0-or-later |

Installed license texts: `share/licenses/scarletnx/`. AppImage system-library licenses
and package manifest: `share/licenses/`. Component license files contain the full terms.

Community credits: [README](../README.md#special-thanks).

Wimsy ([SVG metadata](../dist/wimsy.svg)): drawn by Liz, GPL-3.0-or-later.
The splash's Fuddles ([SVG metadata](../dist/splash/wimsy-fuddles.svg)): Wimsy with Fuddles' tails, circuits and
colours, by AO55 after Liz's Wimsy, GPL-3.0-or-later.
