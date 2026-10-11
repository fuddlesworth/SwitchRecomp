# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
"""The game module's C: one file per chunk of functions, and module.c with the entry tables, the fallback interpreter
and the descriptor the runtime reads (swr_module)."""

import os
import re
import time

from . import codegen, nxloader
from . import abi_version


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
        % (abi_version(), len(mods), len(ents))
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
