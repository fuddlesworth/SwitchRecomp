<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Build

Run from checkout root. [Extract/AOT/run](USAGE.md) · [Tests/diagnostics](DEVELOPMENT.md).

## Prerequisites

- **Linux / FreeBSD:** Clang (CI: 23; FreeBSD's system Clang also works), LLVM's development files
  (`llvm-config`, required by the recompiler), CMake ≥3.25, Ninja, POSIX shell, GNU make (`gmake` on FreeBSD),
  NASM on x86-64, Perl, Git, pkg-config and `patch`.
  Use a system SDL3 development package, or X11/Wayland and audio development libraries for the bundled
  SDL3 build.
- **Windows:** Win10/11 x64 + Git. Bootstrap stages SHA-256-checked llvm-mingw/Clang, CMake, Ninja,
  NASM, Strawberry Perl and w64devkit in `deps/tools`. llvm-mingw has no LLVM libraries. Install LLVM development files
  compatible with MinGW and set `-DSNX_LLVM_CONFIG=/path/to/llvm-config`. CMake fails without them.
- **Display:** a Vulkan 1.3 driver. Headless runs use Vulkan offscreen and skip SDL video initialization.
  SDL3 remains a required build dependency.
- **FFmpeg:** required by every NX runtime build. CMake builds the pinned source release.
  Avoid spaces in checkout/cache paths.
- **OpenSSL:** required by every build with the NX or 3DS loaders (hashes, ciphers, the ssl service's TLS). CMake
  builds the pinned source release with OpenSSL's Configure, which needs **Perl**.

## Linux / FreeBSD

Install dependencies before configuring. The default preset uses **Ninja** and **Clang/Clang++**.
FFmpeg also uses **GNU make** and, on x86-64, **NASM**, and OpenSSL uses **Perl** and GNU make. These tools are not
downloaded by CMake.
The build patches its copy of hactool with `patch`, which `build-essential`, `base-devel` and FreeBSD's base
system include; Fedora lists it below.

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install build-essential git cmake ninja-build clang llvm-dev nasm pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev
```

This includes the X11 and audio development libraries for the SDL3 source fallback. Without the audio
ones, SDL3 builds with no sound output and games run silent. If your release provides `libsdl3-dev`,
install it to use the distribution's SDL3 instead:

```sh
sudo apt install libsdl3-dev
```

If apt cannot find that package, skip it. ScarletNX builds the pinned SDL3 automatically.
Installing `libsdl2-dev` does not satisfy the SDL3 dependency.

For native Wayland support in the source fallback, also install:

```sh
sudo apt install libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev libegl-dev
```

Without `libegl-dev` SDL3 quietly leaves Wayland out and builds X11 only. The `SDL_WAYLAND` line of
its configure summary reads `(Wanted: ON): ON` when Wayland is in.

CMake needs version 3.25 or newer, and Clang must support C23/C++23. Upgrade these tools if your
release ships older versions. Install your GPU's Vulkan driver separately. For Intel/AMD or lavapipe
on Debian/Ubuntu, the usual packages are `libvulkan1 mesa-vulkan-drivers`. NVIDIA users need a
Vulkan-capable NVIDIA driver.

### Fedora

```sh
sudo dnf install clang cmake ninja-build nasm make perl git pkgconf-pkg-config patch llvm-devel \
  libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel \
  libXScrnSaver-devel libXtst-devel alsa-lib-devel pulseaudio-libs-devel pipewire-devel
```

These are the Debian/Ubuntu packages above under Fedora's names. `llvm-devel` (`llvm-dev` on
Debian/Ubuntu) is the recompiler's code generator, which the configure requires. Fedora 44
provides SDL3 3.4; install `SDL3-devel` to use it instead of the source fallback:

```sh
sudo dnf install SDL3-devel
```

For native Wayland support in the source fallback, also install:

```sh
sudo dnf install wayland-devel libxkbcommon-devel wayland-protocols-devel libdecor-devel libglvnd-devel
```

`libglvnd-devel` holds the EGL headers. For Intel/AMD or lavapipe, the usual Vulkan packages are
`vulkan-loader mesa-vulkan-drivers`.

### Arch Linux

```sh
sudo pacman -Syu --needed base-devel git cmake ninja clang llvm nasm perl pkgconf sdl3
```

### FreeBSD

FreeBSD provides Clang in the base system. Install the remaining tools and the SDL3 package:

```sh
sudo pkg install git cmake ninja gmake nasm perl5 pkgconf sdl3 llvm
```

### Configure and build

```sh
git clone https://github.com/scarletnx/ScarletNX.git && cd ScarletNX &&
cmake --preset default -B build && cmake --build build
# -> build/{scarletnx,hactool,snx-shaderc}
```

### SDL3 troubleshooting

On all platforms, CPM first looks for a compatible SDL3 development package (3.4.18 or newer) through
`SDL3Config.cmake`. If none is found, CPM downloads and builds the SHA-256-pinned SDL3. The fallback is static, so no separate SDL3 runtime
installation is needed. It still needs your platform's window-system development libraries.

- An old checkout showing `Could not find a package configuration file provided by SDL3` needs either
  the SDL3 **development** package or this updated fallback. Pull the latest checkout and configure again.
- For SDL3 installed in a custom prefix, pass `-DCMAKE_PREFIX_PATH=/path/to/prefix`, or
  `-DSDL3_DIR=/path/to/prefix/lib/cmake/SDL3` (the directory containing `SDL3Config.cmake`).
- To force the source build, pass `-DCPM_DOWNLOAD_SDL3=ON`. This is CPM 0.43.2's per-package override.
  That version does not support `CPM_FORCE_BUNDLED_SDL3`. No bundled override is forced by ScarletNX.
  The source build is recommended on Windows to match the MinGW toolchain and keep SDL3 statically linked.
- If SDL3's configure reports missing X11/Wayland development libraries, install the packages above.
  Do not work around that by disabling both display backends for a windowed build.
- No sound, and the log says `audio: SDL could not start its audio: No available audio device`: the
  SDL3 fallback was configured without audio development libraries (its configure warns `Unable to find
  the alsa development library`). Install the audio packages above and configure again.
- SDL3 is mandatory for the NX runtime, including headless runs. Use `-H` at runtime to skip
  SDL video initialization and run without a display server. The separate headless backend was removed.
  For an old build directory, remove the cached backend selection with `-USNX_BACKEND`.

After installing dependencies, rerun:

```sh
cmake --preset default -B build && cmake --build build
```

If that directory was configured with a different compiler or generator, use a new build directory
(for example `-B build/clang`) rather than mixing the old cache with the new toolchain.

## Windows / PowerShell

```powershell
& {
    $ErrorActionPreference = 'Stop'
    git clone https://github.com/scarletnx/ScarletNX.git
    if ($LASTEXITCODE) { throw 'Clone failed' }
    Set-Location ScarletNX
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
    if ($LASTEXITCODE) { throw 'Bootstrap failed' }
    Set-ExecutionPolicy -Scope Process Bypass
    . .\scripts\env.ps1
    cmake --preset default -B build
    if ($LASTEXITCODE) { throw 'Configure failed' }
    cmake --build build
    if ($LASTEXITCODE) { throw 'Build failed' }
}
# -> build\{scarletnx.exe,hactool.exe,snx-shaderc.exe}
```

Each new shell, from the checkout:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. .\scripts\env.ps1
```

`env.ps1 -Isolated` restricts PATH to bundled tools, Git and Windows.

## Rebuild, deploy, cache

```sh
cmake --build build
```

Default: Ninja + Clang, `RelWithDebInfo` (`-O2 -g`). Runtime relinks deploy `games/Game[.exe]` beside
matching `Game_code.so/.dll`; no-op builds skip deployment. Recompiler/ABI edits require [module rebuilds](DEVELOPMENT.md).

CPM: [SHA-256-pinned sources](../cmake/dependencies.cmake), shared `.cache/cpm` cache, offline configure when populated.
The cache holds each package as released, so one filled by hand (a Flatpak manifest's sources, a distribution's
offline build) works: hactool's patches go on a copy in the build tree.
Override via CMake/env `CPM_SOURCE_CACHE`. SDL3 uses CPM's system-package discovery with a pinned static
source fallback. Use `-DCPM_DOWNLOAD_SDL3=ON` to request the bundled source build.

FFmpeg and OpenSSL build with their own configure and make, in each build tree. With `SNX_DEPS_CACHE` (CMake or env)
naming a folder, a tree reuses a finished build there whose inputs match (source, configure line, compiler, C headers
and the cmake file that builds it, see [deps_cache.cmake](../cmake/deps_cache.cmake)) and publishes the ones it
builds, so other trees and branches skip both. A change to any input is a new folder; old ones are only left behind.

## Build variants

SDL3, Vulkan and FFmpeg are mandatory for the NX runtime, including when running with `-H`.
There are no renderer or media-decoding disable options. Tools-only, hactool-only and GNUABI-only builds do not need them.

Persistent per-build-dir options (POSIX examples):

```sh
cmake --preset default -B build -DSNX_FFMPEG_ASM=OFF && cmake --build build
```

- `-DSNX_FFMPEG_ASM=OFF`: omit x86-64 NASM assembly; slower FFmpeg decoding.
- `-DCPM_DOWNLOAD_SDL3=ON`: build the pinned SDL3 instead of using a system SDL3 development package.
- `-DSNX_DEPLOY=OFF`: disable runtime deployment.
- `-DSNX_GAMES_DIR=/path`: override deployment dir (default: `games/`).
- `-DCPM_SOURCE_CACHE=/path`: override source-download cache.

```sh
# Build without the GUI launcher; SDL3 remains required by the runtime:
cmake --preset default -B build/headless-vk -DSNX_BUILD_GUI=OFF -DSNX_DEPLOY=OFF &&
cmake --build build/headless-vk
# Run Vulkan without a display server, a surface or a swapchain:
./build/headless-vk/scarletnx "Game.xci" -H

cmake --preset windows-cross && cmake --build --preset windows-cross
# -> build/windows-cross/scarletnx.exe; requires llvm-mingw on PATH
```

All NX builds use SDL3 and support `-H` at runtime. Omit the flag to run with a window.
[Software Vulkan / lavapipe setup](USAGE.md#headless-vulkan--lavapipe).

Offscreen integration test, with an installed Vulkan 1.3 ICD (lavapipe works):

```sh
cmake --preset default -B build/headless-vk -DSNX_BUILD_GUI=OFF \
  -DSNX_DEPLOY=OFF -DSNX_TEST_VULKAN=ON && cmake --build build/headless-vk
# Set VK_DRIVER_FILES to your lavapipe manifest to force CPU rendering.
SNX_VK_VALIDATION=1 ctest --test-dir build/headless-vk -R vulkan_headless --output-on-failure
```

The test uses synthetic pixels, not a game. It checks offscreen submissions, frame recycling, cropping,
readback hashes and PPM contents without WSI. Validation requires the Khronos validation layer to be installed.
`SNX_TEST_VULKAN` is off by default so ordinary unit tests do not require an ICD.

Cross-builds omit host hactool. Use a native driver for `-x`. The recompiler still requires LLVM development
files for the target. `SNX_LLVM_CONFIG` must run on the build machine and describe that target installation,
not the build machine's LLVM libraries. Running the `snx-rtlib-gen` build helper also requires a suitable
`CMAKE_CROSSCOMPILING_EMULATOR`.

Host tools only, without SDL/Vulkan/FFmpeg or Python:

```sh
cmake --preset default -B build/tools -DSNX_TOOLS_ONLY=ON && cmake --build build/tools
build/tools/scarletnx -r -h
ctest --test-dir build/tools --output-on-failure
```

The recompiler generates game code with LLVM (`--backend=llvm`, the default and the recommended backend). LLVM's
development files are required: `llvm-config` finds them (`-DSNX_LLVM_CONFIG=/path/to/llvm-config` picks one), and
the configure stops without them on every platform, including Windows and cross-builds. There is no recompiler
build without LLVM. Modules are built for the machine that builds them, with its instruction set
extensions (build == host == target); nothing is detected at run time. Whole functions are batched into units of 90000
instructions (20000 on Android; `--chunk`), with a progress line per compiled unit.
`--backend=gcc` (libgccjit) is optional, built when libgccjit is found (`libgccjit` on Arch, `libgccjit-14-dev` on
Debian, `libgccjit-devel` on Fedora; `-DSNX_GCCJIT_ROOT=` points at another GCC's prefix). It is meant for DEC Alpha EV67 hosts, which LLVM has no backend for; everywhere else use LLVM.

Extraction builds missing hactool separately. Recompilation deploys the running full runtime;
the tools-only build creates one in `build/native-recomp`, or accepts `--runtime FILE`.
`--repo DIR` and `--build-dir DIR` before the command override compiled-in checkout/tool paths; without that
checkout (an installed copy), the user's data folder stands in for it (`~/.local/share/scarletnx`,
`%APPDATA%\scarletnx` on Windows), and on Linux the XDG config, state and cache folders hold its settings, logs and
caches ([USAGE](USAGE.md#folders)).
Python ≥3.12 is optional for `tools/*.py` diagnostics.

## Install / AppImage

```sh
cmake --install build --prefix stage
# -> stage/bin/, stage/share/licenses/scarletnx/
```

AppImage: run inside pkgforge's Arch container with `quick-sharun` ([CI setup](../.github/workflows/build.yml)):

```sh
cmake --preset default -B build/appimage -DSNX_DEPLOY=OFF -DCMAKE_INSTALL_PREFIX=/usr &&
cmake --build build/appimage &&
cmake --install build/appimage &&
GITHUB_REPOSITORY=scarletnx/ScarletNX scripts/make-appimage.sh
# -> dist/ScarletNX-<arch>.AppImage
```

[Nightly AppImages](https://github.com/scarletnx/ScarletNX/releases/tag/nightly), x86_64 and aarch64 · [Launch](USAGE.md).

[Licenses/credits](CREDITS.md) · [Architecture](ARCHITECTURE.md) · [Roadmap](ROADMAP.md).
