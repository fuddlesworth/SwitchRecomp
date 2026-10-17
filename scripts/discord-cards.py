#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Discord cards: one message for each pull request, each issue and each push to main, posted when it comes and edited
as it changes, so each channel shows them as they are now.

    scripts/discord-cards.py event --repo OWNER/REPO --to OWNER/NIGHTLY [--checkout DIR] [--every SECONDS]
    scripts/discord-cards.py refresh --repo OWNER/REPO --to OWNER/NIGHTLY {pr N | issue N | nightly SHA}

event follows the event GitHub Actions ran it for (GITHUB_EVENT_NAME, GITHUB_EVENT_PATH): a push to main posts that
commit's card on DISCORD_WEBHOOK_NIGHTLY's channel; a pull request opened, pushed to, edited, reviewed, commented on or
closed, its card on DISCORD_WEBHOOK_PRS's; an issue opened, edited, labeled, commented on or closed, its card on
DISCORD_WEBHOOK_ISSUES's. A Build run requested (workflow_run) is followed while it builds: its pull request's or
nightly's card is edited as each package's job ends. scripts/publish-builds.py redraws a card once its files are
published.

A card is drawn from what GitHub says now, never from the event alone, so any event draws it whole: its state, the
commits or the changes, the latest comment (bots' left out), and each package: waiting, building, built, failed, or
published with its link and size. Its message id is kept in NIGHTLY's discord-cards branch, cards/<key> (pr-N, issue-N,
nightly-SHA), written with NIGHTLY_TOKEN. Following a build only edits a card, never posts one, so it never posts a card
another run is posting. Without a webhook nothing is posted, and the run says so. --dry-run reads, and prints each write
instead. .github/workflows/cards.yml runs it; tests/discord_cards_test.py runs it against a fake gh and a fake Discord.
"""

import argparse
import base64
import colorsys
import datetime
import importlib.util
import json
import math
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

spec = importlib.util.spec_from_file_location('nightly_notes', Path(__file__).with_name('nightly-notes.py'))
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)

SERVER = notes.SERVER
BRANCH = 'discord-cards'
LABEL = 'publish builds'  # what lets a fork's pull request be published (scripts/publish-builds.py)
# The job of .github/workflows/build.yml that makes each release file
JOBS = {
    'ScarletNX-linux-x86_64.AppImage': 'Linux AppImage (x86_64)',
    'ScarletNX-linux-aarch64.AppImage': 'Linux AppImage (aarch64)',
    'ScarletNX-windows-x86_64.zip': 'Windows (Clang + LLVM)',
    'ScarletNX-android-universal.apk': 'Android APK',
    'ScarletNX-macos-arm64.zip': 'macOS (arm64)',
    'ScarletNX-ios-arm64.ipa': 'iOS (arm64)',
}
# a nightly's colours: building, published, a package failed, not published
BUILDING, SCARLET, AMBER, CLOSED = 0x1F6FEB, 0xB0102A, 0xD29922, 0x6E7681
# GitHub's own, for a pull request and an issue: open, draft, merged, closed; an issue done, or not planned
OPEN, DRAFT, MERGED, REFUSED, DONE, NOT_PLANNED = 0x1F883D, 0x6E7781, 0x8250DF, 0xCF222E, 0x8250DF, 0x6E7781
# Discord colours no text: a label is its name as GitHub's pill, after the square of its colour's hue (degrees)
HUES = {0: '\U0001f7e5', 30: '\U0001f7e7', 50: '\U0001f7e8', 120: '\U0001f7e9', 210: '\U0001f7e6', 275: '\U0001f7ea'}
BROWN, BLACK, WHITE = '\U0001f7eb', '\u2b1b', '\u2b1c'
# the mascot beside each card
WIMSY = 'https://raw.githubusercontent.com/{repo}/main/dist/icons/hicolor/128x128/apps/wimsy.png'
# what a card lists, at most, before "and N more": a nightly's changes, a pull request's commits; a text's letters
MOST_LINES, MOST_COMMITS, MOST_TEXT = 10, 6, 300
# Discord's limits: an embed's description, and its whole text
DESCRIPTION, EMBED = 4096, 6000
# where scripts/publish-builds.py kept a pull request's message id before the cards
MARKER = re.compile(r'<!-- discord-message: (\d+) -->')
CHANNELS = {'pr': 'prs', 'issue': 'issues', 'nightly': 'nightly'}
# a job's states before it runs
WAITING = {'queued', 'waiting', 'pending', 'requested'}
# a write to the cards' branch that another run's came before, tried again this many times in all
CONFLICT, TRIES = re.compile(r'HTTP 409|is at [0-9a-f]+ but expected|"?sha"? wasn.t supplied|does not match'), 3
# A card's file while the run that made it posts the card: CLAIM and when. Another run waits for the id, at most WAIT
# seconds; a claim older than STALE seconds is a run that died, and taken over
CLAIM, WAIT, STALE = 'posting', 60, 300


class Hub:
    """GitHub through gh (REPO with GH_TOKEN, the nightly repository with NIGHTLY_TOKEN), and Discord's webhooks"""

    def __init__(self, repo, to, dry):
        self.repo, self.to, self.dry = repo, to, dry
        self.tokens = {repo: os.environ.get('GH_TOKEN', ''), to: os.environ.get('NIGHTLY_TOKEN', '')}

    def token(self, repo):
        if repo == self.to and not self.tokens[repo] and not self.dry:
            raise RuntimeError(f'NIGHTLY_TOKEN is not set: nothing can be written in {self.to}')
        return self.tokens[repo]

    def gh(self, args, repo, data=None, write=False):
        if write and self.dry:
            print('would run: gh', ' '.join(args), json.dumps(data) if data is not None else '')
            return {}
        run = subprocess.run(
            ['gh', *args],
            input=json.dumps(data) if data is not None else None,
            capture_output=True,
            text=True,
            env=dict(os.environ, GH_TOKEN=self.token(repo)),
            check=False,
        )
        if run.returncode:
            raise RuntimeError(f'gh {" ".join(args)}: {run.stderr.strip() or run.stdout.strip()}')
        return json.loads(run.stdout) if run.stdout.strip() else {}

    def read(self, path, repo=None):
        return self.gh(['api', path], repo or self.repo)

    def graphql(self, query):
        return self.gh(['api', 'graphql', '-f', f'query={query}'], self.repo)

    def missing(self, path, repo=None):
        """path read, or None when GitHub has nothing there (404)"""
        try:
            return self.read(path, repo)
        except RuntimeError as error:
            if 'HTTP 404' in str(error) or 'Not Found' in str(error):
                return None
            raise

    def pages(self, path, repo=None):
        """every item of a listing, 100 a page"""
        items, page = [], 1
        while True:
            sep = '&' if '?' in path else '?'
            got = self.gh(['api', f'{path}{sep}per_page=100&page={page}'], repo or self.repo)
            if isinstance(got, dict):
                got = next((got[key] for key in ('artifacts', 'workflow_runs', 'jobs') if key in got), got)
            items += got
            if len(got) < 100:
                return items
            page += 1

    def write(self, repo, method, path, data=None):
        args = ['api', '-X', method, path, *(['--input', '-'] if data is not None else [])]
        return self.gh(args, repo, data, write=True)

    def releases(self, repo):
        return self.pages(f'repos/{repo}/releases', repo)

    def webhook(self, channel):
        return os.environ.get(f'DISCORD_WEBHOOK_{channel.upper()}', '')

    def post(self, channel, embed, message=None, create=True):
        """the message of channel (releases, nightly, prs or issues) edited when message names it, else posted when
        create: its id, or None when there is none"""
        hook = self.webhook(channel)
        if not hook:
            print(f'::notice::No DISCORD_WEBHOOK_{channel.upper()}: nothing posted on Discord')
            return message
        body = {'embeds': [embed], 'allowed_mentions': {'parse': []}}
        if self.dry:
            print(f'would {"edit" if message else "post"} on Discord ({channel}):', json.dumps(body))
            return message
        # a webhook of a forum channel or a thread names it in its query (thread_id=...), which both requests keep
        base, _, query = hook.partition('?')
        edit = f'{base}/messages/{message}' + (f'?{query}' if query else '')
        post = f'{base}?' + (f'{query}&' if query else '') + 'wait=true'
        for method, url in (('PATCH', edit), ('POST', post)):
            if (method == 'PATCH' and not message) or (method == 'POST' and not create):
                continue
            request = urllib.request.Request(
                url,
                data=json.dumps(body).encode(),
                method=method,
                # Discord refuses urllib's own agent
                headers={'Content-Type': 'application/json', 'User-Agent': f'DiscordBot ({SERVER}/{self.repo}, 1)'},
            )
            try:
                with urllib.request.urlopen(request, timeout=30) as reply:
                    return json.loads(reply.read() or b'{}').get('id', message)
            except urllib.error.HTTPError as error:
                if method == 'PATCH' and error.code == 404:  # deleted on Discord: a new one, when it may post
                    continue
                print(f'::warning::Discord answered {error.code} to the {channel} message: {error.read()[:200]!r}')
                return message
            except (urllib.error.URLError, OSError) as error:
                print(f'::warning::Discord could not be reached for the {channel} message: {error}')
                return message
        return None

    def embed(self, title, url, description, fields=(), color=SCARLET, footer='', author=None):
        """a message's embed, its description cut by whole lines, the last first, to what Discord takes: what it lists
        gives way, the fields stay"""
        title = title[:256]
        room = EMBED - len(title) - len(footer) - len((author or {}).get('name', ''))
        room = max(0, min(DESCRIPTION, room - sum(len(f['name']) + len(f['value']) for f in fields)))
        while len(description) > room:
            description = description.rsplit('\n', 1)[0] if '\n' in description else description[:room]
        made = {
            'title': title,
            'url': url,
            'description': description,
            'color': color,
            'thumbnail': {'url': WIMSY.format(repo=self.repo)},
            'timestamp': datetime.datetime.now(datetime.UTC).isoformat(timespec='seconds'),
        }
        if fields:
            made['fields'] = list(fields)
        if footer:
            made['footer'] = {'text': footer}
        if author:
            made['author'] = author
        return made


