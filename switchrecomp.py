#!/usr/bin/env python3
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
"""
SwitchRecomp: static recompiler for Switch games, AArch64 -> C -> native.

The game code is translated to C ahead of time (plus a fallback interpreter built from the same
instruction templates). The runtime provides HLE kernel/services and a GL renderer for the GPU.

Usage:
  python switchrecomp.py GAME_FOLDER                   (with exefs/ and romfs/ from hactool)
  python switchrecomp.py game.nro                      (homebrew)
  python switchrecomp.py FOLDER -o Game --cc gcc -O 2 --jobs 8

Output (next to the executable):
  Game.exe             runtime (Windows) or Game (Linux)
  Game_code.dll/.so    the game code already translated to native
  Game_data/           exefs + romfs + save data
  Game.ini             options

LEGAL: does not include or download games, keys or firmware. Use your own copy.
"""

import argparse, os, sys, shutil, subprocess, struct, time, hashlib, json
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nxloader, a64, codegen

RT = os.path.join(HERE, 'runtime')
RUNTIME_SRC = [
    'plat',
    'mem',
    'kernel',
    'ipc',
    'svc_core',
    'svc_hid',
    'svc_fs',
    'svc_vi',
    'nvdrv',
    'gpu',
    'gpu_engines',
    'gpu_3d',
    'loader',
    'main',
]
ABI = 1


def find_cc(pref=None, target_win=False):
    cands = [pref] if pref else []
    if target_win:
        # windows: prefer the bundled tcc\, builds in seconds
        cands += (['tcc'] if sys.platform.startswith('win') else []) + ['x86_64-w64-mingw32-gcc', 'gcc', 'clang', 'tcc']
    else:
        cands += ['gcc', 'clang', 'cc', 'tcc']
    for c in cands:
        if not c:
            continue
        p = shutil.which(c) or (c if os.path.isfile(c) else None)
        if not p:
            # portable tcc/python next to us
            for d in (
                os.path.join(HERE, 'tcc'),
                os.path.join(HERE, 'tools', 'tcc'),
                os.path.join(HERE, 'w64devkit', 'bin'),
            ):
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


# ======================================================================= codegen
def write_if_changed(p, text):
    # don't touch unchanged files so make-style reuse of the .o works
    try:
        with open(p, 'r') as fi:
            if fi.read() == text:
                return False
    except OSError:
        pass
    with open(p, 'w') as fo:
        fo.write(text)
    return True


