# 32v64: spruce's 32-bit cores against its 64-bit cores

spruce ships two builds of most cores: `RetroArch/.retroarch/cores` for its
32-bit RetroArch and `cores64` for its 64-bit one. A 32-bit device (A30, Miyoo
Mini) can only play netplay with a 64-bit device if the two builds compute
the same game from the same inputs and load each other's states. This harness
checks that either on one 64-bit spruce device that runs both builds (an H700
runs armhf binaries; spruce keeps 32-bit libraries in `spruce/h700/lib32`), or
split across a real 32-bit device and a 64-bit one, swapping save states
between them (see "Two devices" below).

Results and conclusions: [docs/analysis/32-vs-64bit.md](../../../docs/analysis/32-vs-64bit.md).

## Pieces

| file | what |
|---|---|
| `build.sh` | builds everything below into `build/32v64/<TARGET>/` (`.32` armhf with the A30 toolchain, `.64` aarch64 with the toolchain of `TARGET`: `h700`, or `tg5040` for the Brick) |
| `shim/test/determinism.c` | runs N frames with frame-number-derived inputs, hashing state every `CHECKPOINT` frames |
| `shim/test/statecheck.c` | save/load round trip, serialize side effects (`--serprobe`), host/client amplify, `--load`/`--save` |
| `testing/corecmp/corecmp.c` | per-frame RAM/video/audio/state traces, for cores whose whole-state hash is noise |
| `fixedtime.c` | LD_PRELOAD: pins the wall clock, so RTC carts and gambatte's power-on time compare |
| `alignfix.c` | LD_PRELOAD (32-bit): emulates the 32-bit kernel's misaligned LDM/STM/LDRD/STRD fixups that an arm64 kernel (and qemu-user) does not do |
| `busdump.c` | LD_PRELOAD (32-bit): reports the faulting instruction and library on SIGBUS, exits 77 |
| `cell.sh` | one core + ROM: determinism twice (pinned clock) and once (real clock) per build, statecheck per build, then the four cross loads |
| `corecmp-pair.sh` | one core + ROM in corecmp's run-pair layout (a/a2/b/b2/aa/bb/ab/ba) |
| `batch.sh` | runs a cell list N at a time, resumable, logging to `progress.txt` |
| `judge.py` | turns `out/` into one verdict per cell |
| `cells*.txt`, `corecmp.txt` | the cells run on 2026-11-09 |

The determinism and corecmp tools take core options from `STATECHECK_VARS` /
`CORECMP_VARS` (`key=value;...`; `cell.sh` fills them from spruce's `.opt`
file for the core, so the cores run with the settings spruce uses) and the
system folder from `SYSDIR`.

## Running it

On the PC:

```sh
testing/spruce/32v64/build.sh
D=root@<device>:/mnt/SDCARD/cc3264
ssh root@<device> mkdir -p /mnt/SDCARD/cc3264/bin /mnt/SDCARD/cc3264/roms /mnt/SDCARD/cc3264/sys
scp build/32v64/h700/* testing/spruce/32v64/*.sh testing/spruce/32v64/*.txt $D/bin/
```

Copy the ROMs each cell list names into `cc3264/roms/<system>/` (or point a
cell at `@/absolute/path`), and BIOS files into `cc3264/sys`. For the
GLIBCXX cells, copy `spruce/a30/lib` to `cc3264/lib32a30`.

On the device, from `/mnt/SDCARD/cc3264`, each pass under `nohup`:

```sh
sh bin/batch.sh bin/cells.txt 3
LIB32=/mnt/SDCARD/cc3264/lib32a30 sh bin/batch.sh bin/cells-a30lib.txt 3
PRELOAD32=alignfix sh bin/batch.sh bin/cells-alignfix.txt 3
RUN=corecmp-pair.sh sh bin/batch.sh bin/corecmp.txt 3
```

Three jobs at a time suits a four-core H700; the whole set took about two
hours on 2026-11-09. Then back on the PC:

```sh
scp -r root@<device>:/mnt/SDCARD/cc3264/out root@<device>:/mnt/SDCARD/cc3264/cc testing/results/<run>/
python3 testing/spruce/32v64/judge.py testing/results/<run>/out
python3 testing/corecmp/compare.py testing/results/<run>/cc/<cell>     # per corecmp cell
```

## Two devices

A Brick cannot run 32-bit binaries (no armhf runtime under spruce), and an
emulated 32-bit kernel is not the real one, so the faithful pairing is the
32-bit build on an A30 and the 64-bit build on the 64-bit device. Each device
runs only its own build (`ARCHS`), in two phases (`PHASE`):

```sh
TARGET=tg5040 testing/spruce/32v64/build.sh     # copy build/32v64/tg5040/* to both
# same ROMs on both (compare md5sums) and the same .opt files on both, in cc3264/opt

# A30
ARCHS=32 PHASE=native OPTDIR=/mnt/SDCARD/cc3264/opt \
  LIB32=/mnt/SDCARD/spruce/a30/lib:/mnt/SDCARD/miyoo/lib:/usr/miyoo/lib:/usr/lib:/lib \
  sh bin/batch.sh bin/cells.txt 2
# Brick (spruce's RetroArch library path)
LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib:/lib:/mnt/SDCARD/spruce/flip/lib \
  ARCHS=64 PHASE=native OPTDIR=/mnt/SDCARD/cc3264/opt sh bin/batch.sh bin/cells.txt 3
```

Then copy each device's `out/<cell>/st32.bin` / `st64.bin` to the other, run
the same commands with `PHASE=cross`, and merge both `out/` folders on the PC
(the file names do not collide) with a `done` file per cell for `judge.py`.
Neither device has `nohup`; start batches as
`(trap "" HUP; exec sh bin/batch.sh ...) </dev/null >batch.log 2>&1 &`.
The A30's own kernel fixes misaligned accesses and `spruce/a30/lib` has the
newer libstdc++, so `cells-alignfix.txt` and `cells-a30lib.txt` are not needed.

## Reading the verdicts

| verdict | meaning |
|---|---|
| COMPATIBLE | both builds self-deterministic, all checkpoints equal across builds, and each build's state loads into the other and plays on identically |
| SAME EMULATION, cross-load differs | cold boots agree, but a state from the other build plays on differently |
| LOADS MATCH, cold boot drifts | cross loads agree, cold boots do not (gpSP's dynarec) |
| INCOMPATIBLE (state size) | the builds' states are different sizes; netplay refuses at connection |
| INCOMPATIBLE (diverges) | self-deterministic, but the builds compute different games |
| NONDETERMINISTIC | a build does not match itself; the whole-state hash cannot judge it, so use corecmp |
| NO RUN | a build did not load or run; the note says why |

`NONDETERMINISTIC` is usually harness noise, not the game: gambatte stores a
host pointer and leaves MBC fields uninitialised in its state, so two runs of
one build hash differently while RAM and video match. The corecmp traces
settle those.
