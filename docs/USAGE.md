<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Usage

Setup: [BUILD](BUILD.md). Checkout CWD; own dumps/keys.
For the keyboard-controlled SDL3 menu and file drops, see [GUI](GUI.md).

## Extract → AOT → run

Pass a single XCI, NSP, DXCI or DNSP to extract, recompile and play:

```sh
./build/scarletnx "Game.xci"
```

The driver runs each step as a child process and starts the game with `games/` as its working directory.
The name comes from the dump stem, retaining letters, digits, UTF-8, spaces, hyphens and underscores.
`--repo DIR` and `--build-dir DIR` may precede the dump. Keys use the usual discovery paths below.
`-H` (`--headless`) can precede or follow the dump and is passed to the game only.
Omitting it starts the game with a window, unless the build has no window backend.
`-x` means `--extract`, and `-r` means `--recomp` (the old `--recomp` spelling was removed).
On UNIX, short flags can be combined. Windows accepts separate short flags only.

To prepare a dump without playing, use `./build/scarletnx -r "Game.xci"`.
This extracts when necessary and forces recompilation, even if a cached executable exists.
`-rx` is equivalent on UNIX (`-r -x` on Windows). `-x` alone only extracts.
Pass arguments to the game executable after `--`, for example:

```sh
./build/scarletnx "Game.xci" -- some arg
```

The argument boundary is retained when launching the deployed game, so game arguments
cannot trigger launcher actions or overwrite runtime settings. This does not add a
homebrew-loader guest argv block.
`-f` (or `--fullscreen`, `--fullscreen=1`) can also precede or follow the dump and is forwarded to the game,
which then starts fullscreen (not in timed test runs). `--fullscreen=0` starts it windowed. Without either, the
game's `.ini` decides (`fullscreen=1` starts fullscreen).
`--touch=0|1|2` is forwarded to the game too: its touch screen takes nothing, the fingers (the default), or the
fingers and the mouse's left button. The menu's *Touch* item passes it; the game's `.ini` keeps its own `touch`.
`--controls=0|1` likewise hides or shows the [on-screen controls](#on-screen-controls) (the menu's *On-screen
controls* item, on by default on Android); the game's `.ini` keeps `controls`.
`--backend=gcc` selects libgccjit instead of the default LLVM backend when preparing
a dump. An explicit `--backend=llvm|gcc` forces recompilation rather than reusing a
cached executable built by another compiler.
Use the separate commands for updates, custom keys or other compiler options.

DXCI and DNSP with decrypted content need no keys. Containers are identified by their headers,
including renamed files. A matching bundled update is applied automatically. An explicit `--update`
replaces the bundled update and must match the base title. DLC-only and update-only dumps cannot
be used as a base game.

```powershell
.\build\windows-native\scarletnx.exe "C:\Games\Game.dxci"
.\build\windows-native\scarletnx.exe --extract "C:\Games\Game.dnsp" --update "C:\Games\Update.dnsp"
```

Run the same command again to retry after a failure or interruption:

- `work/Game/` skips extraction. Automatic extraction publishes this directory only after success,
  using `work/Game.extracting/` while running. An incomplete staging directory is discarded on retry.
- A complete `games/Game` (`Game.exe` on Windows), code module and matching `Game.abi` stamp
  skip recompilation. An ABI change or missing/invalid stamp triggers recompilation automatically.
  Older unstamped games rebuild once. Failed builds never publish a valid ABI checkpoint.
- Changing directory and running the game happen on every launch. A game failure never invalidates the build.

Failures stop the workflow and print the failed stage and exit status or signal. On POSIX, stopping a
child also aborts that stage so it can be retried, except for intentional debugger stops. Uncatchable termination of the driver itself, such as
`SIGKILL`, cannot print a diagnostic. Do not run concurrent workflows for the same output name.
Existing manually extracted directories are assumed complete. Remove stale checkpoints or use the
explicit commands when changing a dump or rebuilding game code.

### NRO applications

Pass an uncompressed NRO directly to recompile and play, without keys or extraction:

```sh
./build/scarletnx "app.nro" -H
```