def gen_module(prog, mods, npdm, outdir, title, chunk_instrs, say):
    funcs = prog.funcs
    order = sorted(funcs)
    # split by instruction count
    chunks = []
    cur = []
    n = 0
    for a in order:
        f = funcs[a]
        cur.append(f)
        n += len(f.addrs)
        if n >= chunk_instrs:
            chunks.append(cur)
            cur = []
            n = 0
    if cur:
        chunks.append(cur)
    files = []
    t0 = time.time()
    for ci, ch in enumerate(chunks):
        body = []
        refs = set()
        for f in ch:
            c = codegen.emit_function(prog, f, funcs)
            body.append(c)
        names_here = {f.name for f in ch}
        text = '\n'.join(body)
        # only prototype what this chunk references
        import re

        for m in set(re.findall(r'\bF_[0-9A-F]+\b', text)):
            refs.add(m)
        protos = ''.join('void %s(CPU* RESTRICT c, u64 e);\n' % r for r in sorted(refs))
        p = os.path.join(outdir, 'code_%03d.c' % ci)
        write_if_changed(p, '#include "cpu.h"\n' + protos + text + '\n')
        files.append(p)
        if ci % 20 == 0:
            say('  C generated: %d/%d chunks (%.0fs)' % (ci + 1, len(chunks), time.time() - t0))
    # main module: tables + interpreter
    ents = []
    for a in order:
        f = funcs[a]
        for l in sorted(f.leaders):
            ents.append((l, f.name))
    names = sorted({f.name for f in funcs.values()})
    idx = {n: i for i, n in enumerate(names)}
    L = ['#include "cpu.h"', '#include "cpu_helpers.c"']
    L += ['void %s(CPU* RESTRICT c, u64 e);' % n for n in names]
    L.append(codegen.gen_interp(step_mode=False))
    L.append('GuestFn swr_interp_entry;')
    L.append('static const GuestFn FNS[%d]={%s};' % (max(1, len(names)), ','.join(names) or '0'))
    L.append(
        'static const u32 ENT_OFF[%d]={%s};'
        % (max(1, len(ents)), ','.join('%d' % (a - nxloader.CODE_BASE) for a, _ in ents) or '0')
    )
    L.append('static const u32 ENT_FN[%d]={%s};' % (max(1, len(ents)), ','.join(str(idx[n]) for _, n in ents) or '0'))
    L.append('static u64 ENT_ADDR[%d]; static GuestFn ENT_PTR[%d];' % (max(1, len(ents)), max(1, len(ents))))
    L.append(
        'typedef struct SwrModInfo { const char* name; u64 base, size; u8 build_id[32]; u32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size; u32 is_nro; } SwrModInfo;'
    )
    mi = []
    for m in mods:
        bid = (m.build_id + b'\0' * 32)[:32]
        mi.append(
            '{"%s",0x%XULL,0x%XULL,{%s},%d,%d,%d,%d,%d,%d,%d,%d}'
            % (
                m.name,
                m.base,
                m.size,
                ','.join(str(b) for b in bid),
                m.text[0],
                m.text[1],
                m.ro[0],
                m.ro[1],
                m.data[0],
                m.data[1],
                m.bss,
                1 if getattr(m, 'nro', False) else 0,
            )
        )
    L.append('static const SwrModInfo MODS[%d]={%s};' % (len(mods), ','.join(mi)))
    lo = min(m.base + m.text[0] for m in mods)
    hi = max(m.base + m.text[0] + m.text[1] for m in mods)
    np_ = nxloader.parse_npdm(npdm)
    L.append("""typedef struct SwrModule { u32 abi; u32 nmods; const SwrModInfo* mods; u32 nentries; const u64* entry_addr; const GuestFn* entry_fn;
  GuestFn interp; u64 code_lo, code_hi; const char* title; u64 title_id; u32 main_stack, main_prio, main_core;
  void (*init)(const RtAPI* rt); u64 (*step)(CPU* c, u32 w, u64 pc); } SwrModule;""")
    L.append('static const RtAPI* g_rt;')
    L.append('static void mod_init(const RtAPI* rt){ g_rt=rt; }')
    L.append('static SwrModule MOD;')
    L.append('SWR_EXPORT const SwrModule* swr_module(void){')
    L.append(
        ' for(u32 i=0;i<%d;i++){ ENT_ADDR[i]=0x%XULL+ENT_OFF[i]; ENT_PTR[i]=FNS[ENT_FN[i]]; }'
        % (len(ents), nxloader.CODE_BASE)
    )
    L.append(' swr_interp_entry=swr_interp;')
    L.append(
        ' MOD.abi=%d; MOD.nmods=%d; MOD.mods=MODS; MOD.nentries=%d; MOD.entry_addr=ENT_ADDR; MOD.entry_fn=ENT_PTR; MOD.interp=swr_interp;'
        % (ABI, len(mods), len(ents))
    )
    L.append(
        ' MOD.code_lo=0x%XULL; MOD.code_hi=0x%XULL; MOD.title="%s"; MOD.title_id=0x%XULL;'
        % (lo, hi, title.replace('"', ''), np_['title_id'])
    )
    L.append(
        ' MOD.main_stack=%d; MOD.main_prio=%d; MOD.main_core=%d; MOD.init=mod_init; return &MOD; }'
        % (np_['stack'], np_['prio'], np_['core'])
    )
    p = os.path.join(outdir, 'module.c')
    write_if_changed(p, '\n'.join(L) + '\n')
    files.insert(0, p)
    say('C code: %d files, %.1f MB, %d entries' % (len(files), sum(os.path.getsize(x) for x in files) / 1e6, len(ents)))
    return files


