# The AGB board

`scarletnx-agb` is the Game Boy Advance system type. It loads a GBA ROM image
(`.gba`) onto an emulated AGB board, the codename of the console's hardware, and
runs it with the same static recompiler the other system types use. The board
library under `src/agb/` is CPU-only: it links `scarletnx-common` and nothing of
`src/nx/` or `src/gnuabi/`, so the board builds without the Switch runtime's
graphics and media packages.

```sh
cmake -S . -B build/agb -DSNX_BUILD_NX=OFF -DSNX_BUILD_AGB=ON
cmake --build build/agb
build/agb/scarletnx-agb Game.gba
```

`scarletnx-agb` is also a menu entry of `scarletnx-gui` ("Load GBA ROM"), next
to the Switch and Linux loaders.

## Status

The board is under construction, piece by piece:

| Part | State |
| --- | --- |
| Cartridge and ROM header | done: header parsing and validation |
| Memory map | done: every region and mirror, the I/O window as plain storage |
| HLE BIOS interface | mostly done: the software interrupts run as board routines, the waits and sound driver stubs |
| PPU and display | done: the text, affine and bitmap layers, sprites, windows and blending |
| APU (audio) | done: the four PSG channels, the two FIFOs and the stereo mixer |
| Keypad (HID) | not yet: KEYINPUT reads as every button released |
| CPU, DMA, timers, interrupts | done |

A ROM is recompiled when it is loaded, the same process the other system
types use, and runs on the board from its native module.

## The display device

`ppu/ppu.c` owns the LCD controller's registers at 0x04000000 and its timing,
308 dots of four cycles a scanline and 228 scanlines a frame, with the blanking
flags in DISPSTAT and the vblank, hblank and vcount interrupts. Each visible
line is drawn into the 240 by 160 RGBA frame the host presents: the text,
affine and bitmap background layers, the sprites with their affine forms, the
windows, and the colour special effects (alpha blending and brightness). Mosaic
is the one display feature still missing.

## The sound hardware

`apu/apu.c` owns the sound registers and the mixer: the two square channels with
their envelopes, length counters and sweep, the wave channel with its two banks
of four bit digits, the noise channel with its feedback register, and the two
direct sound FIFOs the audio DMA fills. The mixer samples at 32768 Hz and
produces the interleaved stereo stream the host plays, with the SOUNDCNT_L and
SOUNDCNT_H volume and panning settings applied.

## The memory map

`board/memory.c` owns the address space the CPU will run on: work RAM (256 KiB
at 0x02000000 and 32 KiB at 0x03000000), the palette, VRAM and OAM, the save
memory at 0x0E000000 and the cartridge window at 0x08000000, each with the
mirrors the console decodes. The I/O window at 0x04000000 is plain register
storage so far. The devices replace their registers with real behaviour as they
land.

## The HLE BIOS

The console's BIOS is not shipped with the emulator. The software interrupts run
as board routines (`bios/bios.c`), the way the Switch runtime turns SVC calls
into HLE and the GNU ABI runtime turns `SVC #0` into Linux system calls. Div,
Sqrt, ArcTan and ArcTan2 compute in fixed point, CpuSet and CpuFastSet move
memory, the LZ77, run length and difference calls decompress, BitUnPack widens
packed fields, and BgAffineSet and ObjAffineSet build the rotation matrices. The
calls that need the interrupt controller or the sound hardware (Halt, Stop,
IntrWait, VBlankIntrWait, SoundBias and the sound driver) are known stubs that
land with those devices.

Angles are turns in 16 bits, the way the BIOS counts them, and the affine matrix
drops the low byte of the rotation angle, giving 256 steps around the circle.
The trigonometry rounds to 1.14 fixed point like the BIOS's own tables.

## The recompiled ROM

`cpu/recomp.c` takes the ROM's ARM and Thumb code through the existing ARM
recompiler of `src/common/cpu/arm`, block by block, and links the emitted
objects into a native module with the host compiler. Loading a ROM does this
automatically and caches the module beside it as `<ROM>.code.so`, the way the
NX system type caches `<Game>_code.so`; `-r` rebuilds it. Blocks the discovery
step does not reach, a branch through a register for example, are compiled
when they are first dispatched.

The CPU state is the recompiler's `SnxArmCPU` on a flat window (`cpu/memmap.c`)
where the board's regions live at their canonical addresses with the console's
mirrors mapped to the same pages, so generated code reaches everything with
one pointer add. The privileged instructions the recompiler refuses, the mode
switching MSR/MRS forms, SWP and the exception return, are interpreted against
the banked register state. Software interrupts go to the HLE BIOS and
interrupts follow the entry and return the BIOS stub performs. Recompiled
blocks are charged two cycles per instruction, and the waits, the save memory
width rules and the timer driven FIFO rates are approximations documented
divergences.

## The ROM header

Every GBA ROM image starts with a header the cartridge presents to the console:
an ARM branch at offset 0, the Nintendo logo, the title at 0xA0, the game code at
0xAC, the maker code at 0xB0, the fixed byte 0x96 at 0xB2 and the complement
check at 0xBD over the title block. `cartridge/cartridge.c` validates the branch,
the fixed byte and the complement check, and keeps the parsed fields beside the
image. A ROM the console would reject is refused with the failing byte named.