class Store:
    """each card's message id, a file of the nightly repository's BRANCH: cards/<key>"""

    def __init__(self, hub):
        self.hub = hub

    def get(self, key):
        found = self.hub.missing(f'repos/{self.hub.to}/contents/cards/{key}?ref={BRANCH}', self.hub.to)
        if found and found.get('content'):
            return base64.b64decode(found['content']).decode().strip(), found['sha']
        return None, None

    def branch(self):
        """the branch made from the default one when there is none; one another run made first has been made"""
        to = self.hub.to
        if not self.hub.missing(f'repos/{to}/git/ref/heads/{BRANCH}', to):
            default = self.hub.read(f'repos/{to}', to)['default_branch']
            head = self.hub.read(f'repos/{to}/git/ref/heads/{default}', to)['object']['sha']
            try:
                self.hub.write(to, 'POST', f'repos/{to}/git/refs', {'ref': f'refs/heads/{BRANCH}', 'sha': head})
            except RuntimeError as error:
                if 'Reference already exists' not in str(error):
                    raise

    def claim(self, key, sha=None):
        """key's file made, saying this run posts the card: its sha, or None when another run's came first. GitHub
        makes a file only once without its sha, so two runs never both claim one; sha: a dead run's claim, replaced"""
        self.branch()
        now = datetime.datetime.now(datetime.UTC).isoformat(timespec='seconds')
        data = {
            'message': f'The Discord message of {key}, being posted',
            'content': base64.b64encode(f'{CLAIM} {now}\n'.encode()).decode(),
            'branch': BRANCH,
        }
        if sha:
            data['sha'] = sha
        try:
            made = self.hub.write(self.hub.to, 'PUT', f'repos/{self.hub.to}/contents/cards/{key}', data)
        except RuntimeError as error:
            if CONFLICT.search(str(error)):
                return None
            raise
        return (made.get('content') or {}).get('sha', 'dry run')

    def release(self, key, sha):
        """a claim given back, its card not posted"""
        data = {'message': f'The Discord message of {key}, not posted', 'sha': sha, 'branch': BRANCH}
        self.hub.write(self.hub.to, 'DELETE', f'repos/{self.hub.to}/contents/cards/{key}', data)

    def put(self, key, message, sha=None):
        """message kept as key's. Runs of other cards write the branch too, at the same moment when one event starts
        several, so a write that lost to another's is tried again on what it left. A message that still cannot be kept
        is named, so it can be found on Discord"""
        to = self.hub.to
        self.branch()
        for attempt in range(TRIES):
            data = {
                'message': f'The Discord message of {key}',
                'content': base64.b64encode(f'{message}\n'.encode()).decode(),
                'branch': BRANCH,
            }
            if sha:
                data['sha'] = sha
            try:
                self.hub.write(to, 'PUT', f'repos/{to}/contents/cards/{key}', data)
                return
            except RuntimeError as error:
                if attempt == TRIES - 1 or not CONFLICT.search(str(error)):
                    raise RuntimeError(
                        f'{key} is message {message} on Discord, but it could not be kept: {error}'
                    ) from error
                time.sleep(attempt + 1)
                sha = self.get(key)[1]


