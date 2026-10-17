#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/discord-cards.py against tests/fake_github.py's fake gh and fake Discord: a push to main's nightly card
posted at once and edited as each package's job ends, then once published; a pull request's card drawn at each push,
comment and review, a fork's built packages waiting for the team's label, merged; an issue's card on its own channel,
its latest comment and how it was closed; a message id kept before the cards taken over; what a card quotes; Discord's
limits; and the Build jobs the cards name being build.yml's. Offline."""

import base64
import datetime
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from fake_github import FAKE_GH, Discord, posted, serve

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'discord-cards.py'
REPO, TO = 'owner/repo', 'owner/nightly'
JOBS = [
    'Linux AppImage (x86_64)',
    'Linux AppImage (aarch64)',
    'Windows (Clang + LLVM)',
    'Android APK',
    'macOS (arm64)',
    'iOS (arm64)',
]
FILES = [
    'ScarletNX-linux-x86_64.AppImage',
    'ScarletNX-linux-aarch64.AppImage',
    'ScarletNX-windows-x86_64.zip',
    'ScarletNX-android-universal.apk',
    'ScarletNX-macos-arm64.zip',
    'ScarletNX-ios-arm64.ipa',
]
N = len(JOBS)  # the packages, one job each
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


spec = importlib.util.spec_from_file_location('discord_cards', SCRIPT)
cards = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cards)
server, HOOK = serve()


def job(name, status, conclusion=None, run=1):
    return {'name': name, 'status': status, 'conclusion': conclusion, 'html_url': f'https://x/runs/{run}/{name[:5]}'}


def jobs(states, run=1):
    """the packages' jobs: each (status, conclusion)"""
    return [job(name, *state, run=run) for name, state in zip(JOBS, states)]


