#!/usr/bin/env python3
"""Recompiles an extracted game (scripts/extract.py) into games/, for this machine (Linux or Windows):

    games/NAME[.exe]               the runtime, built by CMake with the chosen preset
    games/NAME_code.so / .dll      the game's code translated to C and compiled with the preset's compiler
    games/NAME_data/               exefs, romfs and saves
    games/shader_cache/spirv/      SPIR-V for the game's own shader files, when it ships them (Vulkan renderer)

    python3 scripts/recomp.py work/Catherine Catherine
    python3 scripts/recomp.py work/Catherine Catherine --preset clang -- --native

Options after `--` go to switchrecomp.py. The first run of a big game takes several minutes; later runs only
recompile what changed.
"""

import argparse
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOWS = sys.platform == 'win32'
COMPILERS = {'gcc': 'gcc', 'clang': 'clang'}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('game', help='the extracted game folder (exefs/ + romfs/)')
    ap.add_argument('name', help='output name, e.g. Catherine')
    ap.add_argument('--preset', choices=sorted(COMPILERS), default='gcc', help='CMake preset and compiler')
    ap.add_argument('extra', nargs='*', help='more switchrecomp.py options, after --')
    a = ap.parse_args()

    subprocess.run(['cmake', '--preset', a.preset], cwd=REPO, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', '--preset', a.preset], cwd=REPO, check=True)
    runtime = os.path.join(REPO, 'build', a.preset, 'swr-runtime' + ('.exe' if WINDOWS else ''))
    extra = list(a.extra)
    if '--cc' not in extra:
        extra += ['--cc', COMPILERS[a.preset]]
    cmd = [
        sys.executable,
        os.path.join(REPO, 'switchrecomp.py'),
        a.game,
        '-o',
        a.name,
        '--outdir',
        os.path.join(REPO, 'games'),
        '--target',
        'win64' if WINDOWS else 'linux',
        '--backend',
        'win32' if WINDOWS else 'x11',
        '--runtime',
        runtime,
    ] + extra
    rc = subprocess.run(cmd).returncode
    if rc == 0:
        prebuild_shaders(a.preset, os.path.join(REPO, 'games', a.name + '_data', 'romfs'))
    return rc


NVN_SHADER_MAGIC = b'\x91\x68\x86\x19'


def prebuild_shaders(preset, romfs):
    """Fills the Vulkan renderer's SPIR-V cache from the NVN shader files a game ships in its romfs (Catherine has
    them in data/shaders_binary), so the first run compiles fewer shaders while playing."""
    shaderc = os.path.join(REPO, 'build', preset, 'swr-shaderc' + ('.exe' if WINDOWS else ''))
    if not os.path.isfile(shaderc) or not os.path.isdir(romfs):
        return
    cache = os.path.join(REPO, 'games', 'shader_cache', 'spirv')
    for root, _dirs, files in os.walk(romfs):
        for f in files:
            with open(os.path.join(root, f), 'rb') as fh:
                if fh.read(4) == NVN_SHADER_MAGIC:
                    break
        else:
            continue
        os.makedirs(cache, exist_ok=True)
        print('Translating the shaders in %s' % os.path.relpath(root, romfs), flush=True)
        subprocess.run([shaderc, '--nvn', root, '--cache', cache])


if __name__ == '__main__':
    sys.exit(main())