def excerpt(text, most=MOST_TEXT):
    """a comment or a description as a card quotes it: no hidden comments, an image as [image], cut by a word"""
    text = re.sub(r'<!--.*?-->', '', text or '', flags=re.DOTALL)
    text = re.sub(r'!\[[^\]]*\]\([^)]*\)|<img\b[^>]*>', '[image]', text)
    text = re.sub(r'<details>.*?</details>', '[details]', text, flags=re.DOTALL | re.IGNORECASE)
    text = re.sub(r'\n{3,}', '\n\n', text.replace('\r', '')).strip()
    if len(text) > most:
        text = text[:most].rsplit(' ', 1)[0].rstrip() + '...'
    if text.count('```') % 2:  # a code block cut open: kept closed
        text += '\n```'
    return '\n'.join(f'> {line}' if line else '>' for line in text.splitlines())


def square(color):
    """the coloured square nearest a colour (RRGGBB): a grey by its lightness, a dark orange brown, else by hue"""
    hue, light, saturation = colorsys.rgb_to_hls(*(int(color[i : i + 2], 16) / 255 for i in (0, 2, 4)))
    if light < 0.2 or light > 0.92 or saturation < 0.2:
        return WHITE if light >= 0.5 else BLACK
    hue *= 360
    if 10 <= hue <= 45 and light < 0.3:
        return BROWN
    return HUES[min(HUES, key=lambda h: min(abs(hue - h), 360 - abs(hue - h)))]


