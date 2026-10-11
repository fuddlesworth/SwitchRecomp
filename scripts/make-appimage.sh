#!/usr/bin/env bash
# Makes dist/SwitchRecomp-<arch>.AppImage, a runtime that runs on any Linux, with pkgforge's quick-sharun
# (https://github.com/pkgforge-dev/Anylinux-AppImages): it bundles the runtime with every library it loads, SDL3, the
# Vulkan loader and Mesa's Vulkan drivers included. CI runs it in pkgforge's Arch container after `cmake --install`
# into /usr (.github/workflows/build.yml), and quick-sharun has to be on PATH (pkgforge-dev/anylinux-setup-action puts
# it there).
#
# A copy named after a game works like the bare runtime: games/Catherine.AppImage (or games/Catherine) loads
# games/Catherine_code.so, since the runtime takes its name and folder from $APPIMAGE.
set -euo pipefail

ARCH=$(uname -m)
REPOSITORY=${GITHUB_REPOSITORY:-fuddlesworth/SwitchRecomp}
VERSION=${VERSION:-$(git -C "$(dirname "$0")/.." describe --always --dirty 2>/dev/null || echo unknown)}
export ARCH VERSION
export OUTPATH=./dist
export OUTNAME=SwitchRecomp-$ARCH.AppImage
export ICON=/usr/share/icons/hicolor/scalable/apps/switchrecomp.svg
export DESKTOP=/usr/share/applications/switchrecomp.desktop
# AppImageUpdate finds new builds in the rolling release through the .zsync file next to the AppImage
export UPINFO="gh-releases-zsync|${REPOSITORY%/*}|${REPOSITORY#*/}|nightly|*$ARCH.AppImage.zsync"
# The runtime exits at once without a game module, so tracing it for dlopened libraries finds nothing: name them
export STRACE_MODE=0 DEPLOY_SDL=1 DEPLOY_VULKAN=1

quick-sharun /usr/bin/swr-runtime
quick-sharun --make-appimage

# Without a game the runtime says which module it looked for. Under another name, it must look next to the AppImage
# for that name's module: that shows the libraries load and the name and folder come from $APPIMAGE
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
cp "$OUTPATH/$OUTNAME" "$probe/Probe.AppImage"
out=$(cd "$probe" && APPIMAGE_EXTRACT_AND_RUN=1 ./Probe.AppImage 2>&1 || true)
if ! grep -qF "$probe/Probe_code.so" <<<"$out"; then
    echo "The AppImage did not look for $probe/Probe_code.so. It printed:" >&2
    echo "$out" >&2
    exit 1
fi
echo "AppImage checked: it looks for its game module next to itself"
