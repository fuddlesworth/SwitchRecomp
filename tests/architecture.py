# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check source include ownership and optional CMake Graphviz dependency closure."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

BOUNDARIES = {'common': {'common'}, 'nx': {'nx', 'common'}, 'gnuabi': {'gnuabi', 'common'}, 'agb': {'agb', 'common'}}
INCLUDE = re.compile(r'^\s*#\s*include\s*(["<])([^">]+)[">]', re.MULTILINE)


def check_sources(repo):
    errors = []
    src = repo / 'src'
    include_suffixes = {}
    for header in src.rglob('*'):
        if header.suffix not in {'.h', '.hpp', '.inc'}:
            continue
        parts = header.relative_to(src).parts[1:]
        for offset in range(len(parts)):
            include_suffixes.setdefault('/'.join(parts[offset:]), set()).add(header.resolve())
    for owner, allowed in BOUNDARIES.items():
        root = src / owner
        if not root.is_dir():
            errors.append(f'missing source boundary: {root}')
            continue
        for path in sorted(root.rglob('*')):
            if path.suffix not in {'.c', '.h', '.cpp', '.hpp', '.inc'}:
                continue
            source = path.read_text(encoding='utf-8')
            for match in INCLUDE.finditer(source):
                name = match.group(2)
                candidates = [path.parent / name, src / name]
                # Resolve against every source boundary, even forbidden ones: a
                # private include root must not hide a dependency from this test.
                candidates.extend(src / boundary / name for boundary in BOUNDARIES)
                targets = {candidate.resolve() for candidate in candidates if candidate.is_file()}
                if not targets and match.group(1) == '"':
                    # Runtime targets may expose a deeper include root. Check a
                    # unique header suffix even if that include root was omitted
                    # here; system angle-bracket headers keep host resolution.
                    targets = include_suffixes.get(name.replace('\\', '/'), set())
                for target in targets:
                    if not target.is_relative_to(src.resolve()):
                        continue
                    dependency = target.relative_to(src.resolve()).parts[0]
                    if dependency in BOUNDARIES and dependency not in allowed:
                        line = source.count('\n', 0, match.start()) + 1
                        errors.append(f'{path.relative_to(repo)}:{line}: {owner} includes {dependency}: {name}')
    for legacy in ('runtime', 'scarletnx'):
        if any(path.suffix in {'.c', '.h', '.cpp', '.hpp', '.inc'} for path in (repo / legacy).rglob('*')):
            errors.append(f'legacy source files still present: {legacy}/')
    return errors


def check_graph(graph):
    source = graph.read_text(encoding='utf-8')
    nodes = {
        node: label.split('\\n', 1)[0] for node, label in re.findall(r'"([^"]+)"\s*\[\s*label\s*=\s*"([^"]+)"', source)
    }
    edges = {}
    for left, right in re.findall(r'"([^"]+)"\s*->\s*"([^"]+)"', source):
        edges.setdefault(left, set()).add(right)
    by_name = {name: node for node, name in nodes.items()}

    def closure(node):
        result, pending = set(), [node]
        while pending:
            current = pending.pop()
            for child in edges.get(current, ()):
                if child not in result:
                    result.add(child)
                    pending.append(child)
        return {nodes.get(item, item) for item in result}

    errors = []
    for owner, forbidden in (
        ('scarletnx-common', ('scarletnx-nx', 'scarletnx-gnuabi', 'scarletnx-agb')),
        ('scarletnx-nx', ('scarletnx-gnuabi', 'scarletnx-agb')),
        ('scarletnx-gnuabi', ('scarletnx-nx', 'scarletnx-agb')),
        ('scarletnx-agb', ('scarletnx-nx', 'scarletnx-gnuabi')),
    ):
        if owner not in by_name:
            if owner == 'scarletnx-common':
                errors.append(f'CMake graph is missing target {owner}')
            continue
        dependencies = closure(by_name[owner])
        for target in forbidden:
            if target in dependencies:
                errors.append(f'{owner} transitively links forbidden target {target}')
        if owner in ('scarletnx-common', 'scarletnx-gnuabi', 'scarletnx-agb'):
            for target in dependencies:
                if re.search(
                    r'(?:^|[-_:])(gpu|maxwell|vulkan|volk|glslang|sdl3|ffmpeg|spirv)(?:$|[-_:])', target, re.IGNORECASE
                ) or target in {'snx_vk', 'snx_null', 'snx_shader', 'snx_video', 'snx_platform'}:
                    errors.append(f'{owner} transitively links graphics/media target {target}')
    return errors