def chip(label):
    """a label as a card shows it: the square of its colour, then its name as GitHub's pill"""
    color = label.get('color') or ''
    if not re.fullmatch(r'[0-9A-Fa-f]{6}', color):
        return f'`{label["name"]}`'
    return f'{square(color)} `{label["name"]}`'


def plural(count, word):
    return f'{count} {word}{"" if count == 1 else "s"}'


class Cards:
    def __init__(self, hub, checkout='.', keep=30, every=20):
        self.hub, self.checkout, self.keep, self.every = hub, checkout, keep, every
        self.store = Store(hub)
        self.url = f'{SERVER}/{hub.repo}'
        self._releases = None

    # ---- what GitHub says now
    def nightly_releases(self):
        """the nightly repository's releases, the newest published first: their created_at is their tag's commit's
        date, which every nightly there shares"""
        if self._releases is None:
            published = sorted(self.hub.releases(self.hub.to), key=lambda r: r.get('published_at') or '', reverse=True)
            self._releases = published
        return self._releases

    def build_of(self, sha):
        """the newest Build run of sha"""
        path = f'repos/{self.hub.repo}/actions/workflows/build.yml/runs?head_sha={sha}'
        runs = [r for r in self.hub.pages(path) if r['event'] in ('push', 'pull_request')]
        return max(runs, key=lambda r: r['created_at']) if runs else None

    def jobs_of(self, run):
        return self.hub.pages(f'repos/{self.hub.repo}/actions/runs/{run["id"]}/jobs') if run else []

    def latest_comment(self, number, count, pr=None):
        """the newest comment of an issue or a pull request (its reviews and review comments too) a person wrote"""
        found = []

        def last_of(path, total):
            for page in range(math.ceil(total / 100), 0, -1):  # from the last page back, to the first a person wrote on
                got = [c for c in self.hub.read(f'{path}?per_page=100&page={page}') if not bot(c)]
                if got:
                    return got
            return []

        def bot(item):
            return (item.get('user') or {}).get('type') == 'Bot' or not (item.get('body') or '').strip()

        found += last_of(f'repos/{self.hub.repo}/issues/{number}/comments', count)
        if pr:
            found += [
                dict(r, created_at=r['submitted_at'])
                for r in self.hub.pages(f'repos/{self.hub.repo}/pulls/{number}/reviews')
                if r.get('submitted_at') and not bot(r)
            ]
            found += last_of(f'repos/{self.hub.repo}/pulls/{number}/comments', pr.get('review_comments', 0))
        return max(found, key=lambda c: c['created_at']) if found else None

    def linked(self, number, pr):
        """the issues a pull request closes, or the pull requests that close an issue: (number, url) each"""
        field, refs = ('pullRequest', 'closingIssuesReferences') if pr else ('issue', 'closedByPullRequestsReferences')
        owner, name = self.hub.repo.split('/')
        query = (
            f'query{{repository(owner:"{owner}",name:"{name}"){{{field}(number:{number})'
            f'{{{refs}(first:10){{nodes{{number url}}}}}}}}}}'
        )
        nodes = self.hub.graphql(query)['data']['repository'][field][refs]['nodes']
        return [(node['number'], node['url']) for node in nodes]

    @staticmethod
    def meta(item, kind='', links=(), word=''):
        """what a card says of its item besides its state: its type, its labels, who it is assigned to, its milestone,
        and the issues or pull requests it is linked to"""
        parts = [f'**{kind}**'] if kind else []
        labels = ' '.join(chip(label) for label in item.get('labels', []))
        if labels:
            parts.append(labels)
        people = item.get('assignees') or []
        if people:
            parts.append('assigned to ' + ', '.join(f'[{p["login"]}]({p["html_url"]})' for p in people))
        if item.get('milestone'):
            parts.append(f'milestone [{item["milestone"]["title"]}]({item["milestone"]["html_url"]})')
        if links:
            parts.append(f'{word} ' + ', '.join(f'[#{n}]({url})' for n, url in links))
        return ' | '.join(parts)

    def comment_lines(self, comment):
        if not comment:
            return []
        who = comment['user']['login']
        return ['', f'**Latest comment**, by **{who}** ([on GitHub]({comment["html_url"]}))', excerpt(comment['body'])]

    def packages(self, jobs, release, built):
        """a field per release file: published, its link and size; else what its job does; built: what a built one
        waits for"""
        assets = {a['name']: a for a in (release or {}).get('assets', [])}
        by_name = {j['name']: j for j in jobs}
        fields = []
        for name in notes.ordered(list(JOBS)):
            system, arch, kind = notes.SYSTEMS[name]
            job, asset = by_name.get(JOBS[name]), assets.get(name)
            if asset:
                value = f'[{kind}]({asset["browser_download_url"]})\n{notes.size(asset["size"])}'
            elif not job or job['status'] in WAITING:
                value = 'Waiting'
            elif job['status'] != 'completed':
                value = f'Building\n[log]({job["html_url"]})'
            elif job['conclusion'] == 'success':
                value = f'Built\n{built}' if built else 'Built'
            elif job['conclusion'] in ('cancelled', 'skipped'):
                value = job['conclusion'].capitalize()
            else:
                value = f'Failed\n[log]({job["html_url"]})'
            fields.append({'name': system if ',' in arch else f'{system} {arch}', 'value': value, 'inline': True})
        return fields

    @staticmethod
    def failed(jobs):
        return any(
            j['name'] in JOBS.values() and j['status'] == 'completed' and j['conclusion'] not in ('success', 'skipped')
            for j in jobs
        )

    @staticmethod
    def listed(lines, more_url, most):
        shown = [f'- {line}' for line in lines[:most]]
        if len(lines) > most:
            shown.append(f'- and [{len(lines) - most} more]({more_url})')
        return shown

    def show(self, key, embed_of, create=True, tries=TRIES):
        """key's card drawn by embed_of() and posted, or its message edited; nothing drawn when it cannot be shown"""
        channel = CHANNELS[key.split('-')[0]]
        if not self.hub.webhook(channel):
            print(f'::notice::No DISCORD_WEBHOOK_{channel.upper()}: no card for {key}')
            return None
        message, sha = self.claimed(key)
        if message is False:
            print(f'::notice::{key} is being posted by another run, which has not kept its id yet')
            return None
        if not message and key.startswith('pr-'):  # kept in its prerelease's notes before the cards
            release = next((r for r in self.nightly_releases() if r['tag_name'] == key), None)
            message = next(iter(MARKER.findall((release or {}).get('body') or '')), None)
        if not message and not create:
            print(f'{key} has no card yet: a run that may post it will')
            return None
        claim = None
        if not message:  # this run posts it, once it has claimed it; one that claimed it first posts it instead
            claim = self.store.claim(key, sha)
            if not claim and tries > 1:
                return self.show(key, embed_of, create, tries - 1)
            if not claim:
                raise RuntimeError(f'{key} could not be claimed: other runs kept making its file first')
            sha = claim
        posted = self.hub.post(channel, embed_of(), message, create)
        if posted and (posted != message or not sha or claim):  # a new message, or one not kept here yet
            self.store.put(key, posted, sha)
        elif claim and claim != 'dry run':
            self.store.release(key, claim)
        print(f'{key}: card {"posted" if posted and posted != message else "edited" if posted else "not shown"}')
        return posted

    def claimed(self, key):
        """key's message id and its file's sha, waiting while another run posts the card: (None, sha) for a claim
        STALE seconds old, a run that died, to be taken over; (False, None) when the wait ends first"""
        for _ in range(max(1, int(WAIT / max(self.every, 1)))):
            message, sha = self.store.get(key)
            if not (message or '').startswith(CLAIM):
                return message, sha
            made = datetime.datetime.fromisoformat(message.split()[1])
            if (datetime.datetime.now(datetime.UTC) - made).total_seconds() > STALE:
                return None, sha
            time.sleep(self.every)
        return False, None

    # ---- the cards
    def pr(self, number, create=True, run=None, jobs=None):
        return self.show(f'pr-{number}', lambda: self.pr_embed(number, run, jobs), create)

    def pr_embed(self, number, run=None, jobs=None):
        repo, url = self.hub.repo, self.url
        pr = self.hub.read(f'repos/{repo}/pulls/{number}')
        head = pr['head']['sha']
        commit = f'{url}/pull/{number}/commits/{head}'
        commits = list(reversed(self.hub.pages(f'repos/{repo}/pulls/{number}/commits')))
        subjects = [
            notes.escape((c['commit']['message'] or '').splitlines()[0] if c['commit']['message'] else '')
            for c in commits
        ]
        comment = self.latest_comment(number, pr.get('comments', 0), pr)
        author = {'name': pr['user']['login'], 'url': pr['user']['html_url']}
        if pr['user'].get('avatar_url'):
            author['icon_url'] = pr['user']['avatar_url']
        title = f'PR #{number}: {pr["title"]}'
        fields, color = [], REFUSED
        if pr.get('merged'):
            merged = pr.get('merge_commit_sha') or ''
            said = f'**Merged** into main at [`{merged[:7]}`]({url}/commit/{merged}): the next nightly has it.'
            color = MERGED
        elif pr['state'] == 'closed':
            said = '**Closed** without a merge.'
        else:
            base = self.hub.read(f'repos/{repo}/compare/{pr["base"]["sha"]}...{head}')['merge_base_commit']['sha']
            nightly = next(
                (
                    r['html_url']
                    for r in self.nightly_releases()
                    if r['tag_name'].startswith('nightly-') and r['tag_name'].endswith(f'-{base[:7]}')
                ),
                '',
            )
            said = (
                f'**{"Draft" if pr.get("draft") else "Open"}** at [`{head[:7]}`]({commit}), on main at '
                f'[`{base[:7]}`]({url}/commit/{base})' + (f' ([its nightly]({nightly}))' if nightly else '') + '.'
            )
            run = run or self.build_of(head)
            jobs = self.jobs_of(run) if jobs is None else jobs
            release = next((r for r in self.nightly_releases() if r['tag_name'] == f'pr-{number}'), None)
            current = release if release and f'/commits/{head}' in (release.get('body') or '') else None
            fork = (pr['head'].get('repo') or {}).get('full_name') != repo
            labeled = LABEL in {label['name'] for label in pr.get('labels', [])}
            built = "waits for the team's label" if fork and not labeled else 'being published'
            fields = self.packages(jobs, current, built)
            if not run:
                state = 'Not started'
            elif run['status'] != 'completed':
                state = f'[Running]({run["html_url"]})'
            else:
                state = f'[{"Passed" if run["conclusion"] == "success" else run["conclusion"].capitalize()}]({run["html_url"]})'
            fields.append({'name': 'Build', 'value': state, 'inline': True})
            if current:
                files = f'[All files]({current["html_url"]})'
            elif release:
                files = f'[The push before]({release["html_url"]})'
            else:
                files = 'None yet'
            fields.append({'name': 'Builds', 'value': files, 'inline': True})
            color = DRAFT if pr.get('draft') else OPEN
        lines = [said]
        about = self.meta(pr, links=self.linked(number, True), word='closes')
        if about:
            lines.append(about)
        if subjects:
            lines += ['', f'**{plural(len(subjects), "commit")}**, the newest first']
            lines += self.listed(subjects, f'{pr["html_url"]}/commits', MOST_COMMITS)
        lines += self.comment_lines(comment)
        footer = 'Edited at each push, comment and build'
        return self.hub.embed(title, pr['html_url'], '\n'.join(lines), fields, color, footer, author)

    def issue(self, number, create=True):
        return self.show(f'issue-{number}', lambda: self.issue_embed(number), create)

    def issue_embed(self, number):
        issue = self.hub.read(f'repos/{self.hub.repo}/issues/{number}')
        if issue['state'] == 'open':
            said, color = '**Open**', OPEN
        elif issue.get('state_reason') == 'completed':
            said, color = '**Closed** as done', DONE
        else:
            said, color = f'**Closed** as {(issue.get("state_reason") or "closed").replace("_", " ")}', NOT_PLANNED
        about = self.meta(issue, (issue.get('type') or {}).get('name', ''), self.linked(number, False), 'fixed by')
        lines = [said, *([about] if about else []), '', excerpt(issue.get('body')) or '*No description.*']
        lines += self.comment_lines(self.latest_comment(number, issue.get('comments', 0)))
        author = {'name': issue['user']['login'], 'url': issue['user']['html_url']}
        if issue['user'].get('avatar_url'):
            author['icon_url'] = issue['user']['avatar_url']
        footer = f'{plural(issue.get("comments", 0), "comment")}, edited at each change and comment'
        title = f'#{number}: {issue["title"]}'
        return self.hub.embed(title, issue['html_url'], '\n'.join(lines), (), color, footer, author)

    def nightly(self, sha, create=True, run=None, jobs=None):
        return self.show(f'nightly-{sha}', lambda: self.nightly_embed(sha, run, jobs), create)

    def nightly_embed(self, sha, run=None, jobs=None):
        url, commit = self.url, f'{self.url}/commit/{sha}'
        date = notes.git(self.checkout, 'log', '-1', '--format=%cs', sha).strip()
        nightlies = [r for r in self.nightly_releases() if r['tag_name'].startswith('nightly-')]
        own = next((r for r in nightlies if r['tag_name'].endswith(f'-{sha[:7]}')), None)
        older = nightlies[nightlies.index(own) + 1 :] if own else nightlies
        built_from = [m.group(0) for r in older for m in [re.search(r'[0-9a-f]{40}', r.get('body') or '')] if m]
        previous = next((c for c in built_from if c != sha), '')
        run = run or self.build_of(sha)
        jobs = self.jobs_of(run) if jobs is None else jobs
        link, color, built = commit, BUILDING, ''
        if own:
            said, link, color = (
                f'Built from main at [`{sha[:7]}`]({commit}), committed {date}.',
                own['html_url'],
                SCARLET,
            )
        elif not run or run['status'] != 'completed':
            said = f'Building main at [`{sha[:7]}`]({commit}), committed {date}.'
        elif run['conclusion'] != 'success':
            said, color = (
                f'Not published: the build of [`{sha[:7]}`]({commit}) failed ([run]({run["html_url"]})).',
                AMBER,
            )
            built = 'not published'
        elif previous and self.ancestor(sha, previous):
            newer = older[0]
            said, color = (
                f'Not published: [{newer["name"]}]({newer["html_url"]}), of a newer commit, came first.',
                CLOSED,
            )
            built = 'not published'
        else:
            said, built = (
                f'Built from main at [`{sha[:7]}`]({commit}), committed {date}: being published.',
                'being published',
            )
        lines = [said]
        items = notes.changes(self.checkout, previous, sha)
        if items:
            compare = f'{url}/compare/{previous}...{sha}'
            lines += ['', f'**Changes** ([`{previous[:7]}...{sha[:7]}`]({compare}))']
            lines += self.listed([notes.change_line(item, url) for item in items], compare, MOST_LINES)
        fields = self.packages(jobs, own, built)
        fields += [
            {'name': 'Source', 'value': f'[`{sha[:7]}`]({commit})', 'inline': True},
            {'name': 'Every nightly', 'value': f'[{self.hub.to}]({SERVER}/{self.hub.to}/releases)', 'inline': True},
        ]
        color = AMBER if self.failed(jobs) and not own else color
        title = notes.title(self.checkout, sha)
        return self.hub.embed(
            title, link, '\n'.join(lines), fields, color, f'The newest {self.keep} nightlies are kept'
        )

    def ancestor(self, sha, newer):
        command = ['git', '-C', self.checkout, 'merge-base', '--is-ancestor', sha, newer]
        return sha != newer and subprocess.run(command, capture_output=True, check=False).returncode == 0

    # ---- following a build
    def target(self, run):
        """the card a Build run's packages go on: ('pr', N), ('nightly', SHA), or None"""
        if (
            run['event'] == 'push'
            and run['head_branch'] == 'main'
            and run['head_repository']['full_name'] == self.hub.repo
        ):
            return 'nightly', run['head_sha']
        if run['event'] == 'pull_request':
            if run.get('pull_requests'):
                return 'pr', run['pull_requests'][0]['number']
            pulls = self.hub.pages(f'repos/{self.hub.repo}/pulls?state=open')  # a fork's run names none
            found = next((p['number'] for p in pulls if p['head']['sha'] == run['head_sha']), None)
            return ('pr', found) if found else None
        return None

    def watch(self, run_id, every=20, limit=3600):
        """the run's card edited each time one of its jobs changes, until the run ends or limit seconds pass"""
        start, seen, aim = time.monotonic(), None, None
        while True:
            run = self.hub.read(f'repos/{self.hub.repo}/actions/runs/{run_id}')
            aim = aim or self.target(run)
            if not aim:
                print(f'run {run_id} ({run["event"]} of {run["head_branch"]}) has no card')
                return
            jobs = self.jobs_of(run)
            now = json.dumps(sorted((j['name'], j['status'], j.get('conclusion')) for j in jobs) + [run['status']])
            if now != seen:
                self._releases = None
                kind, ident = aim
                getattr(self, kind)(ident, create=False, run=run, jobs=jobs)
                seen = now
            if run['status'] == 'completed' or time.monotonic() - start > limit:
                return
            time.sleep(every)

    # ---- what an event shows
    def on_event(self, name, event, every):
        if name == 'push':
            if event.get('ref') == f'refs/heads/{event["repository"]["default_branch"]}' and not event.get('deleted'):
                self.nightly(event['after'])
            else:
                print(f'a push to {event.get("ref")}: no card')
        elif name in ('pull_request_target', 'pull_request', 'pull_request_review', 'pull_request_review_comment'):
            self.pr(event['pull_request']['number'])
        elif name == 'issues':
            self.issue(event['issue']['number'])
        elif name == 'issue_comment':
            if event['issue'].get('pull_request'):
                self.pr(event['issue']['number'])
            else:
                self.issue(event['issue']['number'])
        elif name == 'workflow_run':
            self.watch(event['workflow_run']['id'], every)
        else:
            print(f'{name}: no card')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('step', choices=['event', 'refresh'])
    parser.add_argument('kind', nargs='?', choices=['pr', 'issue', 'nightly'])
    parser.add_argument('ident', nargs='?', help="the pull request's or the issue's number, or the nightly's commit")
    parser.add_argument('--repo', required=True)
    parser.add_argument('--to', required=True, help='the nightly repository, which keeps the cards')
    parser.add_argument('--checkout', default='.', help="REPO's history, for a nightly's changes")
    parser.add_argument('--keep', type=int, default=30, help='the nightlies kept')
    parser.add_argument(
        '--every', type=float, default=20, help="seconds between two looks at a build or another run's card"
    )
    parser.add_argument('--dry-run', action='store_true', help='read, and print each write instead')
    args = parser.parse_args()
    if args.step == 'refresh' and not (args.kind and args.ident):
        parser.error('refresh takes pr N, issue N or nightly SHA')
    cards = Cards(Hub(args.repo, args.to, args.dry_run), args.checkout, args.keep, args.every)
    if args.step == 'refresh':
        getattr(cards, args.kind)(args.ident if args.kind == 'nightly' else int(args.ident))
        return
    with open(os.environ['GITHUB_EVENT_PATH'], encoding='utf-8') as file:
        cards.on_event(os.environ['GITHUB_EVENT_NAME'], json.load(file), args.every)


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'discord-cards: {error}')
