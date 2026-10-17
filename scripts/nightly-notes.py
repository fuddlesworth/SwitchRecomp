# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A nightly's title and notes: the downloads by system, with their sizes and how to run them, what changed since the
previous nightly, and where the source is, which the GPL asks for beside the binaries. scripts/publish-builds.py takes
them for the nightlies of the nightly repository and a version's release, and the downloads and the source for a pull
request's builds.

The changes are the checkout's history from the commit the previous nightly was built from to the one built, left out
without it or when it is not an ancestor. The notes link the built commit first, which is where the next nightly finds
it. Each change links its pull request or commit in the repository: a bare #N in another repository's release would
name that repository's own.
"""

import collections
import re
import subprocess

SERVER = 'https://github.com'
# The release's files in the order the tables list them, named ScarletNX-<system>-<arch>: the system, its
# architectures and what the file is. A file a release gets that is missing here stops the notes: add its line
SYSTEMS = {
    'ScarletNX-linux-x86_64.AppImage': ('Linux', 'x86-64', 'AppImage'),
    'ScarletNX-linux-aarch64.AppImage': ('Linux', 'ARM64', 'AppImage'),
    'ScarletNX-windows-x86_64.zip': ('Windows', 'x86-64', 'zip'),
    'ScarletNX-android-universal.apk': ('Android', 'ARM64, x86-64', 'APK'),
    'ScarletNX-macos-arm64.zip': ('macOS', 'ARM64', 'zip'),
    'ScarletNX-ios-arm64.ipa': ('iOS', 'ARM64', 'IPA'),
}
# how each system's file runs, under the downloads; {updates}: the release AppImageUpdate takes an AppImage to, the
# latest of the repository its update information names (scripts/make-appimage.sh)
RUNNING = {
    'Linux': 'make the AppImage executable and run it. AppImageUpdate takes it to {updates}.',
    'Windows': 'unzip it anywhere and run `bin\\scarletnx.exe`.',
    'Android': 'Android 5.0 and later. The APK is signed with a debug key.',
    'macOS': "unzip it and open `ScarletNX.app`; signed ad hoc, so a download may need *Open* from the Finder's menu the first time.",
    'iOS': 'unsigned: AltStore, SideStore or a developer account signs it for the device.',
}
MERGE = re.compile(r'Merge pull request #(\d+) from \S+')
# what text from a commit or a pull request could make of the markdown around it: a link, a tag, a table cell
MARKDOWN = re.compile(r'([\\`*_\[\]<>|~])')
MOST_CHANGES = 30

# A change of main: its subject or its pull request's title, the pull request's number (None for a commit pushed to
# main), and the commit
Change = collections.namedtuple('Change', 'text pr sha')


def git(checkout, *args):
    return subprocess.run(['git', '-C', checkout, *args], check=True, capture_output=True, text=True).stdout


def escape(text):
    """text shown as it is, in GitHub's markdown and Discord's"""
    return MARKDOWN.sub(r'\\\1', text)


def changes(checkout, previous, sha):
    """What main got from PREVIOUS to SHA, newest first: a merged pull request by its title, any other commit by its
    subject. None when PREVIOUS is missing or not an ancestor of SHA"""
    if not previous:
        return None
    ancestor = subprocess.run(
        ['git', '-C', checkout, 'merge-base', '--is-ancestor', previous, sha], capture_output=True, check=False
    )
    if ancestor.returncode:
        return None
    items = []
    log = git(checkout, 'log', '--first-parent', '--format=%H%x1f%s%x1f%b%x1e', f'{previous}..{sha}')
    for record in log.split('\x1e'):
        record = record.strip('\n')
        if not record:
            continue
        commit, subject, body = record.split('\x1f', 2)
        merge = MERGE.fullmatch(subject)
        if merge:
            title = next((line.strip() for line in body.splitlines() if line.strip()), subject)
            items.append(Change(title, int(merge.group(1)), commit))
        else:
            items.append(Change(subject, None, commit))
    return items


def change_line(change, url):
    """a change as a line, its pull request or its commit linked in the repository at url"""
    if change.pr:
        return f'{escape(change.text)} ([#{change.pr}]({url}/pull/{change.pr}))'
    return f'{escape(change.text)} ([`{change.sha[:7]}`]({url}/commit/{change.sha}))'


def title(checkout, sha):
    return f'Nightly {git(checkout, "log", "-1", "--format=%cs", sha).strip()} ({sha[:7]})'


def size(count):
    return f'{count / 1e6:.1f} MB'


def ordered(names):
    """names in the order of SYSTEMS; one that is not there stops the notes, saying where to add it"""
    unknown = [name for name in names if name not in SYSTEMS]
    if unknown:
        raise SystemExit(f'{", ".join(unknown)}: what system is it for? Add it to SYSTEMS in scripts/nightly-notes.py')
    return sorted(names, key=list(SYSTEMS).index)


def system(name):
    """what name is for, as a run says it: Linux, x86-64"""
    return ', '.join(SYSTEMS[ordered([name])[0]][:2])


def table(files, download, sizes=None, missing=(), why=''):
    """The files, a row each in the order of SYSTEMS, linked from the release they are in, with their sizes; a file
    the run did not build says so, linking why"""
    sizes = sizes or {}
    rows = ['| System | Architecture | Download | Size |', '|---|---|---|---|']
    for name in ordered([*files, *missing]):
        named, arch, kind = SYSTEMS[name]
        if name in files:
            got, weight = f'[{kind}]({download}/{name})', size(sizes[name]) if name in sizes else ''
        else:
            got, weight = f'not built, [see the run]({why})' if why else 'not built', ''
        rows.append(f'| **{named}** | {arch} | {got} | {weight} |')
    return rows


def downloads(files, download, url, sha, sizes=None, missing=(), why='', updates='the newest nightly'):
    """the downloads: the table, how each system's file runs, and the documents"""
    lines = ['## Downloads', '', *table(files, download, sizes, missing, why), '']
    shown = dict.fromkeys(SYSTEMS[name][0] for name in ordered(files))
    lines += [f'- **{name}**: {RUNNING[name].format(updates=updates)}' for name in shown]
    lines += [
        '',
        (
            f'More in [USAGE.md]({url}/blob/{sha}/docs/USAGE.md) and, for the app, '
            f'[ANDROID.md]({url}/blob/{sha}/docs/ANDROID.md).'
        ),
    ]
    return lines


