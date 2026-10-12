#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Runs a recompiled game unattended, one or more times, and summarizes each run.

    tools/run_game.py Game                               # one 60 s run of the New Game script
    tools/run_game.py Game --runs 5 --seconds 45         # five runs, for an intermittent bug
    tools/run_game.py Game --script 15000:400 --out /tmp/x
    tools/run_game.py Game --fps_limit=0                 # options it doesn't know go to the runtime

Each run writes run<N>.log and run<N>.png (the last frame; .ppm without Pillow) into --out (default: a new folder in
the system's temporary folder). The summary line has the exit, the crash site (game pc and the symbol, with --dump),
and the frame count the runtime prints when --seconds ends the run, with the average FPS over the run.

Scripts are comma-separated `milliseconds:hexbuttons` presses, each held for 300 ms. Buttons: A 1, B 2, X 4, Y 8,
L 40, R 80, ZL 100, ZR 200, Plus 400, Minus 800, Left 1000, Up 2000, Right 4000, Down 8000.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Catherine from boot to the New Game cutscene: skip the intro movie, accept the terms, pick New Game, skip prompts
NEW_GAME = '15000:400,16000:1,18000:400,21000:1,31000:1,36000:1,40000:1,44000:1'


def summarize(log, dump, seconds):
    with open(log, errors='replace') as f:
        text = f.read()
    # The crash report's header, then the faulting thread's registers somewhere after it (core/crash.c, kernel/debug.c)
    crash = re.search(r'\*\*\* Crash \(signal (\d+)\)', text)
    regs = re.compile(r'^\s*pc=([0-9a-f]+) lr=([0-9a-f]+)', re.M).search(text, crash.end()) if crash else None
    brk = re.search(r'\*\*\* The game called svcBreak \(reason=([0-9a-f]+)', text)
    # Printed by the backend when --seconds (or --frames) ends the run
    frames = re.search(r'^frames=(\d+) hash=', text, re.M)
    parts = []
    if crash and regs:
        parts.append('CRASH signal %s at pc %s (lr %s)' % (crash.group(1), regs.group(1), regs.group(2)))
        if dump:
            names = subprocess.run(
                [sys.executable, os.path.join(REPO, 'tools', 'symbols.py'), dump, regs.group(1), regs.group(2)],
                capture_output=True,
                text=True,
            ).stdout.split('\n')
            parts.append(' / '.join(n for n in names if n))
    elif crash:
        parts.append('CRASH signal %s (no game registers in the log)' % crash.group(1))
    elif brk:
        parts.append('svcBreak reason %s' % brk.group(1))
    else:
        parts.append('ok')
    if frames:
        n = int(frames.group(1))
        parts.append('%d frames (%.1f FPS on average)' % (n, n / seconds) if seconds else '%d frames' % n)
    return ', '.join(parts), bool(crash or brk)


def main():
    # No abbreviations: a runtime option must never be taken for a prefix of one of these
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], allow_abbrev=False)
    ap.add_argument('game', help='name of the game in games/ (the runtime copy, e.g. Game)')
    ap.add_argument('--runs', type=int, default=1)
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument(
        '--script',
        default=NEW_GAME,
        help='button script, "" for none (default: the New Game script of the game the fork is tested with)',
    )
    ap.add_argument('--out', help='folder for logs and screenshots')
    ap.add_argument('--dump', help='extracted game folder, to name crash sites (e.g. work/Game)')
    ap.add_argument('args', nargs='*', help='more runtime options, e.g. --fps_limit=0 (with or without -- before them)')
    a, unknown = ap.parse_known_args()
    a.args += unknown

    games = os.path.join(REPO, 'games')
    exe = os.path.join(games, a.game + ('.exe' if sys.platform == 'win32' else ''))
    if not os.path.isfile(exe):
        raise SystemExit('%s does not exist: build the runtime (it deploys there) or recompile the game' % exe)
    out = a.out or tempfile.mkdtemp(prefix='snx-run-')
    os.makedirs(out, exist_ok=True)
    failed, no_pillow = 0, False
    for i in range(1, a.runs + 1):
        log = os.path.join(out, 'run%d.log' % i)
        shot = os.path.join(out, 'run%d' % i)
        cmd = [exe, '--seconds=%d' % a.seconds, '--shot=' + shot] + (['--script=' + a.script] if a.script else [])
        with open(log, 'w') as f:
            try:
                r = subprocess.run(cmd + a.args, cwd=games, stdout=f, stderr=subprocess.STDOUT, timeout=a.seconds + 60)
                code = r.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        line, bad = summarize(log, a.dump, a.seconds)
        failed += bad or code != 0
        # The runtime writes the last frame as <shot>.ppm; PNG is what an image viewer (or Claude) reads
        if os.path.isfile(shot + '.ppm'):
            try:
                from PIL import Image

                Image.open(shot + '.ppm').save(shot + '.png')
                os.remove(shot + '.ppm')
            except ImportError:
                if not no_pillow:
                    print('Pillow is not installed: the screenshots stay .ppm files', flush=True)
                no_pillow = True
        print('run %d: exit %s, %s' % (i, code, line), flush=True)
    print('%d of %d runs failed. Logs and screenshots: %s' % (failed, a.runs, out))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
