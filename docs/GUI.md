<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# GUI launcher

`scarletnx-gui` is a small SDL3 launcher for the command-line drivers. It uses an
embedded public-domain 8x16 VGA BIOS font, nearest-neighbor integer scaling, and
English text only. No font files or localization packages are needed.

```sh
./build/scarletnx-gui
```

The window's title tells which build it is, as `ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)`, for example
`ScarletNX main 43bf71a (clang 22.1.8, Linux x86_64)`; the commit ends in `-dirty` when the tree had changes, a
build of a detached checkout outside CI says `detached`, and both read `unknown` outside a git checkout. A game's window carries the same line after the game's name.

- Up/Down and Left/Right move the yellow focus, wrapping at the ends.
- Moving the mouse over an item focuses it in yellow without activating it.
- Space, Enter or a left click explicitly activates an item, marking it bright
  pink. Activation is tracked separately from hover/focus, so the activated item
  stays pink while another item is hovered in yellow. Mouse controls also work
  on load sub-options, browser entries and Back.
- A finger taps an item anywhere on its row, as a click does on its label; a swipe across
  turns the pages of `games/`; a tap on a finished run returns to the menu, and one on a
  running run's ESC line aborts it. Android's back button is ESC. On a screen packed with
  pixels (a phone) the text takes the display's own scale, as far as the menu still fits.
- Moving outside a submenu (including over another main item) leaves it open
  and anchored to its activated parent. ESC goes back; explicitly activating a
  different main item replaces/closes the submenu.
- Selecting **Load Switch NSP/XCI/NRO**, **Load Linux ARM ELF** or **Load 3DS CIA** keeps that item
  visible and opens two selectable options underneath it: **Select via drag and drop**
  and **Select file from games/ instead**.
- Activate **Select via drag and drop** to wait for a file. SDL file drops and text drops
  are supported. Ctrl+V also accepts a clipboard path while waiting. ESC returns to
  the two options, and another ESC returns to the main menu.
- **Load 3DS CIA** uses `scarletnx-3ds`. Its `games/` browser lists CIA inputs,
  not host executables. The driver extracts supported decrypted application CIAs,
  then returns an explicit error because ARM11 AOT/execution is still pending.
  Existing extraction directories are refused rather than treated as cache hits.
  See [3DS port](3DS.md) for limits and extract-only usage.
- **Setup device keys**, immediately after the three load entries, prompts you to
  drag and drop `prod.keys` (or paste its path with Ctrl+V); a blinking
  **[ Drop prod.keys here ]** waits in the middle of the screen (the theme's
  `[prompt]`). It installs the file to `~/.switch/prod.keys`, or
  `%USERPROFILE%\.switch\prod.keys` on Windows; an installed copy on Linux puts it in its
  data folder instead (`~/.local/share/scarletnx/prod.keys`), which the driver reads before
  `~/.switch`, and names the keys of `~/.switch` it would use until then. It creates the
  folder if needed. Existing keys trigger the bright-red warning
  **Keys already setup. This will overwrite the previous ones**. A successful
  drop replaces them and returns to the menu; ESC cancels without changing them.
- **[ON/OFF] Fullscreen** sets how the Switch game starts. ON passes `-f` (short for
  `--fullscreen`) and OFF passes `--fullscreen=0`, so the launcher's choice wins over the
  game's `.ini`. On Android it also puts the launcher itself on the whole screen.
- **Touch** sets the game's touch screen: the fingers, the mouse too, or off. It passes
  `--touch=1`, `--touch=2` or `--touch=0`.
- **[ON/OFF] On-screen controls** shows or hides the pad drawn over the game. It passes
  `--controls=1` or `--controls=0`, and the game's menu edits the pad.
- **[ON/OFF] Enable libgccjit** passes `--backend=gcc` when preparing a Switch dump.
  It stays OFF if this build lacks libgccjit. Prepared executables in `games/`
  already have a compiled backend, so this setting does not recompile them.
- **[ON/OFF] Enable gdbstub** passes `-S` and `-s` as separate arguments, enabling
  the Switch runtime's stub on port 1234 and waiting for the debugger at startup.
  The runtime currently supports the stub on UNIX only.
- Selecting or hovering these settings shows a short description immediately
  above the CPU/RAM footer. The CPU-only Linux driver does not support these
  Switch options; they are never forwarded as Linux guest arguments or to the 3DS driver.
