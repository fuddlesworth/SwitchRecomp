#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
"""PreToolUse guard for the hard rules of this project (CLAUDE.md).

Claude Code passes the tool call as JSON on stdin. Exit code 2 blocks the call and shows stderr to Claude.

A Bash command is read the way the shell reads it: split into simple commands at ; & | newlines, parentheses and
backticks outside quotes, each one tokenized with its quotes removed, and the wrappers in front of a command (then,
do, !, env, xargs, timeout, nohup, sudo...) skipped. So `git add "games"` or `then pkill x` doesn't get past a rule,
and a rule's word inside a quoted string (grep 'a|sudo') doesn't trip it. Here-document bodies (commit messages) are
text, not commands.
"""

import json
import os
import re
import shlex
import sys

# Files that belong to the game's owner or are made from them: never staged, uploaded or edited
GAME_EXT = re.compile(r'\.(keys|xci|nsp|nsz|xcz|nca|nro|nso|npdm|tik|cert|swrs|spv|ppm)$', re.I)
GAME_DIR = re.compile(r'(^|/)(games|work|deps|keys|dumps|exefs|romfs)(/|$)')

# Generated or third-party trees: change the source that produces them instead. Not anchored at the start, so a
# worktree's games/ (.claude/worktrees/NAME/games/) is covered too
PROTECTED_PATHS = re.compile(r'(^|/)(games|work|deps|\.cache|build[^/]*|third_party)/|\.keys$')

SEPARATORS = ';&|()\n`'
# Words that run the command after them: skipped, with their options, to find the command itself
WRAPPERS = set(
    'then do else elif if while until ! { } time nohup env exec command builtin xargs timeout nice ionice stdbuf setsid '
    'unbuffer sudo doas'.split()
)
UPLOADERS = {'curl', 'wget', 'scp', 'rsync', 'rclone', 'sftp', 'gh'}

MSG_GAME = (
    'Keys, dumps, work/, games/ and deps/ never go into git, and `git add -f` would bypass .gitignore. '
    'Stage only source files, by name.'
)
MSG_BROAD = 'Stage files by name (git add path...), not with -A, -u, ., *, :/ or commit -a.'
MSG_PUSH = 'Never force-push. Fix the branch with a new commit instead.'
MSG_PKILL = 'pkill/killall match the shell running them. Use `pgrep -a NAME`, then `kill PID`.'
MSG_PGREP_KILL = (
    'kill with pgrep -f matches the shell running the command, whose command line holds the pattern. '
    'List with `pgrep -a NAME`, then `kill PID` with the numbers in a separate command.'
)
MSG_SUDO = 'No sudo: the user installs system packages. Say what is needed and why.'
MSG_UPLOAD = 'Keys, dumps, work/ and games/ (and shader or texture dumps) never leave this machine.'


def is_game_path(arg):
    return bool(GAME_EXT.search(arg) or GAME_DIR.search(arg))


def strip_heredocs(command):
    """The command with the bodies of its here-documents left out."""
    out, pos = [], 0
    for m in re.finditer(r'<<-?[ \t]*([\'"]?)([A-Za-z_][A-Za-z0-9_]*)\1', command):
        if m.start() < pos:
            continue
        body = command.find('\n', m.end())
        if body < 0:
            break
        end = re.compile(r'^[ \t]*%s[ \t]*$' % re.escape(m.group(2)), re.M).search(command, body + 1)
        if not end:
            break
        out.append(command[pos : body + 1])
        pos = end.end()
    out.append(command[pos:])
    return ''.join(out)


def tokens(command):
    lex = shlex.shlex(command, posix=True, punctuation_chars=SEPARATORS)
    lex.whitespace = ' \t\r'
    lex.whitespace_split = True
    try:
        return list(lex)
    except ValueError:  # unbalanced quotes: split at whitespace and separators, quotes dropped
        return re.findall(r'[%s]+|[^\s%s]+' % (re.escape(SEPARATORS), re.escape(SEPARATORS)), command.replace('"', ''))


def simple_commands(command):
    """(wrappers, command word, arguments) for every simple command in a command line, including the ones in a
    command substitution inside double quotes."""
    command = strip_heredocs(command)
    lines = [command] + re.findall(r'\$\(([^()]*)\)', command) + re.findall(r'`([^`]*)`', command)
    for line in lines:
        words = []
        for tok in tokens(line) + [';']:
            if tok and all(ch in SEPARATORS for ch in tok):
                if words:
                    yield split_wrappers(words)
                words = []
            else:
                words.append(tok)


def split_wrappers(words):
    wrappers, i = [], 0
    while i < len(words):
        w = words[i]
        name = os.path.basename(w)
        if re.match(r'^[A-Za-z_][A-Za-z0-9_]*=', w):  # VAR=value before a command
            i += 1
        elif name in WRAPPERS:
            wrappers.append(name)
            i += 1
            # the wrapper's options, and timeout's duration
            while i < len(words) and (words[i].startswith('-') or re.match(r'^[\d.]+[smhd]?$', words[i])):
                i += 1
        else:
            return wrappers, name, words[i + 1 :]
    return wrappers, '', []


