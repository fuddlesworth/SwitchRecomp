<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# GUI launcher

## Index

* [Games: the games of games/ and the player's folders](#games-the-games-of-games-and-the-players-folders)
* [Output and resource use](#output-and-resource-use)
* [Themes](#themes)
  * [Theme formats](#theme-formats)
* [Building and testing](#building-and-testing)

`scarletnx` started with no arguments (or a model's name, or the AppImage), with no game of its
name beside it, opens its launcher: a small SDL3 menu for the command-line drivers. A copy named
after a game, without its code, says so and never opens it. It uses an
embedded public-domain 8x16 VGA BIOS font, nearest-neighbor integer scaling, and
English text only. No font files or localization packages are needed.

```sh
./build/scarletnx
```

The window's title tells which build it is, as `ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)`, for example
`ScarletNX main 43bf71a (clang 22.1.8, Linux x86_64)`; the commit ends in `-dirty` when the tree had changes, a
build of a detached checkout outside CI says `detached`, and both read `unknown` outside a git checkout. A game's window carries the same line after the game's name.

It opens on a splash: Wimsy, or Fuddles (`dist/splash/`), picked at random each start, a third of the window's
shorter side on the theme's `[splash] page` for its `ms`; a key, a click or a finger ends it. The drawings are SVGs
built in, drawn at the window's size ([lunasvg](https://github.com/sammycage/lunasvg)).

- Up/Down and Left/Right move the yellow focus, wrapping at the ends.
- Tab and Shift+Tab go round the menu, Games and the Settings, the tabs at the top; a click on a tab
  opens it. A theme edited and not saved keeps its screen.
- A list with rows out of sight (the menu with big letters, `games/`'s pages, the Settings, the theme
  editor) shows an arrow past the right edge of its boxes, up on its first row and down on its last, in
  the soft colour.
- Moving the mouse over an item focuses it in yellow without activating it.
- The keys at the foot are buttons: a click or a tap on one presses it, as the keyboard does (ESC or
  Back goes back, Tab to the next screen, Enter or Tap selects, Swipe across turns the page). A key of
  words between slashes presses the word clicked, `Left` or `Right` of `Left/Right`; the text after it
  presses the key when its words all press one. `Arrows`, `mouse` and `pad` press nothing.
- The menu, top to bottom: the three loads, **Setup device keys**, **Settings**, **Share log**, on
  Android **Open data folder**, and **Exit**. The games, and the recent ones, are the Games tab's.
- Space, Enter or a left click explicitly activates an item, marking it bright
  pink. Activation is tracked separately from hover/focus, so the activated item
  stays pink while another item is hovered in yellow. Mouse controls also work
  on Games' rows.
- A finger taps an item anywhere on its row, as a click does on its label; a swipe across
  turns the pages of `games/`; a tap on a finished run returns to the menu, and one on a
  running run's ESC line aborts it. Android's back button is ESC. On a screen packed with
  pixels (a phone) the text takes the display's own scale, as far as the menu still fits.
- **Load Switch NSP/XCI/NRO**, **Load Linux ARM ELF** and **Load 3DS CIA** open a screen as the
  keys': a click on its blinking **[ Pick or drop a file ]**, or Enter, opens the system's file
  picker (on Linux the desktop's, else zenity's). Dropping a file on the window and Ctrl+V with
  a path work too. ESC returns to the menu. On Android the picker opens at once. A dump not extracted yet takes
  about its size for the extraction and, for a Switch game, as much again for the RomFS its folder gets; when its
  disks have less free, the menu says how much it needs and *Run anyway* takes a second press.
- **Load 3DS CIA** uses `scarletnx-3ds`. Its `games/` browser lists CIA inputs,
  not host executables. The driver extracts supported decrypted application CIAs.
  A native Linux build with `SNX_3DS_AOT=ON` then performs static ARM11 integer
  compilation/execution; the default build reports missing AOT support. Larger
  images use bounded conservative discovery (16 MiB text cap). SuperTux now
  connects/registers `srv:`, then waits for absent `APT:S`, before main,
  graphics or audio. No service/session success is invented to bypass that wait.
  Full HLE/devices remain incomplete. Like the Switch driver, it uses an
  extraction and a compiled module already there, and recompiles only when needed.
  See [3DS port](3DS.md) for limits.
- **Setup device keys**, immediately after the three load entries, blinks red (the theme's `failure` colour, at its
  `blink_ms`) while no `prod.keys` is in use. It prompts you to
  pick `prod.keys` (a click on the blinking **[ Pick or drop prod.keys ]** in the middle of
  the screen, the theme's `[prompt]`, or Enter), drop it, or paste its path with Ctrl+V. It installs the file to `~/.switch/prod.keys`, or
  `%USERPROFILE%\.switch\prod.keys` on Windows; on Linux, in the user's data folder, it puts it there
  instead (`~/.local/share/scarletnx/prod.keys`), which the driver reads before
  `~/.switch`, and names the keys of `~/.switch` it would use until then. It creates the
  folder if needed. Existing keys trigger the bright-red warning
  **Keys already setup. This will overwrite the previous ones**. The screen then says what the try
  did, *Installed* or *Not installed* and why, and goes back to the menu by itself after 3 seconds
  (10 after a failure, while another file can still be picked); ESC cancels without changing them.
- **Settings** opens on its groups, Enter opening one and ESC coming back: **App**, then
  **Launch** (the launcher's two), then a group per section of the game's `.ini` (Display,
  Graphics, Performance, On-screen controls, Controllers, Network, Debug), as `scarletnx --settings`
  prints them from the runtime's own tables. The game's menu's settings have its names and words
  (*Scale* 100%, *Touch screen* Fingers), and its **Buttons** page, whose **All** steps every row;
  *Edit layout* and *Reset layout* stay in the game's menu, as the layout is each game's own.
  Controllers' **Button map** draws the pad as the game's menu does: each control in a box down either
  side with the console button it sends (where the console's is, unless the player's map says otherwise, A and B
  swapped when the group's **Swap A/B** is on), and what the focus does at the bottom. Up/Down move, Left/Right
  change a control's button and Enter (or the pad's A) waits for a press on the pad, which takes it; a change keeps
  the whole map as `gamepad_map`, and Del puts it back to the `.ini`'s. The first pad plugged in is shown, its held
  buttons lit; with none, the middle says why (a joystick SDL has no mapping for is named, with its GUID and the
  file its line goes in). The launcher reads pads only on this page.
  Controllers' **Player** row, first, goes round *All* and 1 to 8, as Eden's tabs (Left/Right or Enter): *All*
  shows the settings every player shares; a player its own, *Controller*, *Connected*, *Type*, *Swap A/B*,
  *Vibration* and *Button map*, each *As All* until picked, then the rows no player has its own of (the
  dead zone, background input, Joy-Cons). A player's *Controller* is *Any*, the next to connect, until Enter on it
  and a press on a controller give it that one (ESC leaves it as it was, Del makes it *Any* again); its *Button
  map* is the page above on its own keys, its A and B as its *Swap A/B* says.
  **App** holds **Theme** and **Text size**, which Left/Right or Enter go round either way, **Theme editor**, which comes back to App, and **Game folders**: `games/`,
  always listed, then the player's own folders, whose games the `games/` browser lists after
  `games/`'s. **Add a folder** opens the system's folder picker; a folder dropped on the window
  or a path pasted with Ctrl+V is added too. A folder with folders in it asks first: *Only this
  folder*, or *With subfolders*, whose games are listed too, four levels down, hidden folders
  left alone (its row says *+ subfolders*). Del, or Enter twice, takes one off the list (the
  folder stays). They are kept in `folders.txt` beside `settings.ini`, a line each, `/**` after
  one kept with its subfolders; one that cannot be read is named when the games are listed. **Folders** has a row for
  the data folder (**Data**: `games/`, `themes/` and `prod.keys`), **Games** and **Extractions**, each its default or a
  folder picked. Enter opens the folder picker, a folder dropped or pasted with Ctrl+V takes the focused row, Del puts
  its default back and the last row, **All back to the defaults**, all three. A row says *default*, *picked* or why
  its folder cannot be; the line under the rows names the folder in use, the one above its disk's free space. A folder
  not there, not a folder, that cannot be written or, for the games, on a disk that runs no programs (`noexec`) is
  refused, and a run whose picked folder no longer can be stops, naming it: the default never takes its place. On
  Android a folder outside the app's own storage is asked about first, since Android checks every file access there
  and games may run slower: *Use it* takes a second tap, and its row says *outside the app*.
  Nothing is moved, the folder before keeps what it has. The menu's own files stay where they are. **Clean old settings**, Enter twice (the first says what it
  would do), takes out the settings nothing reads any more: the `[Deprecated]` section the runtime
  gives a game's `.ini` for the keys it no longer uses (in the config folder's `games/`, in `games/`
  and in the player's folders), the keys of `settings.ini` that no setting of the runtime's takes
  (only while the runtime lists its settings) and those of `app.ini` the menu does not know. A box
  over the screen then lists each file and what was done to it, or why not, until a key closes it;
  Share log lists them too. A group of settings shows
  a row each, its help above, Left/Right going round its values, Enter typing one. A setting not picked shows the runtime's default and `[default]` (*Off [default]*):
  the run takes the game's `.ini`'s, which starts as that; one picked goes to every
  Switch run as `--key=value`, over the `.ini` ([USAGE](USAGE.md)), and is kept for every game:
  what was changed, by the `.ini`'s sections and keys, in `settings.ini` of the config folder on
  Linux (`~/.config/scarletnx/`), else beside `recent.txt`. A run takes a setting from what that
  run picks, then from the game's own picks (to come), then from these, and last from the game's
  `.ini`. Del puts a setting back to its default, and the groups' last row, **Reset all to the
  defaults**, every one. The launcher's own choices start picked: `fullscreen` 0, `controls` 0 (on Android 1,
  and `fullscreen` puts the launcher itself on the whole screen too).
  **libgccjit** passes `--backend=gcc` when preparing a dump (it stays off in a build without
  libgccjit; a game of `games/` another build made is recompiled with LLVM), **gdbstub** passes `-S` and `-s`, the
  runtime's stub on port 1234 waiting for the debugger at startup (UNIX only). None of them
  reaches the Linux or the 3DS driver.
- The Status box, after the RAM and the CPU, says what ScarletNX's folders take (games/,
  work/ and themes/ of the data folder, and on Linux its config, state and cache folders, else
  the menu's own) and what is left on that disk, counted as the menu opens and after each run.
- Settings > App's **Theme** switches to the next theme, or the one before: the built-in Default
  and YoRHa, then each folder of `themes/` (see [Themes](#themes)). The menu opens in YoRHa until one is picked.
  It is kept for the next start,
  as is Text size, in `app.ini` beside `settings.ini`: `[app]` with `theme` (a theme's folder
  name) and `text_size` (`auto` or 1 to 4), and once one is picked `[folders]` with Folders' `data`, `games` and
  `work` (a full path, nothing for the default). A value the
  menu does not know, or a theme no longer there, is named in the menu's message as it opens and
  left out; a file that cannot be written is named in App's note.
- Settings > App's **Theme editor** edits the current theme: **Save** first, the folder it is saved in, a row
  per `theme.ini` key, and beside them a
  preview of the menu or a run (Ctrl+P, or a click on it, switches). Left/Right change a word, a switch or a
  number, Enter types a text or a number and opens a colour's picker. The picker sits over the keys, so the preview
  shows the colour as it changes: a bar per channel, R, G and B, which Left/Right step by 1 and PgUp/PgDn by 16, or
  a click, a drag or a finger sets, then the colour as `#RRGGBB`, which Enter there types. Enter keeps the colour, as
  does a click or a tap off the picker; ESC (Back on Android) puts back the one it had.
  Save (or Ctrl+S) writes `themes/<folder>/theme.ini` in the data folder and switches to it;
  a built-in is saved as a new theme, `mytheme` until renamed. ESC leaves, asking first if
  there are unsaved changes.
- Settings > App's **Text size** steps the menu's letters through Auto (fit the screen) and four
  sizes. Bigger letters scroll the menu. The foot's letters (its keys, and the tips when they are
  there) are a size under the rest's, 1x at the least.
- **Share log** saves the last run's output and game log to `scarletnx.log`. On the
  desktop it goes in the working folder with its path copied. On Android it goes to the
  data folder's `state/` and is shared.
- **Open data folder**, on Android, opens the data folder, the app's own storage, in the
  system's Files app ([ANDROID](ANDROID.md)).
- **Exit** closes the launcher. Closing its window during a run terminates the
  entire owned process tree, so let the game exit normally to preserve its saves.
- As it opens, the menu shows in a box what an older version left where this one no longer looks, a
  folder at a time: where it is (`- `, in the failure colour, a path too long for a row going on in the next) and
  under it the space it takes, counted as the box shows (*counting* until then). They are the menu's `gui/`, `work/`, each game laid out
  beside the others in `games/` or a game folder (`games/<id>/` now), a portable copy's `games/`
  beside the program, and a build tree's `games/`, `work/` and keys in its checkout
  ([USAGE](USAGE.md#folders)). Any key, click or tap closes it; nothing is moved, and it shows again at the next
  start for as long as they are there.
  **Share log** lists them all, each with where it goes now.
- **Recent games**, in Games under Add a folder, lists the last games run, newest first, three and a row that
  shows the rest, by the name their run gave (the runner's `ScarletNX - <name>` line), else their file's.
  The focus moves between them, Add a folder and the page's games in one ring, and the tips show the focused
  game's path. Choosing one runs it again with the driver it was
  loaded with and the options the menu has now; a game whose file is gone leaves the
  list instead. The list is kept in `recent.txt`, beside `last-run.log`, at the root of the menu's state
  folder ([USAGE](USAGE.md#folders)): `$XDG_STATE_HOME/scarletnx/` on Linux, by default
  `~/.local/state/scarletnx/`, or `%APPDATA%\scarletnx\state\` on Windows. An older version's, in a `gui/`
  folder, stay where they are. A line per
  game: `switch`, `linux` or `3ds`, `file` or `games` (a program from games/, run as it is),
  the path and the name, separated by tabs. A list that cannot be written there is named in the
  message, after a run's end too, until it can be.

The launcher runs Switch inputs with the `scarletnx` it is part of, and looks beside it, then on
PATH, for `scarletnx-gnuabi` or `scarletnx-3ds`. Files are passed directly
as argv entries, never through a shell. Spaces, Unicode paths and shell punctuation
are not evaluated. Local `file://` text drops are decoded, but lists and remote URI
authorities are rejected. The working directory is not changed and is shown in the
Status box. See [GNUABI](GNUABI.md) for supported Linux ELF applications.

## Games: the games of games/ and the player's folders

**Games**, the tab, lists the games: the ones ready to run (a recompiled game runs directly), the dumps to prepare
on Android, and CIA files, which go to the 3DS driver. It reads the `games/` of the
folder the driver works in, which the menu passes it as `--repo`: a portable copy's `.scarletnx/` beside the
AppImage (or the GUI), else the user's data folder, a build tree's menu's too ([USAGE](USAGE.md#folders)), then each
folder of Settings > App > Game folders. Each game is shown by the name in its `control.nacp`, which the recompile copies into
its data, else by its folder's or file's name, sorted by it in each folder. Only immediate files are listed:
executables (execute permission on UNIX, `.exe` on Windows) and `.cia` files (case-insensitive,
no execute permission needed), which run with the 3DS driver. Hidden files, shared libraries and
directories are excluded. The folders are looked at once a second while the menu is open (their
names only), so a game put in or taken out shows in the open list; a folder that
cannot be read is named and the others still listed.

- **Add a folder** is the first row on every page, a wider gap under it: it opens the folder picker of Settings >
  App > Game folders. Under it the recent games, then the page's games. ESC returns to the menu.
- Up/Down go round Add a folder, the recent games and the page's files. Enter/Space opens one.
- **R** (on Android a tap on *Recompile* at the foot) recompiles the Switch game in focus, recent or on the page,
  where it is, or prepares its dump again: `scarletnx -r`, with libgccjit when **libgccjit** is on. It plays nothing;
  Enter then does. On another row, or a 3DS game, it says why it does nothing.
- Left/Right and PgUp/PgDown go to the previous/next page. The page number and
  total file count are shown. Paging stops at the first and last pages.
- Resizing adjusts the page size while keeping the selected file visible.
- Empty or missing directories show a diagnostic and still provide Add a folder.

A recompiled game, in a folder of its own (`games/<id>/<id>`), runs with the Settings through the menu's own
`scarletnx` and is never extracted as a dump: it plays as it is when this build made it, and is recompiled first,
where it is, when another build did ([USAGE](USAGE.md#extract--aot--run)), the run showing the recompile before the
game loads. A game an older version laid out beside the others runs directly, as every game does on iOS. A CIA goes
to `scarletnx-3ds`, which is never executed on it directly. Output capture, resource accounting and the return to
the main menu work as for a file picked by a load.

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

The Status box samples combined GUI and descendant resource use every half second:

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

A theme colours and dresses the screens: the head (the tabs and a title), the boxes (the list, the box
beside it about the focus, the Status box: resource use, the disk and the working folder), the tips (what the
focus is about or the screen's message) and the foot (the screen's keys). It applies to the menu screens
and to the run, which is **compiling** until the runner prints its `ScarletNX - <name>` banner and
**loading** from then on.

The screens are boxes side by side, nothing over another, the default framing them in thin lines and YoRHa
filling them. At the top the tabs, Menu, Games and Settings, and the screen's title after them against the
right (`[head] place = tabs`) or on a row of its own under them (`below`). Under them the list, each entry a box
of its own, the focused one framed in the focus colour or inverted, with the theme's cursor in the margin;
beside it a box with what the focus is about (a recent game's path, Games' folders, the keys in use, the last
run's output) and under that one the Status box. The tips have a row of their own under the head (`[tips] place =
top`, both built-ins'), or go to the foot under the list's column with `bottom`, where they take one row, a long one
sliding, or wrap over its rows with `[tips] wrap`. At the foot the screen's keys, the whole width, or under the side
box's column beside the tips, in letters a size under the rest's; the tips and the keys each in a box with a bar down
its left (`[tips] box`, `[footer] box`) or on the page. Games lists Add a folder, the recent games and a
page of the games, as many as the list's box holds, the focused one's path and kind beside them; the keys'
screen, and a load's, is one box with where the keys go, the waiting line in its middle (a click in the box
opens the picker) and Back at its foot. The Settings list their groups, and beside them the focused group's
rows: Enter, or a click on a row, opens the group, its rows taking the focus under a Back that a click or a finger
takes as ESC, the group shown as the focused one; a click on another group opens that one. The theme editor lists its keys, the preview beside them. The list's column is two fifths of
every screen (`[body] sizing = fixed`), and a window too narrow for both shows the list alone; with `adaptive` the
column is as wide as its entries and the side box goes under the list. A text too long for its box wraps over
its rows, or takes one that slides with `[body] text = slide`. A run of the console layout shows the driver's output in a box sized to it,
beside it what runs and how it ended over the status, the file in the tips and the ESC line where the keys
go, which a tap aborts. The button map draws the pad in the box between the head and the foot. A question, or
the files an older version left, comes in a box over the screen darkened.

The built-ins are `default`, the look above, and `yorha`: paper and ink, a title twice the text's size, a faint
grid on the page, square markers, the focused box inverted, the tips in a box under the head, the keys in one on a
band of ink at the foot, and a run drawn as a
dark loading screen in its own colours (`[run]`): the title, the game's name with turning
dots and a turning ring; each step the driver and the runner report (unpack, extract,
recompile with its blocks, built, launch, GPU) typed down the page, with turning dots
while it runs and `...OK` or `...FAILED` once it ends, the cursor blinking on the current
one, and the other lines it printed indented under it (`[run] bar` adds a progress bar
with the running step's percentage). A step that counts, the unpack and the extraction by
what hactool has written, the recompile by its blocks, shows how far it is on its line as
`[run] progress` says: `amount` (`1.2 / 4.0 GB`, `3 / 6`), `percent` (`30%`, YoRHa's) or
`time`, what is left at its pace so far (`01:23 left`). With `layout = console` the
driver's own row says it, `unpacked 1229 of 4096 MB (30%)`, written over itself each second. The menu fades out to black as a run starts and the run fades in from it; ESC
fades the run out and the menu back in.

A theme is also a folder holding a `theme.ini`, the folder's name its id, in
`themes/` in that same folder or in `themes/` beside the GUI; the first one found
wins, and a folder named like a built-in replaces it. The built-ins are such files, and
nothing else: each folder of `src/gui/themes/` is one, its `theme.ini` compiled into the GUI
(`cmake/gui_themes.cmake`) and read as any other, setting every key. The runtime's game menu
takes YoRHa's colours from the same file at build time. The files are copied beside the GUI
in the build and installed in `share/scarletnx/themes`: copy one under a new name to make a theme, or edit one
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
; #RRGGBB. soft: hints, secondary text, rules. focus: the focused box, or its band.
; dim: the boxes' frames, a filled box, the grid and the title's shadow
page = #101418
ink = #D8D4C4
soft = #8A8676
focus = #D8D4C4
active = #C8A040
failure = #D05040
dim = #3E3E40
; a diff's added lines, as failure its removed ones; no screen uses it yet
added = #6A9A50
[head]
; plain | bar: the tabs and the title on a band of ink; rule: a line under the head
style = bar
rule = true
; the title after the tabs (tabs) or on a row of its own under them (below)
place = tabs
; the title's letters, times the text's (1 to 3), and a shadow under them in the dim colour
size = 2
shadow = true
; the title per screen: menu, games, keys, compiling, loading
menu = NIGHT  /  MENU
[body]
; before each item; quotes keep spaces
marker = "> "
; the focused box: color | bar
focus = bar
; the boxes outline (framed) or fill (filled)
panels = fill
; fixed: the list two fifths of every screen, alone on a narrow one | adaptive: as wide as its entries
sizing = fixed
; a text too long for its box: wrap (over its rows) | slide (on one row)
text = wrap
; in the list's margin, beside the focused box
cursor = "> "
[backdrop]
; a faint grid on the page behind the boxes, in the dim colour
grid = true
[tips]
; what the focus is about: bottom (a box at the foot beside the keys) | top (a row under the head)
place = top
; in a box with a bar at its left, or on the page
box = false
; over the foot's rows, or on one row that slides
wrap = false
[footer]
; plain | bar: on a band of ink | none: no Status box; rule: a line over the foot
style = plain
rule = true
; the keys in a box with a bar at its left, or on the page
box = true
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
; a running step's progress: amount | percent | time
progress = percent
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
; the boot's: one of Wimsy's drawings, at random, on page for ms milliseconds (0: none). chance: the
; boots in 100 that show this theme's, the default theme's the others
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
  keeps its meaning, and each value reads to the same theme.
- A new key, or a new word for a key, keeps N: an older GUI leaves what it does not know, and
  a file that does not name it takes its base's value, so an older theme gets the new look of
  the built-in it starts from (the title's place, the tips, the foot's boxes).
- A key a newer one replaces is deprecated, never given a new meaning: it still reads as the new keys say, and the
  GUI never writes it again. The format has none so far.
- N goes up only when a key that exists has to mean something else. The GUI then reads the
  older format's files with the older meaning.
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

For just the GUI and Linux runtime, use `-DSNX_BUILD_NX=OFF -DSNX_BUILD_GUI=ON`: the launcher is
then `scarletnx-gui`, a program of its own, which looks for `scarletnx` as it does for the other drivers.
Tools-only and hactool-only builds cannot enable the GUI.
Supported host platforms are Linux, FreeBSD, Windows (llvm-mingw) and Android (`dist/android`).
SDL3 handles window creation, keyboard/mouse input, rendering, clipboard and drag-and-drop
on every host. The platform-specific code only handles child processes and resource
accounting. See [BUILD](BUILD.md) for SDL3's desktop dependencies.

The tests cover menu transitions, bounded console scrolling, split UTF-8/ANSI
sequences, path parsing, argument quoting, inherited grandchild output, resource
accounting and process-tree cleanup. Browser tests cover filtering, missing/empty
directories, pagination, resizing, Back/ESC, launching selected files and R's recompile.
Key setup tests cover installation, replacement, invalid drops, failed writes,
self-copy safety, cancellation, and the bright-red overwrite warning.
Settings tests cover the runtime's list read into rows, picks, the options each combination
passes, and the screens of its groups and App; menu tests cover mouse hover versus explicit
activation, the menu without the games and Games with Add a folder and the recent games' show-more row,
yellow/pink highlighting and the tips. Theme tests cover the built-ins, a `theme.ini` over its base
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
