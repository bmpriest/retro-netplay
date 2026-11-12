# spruce's 32-bit cores against its 64-bit cores

A 32-bit spruce device (Miyoo A30, Miyoo Mini) runs spruce's 32-bit RetroArch
with the cores in `RetroArch/.retroarch/cores`; a 64-bit device runs
`cores64`. Netplay between them needs the two builds of a core to compute the
same game from the same inputs and to load each other's states. This is which
of spruce 4.6.0's cores do, measured 2026-11-09.

The harness and how to rerun it: [testing/spruce/32v64](../../testing/spruce/32v64/README.md).
The raw output is in `testing/results/spruce-32v64-20261109` (H700) and
`testing/results/spruce-32v64-a30-brick-20261110` (A30 + Brick), both
git-ignored.

The state checks and corecmp traces were repeated on 2026-11-10 with each
build on the hardware that runs it: the 32-bit build on a Miyoo A30 and the
64-bit build on a spruce TrimUI Brick (see
[On the real hardware](#on-the-real-hardware-a30-and-brick)). Every verdict
below held.

## How it was measured

Both builds ran natively on one spruce RG35XXH (H700, spruce 4.6.0): the H700
runs armhf binaries, with spruce's 32-bit libraries in `spruce/h700/lib32`.
Each core got the options from spruce's own `.opt` file for it, and the wall
clock was pinned so RTC carts and power-on times compare.

Per core and ROM ("cell"):

- **cold boot**: each build plays 3600 frames from power-on with inputs derived
  from the frame number, hashing state every 300 frames; twice, to check that
  each build matches itself, and once on the real clock.
- **states**: each build saves at frame 1800; save/load round trip, serialize
  side effects, host/client amplification.
- **cross loads**: every build loads every build's state and plays 600 more
  frames. A state from the 32-bit build must play on the same in both builds,
  and likewise for the 64-bit one.
- **corecmp** where whole-state hashes are noise: per-frame RAM, video, audio
  and state traces, so the game itself can be compared.

## Results

| core | spruce 32-bit | spruce 64-bit | result | games |
|---|---|---|---|---|
| fceumm | (SVN) 5cd4a43 | (SVN) 3544ff5 | compatible | Mega Man 2, SMB3, Squatch Quest |
| nestopia | 1.53.2 51ad831 | 1.53.1 d2eefab | compatible | Mega Man 2, SMB3 |
| quicknes | dbf19f7 | dbf19f7 | compatible¹ | Mega Man 2, SMB3 |
| snes9x | 1.63 5a40cd5 | 1.63 49f48456 | compatible | Super Mario Kart, Contra III, Star Fox |
| mednafen_supafaust | 1.29.0 75c658c | 1.29.0 e25f667 | compatible | Super Mario Kart, Contra III, Star Fox |
| picodrive | 2.05-975cdaf | 2.04-7102a71 | compatible | Streets of Rage 2, Gunstar Heroes, Sonic Chaos (GG), Astro Force (SMS) |
| gearsystem | 3.8.4 8d83e6a | 3.8.1 81decc2 | compatible⁷ | Sonic Chaos (GG), Astro Force (SMS) |
| gambatte | v0.5.0-netlink 77f2567 | same | compatible² | Tetris, Pokémon Red, Crystal, Shantae |
| gearboy | 3.7.3 de80c46 | 3.1.1 | compatible² | Tetris, Pokémon Crystal |
| pokedaisy | 0.3 | 0.3 | compatible | Pokémon Emerald |
| armgba | 1.4 | 1.4 | compatible³ | Mario Kart Super Circuit, Pokémon Emerald |
| swanstation | 1.0.0 b6c30a7b | same | compatible³ | Crash Team Racing |
| stella2014 | 3.9.3 3cc89f0 | same | compatible | Anguna, Halo 2600 |
| mednafen_pce_fast | 1.31.0 3e666a3 | 1.31.0 baa3551 | compatible | homebrew (GreenSpace, PlaySample) |
| mednafen_supergrafx | 1.29.0 a776133 | same | compatible | GreenSpace |
| gpsp | v1.1.0-5819380 | same | **not with the dynarec**⁴ | Advance Wars 2, Emerald, MK Super Circuit |
| mgba | 0.10-dev ec5ecb26d | 0.11-dev 88b2273 | **no**: differ from frame 1 | Tetris, Crystal, Emerald, MK Super Circuit |
| mednafen_wswan | 0.9.35.1 2aeb47d | 0.9.35.1 2a49b3cd33 | **no**: differ from frame 1 | WonderCell |
| genesis_plus_gx | 1.7.4 252a94c | 1.7.4 f64e0a1 | **no**⁵ | Streets of Rage 2, Gunstar Heroes, Sonic Chaos, Astro Force |
| chimerasnes | 04c57c2 | same | **no**: states 495456 vs 495536 bytes | Super Mario Kart, Contra III, Star Fox |
| pcsx_rearmed | r25 228c14e | r25 febf224 | **no**: RAM differs from frame 1⁶ | Crash Team Racing |
| fuse | cad85b7 | same | not judged: the 32-bit build segfaults | Cadaverion |
| ardens | 0.25.9 | same | not judged: load fails in the harness on both | Choplifter, Arduboy3D |

1. QuickNES's post-load state hash differs by which build is running, not by
   which build saved the state; corecmp shows RAM, video and state equal on
   every frame after a cross load. Its 32-bit build needs the misaligned
   access fixups below.
2. Whole-state hashes do not even match a build against itself (gambatte
   stores a host pointer and leaves MBC fields uninitialised in its state;
   gearboy similarly). corecmp: RAM, video and audio equal between builds from
   power-on, and a cross load plays on exactly like a load of the build's own
   state.
3. Only with a newer libstdc++ for the 32-bit build (below).
4. Same revision, but the ARM32 and ARM64 dynarecs drift: cold boots differ
   within 300-700 frames of RAM. With `gpsp_drc=disabled` both builds are
   bit-identical from power-on. With the dynarec on, a cross load starts out
   matching and then drifts the same way (Mario Kart: RAM by frame 2690).
5. Mega Drive RAM diverges around frame 156 of Streets of Rage 2, and loading
   one build's MD state into the other segfaults. SMS/GG cold boots agree, but
   cross loads do not.
6. With spruce's `pcsx_rearmed_drc_thread = "auto"` neither build matches
   itself. With the threaded dynarec off (A30 + Brick corecmp), each build
   repeats itself exactly, from power-on and after loading its own state, but
   the two builds' RAM differs from frame 1 and a cross load diverges at once.
   The live test below agrees.
7. On the A30 the 32-bit build's state blob is not the same from run to run,
   so its whole-state hashes do not match themselves. corecmp: RAM, video and
   audio equal on every frame, between runs and against the 64-bit build, and
   a cross load plays on exactly like a load of the build's own state. The
   game is the same; the state carries some bytes that are not (as with
   gambatte), which matters to anything that compares state checksums.

### Harness and platform findings

- 32-bit armGBA, SwanStation and arduous need `GLIBCXX_3.4.32`. Only
  `spruce/a30/lib` has it; `spruce/h700/lib32` has 3.4.30, so these cores do not
  load under spruce's 32-bit RetroArch on an H700 (`ra32.h700`).
- 32-bit Genesis Plus GX and QuickNES use misaligned LDM/STM, which faults
  (SIGBUS) under an arm64 kernel and qemu-user. A 32-bit kernel, as on the
  A30, fixes them up silently; the harness's `alignfix` preload does the same.
- 32-bit armGBA's `retro_serialize` needs an 8-byte-aligned buffer. The shim
  passed one at `malloc + 20` and armGBA faulted on the A30; the shim now
  serializes through an aligned copy (ce78249).

## On the real hardware: A30 and Brick

2026-11-10: the same cells with the 32-bit build on a Miyoo A30 (spruce 4.6.0,
its own kernel and `spruce/a30/lib`) and the 64-bit build on a spruce TrimUI
Brick (harness built with the tg5040 toolchain, spruce's RetroArch library
path). Each device ran its own build's cold boots and saves; the states were
then swapped so each build loaded the other's. Both devices used the same 25
ROMs (md5-checked) and the same core option files.

- **The 64-bit side did not change.** For every core that matches itself, the
  Brick's per-checkpoint hashes are identical to the H700's. spruce's
  `cores64` compute the same on both, so the H700 results stand for the Brick.
- **The 32-bit side matches too.** Every state-check verdict is the same as
  on the H700 except gearsystem's, whose 32-bit state is not reproducible on
  the A30; corecmp shows the game itself is (footnote 7).
- **corecmp on 22 pairs** (gambatte, gearboy, gearsystem, Genesis Plus GX,
  gpSP with and without the dynarec, mGBA, pcsx_rearmed, chimerasnes,
  QuickNES): every pair the H700 also ran gives the same first-difference
  frames, frame for frame. New pairs: gearsystem on Sonic Chaos and Astro
  Force (same game, cross loads exact), Genesis Plus GX on Gunstar Heroes (MD
  RAM differs from frame 110, cross loads segfault, like Streets of Rage 2) and
  Sonic Chaos (cold boots agree, cross loads diverge at once, like Astro
  Force), gambatte on Tetris (same game).
- **mGBA on the A30** segfaults at once on Pokémon Crystal (it ran on the
  H700). It is incompatible either way.
- **No workarounds needed on the A30.** Its kernel fixes up the misaligned
  accesses itself, so 32-bit QuickNES and Genesis Plus GX run without the
  `alignfix` preload, and `spruce/a30/lib` has the libstdc++ that armGBA and
  SwanStation need. Both are compatible natively.
- **fuse:** the 32-bit build still segfaults, and loading the 64-bit build's
  state hung (19 minutes at full CPU, killed).

Three harness problems showed up and are fixed in the scripts:

- **Option files.** Each device's own `.opt` files differ: the A30's armGBA
  file had Link Mode "together", which makes its state 1,730,890 bytes
  against 788,588, and gpSP, fceumm, gambatte and pcsx options differed
  slightly. Both devices now read one shared copy (`OPTDIR`).
- **Time zones.** The A30 runs on UTC and the Brick on MST. The clock is
  pinned, but RTC cartridges (Pokémon Emerald and Crystal) convert it to local
  time, so they saw different hours. The scripts now set `TZ=UTC0`.
- **Extra options lost.** A cell's extra options (`gpsp_drc=disabled`,
  pcsx's `drc_thread=disabled`) were appended after the `.opt` values, but
  the tools take the first value for a key and keep only the first 32 keys
  (spruce's PCSX-ReARMed.opt has 79). With the shared gpSP.opt the dynarec
  stayed on, and pcsx never got its extras on either rig, which is why it did
  not match itself on the H700. The extras now come first.

## Live: A30 against a spruce H700

Netplay's identity check compares the core's version string, so without help
every stock-core pair above is refused on that alone. With the test override
`relaxed_core_identity=1` (shim e69278f: version string ignored, everything
else still checked), an A30 hosting and a spruce H700 (cores64) joining,
2026-11-09:

| core | result |
|---|---|
| fceumm, supafaust, picodrive | play in step, no desync, 58-60 fps |
| pcsx_rearmed | desyncs from frame 0; the handshake does not equalise it and it resyncs repeatedly |
| armGBA Together | Game Boy/Color refused at connection: states differ in size (Tetris 841924 vs 843364 bytes). GBA works once the aligned-buffer fix is in |
| armGBA Separate (link) | passes |
| Netplay's own gambatte_dual and mgba_dual | work 32 against 64 without the override (Mario Kart Super Circuit only about 40 fps on the A30) |

The `compat_ra32` path (the H700 drops to spruce's 32-bit RetroArch and cores)
also works. Core negotiation then wrongly flags every core as mismatched,
because it compares the A30's `cores/` with the H700's `cores64/`.

## What this means for Netplay

- For NES (fceumm, nestopia, quicknes), SNES (snes9x, supafaust), picodrive,
  Game Boy (gambatte, gearboy) and the smaller systems in the table, spruce's
  two builds are interchangeable. Only Netplay's version-string check stands
  between a 32-bit and a 64-bit device.
- mGBA, Genesis Plus GX, chimerasnes, WonderSwan and pcsx_rearmed cannot be
  mixed. For GBA use gpSP with the dynarec off, armGBA, or Netplay's own
  mgba_dual. For Mega Drive use picodrive.
- The other route is both devices running 32-bit cores: `compat_ra32` above,
  or Netplay's own spruce32 armv7 set (`make compatibility-spruce32`, see
  [game-systems.md](../game-systems.md)), which is defined but not yet built
  into the package.
