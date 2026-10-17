#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/publish-builds.py against a fake gh (a small GitHub kept in a JSON file) and a fake Discord (a local HTTP
server): a version tag's release in the repository; the nightly made in the nightly repository, kept to the newest N,
never one older than the latest, and linked from the repository's rolling prerelease; a pull request's prerelease
replaced at each push and gone when it closes, its Discord card drawn again; a fork's only once the team labels it
after its last push; and the writes to the nightly repository made with its own token. Each release's notes link every
download with its size and say what changed: the changes since the last one, a pull request's commits and the main
commit it stands on; a version's message and a card, each download. The fake GitHub and Discord are
tests/fake_github.py's. Offline."""

import base64
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from fake_github import FAKE_GH, Discord, posted, serve

SCRIPT = Path(__file__).resolve().parent.parent / 'scripts' / 'publish-builds.py'
REPO, TO = 'owner/repo', 'owner/nightly'
FILES = [
    'ScarletNX-linux-x86_64.AppImage',
    'ScarletNX-linux-aarch64.AppImage',
    'ScarletNX-windows-x86_64.zip',
    'ScarletNX-android-universal.apk',
    'ScarletNX-macos-arm64.zip',
    'ScarletNX-ios-arm64.ipa',
]
ZSYNC = ['ScarletNX-linux-x86_64.AppImage.zsync', 'ScarletNX-linux-aarch64.AppImage.zsync']
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


