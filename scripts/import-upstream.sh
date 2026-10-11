#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# Imports an MZ Labs SwitchRecomp release zip onto the `upstream` branch, without touching the working tree or the
# current branch: one commit with the release as it is (tagged upstream-VERSION), then one with it formatted by
# scripts/format.sh. Merge `upstream` into `main` afterwards. Since both branches are formatted the same way, the merge
# only shows what the release changed.
#
#   scripts/import-upstream.sh ~/Downloads/SwitchRecomp_v143.zip v143
#
# The bundled Windows toolchains (python, tcc, gcc, w64devkit), prebuilt runtimes (bin) and executables are left out:
# they are third-party binaries, and the zip they came from stays the place to get them.
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 RELEASE.zip VERSION" >&2
    exit 2
fi
zip=$1
version=$2
repo=$(git rev-parse --show-toplevel)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
unzip -q "$zip" -d "$tmp/x"
src="$tmp/x/SwitchRecomp"
if [ ! -d "$src/runtime" ]; then
    echo "$zip has no SwitchRecomp/runtime folder" >&2
    exit 1
fi

# A private index, so the real one and the working tree are left alone. -f because a global excludes file must not
# drop upstream files.
export GIT_INDEX_FILE="$tmp/index"
snapshot() {
    rm -f "$GIT_INDEX_FILE"
    git -C "$repo" --work-tree="$src" add -A -f -- . \
        ':(exclude)python' ':(exclude)tcc' ':(exclude)gcc' ':(exclude)w64devkit' ':(exclude)bin' ':(exclude)*.exe'
    git -C "$repo" write-tree
}

parent=$(git -C "$repo" rev-parse -q --verify refs/heads/upstream || true)
raw_tree=$(snapshot)
if git -C "$repo" rev-parse -q --verify "refs/tags/upstream-$version" >/dev/null; then
    echo "upstream-$version already exists" >&2
    exit 1
fi
raw=$(git -C "$repo" commit-tree "$raw_tree" ${parent:+-p "$parent"} \
    -m "Import SwitchRecomp $version" \
    -m "The MZ Labs release $(basename "$zip"), without the bundled Windows toolchains, prebuilt runtimes and executables.")

"$repo/scripts/format.sh" "$src"
formatted=$(git -C "$repo" commit-tree "$(snapshot)" -p "$raw" \
    -m "Format SwitchRecomp $version" \
    -m "scripts/format.sh, as main is formatted, so merging this release into main only shows what it changed.")

git -C "$repo" update-ref refs/heads/upstream "$formatted" ${parent:+"$parent"}
git -C "$repo" tag "upstream-$version" "$raw"
echo "upstream is now $formatted, the release as imported is upstream-$version"