def short_flags(arg):
    return arg[1:] if re.match(r'^-[A-Za-z]+$', arg) else ''


def git_subcommand(args):
    """The git subcommand and its arguments, after git's own options (-C dir, -c key=value...)."""
    i = 0
    while i < len(args) and args[i].startswith('-'):
        i += 1 + (args[i] in ('-C', '-c', '--git-dir', '--work-tree', '--namespace', '--config-env'))
    return (args[i], args[i + 1 :]) if i < len(args) else ('', [])


def check_git(args):
    sub, rest = git_subcommand(args)
    if sub in ('add', 'stage'):
        paths, broad, force = [], False, False
        for k, a in enumerate(rest):
            if a == '--':
                paths += rest[k + 1 :]
                break
            if a.startswith('-'):
                force |= a == '--force' or 'f' in short_flags(a)
                broad |= a in ('--all', '--update', '--no-ignore-removal') or a.startswith('--pathspec-from-file')
                broad |= 'A' in short_flags(a) or 'u' in short_flags(a)
            else:
                paths.append(a)
        if force or any(is_game_path(p) for p in paths):
            return MSG_GAME
        if broad or any(p in ('.', './', '..', '*', ':', ':/') or p.startswith((':/', ':(')) for p in paths):
            return MSG_BROAD
    elif sub == 'commit':
        k = 0
        while k < len(rest) and rest[k] != '--':
            a = rest[k]
            k += 1
            if a == '--all':
                return MSG_BROAD
            for n, ch in enumerate(short_flags(a)):
                if ch == 'a':
                    return MSG_BROAD
                if ch in 'mFCct':  # takes a value: the rest of the word, or the next word
                    k += n == len(a) - 2
                    break
    elif sub == 'push':
        for a in rest:
            if a.startswith(('--force', '--mirror')) or 'f' in short_flags(a) or a.startswith('+'):
                return MSG_PUSH
    return None


def option_values(args, names):
    """Values of the options in names, written as `-X value`, `-Xvalue`, `--name value` or `--name=value`."""
    for k, a in enumerate(args):
        for n in names:
            if a == n and k + 1 < len(args):
                yield args[k + 1]
            elif n.startswith('--') and a.startswith(n + '='):
                yield a[len(n) + 1 :]
            elif not n.startswith('--') and a.startswith(n) and len(a) > len(n):
                yield a[len(n) :]


def check_upload(word, args):
    """Whether a command sends a game file somewhere else."""
    if word == 'curl':
        sent = list(option_values(args, ('-T', '--upload-file')))
        for v in option_values(args, ('-F', '--form', '-d', '--data', '--data-binary', '--data-urlencode', '--json')):
            if '@' in v:
                sent.append(v.split('@', 1)[1].split(';')[0])
    elif word == 'wget':
        sent = list(option_values(args, ('--post-file', '--body-file')))
    elif word in ('scp', 'rsync', 'rclone', 'sftp'):
        sent = [a for a in args if not a.startswith('-')][:-1]  # every source; the last one is where they go
    elif word == 'gh' and ' '.join(args[:2]) in ('gist create', 'gist edit', 'release upload', 'release create'):
        # the files: words that are neither options nor an option's value (notes and titles have spaces anyway)
        sent = [a for k, a in enumerate(args[2:], 2) if not a.startswith('-') and not args[k - 1].startswith('-')]
    else:
        return False
    return any(' ' not in s and is_game_path(s) for s in sent)


def check_bash(command):
    cmds = list(simple_commands(command))
    for wrappers, word, args in cmds:
        if word in ('pkill', 'killall'):
            return MSG_PKILL
        if word in ('sudo', 'doas') or 'sudo' in wrappers or 'doas' in wrappers:
            return MSG_SUDO
        if word == 'git':
            msg = check_git(args)
            if msg:
                return msg
        if word in UPLOADERS and check_upload(word, args):
            return MSG_UPLOAD
    # kill $(pgrep -f ...), pgrep -f ... | xargs kill, a loop over pgrep -f
    pgrep_f = any(w == 'pgrep' and any(a == '--full' or 'f' in short_flags(a) for a in args) for _, w, args in cmds)
    if pgrep_f and any(w == 'kill' for _, w, _ in cmds):
        return MSG_PGREP_KILL
    return None


def main():
    call = json.load(sys.stdin)
    tool = call.get('tool_name', '')
    params = call.get('tool_input', {})
    if tool == 'Bash':
        message = check_bash(params.get('command', ''))
        if message:
            print(message, file=sys.stderr)
            sys.exit(2)
    elif tool in ('Edit', 'Write', 'MultiEdit', 'NotebookEdit'):
        path = params.get('file_path') or params.get('notebook_path') or ''
        project = os.environ.get('CLAUDE_PROJECT_DIR', os.getcwd()).rstrip('/') + '/'
        if path.startswith(project) and PROTECTED_PATHS.search(path[len(project) :]):
            print(
                f'{path} is generated, third-party or a game file. Change the source, script or package pin '
                'that produces it instead.',
                file=sys.stderr,
            )
            sys.exit(2)


if __name__ == '__main__':
    main()