The `.nro` extension is case-insensitive. NRO launches always recompile and restage the input,
so a same-named cached executable or extraction directory cannot silently select an older program.
Embedded RomFS assets are staged by the recompiler. Compressed NROs are not supported.
This uses ScarletNX's existing guest runtime, not a complete homebrew-loader environment.
Compatibility still depends on the application's runtime and service requirements.
See [NRO implementation notes](NRO.md) for the reference and validation policy.

To run the steps separately:

```sh
./build/scarletnx -x "Game.xci" --name Game
./build/scarletnx -r work/Game Game
games/Game
```

```powershell
.\build\scarletnx.exe -x "Game.xci" --name Game
.\build\scarletnx.exe -r work/Game Game
.\games\Game.exe
```

Remaining examples: POSIX.

```sh
# NSP + same-title update
./build/scarletnx -x Game.nsp --update update.nsp --name Game \
  --keys /path/prod.keys --title-keys /path/title.keys

./build/scarletnx -r work/Game Game --jobs 8
./build/scarletnx -r app.nro -o App --outdir games
./build/scarletnx -r -h
```

- **Keys:** encrypted content uses flags → the data folder (the checkout, or an installed copy's, below) → `~/.switch`
  (`%USERPROFILE%\.switch` on Windows).
  Common NSP tickets supplement title keys. Fully decrypted content needs neither key file.
- **Extraction:** defaults to `work/<dump stem>`; `--name`/`--out DIR` override. Refuses existing `exefs`;
  builds missing hactool. [Temp cleanup/key filtering](../src/nx/runtime/driver/extract.c).
- **Recompile:** AOT → module (LLVM, or `--backend=gcc` for libgccjit on DEC Alpha EV67 hosts) →
  final runtime deployment, for this machine's CPU. Shaders compile directly to SPIR-V at runtime.
  `-O 0..3,s,z` sets the optimization level.
- **Codegen:** `--no-build` stages data/INI only; `--interp-only` omits AOT functions.
  `--chunk N` sets instructions per code unit. Nothing is cached between runs.
  `--keep-ir` saves the generated code (LLVM IR, libgccjit dumps) in `<outdir>/<NAME>_build/`.
- **Data updates:** ExeFS refreshes; `Game_data/romfs` is reused.
  Replace cached RomFS after asset changes; preserve `Game_data/save/`.

| Output | Contents |
| --- | --- |
| `work/Game/{exefs,romfs}` | Extracted input |
| `games/Game[.exe]`, `Game_code.so/.dll` | Runtime + AOT module |
| `games/Game_data/`, `Game.ini` | Data/saves + settings |
| `games/Game_build/` | Generated code, only with `--keep-ir` |
| `games/shader_cache/vk_pipelines.bin` | Vulkan pipeline cache |

## GDB (UNIX only)

Install `gdbserver` on PATH. `-S` enables the native-code stub on loopback port **1234**
(the QEMU default port). The game runs until a debugger connects.
`-s` enables the stub and waits before runtime initialization. It implies `-S`.

```sh
./build/scarletnx -s "Game.xci"
# Or: games/Game -Hs
```

In GDB, load the deployed executable's symbols and connect:

```gdb
file games/Game
tar rem :1234
continue
```

The launcher prints a connection hint before waiting. A separate helper keeps accepting
connections while the game is stopped, then delegates the remote protocol to `gdbserver`.
The stub debugs host/native registers and code, not an emulated ARM64 CPU. It supports one
connection per run. Local ptrace permissions must allow attachment. Binding failures and
missing `gdbserver` are reported instead of silently running without a debugger.
No debugger flags or combined short flags are supported on Windows.

## Headless Vulkan / lavapipe

The normal SDL3 build can run without initializing SDL video or opening a window:

```sh
./build/scarletnx --game=games/Game -H --seconds=30 --shot=/tmp/frame
# Or extract, recompile and launch a dump without a window:
./build/scarletnx "Game.xci" -H
```

`-H` enables it. Omit the flag for the normal window. `--headless` is the long spelling
and accepts no value (`--headless=0` and `--headless=1` are errors).
This is a startup option, not a live toggle or an INI setting. SDL3 is required by every NX runtime
build, but headless runs do not initialize its video subsystem or require a display server.

Rendering still uses Vulkan 1.3, including uploads, draws, compute and screenshot readback. Headless runs
create no window, `VkSurfaceKHR` or swapchain and request no WSI extensions, including `VK_EXT_headless_surface`.
Frames still submit and recycle their resources. `--seconds`, `--frames`, `--script` and `--shot` work as before.
FFmpeg decoding is always included in the NX runtime, including headless builds, for video and hwopus audio.

To use software rendering, select Mesa's **lavapipe Vulkan ICD**, rather than the hardware driver. Mesa may
report this device as `llvmpipe` in the Vulkan log. Selecting a CPU ICD is independent of `-H`:

```sh
# Linux example. Substitute the actual lvp ICD manifest installed on your machine.
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/scarletnx --game=games/Game -H --seconds=30 --shot=/tmp/frame
```

Manifest names vary (for example `lvp_icd.json`). FreeBSD packages normally install manifests under
`/usr/local/share/vulkan/icd.d/`. Older Vulkan loaders use `VK_ICD_FILENAMES` instead of `VK_DRIVER_FILES`.
`LIBGL_ALWAYS_SOFTWARE` is an OpenGL setting and is not how this Vulkan renderer selects lavapipe.
No display server is required. Check the `vulkan:` device line in the log to confirm the chosen ICD.

For a build without an SDL3 dependency, see [headless build options](BUILD.md#build-variants).
This does not impose a staging-memory cap or fix the allocation/flush issues identified separately.

## AppImage / display

```sh
chmod +x ScarletNX-x86_64.AppImage
./ScarletNX-x86_64.AppImage --game=games/Game
SDL_VIDEO_DRIVER=x11 games/Game
```

On an ARM64 machine the image is `ScarletNX-aarch64.AppImage`.
`--game` selects `Game_code.so`/`Game_data/`; or name the image `games/Game.AppImage` to load adjacent files.
The AppImage also prepares dumps: `./ScarletNX-x86_64.AppImage Game.nsp`, in the folder [Folders](#folders) names,
and each game gets a copy of the AppImage (on Windows, of `scarletnx.exe` and the DLLs next to it).
X11 bypasses Wayland. [Licenses](CREDITS.md).

## Folders

The driver works in one folder, the first of these that applies (`--repo DIR` names another):

1. the checkout it was built from;
2. a portable copy's: the folder of the AppImage, or of the Windows zip's programs, with a `games/` folder beside it;
3. the user's data folder: `~/.local/share/scarletnx` on Linux, `%APPDATA%\scarletnx` on Windows,
   `~/Library/Application Support/scarletnx` on macOS. On Android the app names its own, `/sdcard/ScarletNX`.

Everything stays in that folder, except on Linux when it is the user's data folder: there what is not a game itself
goes in the [XDG](https://specifications.freedesktop.org/basedir/latest/) folder of its kind.

| What | In the folder | Linux, the user's data folder |
| --- | --- | --- |
| a game: its program, `Game_code.so`, `Game_data/` (saves), `captures/` | `games/` | `~/.local/share/scarletnx/games/` |
| its settings, `Game.ini` | `games/` | `~/.config/scarletnx/games/` |
| its `Game.log` and `threads.txt` | `games/` | `~/.local/state/scarletnx/games/` |
| its pipeline cache | `games/shader_cache/` | `~/.cache/scarletnx/games/shader_cache/` |
| extractions (`3ds/` for the 3DS driver) | `work/` | `~/.cache/scarletnx/work/` |
| the menu's themes, `<name>/theme.ini` | `themes/` | `~/.local/share/scarletnx/themes/` |
| `prod.keys`, read before `~/.switch` | the folder | `~/.local/share/scarletnx/` |
| the menu's Share log, `scarletnx.log` | the working folder (`/sdcard/ScarletNX/` on Android) | `~/.local/state/scarletnx/` |

The menu's `recent.txt` and `last-run.log` follow the user, not the folder: `~/.local/state/scarletnx/gui/` on Linux,
`%APPDATA%\scarletnx\gui\` on Windows. `~/.local/share`, `~/.config`, `~/.local/state` and `~/.cache` are the defaults
of `$XDG_DATA_HOME`, `$XDG_CONFIG_HOME`, `$XDG_STATE_HOME` and `$XDG_CACHE_HOME` (a relative one is ignored).

An older version kept all of it in the data folder (`games/Game.ini`, `games/Game.log`, `work/`, `gui/recent.txt`).
This one moves none of it: as long as an older file is there, each run says where it is and where it goes now, in
the output or the game's log.

## Settings / input

`Game.ini`: help rewritten at startup, most settings hot-reload. `--key=value` overrides one run.
Restart for `docked`, `fullscreen`, `language`, `resolution`, `anisotropy`, `memory_gb`, `heap_max_mb`, `profile`.
`fullscreen=1` starts fullscreen, except in timed test runs (`--seconds`, `--frames`). `-f` or `--fullscreen=0` sets
it for one run.

| Setting | Effect |
| --- | --- |
| `docked=1` | TV mode |
| `resolution=1` | Native scale; range 0.25–4 |
| `fps_limit=game` | The pace the game asks for, as on the console: 60 FPS, or 30 for a game made for 30. `0` is unlimited, and `10` to `1000` is that many FPS |
| `vsync=1` | Images are shown only on the display's refresh, so nothing tears. `0` doesn't wait for it: the newest image is shown on the next refresh where the display can do that, else at once (which may tear). A display that can do neither waits for the refresh as `1` does |
| `anisotropy=0` | Texture filtering as the game asks. `2` to `16` sharpens surfaces seen at an angle |
| `shadows=0` | Skip shadow maps |
| `language=1` | English (US); other IDs in INI |
| `network=1` | Online through the computer's connection (bsd, sfdnsres, nifm); `0` offline, loopback only |
| `log=2` | Verbose, adds runtime overhead |
| `auto_stub_ipc=1` | A service command the runtime doesn't know answers success, logged as `[undeclared]`, and a service it doesn't have answers every command that way. `0` answers them with the console's UnknownCommandId instead, which shows the gap where the game makes the call |

`audout` PCM playback and the audio renderer's device sink (`audren`) use SDL3's system-default output on
Windows, Linux and supported Unix systems. Choose the output in your operating system's sound settings. The
renderer does not render splitters, voice biquad filters, or the delay, reverb and biquad effects yet.

SDL gamepads use Switch positions; pads + keyboard share one guest controller.

| Keyboard | Switch |
| --- | --- |
| Arrows; WASD / IJKL | D-pad; L/R sticks |
| X or Space / Z / C / V | A / B / X / Y |
| Q / E / 1 / 3 | L / R / ZL / ZR |
| Enter / Backspace | Plus / Minus |

F1 help; F2 the game's menu; F3 screenshot; F4 FPS limit off and on; F5 thread dump; F11/Alt+Enter fullscreen;
Escape quit. A window shows at most one image per refresh and drops the rest unless images tear, and the game keeps
the pace `fps_limit` sets.

### On-screen controls

A pad drawn over the game, with rEden's buttons and sizes, laid out as the console's two controllers hold them:
sticks, D-pad, A B X Y, L R ZL ZR, plus and minus, the stick buttons, Home (the game's menu) and Capture (a
screenshot). Fingers press them, the mouse's left button too; a finger that lands off them goes to the game's
touch screen. They are on by default on Android and off elsewhere.

The game's menu (Back on Android, Home on a gamepad, F2) has their page: show them, *Edit layout* (drag a control;
tap one and size it with the bar on top; *Done* or Back ends it), snap to a grid, scale and opacity, the buttons
shown, a stick's middle where the finger lands, a D-pad a finger slides on, a buzz on each press (Android), the touch
screen, and *Reset layout*. The game's `.ini` keeps them: `controls`, `controls_scale`, `controls_opacity`,
`controls_snap`, `controls_rel_center`, `controls_dpad_slide`, `controls_haptics`, and a control's place once it
was moved (`controls_a=0.930 0.470 100 1`: its middle across the screen, its size in percent, shown).
Beside the executable: `Game.log`, `threads.txt` (15 s refresh), `captures/` (screenshots); an installed copy on Linux
keeps the first two in its state folder ([Folders](#folders)).
Scripts/disasm/shaders: [DEVELOPMENT](DEVELOPMENT.md).
