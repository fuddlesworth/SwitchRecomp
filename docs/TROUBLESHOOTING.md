<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Troubleshooting

The messages a player meets most, as ScarletNX prints them. The driver's messages start with `scarletnx: `.
A game's fatal error opens a box and goes to its log as `FATAL: `.

## Where the logs are

[USAGE: Folders](USAGE.md#folders) lists them: a game's `<id>.log` and `threads.txt`, the menu's `last-run.log`,
and `scarletnx.log`, which the menu's **Share log** saves. An error from before the game's code loads, such as the
two below about a dump and the recompiled code, goes to the terminal and the box only.

## Preparing a dump

### `Encrypted content needs prod.keys. Provide --keys, or put it at <path> or in ~/.switch`

The dump is encrypted and no `prod.keys` was found. The driver looks in its data folder (the `<path>` shown), then
in `~/.switch` (`%USERPROFILE%\.switch` on Windows). Install the keys with the menu's **Setup device keys**, copy
`prod.keys` to one of those places, or extract with `scarletnx -x Game.nsp --keys FILE`. A DXCI or DNSP with
decrypted content needs none.
`Specified prod.keys file is missing` means the file given to `--keys` does not exist.

### `Unsupported container header. Expected NSP/DNSP (PFS0) or XCI/DXCI (HEAD/DXCI)`

The file is not an NSP, XCI, DNSP or DXCI. The header decides, not the extension, so a renamed dump still works and
another kind of file does not.

### `Extracted exefs has no main; verify that the update matches the base game`

The update given with `--update` does not match the base game. Extract again with that game's own update, or none.

### `<path> is a file where this game's folder goes: a game an older version made there, named <id>; move its files as docs/DEPRECATIONS.md says`

An older version put the game's program where its folder goes now. Move its files as
[DEPRECATIONS](DEPRECATIONS.md) says, then prepare the dump again.

## Starting a game

### `<file> is a game dump. ScarletNX does not run dumps: it runs a game recompiled from one.`

A dump was given to `--game`, or to a game's own program. Open the dump without `--game` (`scarletnx Game.nsp`) to
extract, recompile and play it, or point `--game` at the recompiled game, for example `--game=games/Game`.

### `Could not load the game's recompiled code.`

The line before it says why:

- `Cannot find the recompiled code: <path>`: the program runs the game it is named after, from the game's folder.
  Run `games/<id>/<id>` itself, or name the game with `--game=games/<id>`.
- `Cannot load the recompiled code: <path>`: the file is there but does not load. A library it needs is missing,
  or it was built for another system. Recompile it here with `scarletnx -r Game.nsp`.
- `<path> is not a ScarletNX module`: that file is not a recompiled game.

### `The recompiled code is from another ScarletNX version.`

The game was recompiled by an older or newer ScarletNX. Open its dump again: the driver checks the game's `.abi`
stamp and recompiles when it does not match. In the menu, load the dump with **Load Switch NSP/XCI/NRO** or pick
it in **Games**, not the "ready to run" entry, which starts the old program directly. On the command line, run
`scarletnx Game.nsp`. `scarletnx -r Game.nsp` recompiles even when the stamp matches, then stops without playing.

### `Vulkan could not start (see the log). ScarletNX needs a GPU and driver with Vulkan 1.3.`

Most often no GPU offers Vulkan 1.3 with a queue that can present to the window. The log says
`vulkan: no Vulkan 1.3 device with a suitable presentation queue` then. Update the GPU driver. See
[Build prerequisites](BUILD.md#prerequisites). A machine without one can render on the CPU with lavapipe, as in
[USAGE: Headless Vulkan / lavapipe](USAGE.md#headless-vulkan--lavapipe). A headless run (`-H`) says
`Headless Vulkan could not start (see the log). Vulkan 1.3 is required.` for the same cause.

### `SDL could not start its video: <reason>`

SDL could not start its video subsystem, for the reason SDL gives. Without a display server (over SSH, for
example), run with `-H`, which does not start it. On Linux, a build
whose SDL3 lacks the window-system or audio libraries is covered in
[BUILD: SDL3 troubleshooting](BUILD.md#sdl3-troubleshooting).

### `Unsupported instruction <word> at <address>` or `vulkan: the GPU device was lost (driver reset or crash)`

The game did something ScarletNX does not handle yet: an instruction it does not translate, or GPU work the driver
had to reset. Nothing on the player's side fixes it. Save the log with **Share log** and report it.

## Android

### `Pick the file from the device's storage, not from an app (<uri>).`

The picker returned a document with no file behind it, such as one from a cloud app, or a file the app cannot read.
Copy the dump to the device's storage, for example `/sdcard/ScarletNX/games/`, which **Games** lists, and allow
the access to files the app asks for when it starts. See [Android](ANDROID.md).
