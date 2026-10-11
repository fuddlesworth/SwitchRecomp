#!/usr/bin/env python3
"""Names game addresses from the modules' symbol tables (demangled).

    tools/symbols.py work/Catherine cf34380 82f161c ...

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
        return '%x %s %s' % (addr, self.module(addr), '%s+%x' % hit if hit else '?')

    def function_range(self, name):
        """Start and end of the first symbol whose name contains `name`. The end is the next symbol."""
        for i, n in enumerate(self.names):
            if name in n:
                end = self.addrs[i + 1] if i + 1 < len(self.addrs) else self.addrs[i] + 0x400
                return self.addrs[i], min(end, self.addrs[i] + MAX_DISTANCE)
        raise SystemExit('no symbol contains %r' % name)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mods, _, _ = nxloader.load_program(sys.argv[1])
    syms = Symbols(mods)
    for arg in sys.argv[2:]:
        print(syms.describe(int(arg, 16)))


if __name__ == '__main__':
    main()