def compile_all(files, cc, opt, outdir, target_win, jobs, say, extra_flags=()):
    objs = []
    base = [cc]
    if is_tcc(cc):
        flags = ['-w', '-I', RT] + tcc_b(cc)
    else:
        flags = [
            '-O' + opt,
            '-fno-strict-aliasing',
            '-fwrapv',
            '-w',
            '-I',
            RT,
            '-foptimize-sibling-calls',
            '-fno-semantic-interposition',
        ]
        if not target_win:
            flags += ['-fPIC', '-fvisibility=hidden']
        flags += list(extra_flags)
    t0 = time.time()
    done = [0]

    # object dir is keyed on compiler + flags + cpu.h, so tcc and gcc objects never mix
    import hashlib

    sig = hashlib.sha1(
        (
            '|'.join(base + flags)
            + '|'
            + open(os.path.join(RT, 'cpu.h'), 'rb').read().decode('latin1')
            + '|'
            + open(os.path.join(RT, 'cpu_helpers.c'), 'rb').read().decode('latin1')
        ).encode('latin1')
    ).hexdigest()[:10]

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
    # (gcc and tcc both take it) with short relative names
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


def build_runtime(cc, out, target_win, backend, say, opt='2'):
    gl = backend != 'headless'
    extra = ['gpu_gl', 'gl_load', 'shader', 'astc', 'glq', 'glq_gen'] if gl else ['gpu_nogl']
    gles = backend in ('x11es', 'android')
    if gles:
        extra += ['overlay']
    srcs = [os.path.join(RT, s + '.c') for s in RUNTIME_SRC + extra] + [
        os.path.join(RT, 'backend_%s.c' % backend),
        os.path.join(RT, 'entry.c'),
    ]
    if gles or gl:
        srcs += [os.path.join(RT, 'bcn', 'bcn.cpp'), os.path.join(RT, 'bcn', 'bcdec.cpp')]
    # LSFG (uses the user's Lossless.dll), desktop GL only
    spvc = None
    if gl and backend in ('win32', 'x11'):
        spvc = os.path.join(RT, 'spvc', 'win' if target_win else 'lin', 'libspvc.a')
        if os.path.isfile(spvc):
            srcs += [os.path.join(RT, 'lsfg.c'), os.path.join(RT, 'lsfg_spvc.cpp')]
        else:
            spvc = None
    if gl and not spvc:
        srcs += [os.path.join(RT, 'lsfg_stub.c')]
    if target_win and backend == 'win32':
        srcs += [os.path.join(RT, 'rec_win32.c')]  # F7 recorder, Media Foundation
    # NVDEC/VIC video: link OpenH264 (BSD) statically if we have it for this target
    oh = os.path.join(RT, 'oh264', 'win' if target_win else 'lin')
    h264 = os.path.isfile(os.path.join(oh, 'libdecoder.a')) and backend in ('win32', 'x11', 'headless')
    srcs += [os.path.join(RT, 'host1x.c')]
    cmd = (
        [
            cc,
            '-O' + opt,
            '-w',
            '-fno-strict-aliasing',
            '-Wl,-S',
            '-I',
            RT,
            '-I',
            os.path.join(RT, 'bcn'),
            '-I',
            os.path.join(RT, 'spvc'),
            '-I',
            os.path.join(RT, 'oh264', 'include'),
        ]
        + srcs
        + ['-o', out]
    )
    if not h264:
        cmd += ['-DSWR_NO_H264']
    if spvc:
        cmd += [spvc]
    if h264:
        cmd += [os.path.join(oh, 'libdecoder.a'), os.path.join(oh, 'libcommon.a')]
    if spvc or h264:
        cmd += ['-lstdc++']
    if target_win:
        cmd += [
            '-static',
            '-lgdi32',
            '-lwinmm',
            '-luser32',
            '-lopengl32',
            '-lcomdlg32',
            '-lshell32',
            '-mwindows' if backend == 'win32' else '-mconsole',
        ]
        if backend == 'win32':
            cmd += ['-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lole32']
        res = os.path.join(RT, 'icon.res')
    else:
        cmd += ['-lpthread', '-ldl', '-lm', '-rdynamic']
        if backend == 'x11':
            cmd += ['-lX11', '-lGL']
        if backend == 'x11es':
            cmd += ['-lX11', '-lEGL']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('Runtime build failed:\n' + r.stderr[-4000:])


