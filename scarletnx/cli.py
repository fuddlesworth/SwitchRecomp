# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
# This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE).
"""The command line: python3 -m scarletnx GAME_FOLDER [-o NAME] [--outdir DIR] [--cc CC] ...

Output (next to the executable):
  Game.exe / Game       the runtime, copied from --runtime or built with CMake
  Game_code.dll / .so   the game code already translated to native
  Game_data/            exefs + romfs + save data
  Game.ini              options
"""

import argparse
import os
import shutil
import sys

from . import codegen, data, module, nxloader, toolchain


def main():
    ap = argparse.ArgumentParser(description='Recompiles a Switch game into a native executable.')
    ap.add_argument('input', help='game folder (exefs/ + romfs/) or .nro file')
    ap.add_argument('-o', '--out', help='output name (no extension)')
    ap.add_argument('--outdir', help='output folder')
    ap.add_argument('--cc', help='C compiler (clang, x86_64-w64-mingw32-clang)')
    ap.add_argument('-O', default='2', help='optimization level (0-3)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--chunk', type=int, default=30000, help='instructions per C file')
    ap.add_argument('--target', choices=['auto', 'win64', 'linux'], default='auto')
    ap.add_argument('--native', action='store_true', help='optimize for THIS processor (-march=native)')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--keep-c', action='store_true')
    ap.add_argument('--interp-only', action='store_true', help='(diagnostics) no recompilation, everything interpreted')
    ap.add_argument('--runtime', help='use an already built runtime instead of building it with CMake')
    ap.add_argument('-q', '--quiet', action='store_true')
    a = ap.parse_args()
    say = (lambda m: None) if a.quiet else (lambda m: print(m, flush=True))
    say('ScarletNX: use your own copy of the game. No games or keys are included.')
    target_win = a.target == 'win64' or (a.target == 'auto' and sys.platform.startswith('win'))
    mods, npdm, romfs = nxloader.load_program(a.input)
    for m in mods:
        say('  %-8s %010X  code %.1f MB  .eh_frame functions %d' % (m.name, m.base, m.text[1] / 1048576.0, len(m.fde)))
        if m.eh_error:
            say(
                '  warning: the .eh_frame of %s %s. Its other functions are found by following calls.'
                % (m.name, m.eh_error)
            )
    info = nxloader.parse_npdm(npdm)
    title = (
        a.out
        or (info['name'] if info['name'] not in ('Application', '') else None)
        or os.path.splitext(os.path.basename(os.path.normpath(a.input)))[0]
    )
    title = ''.join(ch for ch in title if ch.isalnum() or ch in '-_ ') or 'Game'
    outdir = os.path.abspath(a.outdir or os.getcwd())
    os.makedirs(outdir, exist_ok=True)
    base = os.path.join(outdir, title)
    hints = []
    hp = base + '.hints'
    if os.path.exists(hp):
        with open(hp) as f:
            for ln in f:
                try:
                    hints.append(int(ln.strip(), 16))
                except ValueError:
                    pass
        say('Previous execution hints: %d' % len(hints))
    prog = codegen.Program(mods, hints)
    if a.interp_only:
        prog.funcs = {}
        prog.extra_entries = set()
    else:
        say('Analyzing the code...')
        prog.analyze(say)
    build = base + '_build'
    os.makedirs(build, exist_ok=True)
    say('Translating ARM64 to C...')
    files = module.gen_module(prog, mods, npdm, build, title, a.chunk, say)
    datadir = base + '_data'
    data.copy_data(mods, npdm, romfs, a.input, datadir, say)
    if not os.path.exists(base + '.ini'):
        with open(base + '.ini', 'w', encoding='utf-8') as f:
            f.write(data.INI % {'title': title})
    if a.no_build:
        return
    cc = toolchain.find_cc(a.cc, target_win)
    if not cc:
        raise SystemExit(
            'C compiler not found: %s' % a.cc
            if a.cc
            else 'No C compiler found (%s).' % ' or '.join(toolchain.cc_candidates(target_win))
        )
    say('Compiling with %s (-O%s, %d at a time)...' % (os.path.basename(cc), a.O, a.jobs))
    extra = []
    if a.native:
        extra.append('-march=native')
    if os.environ.get('SNX_CFLAGS'):
        extra += os.environ['SNX_CFLAGS'].split()  # e.g. -march for someone else's cpu
    objs = toolchain.compile_all(files, cc, a.O, target_win, a.jobs, say, extra)
    modout = base + ('_code.dll' if target_win else '_code.so')
    say('Linking %s ...' % os.path.basename(modout))
    toolchain.link_module(objs, cc, modout, target_win)
    exe = base + ('.exe' if target_win else '')
    # copy beside it and rename over it (as cmake/deploy.cmake does): writing into a runtime that is running fails
    # (ETXTBSY), and an interrupted copy would leave half an executable
    tmp = exe + '.tmp'
    shutil.copyfile(a.runtime or toolchain.build_runtime(target_win, say), tmp)
    if not target_win:
        os.chmod(tmp, 0o755)
    os.replace(tmp, exe)
    say('Done: %s' % exe)
