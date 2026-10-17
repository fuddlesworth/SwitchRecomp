<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVDEC H.264 packet submission audit

Green video can result from damaged decoding or unwritten/stale YUV surfaces. This
change fixes concrete submission defects in `src/nx/gpu/video/nvdec.c`; without a
failing game's capture it does not establish which defect caused that game's symptom.

## Findings and changes

- The old reusable allocation reserved only eight extra bytes and did not zero the
  packet tail. FFmpeg requires `AV_INPUT_BUFFER_PADDING_SIZE` accessible, zeroed
  bytes beyond `AVPacket.size`. Smaller jobs could expose old payload bytes to the
  bit reader. Use `av_new_packet` and `av_shrink_packet`, giving each packet owned,
  reference-counted storage and zero padding at its actual end.
- `avcodec_send_packet` returning `EAGAIN` means the packet was **not consumed**.
  Previously it was dropped after draining output. Drain and retry the same packet
  once; fail rather than spin if the API still refuses it. Frames captured during
  that drain must not be attributed to the current surface, including second fields.
- Receive errors were discarded. Now only receive-side `EAGAIN` ends a successful
  drain; decode errors and unexpected EOF fail the job. Do not send NULL packets or
  flush between jobs: subsequent P/B pictures need the decoder's reference history.
- SPS/PPS bytes were cached before the decoder accepted the job. Commit the cache
  after successful submission/draining; invalidate it after any failed job so the
  next job resends the parameter sets.
- A `get_buffer2` callback did not mean the job succeeded. Previously rejected
  packets could still publish their captured buffer. Never publish after a send or
  receive error, clear failed-job captures, propagate frame-reference allocation
  failure, and report failed status when no output surface was written.
- Four-byte AVCC lengths of 256..511 also begin with `00 00 01`. Check a complete,
  valid length-prefixed sequence before the Annex B heuristic. Preserve Annex B
  leading zeros, validate bare NAL headers, and reject invalid/truncated prefixes
  instead of blindly adding a start code. One-/two-byte AVCC lengths are not added.

These rules follow FFmpeg's [send/receive API contract](https://ffmpeg.org/doxygen/9.0/group__lavc__encdec.html)
and [packet API](https://ffmpeg.org/doxygen/9.0/group__lavc__packet.html).

## Tests

`nvdec_packets` uses the production implementation with a small, isolated FFmpeg
API double. It covers packet bytes/padding, shorter consecutive packets, ambiguous
lengths, malformed inputs, allocation failures, backpressure, bounded retry, receive
errors/EOF, header-cache recovery, stale callbacks/fields and surface failure status.
The double's headers are private to that target, never used by the runtime.

```sh
cmake -S . -B build/packet-tests -DSNX_TOOLS_ONLY=ON -DSNX_TEST_SANITIZERS=ON
cmake --build build/packet-tests --target snx-test-nvdec-packets
ctest --test-dir build/packet-tests -R '^nvdec_packets$' --output-on-failure
```

`nvdec_decode` builds against real FFmpeg when `SNX_TOOLS_ONLY=OFF`.
It decodes synthetic 16x16 I_PCM and reference-dependent
P-skip pictures, checks exact luma/interleaved chroma values, reuses SPS/PPS and
exercises Annex B, four-byte AVCC and bare-NAL submission. No copyrighted video or
encoder dependency is required. A second pass offers the same pictures to the
machine's Vulkan decode; a driver whose minimum coded extent is above 16x16 (radv's
floor is 64x64) declines them, FFmpeg retries without Vulkan by itself, and the pass
reports that the CPU ran.

```sh
cmake --build build --target snx-test-nvdec-decode
ctest --test-dir build -R '^nvdec_(packets|decode)$' --output-on-failure
```

`nvdec_replay` runs the frontends' synthetic pins anywhere, and replays recorded game
jobs when captures are under `tests/data/nvdec` (CTest reports 77, skip, without
them). With the FE3H VP9 captures it composes the game's 720p stream, decodes it,
pins each picture's luma crc32, and then runs the same stream through the machine's
Vulkan decode, requiring the read-back pictures to be the software decoder's byte for
byte; a machine without a device, or a driver that declines the stream, is told apart
and stays green.

Validation here: 16 tools-only CTests passed with Clang 21.1.5 and ASan/UBSan;
the real-decoder harness passed 2,290 checks against the pinned FFmpeg 9.0.2.
The locally built FFmpeg library itself was not sanitizer-instrumented. A `-no-pie`
executable link flag was needed for ASan's address-space reservation in the sandbox.

## Hardware decoding (Vulkan)

The movie jobs run on the machine's GPU where its driver decodes them: FFmpeg's
Vulkan video decoders for H.264 and VP9 cover AMD, Intel, NVIDIA and mobile drivers
through one backend, and the bundled FFmpeg carries them with their device code. A
machine with no Vulkan device, or one whose driver cannot open the decoder, falls
back to the CPU when the decoder opens, and the log says which happened.
`SNX_NVDEC_CPU=1` forces the CPU path, which capture replays want to come out the
same everywhere.

A driver can also turn a stream down after its device was created - a picture below
its minimum coded extent (radv's floor is 64x64) or a level it does not decode.
FFmpeg's own `ff_get_format` drops Vulkan from the format list and retries when the
hardware start fails, so the decoder instance lands on the CPU before it has decoded
anything: no reference history is replayed, and the log names it ("the Vulkan driver
would not decode %s"). Nothing depends on a mid-stream switch, which a reference
chain could not survive.

A hardware picture arrives as NV12 and is downloaded with `av_hwframe_transfer_data`
into the same block-linear surface writes as the software path's planar 4:2:0, so
surfaces are identical; corruption of a picture the decoder never shows back (an
alt-ref) would stay invisible to the game's own sampled surfaces, but the write is
semaphore-synced by the transfer either way. FFmpeg dlopens the Vulkan loader at run
time, so nothing new is linked and machines without Vulkan run as before.

## Limits

The tests substitute linear surface addressing; they verify publication and NV12
interleaving, not actual GOB layout, VIC conversion or GPU presentation. Actual
interlaced/B-frame game streams, alternate SPS/PPS IDs and unsupported chroma formats
still need capture-based coverage. This patch does not change SPS/PPS reconstruction,
the decode-order `get_buffer2` strategy, or recover reference pictures already lost
to malformed input. Framing autodetection remains a heuristic without explicit
container metadata. A game-level green-screen fix still needs in-game verification.
On the hardware path the VP9 capture replay pins real pictures byte for byte; H.264
there has only synthetic coverage, below the drivers' minimum picture sizes, and
field-coded pairs have had none yet.
