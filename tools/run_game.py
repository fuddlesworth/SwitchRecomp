#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
"""Runs a recompiled game unattended, one or more times, and summarizes each run.

    tools/run_game.py Catherine                          # one 60 s run of the New Game script
    tools/run_game.py Catherine --runs 5 --seconds 45    # five runs, for an intermittent bug
    tools/run_game.py Catherine --script 15000:400 --out /tmp/x

Each run writes run<N>.log and run<N>.png (the last frame) into --out (default: a new folder in /tmp). The summary
line has the exit, the crash site (game pc and the symbol, with --dump), the last FPS line and the frame count.

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


def summarize(log, dump):
    text = open(log, errors='replace').read()
    crash = re.search(r'\*\*\* Crash \(signal (\d+)\).*?\n(?:.*\n){0,12}?\s*pc=([0-9a-f]+) lr=([0-9a-f]+)', text)
    brk = re.search(r'\*\*\* The game called svcBreak \(reason=([0-9a-f]+)', text)
    fps = re.findall(r'performance: ([\d.]+) FPS', text)
    frames = re.search(r'frames=(\d+) hash=', text)
    parts = []
    if crash:
        parts.append('CRASH signal %s at pc %s (lr %s)' % crash.groups())
        if dump:
            names = subprocess.run(
                [sys.executable, os.path.join(REPO, 'tools', 'symbols.py'), dump, crash.group(2), crash.group(3)],
                capture_output=True,
                text=True,
            ).stdout.split('\n')
            parts.append(' / '.join(n for n in names if n))
    elif brk:
        parts.append('svcBreak reason %s' % brk.group(1))
    else:
        parts.append('ok')
    if fps:
        parts.append('last %s FPS' % fps[-1])
    if frames:
        parts.append('%s frames' % frames.group(1))
    return ', '.join(parts), bool(crash or brk)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('game', help='name of the game in games/ (the runtime copy, e.g. Catherine)')
    ap.add_argument('--runs', type=int, default=1)
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument('--script', default=NEW_GAME, help='button script, "" for none (default: Catherine New Game)')
    ap.add_argument('--out', help='folder for logs and screenshots')
    ap.add_argument('--dump', help='extracted game folder, to name crash sites (e.g. work/Catherine)')
    ap.add_argument('args', nargs='*', help='more runtime options, e.g. --fps_limit=0')
    a = ap.parse_args()

    games = os.path.join(REPO, 'games')
    exe = os.path.join(games, a.game + ('.exe' if sys.platform == 'win32' else ''))
    if not os.path.isfile(exe):
        raise SystemExit('%s does not exist: build the runtime (it deploys there) or recompile the game' % exe)
    out = a.out or tempfile.mkdtemp(prefix='swr-run-')
    os.makedirs(out, exist_ok=True)
    failed = 0
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
        line, bad = summarize(log, a.dump)
        failed += bad or code != 0
        # The runtime writes the last frame as <shot>.ppm; PNG is what an image viewer (or Claude) reads
        if os.path.isfile(shot + '.ppm'):
            try:
                from PIL import Image

                Image.open(shot + '.ppm').save(shot + '.png')
                os.remove(shot + '.ppm')
            except ImportError:
                pass
        print('run %d: exit %s, %s' % (i, code, line), flush=True)
    print('%d of %d runs failed. Logs and screenshots: %s' % (failed, a.runs, out))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