server, HOOK = serve()

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    bin_dir, checkout = root / 'bin', root / 'checkout'
    bin_dir.mkdir()
    (bin_dir / 'gh').write_text(FAKE_GH)
    (bin_dir / 'gh').chmod(0o755)
    state_file, log_file = root / 'state.json', root / 'gh.log'

    def git(*args):
        env = dict(os.environ, GIT_COMMITTER_DATE='2026-10-15T12:00:00Z', GIT_AUTHOR_DATE='2026-10-15T12:00:00Z')
        return subprocess.run(
            ['git', '-C', str(checkout), '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
            + ['-c', 'core.hooksPath=' + os.devnull, '-c', 'tag.gpgsign=false', *args],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        ).stdout.strip()

    checkout.mkdir()
    git('init', '-q', '-b', 'main')
    git('commit', '-q', '--allow-empty', '-m', 'the older nightly')
    oldest = git('rev-parse', 'HEAD')
    git('commit', '-q', '--allow-empty', '-m', 'the previous nightly')
    previous = git('rev-parse', 'HEAD')
    git('tag', 'v0.1.0')
    git('switch', '-q', '-c', 'feature')
    git('commit', '-q', '--allow-empty', '-m', '[cpu] a commit of the pull request')
    git('switch', '-q', 'main')
    git('merge', '-q', '--no-ff', 'feature', '-m', 'Merge pull request #5 from someone/feature\n\n[ci] its title')
    sha = git('rev-parse', 'HEAD')
    git('tag', 'v0.2.0')

    def save_state(state):
        state_file.write_text(json.dumps(state))

    def state():
        return json.loads(state_file.read_text())

    def releases(repo=TO, s=None):
        return (s or state())['releases'].get(repo, [])

    def find(tag, repo=TO, s=None):
        return next((r for r in releases(repo, s) if r['tag_name'] == tag), None)

    def with_releases(repo, items):
        return dict(state(), releases=dict(state()['releases'], **{repo: items}))

    def card_id(key, s=None):
        """the message id the nightly repository keeps for a card"""
        entry = (s or state()).get('files', {}).get(TO, {}).get(f'discord-cards:cards/{key}')
        return base64.b64decode(entry['content']).decode().strip() if entry else None

    def calls():
        return [json.loads(line) for line in log_file.read_text().splitlines()] if log_file.exists() else []

    def artifacts(name, commit, skip=()):
        folder = root / name
        short = commit[:7]
        made = {
            f'ScarletNX-linux-x86_64-{short}.AppImage': {
                'ScarletNX-linux-x86_64.AppImage': 'elf x86',
                'ScarletNX-linux-x86_64.AppImage.zsync': 'zsync x86',
                'appinfo': 'i',
            },
            f'ScarletNX-linux-aarch64-{short}.AppImage': {
                'ScarletNX-linux-aarch64.AppImage': 'elf arm',
                'ScarletNX-linux-aarch64.AppImage.zsync': 'zsync arm',
            },
            f'ScarletNX-windows-x86_64-{short}': {'bin/scarletnx.exe': 'MZ', 'share/licenses/LICENSE': 'GPL'},
            f'ScarletNX-android-universal-{short}.apk': {f'ScarletNX-android-universal-{short}.apk': 'PK apk'},
            f'ScarletNX-macos-arm64-{short}.zip': {f'ScarletNX-macos-arm64-{short}.zip': 'PK app'},
            f'ScarletNX-ios-arm64-{short}.ipa': {f'ScarletNX-ios-arm64-{short}.ipa': 'PK ipa'},
        }
        for artifact, files in made.items():
            for file, text in files.items():
                if any(part in f'{artifact}/{file}' for part in skip):
                    continue
                (folder / artifact / file).parent.mkdir(parents=True, exist_ok=True)
                (folder / artifact / file).write_text(text)
        return folder

    def run(step, *extra, event=None, name='workflow_run', hooks=True, env_extra=None):
        event_file, output = root / 'event.json', root / 'output'
        event_file.write_text(json.dumps(event or {}))
        output.write_text('')
        env = dict(
            os.environ,
            PATH=f'{bin_dir}{os.pathsep}{os.environ["PATH"]}',
            FAKE_GH_STATE=str(state_file),
            FAKE_GH_LOG=str(log_file),
            GH_TOKEN='repo-token',
            NIGHTLY_TOKEN='nightly-token',
            GITHUB_EVENT_NAME=name,
            GITHUB_EVENT_PATH=str(event_file),
            GITHUB_OUTPUT=str(output),
        )
        for kind in ('RELEASES', 'NIGHTLY', 'PRS'):
            env.pop(f'DISCORD_WEBHOOK_{kind}', None)
        if hooks:
            env.update(
                DISCORD_WEBHOOK_RELEASES=f'{HOOK}-releases',
                DISCORD_WEBHOOK_NIGHTLY=f'{HOOK}-nightly',
                DISCORD_WEBHOOK_PRS=f'{HOOK}-prs?thread_id=55',
            )
        env.update(env_extra or {})
        done = subprocess.run(
            [sys.executable, str(SCRIPT), step, '--repo', REPO, '--to', TO, *extra],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        planned = dict(line.split('=', 1) for line in output.read_text().splitlines())
        return done, planned

    def push_run(commit, conclusion='success', branch='main', number=1):
        return {
            'workflow_run': {
                'id': number,
                'event': 'push',
                'head_branch': branch,
                'head_sha': commit,
                'conclusion': conclusion,
                'created_at': '2026-10-16T10:00:00Z',
                'head_repository': {'full_name': REPO},
            }
        }

    def nightly_release(commit, created, number):
        return {
            'id': number,
            'tag_name': f'nightly-2026-10-15-{commit[:7]}',
            'name': 'n',
            'created_at': '2026-10-01T00:00:00Z',  # GitHub's: the tag's commit's date, which every nightly shares
            'published_at': created,
            'body': f'Built from [`{commit[:7]}`](https://github.com/{REPO}/commit/{commit}).',
            'assets': [],
            'prerelease': False,
        }

    def everything(commit, run_id='41'):
        return ['--run', run_id, '--sha', commit, '--artifacts', str(artifacts('full', commit))]

    def publish(kind, *extra, **options):
        return run('publish', '--kind', kind, *extra, **options)[0]

    # ---- nightlies: in the nightly repository, linked from the rolling prerelease
    rolling = {
        'id': 9,
        'tag_name': 'nightly',
        'name': 'Nightly',
        'created_at': '2026-09-01',
        'body': 'old',
        'prerelease': True,
        'assets': [{'id': 7, 'name': 'ScarletNX-x86_64.AppImage', 'data': ''}],
    }
    save_state(
        {
            'next': 100,
            'clock': 0,
            'pulls': [],
            'events': {},
            'permissions': {},
            'runs': [],
            'tags': {
                TO: {f'nightly-2026-10-15-{c[:7]}': 'x' for c in (oldest, previous)},
                REPO: {'nightly': oldest, 'v0.1.0': previous, 'v0.2.0': sha},
            },
            'releases': {
                TO: [nightly_release(oldest, '2026-10-01', 1), nightly_release(previous, '2026-10-02', 2)],
                REPO: [rolling],
            },
        }
    )
    done, planned = run('plan', event=push_run(sha, number=41))
    check(
        'a green push to main is planned as a nightly',
        planned == {'kind': 'nightly', 'run': '41', 'sha': sha, 'short': sha[:7], 'pr': '', 'tag': ''},
        planned,
    )
    for event, why in (
        (push_run(sha, 'failure'), 'a failed run'),
        (push_run(sha, branch='dev'), 'another branch'),
        (push_run(sha, branch='v9.9.9'), 'a branch named like a version, no tag'),
    ):
        done, planned = run('plan', event=event)
        check(f'{why} is not planned', not planned and done.returncode == 0, done.stdout + done.stderr)
    log_file.unlink(missing_ok=True)
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '2')
    check('the nightly publishes', done.returncode == 0, done.stdout + done.stderr)
    s = state()
    tag = f'nightly-2026-10-15-{sha[:7]}'
    made = find(tag, s=s)
    check(
        'the release made, the latest, not a prerelease',
        made and made['make_latest'] == 'true' and not made['prerelease'],
        made,
    )
    check(
        "its files, and the AppImages' .zsync",
        made and sorted(a['name'] for a in made['assets']) == sorted(FILES + ZSYNC),
        made and made['assets'],
    )
    check(
        'its notes: the files linked from this release, the changes, the source',
        made
        and f'https://github.com/{TO}/releases/download/{tag}/ScarletNX-android-universal.apk' in made['body']
        and f'- \\[ci\\] its title ([#5](https://github.com/{REPO}/pull/5))' in made['body']
        and f'https://github.com/{REPO}/tree/{sha}' in made['body']
        and 'newest 2 nightlies' in made['body']
        and f'[`{previous[:7]}...{sha[:7]}`]' in made['body']
        and '.AppImage.zsync' not in made['body'],
        made and made['body'],
    )
    data = {a['name']: a['data'] for a in made['assets']} if made else {}
    archive = data.get('ScarletNX-windows-x86_64.zip', '')
    check('the Windows build zipped from its folder', archive.startswith('PK') and 'bin/scarletnx.exe' in archive)
    check(
        'the macOS app and the iOS IPA as their jobs made them, not zipped again',
        data.get('ScarletNX-macos-arm64.zip') == 'PK app' and data.get('ScarletNX-ios-arm64.ipa') == 'PK ipa',
        {k: v[:20] for k, v in data.items()},
    )
    check(
        'only the newest 2 kept, the oldest and its tag gone',
        sorted(r['tag_name'] for r in releases(s=s)) == sorted([tag, f'nightly-2026-10-15-{previous[:7]}'])
        and f'nightly-2026-10-15-{oldest[:7]}' not in s['tags'][TO],
        s['tags'][TO],
    )
    check(
        'its downloads by system, each with its size (bytes as megabytes)',
        made
        and f'| **Windows** | x86-64 | [zip](https://github.com/{TO}/releases/download/{tag}/ScarletNX-windows-x86_64.zip) |'
        in made['body']
        and '| **Linux** | x86-64 | [AppImage](' in made['body']
        and ' | 0.0 MB |' in made['body'],
        made and made['body'],
    )
    linked = find('nightly', REPO, s)
    check(
        "the rolling prerelease: no files, a link to the latest, its tag moved there, the latest's downloads and changes",
        linked
        and not linked['assets']
        and linked['prerelease']
        and f'https://github.com/{TO}/releases/tag/{tag}' in linked['body']
        and f'[APK](https://github.com/{TO}/releases/download/{tag}/ScarletNX-android-universal.apk)' in linked['body']
        and f'([#5](https://github.com/{REPO}/pull/5))' in linked['body']
        and s['tags'][REPO]['nightly'] == sha,
        linked,
    )
    writes = [c for c in calls() if '-X' in c['args'] or c['args'][0] == 'release']
    to_writes = [c for c in writes if TO in ' '.join(c['args'])]
    repo_writes = [c for c in writes if REPO in ' '.join(c['args'])]
    check(
        'the nightly repository written with its token, the repository with its own',
        to_writes
        and repo_writes
        and all(c['token'] == 'nightly-token' for c in to_writes)
        and all(c['token'] == 'repo-token' for c in repo_writes),
        writes,
    )
    sent = posted('nightly')
    check(
        'one Discord message on the nightly channel, its title and changes',
        len(sent) == 1
        and sent[0]['method'] == 'POST'
        and sent[0]['path'].endswith('?wait=true')
        and sent[0]['body']['embeds'][0]['title'] == f'Nightly 2026-10-15 ({sha[:7]})'
        and f'\\[ci\\] its title ([#5](https://github.com/{REPO}/pull/5))'
        in sent[0]['body']['embeds'][0]['description']
        and sent[0]['agent'].startswith('DiscordBot'),
        sent,
    )
    shown = sent[0]['body']['embeds'][0] if sent else {}
    check(
        'the message links each download with its size, then the source and every nightly',
        [f['name'] for f in shown.get('fields', [])]
        == [
            'Linux x86-64',
            'Linux ARM64',
            'Windows x86-64',
            'Android',
            'macOS ARM64',
            'iOS ARM64',
            'Source',
            'Every nightly',
        ]
        and shown['fields'][2]['value']
        == f'[zip](https://github.com/{TO}/releases/download/{tag}/ScarletNX-windows-x86_64.zip)\n0.0 MB'
        and all(f['inline'] for f in shown['fields'])
        and shown['url'] == f'https://github.com/{TO}/releases/tag/{tag}'
        and shown['thumbnail']['url'].endswith('/dist/icons/hicolor/128x128/apps/wimsy.png')
        and shown['timestamp'].endswith('+00:00'),
        shown,
    )
    # the run again: the files replaced, no news
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '2')
    check(
        'a run again replaces, and edits its card, posting none',
        done.returncode == 0
        and [r['method'] for r in posted('nightly')] == ['POST', 'PATCH']
        and card_id(f'nightly-{sha}') == posted('nightly')[0]['reply']
        and len(releases()) == 2,
        done.stdout + done.stderr,
    )
    # a run of an older commit ending last: not published over the newest
    older_files = ['--run', '40', '--sha', oldest, '--artifacts', str(artifacts('old', oldest))]
    done = publish('nightly', *older_files, '--checkout', str(checkout))
    check(
        'an older commit is not published',
        done.returncode == 0
        and 'older than the newest nightly' in done.stdout
        and all(oldest[:7] not in r['tag_name'] for r in releases()),
        done.stdout + done.stderr,
    )
    # a nightly without one of its packages, or an AppImage without its .zsync, stops, saying which
    for skip, said in (('ScarletNX-windows', 'Windows, x86-64'), ('AppImage.zsync', 'has no ScarletNX-linux')):
        partial = ['--run', '42', '--sha', sha, '--artifacts', str(artifacts(f'part-{len(skip)}', sha, (skip,)))]
        done = publish('nightly', *partial, '--checkout', str(checkout))
        check(f'a nightly missing {skip} stops', done.returncode != 0 and said in done.stderr, done.stderr)
    # dry run: reads only
    log_file.unlink(missing_ok=True)
    save_state(with_releases(TO, [r for r in releases() if r['tag_name'] != tag]))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--dry-run')
    check(
        '--dry-run writes nothing',
        done.returncode == 0
        and not any('-X' in c['args'] or c['args'][0] == 'release' for c in calls())
        and 'would run: gh api -X POST' in done.stdout,
        done.stdout + done.stderr,
    )
    # no rolling prerelease yet: made, on the commit
    save_state(with_releases(REPO, []))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout))
    linked = find('nightly', REPO)
    check(
        'a rolling prerelease made when there is none',
        done.returncode == 0 and linked and linked['target_commitish'] == sha and linked['prerelease'],
        done.stdout + done.stderr,
    )

    # ---- a version tag: the repository's release
    first = {
        'id': 50,
        'tag_name': 'v0.1.0',
        'name': 'ScarletNX v0.1.0',
        'created_at': '2026-10-01',
        'published_at': '2026-10-01',
        'body': '',
        'prerelease': False,
        'assets': [],
    }
    save_state(with_releases(REPO, [*releases(REPO), first]))
    done, planned = run('plan', event=push_run(sha, branch='v0.2.0', number=43))
    check(
        'a version tag pushed is planned as a release',
        planned.get('kind') == 'release' and planned.get('tag') == 'v0.2.0',
        done.stdout + done.stderr,
    )
    done = publish('release', '--tag', 'v0.2.0', *everything(sha), '--checkout', str(checkout))
    version = find('v0.2.0', REPO)
    check(
        "the repository's release: the latest, on the commit, its files and their .zsync",
        done.returncode == 0
        and version
        and version['make_latest'] == 'true'
        and not version['prerelease']
        and version['target_commitish'] == sha
        and sorted(a['name'] for a in version['assets']) == sorted(FILES + ZSYNC),
        done.stdout + done.stderr,
    )
    check(
        'its notes: the changes since the previous version, its files',
        version
        and '## Changes since v0.1.0' in version['body']
        and version['body'].startswith('ScarletNX v0.2.0, built from [`')
        and 'AppImageUpdate takes it to the newest version.' in version['body']
        and f'- \\[ci\\] its title ([#5](https://github.com/{REPO}/pull/5))' in version['body']
        and f'https://github.com/{REPO}/releases/download/v0.2.0/ScarletNX-windows-x86_64.zip' in version['body'],
        version and version['body'],
    )
    sent = posted('releases')
    check(
        'one message on the releases channel, its downloads, the source, and since which version',
        len(sent) == 1
        and sent[0]['body']['embeds'][0]['title'] == 'ScarletNX v0.2.0'
        and [f['name'] for f in sent[0]['body']['embeds'][0]['fields']][len(FILES) :] == ['Source', 'Since']
        and '/compare/v0.1.0...v0.2.0' in sent[0]['body']['embeds'][0]['fields'][len(FILES) + 1]['value'],
        sent,
    )
    git('tag', 'v0.3.0-rc1')
    done = publish('release', '--tag', 'v0.3.0-rc1', *everything(sha), '--checkout', str(checkout))
    candidate = find('v0.3.0-rc1', REPO)
    check(
        'a tag with a "-" is a prerelease, never the latest',
        candidate and candidate['prerelease'] and candidate['make_latest'] == 'false',
        done.stdout + done.stderr,
    )
    done = publish('release', '--tag', 'main', *everything(sha))
    check('--kind release refuses a tag that is no version', done.returncode == 2, done.stderr)

    # ---- a pull request of the repository
    def pull(number, head, fork=False, labels=()):
        return {
            'number': number,
            'state': 'open',
            'title': 'Fix it @everyone',
            'html_url': f'https://x/pull/{number}',
            'user': {'login': 'alice', 'html_url': 'https://x/alice', 'avatar_url': 'https://x/alice.png'},
            'labels': [{'name': n} for n in labels],
            'merged': False,
            'base': {'sha': 'f' * 40},
            'head': {'sha': head, 'repo': {'full_name': 'alice/fork' if fork else REPO}},
        }

    def pr_run(head, number=50, created='2026-10-16T10:00:00Z', failed=('Windows (Clang + LLVM)',)):
        jobs = ['Linux AppImage (x86_64)', 'Linux AppImage (aarch64)', 'Windows (Clang + LLVM)', 'Android APK']
        return {
            'id': number,
            'event': 'pull_request',
            'head_branch': 'x',
            'head_sha': head,
            'conclusion': 'failure' if failed else 'success',
            'status': 'completed',
            'created_at': created,
            'html_url': f'https://x/runs/{number}',
            'head_repository': {'full_name': REPO},
            'jobs': [
                {
                    'name': job,
                    'status': 'completed',
                    'conclusion': 'failure' if job in failed else 'success',
                    'html_url': f'https://x/runs/{number}/{job[:5]}',
                }
                for job in jobs
            ],
        }

    def pr_files(head, name, run_id, skip=()):
        return ['--run', run_id, '--sha', head, '--pr', '7', '--artifacts', str(artifacts(name, head, skip))]

    head1, head2 = 'a' * 40, 'b' * 40
    base_sha = 'e' * 40
    commits = [
        {'sha': f'{n:x}' * 40, 'commit': {'message': f'[gui] commit {n} [x](http://e)\n\nIts body.'}} for n in range(12)
    ]
    save_state(dict(state(), pulls=[pull(7, head1)], commits={'7': commits}, merge_base=base_sha, runs=[pr_run(head1)]))
    done, planned = run('plan', event={'workflow_run': pr_run(head1)})
    check('a pull request run is planned', planned.get('kind') == 'pr' and planned.get('pr') == '7', planned)
    done = publish('pr', *pr_files(head1, 'pr1-part', '50', ('ScarletNX-windows',)))
    pre = find('pr-7')
    check(
        'its prerelease, never the latest, titled PR #N',
        pre and pre['prerelease'] and pre['make_latest'] == 'false' and pre['name'] == 'PR #7: Fix it @everyone',
        pre,
    )
    check(
        'what was not built said in its row, linking the run',
        pre
        and '| **Windows** | x86-64 | not built, [see the run](https://github.com/owner/repo/actions/runs/50) |  |'
        in pre['body']
        and '- **Windows**' not in pre['body']
        and len(pre['assets']) == len(FILES + ZSYNC) - 1,
        pre,
    )
    check(
        'its notes: the pull request, its author, its commit and the main commit it stands on, its commits by subject',
        pre
        and pre['body'].startswith('Test builds of [#7](https://x/pull/7) by [@alice](https://x/alice), at [`aaaaaaa`]')
        and f'on main at [`eeeeeee`](https://github.com/{REPO}/commit/{base_sha}).' in pre['body']
        and '12 commits, the newest first:' in pre['body']
        and "takes it to the newest nightly, not this pull request's next builds." in pre['body']
        and pre['body'].index('commit 11 ') < pre['body'].index('commit 0 ')
        and f'- \\[gui\\] commit 0 \\[x\\](http://e) ([`0000000`](https://github.com/{REPO}/pull/7/commits/{"0" * 40}))'
        in pre['body']
        and 'Its body' not in pre['body'],
        pre and pre['body'],
    )
    prs = posted('prs')
    check(
        'one message on the pull requests channel, in its thread, no one pinged',
        len(prs) == 1
        and prs[0]['path'].endswith('?thread_id=55&wait=true')
        and prs[0]['body']['allowed_mentions'] == {'parse': []}
        and prs[0]['body']['embeds'][0]['title'] == 'PR #7: Fix it @everyone',
        prs,
    )
    shown = prs[0]['body']['embeds'][0] if prs else {}
    check(
        "the card: its author, GitHub's open colour, the failed system linking its log, the first commits and the rest",
        shown.get('author') == {'name': 'alice', 'url': 'https://x/alice', 'icon_url': 'https://x/alice.png'}
        and shown['url'] == 'https://x/pull/7'
        and shown['color'] == 0x1F883D  # GitHub's open; the failed system says so in its field
        and shown['fields'][2]
        == {'name': 'Windows x86-64', 'value': 'Failed\n[log](https://x/runs/50/Windo)', 'inline': True}
        and shown['fields'][0]['value'].startswith(f'[AppImage](https://github.com/{TO}/releases/download/pr-7/')
        and shown['fields'][len(FILES) :]
        == [
            {'name': 'Build', 'value': '[Failure](https://x/runs/50)', 'inline': True},
            {'name': 'Builds', 'value': f'[All files](https://github.com/{TO}/releases/tag/pr-7)', 'inline': True},
        ]
        and '**12 commits**, the newest first' in shown['description']
        and shown['description'].count('\n- ') == 7
        and '- \\[gui\\] commit 11 \\[x\\](http://e)\n' in shown['description']
        and 'commit 5 ' not in shown['description']
        and '/commits/' not in shown['description'].split('**12 commits**')[1]
        and '- and [6 more](https://x/pull/7/commits)' in shown['description']
        and shown['footer']['text'] == 'Edited at each push, comment and build',
        shown,
    )
    message = str(Discord.next_id[0])
    check(
        "the card's id kept in the nightly repository, not in the notes",
        card_id('pr-7') == message and 'discord-message' not in pre['body'],
        state().get('files'),
    )
    # a push: the same prerelease, all four now, the message edited; it stands on a commit a nightly was built from
    save_state(dict(state(), pulls=[pull(7, head2)], merge_base=sha, runs=[pr_run(head2, 51, failed=())]))
    done = publish('pr', *pr_files(head2, 'pr2', '51'))
    pre = find('pr-7')
    prs = posted('prs')
    check(
        'the main commit it stands on links its nightly, to set the two side by side',
        pre
        and f'on main at [`{sha[:7]}`](https://github.com/{REPO}/commit/{sha}) '
        f'([its nightly](https://github.com/{TO}/releases/tag/nightly-2026-10-15-{sha[:7]})).'
        in pre['body']
        and f'([its nightly](https://github.com/{TO}/releases/tag/nightly-2026-10-15-{sha[:7]}))'
        in prs[-1]['body']['embeds'][0]['description'],
        pre and pre['body'],
    )
    check(
        'the push replaces it',
        pre
        and len(pre['assets']) == len(FILES + ZSYNC)
        and head2[:7] in pre['body']
        and 'not built' not in pre['body'],
        pre,
    )
    check(
        'and edits the one message, every system built now',
        len(prs) == 2
        and prs[1]['method'] == 'PATCH'
        and prs[1]['path'].endswith(f'/messages/{message}?thread_id=55')
        and prs[1]['body']['embeds'][0]['color'] == 0x1F883D
        and not any('Failed' in f['value'] for f in prs[1]['body']['embeds'][0]['fields']),
        prs,
    )
    # the run of the push before, ending last
    done = publish('pr', *pr_files(head1, 'pr1', '50'))
    check(
        "an older push's builds are not published",
        'are not published' in done.stdout and head2[:7] in find('pr-7')['body'],
        done.stdout,
    )
    # a message deleted on Discord: a new one, its id kept
    Discord.messages.pop(message)
    done = publish('pr', *pr_files(head2, 'pr2', '51'))
    pre = find('pr-7')
    check(
        'a deleted message is posted again, its new id kept',
        card_id('pr-7') == str(Discord.next_id[0]) and str(Discord.next_id[0]) != message,
        card_id('pr-7'),
    )
    # closed, merged: gone; its card is the cards workflow's (tests/discord_cards_test.py)
    closed = dict(pull(7, head2), state='closed', merged=True)
    before = len(posted('prs'))
    done, planned = run('plan', name='pull_request_target', event={'action': 'closed', 'pull_request': closed})
    s = state()
    check(
        'closed: its prerelease and tag gone',
        'pr-7' not in s['tags'][TO] and not find('pr-7', s=s) and not planned,
        done.stdout + done.stderr,
    )
    check('closing it posts nothing here', len(posted('prs')) == before, posted('prs')[before:])

    # ---- a fork's pull request
    def labeled(login, at):
        return {'event': 'labeled', 'label': {'name': 'publish builds'}, 'actor': {'login': login}, 'created_at': at}

    fork_head = 'c' * 40
    base = dict(
        state(),
        pulls=[pull(9, fork_head, fork=True)],
        permissions={'bob': 'write', 'eve': 'read'},
        runs=[pr_run(fork_head, 60)],
    )
    save_state(base)
    done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
    check("a fork's run without the label is not planned", not planned and 'waits for' in done.stdout, done.stdout)
    for who, at, why in (
        ('eve', '2026-10-16T11:00:00Z', 'by someone not of the team'),
        ('bob', '2026-10-16T09:00:00Z', 'before its last push'),
    ):
        save_state(dict(base, pulls=[pull(9, fork_head, True, ['publish builds'])], events={'9': [labeled(who, at)]}))
        done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
        check(f'a label {why} lets nothing through', not planned, done.stdout)
    approved = [labeled('bob', '2026-10-16T11:00:00Z')]
    save_state(dict(base, pulls=[pull(9, fork_head, True, ['publish builds'])], events={'9': approved}))
    done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
    check("the team's label after the push lets it through", planned.get('pr') == '9', done.stdout + done.stderr)
    event = {
        'action': 'labeled',
        'label': {'name': 'publish builds'},
        'pull_request': pull(9, fork_head, True, ['publish builds']),
    }
    done, planned = run('plan', name='pull_request_target', event=event)
    check('the label put on a finished run publishes it', planned.get('run') == '60', done.stdout + done.stderr)
    save_state(dict(state(), runs=[]))
    done, planned = run('plan', name='pull_request_target', event=event)
    check(
        'the label put while it builds waits for the run',
        not planned and 'has not ended' in done.stdout,
        done.stdout,
    )
    log_file.unlink(missing_ok=True)
    done, planned = run('plan', name='pull_request_target', event=dict(event, action='synchronize'))
    took = [c for c in calls() if c['args'][:3] == ['api', '-X', 'DELETE']]
    check(
        "a fork's push takes the label off, with the repository's token",
        took
        and took[0]['token'] == 'repo-token'
        and took[0]['args'][3].endswith('/labels/publish%20builds')
        and not state()['pulls'][0]['labels'],
        took,
    )

    # ---- no token for the nightly repository: it fails, saying which
    done, _ = run(
        'plan',
        name='pull_request_target',
        event={'action': 'closed', 'pull_request': closed},
        env_extra={'NIGHTLY_TOKEN': ''},
    )
    check(
        'without NIGHTLY_TOKEN it stops, naming it',
        done.returncode != 0 and 'NIGHTLY_TOKEN is not set' in done.stderr,
        done.stdout + done.stderr,
    )

    # ---- no webhook: published, and said
    save_state(dict(state(), pulls=[pull(8, head1)]))
    unhooked = ['--run', '52', '--sha', head1, '--pr', '8', '--artifacts', str(artifacts('pr1', head1))]
    done = publish('pr', *unhooked, hooks=False)
    check(
        'without a webhook it publishes and says nothing was posted',
        done.returncode == 0 and '::notice::No DISCORD_WEBHOOK_PRS' in done.stdout and find('pr-8'),
        done.stdout + done.stderr,
    )

server.shutdown()
for failure in failures:
    print('FAIL:', failure)
print('publish-builds:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