def source(tree):
    """Where a build's source is, which the GPL asks for beside the binaries, and its libraries' licenses, folded"""
    return [
        '<details>',
        '<summary>Source and licenses</summary>',
        '',
        f'The source of this build is at {tree}.',
        '',
        (
            '- The third-party libraries built into the runtime come from the release archives that '
            'cmake/dependencies.cmake lists, each checked against the SHA-256 written beside it.'
        ),
        (
            "- The AppImages also bundle libraries from Arch Linux packages (Arch Linux ARM's for ARM64): their "
            'licenses are in share/licenses, and the packages in share/licenses/bundled-packages.txt.'
        ),
        (
            '- The Windows zip holds the runtime DLLs it needs with their licenses in share/licenses, and the MSYS2 '
            'packages it was built with in share/licenses/msys2/build-packages.txt.'
        ),
        '',
        '</details>',
    ]


def notes(
    checkout,
    sha,
    previous,
    repo,
    files,
    download,
    kept,
    since='the previous nightly',
    sizes=None,
    lead='Built from main at',
    updates='the newest nightly',
):
    """download: where the release's files are; kept: what becomes of the release; since: what previous was; sizes:
    each file's bytes; lead: what the first line calls the build; updates: the release AppImageUpdate takes it to"""
    url = f'{SERVER}/{repo}'
    date = git(checkout, 'log', '-1', '--format=%cs', sha).strip()
    lines = [f'{lead} [`{sha[:7]}`]({url}/commit/{sha}), committed {date}.' + (f' {kept}' if kept else ''), '']
    lines += downloads(files, download, url, sha, sizes, updates=updates)
    items = changes(checkout, previous, sha)
    if items is not None:
        compare = f'{url}/compare/{previous}...{sha}'
        counted = f'{len(items)} change{"" if len(items) == 1 else "s"}, the newest first' if items else 'no change'
        lines += ['', f'## Changes since {since}', '', f'[`{previous[:7]}...{sha[:7]}`]({compare}): {counted}.', '']
        lines += [f'- {change_line(item, url)}' for item in items[:MOST_CHANGES]] or [
            'None: the same commit, built again.'
        ]
        if len(items) > MOST_CHANGES:
            lines.append(f'- and [{len(items) - MOST_CHANGES} more]({compare})')
    lines += ['', *source(f'{url}/tree/{sha}')]
    return '\n'.join(lines) + '\n'
