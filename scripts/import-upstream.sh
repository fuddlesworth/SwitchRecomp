#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Imports an MZ Labs SwitchRecomp release zip onto the `upstream` branch, without touching the working tree or the
# current branch: one commit with the release as it is (tagged upstream-VERSION), then one with it formatted by
# scripts/format.sh. Releases are ported to main by hand (import-upstream skill). Since every release is formatted the
# same way, the diff between two of them (upstream~2..upstream) shows only what the new release changed.
#
#   scripts/import-upstream.sh ~/Downloads/SwitchRecomp_v143.zip v143
#
# Left out: the bundled Windows toolchains (python, tcc, gcc, w64devkit), prebuilt runtimes (bin), executables and
# libraries, which are third-party binaries the zip stays the place to get, and anything of a game (keys, dumps).
set -euo pipefail
# set -e also inside $(...), so a failing `git add` in snapshot() stops the import instead of committing part of a tree
shopt -s inherit_errexit

if [ $# -ne 2 ]; then
    echo "usage: $0 RELEASE.zip VERSION" >&2
    exit 2
fi
zip=$1
version=$2
repo=$(git rev-parse --show-toplevel)

if git -C "$repo" rev-parse -q --verify "refs/tags/upstream-$version" >/dev/null; then
    echo "upstream-$version already exists" >&2
    exit 1
fi
# Moving a branch that a worktree has checked out would leave that worktree's files behind its HEAD
if git -C "$repo" worktree list --porcelain | grep -qx 'branch refs/heads/upstream'; then
    echo "The upstream branch is checked out in a worktree (git worktree list): switch that worktree away first" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
unzip -q "$zip" -d "$tmp/x"
src="$tmp/x/SwitchRecomp"
if [ ! -d "$src/runtime" ]; then
    echo "$zip has no ScarletNX/runtime folder" >&2
    exit 1
fi

# A private index, so the real one and the working tree are left alone. -f because a global excludes file must not
# drop upstream files, which is why the binaries and game files are excluded here by name
export GIT_INDEX_FILE="$tmp/index"
snapshot() {
    rm -f "$GIT_INDEX_FILE"
    git -C "$repo" --work-tree="$src" add -A -f -- . \
        ':(exclude)python' ':(exclude)tcc' ':(exclude)gcc' ':(exclude)w64devkit' ':(exclude)bin' ':(exclude)*.exe' \
        ':(exclude)*.a' ':(exclude)*.lib' ':(exclude)*.dll' ':(exclude)*.so' \
        ':(exclude)*.keys' ':(exclude)*.xci' ':(exclude)*.nsp' ':(exclude)*.nsz' ':(exclude)*.xcz' ':(exclude)*.nca' \
        ':(exclude)*.nro' ':(exclude)*.nso' ':(exclude)*.tik' ':(exclude)*.cert'
    local tree
    tree=$(git -C "$repo" write-tree)
    if [ -z "$(git -C "$repo" ls-tree "$tree" runtime)" ]; then
        echo "The imported tree has no runtime folder" >&2
        exit 1
    fi
    echo "$tree"
}

parent=$(git -C "$repo" rev-parse -q --verify refs/heads/upstream || true)
raw_tree=$(snapshot)
raw=$(git -C "$repo" commit-tree "$raw_tree" ${parent:+-p "$parent"} \
    -m "Import SwitchRecomp $version" \
    -m "The MZ Labs release $(basename "$zip"), without the bundled Windows toolchains, prebuilt runtimes, executables and libraries.")

"$repo/scripts/format.sh" "$src"
formatted_tree=$(snapshot)
formatted=$(git -C "$repo" commit-tree "$formatted_tree" -p "$raw" \
    -m "Format SwitchRecomp $version" \
    -m "scripts/format.sh, as main is formatted, so the diff from the previous release shows only what this one changed.")

git -C "$repo" update-ref refs/heads/upstream "$formatted" ${parent:+"$parent"}
git -C "$repo" tag "upstream-$version" "$raw"
echo "upstream is now $formatted, the release as imported is upstream-$version"
