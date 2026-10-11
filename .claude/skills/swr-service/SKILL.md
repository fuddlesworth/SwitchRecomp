---
name: swr-service
description: "Implement or fix a high-level-emulated Switch service or device (IPC commands, nvdrv ioctls, shared memory) so it answers the way the console does, as the game's SDK code expects. Use when a log shows an unimplemented command, a game stalls waiting on a service, an svcBreak follows a service call, or when adding audio, input, video or system services."
---

# Services

## Where

- `runtime/ipc.c`: dispatch and the helpers: `in32(c, off)`/`in64` read the request's raw data, `out32`/`out64`/
  `out8` append to the reply, `buf_in`/`buf_out` give a buffer of either kind (A or X in, B or C out, which "Auto"
  commands mix), `out_obj` returns a new session object, `out_copy_h`/`out_move_h` return handles.
- `runtime/svc_core.c`: most services (applet, audio, fs helpers, time, set, ...), one `*_disp` per service and one
  per interface object, registered with `svc_register("name", ctor)`.
- `svc_hid.c` (input and its shared memory), `svc_fs.c`, `svc_vi.c` (display, binder), `nvdrv.c` (nvhost devices),
  `host1x.c` (NVDEC, VIC), `main.c` (`audren_update`).
- `svcobj_new("IName", dispatch, sizeof(State))` gives an object with zeroed state in `o->data`.

## Procedure

1. **Find the command.** `-v` logs every call: `grep "IAudioOut cmd" run1.log | sort | uniq -c`. The command
   number and the in-data are there. Switchbrew's page for the service gives the command's name and layout.
2. **Read the caller.** The game's SDK wrapper (`nn::audio::...`, `nn::hid::...`) shows what it reads from the
   reply and what it does with it. `tools/symbols.py` and `tools/disasm.py` on the wrapper, or the `sdk-scout` agent.
   A field the SDK reads and the runtime never writes is a bug even when nothing crashes yet.
3. **Answer like the console.** Same result codes, same output layout, same handle and event behavior. Where the
   console times something (audio buffers, vsync), keep the timing: games sync against it.
4. **Mind the threads.** Service calls come from game threads, which run in parallel on the host. State shared
   between calls takes a lock (`PMutex`, `pm_lock`), events go through `kevent_signal`.
5. **Verify** with the scene that uses it (`swr-run`), and grep the `-v` log to see the new answers.

## References and licensing

Switchbrew documents commands and structures. Ryujinx (MIT) is the best behavioral reference and can be ported
from with its notice. yuzu and its forks are GPL: read to understand, never copy or paraphrase closely.

## Known traps

- Reporting more than one npad style for a controller makes the SDK assert in `GetVibrationDeviceHandles`.
- An `IAudioOut` that never releases buffers stalls CRI movies after a few frames. The released-buffer array must
  be cleared first, because the SDK reads every slot.
- `WaitSynchronization` with zero handles is legal (a plain sleep).
- nvhost syncpoint ids above 127 overflow NvRmStream's tables: allocate with `syncpt_alloc()`.