def copy_data(mods, npdm, romfs, entry, datadir, say):
    ex = os.path.join(datadir, 'exefs')
    os.makedirs(ex, exist_ok=True)
    if os.path.isfile(entry) and entry.lower().endswith('.nro'):
        shutil.copyfile(entry, os.path.join(ex, 'main.nro'))
        if isinstance(romfs, (bytes, bytearray)):
            open(os.path.join(datadir, 'romfs.bin'), 'wb').write(romfs)
        return
    src = nxloader.find_exefs(entry)
    for n in os.listdir(src):
        s = os.path.join(src, n)
        d = os.path.join(ex, n)
        if os.path.isfile(s) and not (os.path.exists(d) and os.path.getsize(d) == os.path.getsize(s)):
            shutil.copyfile(s, d)
    if isinstance(romfs, str):
        dst = os.path.join(datadir, 'romfs')
        if os.path.abspath(romfs) != os.path.abspath(dst) and not os.path.exists(dst):
            say('Copying romfs (may take a while)...')
            shutil.copytree(romfs, dst)


INI = """; %(title)s - SwitchRecomp settings
; This file is rewritten (organized, with every option explained) the first time the game starts.
[Display]
language=1
"""


def main():
    ap = argparse.ArgumentParser(description='Recompiles a Switch game into a native executable.')
    ap.add_argument('input', help='game folder (exefs/ + romfs/) or .nro file')
    ap.add_argument('-o', '--out', help='output name (no extension)')
    ap.add_argument('--outdir', help='output folder')
    ap.add_argument('--cc', help='C compiler (gcc, clang, tcc, x86_64-w64-mingw32-gcc)')
    ap.add_argument('-O', default='2', help='optimization level (0-3)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--chunk', type=int, default=30000, help='instructions per C file')
    ap.add_argument('--target', choices=['auto', 'win64', 'linux'], default='auto')
    ap.add_argument('--backend', choices=['auto', 'win32', 'x11', 'x11es', 'headless', 'android'], default='auto')
    ap.add_argument('--native', action='store_true', help='optimize for THIS processor (-march=native)')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--keep-c', action='store_true')
    ap.add_argument('--interp-only', action='store_true', help='(diagnostics) no recompilation, everything interpreted')
    ap.add_argument('--runtime', help='use an already built runtime (.exe) instead of building it')
    ap.add_argument('-q', '--quiet', action='store_true')
    a = ap.parse_args()
    say = (lambda m: None) if a.quiet else (lambda m: print(m, flush=True))
    say('SwitchRecomp - use your own copy of the game. No games or keys included.')
    target_win = a.target == 'win64' or (a.target == 'auto' and sys.platform.startswith('win'))
    backend = a.backend if a.backend != 'auto' else ('win32' if target_win else 'headless')
    mods, npdm, romfs = nxloader.load_program(a.input)
    for m in mods:
        say('  %-8s %010X  code %.1f MB  .eh_frame functions %d' % (m.name, m.base, m.text[1] / 1048576.0, len(m.fde)))
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
        for ln in open(hp):
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
    files = gen_module(prog, mods, npdm, build, title, a.chunk, say)
    datadir = base + '_data'
    copy_data(mods, npdm, romfs, a.input, datadir, say)
    if not os.path.exists(base + '.ini'):
        open(base + '.ini', 'w').write(INI % {'title': title})
    if a.no_build:
        return
    cc = find_cc(a.cc, target_win)
    if not cc:
        raise SystemExit('No C compiler found (gcc, clang or tcc).')
    say('Compiling with %s (-O%s, %d at a time)...' % (os.path.basename(cc), a.O, a.jobs))
    extra = []
    if a.native and not is_tcc(cc):
        extra.append('-march=native')
    if os.environ.get('SWR_CFLAGS') and not is_tcc(cc):
        extra += os.environ['SWR_CFLAGS'].split()  # e.g. -march for someone else's cpu
    objs = compile_all(files, cc, a.O, build, target_win, a.jobs, say, extra)
    modout = base + ('_code.dll' if target_win else '_code.so')
    say('Linking %s ...' % os.path.basename(modout))
    link_module(objs, cc, modout, target_win, say)
    exe = base + ('.exe' if target_win else '')
    if a.runtime:
        shutil.copyfile(a.runtime, exe)
    else:
        pre = os.path.join(
            HERE, 'bin', 'runtime_%s_%s%s' % ('win64' if target_win else 'linux', backend, '.exe' if target_win else '')
        )
        if os.path.exists(pre):
            shutil.copyfile(pre, exe)
        else:
            say('Compiling the runtime (%s)...' % backend)
            build_runtime(cc, exe, target_win, backend, say)
    if not target_win:
        os.chmod(exe, 0o755)
    if not a.keep_c:
        pass
    say('Done: %s' % exe)


if __name__ == '__main__':
    main()
