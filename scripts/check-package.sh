#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Runs a packaged ScarletNX as a user would, in an empty folder of a machine without the checkout it was built from:
# the AppImage, or bin/scarletnx(.exe) of `cmake --install` (the Windows zip). CI runs it in jobs of their own
# (.github/workflows/build.yml): in the build jobs the checkout is where the package looks for it, and hides what an
# installed copy lacks.
#
#   scripts/check-package.sh dist/ScarletNX-x86_64.AppImage
#   scripts/check-package.sh stage/bin/scarletnx.exe
set -euo pipefail

if [ $# -ne 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 PACKAGED_SCARLETNX" >&2
    exit 2
fi
runtime=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
exe=
case $runtime in
    *.exe) exe=.exe ;;
    # CI runners have no FUSE to mount it
    *.AppImage) export APPIMAGE_EXTRACT_AND_RUN=1 ;;
esac
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
# An installed copy keeps games/ and prod.keys in the user's data folder, wherever it was started from, and on Linux a
# game's settings, log and caches (work/ too) in the XDG config, state and cache folders: point them into this run's
export XDG_DATA_HOME="$work/data" XDG_CONFIG_HOME="$work/config" XDG_STATE_HOME="$work/state" XDG_CACHE_HOME="$work/cache"
data=$XDG_DATA_HOME/scarletnx
config=$XDG_CONFIG_HOME/scarletnx/games
state=$XDG_STATE_HOME/scarletnx/games
if [ -n "$exe" ]; then
    APPDATA=$(cygpath -w "$work/data")
    export APPDATA
    config=$data/games
    state=$data/games
fi

failed=0
pass() { echo "ok: $1"; }
fail() {
    echo "FAILED: $1" >&2
    shift
    printf '%s\n' "$@" | sed 's/^/    /' >&2
    failed=1
}
# the command's output, whatever its exit status; the status in $status
run() {
    status=0
    out=$(timeout 300 "$@" 2>&1) || status=$?
}

# tests/nx_runtime_smoke.py's homebrew: an NRO whose code prints a line through svcOutputDebugString and exits
put() { printf '%b' "$3" | dd of="$1" bs=1 seek=$(($2)) conv=notrunc status=none; }
word() { put "$1" "$2" "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(($3 & 255)) $(($3 >> 8 & 255)) $(($3 >> 16 & 255)) $(($3 >> 24)))"; }
head -c 512 /dev/zero >smoke.nro
word smoke.nro 0x00 0x14000020 # B +0x80, over the header
put smoke.nro 0x10 NRO0
word smoke.nro 0x18 0x200
for field in 0x20:0 0x24:0x100 0x28:0x100 0x2c:0x80 0x30:0x180 0x34:0x80; do # text, rodata, data
    word smoke.nro "${field%:*}" "${field#*:}"
done
word smoke.nro 0x80 0x10000400 # ADR X0, the line in rodata
word smoke.nro 0x84 0xd2800221 # MOV X1, its 17 bytes
word smoke.nro 0x88 0xd40004e1 # SVC 0x27, OutputDebugString
word smoke.nro 0x8c 0xd40000e1 # SVC 7, ExitProcess
word smoke.nro 0x90 0xd4200000 # BRK
put smoke.nro 0x100 'NX runtime smoke\n'

for mode in "" -x -r; do
    run "$runtime" ${mode:+"$mode"} --help
    if [ "$status" -eq 0 ] && grep -q '^Usage: scarletnx' <<<"$out"; then
        pass "scarletnx${mode:+ $mode} --help"
    else
        fail "scarletnx${mode:+ $mode} --help exited $status" "$out"
    fi
done

# No --outdir: an installed copy has no checkout, so its games go in the data folder
run "$runtime" -r smoke.nro Smoke --jobs 2
if [ "$status" -eq 0 ] && [ -f "$data/games/Smoke$exe" ] && [ -f "$data/games/Smoke.abi" ]; then
    pass "scarletnx -r smoke.nro made $data/games/Smoke$exe"
else
    fail "scarletnx -r smoke.nro exited $status, without $data/games/Smoke$exe and its .abi" "$out"
fi
if [ -f "$config/Smoke.ini" ] && { [ "$config" = "$data/games" ] || [ ! -e "$data/games/Smoke.ini" ]; }; then
    pass "its settings are $config/Smoke.ini"
else
    fail "scarletnx -r smoke.nro did not write its settings in $config only" "$out"
fi

# The runtime left next to the game must start on its own (on Windows, with the DLLs it needs), find the game's code
# by its name and take its ABI. The banner comes after that and before the GPU, which CI runners lack
if [ -f "$data/games/Smoke$exe" ]; then
    run env -C "$data/games" "./Smoke$exe" -H
    if grep -q '^ScarletNX - Smoke' <<<"$out"; then
        pass "$data/games/Smoke$exe started the game (exit $status: $(tail -n 1 <<<"$out"))"
    else
        fail "$data/games/Smoke$exe exited $status before the game's banner" "$out"
    fi
    if grep -q 'ScarletNX - Smoke' "$state/Smoke.log" 2>/dev/null &&
        { [ "$state" = "$data/games" ] || [ ! -e "$data/games/Smoke.log" ]; }; then
        pass "its log is $state/Smoke.log"
    else
        fail "the game did not log in $state only" "$out"
    fi
fi

# A dump on its own: recompile, then play, each a child of the first. Another name, which Windows would not tell
# from Smoke by case
cp smoke.nro play.nro
run "$runtime" play.nro -H
if grep -q '^ScarletNX - play' <<<"$out"; then
    pass "scarletnx play.nro recompiled and started the game (exit $status)"
else
    fail "scarletnx play.nro exited $status before the game's banner" "$out"
fi

# A PFS0 without files: only hactool, unpacking it, can tell there is no program in it
printf 'PFS0\0\0\0\0\0\0\0\0\0\0\0\0' >empty.nsp
run "$runtime" -x empty.nsp
if grep -q 'no base Program NCA' <<<"$out"; then
    pass "scarletnx -x empty.nsp ran hactool"
else
    fail "scarletnx -x empty.nsp exited $status without hactool unpacking it" "$out"
fi

if [ "$failed" -ne 0 ]; then
    echo "$runtime does not work as installed" >&2
    exit 1
fi
echo "$runtime works as installed"
