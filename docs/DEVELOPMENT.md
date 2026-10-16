<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Development

[Architecture](ARCHITECTURE.md) · [build/toolchains](BUILD.md) · [usage](USAGE.md) · [agent rules](../CLAUDE.md).
Run from checkout. POSIX examples: Windows uses `python`, `.exe`, PowerShell env syntax.

## Rebuild / checks

```sh
cmake --build build
./build/scarletnx -r work/Game Game --jobs 8
ctest --test-dir build --output-on-failure
scripts/format.sh --check
python3 tests/run_filesystem_tree.py --cc clang --sanitize
clang -fsyntax-only -Wall -fno-strict-aliasing -fwrapv -I src/common/cpu/aarch64 src/common/cpu/aarch64/cpu_helpers.c
```

Runtime relink deploys to `games/` by default. Recompiler/`src/common/cpu/aarch64/` edits require `-r`.
Avoid builds during timing runs. Formatting: clang-format 23 + `uv`/pinned Ruff; omit `--check` to write.
FS tests use production tree/HLE + synthetic temp data; `--only tree|service` selects a suite.

CI: Linux/Windows builds, ABI syntax, native tool/FS regressions, optional Python diagnostics. Formatting is checked locally (`scripts/format.sh --check`).
Verify affected scenes manually, repeat intermittent failures, record evidence/scope.
Test guest semantics + malformed IPC with synthetic cases.

## Run / diagnose

```sh
(cd games && ./Game --seconds=60 --script=15000:400,16000:1 --press_len=300 --shot=out)
python3 tools/run_game.py Game --runs 5 --seconds 60 --dump work/Game --script ""
python3 tools/symbols.py work/Game cf34380
python3 tools/disasm.py work/Game NvRmStreamFlushSync
python3 tools/disasm.py work/Game cf34240 cf343c0
```

- Runtime: `--seconds=N` / `--frames=N`; `--shot=out` → CWD `out.ppm`; `--shot_every=N` → every N frames.
  `--script=ms:hexbuttons,...`, `--press_len=ms` (default 300).
  Masks: A/B/X/Y `1/2/4/8`, L/R `40/80`, ZL/ZR `100/200`, Plus/Minus `400/800`,
  left/up/right/down `1000/2000/4000/8000`.
- `run_game.py`: Catherine New Game sequence by default; **`--script ""` disables it**.
  `--out DIR` → `runN.log` + PNG (PPM without Pillow); default fresh temp dir.
  Summary: exit, PC/symbol (`--dump`), frames/FPS. Unknown args pass through, e.g. `--fps_limit=0`.
- Guest VAs: hex; disasm function queries: substrings. Disasm requires `llvm-objcopy` + `llvm-objdump`.
  `--log=2`: verbose; Windows `--profile=1`: `profile.txt` every 10 s.

## Shaders

```sh
SNX_SHADER_CORPUS=corpus games/Game
build/snx-shaderc corpus --validate --out translated
```

`--validate` needs `spirv-val`. `--out` writes SPIR-V modules for build comparisons.
PowerShell: `$env:SNX_SHADER_CORPUS='corpus'`. Where the game gets no environment (Android), its INI's `[Debug]
diag=1` keeps each shader's entry in `captures/shaders` beside the game, for the same `snx-shaderc`.

## Upstream / provenance

```sh
scripts/import-upstream.sh SwitchRecomp_v143.zip v143
```

`upstream`: MZ Labs releases + formatted copies; `upstream-vN`: original imports; fork base: `upstream-v142`.
Port to `main` manually (different layouts). Exclude dumps/keys/generated modules/dependency builds from Git.
Preserve headers + [credits/install notices](CREDITS.md).
