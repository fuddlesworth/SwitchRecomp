# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""CIA -> contents/ExeFS/RomFS, failure atomicity and bounded malformed fixtures."""

import hashlib
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from ctr_fixtures import TITLE, NONE, cia, ncch, put


def main():
    driver = str(Path(sys.argv[1]).resolve())
    cases = 0
    with tempfile.TemporaryDirectory(prefix='snx-ctr-extract-') as temp:
        root = Path(temp)
        source = root / 'input with spaces;literal.CIA'

        def run(blob, status=0, message='Extracted', output=None, extra=(), explicit=True, env=None):
            nonlocal cases
            cases += 1
            source.write_bytes(blob)
            output = output or root / f'out-{cases}'
            args = [driver, *extra]
            if explicit:
                args += ['-x', '--output', str(output)]
            args += [str(source)]
            p = subprocess.run(args, capture_output=True, text=True, cwd=root, timeout=20, env=env)
            text = p.stdout + p.stderr
            assert p.returncode == status, (cases, args, p.returncode, text)
            assert message in text, (cases, message, text)
            assert 'ScarletNX - ' not in text and 'AddressSanitizer' not in text and 'runtime error:' not in text, text
            assert not list(root.rglob('.snx-*')), 'staging directory leaked'
            if status and explicit:
                assert not output.exists(), (cases, output)
            return output, text

        plain = cia()
        output, _ = run(plain)
        assert (output / 'romfs/hello.txt').read_bytes() == b'hello'
        assert (output / 'romfs/dir/é😀.bin').read_bytes() == b'DATA'
        assert (output / 'exefs/icon').read_bytes() == b'ICON'
        code = (output / 'exefs/.code').read_bytes()
        assert len(code) == 0x3000 and code[:8] == struct.pack('<II', 0xE3A0002A, 0xEF000003)
        manifest = dict(line.split('=', 1) for line in (output / 'manifest.ini').read_text().splitlines())
        assert manifest['state'] == 'extracted' and manifest['title_id'] == f'{TITLE:016x}'
        assert manifest['input_sha256'] == hashlib.sha256(plain).hexdigest()
        assert manifest['code_sha256'] == hashlib.sha256(code).hexdigest()
        assert sorted(p.name for p in (output / 'contents').iterdir()) == ['00001111.app', '00002222.app']
        assert (output / 'contents/00002222.app').read_bytes() == ncch()
        before = {p.relative_to(output): p.read_bytes() for p in output.rglob('*') if p.is_file()}
        source.write_bytes(plain)
        p = subprocess.run(
            [driver, '-x', '--output', str(output), str(source)], capture_output=True, text=True, timeout=20
        )
        assert p.returncode == 1 and 'output already exists' in p.stderr
        assert before == {p.relative_to(output): p.read_bytes() for p in output.rglob('*') if p.is_file()}
        # Signature-dependent body positions, absent content and optional metadata.
        for sig in [0x10000, 0x10001, 0x10002, 0x10003, 0x10004, 0x10005]:
            run(cia(signature=sig, with_meta=False))
        compressed, _ = run(cia(application=ncch(compressed=True)))
        assert (compressed / 'exefs/.code').read_bytes() == b'A' * 0x3000
        no_rom, _ = run(cia(application=ncch(with_romfs=False)))
        assert (no_rom / 'romfs').is_dir() and not list((no_rom / 'romfs').iterdir())
        run(cia(manual_size=1024 * 1024 + 19))  # streaming, not whole-content allocation
        certs, _ = run(cia(certificates=b'SYNTHETIC-CERTIFICATE'))
        assert (certs / 'certificates.bin').read_bytes() == b'SYNTHETIC-CERTIFICATE'
        assert (certs / 'cia-header.bin').stat().st_size == 0x2020
        trimmed, _ = run(cia(application=ncch(code_blob=code[:0x2004])))
        assert (trimmed / 'exefs/.code').read_bytes() == code
        bss_only, _ = run(
            cia(
                application=ncch(
                    code_blob=code[:0x2000], patch_exheader=lambda e: put(e, 0x30, '<4I', 0x300000, 0, 0, 0x1000)
                )
            )
        )
        assert 'data_pages=0' in (bss_only / 'manifest.ini').read_text()

        def empty_icon(e):
            put(e, 28, '<I', 0)
            e[0x1C0:0x1E0] = hashlib.sha256(b'').digest()

        empty, _ = run(cia(application=ncch(patch_exefs=empty_icon)))
        assert (empty / 'exefs/icon').read_bytes() == b''
        # Default path is content addressed, in the data folder's work/3ds; extraction succeeds but execution is not
        # claimed.
        name = f'{TITLE:016x}-{hashlib.sha256(plain).hexdigest()}'
        data = root / 'data'
        _, text = run(
            plain,
            3,
            'static compilation/execution is not implemented',
            extra=('--repo', str(data)),
            explicit=False,
        )
        expected = data.resolve() / 'work/3ds' / name
        assert expected.is_dir() and str(expected) in text
        if sys.platform != 'win32' and sys.platform != 'darwin':
            # An installed build's (its folder is the user's data folder) is in the XDG cache folder
            env = {**os.environ, 'XDG_DATA_HOME': str(root / 'xdg-data'), 'XDG_CACHE_HOME': str(root / 'xdg-cache')}
            _, text = run(
                plain,
                3,
                'static compilation/execution is not implemented',
                extra=('--repo', str(root / 'xdg-data/scarletnx')),
                explicit=False,
                env=env,
            )
            expected = (root / 'xdg-cache').resolve() / 'scarletnx/work/3ds' / name
            assert expected.is_dir() and str(expected) in text
            assert not (root / 'xdg-data/scarletnx/work').exists()
        # Requested extraction alone may succeed without a runtime; normal launches stop before AOT.
        run(cia(encrypted=True), 3, 'encrypted CIA')
        run(cia(application=ncch(encrypted=True)), 3, 'encrypted NCCH')
        run(cia(title=0x0004000E01234500), 3, 'updates/DLC/system')
        run(cia(ticket_title=TITLE + 1), 1, 'title ID mismatch')
        run(cia(boot=7), 1, 'boot content')
        run(cia(info_ranges=[(0, 2), (1, 2)]), 1, 'overlapping TMD')
        run(cia(info_ranges=[(0, 2)]), 1, 'uncovered or duplicate')
        run(cia(patch_chunks=lambda c: put(c, 0x34, '>H', 0)), 1, 'duplicate TMD content index')
        run(cia(patch_chunks=lambda c: put(c, 0x30, '>I', 0x1111)), 1, 'duplicate TMD content ID')
        run(cia(patch_chunks=lambda c: put(c, 0x38, '>Q', 2**64 - 1)), 1, 'content length')
        bad_bitmap = bytearray(plain)
        bad_bitmap[0x21] |= 0x80
        run(bad_bitmap, 1, 'bitmap/content sizes')
        bad_hash = bytearray(plain)
        bad_hash[-0x400 - 100] ^= 1  # inside NCCH, before metadata (padding may surround it)
        run(bad_hash, 1, 'SHA-256 mismatch')
        run(cia(patch_body=lambda b: b.__setitem__(0xA4, b[0xA4] ^ 1)), 1, 'content-info SHA-256')
        run(cia(application=ncch(patch_exheader=lambda e: put(e, 0x10, '<I', 0x100001))), 1, 'segment/BSS')
        run(cia(application=ncch(patch_exheader=lambda e: put(e, 0x20, '<I', 0x100000))), 1, 'overlapping process')
        run(cia(application=ncch(patch_exheader=lambda e: put(e, 0x14, '<I', 0xFFFFFFFF))), 1, 'segment/BSS')
        run(cia(application=ncch(patch_exheader=lambda e: put(e, 0x3C, '<I', 0xFFFFFFFF))), 1, 'segment/BSS')
        run(cia(application=ncch(patch_header=lambda h: put(h, 0x1B0, '<I', 5))), 1, 'overlapping NCCH')
        run(cia(application=ncch(patch_header=lambda h: h.__setitem__(0x18E, 31))), 3, 'media unit/platform')
        run(cia(application=ncch(patch_exefs=lambda e: e.__setitem__(slice(0, 8), b'../bad\0\0'))), 1, 'unsafe ExeFS')
        run(cia(application=ncch(patch_exefs=lambda e: put(e, 8, '<I', 0xFFFFFFFF))), 1, 'ExeFS file exceeds')
        run(cia(application=ncch(patch_exefs=lambda e: e.__setitem__(6, ord('X')))), 1, 'name encoding/padding')
        run(cia(application=ncch(patch_exefs=lambda e: e.__setitem__(0, 0xFF))), 1, 'name encoding/padding')
        run(
            cia(application=ncch(patch_exefs=lambda e: e.__setitem__(slice(16, 24), b'.code\0\0\0'))),
            1,
            'duplicate or overlapping',
        )
        run(cia(application=ncch(patch_exefs=lambda e: e.__setitem__(0x200, 0))), 1, 'SHA-256 mismatch')
        run(cia(application=ncch(compressed=True, code_blob=b'invalid!')), 1, 'BLZ')
        for name in [
            '..',
            '../escape',
            '/absolute',
            'a\\b',
            'a:b',
            'CON',
            'NUL.bin',
            'COM1.txt',
            'COM¹',
            'LPT².txt',
            'CONOUT$',
            'CLOCK$',
            'trail.',
            'x\0y',
            '\ud800',
        ]:
            run(cia(application=ncch(romfs_options={'file_name': name})), 1, 'invalid RomFS file parent/name')
        run(cia(application=ncch(romfs_options={'directory_name': '..'})), 1, 'unsafe RomFS directory')
        run(cia(application=ncch(romfs_options={'duplicate': True})), 1, 'cannot create new output file')
        run(cia(application=ncch(romfs_options={'patch': lambda h, d, f: put(d, 24 + 8, '<I', 24)})), 1, 'cyclic RomFS')
        run(cia(application=ncch(romfs_options={'patch': lambda h, d, f: put(f, 4, '<I', 0)})), 1, 'cyclic RomFS')
        run(cia(application=ncch(romfs_options={'patch': lambda h, d, f: put(d, 8, '<I', 28)})), 1, 'directory link')
        run(
            cia(application=ncch(romfs_options={'patch': lambda h, d, f: put(f, 8, '<Q', 2**64 - 1)})),
            1,
            'file data exceeds',
        )
        run(
            cia(application=ncch(romfs_options={'patch': lambda h, d, f: put(h, 0x1C, '<I', 0xFFFFFFFC)})),
            1,
            'table range',
        )
        run(
            cia(application=ncch(romfs_options={'patch': lambda h, d, f: put(f, 28, '<I', 0xFFFFFFFE)})),
            1,
            'name length',
        )
        for n in [0, 1, 31, 0x201F, 0x2041, len(plain) // 2, len(plain) - 1]:
            run(plain[:n], 1, 'scarletnx-3ds:')
        if os.name != 'nt':
            outside = root / 'outside'
            outside.mkdir()
            link = root / 'linked'
            link.symlink_to(outside, target_is_directory=True)
            run(plain, 1, 'symlink ancestor', output=link / 'escape')
            assert not list(outside.iterdir())
            dangling = root / 'dangling'
            dangling.symlink_to(root / 'missing')
            run(plain, 1, 'symlink ancestor', output=dangling)
        # Reproducible mutation smoke test: a changed ignored/padding field may still be valid.
        rng = random.Random(0x435452)
        for i in range(40):
            mutant = bytearray(plain)
            for _ in range(1 + i % 4):
                at = rng.randrange(len(mutant))
                mutant[at] ^= 1 << rng.randrange(8)
            source.write_bytes(mutant)
            out = root / f'mutant-{i}'
            p = subprocess.run(
                [driver, '-x', '--output', str(out), str(source)], capture_output=True, text=True, timeout=20
            )
            assert p.returncode in [0, 1, 3], (i, p.returncode, p.stderr)
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr, p.stderr
            assert not list(root.rglob('.snx-*'))
            if p.returncode:
                assert not out.exists()
            else:
                shutil.rmtree(out)
        # Recompute all outer hashes so mutations reach the inner RomFS parser.
        for i in range(48):
            choice = i % 3
            offsets = [list(range(0, 0x28, 4)), list(range(0, 24, 4)), list(range(0, 32, 4))][choice]
            offset = rng.choice(offsets)
            value = rng.choice([0, NONE, 1, 4, 24, 32, 0xFFFFFFF0])

            def patch(h, d, f):
                put([h, d, f][choice], offset, '<I', value)

            source.write_bytes(cia(application=ncch(romfs_options={'patch': patch})))
            out = root / f'inner-mutant-{i}'
            p = subprocess.run(
                [driver, '-x', '--output', str(out), str(source)], capture_output=True, text=True, timeout=20
            )
            assert p.returncode in [0, 1, 3], (i, p.returncode, p.stderr)
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr, p.stderr
            assert not list(root.rglob('.snx-*'))
            if p.returncode:
                assert not out.exists()
            else:
                shutil.rmtree(out)
    print(f'3DS CIA/NCCH/ExeFS/BLZ/RomFS extraction: {cases} fixtures + 88 mutations passed')


if __name__ == '__main__':
    main()