- **Theme** switches to the next theme: the built-in Default and YoRHa, then each
  folder of `themes/` (see [Themes](#themes)). It lasts for the session.
- **Theme editor** edits the current theme: a row per `theme.ini` key, and beside them a
  preview of the menu or a run (Tab switches). Left/Right change a word, a switch or a
  number, Enter types a colour, a text or a number.
  Save (Ctrl+S) writes `themes/<folder>/theme.ini` in the data folder and switches to it;
  a built-in is saved as a new theme, `mytheme` until renamed. ESC leaves, asking first if
  there are unsaved changes.
- **Text size** steps the menu's letters through Auto (fit the screen) and four sizes.
  Bigger letters scroll the menu.
- **Share log** saves the last run's output and game log to `scarletnx.log`. On the
  desktop it goes in the working folder with its path copied. On Android it goes to
  `/sdcard/ScarletNX/` and is shared.
- **Exit** closes the launcher. Closing its window during a run terminates the
  entire owned process tree, so let the game exit normally to preserve its saves.
- **Recent games**, above the items, lists the last five games run, newest first, by
  the name their run gave (the runner's `ScarletNX - <name>` line), else their file's.
  The focus moves between them and the items in one ring, and the line under the menu
  shows the focused game's path. Choosing one runs it again with the driver it was
  loaded with and the options the menu has now; a game whose file is gone leaves the
  list instead. The list is kept in `recent.txt`, beside `last-run.log`, in the user's state
  folder on Linux (`$XDG_STATE_HOME/scarletnx/gui/`, by default `~/.local/state/scarletnx/gui/`;
  an older version's in `~/.local/share/scarletnx/gui/` stay where they are), or in
  `%APPDATA%\scarletnx\gui\` on Windows, a line per
  game: `switch`, `linux` or `3ds`, `file` or `games` (a program from games/, run as it is),
  the path and the name, separated by tabs.

The launcher looks beside its executable, then on PATH, for `scarletnx-mariko`
(the existing Switch driver alias), `scarletnx-gnuabi` or `scarletnx-3ds`. Files are passed directly
as argv entries, never through a shell. Spaces, Unicode paths and shell punctuation
are not evaluated. Local `file://` text drops are decoded, but lists and remote URI
authorities are rejected. The working directory is not changed and is shown at the
bottom of the window. See [GNUABI](GNUABI.md) for supported Linux ELF applications.

## Selecting an executable from games/

**Select file from games/ instead** replaces the menu and old output with a file
list. It reads the `games/` of the folder the driver works in, which the menu passes
it as `--repo`: the checkout of a build tree, else the folder of an AppImage (or of the
GUI) that has a `games/` beside it, else the user's data folder (docs/USAGE.md). Each entry is displayed by basename, sorted
alphabetically. Only immediate executable files are listed (execute permission
on UNIX, `.exe` on Windows). Hidden files, shared libraries and directories are
excluded. For **Load 3DS CIA**, only immediate non-hidden `.cia` files are listed
(case-insensitive); they do not need execute permission, and directories are
excluded. The list is refreshed each time it is opened.

- **Back [ESC]** is the first selectable item on every page. Enter/Space on it, or
  ESC anywhere in the browser, returns to the two load options.
- Up/Down select Back or a file on the current page. Enter/Space opens a file.
- Left/Right and PgUp/PgDown go to the previous/next page. The page number and
  total file count are shown. Paging stops at the first and last pages.
- Resizing adjusts the page size while keeping the selected file visible.
- Empty or missing directories show a diagnostic and still provide Back.

For the Switch selection these are already-prepared native game launchers, so
selecting one runs it directly with the fullscreen, touch, on-screen controls and
debugger settings rather than trying to extract it as a dump. For the Linux
selection, the selected file is passed to `scarletnx-gnuabi` as an ARM64 ELF.
Output capture, resource accounting and the return to the main menu work the same
way for both input methods. For the 3DS selection, the browser passes a CIA input
to `scarletnx-3ds`; it never executes it directly.

## Output and resource use

While a driver runs, the window displays its merged stdout/stderr pipe. Descendants
inherit the same pipe, including extraction, recompilation and the final game.
The console consumes bytes as they arrive, even without a newline. Long lines wrap
at the right edge. At the bottom, rows move up and the oldest row is discarded.
Only the visible screen is retained, with no scrollback or accumulating log file.
ANSI color/title escapes are ignored. UTF-8 characters available in CP437 use the
corresponding VGA glyph, and other characters are displayed as `?`.

After the driver and its descendants finish, the initial menu returns automatically
with the exit status and the last visible output. Nothing is restarted automatically.
A missing driver or invalid dropped path leaves the prompt open with a diagnostic.
Drivers use line-buffered stdout so progress remains visible when piped. A third-party
program that buffers or privately redirects its output still controls when it flushes.

The footer samples combined GUI and descendant resource use every half second:

- RAM is the sum of resident sets (Windows working sets), in MB or GB. Shared pages may
  be counted more than once. It is not the machine's total installed memory.
- CPU is a share of the whole machine, 100% being every core busy, then how many cores'
  worth of time that is.

Linux and FreeBSD track a private child session, including the separate process
groups created by the driver's stage supervisor and descendants that outlive their
immediate parent. Linux uses `/proc`, FreeBSD uses `sysctl`. Windows assigns the
suspended driver to a Job Object before letting it run, containing its descendants
and providing job-wide CPU accounting. No elevated privileges are needed.

## Themes

A theme colours and dresses the screen's three parts: the head (a title and the hints
at the top), the body (the menu, the file list or the driver's output) and the footer
(resource use and the working directory). It applies to the menu screens and to the
run, which is **compiling** until the runner prints its `ScarletNX - <name>` banner and
**loading** from then on.

The built-ins are `default`, the look above, and `yorha`: paper and ink with a header
bar, square markers and the focused line inverted on the menus, and a run drawn as a
dark loading screen in its own colours (`[run]`): the title, the game's name with turning
dots and a turning ring; each step the driver and the runner report (unpack, extract,
recompile with its blocks, built, launch, GPU) typed down the page, with turning dots
while it runs and `...OK` or `...FAILED` once it ends, the cursor blinking on the current
one, and the other lines it printed indented under it (`[run] bar` adds a progress bar
with the recompile's percentage). The menu fades out to black as a run starts and the run fades in from it; ESC
fades the run out and the menu back in.

A theme is also a folder holding a `theme.ini`, the folder's name its id, in
`themes/` in that same folder or in `themes/` beside the GUI; the first one found
wins, and a folder named like a built-in replaces it. The built-ins are such files, and
nothing else: each folder of `src/gui/themes/` is one, its `theme.ini` compiled into the GUI
(`cmake/gui_themes.cmake`) and read as any other, setting every key. The runtime's game menu
takes YoRHa's colours from the same file at build time. The files are copied beside the GUI
in the build and installed with it: copy one under a new name to make a theme, or edit one
to change the built-in. `[theme] base` picks the built-in a file starts from, and a key it
leaves out, or that cannot be read, keeps the base's value. The menu's **Theme editor**
writes one with every key.

```ini
[theme]
; the theme file's format, 1; a newer file's keys this GUI knows still apply
format = 1
name = Night ink
author = me
; default | yorha
base = yorha
[colors]
; #RRGGBB. soft: hints, secondary text, rules. focus: the focused line, or its band.
; dim: the ring's outer circle, the progress bar's empty segments and its rule
page = #101418
ink = #D8D4C4
soft = #8A8676
focus = #D8D4C4
active = #C8A040
failure = #D05040
dim = #3E3E40
[head]
; plain | bar | none; rule: a line under the head
style = bar
rule = true
; the title per screen: menu, games, keys, compiling, loading
menu = NIGHT  /  MENU
[body]
; before each menu item; quotes keep spaces
marker = "> "
; color | bar
focus = bar
[footer]
style = plain
rule = true
[run]
; console: the driver's output as it comes; steps: the loading screen above, in its own colours
layout = steps
page = #141416
ink = #E6E4DC
soft = #8E8C86
dim = #3E3E40
failure = #D2463A
warning = #D9B24A
; faint lines over the page, the progress bar and percentage, and the ring: none | left | right
texture = true
bar = true
ring = right
; detailed: each step's other lines under it, down the screen; collapse: a done step's
; folded away, a failed one's kept
detailed = true
collapse = false
[prompt]
; a line that waits on the user, when its key is true: framed, left | center | right on its
; row, blinking at blink_ms. keys: the keys screen's drop line; done, failed: the ESC line
; after a run; aborting: a run being aborted
align = center
open = "[ "
close = " ]"
blink = true
keys = true
failed = true
[transitions]
; milliseconds, 0 to 10000; 0 turns the thing off. fade_out_ms: the menu as a run starts,
; and a run ESC leaves; fade_in_ms: what comes after
fade_in_ms = 250
fade_out_ms = 500
; how a step's line comes in: none | typing | slide | fade
line_in = typing
line_in_ms = 220
typing_ms = 5
; async: every line types from when it comes; sync: one at a time. A running step's newest line
; types a character each typing_ms, any other line whole in typing_ms
typing = async
blink_ms = 500
; a line too long for its row (big letters) slides left a column every scroll_ms and back; 0 cuts it
scroll_ms = 120
[splash]
; the boot's: the program's icon on page for ms milliseconds (0: none). chance: the boots in 100 that
; show this theme's, the default theme's the others
page = #DAD6BE
ms = 1000
chance = 25
```

Comments take a line of their own, starting with `;` or `#`.

The format is rEden's ErET format 1, extended, so one file can serve both: each program
reads the keys it knows and leaves the others. From ErET's, the GUI also reads:

| ErET key | here |
|---|---|
| `[theme] layout` | the base when no `base` names one: `menu` and `loading` are `yorha`, `plain`, `debug` and `skip` are `default` |
| `[colors]` | with `layout = loading` and no `base`, the run's colours, as `[run]`: ErET's loading screen is YoRHa's runs |
| `[colors] texture`, `warning` | as `[run] texture`, and the colour of a run being aborted |
| `[header] ring_side` | `right`, `left` or `right-near-text` (right after the name and its dots), as `[run] ring` |
| `[transitions] typing` | `async` or `sync`, as above |
| `[loading] title` | the loading screen's title |

ErET needs `[theme] layout`, so the built-in files carry one: `debug` and `menu`.

### Theme formats

`[theme] format = N` names the theme format a file was written for; this GUI's is 1
(`GUI_THEME_FORMAT`).

- A file of a format the GUI knows reads the same in it and in every later GUI: each key
  keeps its meaning, and each value reads to the same theme. `tests/gui_themes/<N>/` holds
  files of format N as a theme tool wrote them, with the theme each reads to (`<name>.json`,
  the GUI's fields by key); `gui_theme_formats` checks them, and `gui_theme_write` checks
  that each, written again, reads to the same theme.
  They are never changed: a field added to themes later is simply not in an older one's JSON.
- A new key, or a new word for a key, keeps N: an older GUI leaves what it does not know, and
  the base's value stays. A new key's value in the built-ins must keep an older file looking
  as it did.
- N goes up only when a key that exists has to mean something else. The GUI then reads the
  older format's files with the older meaning, their fixtures stay, and the new format's go
  in `tests/gui_themes/<N+1>/`.
- A file of a newer format than the GUI's: the keys it knows still apply, the others are left.
- The theme editor writes every key and the newest format it knows (`gui_theme_write`), so its
  files never lean on a built-in's values, which may change. It reads an older file as that
  format reads, and writes it in the current one.

## Building and testing

The NX build enables `SNX_BUILD_GUI` by default. Disable it with
`-DSNX_BUILD_GUI=OFF`. Tools-only builds do not enable the GUI.
The GUI itself needs only SDL3, not Vulkan or FFmpeg. The driver binaries can be
built separately and installed beside it or made available on PATH.

To build it alongside the NX runtime (which supports `-H` for no-window runs):

```sh
cmake -S . -B build/gui -DSNX_DEPLOY=OFF -DSNX_BUILD_GUI=ON
cmake --build build/gui
ctest --test-dir build/gui -R '^gui_' --output-on-failure
```

For just the GUI and Linux runtime, use `-DSNX_BUILD_NX=OFF -DSNX_BUILD_GUI=ON`.
Tools-only and hactool-only builds cannot enable the GUI.
Supported host platforms are Linux, FreeBSD, Windows (llvm-mingw) and Android (`dist/android`).
SDL3 handles window creation, keyboard/mouse input, rendering, clipboard and drag-and-drop
on every host. The platform-specific code only handles child processes and resource
accounting. See [BUILD](BUILD.md) for SDL3's desktop dependencies.

The tests cover menu transitions, bounded console scrolling, split UTF-8/ANSI
sequences, path parsing, argument quoting, inherited grandchild output, resource
accounting and process-tree cleanup. Browser tests cover filtering, missing/empty
directories, pagination, resizing, Back/ESC and launching selected files.
Key setup tests cover installation, replacement, invalid drops, failed writes,
self-copy safety, cancellation, and the bright-red overwrite warning.
Setting tests cover ON/OFF argument combinations, backend availability, mouse
hover versus explicit activation, persistent submenus, yellow/pink highlighting
and footer descriptions. Theme tests cover the built-ins, a `theme.ini` over its base
(unreadable values and unknown keys included), the `themes/` folders listed, the
compiling and loading screens, and the YoRHa bars and band in the real renderer. Step
tests cover each driver and runner line, the other lines kept under their step, lines
split across reads, the progress and how a run ends; the renderer tests check the menu
fading out into a run, the loading screen, its typing, detailed and collapsed steps,
texture and fades at a fixed clock.
SDL tests use the dummy video driver and the
software renderer, exercising the real font and event handlers without a display.
The font's provenance and public-domain notice are in
[`src/gui/FONT-NOTICE.txt`](../src/gui/FONT-NOTICE.txt).
