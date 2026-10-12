---
name: swr-run
description: "Run a recompiled game unattended and judge the result: scripted button input, timed exit, screenshots, reading the log, and repeat runs for intermittent crashes. Use when verifying a runtime change, reproducing a bug, checking a scene renders, measuring a crash rate, or when the user asks to run or test the game."
---

# Run the game

## One command

```bash
tools/run_game.py Catherine --dump work/Catherine                 # 60 s, Catherine boot to the New Game cutscene
tools/run_game.py Catherine --runs 5 --dump work/Catherine        # crash rate
tools/run_game.py Catherine --seconds 20 --script "" --out "$SCRATCH/boot"  # boot only, no input
```

`$SCRATCH` stands for the session's scratchpad directory: keep `--out` there, never in `/tmp` or the repo (without
`--out` it makes a new folder in `/tmp`). It runs the game from `games/`, writes `runN.log` and `runN.png` (the last
frame, converted from the runtime's `.ppm` when Pillow is installed, otherwise `runN.ppm` stays and it says so once)
per run, and prints one line per run: the exit code (or `timeout`), then `ok`, the crash site (the signal from the
`*** Crash` header and the faulting thread's `pc`/`lr` after it, named with `--dump`) or the `svcBreak` reason, then
the frame count with the average FPS over `--seconds`. Options it doesn't know (`--fps_limit=0`, `-v`...) go to the
runtime. Look at the picture with the Read tool: a frame that "rendered" can still be black or garbage, and only the
picture shows it.

The runtime options behind it, for a run by hand from `games/`:

| Option | Effect |
| --- | --- |
| `--seconds=N` | Exit after N seconds, printing `frames=N hash=...` (the last frame's hash) and writing `--shot` |
| `--frames=N` | Exit after N presented frames |
| `--shot=PATH` | Screenshot to `PATH.ppm` at exit |
| `--shot_every=N` | Also a numbered screenshot every N frames (`PATH_0000.ppm`, ...), needs `--shot` |
| `--script=ms:hex,...` | Press buttons at those times, each held 300 ms (`--press_len=ms` changes it) |
| `--press=hex --press_at=ms` | One press, held `--press_len` ms too |
| `-v` | Log every IPC call (`<service> cmd N (in ...) -> rc`): huge, grep it |
| `--fps_limit=0` | Unlimited frame rate |
| `--game=PATH` | Run the game PATH names (`games/Catherine` runs `games/Catherine_code.so`) instead of the one the runtime is named after. Caches, captures and logs still go next to the game |

The timing, screenshot, press and script options are `platform/unattended.c`'s, shared by the SDL3 and headless
backends, so a headless build takes `--script` too. With `--seconds` or `--frames` the window doesn't go fullscreen. The SDL3 backend runs on Wayland
when the desktop has it. `SDL_VIDEO_DRIVER=x11` runs it on X11 instead: test a platform change on both.

Buttons (hex): A 1, B 2, X 4, Y 8, L 40, R 80, ZL 100, ZR 200, Plus 400, Minus 800, Left 1000, Up 2000, Right 4000,
Down 8000. Combine by adding. The values are 64-bit, and bits 40 to 47 push the sticks all the way: 40 left stick
left (`10000000000`), 41 up, 42 right, 43 down, then 44 to 47 the same for the right stick (`800000000000` is right
stick down).

Catherine's New Game script (`run_game.py`'s default): Plus at 15 s skips the intro movie, A at 16 s, Plus at 18 s,
then A at 21, 31, 36, 40 and 44 s through the menus and prompts. With save data from earlier runs the screens come
in a different order, so check the screenshots before trusting a script.

## Reading the log

`games/<Game>.log` (and the run log) grows to megabytes. Never print it whole.

- `grep -n "Crash\|svcBreak\|FATAL" run1.log`: the failure, then `sed -n` around it.
- The log has no FPS line. `frames=N` at the end of a `--seconds` run, divided by the seconds, is the average frame
  rate (`run_game.py` prints it).
- `grep "vulkan:" run1.log | tail -3`: every 600 presents, the draws and skipped draws, compute dispatches and
  batches with the time spent waiting on them, and skipped dispatches. `WARN` lines with `vulkan: shader ... didn't
  compile` or `untranslated instructions` are rendering bugs.
- `video:` lines: NVDEC and VIC jobs. `romfs:` lines: files the game opened (movies are `.usm`).
- `games/threads.txt`: every guest thread's state and wait, rewritten every 15 s. A hang is read from here.
- `diag=1` in the game's `.ini` saves the QMD and program of each new compute shader (the first 32) to
  `games/captures/compute/`. F5 writes every game thread's state to the log at any time.

Crash reports are handled with the `swr-crash` skill.

## Intermittent bugs

- One clean run proves nothing. Measure the rate before the fix with the same script and run count, then after, and
  put both in the commit message (for example "crashed 4 of 6 runs before, 0 of 20 after").
- Save data from earlier runs (`games/<Game>_data/save/`) changes what the game does at boot. A rate measured with
  one save state is not comparable with one measured with another. To start fresh, move the save folder aside
  (outside `games/`), never delete it without asking.
- Don't build or run anything heavy while measuring: timing changes the rate.
- Runs are sequential. Two games at once share the GPU and the saves.

## Stopping a game

`pgrep -a Catherine`, then `kill PID`. Never `pkill`, which matches the shell running it.
