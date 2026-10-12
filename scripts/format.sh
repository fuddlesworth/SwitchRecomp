#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# Formats the C and C++ sources with clang-format 23 and the Python sources with ruff (through uvx, pinned below).
#
#   scripts/format.sh            format the repository
#   scripts/format.sh --check    list what is not formatted, exit 1 if anything is
#   scripts/format.sh DIR        format another tree with this repository's settings (import-upstream.sh does this)
#
# Formatted: the C and C++ files under runtime/ and tools/, and the Python files up to two folders deep plus the Claude
# Code hooks. Left alone: third-party code (.cache/cpm), and in upstream trees the font bitmap in overlay_font.h and
# the GL function list in glfn.h, an X-macro list clang-format can't lay out stably.
set -euo pipefail

CLANG_FORMAT_MAJOR=23
RUFF=ruff@0.17.0

repo=$(cd "$(dirname "$0")/.." && pwd)
check=0
root=$repo
for arg in "$@"; do
    case $arg in
        --check) check=1 ;;
        -*) echo "usage: $0 [--check] [DIR]" >&2; exit 2 ;;
        *) root=$(cd "$arg" && pwd) ;;
    esac
done

if ! command -v clang-format >/dev/null; then
    echo "clang-format $CLANG_FORMAT_MAJOR is required and is not on PATH" >&2
    exit 1
fi
version=$(clang-format --version | sed -n 's/.*version \([0-9]*\)\..*/\1/p')
if [ "$version" != "$CLANG_FORMAT_MAJOR" ]; then
    echo "clang-format $CLANG_FORMAT_MAJOR is required (found ${version:-none}): another version formats differently" >&2
    exit 1
fi

cd "$root"
# An upstream tree has no tools/ or .claude/hooks/
c_dirs=()
for d in runtime tools; do [ -d "$d" ] && c_dirs+=("$d"); done
py_extra=()
[ -d .claude/hooks ] && py_extra+=(.claude/hooks)
c_files=()
if [ ${#c_dirs[@]} -gt 0 ]; then
    mapfile -t c_files < <(find "${c_dirs[@]}" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
        -not -name 'overlay_font.h' -not -name 'glfn.h' | sort)
fi
mapfile -t py_files < <( (find . -maxdepth 2 -name '*.py' -not -path './third_party/*' -not -path './.cache/*' \
    -not -path './build*' -not -path './python/*'; [ ${#py_extra[@]} -eq 0 ] || find "${py_extra[@]}" -name '*.py') | sort)

# cksum is POSIX (sha1sum is missing on FreeBSD); it only has to tell whether a pass changed anything
digest() { cat "${c_files[@]}" | cksum; }

status=0
if [ $check = 1 ]; then
    for f in "${c_files[@]}"; do
        clang-format --style="file:$repo/.clang-format" --dry-run -Werror "$f" 2>/dev/null || { echo "$f"; status=1; }
    done
    if [ ${#py_files[@]} -gt 0 ]; then
        uvx -q "$RUFF" format --no-cache --config "$repo/pyproject.toml" --check "${py_files[@]}" || status=1
    fi
    [ $status = 0 ] || echo "Not formatted: run scripts/format.sh" >&2
else
    # clang-format is not idempotent here (trailing comments after a closing brace realign on a second pass), so
    # repeat until nothing changes: the result is then stable, and --check agrees with it. With no files, cat and
    # clang-format would read stdin and ruff would format the whole folder: skip them
    if [ ${#c_files[@]} -gt 0 ]; then
        for pass in 1 2 3 4 5; do
            before=$(digest)
            clang-format --style="file:$repo/.clang-format" -i "${c_files[@]}"
            [ "$(digest)" = "$before" ] && break
            if [ $pass = 5 ]; then
                echo "clang-format did not settle after 5 passes" >&2
                exit 1
            fi
        done
    fi
    if [ ${#py_files[@]} -gt 0 ]; then
        uvx -q "$RUFF" format --no-cache --config "$repo/pyproject.toml" -q "${py_files[@]}"
    fi
fi
exit $status
