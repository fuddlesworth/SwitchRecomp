#!/usr/bin/env bash
# Formats the C and C++ sources with clang-format 23 and the Python sources with ruff (through uvx, pinned below).
#
#   scripts/format.sh            format the repository
#   scripts/format.sh --check    list what is not formatted, exit 1 if anything is
#   scripts/format.sh DIR        format another tree with this repository's settings (import-upstream.sh does this)
#
# Left alone: third-party code (runtime/bcn, third_party/), the font bitmap in runtime/overlay_font.h, and the GL
# function list in runtime/glfn.h, an X-macro list clang-format can't lay out stably.
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

version=$(clang-format --version | sed -n 's/.*version \([0-9]*\)\..*/\1/p')
if [ "$version" != "$CLANG_FORMAT_MAJOR" ]; then
    echo "clang-format $CLANG_FORMAT_MAJOR is required (found ${version:-none}): another version formats differently" >&2
    exit 1
fi

cd "$root"
mapfile -t c_files < <(find runtime -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
    -not -path 'runtime/bcn/*' -not -name 'overlay_font.h' -not -name 'glfn.h' | sort)
mapfile -t py_files < <(find . -maxdepth 2 -name '*.py' -not -path './third_party/*' -not -path './build*' \
    -not -path './python/*' | sort)

status=0
if [ $check = 1 ]; then
    for f in "${c_files[@]}"; do
        clang-format --style="file:$repo/.clang-format" --dry-run -Werror "$f" 2>/dev/null || { echo "$f"; status=1; }
    done
    uvx -q "$RUFF" format --no-cache --config "$repo/pyproject.toml" --check "${py_files[@]}" || status=1
    [ $status = 0 ] || echo "Not formatted: run scripts/format.sh" >&2
else
    # clang-format is not idempotent here (trailing comments after a closing brace realign on a second pass), so
    # repeat until nothing changes: the result is then stable, and --check agrees with it
    for pass in 1 2 3 4 5; do
        before=$(cat "${c_files[@]}" | sha1sum)
        clang-format --style="file:$repo/.clang-format" -i "${c_files[@]}"
        [ "$(cat "${c_files[@]}" | sha1sum)" = "$before" ] && break
        if [ $pass = 5 ]; then
            echo "clang-format did not settle after 5 passes" >&2
            exit 1
        fi
    done
    uvx -q "$RUFF" format --no-cache --config "$repo/pyproject.toml" -q "${py_files[@]}"
fi
exit $status
