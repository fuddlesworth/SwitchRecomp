# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
# This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE).
"""Compilers: finding one, compiling and linking the game module, and building the runtime, which CMake owns (its
source list lives only in runtime's CMakeLists.txt)."""

import glob
import hashlib
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

from . import ABI_DIR, REPO


def find_cc(pref=None, target_win=False):
    if pref:
        cands = [pref]  # the compiler asked for or none: never a different one in its place
    elif target_win and sys.platform.startswith('win'):
        cands = ['x86_64-w64-mingw32-clang', 'clang']
    elif target_win:
        # cross: only llvm-mingw makes a Windows DLL here, the host's clang would make an ELF named .dll
        cands = ['x86_64-w64-mingw32-clang']
    else:
        cands = ['clang']
    for c in cands:
        p = shutil.which(c) or (c if os.path.isfile(c) else None)
        if p:
            return p
    return None


def compile_all(files, cc, opt, target_win, jobs, say, extra_flags=()):
    objs = []
    base = [cc]
    flags = [
        '-O' + opt,
        '-fno-strict-aliasing',
        '-fwrapv',
        '-w',
        '-I',
        ABI_DIR,
        '-foptimize-sibling-calls',
        '-fno-semantic-interposition',
    ]
    if not target_win:
        flags += ['-fPIC', '-fvisibility=hidden']
    # no -frounding-math: the code is compiled for round to nearest, the only FPCR mode games were seen to use.
    # Under a directed mode Clang's folding can differ from the ARM result (FNMUL as (-a)*b, a conversion of 0 giving
    # -0), so other modes are not supported and the runtime warns when a game selects one (snx_fpcr_apply)
    flags += list(extra_flags)
    t0 = time.time()
    done = [0]

    # objects are keyed on compiler (its path and version) + flags + the ABI files (cpu.h, cpu_helpers.c and the helpers
    # it includes), so objects from another compiler or another ABI never mix
    h = hashlib.sha1('|'.join(base + flags).encode())
    try:
        h.update(subprocess.run([cc, '--version'], capture_output=True).stdout)
    except OSError:
        pass
    for abi_root, abi_dirs, abi_files in os.walk(ABI_DIR):
        abi_dirs.sort()
        for name in sorted(abi_files):
            if name.endswith(('.h', '.c')):
                with open(os.path.join(abi_root, name), 'rb') as fh:
                    h.update(fh.read())
    sig = h.hexdigest()[:10]

    def one(src):
        obj = os.path.splitext(src)[0] + '.' + sig + '.o'
        if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
            done[0] += 1
            return obj, None
        r = subprocess.run(base + flags + ['-c', src, '-o', obj], capture_output=True, text=True)
        done[0] += 1
        if done[0] % 10 == 0 or done[0] == len(files):
            say('  compiled %d/%d (%.0fs)' % (done[0], len(files), time.time() - t0))
        return obj, (r.stderr if r.returncode else None)

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for obj, err in ex.map(one, files):
            if err:
                # drop the chunks still queued: leaving the with block waits only for the ones already compiling
                ex.shutdown(wait=False, cancel_futures=True)
                raise SystemExit('Compilation failed:\n' + err[-4000:])
            objs.append(obj)
    # objects of other signatures (an older ABI, other flags) are never used again: a big game leaves a GB of them
    for src, obj in zip(files, objs):
        for old in glob.glob(glob.escape(os.path.splitext(src)[0]) + '.*.o'):
            if old != obj:
                os.remove(old)
    return objs


def link_module(objs, cc, out, target_win):
    # big games have hundreds of objects and windows caps command lines at ~32K, so use an @rsp file
    # with short relative names
    bdir = os.path.dirname(os.path.abspath(objs[0])) if objs else '.'
    names = [os.path.relpath(os.path.abspath(o), bdir).replace('\\', '/') for o in objs]
    rsp = os.path.join(bdir, 'objects.rsp')
    with open(rsp, 'w') as fo:
        fo.write('\n'.join('"%s"' % n for n in names) + '\n')
    out = os.path.abspath(out)
    if os.path.isfile(cc):
        cc = os.path.abspath(cc)
    cmd = [cc, '-shared', '-o', out, '@objects.rsp']
    if not target_win:
        cmd += ['-lm']
    else:
        cmd += ['-static-libgcc']  # llvm-mingw: the unwinder linked in, so the module needs no extra DLL
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=bdir)
    if r.returncode:
        raise SystemExit('Linking failed:\n' + r.stderr[-4000:])


def build_runtime(target_win, say):
    """Builds the runtime with the CMake preset matching the target, and returns its path."""
    preset = 'windows-cross' if target_win and not sys.platform.startswith('win') else 'default'
    # a build folder of its own: SNX_DEPLOY=OFF stays in the cache, and in the preset's folder it would silently stop
    # the developer's own builds from deploying to games/
    bdir = os.path.join(REPO, 'build', preset + '-recomp')
    say('Building the runtime (CMake preset %s, in %s)...' % (preset, bdir))
    for cmd in (['cmake', '--preset', preset, '-B', bdir, '-DSNX_DEPLOY=OFF'], ['cmake', '--build', bdir]):
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('Runtime build failed:\n' + (r.stdout + r.stderr)[-4000:])
    return os.path.join(bdir, 'scarletnx' + ('.exe' if target_win else ''))
