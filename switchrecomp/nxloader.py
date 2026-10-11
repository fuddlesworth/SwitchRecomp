# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
"""
nxloader: NSO0/NRO0, MOD0, dynamic section, symbols, relocs, .eh_frame.
Load addresses are decided here and baked into the generated code, the C runtime
loads modules in the same order.
"""

import os, struct, hashlib

try:
    import lz4.block as _lz4
except Exception:  # no lz4 module, use the slow pure python one
    _lz4 = None

CODE_BASE = 0x08000000  # same as hw (39-bit AS)
PAGE = 0x1000


def lz4_block(src, size):
    if _lz4:
        return _lz4.decompress(src, uncompressed_size=size)
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        tok = src[i]
        i += 1
        l = tok >> 4
        if l == 15:
            while True:
                b = src[i]
                i += 1
                l += b
                if b != 255:
                    break
        out += src[i : i + l]
        i += l
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        m = tok & 15
        if m == 15:
            while True:
                b = src[i]
                i += 1
                m += b
                if b != 255:
                    break
        m += 4
        p = len(out) - off
        for k in range(m):
            out.append(out[p + k])
    return bytes(out)


def align(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


class Module:
    """decompressed module image (text|ro|data|bss)"""

    def __init__(self, name, image, text, ro, data, bss, build_id=b''):
        self.name = name
        self.image = image
        self.text = text
        self.ro = ro
        self.data = data  # (offset, size)
        self.bss = bss  # size
        self.build_id = build_id
        self.size = align(max(data[0] + data[1], len(image)) + bss)
        self.base = 0
        self.syms = {}
        self.funcs = set()
        self.fde = []
        self.imports = []
        self.relptrs = set()
        self.mod0 = None
        self.parse()

    # ---------------------------------------------------------------- parsing
    def u32(self, o):
        return struct.unpack_from('<I', self.image, o)[0]

    def u64(self, o):
        return struct.unpack_from('<Q', self.image, o)[0]

    def s32(self, o):
        return struct.unpack_from('<i', self.image, o)[0]

    def word(self, off):
        if 0 <= off <= len(self.image) - 4:
            return struct.unpack_from('<I', self.image, off)[0]
        return None

    def in_text(self, off):
        return self.text[0] <= off < self.text[0] + self.text[1] and not off & 3

    def parse(self):
        img = self.image
        m0 = self.u32(4)
        if m0 + 0x1C <= len(img) and img[m0 : m0 + 4] == b'MOD0':
            dyn = m0 + self.s32(m0 + 4)
            self.mod0 = dict(
                off=m0,
                dynamic=dyn,
                bss_start=m0 + self.s32(m0 + 8),
                bss_end=m0 + self.s32(m0 + 12),
                ehf_start=m0 + self.s32(m0 + 16),
                ehf_end=m0 + self.s32(m0 + 20),
            )
            self.parse_dynamic(dyn)
            self.parse_eh(self.mod0['ehf_start'], self.mod0['ehf_end'])

    def parse_dynamic(self, off):
        d = {}
        relas = []
        tags = []
        while off + 16 <= len(self.image):
            tag, val = struct.unpack_from('<qQ', self.image, off)
            off += 16
            if tag == 0:
                break
            tags.append((tag, val))
            d.setdefault(tag, val)
        self.dyn = d
        strtab = d.get(5)
        symtab = d.get(6)
        strsz = d.get(10, 0)
        syment = d.get(11, 24)
        hashoff = d.get(4)
        gnuhash = d.get(0x6FFFFEF5)
        nsyms = 0
        if hashoff is not None:
            nsyms = self.u32(hashoff + 4)
        elif strtab and symtab and strtab > symtab:
            nsyms = (strtab - symtab) // syment
        self.symlist = []
        if symtab is not None and strtab is not None:
            for i in range(nsyms):
                o = symtab + i * syment
                if o + 24 > len(self.image):
                    break
                nm, info, oth, shn, val, size = struct.unpack_from('<IBBHQQ', self.image, o)
                e = self.image.find(b'\0', strtab + nm)
                name = self.image[strtab + nm : e].decode('latin1') if nm else ''
                self.symlist.append((name, info, shn, val, size))
                if shn and name and (info & 15) in (1, 2, 0) and val:
                    self.syms[val] = name
                    if (info & 15) == 2 and self.in_text(val):
                        self.funcs.add(val)
                elif shn == 0 and name:
                    self.imports.append(name)
        # relocs pointing into code (vtables, callbacks)
        for rt, rs in ((7, 8), (0x17, 2)):  # DT_RELA/RELASZ, DT_JMPREL/PLTRELSZ
            base = d.get(rt)
            size = d.get(rs if rt == 7 else 2, 0)
            if base is None:
                continue
            for o in range(base, base + size, 24):
                if o + 24 > len(self.image):
                    break
                r_off, r_info, r_add = struct.unpack_from('<QQq', self.image, o)
                typ = r_info & 0xFFFFFFFF
                if typ == 1027 and self.in_text(r_add):  # R_AARCH64_RELATIVE
                    self.relptrs.add(r_add)
        # absolute pointers already in .data (rare for NSO, shows up in NRO)

    # ---------------------------------------------------------------- eh_frame
    def read_enc(self, o, enc, base_for_pcrel=None, datarel=0):
        if enc == 0xFF:
            return None, o
        fmt = enc & 0x0F
        app = enc & 0x70
        if fmt == 0x00:
            v = self.u64(o)
            o2 = o + 8
        elif fmt == 0x01:
            v, o2 = self.uleb(o)
        elif fmt == 0x02:
            v = struct.unpack_from('<H', self.image, o)[0]
            o2 = o + 2
        elif fmt == 0x03:
            v = self.u32(o)
            o2 = o + 4
        elif fmt == 0x04:
            v = self.u64(o)
            o2 = o + 8
        elif fmt == 0x09:
            v, o2 = self.sleb(o)
        elif fmt == 0x0A:
            v = struct.unpack_from('<h', self.image, o)[0]
            o2 = o + 2
        elif fmt == 0x0B:
            v = self.s32(o)
            o2 = o + 4
        elif fmt == 0x0C:
            v = struct.unpack_from('<q', self.image, o)[0]
            o2 = o + 8
        else:
            raise ValueError('enc %x' % enc)
        if app == 0x10:
            v += o
        elif app == 0x30:
            v += datarel
        return v & 0xFFFFFFFFFFFFFFFF, o2

    def uleb(self, o):
        r = 0
        s = 0
        while True:
            b = self.image[o]
            o += 1
            r |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                return r, o

    def sleb(self, o):
        r = 0
        s = 0
        while True:
            b = self.image[o]
            o += 1
            r |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                if b & 0x40:
                    r -= 1 << s
                return r, o

    def parse_eh(self, hs, he):
        if not (0 < hs < he <= len(self.image)):
            return
        try:
            ver, pe, ce, te = self.image[hs : hs + 4]
            if ver != 1:
                return
            o = hs + 4
            ehf, o = self.read_enc(o, pe, datarel=hs)
            cnt, o = self.read_enc(o, ce, datarel=hs)
            cies = {}
            for i in range(cnt or 0):
                loc, o = self.read_enc(o, te, datarel=hs)
                fde, o = self.read_enc(o, te, datarel=hs)
                r = self.parse_fde(fde, cies)
                if r:
                    self.fde.append(r)
        except (struct.error, IndexError, ValueError):
            pass
        for s, n in self.fde:
            if self.in_text(s):
                self.funcs.add(s)

    def parse_fde(self, o, cies):
        ln = self.u32(o)
        if ln in (0, 0xFFFFFFFF):
            return None
        cie_ptr = o + 4 - self.u32(o + 4)
        if cie_ptr not in cies:
            cies[cie_ptr] = self.parse_cie(cie_ptr)
        enc = cies[cie_ptr]
        p = o + 8
        start, p = self.read_enc(p, enc)
        rng, p = self.read_enc(p, enc & 0x0F)
        return (start & 0xFFFFFFFF, rng)

    def parse_cie(self, o):
        p = o + 8
        ver = self.image[p]
        p += 1
        e = self.image.find(b'\0', p)
        aug = self.image[p:e].decode('latin1')
        p = e + 1
        _, p = self.uleb(p)
        _, p = self.sleb(p)
        if ver == 1:
            p += 1
        else:
            _, p = self.uleb(p)
        enc = 0
        if aug.startswith('z'):
            _, p = self.uleb(p)
            for ch in aug[1:]:
                if ch == 'R':
                    enc = self.image[p]
                    p += 1
                elif ch == 'P':
                    pe = self.image[p]
                    p += 1
                    _, p = self.read_enc(p, pe)
                elif ch == 'L':
                    p += 1
        return enc


def load_nso(path, name=None):
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0':
        raise ValueError('%s is not NSO0' % path)
    flags = struct.unpack_from('<I', d, 0xC)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    fsz = struct.unpack_from('<III', d, 0x60)
    bss = struct.unpack_from('<I', d, 0x3C)[0]
    size = segs[2][1] + segs[2][2]
    img = bytearray(size)
    for i, (foff, moff, sz) in enumerate(segs):
        raw = d[foff : foff + fsz[i]]
        if flags & (1 << i):
            raw = lz4_block(raw, sz)
        img[moff : moff + sz] = raw[:sz]
        if flags & (8 << i):
            h = hashlib.sha256(raw[:sz]).digest()
            if h != d[0xA0 + i * 0x20 : 0xC0 + i * 0x20]:
                raise ValueError('%s: hash of segment %d does not match (damaged file?)' % (path, i))
    return Module(
        name or os.path.basename(path),
        bytes(img),
        (segs[0][1], segs[0][2]),
        (segs[1][1], segs[1][2]),
        (segs[2][1], segs[2][2]),
        bss,
        d[0x40:0x60].rstrip(b'\0'),
    )


def load_nro(path):
    d = open(path, 'rb').read()
    if d[0x10:0x14] != b'NRO0':
        raise ValueError('%s is not NRO0' % path)
    size = struct.unpack_from('<I', d, 0x18)[0]
    segs = [struct.unpack_from('<II', d, 0x20 + i * 8) for i in range(3)]
    bss = struct.unpack_from('<I', d, 0x38)[0]
    img = d[:size]
    m = Module('main', img, segs[0], segs[1], segs[2], bss, d[0x40:0x60].rstrip(b'\0'))
    m.nro = True
    m.asset = d[size:]  # icon / nacp / embedded romfs
    return m


def nro_romfs(m):
    """romfs from the NRO ASET section"""
    a = getattr(m, 'asset', b'')
    if len(a) < 0x38 or a[:4] != b'ASET':
        return None
    off, sz = struct.unpack_from('<QQ', a, 0x28)
    return a[off : off + sz] if sz else None


MODULE_ORDER = ['rtld', 'main'] + ['subsdk%d' % i for i in range(10)] + ['sdk']


def find_exefs(folder):
    """game dir (with exefs/ + romfs/) or the exefs dir itself"""
    for c in (os.path.join(folder, 'exefs'), os.path.join(folder, 'ExeFS'), folder):
        if os.path.isfile(os.path.join(c, 'main')):
            return c
    return None


def load_program(entry):
    """-> (modules sorted with bases assigned, npdm bytes, romfs path or bytes)"""
    mods = []
    npdm = None
    romfs = None
    if os.path.isfile(entry) and entry.lower().endswith('.nro'):
        m = load_nro(entry)
        mods = [m]
        romfs = nro_romfs(m)
    else:
        ex = find_exefs(entry)
        if not ex:
            raise SystemExit('Cannot find exefs/main in %s' % entry)
        for n in MODULE_ORDER:
            p = os.path.join(ex, n)
            if os.path.isfile(p):
                mods.append(load_nso(p, n))
        if os.path.isfile(os.path.join(ex, 'main.npdm')):
            npdm = open(os.path.join(ex, 'main.npdm'), 'rb').read()
        root = os.path.dirname(ex) if os.path.basename(ex).lower() == 'exefs' else ex
        for c in ('romfs', 'RomFS'):
            if os.path.isdir(os.path.join(root, c)):
                romfs = os.path.join(root, c)
    base = CODE_BASE
    for m in mods:
        m.base = base
        base += m.size + PAGE  # guard page
    return mods, npdm, romfs


def parse_npdm(d):
    """bits of main.npdm we care about: stack size, priority, core, is64"""
    r = dict(stack=0x100000, prio=44, core=0, is64=True, aspace=3, title_id=0x0100000000001000, name='Application')
    if not d or d[:4] != b'META':
        return r
    r['is64'] = bool(d[0xC] & 1)
    r['aspace'] = (d[0xC] >> 1) & 7
    r['prio'] = d[0xE]
    r['core'] = d[0xF]
    r['stack'] = struct.unpack_from('<I', d, 0x1C)[0] or 0x100000
    r['name'] = d[0x20:0x30].split(b'\0')[0].decode('latin1')
    aoff = struct.unpack_from('<I', d, 0x70)[0]
    if d[aoff : aoff + 4] == b'ACI0':
        r['title_id'] = struct.unpack_from('<Q', d, aoff + 0x10)[0]
    return r


if __name__ == '__main__':
    import sys

    mods, npdm, romfs = load_program(sys.argv[1])
    for m in mods:
        print(
            '%-8s base=%010X size=%08X text=%X ro=%X data=%X bss=%X syms=%d funcs=%d fde=%d relptr=%d imports=%d'
            % (
                m.name,
                m.base,
                m.size,
                m.text[1],
                m.ro[1],
                m.data[1],
                m.bss,
                len(m.syms),
                len(m.funcs),
                len(m.fde),
                len(m.relptrs),
                len(m.imports),
            )
        )
    print(parse_npdm(npdm))
    print('romfs', romfs if isinstance(romfs, (str, type(None))) else len(romfs))
