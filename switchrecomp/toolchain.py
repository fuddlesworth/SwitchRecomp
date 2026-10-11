# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
"""Compilers: finding one, compiling and linking the game module, and building the runtime, which CMake owns (its
source list lives only in runtime's CMakeLists.txt)."""

import hashlib
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

from . import ABI_DIR, REPO


def find_cc(pref=None, target_win=False):
    cands = [pref] if pref else []
    if target_win:
        # windows: prefer the bundled tcc\, builds in seconds
        cands += (['tcc'] if sys.platform.startswith('win') else []) + ['x86_64-w64-mingw32-clang', 'clang', 'tcc']
    else:
        cands += ['clang', 'tcc']
    for c in cands:
        if not c:
            continue
        p = shutil.which(c) or (c if os.path.isfile(c) else None)
        if not p:
            # portable tcc/python next to us
            for d in (os.path.join(REPO, 'tcc'),):
                q = os.path.join(d, c + ('.exe' if sys.platform.startswith('win') else ''))
                if os.path.isfile(q):
                    p = q
                    break
        if p:
            return p
    return None


def is_tcc(cc):
    return 'tcc' in os.path.basename(cc).lower()


def tcc_b(cc):
    # bundled tcc keeps include\ and lib\ next to the exe
    d = os.path.dirname(os.path.abspath(cc))
    return ['-B', d] if os.path.isdir(os.path.join(d, 'include')) else []


def compile_all(files, cc, opt, outdir, target_win, jobs, say, extra_flags=()):
    objs = []
    base = [cc]
    if is_tcc(cc):
        flags = ['-w', '-I', ABI_DIR] + tcc_b(cc)
    else:
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
        flags += list(extra_flags)
    t0 = time.time()
    done = [0]

    # objects are keyed on compiler + flags + the ABI files, so tcc and clang objects never mix
    h = hashlib.sha1('|'.join(base + flags).encode())
    for f in sorted(os.listdir(ABI_DIR)):
        if f in ('cpu.h', 'cpu_helpers.c'):
            with open(os.path.join(ABI_DIR, f), 'rb') as fh:
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
                raise SystemExit('Compilation failed:\n' + err[-4000:])
            objs.append(obj)
    return objs


def link_module(objs, cc, out, target_win, say):
    # big games have hundreds of objects and windows caps command lines at ~32K, so use an @rsp file
    # (clang and tcc both take it) with short relative names
    bdir = os.path.dirname(os.path.abspath(objs[0])) if objs else '.'
    names = [os.path.relpath(os.path.abspath(o), bdir).replace('\\', '/') for o in objs]
    rsp = os.path.join(bdir, 'objects.rsp')
    with open(rsp, 'w') as fo:
        fo.write('\n'.join('"%s"' % n for n in names) + '\n')
    out = os.path.abspath(out)
    if os.path.isfile(cc):
        cc = os.path.abspath(cc)
    if is_tcc(cc):
        cmd = [cc, '-shared', '-o', out] + tcc_b(cc) + ['@objects.rsp']
    else:
        cmd = [cc, '-shared', '-o', out, '@objects.rsp']
        if not target_win:
            cmd += ['-lm']
        else:
            cmd += ['-static-libgcc']
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=bdir)
    if r.returncode:
        raise SystemExit('Linking failed:\n' + r.stderr[-4000:])


def build_runtime(cc, target_win, say):
    """Builds the runtime with the CMake preset matching the compiler and target, and returns its path."""
    preset = 'windows-cross' if target_win and not sys.platform.startswith('win') else 'default'
    say('Building the runtime (CMake preset %s)...' % preset)
    for cmd in (['cmake', '--preset', preset, '-DSWR_DEPLOY=OFF'], ['cmake', '--build', '--preset', preset]):
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('Runtime build failed:\n' + (r.stdout + r.stderr)[-4000:])
    return os.path.join(REPO, 'build', preset, 'swr-runtime' + ('.exe' if target_win else ''))
