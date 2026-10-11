#!/usr/bin/env python3
"""PreToolUse guard for the hard rules of this project (CLAUDE.md).

Claude Code passes the tool call as JSON on stdin. Exit code 2 blocks the call and shows stderr to Claude.
"""
import json
import os
import re
import sys

# Files that belong to the game's owner or are generated, never staged or edited
GAME_FILES = r'(\.(keys|xci|nsp|nsz|xcz|nca|tik|cert)\b|(^|[\s/"\'])(games|work|deps)(/|\s|$))'

BASH_RULES = [
    (
        # Keys, dumps and recompiled games must never reach git, even when named explicitly or forced
        lambda c: re.search(r'\bgit\s+add\b[^;&|\n]*' + GAME_FILES, c) is not None
        or re.search(r'\bgit\s+add\b[^;&|\n]*\s(-f|--force)\b', c) is not None,
        'Keys, dumps, work/, games/ and deps/ never go into git, and `git add -f` would bypass .gitignore. '
        'Stage only source files, by name.',
    ),
    (
        # A broad add picks up whatever is lying around; stage by name
        lambda c: re.search(r'\bgit\s+(add\s+(-A|--all|\.(\s|$))|commit([ \t]+-[a-zA-Z]+)*[ \t]+-[a-zA-Z]*a[a-zA-Z]*\b)', c)
        is not None,
        'Stage files by name (git add path...), not with -A, . or commit -a.',
    ),
    (
        lambda c: re.search(r'\bgit\s+push\b[^;&|\n]*(\s--force\b|\s-f\b|\s--force-with-lease\b|\s\+\S)', c) is not None,
        'Never force-push. Fix the branch with a new commit instead.',
    ),
    (
        # The pattern also matches the shell running pkill, which then kills itself
        lambda c: re.search(r'(^|[;&|(]|\bsudo)\s*(pkill|killall)\b', c, re.M) is not None,
        'pkill/killall match the shell running them. Use `pgrep -a NAME`, then `kill PID`.',
    ),
    (
        lambda c: re.search(r'(^|[;&|(])\s*sudo\b', c, re.M) is not None,
        'No sudo: the user installs system packages. Say what is needed and why.',
    ),
]

# Generated or third-party trees: change the source that produces them instead
PROTECTED_PATHS = re.compile(r'^(games|work|deps|\.cache|build[^/]*|third_party)/|\.keys$')


def main():
    call = json.load(sys.stdin)
    tool = call.get('tool_name', '')
    params = call.get('tool_input', {})
    if tool == 'Bash':
        command = params.get('command', '')
        for broken, message in BASH_RULES:
            if broken(command):
                print(message, file=sys.stderr)
                sys.exit(2)
    elif tool in ('Edit', 'Write', 'MultiEdit', 'NotebookEdit'):
        path = params.get('file_path') or params.get('notebook_path') or ''
        project = os.environ.get('CLAUDE_PROJECT_DIR', os.getcwd()).rstrip('/') + '/'
        if path.startswith(project) and PROTECTED_PATHS.search(path[len(project):]):
            print(f'{path} is generated, third-party or a game file. Change the source, script or package pin '
                  'that produces it instead.', file=sys.stderr)
            sys.exit(2)


if __name__ == '__main__':
    main()
