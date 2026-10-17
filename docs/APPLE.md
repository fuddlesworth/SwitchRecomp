<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# macOS and iOS

Both build on a Mac with Xcode, for Apple silicon (arm64), with Xcode's Clang, and the CI builds both on every push
(`.github/workflows/build.yml`). They are new and have only been built by CI. No game has run on either yet, for the
reasons under [What is missing](#what-is-missing).

## The frontend

The menu and the runtime are SDL's window, as on Android. The parts the system does are a thin frontend over it, in the
same shape as Android's `MainActivity` and `src/gui/android.c`:

- `src/gui/apple.m` (Objective-C) is the menu's side: the data folder, the game the menu plays, and the SDL window's
  `UIWindow` or `NSWindow` for the Swift to present over.
- `dist/apple/Frontend.swift` (Swift) is the system's side, called from `apple.m` through `@_cdecl` functions: on iOS,
  the document picker SDL lacks there and the share sheet; on macOS, the Finder showing the saved log.

### macOS

`ScarletNX.app` is the desktop program in a bundle: `scarletnx` and `hactool` in `Contents/MacOS`, the themes and the
licenses in `Contents/share`, where an installed copy looks for them. It works as on Linux. The menu extracts and
recompiles through the driver as a child process, with SDL's own file and folder dialogs. Its data folder is
`~/Library/Application Support/scarletnx`. *Share log* saves the log and selects it in a Finder window. The app is
signed ad hoc, as Apple silicon runs no unsigned code. A copy from a download may need *Open* from the Finder's menu
the first time.

The Vulkan driver is MoltenVK, Ryujinx's build of it ([V380-Ori/Ryujinx.MoltenVK](https://github.com/V380-Ori/Ryujinx.MoltenVK)),
which carries the patches an emulator's pipelines need and mainline MoltenVK lacks. Both apps carry it in their
`Frameworks`, where the runtime has SDL load it (`backend/sdl3.c`). The bare `scarletnx` outside the app uses the
system's Vulkan loader instead (Homebrew's `molten-vk` and `vulkan-loader`).

### iOS

An iOS app runs one process and may not start another, so the iOS app works like Android's game activity without its
menu's driver:

- **The data folder** is the app's Documents, which the Files app (*On My iPhone > ScarletNX*) and the Finder show.
  Games go in its `games/`, each as `games/<game>/<game>_code.so` with its `_data/`, prepared on a computer.
- **The menu** lists those games. *Load* and the folder settings open the system's document picker. Preparing a
  dump says it needs a computer: extraction and recompilation run the driver and hactool as programs of their own.
- **A game** runs in the app's process once the menu closes (`src/nx/runtime/entry.c`): the runtime starts once per
  process, so the app ends with the game.
- **Share log** opens the share sheet with `scarletnx.log`.

The IPA is unsigned: AltStore, SideStore or a developer account signs it for the device, with
`dist/apple/ScarletNX.entitlements` (the larger address space and memory limit a game needs). It holds MoltenVK's
framework.

## What is missing

- **16 KiB pages.** Apple silicon's pages are 16 KiB, and the runtime maps and protects guest memory in the Switch's
  4 KiB pages (`plat_vm_reserve` stops with a message). This is what keeps every game from running on both systems.
- **Game modules.** The recompiler links ELF and PE only (`src/common/cpu/recompiler/llvm/link.c`). macOS needs a
  Mach-O dylib, signed. iOS loads no code it did not ship with, so its games would have to be linked into the app
  (`snx_module_static`, `src/nx/runtime/main.c`) or loaded into JIT memory on a device that allows it.
- **32-bit games on iOS.** Their blocks are compiled at run time with LLVM's MCJIT (`src/nx/runtime/arm.c`), which
  needs memory that becomes executable. iOS gives that only to an app that may JIT.
- **Preparing on iOS**, for the reasons above, until the driver runs in the app's process.
- **The 3DS and Linux drivers** are not built for iOS (`SNX_BUILD_3DS`, `SNX_BUILD_GNUABI`), nor the Linux one for
  macOS.

## Building

LLVM's libraries first, once for each system, with AArch64's code generator alone, the one the recompiler uses
(about 10 minutes on an M1). `scripts/apple-llvm.sh` builds them, and the CI caches them:

```sh
scripts/apple-llvm.sh macos ~/apple-llvm/macos
scripts/apple-llvm.sh ios ~/apple-llvm/ios
```

macOS, then the app:

```sh
cmake --preset macos -DLLVM_DIR=$HOME/apple-llvm/macos/lib/cmake/llvm
cmake --build --preset macos
cmake --install build/macos --prefix stage
scripts/apple-app.sh macos stage out moltenvk   # out/ScarletNX.app
```

`moltenvk` is the release's `MoltenVK-macos.tar`, extracted (`MoltenVK-ios.tar` for iOS). The CI pins the release and
its SHA-256 (`.github/workflows/build.yml`). Homebrew's LLVM works too, with every code generator:
`-DSNX_LLVM_CONFIG="$(brew --prefix llvm)/bin/llvm-config"` in place of `LLVM_DIR`.

iOS, then the IPA:

```sh
cmake --preset ios -DLLVM_DIR=$HOME/apple-llvm/ios/lib/cmake/llvm
cmake --build --preset ios
cmake --install build/ios --prefix stage
scripts/apple-app.sh ios stage out moltenvk     # out/ScarletNX.ipa
```

`MACOS_VERSION` (default 13.0) and `IOS_VERSION` (default 16.0) are the oldest systems `apple-llvm.sh` builds for,
as the presets' `CMAKE_OSX_DEPLOYMENT_TARGET` are the apps'. FFmpeg, OpenSSL and the recompiler's runtime library are built outside
CMake's compiles and get the SDK, architecture and oldest version from `cmake/apple.cmake`.
