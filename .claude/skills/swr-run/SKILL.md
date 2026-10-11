---
name: swr-run
description: "Run a recompiled game unattended and judge the result: scripted button input, timed exit, screenshots, reading the log, and repeat runs for intermittent crashes. Use when verifying a runtime change, reproducing a bug, checking a scene renders, measuring a crash rate, or when the user asks to run or test the game."
---

# Run the game

## One command

```bash
tools/run_game.py Catherine --dump work/Catherine                 # 60 s, Catherine boot to the New Game cutscene
tools/run_game.py Catherine --runs 5 --dump work/Catherine        # crash rate
tools/run_game.py Catherine --seconds 20 --script "" --out /tmp/x # boot only, no input
```

It runs the game from `games/`, writes `runN.log` and `runN.png` (the last frame) per run, and prints one line per
run: exit, crash site with symbols, last FPS. Look at the PNG with the Read tool: a frame that "rendered" can still be
black or garbage, and only the picture shows it.

The runtime options behind it, for a run by hand from `games/`:

| Option | Effect |
| --- | --- |
| `--seconds=N` | Exit after N seconds, writing a screenshot |
| `--shot=PATH` | Screenshot to `PATH.ppm` at exit |
| `--shot_every=N` | With `--shot`, also `PATH_0000.ppm`, `PATH_0001.ppm`... every N frames: follows a scripted run through menus |
| `--script=ms:hex,...` | Press buttons at those times, each held 300 ms (`--press_len=` changes it) |
| `-v` | Log every IPC call (`<service> cmd N (in ...) -> rc`): huge, grep it |
| `--fps_limit=0` | Unlimited frame rate |
| `--game=PATH` | Run the game PATH names (`games/Catherine` runs `games/Catherine_code.so`) instead of the one the runtime is named after. Caches, captures and logs still go next to the game |

The SDL3 backend runs on Wayland when the desktop has it. `SDL_VIDEO_DRIVER=x11` runs it on X11 instead: test a
platform change on both.

Buttons (hex): A 1, B 2, X 4, Y 8, L 40, R 80, ZL 100, ZR 200, Plus 400, Minus 800, Left 1000, Up 2000, Right 4000,
Down 8000. Combine by adding.

Catherine's New Game script (`run_game.py`'s default): Plus at 15 s skips the intro movie, A at 16 s, Plus at 18 s,
then A at 21, 31, 36, 40 and 44 s through the menus and prompts. With save data from earlier runs the screens come
in a different order, so check the screenshots before trusting a script.

## Reading the log

`games/<Game>.log` (and the run log) grows to megabytes. Never print it whole.

- `grep -n "Crash\|svcBreak\|FATAL" run1.log`: the failure, then `sed -n` around it.
- `grep "performance:" run1.log | tail -3`: FPS and how busy the GPU thread is (near 100% means GPU-bound).
- `grep "gpu: frames=" run1.log | tail -1`: draws, shader and texture counters. `failed shader` or `no program`
  above 0 is a rendering bug.
- `video:` lines: NVDEC and VIC jobs. `romfs:` lines: files the game opened (movies are `.usm`).
- `games/threads.txt`: every guest thread's state and wait, rewritten every 15 s. A hang is read from here.
- `diag=1` in the game's `.ini` dumps render targets and a frame trace to `games/captures/` at 20 and 40 s.

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
