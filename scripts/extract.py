#!/usr/bin/env python3
"""Extracts a game dump into the exefs/ + romfs/ folder that switchrecomp.py reads, with hactool from
scripts/bootstrap.py.

    scripts/extract.py "Game.xci"                                   # -> work/Game/{exefs,romfs}
    scripts/extract.py game.nsp --update update.nsp --name Game     # base and update combined
    scripts/extract.py game.xci --keys ~/keys/prod.keys --title-keys ~/keys/title.keys

Keys default to prod.keys and title.keys in the repository root. Lines hactool rejects (a value whose length is not a
whole number of 16-byte blocks, as some dumped key files have) are left out of the copy hactool reads. Title keys of
the tickets inside NSPs are added to the title keys.

The update has to be for the same title as the base: its program ID is the base's with 0x800 added.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HACTOOL = os.path.join(REPO, 'deps', 'host', 'bin', 'hactool' + ('.exe' if sys.platform == 'win32' else ''))
KEY_LINE = re.compile(r'^\s*([0-9A-Za-z_]+)\s*=\s*([0-9A-Fa-f]+)\s*$')


def clean_keys(src, dst):
    """Copies the key lines whose values are whole 16-byte blocks. Returns how many were left out."""
    dropped = 0
    with open(src) as fi, open(dst, 'w') as fo:
        for line in fi:
            m = KEY_LINE.match(line)
            if not m:
                continue
            if len(m.group(2)) % 32:
                dropped += 1
                continue
            fo.write('%s = %s\n' % (m.group(1), m.group(2)))
    return dropped


def ticket_title_key(path):
    """(rights ID, title key) of a common ticket, or None for a personalized one."""
    data = open(path, 'rb').read()
    if len(data) < 0x2B0 or data[0x281] != 0:  # title key type: 0 common, 1 personalized
        return None
    return data[0x2A0:0x2B0].hex(), data[0x180:0x190].hex()


class Hactool:
    def __init__(self, home):
        self.env = dict(os.environ, HOME=home)
        self.env.pop('XDG_CONFIG_HOME', None)

    def run(self, *args):
        # hactool prints key values in its key warnings: switch them off, and never show such a line
        r = subprocess.run([HACTOOL, '--disablekeywarns', *args], capture_output=True, text=True, env=self.env)
        if r.returncode:
            out = '\n'.join(l for l in (r.stdout + r.stderr).splitlines() if 'key' not in l.lower())
            raise SystemExit('hactool %s failed:\n%s' % (' '.join(args), out[-2000:]))
        return r.stdout

    def unpack(self, container, out):
        """The NCAs (and tickets) of an XCI's secure partition or of an NSP."""
        if container.lower().endswith('.xci'):
            self.run('-txci', '--securedir=' + out, container)
        else:
            self.run('-tpfs0', '--outdir=' + out, container)

    def program_nca(self, folder):
        """The largest NCA of content type Program in folder."""
        best = None
        for name in os.listdir(folder):
            path = os.path.join(folder, name)
            if not name.endswith('.nca') or name.endswith('.cnmt.nca'):
                continue
            info = self.run('-i', path)
            m = re.search(r'Content Type:\s*(\w+)', info)
            if m and m.group(1) == 'Program':
                if best is None or os.path.getsize(path) > os.path.getsize(best):
                    best = path
        if best is None:
            raise SystemExit('no Program NCA in ' + folder)
        return best


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('base', help='the base game, .xci or .nsp')
    ap.add_argument('--update', help='an update .nsp for the same title')
    ap.add_argument('--name', help='output name (default: the base file name)')
    ap.add_argument('--out', help='output folder (default: work/NAME)')
    ap.add_argument('--keys', default=os.path.join(REPO, 'prod.keys'))
    ap.add_argument('--title-keys', default=os.path.join(REPO, 'title.keys'))
    a = ap.parse_args()

    if not os.path.isfile(HACTOOL):
        raise SystemExit('hactool is missing: run scripts/bootstrap.py (scripts\\bootstrap.ps1 on Windows)')
    if not os.path.isfile(a.keys):
        raise SystemExit('no keys at %s (use --keys)' % a.keys)
    name = a.name or os.path.splitext(os.path.basename(a.base))[0]
    out = os.path.abspath(a.out or os.path.join(REPO, 'work', name))
    if os.path.exists(os.path.join(out, 'exefs')):
        raise SystemExit('%s already has an exefs folder' % out)

    # The NCAs are as big as the game: unpack them next to the output, not in /tmp. hactool creates the last folder
    # of a path only, and exits 0 when it can't write the files
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='.extract-', dir=os.path.dirname(out))
    try:
        home = os.path.join(tmp, 'home')
        os.makedirs(os.path.join(home, '.switch'))
        dropped = clean_keys(a.keys, os.path.join(home, '.switch', 'prod.keys'))
        if dropped:
            print('Left out %d malformed key lines' % dropped)
        hac = Hactool(home)

        base_dir = os.path.join(tmp, 'base')
        print('Unpacking', a.base)
        hac.unpack(a.base, base_dir)
        update_dir = None
        if a.update:
            update_dir = os.path.join(tmp, 'update')
            print('Unpacking', a.update)
            hac.unpack(a.update, update_dir)

        # Title keys: the user's file, then every common ticket found in the containers
        with open(os.path.join(home, '.switch', 'title.keys'), 'w') as fo:
            if os.path.isfile(a.title_keys):
                with open(a.title_keys) as fi:
                    fo.write(fi.read() + '\n')
            for folder in filter(None, (base_dir, update_dir)):
                for f in os.listdir(folder):
                    if f.endswith('.tik'):
                        key = ticket_title_key(os.path.join(folder, f))
                        if key:
                            fo.write('%s = %s\n' % key)

        base = hac.program_nca(base_dir)
        exefs, romfs = os.path.join(out, 'exefs'), os.path.join(out, 'romfs')
        if update_dir:
            update = hac.program_nca(update_dir)
            print('Extracting the base with the update applied')
            hac.run('--basenca=' + base, '--exefsdir=' + exefs, '--romfsdir=' + romfs, update)
        else:
            print('Extracting the base game')
            hac.run('--exefsdir=' + exefs, '--romfsdir=' + romfs, base)
        if not os.path.isfile(os.path.join(exefs, 'main')):
            raise SystemExit('the extracted exefs has no main: is the update for this base game?')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('Done:', out)


if __name__ == '__main__':
    sys.exit(main())