def user(login, kind='User'):
    return {'login': login, 'html_url': f'https://x/{login}', 'avatar_url': f'https://x/{login}.png', 'type': kind}


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
            + ['-c', 'core.hooksPath=' + os.devnull, *args],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        ).stdout.strip()

    checkout.mkdir()
    git('init', '-q', '-b', 'main')
    git('commit', '-q', '--allow-empty', '-m', 'the previous nightly')
    previous = git('rev-parse', 'HEAD')
    git('switch', '-q', '-c', 'feature')
    git('commit', '-q', '--allow-empty', '-m', '[gui] a commit of the pull request')
    git('switch', '-q', 'main')
    git('merge', '-q', '--no-ff', 'feature', '-m', 'Merge pull request #5 from someone/feature\n\n[gui] its title')
    sha = git('rev-parse', 'HEAD')
    git('commit', '-q', '--allow-empty', '-m', 'a newer commit')
    newer = git('rev-parse', 'HEAD')

    def save_state(s):
        state_file.write_text(json.dumps(s))

    def state():
        return json.loads(state_file.read_text())

    def card_id(key):
        entry = state().get('files', {}).get(TO, {}).get(f'discord-cards:cards/{key}')
        return base64.b64decode(entry['content']).decode().strip() if entry else None

    def calls():
        return [json.loads(line) for line in log_file.read_text().splitlines()] if log_file.exists() else []

    def run(step, *extra, event=None, name='push', hooks=('NIGHTLY', 'PRS', 'ISSUES')):
        event_file = root / 'event.json'
        event_file.write_text(json.dumps(event or {}))
        env = dict(
            os.environ,
            PATH=f'{bin_dir}{os.pathsep}{os.environ["PATH"]}',
            FAKE_GH_STATE=str(state_file),
            FAKE_GH_LOG=str(log_file),
            GH_TOKEN='repo-token',
            NIGHTLY_TOKEN='nightly-token',
            GITHUB_EVENT_NAME=name,
            GITHUB_EVENT_PATH=str(event_file),
        )
        for kind in ('RELEASES', 'NIGHTLY', 'PRS', 'ISSUES'):
            env.pop(f'DISCORD_WEBHOOK_{kind}', None)
        env.update({f'DISCORD_WEBHOOK_{kind}': f'{HOOK}-{kind.lower()}' for kind in hooks})
        command = [sys.executable, str(SCRIPT), step, *extra, '--repo', REPO, '--to', TO, '--checkout', str(checkout)]
        return subprocess.run([*command, '--every', '0'], capture_output=True, text=True, env=env, check=False)

    def nightly_release(commit, created, assets=()):
        tag = f'nightly-2026-10-15-{commit[:7]}'
        return {
            'id': len(created),
            'tag_name': tag,
            'name': f'Nightly 2026-10-15 ({commit[:7]})',
            'created_at': created,
            'published_at': created,
            'html_url': f'https://github.com/{TO}/releases/tag/{tag}',
            'body': f'Built from main at [`{commit[:7]}`](https://github.com/{REPO}/commit/{commit}).',
            'assets': [
                {
                    'id': 90 + n,
                    'name': name,
                    'size': 94_563_085,
                    'browser_download_url': f'https://github.com/{TO}/releases/download/{tag}/{name}',
                }
                for n, name in enumerate(assets)
            ],
            'prerelease': False,
        }

    def push_run(commit, steps, number=1):
        return {
            'id': number,
            'event': 'push',
            'head_branch': 'main',
            'head_sha': commit,
            'status': 'queued',
            'conclusion': None,
            'created_at': '2026-10-16T10:00:00Z',
            'html_url': f'https://x/runs/{number}',
            'head_repository': {'full_name': REPO},
            'pull_requests': [],
            'jobs': [],
            'steps': steps,
        }

    base = {
        'next': 1000,
        'clock': 0,
        'pulls': [],
        'events': {},
        'permissions': {},
        'tags': {TO: {}, REPO: {}},
        'releases': {TO: [nightly_release(previous, '2026-10-01')], REPO: []},
    }

    # ---- a push to main: its card at once, then each package as its job ends
    w, b, ok = ('queued', None), ('in_progress', None), ('completed', 'success')
    steps = [
        {'status': 'in_progress', 'jobs': jobs([w] * N)},
        {'status': 'in_progress', 'jobs': jobs([b, b, b, w, w, w])},
        {'status': 'in_progress', 'jobs': jobs([b, b, b, w, w, w])},  # no change: no edit
        {'status': 'in_progress', 'jobs': jobs([ok, b, b, w, w, w])},
        {'status': 'completed', 'conclusion': 'success', 'jobs': jobs([ok] * N)},
    ]
    save_state(dict(base, runs=[push_run(sha, steps)]))
    watch = {'workflow_run': {'id': 1}}
    done = run('event', event=watch, name='workflow_run')
    check(
        'following a build before its card is there posts nothing',
        done.returncode == 0 and not posted('nightly') and 'has no card yet' in done.stdout,
        done.stdout + done.stderr,
    )
    save_state(dict(base, runs=[push_run(sha, steps)]))
    log_file.unlink(missing_ok=True)
    push = {'ref': 'refs/heads/main', 'after': sha, 'repository': {'default_branch': 'main'}}
    done = run('event', event=push, name='push')
    sent = posted('nightly')
    card = sent[0]['body']['embeds'][0] if sent else {}
    check('a push to main posts its card at once', done.returncode == 0 and len(sent) == 1, done.stdout + done.stderr)
    check(
        'the card: building, the changes linked here, every package waiting for its job',
        card.get('title') == f'Nightly 2026-10-15 ({sha[:7]})'
        and card['description'].startswith(f'Building main at [`{sha[:7]}`]')
        and f'- \\[gui\\] its title ([#5](https://github.com/{REPO}/pull/5))' in card['description']
        and card['color'] == cards.BUILDING
        and [f['value'] for f in card['fields'][:N]] == ['Waiting'] * N
        and card['fields'][N]['name'] == 'Source',
        card,
    )
    message = sent[0].get('reply')
    check('its id kept in the nightly repository', card_id(f'nightly-{sha}') == message, state().get('files'))
    stored = [c for c in calls() if c['args'][:3] in (['api', '-X', 'PUT'], ['api', '-X', 'POST'])]
    check(
        'claimed, then kept, on a branch of its own made once, written with the nightly token',
        [c['args'][3] for c in stored] == [f'repos/{TO}/git/refs'] + [f'repos/{TO}/contents/cards/nightly-{sha}'] * 2
        and all(c['token'] == 'nightly-token' for c in stored),
        stored,
    )
    done = run('event', event=watch, name='workflow_run')
    edits = posted('nightly')[1:]
    values = [[f['value'].split('\n')[0] for f in e['body']['embeds'][0]['fields'][:N]] for e in edits]
    check(
        'following the build edits the card at each change of its jobs, never posting',
        done.returncode == 0
        and all(e['method'] == 'PATCH' and e['path'].endswith(f'/messages/{message}') for e in edits)
        and values
        == [
            ['Waiting'] * N,
            ['Building', 'Building', 'Building', 'Waiting', 'Waiting', 'Waiting'],
            ['Built', 'Building', 'Building', 'Waiting', 'Waiting', 'Waiting'],
            ['Built'] * N,
        ],
        values,
    )
    last = edits[-1]['body']['embeds'][0] if edits else {}
    check(
        'a building package links its log; built everywhere, it is being published',
        edits
        and edits[1]['body']['embeds'][0]['fields'][0]['value'] == 'Building\n[log](https://x/runs/1/Linux)'
        and last['fields'][0]['value'] == 'Built\nbeing published'
        and last['description'].startswith(f'Built from main at [`{sha[:7]}`]')
        and 'being published' in last['description'],
        last,
    )
    # published: its links and sizes, the release linked
    published = nightly_release(sha, '2026-10-02', FILES)
    s = state()
    s['releases'][TO].append(published)
    save_state(s)
    done = run('refresh', 'nightly', sha)
    card = posted('nightly')[-1]['body']['embeds'][0]
    check(
        'published: each package its link and size, the card linking the release, scarlet',
        done.returncode == 0
        and posted('nightly')[-1]['method'] == 'PATCH'
        and card['url'] == published['html_url']
        and card['fields'][2]['value']
        == f'[zip](https://github.com/{TO}/releases/download/{published["tag_name"]}/ScarletNX-windows-x86_64.zip)\n94.6 MB'
        and card['color'] == cards.SCARLET
        and card['description'].startswith(f'Built from main at [`{sha[:7]}`]')
        and f'[`{previous[:7]}...{sha[:7]}`]' in card['description'],
        card,
    )
    # an older commit's build ending after a newer nightly: not published; a failed one says so
    s = state()
    s['releases'][TO] = [nightly_release(previous, '2026-10-01'), nightly_release(newer, '2026-10-03')]
    s['runs'] = [dict(push_run(sha, []), status='completed', conclusion='success', jobs=jobs([ok] * N))]
    save_state(s)
    run('refresh', 'nightly', sha)
    card = posted('nightly')[-1]['body']['embeds'][0]
    check(
        'an older commit built after a newer nightly: not published, grey',
        card['description'].startswith('Not published: [Nightly 2026-10-15')
        and card['color'] == cards.CLOSED
        and card['fields'][0]['value'] == 'Built\nnot published',
        card,
    )
    failed = jobs([ok, ok, ('completed', 'failure'), ok, ok, ok])
    s['releases'][TO] = [nightly_release(previous, '2026-10-01')]
    s['runs'] = [dict(push_run(sha, []), status='completed', conclusion='failure', jobs=failed)]
    save_state(s)
    run('refresh', 'nightly', sha)
    card = posted('nightly')[-1]['body']['embeds'][0]
    check(
        'a failed build: not published, the failed package linking its log, amber',
        card['description'].startswith(f'Not published: the build of [`{sha[:7]}`]')
        and card['fields'][2]['value'] == 'Failed\n[log](https://x/runs/1/Windo)'
        and card['color'] == cards.AMBER,
        card,
    )
    done = run('event', event={'ref': 'refs/heads/dev', 'after': sha, 'repository': {'default_branch': 'main'}})
    check('a push to another branch has no card', 'no card' in done.stdout and len(posted('nightly')) == 8, done.stdout)

    # ---- a pull request: drawn at each push, comment and review
    def pull(number, head, fork=False, labels=(), **more):
        made = {
            'number': number,
            'state': 'open',
            'draft': False,
            'title': 'Fix it @everyone',
            'html_url': f'https://x/pull/{number}',
            'user': user('alice'),
            'labels': [{'name': n} for n in labels],
            'merged': False,
            'comments': 0,
            'review_comments': 0,
            'base': {'sha': 'f' * 40},
            'head': {'sha': head, 'repo': {'full_name': 'alice/fork' if fork else REPO}},
        }
        return dict(made, **more)

    head = 'a' * 40
    commits = [{'sha': f'{n:x}' * 40, 'commit': {'message': f'[gui] commit {n}\n\nIts body.'}} for n in range(3)]
    s = dict(
        base,
        pulls=[pull(7, head, labels=['kind: bug'], assignees=[user('bob')])],
        commits={'7': commits},
        merge_base=sha,
        runs=[],
        links={'7': [{'number': 12, 'url': 'https://x/issues/12'}]},
    )
    s['releases'] = {TO: [nightly_release(sha, '2026-10-02', FILES)], REPO: []}
    save_state(s)
    opened = {'action': 'opened', 'pull_request': {'number': 7}}
    done = run('event', event=opened, name='pull_request_target')
    sent = posted('prs')
    card = sent[0]['body']['embeds'][0] if sent else {}
    check(
        'a pull request opened: its card at once, open, on main with that nightly, its commits, nothing built yet',
        done.returncode == 0
        and len(sent) == 1
        and card['url'] == 'https://x/pull/7'
        and card['author'] == {'name': 'alice', 'url': 'https://x/alice', 'icon_url': 'https://x/alice.png'}
        and card['description'].startswith(f'**Open** at [`aaaaaaa`](https://github.com/{REPO}/pull/7/commits/{head})')
        and f'([its nightly](https://github.com/{TO}/releases/tag/nightly-2026-10-15-{sha[:7]})).\n'
        '`kind: bug` | assigned to [bob](https://x/bob) | closes [#12](https://x/issues/12)\n'
        in card['description']
        and '**3 commits**, the newest first\n- \\[gui\\] commit 2\n' in card['description']
        and [f['value'] for f in card['fields']] == ['Waiting'] * N + ['Not started', 'None yet']
        and card['color'] == cards.OPEN,
        card,
    )
    pr_message = sent[0].get('reply')
    # a comment: the newest a person wrote, quoted, the bot's left out
    comments = [
        {
            'user': user('bob'),
            'created_at': '2026-10-16T10:00:00Z',
            'body': 'Looks good <!-- hidden --> ![shot](https://x/a.png)\n\nOne more thing.',
            'html_url': 'https://x/c/1',
        },
        {
            'user': user('github-actions[bot]', 'Bot'),
            'created_at': '2026-10-16T11:00:00Z',
            'body': 'Builds of aaaaaaa',
            'html_url': 'https://x/c/2',
        },
    ]
    s = state()
    s['pulls'] = [pull(7, head, comments=2)]
    s['comments'] = {'7': comments}
    save_state(s)
    said = {'action': 'created', 'issue': {'number': 7, 'pull_request': {'url': 'x'}}, 'comment': {}}
    done = run('event', event=said, name='issue_comment')
    edit = posted('prs')[-1]
    card = edit['body']['embeds'][0]
    check(
        'a comment edits the same card: the latest a person wrote, quoted, no hidden text, an image named',
        done.returncode == 0
        and edit['method'] == 'PATCH'
        and edit['path'].split('?')[0].endswith(f'/messages/{pr_message}')
        and '**Latest comment**, by **bob** ([on GitHub](https://x/c/1))\n> Looks good  [image]\n>\n> One more thing.'
        in card['description']
        and 'Builds of' not in card['description']
        and 'hidden' not in card['description'],
        card['description'],
    )
    # a review after it is the latest
    s = state()
    s['reviews'] = {
        '7': [
            {
                'user': user('carol'),
                'submitted_at': '2026-10-16T12:00:00Z',
                'body': 'Approved.',
                'html_url': 'https://x/r/1',
            }
        ]
    }
    save_state(s)
    run('event', event={'action': 'submitted', 'pull_request': {'number': 7}}, name='pull_request_review')
    card = posted('prs')[-1]['body']['embeds'][0]
    check(
        'a review newer than the comments is the latest',
        '**Latest comment**, by **carol**' in card['description'],
        card,
    )
    # a fork's build done, not labeled: its packages wait for the team
    s = state()
    s['pulls'] = [pull(7, head, fork=True)]
    s['runs'] = [
        {
            'id': 60,
            'event': 'pull_request',
            'head_branch': 'x',
            'head_sha': head,
            'status': 'completed',
            'conclusion': 'success',
            'created_at': '2026-10-16T10:00:00Z',
            'html_url': 'https://x/runs/60',
            'head_repository': {'full_name': 'alice/fork'},
            'pull_requests': [],
            'jobs': jobs([ok] * N, 60),
        }
    ]
    save_state(s)
    done = run('event', event={'workflow_run': {'id': 60}}, name='workflow_run')
    card = posted('prs')[-1]['body']['embeds'][0]
    check(
        "a fork's run, which names no pull request, finds it by its commit; built, it waits for the team's label",
        done.returncode == 0
        and posted('prs')[-1]['method'] == 'PATCH'
        and card['fields'][0]['value'] == "Built\nwaits for the team's label"
        and card['fields'][N]['value'] == '[Passed](https://x/runs/60)',
        card,
    )
    # merged: green, no packages
    s = state()
    s['pulls'] = [pull(7, head, state='closed', merged=True, merge_commit_sha=newer)]
    save_state(s)
    run('event', event={'action': 'closed', 'pull_request': {'number': 7}}, name='pull_request_target')
    card = posted('prs')[-1]['body']['embeds'][0]
    check(
        'merged: green, the merge commit named, no packages',
        card['description'].startswith(f'**Merged** into main at [`{newer[:7]}`]')
        and card['color'] == cards.MERGED
        and 'fields' not in card,
        card,
    )
    # a message id the notes kept before the cards: that message edited, then kept here
    s = state()
    s['pulls'] = [pull(8, head)]
    s['commits'] = {}
    old = {
        'id': 77,
        'tag_name': 'pr-8',
        'name': 'PR #8',
        'created_at': '2026-10-02',
        'html_url': f'https://github.com/{TO}/releases/tag/pr-8',
        'body': 'Builds of #8.\n\n<!-- discord-message: 4242 -->\n',
        'assets': [],
        'prerelease': True,
    }
    s['releases'][TO].append(old)
    save_state(s)
    Discord.messages['4242'] = {}
    run('event', event={'action': 'synchronize', 'pull_request': {'number': 8}}, name='pull_request_target')
    check(
        "a card the prerelease's notes named is edited, then kept here",
        posted('prs')[-1]['method'] == 'PATCH'
        and posted('prs')[-1]['path'].split('?')[0].endswith('/messages/4242')
        and card_id('pr-8') == '4242',
        posted('prs')[-1],
    )

    # ---- an issue: its own channel, its latest comment, how it closed
    issue = {
        'number': 12,
        'state': 'open',
        'state_reason': None,
        'title': 'It crashes',
        'html_url': 'https://x/issues/12',
        'user': user('dave'),
        'labels': [{'name': 'kind: bug', 'color': 'd73a4a'}, {'name': 'priority: high', 'color': 'd93f0b'}],
        'type': {'name': 'Bug'},
        'assignees': [user('erin')],
        'milestone': {'title': 'v0.1', 'html_url': 'https://x/milestone/1'},
        'comments': 0,
        'body': 'Steps:\n\n1. open it\n2. ' + 'word ' * 100,
    }
    s = state()
    s['issues'] = {'12': issue}
    s['comments'] = {}
    s['links'] = {'12': [{'number': 267, 'url': 'https://x/pull/267'}]}
    save_state(s)
    done = run('event', event={'action': 'opened', 'issue': {'number': 12}}, name='issues')
    sent = posted('issues')
    card = sent[0]['body']['embeds'][0] if sent else {}
    check(
        'an issue opened: its card on the issues channel, open, its labels, its text quoted and cut by a word',
        done.returncode == 0
        and len(sent) == 1
        and card['title'] == '#12: It crashes'
        and card['description'].startswith(
            '**Open**\n**Bug** | \U0001f7e5 `kind: bug` \U0001f7e7 `priority: high` | assigned to [erin](https://x/erin) | milestone '
            '[v0.1](https://x/milestone/1) | fixed by [#267](https://x/pull/267)\n\n> Steps:\n>\n> 1. open it'
        )
        and card['description'].endswith('word...')
        and card['color'] == cards.OPEN
        and card['footer']['text'].startswith('0 comments'),
        card,
    )
    s = state()
    s['issues']['12'] = dict(issue, comments=1, state='closed', state_reason='completed')
    s['comments'] = {
        '12': [
            {'user': user('erin'), 'created_at': '2026-10-16T10:00:00Z', 'body': 'Fixed.', 'html_url': 'https://x/c/9'}
        ]
    }
    save_state(s)
    run('event', event={'action': 'created', 'issue': {'number': 12}}, name='issue_comment')
    card = posted('issues')[-1]['body']['embeds'][0]
    check(
        'commented on and closed: the same card, closed as done, its latest comment',
        posted('issues')[-1]['method'] == 'PATCH'
        and card['description'].startswith('**Closed** as done')
        and '**Latest comment**, by **erin**' in card['description']
        and card['color'] == cards.DONE,
        card,
    )
    s['issues']['12'] = dict(issue, state='closed', state_reason='not_planned')
    save_state(s)
    run('event', event={'action': 'closed', 'issue': {'number': 12}}, name='issues')
    card = posted('issues')[-1]['body']['embeds'][0]
    check(
        'closed as not planned: grey',
        card['description'].startswith('**Closed** as not planned') and card['color'] == cards.NOT_PLANNED,
        card,
    )
    done = run('event', event={'action': 'edited', 'issue': {'number': 12}}, name='issues', hooks=('NIGHTLY', 'PRS'))
    check(
        'without the issues webhook: nothing posted, and said',
        '::notice::No DISCORD_WEBHOOK_ISSUES' in done.stdout and len(posted('issues')) == 3,
        done.stdout,
    )
    # one event starts several runs at once: one makes the cards' branch while another looked, and one's write comes
    # before another's; the card is kept all the same
    log_file.unlink(missing_ok=True)
    s = state()
    s['issues']['14'] = dict(issue, number=14)
    s['hide_branch_once'], s['fail_puts'] = 'discord-cards', 1
    s['put_error'] = 'gh: Invalid request.\n\n"sha" wasn\'t supplied. (HTTP 422)'  # GitHub's, the file made first
    save_state(s)
    done = run('event', event={'action': 'opened', 'issue': {'number': 14}}, name='issues')
    writes = [c['args'][3] for c in calls() if c['args'][:2] == ['api', '-X']]
    check(
        "a branch another run made first, and a claim another run's came before: the card kept all the same",
        done.returncode == 0
        and card_id('issue-14') == posted('issues')[-1].get('reply')
        and writes == [f'repos/{TO}/git/refs'] + [f'repos/{TO}/contents/cards/issue-14'] * 3,
        done.stdout + done.stderr + str(writes),
    )
    s = state()
    s['issues']['15'] = dict(issue, number=15)
    s['pass_puts'], s['fail_puts'] = 1, 1  # claimed, then its id cannot be written
    s['put_error'] = 'gh: Resource not accessible by integration (HTTP 403)'
    save_state(s)
    done = run('event', event={'action': 'opened', 'issue': {'number': 15}}, name='issues')
    check(
        'a card that cannot be kept stops the run, naming its message',
        done.returncode != 0
        and f'issue-15 is message {posted("issues")[-1].get("reply")} on Discord, but it could not be kept'
        in done.stderr
        and 'HTTP 403' in done.stderr,
        done.stderr,
    )

    # a card another run is posting: waited for, never posted twice; one a run that died claimed: taken over
    def claim_of(key, when):
        s = state()
        s['issues'][key.split('-')[1]] = dict(issue, number=int(key.split('-')[1]))
        content = base64.b64encode(f'posting {when}\n'.encode()).decode()
        s['files'][TO][f'discord-cards:cards/{key}'] = {'content': content, 'sha': f'claim-{key}'}
        save_state(s)

    now = datetime.datetime.now(datetime.UTC).isoformat(timespec='seconds')
    claim_of('issue-16', now)
    before = len(posted('issues'))
    done = run('event', event={'action': 'labeled', 'issue': {'number': 16}}, name='issues')
    check(
        'a card another run is posting is waited for, and not posted again',
        done.returncode == 0
        and 'issue-16 is being posted by another run' in done.stdout
        and len(posted('issues')) == before
        and state()['files'][TO]['discord-cards:cards/issue-16']['sha'] == 'claim-issue-16',
        done.stdout + done.stderr,
    )
    claim_of('issue-17', '2026-01-01T00:00:00+00:00')
    done = run('event', event={'action': 'opened', 'issue': {'number': 17}}, name='issues')
    check(
        'a claim a run that died left is taken over: the card posted, its id kept',
        done.returncode == 0
        and card_id('issue-17') == posted('issues')[-1].get('reply')
        and len(posted('issues')) == before + 1,
        done.stdout + done.stderr,
    )
    # dry run: reads only
    log_file.unlink(missing_ok=True)
    s = state()
    s['issues']['13'] = dict(issue, number=13)
    save_state(s)
    done = run('event', '--dry-run', event={'action': 'opened', 'issue': {'number': 13}}, name='issues')
    check(
        '--dry-run writes nothing, on GitHub or Discord',
        done.returncode == 0
        and not any('-X' in c['args'] for c in calls())
        and 'would post on Discord (issues)' in done.stdout
        and len(posted('issues')) == 6,
        done.stdout + done.stderr,
    )

    # ---- a label in GitHub's colour, as near as a square gets; GitHub's colour for each state
    check(
        "labels as GitHub's pills after their colour's square, a grey by its lightness, none without a colour",
        [cards.chip({'name': n, 'color': c}) for n, c in (('a', 'b60205'), ('b', 'd93f0b'), ('c', 'fbca04'))]
        == ['\U0001f7e5 `a`', '\U0001f7e7 `b`', '\U0001f7e8 `c`']
        and [cards.square(c) for c in ('0e8a16', '0075ca', 'd4c5f9', 'ededed', '24292f', '5c3317')]
        == ['\U0001f7e9', '\U0001f7e6', '\U0001f7ea', '\u2b1c', '\u2b1b', '\U0001f7eb']
        and cards.chip({'name': 'x'}) == '`x`',
    )
    check(
        "GitHub's colours: open, draft, merged, closed; an issue done or not planned",
        (cards.OPEN, cards.DRAFT, cards.MERGED, cards.REFUSED, cards.DONE, cards.NOT_PLANNED)
        == (0x1F883D, 0x6E7781, 0x8250DF, 0xCF222E, 0x8250DF, 0x6E7781),
    )

    # ---- what a card quotes, and Discord's limits
    check(
        'a quote: no hidden comment, images and folded parts named, cut by a word, a code block kept closed',
        cards.excerpt('a <!-- x --> b <img src="y"> <details>z</details>') == '> a  b [image] [details]'
        and cards.excerpt('one two three', 9) == '> one two...'
        and cards.excerpt('```\ncode code code', 12).endswith('\n> ```'),
        [cards.excerpt('```\ncode code code', 12)],
    )
    hub = cards.Hub(REPO, TO, True)
    lines = '\n'.join(f'- change {n} ([#{n}](https://x/pull/{n}))' for n in range(400))
    field = {'name': 'Linux x86-64', 'value': '[AppImage](https://x/a)\n94.6 MB', 'inline': True}
    cut = hub.embed('Nightly', 'https://x', lines, [field] * 6)
    check(
        'a long description cut to what Discord takes, by whole lines, the fields kept',
        len(cut['description']) <= cards.DESCRIPTION
        and lines.startswith(cut['description'])
        and lines[len(cut['description'])] == '\n'
        and len(cut['fields']) == 6,
        len(cut['description']),
    )
    big = {'name': 'x' * 256, 'value': 'y' * 1024, 'inline': True}
    cut = hub.embed('Nightly', 'https://x', lines, [big] * 6)
    check(
        'fields over the whole limit leave no description, and it ends',
        cut['description'] == '',
        cut['description'][:80],
    )

# the jobs the cards name are Build's: a renamed job would leave its package waiting forever
build = (ROOT / '.github' / 'workflows' / 'build.yml').read_text()
for name in cards.JOBS.values():
    template = re.sub(r'\((x86_64|aarch64)\)', '(${{ matrix.arch }})', name)
    arch = re.search(r'\((x86_64|aarch64)\)', name)
    check(
        f'{name} is a job of build.yml',
        f'name: {name}\n' in build or (f'name: {template}\n' in build and arch and f'{arch.group(1)}' in build),
    )

server.shutdown()
for failure in failures:
    print('FAIL:', failure)
print('discord-cards:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
