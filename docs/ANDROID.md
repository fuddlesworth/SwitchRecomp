<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Android

The app is the [GUI](GUI.md) and the runtime, each in SDL's activity (`dist/android`): the menu extracts and
recompiles on the phone, then the game opens.

- **The menu** (`MainActivity`) is `scarletnx-gui` as `libmain.so`, in the YoRHa theme. Taps and the back button
  drive it (see [GUI](GUI.md)). Its data folder is in the app's own storage, `.scarletnx/` in its files
  (`/data/data/org.scarletnx/files`): `games/` goes there, the extractions in its `cache/work/` and the games' logs in
  its `state/` ([USAGE](USAGE.md#folders)). It needs no permission and nothing slows it down as the shared storage's
  FUSE does; it goes with the app, which Android 10 and later ask about before uninstalling it. The system's Files app
  shows it as *ScarletNX*, to copy dumps, keys and saves in and out, and the menu's *Open data folder* opens it there.
  Settings > App > Folders may move the data, the games or the extractions to the shared storage; it asks first, as
  games may run slower there, and takes the folder at a second tap.
  `HOME` stays the app's own files, for `.switch/prod.keys`. An older version kept the data in `/sdcard/ScarletNX`
  (or, without *All files access*, in the app's files themselves): as long as its `games/` or keys are there, the
  menu lists them as it opens, with the space each takes; nothing is moved.
- **Loading** (a *Load* item, *Setup device keys*) opens its screen, a box the whole width, and a tap in the box
  opens the system's file picker. A game's file is read by its path, so the app asks for access to the files when
  it starts (all files from Android 11): the picker's path, or for a document without one the file its descriptor
  opens. A cloud's document has none and is refused, its URI in the message. Wherever the dump is, its extraction
  goes to the data folder's `cache/work/`, in the app's storage. *Games* lists the games there and the dumps put
  there, to be prepared.
- **The driver** runs as a child of the menu, as on a desktop, from `libscarletnx-run.so`: an executable named so
  that Android installs it with the libraries, the only files of its own an app may execute. It loads the runtime,
  `libscarletnx.so`, beside it. It cannot open a window there, so it ends at `Ready <game>` and the menu opens it.
  A game of *Games* goes through it too, recompiled first when another version of the app made it.
- **Behind other apps** the driver keeps going: a notification shows the game, its step and a bar, and a tap on it
  returns to the menu. A run that ends there leaves a notification saying so, and its game opens once the menu shows.
  The first run asks to show it (Android 13) and to lift the battery limit; on a phone whose maker adds its own limit
  (Samsung, Xiaomi, OPPO, vivo, Huawei...) it names the setting, with a button to its screen.
- **A game** runs in `GameActivity`, in a process of its own: the runtime's `main` with `--game=games/<game>`.
  Its process ends with the game, and the menu comes back on its first screen. `<game>_code.so` loads where it is;
  from a game folder on the shared storage, which maps nothing executable, the runtime loads a copy in the app's
  cache. The back button opens the
  game's menu over it, in the menu's look (a gamepad's Home does on every system): the on-screen controls'
  settings, the touch screen's, and *Exit*; back again returns to the game. Leaving it for the home screen keeps it:
  its window is made anew when it comes back.
- **Share log**, in the menu, saves the message on the screen, the last run's output (`last-run.log`, which the
  menu keeps on every system) and the newest game log to `scarletnx.log` in the data folder's `state/`, then opens
  the share sheet with that file.

The app runs on arm64 phones and on x86_64 Android (the emulator, some tablets and Chromebooks): the recompiler makes
the code of the device it runs on. It installs from Android 5.0 (API 21). A game needs Vulkan 1.3: on a device without
it (Android 5.0 and 6.0 have no Vulkan at all) the menu still prepares games, and a game says what it lacks. The
fingers reach a game's touch screen (*Touch screen*, in the game's menu or the Settings, turns them off or adds a
mouse). The buttons and sticks are on the screen, over the game ([USAGE](USAGE.md#on-screen-controls)), and a gamepad
works beside them; the menu's *On-screen controls* item hides them.

## Keys

*Setup device keys* copies `prod.keys` into the app's own files, as `.switch/prod.keys` in its `HOME`, and its screen
names that path. Pick it from the device's storage: it is read through the picker, so it needs no access to the
device's files. The picker gives no name to go by, so the file has to read as a prod.keys (`name = hex` lines),
whatever it is called; any other is refused. The menu installs no `title.keys`. It is optional, as an NSP's tickets
give the title keys too.

The keys may also be copied by hand into the data folder, *ScarletNX* in the Files app: `prod.keys`, and `title.keys`
if you have one. The driver reads both there before the app's own files. The *Setup device keys* screen shows only
the keys it installed, so it does not name these.

## Building

LLVM's libraries for each ABI, once (about 10 minutes each on 12 cores):

```sh
scripts/android-llvm.sh arm64-v8a ~/android-llvm/arm64-v8a
scripts/android-llvm.sh x86_64 ~/android-llvm/x86_64
```

Then the APK, from the native build of each ABI given (one APK holds them all; Android installs the device's):

```sh
scripts/android-apk.sh ~/android-build ~/android-llvm arm64-v8a x86_64
adb install -r ~/android-build/stage/gradle/outputs/apk/debug/ScarletNX-debug.apk
```

They need the NDK (`ANDROID_NDK_ROOT`, else the newest under `$ANDROID_HOME/ndk`), the SDK (`ANDROID_HOME`),
Gradle 9 (`GRADLE`, default `gradle`) and a JDK 17 or newer (`JAVA_HOME`). Extra CMake arguments go after `--`.
`ANDROID_API` (default 21) is the oldest Android both scripts build for, the app's `minSdk`.
`android-apk.sh` installs each build into `<out>/stage` (the libraries, SDL's Java classes and the icon), which
`dist/android/build.gradle` packs with the two activities; Gradle's own output goes there too. The CI does the same
on every push (`.github/workflows/build.yml`): an APK per ABI, and one holding both.

The Android emulator runs it on an x86_64 image; `-gpu swiftshader_indirect` gives it SwiftShader's Vulkan 1.3. Its
ARM translation runs the arm64 build too (an APK with arm64 alone, or an arm64 program from `adb shell`), slowly.
