# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""An installed build's files in the XDG Base Directory folders: the game's settings in the config folder, its log and
threads.txt in the state folder, the pipeline cache and the extractions in the cache folder. What an older build left
in the data folder stays there, each run saying where it is and where this one keeps it. A game in a folder of its own keeps everything beside it. No GPU: the game stops
before it, its ExeFS taken away."""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from nx_runtime_smoke import make_nro


def older_ini_kept(games):
    text = (games / 'Smoke.ini').read_text()
    return 'kept from the older build' in text and 'Game language' not in text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver', required=True, type=Path)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    driver = str(args.driver.resolve())
    failures = []

    with tempfile.TemporaryDirectory(prefix='scarletnx-xdg-') as temporary:
        root = Path(temporary)
        env = {k: v for k, v in os.environ.items() if k not in ('SNX_GAME', 'APPIMAGE', 'APPDIR')}
        homes = {kind: root / kind.lower() for kind in ('DATA', 'CONFIG', 'STATE', 'CACHE')}
        for kind, path in homes.items():
            env[f'XDG_{kind}_HOME'] = str(path)
        env['HOME'] = str(root / 'home')
        data, config, state, cache = (homes[kind] / 'scarletnx' for kind in ('DATA', 'CONFIG', 'STATE', 'CACHE'))
        games = data / 'games'

        def run(command, cwd=root):
            p = subprocess.run(
                command, check=False, cwd=cwd, env=env, capture_output=True, text=True, timeout=args.timeout
            )
            return p.returncode, p.stdout + p.stderr

        def check(condition, what, output=''):
            if not condition:
                failures.append(f'{what}\n' + ''.join(f'    {line}\n' for line in output.splitlines()[-30:]))

        # what an older build left beside the game and in work/
        (games / 'shader_cache').mkdir(parents=True)
        (games / 'Smoke.ini').write_text('[Display]\nlanguage=1\n; kept from the older build\n')
        (games / 'Smoke.log').write_text('older log\n')
        (games / 'threads.txt').write_text('older threads\n')
        (games / 'shader_cache/vk_pipelines.bin').write_bytes(b'pipelines')
        (data / 'work/Old/exefs').mkdir(parents=True)
        (data / 'work/Old/exefs/main').write_bytes(b'code')
        nro = root / 'smoke.nro'
        nro.write_bytes(make_nro())

        # recompiled into the data folder (--repo names it, as the menu does): the settings in the config folder
        status, out = run([driver, '--repo', str(data), '-r', str(nro), 'Smoke', '--runtime', driver, '--jobs', '2'])
        check(status == 0 and (games / 'Smoke').is_file(), 'the recompile did not put Smoke in games/', out)
        ini = config / 'games/Smoke.ini'
        older = 'kept from the older build'
        check(ini.is_file() and older not in ini.read_text(), f'the recompile did not write {ini}', out)
        check(older in (games / 'Smoke.ini').read_text(), 'the older Smoke.ini was touched', out)
        told = f'{games / "Smoke.ini"} was left by an older version: this one keeps it in {ini}'
        check(told in out, 'the recompile did not say where the older Smoke.ini is', out)

        # the game, started in games/ as ./Smoke, logs in the state folder and stops before the GPU without its ExeFS.
        # Each start says where the older files are, as long as they are there
        shutil.rmtree(games / 'Smoke_data/exefs')
        log = state / 'games/Smoke.log'
        threads = state / 'games/threads.txt'
        pipelines = cache / 'games/shader_cache/vk_pipelines.bin'
        for start in ('first', 'second'):
            status, out = run(['./Smoke', '-H'], cwd=games)
            text = log.read_text(errors='replace') if log.is_file() else ''
            check(
                status != 0 and 'ScarletNX - Smoke' in text and 'Could not load the game executables' in text,
                f'the game did not log to {log} and stop before the GPU (exit {status}, {start} start)',
                out,
            )
            for older, now in (
                (games / 'Smoke.log', log),
                (games / 'threads.txt', threads),
                (games / 'Smoke.ini', ini),
                (games / 'shader_cache/vk_pipelines.bin', pipelines),
            ):
                told = f'{older} was left by an older version: this one keeps it in {now}'
                check(told in text, f'the {start} start did not say where {older.name} is', text)
        check((games / 'Smoke.log').read_text() == 'older log\n', 'the game wrote in the older Smoke.log', out)
        check((games / 'threads.txt').read_text() == 'older threads\n', 'the older threads.txt was touched', out)
        check(
            (games / 'shader_cache/vk_pipelines.bin').read_bytes() == b'pipelines',
            'the older pipeline cache was touched',
        )
        check(older_ini_kept(games), 'the game wrote its settings in games/', text)
        check(
            all((games / name).exists() for name in ('Smoke.abi', 'Smoke_code.so', 'Smoke_data')),
            'the game itself left games/',
        )

        # an extraction: the older build's work/ stays, and the driver says the cache folder's is used. hactool finds
        # no program in an empty PFS0
        (root / 'empty.nsp').write_bytes(b'PFS0' + bytes(12))
        status, out = run([driver, '--repo', str(data), '-x', str(root / 'empty.nsp')])
        check((data / 'work/Old/exefs/main').read_bytes() == b'code', 'the older work/ was touched', out)
        told = f'{data / "work"} was left by an older version: this one keeps extractions in {cache / "work"}'
        check(told in out, 'the extraction did not say where the older work/ is', out)

        # a folder of its own (a checkout's or a portable copy's games/, --outdir): everything beside the game
        beside = root / 'portable/games'
        status, out = run(
            [driver, '--repo', str(data), '-r', str(nro), 'Beside', '--runtime', driver, '--jobs', '2']
            + ['--outdir', str(beside)]
        )
        check(status == 0 and (beside / 'Beside.ini').is_file(), 'the --outdir recompile did not keep Beside.ini', out)
        shutil.rmtree(beside / 'Beside_data/exefs')
        status, out = run(['./Beside', '-H'], cwd=beside)
        check((beside / 'Beside.log').is_file(), 'the game in a folder of its own did not log beside it', out)
        check(
            not (config / 'games/Beside.ini').exists() and not (state / 'games/Beside.log').exists(),
            'the game in a folder of its own wrote in the XDG folders',
        )
        check(not (root / 'home').exists(), 'something was written in HOME')

    if failures:
        print('XDG layout:\n' + ''.join(failures), file=sys.stderr)
        return 1
    print(
        'XDG layout: settings in config, log and threads.txt in state, pipeline cache and work/ in cache, older '
        'files told on each run and left alone; a folder of its own keeps them beside the game'
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