# A project header with one of these names, on an include path, hides the system one
SYSTEM_HEADERS = {
    'assert.h', 'complex.h', 'ctype.h', 'errno.h', 'fenv.h', 'float.h', 'inttypes.h', 'iso646.h', 'limits.h',
    'locale.h', 'math.h', 'setjmp.h', 'signal.h', 'stdalign.h', 'stdarg.h', 'stdatomic.h', 'stdbool.h', 'stddef.h',
    'stdint.h', 'stdio.h', 'stdlib.h', 'stdnoreturn.h', 'string.h', 'tgmath.h', 'threads.h', 'time.h', 'uchar.h',
    'wchar.h', 'wctype.h', 'dirent.h', 'dlfcn.h', 'fcntl.h', 'poll.h', 'pthread.h', 'sched.h', 'semaphore.h',
    'termios.h', 'unistd.h', 'conio.h', 'direct.h', 'excpt.h', 'intrin.h', 'io.h', 'malloc.h', 'memory.h',
    'process.h', 'windows.h',
}


def check_include_shadows(repo, build):
    reply = build / '.cmake/api/v1/reply'
    indexes = sorted(reply.glob('index-*.json'))
    if not indexes:
        return ['CMake file API did not produce a target/source snapshot']
    index = json.loads(indexes[-1].read_text(encoding='utf-8'))
    model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text(encoding='utf-8'))
    repo = repo.resolve()
    seen, errors = set(), []
    for configuration in model['configurations']:
        for entry in configuration['targets']:
            target = json.loads((reply / entry['jsonFile']).read_text(encoding='utf-8'))
            for group in target.get('compileGroups', ()):
                for include in group.get('includes', ()):
                    directory = Path(include['path'])
                    if not directory.is_absolute():
                        directory = repo / directory
                    directory = directory.resolve()
                    if not directory.is_relative_to(repo) or not directory.is_dir():
                        continue
                    for header in sorted(directory.iterdir()):
                        if header.is_file() and header.name.lower() in SYSTEM_HEADERS and header not in seen:
                            seen.add(header)
                            path = header.relative_to(repo).as_posix()  # the same text on Windows
                            errors.append(f'{path} is on {entry["name"]}\'s include path, where it hides the system '
                                          f'<{header.name.lower()}>')
    return errors


def check_target_sources(repo, build):
    reply = build / '.cmake/api/v1/reply'
    indexes = sorted(reply.glob('index-*.json'))
    if not indexes:
        return ['CMake file API did not produce a target/source snapshot']
    index = json.loads(indexes[-1].read_text(encoding='utf-8'))
    model_file = index['reply']['codemodel-v2']['jsonFile']
    model = json.loads((reply / model_file).read_text(encoding='utf-8'))
    errors = []
    for configuration in model['configurations']:
        for entry in configuration['targets']:
            owner = {
                'scarletnx-common': 'common',
                'scarletnx-nx': 'nx',
                'scarletnx-gnuabi': 'gnuabi',
                'scarletnx-gnuabi-run': 'gnuabi',
                'scarletnx-agb': 'agb',
                'scarletnx-agb-run': 'agb',
            }.get(entry['name'])
            target = json.loads((reply / entry['jsonFile']).read_text(encoding='utf-8'))
            directory = (repo / target.get('paths', {}).get('source', '')).resolve()
            src = (repo / 'src').resolve()
            if not owner and directory.is_relative_to(src):
                owner = directory.relative_to(src).parts[0]
            if owner not in BOUNDARIES:
                continue
            for source in target.get('sources', ()):
                path = Path(source['path'])
                if not path.is_absolute():
                    path = repo / path
                path = path.resolve()
                if path.is_relative_to(src) and path.relative_to(src).parts[0] != owner:
                    errors.append(f'{entry["name"]} compiles a source owned by another runtime: {path}')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--graph', type=Path, help='CMake --graphviz=PATH output, including transitive dependencies')
    parser.add_argument(
        '--build', type=Path, help='Configured build tree: generate and validate its actual CMake graph'
    )
    parser.add_argument('--cmake', default='cmake', help='CMake executable for --build')
    args = parser.parse_args()
    errors = check_sources(args.repo.resolve())
    if args.build:
        graph = args.build.resolve() / 'architecture-targets.dot'
        query = args.build.resolve() / '.cmake/api/v1/query/codemodel-v2'
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch()
        result = subprocess.run(
            [args.cmake, '-S', str(args.repo.resolve()), '-B', str(args.build.resolve()), f'--graphviz={graph}'],
            capture_output=True,
            check=False,
            timeout=120,
        )
        if result.returncode:
            sys.stderr.buffer.write(result.stderr)
            sys.stderr.buffer.write(result.stdout)
            return result.returncode
        errors.extend(check_graph(graph))
        errors.extend(check_target_sources(args.repo.resolve(), args.build.resolve()))
        errors.extend(check_include_shadows(args.repo.resolve(), args.build.resolve()))
    if args.graph:
        errors.extend(check_graph(args.graph))
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('Runtime source boundaries and requested CMake dependency graph passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
