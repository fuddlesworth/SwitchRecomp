#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
"""Names game addresses from the modules' symbol tables (demangled).

    tools/symbols.py work/Game cf34380 82f161c ...

Crash reports and thread dumps print runtime addresses: the modules are laid out at the same bases here.
"""

import bisect
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..')]
from switchrecomp import nxloader  # noqa: E402

# A symbol further away than this is not taken as the enclosing function
MAX_DISTANCE = 0x4000


class Symbols:
    def __init__(self, mods):
        self.mods = mods
        table = []
        for m in mods:
            for value, name in getattr(m, 'syms', {}).items():
                if value:
                    table.append((m.base + value, name))
        table.sort()
        self.addrs = [a for a, _ in table]
        self.names = self._demangle([n for _, n in table])

    @staticmethod
    def _demangle(names):
        try:
            out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True, check=True).stdout
            demangled = out.split('\n')
            if len(demangled) >= len(names):
                return demangled[: len(names)]
        except (OSError, subprocess.CalledProcessError):
            pass
        return names

    def module(self, addr):
        return next((m.name for m in self.mods if m.base <= addr < m.base + m.size), '?')

    def lookup(self, addr):
        """(name, offset) of the symbol at or before addr, or None."""
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i >= 0 and addr - self.addrs[i] < MAX_DISTANCE:
            return self.names[i], addr - self.addrs[i]
        return None

    def exact(self, addr):
        i = bisect.bisect_left(self.addrs, addr)
        return self.names[i] if i < len(self.addrs) and self.addrs[i] == addr else None

    def describe(self, addr):
        hit = self.lookup(addr)
        if hit:
            return '%x %s %s+%x' % (addr, self.module(addr), hit[0], hit[1])
        # Too far from the symbol before it to be inside that function: say which one it was
        i = bisect.bisect_right(self.addrs, addr) - 1
        far = ' (%s+%x is past MAX_DISTANCE)' % (self.names[i], addr - self.addrs[i]) if i >= 0 else ''
        return '%x %s ?%s' % (addr, self.module(addr), far)

    def function_range(self, name):
        """Start and end of the symbol named `name` (a demangled name without its parameters counts), or else of the
        one symbol whose name contains it. The end is the next symbol, at most MAX_DISTANCE further."""
        exact = {i for i, n in enumerate(self.names) if n == name or n.split('(')[0] == name}
        found = exact or {i for i, n in enumerate(self.names) if name in n}
        starts = sorted({self.addrs[i] for i in found})
        if not starts:
            raise SystemExit('no symbol contains %r' % name)
        if len(starts) > 1:
            listed = sorted({(self.addrs[i], self.names[i]) for i in found})
            lines = ['  %x %s' % hit for hit in listed[:30]] + (['  ...'] if len(listed) > 30 else [])
            raise SystemExit('%d symbols match %r, name one exactly:\n%s' % (len(starts), name, '\n'.join(lines)))
        start = starts[0]
        j = bisect.bisect_right(self.addrs, start)  # the next symbol at a higher address
        end = self.addrs[j] if j < len(self.addrs) else start + 0x400
        if end - start > MAX_DISTANCE:
            print(
                '(no symbol follows %s for %#x bytes: showing the first %#x, pass an address range for more)'
                % (name, end - start, MAX_DISTANCE),
                file=sys.stderr,
            )
            end = start + MAX_DISTANCE
        return start, end


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mods, _, _ = nxloader.load_program(sys.argv[1])
    syms = Symbols(mods)
    for arg in sys.argv[2:]:
        print(syms.describe(int(arg, 16)))


if __name__ == '__main__':
    main()
