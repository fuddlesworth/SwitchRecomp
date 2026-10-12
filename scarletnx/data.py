# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
# any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
# This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE).
"""The game's data folder next to the runtime: exefs (always refreshed) and romfs (copied once), and a first .ini."""

import filecmp
import os
import shutil

from . import nxloader


def copy_data(mods, npdm, romfs, entry, datadir, say):
    ex = os.path.join(datadir, 'exefs')
    os.makedirs(ex, exist_ok=True)
    if os.path.isfile(entry) and entry.lower().endswith('.nro'):
        shutil.copyfile(entry, os.path.join(ex, 'main.nro'))
        if isinstance(romfs, (bytes, bytearray)):
            with open(os.path.join(datadir, 'romfs.bin'), 'wb') as f:
                f.write(romfs)
        return
    src = nxloader.find_exefs(entry)
    for n in os.listdir(src):
        s = os.path.join(src, n)
        d = os.path.join(ex, n)
        # by content: an update can change a file (main.npdm) without changing its size
        if os.path.isfile(s) and not (os.path.isfile(d) and filecmp.cmp(s, d, shallow=False)):
            shutil.copyfile(s, d)
    if isinstance(romfs, str):
        dst = os.path.join(datadir, 'romfs')
        if os.path.abspath(romfs) != os.path.abspath(dst) and not os.path.exists(dst):
            say('Copying romfs (may take a while)...')
            shutil.copytree(romfs, dst)


INI = """; ScarletNX settings for %(title)s
; This file is rewritten (organized, with every option explained) the first time the game starts.
[Display]
language=1
"""
