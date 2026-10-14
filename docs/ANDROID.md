<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Android

The app is the [GUI](GUI.md) and the runtime, each in SDL's activity (`dist/android`): the menu extracts and
recompiles on the phone, then the game opens.

- **The menu** (`MainActivity`) is `scarletnx-gui` as `libmain.so`, in the YoRHa theme. Taps and the back button
  drive it (see [GUI](GUI.md)). Its working folder and `HOME` are the app's own files
  (`/data/data/org.scarletnx/files`), where the code it recompiles may run, unlike the shared storage: `games/`,
  `work/` and `.switch/prod.keys` go there.
- **Loading** (*Select a file*, *Setup device keys*) opens the system's file picker. A file on the device's storage
  or a card is read by its path, so the app asks for access to all files when it starts; a document the picker
  gives without one (Downloads' own list, another app's, a cloud's) is refused: pick it under the device's storage.
- **The driver** runs as a child of the menu, as on a desktop, from `libscarletnx-run.so`: an executable named so
  that Android installs it with the libraries, the only files of its own an app may execute. It loads the runtime,
  `libscarletnx.so`, beside it. It cannot open a window there, so it ends at `Ready <game>` and the menu opens it.
- **A game** runs in `GameActivity`, in a process of its own: the runtime's `main` with `--game=games/<game>`.
  Its process ends with the game.
- **Share log**, in the menu, saves the message on the screen, the last run's output (`last-run.log`, which the
  menu keeps on every system) and the newest game log to `Download/ScarletNX/scarletnx-log.txt`, then opens the
  share sheet with them.

Limits: the recompiler makes x86-64 code only, so the app runs on x86_64 Android (the emulator, some tablets and
Chromebooks) until it makes arm64 code. A game needs a gamepad: there are no on-screen controls yet. Vulkan 1.3
is required.

## Building

LLVM's libraries for each ABI, once (about 10 minutes on 12 cores):

```sh
scripts/android-llvm.sh x86_64 ~/android-llvm/x86_64
```

Then the APK, from the native build of each ABI given:

```sh
scripts/android-apk.sh ~/android-build ~/android-llvm x86_64
adb install -r ~/android-build/stage/gradle/outputs/apk/debug/ScarletNX-debug.apk
```

They need the NDK (`ANDROID_NDK_ROOT`, else the newest under `$ANDROID_HOME/ndk`), the SDK (`ANDROID_HOME`),
Gradle 9 (`GRADLE`, default `gradle`) and a JDK 17 or newer (`JAVA_HOME`). Extra CMake arguments go after `--`.
`android-apk.sh` installs each build into `<out>/stage` (the libraries, SDL's Java classes and the icon), which
`dist/android/build.gradle` packs with the two activities; Gradle's own output goes there too.

The Android emulator runs it on an x86_64 image; `-gpu swiftshader_indirect` gives it SwiftShader's Vulkan 1.3.
