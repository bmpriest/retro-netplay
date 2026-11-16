# Releases

## Artifacts

| Archive | Extraction location | Installed application |
| --- | --- | --- |
| NextUI-Netplay-Complete.zip | SD `Tools/<PLATFORM>/Netplay.pak/` (pak at the zip root, as the Pak Store installs it) | Netplay.pak/ |
| Spruce-Netplay-Complete.zip | SD root | App/Netplay/ |

NextUI includes all five platform builds, Varnish on its supported platforms,
ordinary Gambatte/gpSP, both paired cores where built, MGBA.pak, DSP.pak, the
DraStic radio overlay (NDS.pak itself comes from the Pak Store), and
ARMv7/AArch64 compatibility cores. The pak's contents sit at the zip root, as
the Pak Store installs them.
Spruce includes AArch64 builds (`tg5040`, `tg5050`, `my355`, `h700`) and
ARMv7 builds (`my282` for the A30, `my285` for the Mini family), the shims,
ordinary/paired cores, the DraStic radio, Netplay's AArch64 DSperate, the PyUI
frontend/license and AArch64 compatibility cores. Its `netplay.elf` is the
headless engine behind PyUI (`app/spruce/engine.c`) and links only libc, so no
NextUI runtime, SDL or stub libraries are packaged. It uses
Spruce's installed RetroArch and DS emulators rather than installing NextUI
paks. Its target device matrix is in [spruce-parity.md](analysis/spruce-parity.md).
No ROMs are packaged. Neither edition downloads compatibility cores in-app.

The old `Netplay.pak.zip` remains an intermediate base package, and
`compatibility-cores.zip` remains a standalone compatibility pack. They are
not the planned downloadable Core edition.

## Build

Use the pinned sibling build-platforms checkout (or set `BUILDER`) and Docker.
The Makefile fetches/builds pinned source dependencies where required.

```sh
make app app-spruce shim
make cores
make cores-gambatte-dual
make cores-mgba cores-mgba-dual
make compatibility
make cores-dsp cores-dsp-spruce cores-nds varnish udhcpd
make test
make dist-complete
```

Build prerequisites are explicit for Complete compatibility cores. Artifacts
are under `dist/`. `SPRUCE_PLATFORMS` defaults to
`tg5040 tg5050 my355 h700 my282 my285`; do not advertise models whose
build/runtime is omitted. Build scripts support mGBA on all five NextUI platform directories;
performance limits, particularly the A30, remain.

Before shipping, verify ZIP integrity, both layouts, executable modes, AArch64
machine types for the Spruce runtime, packaged dependencies, and absence of
ROMs, mutable sessions, credentials and test artifacts. Record SHA-256 sums. Each archive includes SOURCE_COMMIT; the validator
checks it against the current checkout as well as matching packaged native
binaries to the local build outputs.
Use a clean checkout or record the final commit; packaging does not publish a
GitHub release. The final source commit includes the reviewed UI and harness
changes. Historical test reports must retain their original results and
measurement limitations.

## Runtime requirements and scope

Spruce reads `App/Netplay/config.json` to filter the app by model/family. The
launcher maps the actual firmware name to a packaged build and selects the
matching PyUI device/display setup. This allowlist is deliberate: other
64-bit nightly targets are not automatically covered. The native binary's
presence alone cannot establish architecture compatibility. The my355
engine (Flip and dArkMoss) is built with the H700 toolchain; its cores remain
the my355 builds.

Hardware exercise has focused on the Spruce Brick, RG35XX H, A30 and Mini
Flip; the remaining models are covered by their builds but untested. Spruce's
32-bit RetroArch is routed; DS local wireless is tested on 64-bit devices only.
Netplay chooses each session's core ([engine-matrix.md](engine-matrix.md));
hybrid traffic defaults on and verbose logs default off.

## Publishing

`pak.json` on the default branch is what the Pak Store reads, hourly: it
downloads `{repo_url}/releases/download/{version}/{release_filename}`. So:

1. Build with `make dist-complete` from the commit you will tag.
2. Tag that commit with `pak.json`'s `version` exactly (e.g. `v1.1.0`), publish the
   GitHub release and upload both Complete zips.
3. Only then push a `pak.json` version bump to the default branch.

Use plain `vX.Y.Z` versions. The store compares versions number by number and
reads a suffix such as `b` or `-rc1` as 0, so a beta would never be followed
by its final release. Ship betas as GitHub pre-releases without changing
`pak.json`.
