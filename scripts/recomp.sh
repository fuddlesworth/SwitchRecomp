#!/usr/bin/env bash
# Recompiles an extracted game (scripts/extract.py) for Linux into games/:
#   games/NAME            the runtime, built by CMake (preset gcc unless SWR_PRESET is set)
#   games/NAME_code.so    the game's code translated to C and compiled
#   games/NAME_data/      exefs, romfs and saves
#
#   scripts/recomp.sh work/Catherine Catherine [switchrecomp.py options, e.g. --native or --cc clang]
#
# The first run of a big game takes several minutes. Later runs only recompile what changed.
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 GAME_FOLDER NAME [switchrecomp.py options]" >&2
    exit 2
fi
game=$1
name=$2
shift 2
repo=$(cd "$(dirname "$0")/.." && pwd)
preset=${SWR_PRESET:-gcc}

cmake --preset "$preset" >/dev/null
cmake --build --preset "$preset"
python3 "$repo/switchrecomp.py" "$game" -o "$name" --outdir "$repo/games" --target linux --backend x11 \
    --runtime "$repo/build/$preset/swr-runtime" "$@"
