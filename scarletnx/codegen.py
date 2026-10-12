# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
# This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE).
"""
codegen: analysis -> C.
  - function discovery (.eh_frame, symbols, reloc targets, BL targets, hints)
  - one C function per guest function, gotos between blocks, switch on entry
  - fallback interpreter from the same a64.py templates
(module.py splits the output into chunks that compile in parallel)
"""

import sys
from . import a64
from .a64 import lit, TABLE

UNCOND = ('b', 'br', 'ret', 'brk')


# ======================================================================= analysis
class Func:
    __slots__ = ('start', 'addrs', 'leaders', 'mod', 'has_br', 'name', 'aset')

    def __init__(self, start, addrs, mod):
        self.start = start
        self.addrs = addrs
        self.mod = mod
        self.leaders = set()
        self.has_br = False
        self.name = 'F_%X' % start
        # .eh_frame functions as ranges, a set of every address gets huge on big games
        self.aset = addrs if isinstance(addrs, range) else set(addrs)


_PLAIN = (1, 'x')


class Program:
    def __init__(self, mods, hints=()):
        self.mods = mods
        self.hints = set(hints)
        self._lastm = None
        import array

        for m in mods:
            m._t0 = m.base + m.text[0]
            m._t1 = m._t0 + m.text[1]
            # unpack all of .text in one go
            o = m.text[0]
            n = m.text[1] // 4
            m._words = array.array('I', bytes(m.image[o : o + n * 4]))
            if sys.byteorder != 'little':
                m._words.byteswap()
        # keep only control flow per instruction (1 byte state each); storing the C for everything
        # doesn't fit in RAM for ~100 MB of code
        self.cf = {}  # addr -> (w, branch tuple or None)
        for m in mods:
            m._st = bytearray(m.text[1] // 4 + 1)  # 0 unseen, 1 plain, 2 in cf

    def mod_of(self, a):
        m = self._lastm  # usually the same module as last call
        if m is not None and m._t0 <= a < m._t1:
            return m
        for m in self.mods:
            if m._t0 <= a < m._t1:
                self._lastm = m
                return m
        return None

    def word(self, a):
        m = self.mod_of(a)
        if not m:
            return None
        return m._words[(a - m._t0) >> 2]

    def decode(self, a):
        """instruction + its C, only needed at emit time (a64 caches C per encoding)"""
        m = self._lastm
        if m is None or not (m._t0 <= a < m._t1):
            m = self.mod_of(a)
            if not m:
                return (None, None)
        w = m._words[(a - m._t0) >> 2]
        try:
            res = a64.emit_static(w, a)
        except Exception as e:
            raise RuntimeError('error translating %08X at %X: %s' % (w, a, e))
        return (w, res)

    def decode_cf(self, a):
        """analysis view: (w, branch tuple | None | 'x' for a plain instruction)"""
        m = self._lastm
        if m is None or not (m._t0 <= a < m._t1):
            m = self.mod_of(a)
            if not m:
                return (None, None)
        i = (a - m._t0) >> 2
        st = m._st
        k = st[i]
        if k == 1:
            return _PLAIN
        if k == 2:
            return self.cf[a]
        w = m._words[i]
        try:
            res = a64.emit_static(w, a)
        except Exception as e:
            raise RuntimeError('error translating %08X at %X: %s' % (w, a, e))
        if isinstance(res, tuple) or res is None:
            r = (w, res)
            self.cf[a] = r
            st[i] = 2
            return r
        st[i] = 1
        return _PLAIN

    # --------------------------------------------------------------------------
    def analyze(self, say=print):
        starts = {}  # addr -> end, None = needs exploring
        for m in self.mods:
            t0 = m.base + m.text[0]
            t1 = t0 + m.text[1]
            for s, n in m.fde:
                a = m.base + s
                if t0 <= a < t1 and n > 0 and not a & 3:
                    starts[a] = min(a + n, t1)
            for f in m.funcs:
                starts.setdefault(m.base + f, None)
            for f in m.relptrs:
                starts.setdefault(m.base + f, None)
            starts.setdefault(t0, None)  # entry point
        for h in self.hints:
            if self.mod_of(h):
                starts.setdefault(h, None)
        # overlapping FDEs: keep the one that starts first, and drop the ones starting inside it
        funcs = {}
        fde_sorted = sorted((a, e) for a, e in starts.items() if e)
        covered = []  # (start, end) of FDE ranges
        last_end = 0
        for a, e in fde_sorted:
            if a < last_end:
                continue
            covered.append((a, e))
            last_end = e
        import bisect

        cstarts = [x[0] for x in covered]

        def inside_fde(a):
            i = bisect.bisect_right(cstarts, a) - 1
            return i >= 0 and covered[i][0] <= a < covered[i][1], (covered[i] if i >= 0 else None)

        for a, e in covered:
            funcs[a] = Func(a, range(a, e, 4), self.mod_of(a))
        known = set(cstarts) | set(funcs)  # build once, not per explored function
        # seeds with no FDE get explored recursively
        work = [a for a, e in starts.items() if not e]
        extra_entries = set()
        seen_seed = set()
        while work:
            a = work.pop()
            if a in seen_seed or a & 3:
                continue
            seen_seed.add(a)
            ok, rng = inside_fde(a)
            if ok:
                if a != rng[0]:
                    extra_entries.add(a)
                continue
            if a in funcs or not self.mod_of(a):
                continue
            addrs = self.explore(a, known, work)
            if addrs:
                funcs[a] = Func(a, addrs, self.mod_of(a))
                known.add(a)
        # BL to unknown targets -> more seeds
        changed = True
        rounds = 0
        pending = list(funcs.values())
        while changed and rounds < 8:
            changed = False
            rounds += 1
            new = []
            added = []
            for f in pending:
                for x in f.addrs:
                    w, r = self.decode_cf(x)
                    if isinstance(r, tuple) and r[0] in ('bl', 'b', 'bc'):
                        t = r[-1]
                        if isinstance(t, int) and t not in funcs and self.mod_of(t) and t not in seen_seed:
                            if r[0] != 'bl' and t in f.aset:
                                continue
                            ok, rng = inside_fde(t)
                            if ok:
                                if r[0] == 'bl' and t != rng[0]:
                                    extra_entries.add(t)
                                continue
                            if r[0] == 'bl':
                                new.append(t)
                            elif t < f.start or t > f.addrs[-1]:
                                new.append(t)
            for a in new:
                if a in funcs or a in seen_seed:
                    continue
                seen_seed.add(a)
                addrs = self.explore(a, known, None)
                if addrs:
                    nf = Func(a, addrs, self.mod_of(a))
                    funcs[a] = nf
                    known.add(a)
                    added.append(nf)
                    changed = True
            pending = added
        self.funcs = funcs
        self.extra_entries = extra_entries
        self.compute_leaders()
        ni = sum(len(f.addrs) for f in funcs.values())
        say('Functions: %d  instructions: %d' % (len(funcs), ni))
        return funcs

    def explore(self, a, stops, work):
        """walk from a, stopping at other known functions"""
        seen = set()
        todo = [a]
        while todo and len(seen) <= 200000:
            x = todo.pop()
            while True:
                if x in seen or (x != a and x in stops):
                    break
                w, r = self.decode_cf(x)
                if w is None:
                    break
                seen.add(x)
                if r is None:
                    break
                if isinstance(r, tuple):
                    k = r[0]
                    if k == 'bc':
                        t = r[2]
                        if self.mod_of(t):
                            todo.append(t)
                    elif k == 'b':
                        t = r[1]
                        if self.mod_of(t) and t not in stops and abs(t - a) < 0x100000:
                            todo.append(t)
                        break
                    elif k in ('br', 'ret', 'brk'):
                        break
                    elif k == 'bl':
                        if work is not None and self.mod_of(r[1]):
                            work.append(r[1])
                x += 4
                if len(seen) > 200000:
                    break
        return sorted(seen)

    def compute_leaders(self):
        for f in self.funcs.values():
            s = f.aset
            L = {f.start}
            prev_term = False
            for x in f.addrs:
                if prev_term:
                    L.add(x)
                w, r = self.decode_cf(x)
                prev_term = False
                if isinstance(r, tuple):
                    k = r[0]
                    if k in ('b', 'bc'):
                        t = r[-1]
                        if t in s:
                            L.add(t)
                    if k in ('bl', 'blr'):
                        L.add(x + 4)
                    if k in UNCOND:
                        prev_term = True
                    if k == 'br':
                        f.has_br = True
                elif r is None:
                    prev_term = True
            for e in self.extra_entries:
                if e in s:
                    L.add(e)
            for h in self.hints:
                if h in s:
                    L.add(h)
            f.leaders = {x for x in L if x in s}


# ======================================================================= emit
def fix_undef(code, pc, w):
    if 'UNDEF()' not in code:
        return code
    return code.replace('UNDEF()', 'c->rt->undef(c,%s,%s)' % (lit(pc), lit(w)))


def emit_function(prog, f, funcs):
    s = f.addrs
    sset = set(s)
    L = f.leaders
    out = ['void %s(CPU* RESTRICT c, u64 e){' % f.name, ' u8* const M=c->mem;']
    need_d = len(L) > 1
    if need_d:
        out.append(' if(UNLIKELY(e!=%s)){ c->pc=e; goto D_; }' % lit(f.start))
    else:
        out.append(' if(UNLIKELY(e!=%s)){ CALLDYN(e); return; }' % lit(f.start))
    if s and s[0] != f.start:
        out.append(' goto L_%X;' % f.start)

    def tail(t):
        if t in funcs:
            return '{ %s(c,%s); return; }' % (funcs[t].name, lit(t))
        return '{ CALLDYN(%s); return; }' % lit(t)

    def call(t):
        if t in funcs:
            return '%s(c,%s);' % (funcs[t].name, lit(t))
        return 'CALLDYN(%s);' % lit(t)

    for i, x in enumerate(s):
        if x in L:
            out.append('L_%X:' % x)
        w, r = prog.decode(x)
        nxt = x + 4
        if w is None or r is None:
            out.append(' c->pc=%s; c->rt->undef(c,%s,%s); return;' % (lit(x), lit(x), lit(w or 0)))
            continue
        if isinstance(r, str):
            if r:
                out.append(' ' + fix_undef(r, x, w))
        else:
            k = r[0]
            if k == 'b':
                t = r[1]
                out.append(' goto L_%X;' % t if t in sset else ' ' + tail(t))
                continue
            if k == 'bc':
                cnd, t = r[1], r[2]
                if cnd == '1':
                    out.append(' goto L_%X;' % t if t in sset else ' ' + tail(t))
                    continue
                out.append(' if(%s) %s' % (cnd, 'goto L_%X;' % t if t in sset else tail(t)))
            elif k == 'bl':
                t = r[1]
                out.append(' c->x[30]=%s; %s if(UNLIKELY(c->pc!=%s)) return;' % (lit(nxt), call(t), lit(nxt)))
            elif k == 'blr':
                out.append(
                    ' { u64 t_=%s; c->x[30]=%s; CALLDYN(t_); if(UNLIKELY(c->pc!=%s)) return; }'
                    % (r[1], lit(nxt), lit(nxt))
                )
            elif k == 'br':
                if need_d:
                    out.append(' c->pc=%s; goto D_;' % r[1])
                else:
                    out.append(' { u64 t_=%s; CALLDYN(t_); return; }' % r[1])
                continue
            elif k == 'ret':
                out.append(' c->pc=%s; return;' % r[1])
                continue
            elif k == 'svc':
                out.append(' c->pc=%s; c->rt->svc(c,%s);' % (lit(nxt), r[1]))
            elif k == 'brk':
                out.append(' c->pc=%s; c->rt->brk(c,%s,%s); return;' % (lit(x), lit(x), r[1]))
                continue
        # fell off the end
        if nxt not in sset or (i + 1 < len(s) and s[i + 1] != nxt):
            out.append(' ' + tail(nxt))
    if need_d:
        out.append('D_: switch(c->pc){')
        for x in sorted(L):
            out.append('  case %s: goto L_%X;' % (lit(x), x))
        out.append('  default: CALLDYN(c->pc); return; }')
    out.append('}')
    return '\n'.join(out)


# ======================================================================= interpreter
def gen_interp(step_mode=False):
    """step_mode: execute a single instruction and return the next pc (for differential tests against an emulator,
    which live outside the repo)"""
    bodies = a64.gen_interp_cases()
    L = []
    fname = (
        'u64 snx_step(CPU* RESTRICT c, u32 w, u64 pc)'
        if step_mode
        else 'static void snx_interp(CPU* RESTRICT c, u64 e)'
    )
    L.append(fname + '{')
    L.append(' u8* const M=c->mem;')
    if not step_mode:
        L.append(' u64 pc=e; u32 w; if(c->rt->note_hint) c->rt->note_hint(e);')
        L.append(' for(;;){')
        L.append('  w=RD32(pc);')
    L.append('  switch(w>>21){')
    buckets = a64.build_buckets()
    for top in range(1 << 11):
        lst = buckets[top]
        if not lst:
            continue
        L.append('  case %d:' % top)
        for i in lst:
            name, m, v, spec, fn = TABLE[i]
            L.append('   if((w&%su)==%su) goto E_%d;' % (hex(m), hex(v), i))
        L.append('   goto U_;')
    L.append('  default: goto U_; }')
    for i, (name, m, v, spec, fn) in enumerate(TABLE):
        L.append(' E_%d: /* %s */' % (i, name))
        cases = bodies[i]
        if spec:
            L.append('  switch(%s){' % a64.spec_index_expr(spec))
        for idx, (combo, decl, r) in enumerate(cases):
            body = interp_body(r, step_mode)
            if spec:
                L.append('  case %d: { %s%s }' % (idx, decl, body))
            else:
                L.append('  { %s%s }' % (decl, body))
        if spec:
            L.append('  default: goto U_; }')
    if step_mode:
        L.append(' U_: c->rt->undef(c,pc,w); return pc;')
        L.append(' N_: return pc+4;')
        L.append('}')
    else:
        L.append(' U_: c->pc=pc; c->rt->undef(c,pc,w); return;')
        L.append(' N_: pc+=4; continue;')
        L.append(' J_: { GuestFn f_=snx_lookup(c,pc); if(f_!=snx_interp_entry){ f_(c,pc); return; } continue; }')
        L.append(' }')
        L.append('}')
    return '\n'.join(L)


def interp_body(r, step):
    if r is None:
        return 'goto U_;'
    if isinstance(r, str):
        return (r + ' goto N_;').replace('UNDEF()', 'goto U_')
    k = r[0]
    if step:
        if k == 'b':
            return 'return %s;' % r[1]
        if k == 'bc':
            return 'if(%s) return %s; goto N_;' % (r[1], r[2])
        if k == 'bl':
            return '{ u64 t_=%s; c->x[30]=pc+4; return t_; }' % r[1]
        if k == 'blr':
            return '{ u64 t_=%s; c->x[30]=pc+4; return t_; }' % r[1]
        if k in ('br', 'ret'):
            return 'return %s;' % r[1]
        if k == 'svc':
            return 'c->pc=pc+4; c->rt->svc(c,%s); goto N_;' % r[1]
        if k == 'brk':
            return 'c->rt->brk(c,pc,%s); return pc;' % r[1]
    if k == 'b':
        return 'pc=%s; goto J_;' % r[1]
    if k == 'bc':
        return 'if(%s){ pc=%s; goto J_; } goto N_;' % (r[1], r[2])
    if k == 'bl':
        return '{ u64 t_=%s; c->x[30]=pc+4; CALLDYN(t_); if(c->pc!=pc+4) return; goto N_; }' % r[1]
    if k == 'blr':
        return '{ u64 t_=%s; c->x[30]=pc+4; CALLDYN(t_); if(c->pc!=pc+4) return; goto N_; }' % r[1]
    if k == 'br':
        return 'pc=%s; goto J_;' % r[1]
    if k == 'ret':
        return 'c->pc=%s; return;' % r[1]
    if k == 'svc':
        return 'c->pc=pc+4; c->rt->svc(c,%s); goto N_;' % r[1]
    if k == 'brk':
        return 'c->pc=pc; c->rt->brk(c,pc,%s); return;' % r[1]
    raise ValueError(k)


def emit_static_step(w, pc, name):
    """single instruction as a function that returns the next pc (for differential tests against an emulator, which
    live outside the repo)"""
    r = a64.emit_static(w, pc)
    s = ['u64 %s(CPU* RESTRICT c){ u8* const M=c->mem; (void)M;' % name]
    if r is None:
        s.append(' c->rt->undef(c,%s,%s); return %s;' % (lit(pc), lit(w), lit(pc)))
    elif isinstance(r, str):
        s.append(' ' + fix_undef(r, pc, w) + ' return %s;' % lit(pc + 4))
    else:
        k = r[0]
        if k == 'b':
            s.append(' return %s;' % lit(r[1]))
        elif k == 'bc':
            s.append(' if(%s) return %s; return %s;' % (r[1], lit(r[2]), lit(pc + 4)))
        elif k in ('bl',):
            s.append(' c->x[30]=%s; return %s;' % (lit(pc + 4), lit(r[1])))
        elif k == 'blr':
            s.append(' { u64 t_=%s; c->x[30]=%s; return t_; }' % (r[1], lit(pc + 4)))
        elif k in ('br', 'ret'):
            s.append(' return %s;' % r[1])
        else:
            s.append(' return %s;' % lit(pc + 4))
    s.append('}')
    return '\n'.join(s)
