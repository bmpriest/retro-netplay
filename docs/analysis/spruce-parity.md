# Spruce coverage and NextUI parity

Current code and packaging, 2026-11-12 (v1.0.0). A covered device means the
app allowlist (`launcher/spruce/config.json`), launcher runtime and packaged
binaries cover it; it does not mean every model has been tested physically.

| Spruce device | PyUI identity | Build directory | Physical validation |
| --- | --- | --- | --- |
| TrimUI Brick | TRIMUI_BRICK | tg5040 | Main Spruce test rig |
| TrimUI Brick Pro | TRIMUI_BRICK_PRO | tg5040 | Covered, not tested here |
| TrimUI Smart Pro | TRIMUI_SMART_PRO | tg5040 | Covered, not tested here |
| TrimUI Smart Pro S | TRIMUI_SMART_PRO_S | tg5050 | Covered, not tested here |
| Miyoo Flip | MIYOO_FLIP | my355 | Covered, not tested here |
| Miyoo A30 | MIYOO_A30 | my282 (ARMv7) | Tested |
| Miyoo Mini family (Mini, Mini Plus, Mini Flip) | MIYOO_MINI_FAMILY, per-model identity | my285 (ARMv7, runs the A30's shim and cores) | Mini Flip tested |
| Anbernic H700 XX family | ANBERNIC_RGXX family, model/panel-specific identity | h700 | RG35XX H tested |
| dArkMoss devices (Powkiddy RGB30, RGB20SX, Miniloong Pocket 1, A10 Mini and others, RK3566 and RK3326) | DARKMOSS | my355 | App starts on an A10 Mini V4; sessions not tested |

The dArkMoss devices use the Flip's AArch64 build and matching ABI, with their
own PyUI device, KMSDRM display and runtime paths. spruce marks them with
`PLATFORM_FAMILY=darkmoss` (or `OS_NAME="DARKMOSS"` in `/etc/os-release`), and
the launcher maps every one of them to `my355`. PyUI draws everything, and
`netplay.elf` on spruce is headless (it links only libc), so no display or SDL
setup is Netplay's. Rotation and scaling belong to each PyUI device class.

## Shared features

Shared-screen LinkPlay, RetroArch protocol for supported shared-screen cores,
Gambatte/gpSP/armGBA linking, Gambatte and mGBA Dual, per-session core
selection ([engine-matrix.md](../engine-matrix.md)), compatibility-core
negotiation, game invitations, persistent sessions, save protection/backups,
checks and Wi-Fi recovery are implemented on both firmwares. Hybrid LinkPlay
traffic defaults on; verbose logs default off. RetroArch protocol does not
transport NETPACKET link-core traffic.

Spruce has a native PyUI menu with contextual glyphs and X to end sessions.
It presents one core list grouped by system, checking the core Netplay uses
for sessions; spruce's own emulator choices still apply outside them. NDS
combines DraStic and DSperate. It does not install NextUI's MGBA.pak, DSP.pak
or NDS.pak.

## Firmware differences

- NextUI alone has the Varnish START quick menu, status icon, GameSwitcher
  shortcut and Ad Hoc Network Switch. Invitations also use Varnish on NextUI
  and are independent of the START-menu setting.
- Spruce uses its own supplicant for ad hoc joining and restoration, preserving
  saved networks. It owns sleep and power controls.
- Spruce hooks `ra64.universal`, `ra64.h700`, `ra32.universal`, `ra32.h700`,
  `ra32.a30` and `ra32.mini` (and the Mini Flip's `RetroArch/retroarch`). A
  64-bit device paired with a 32-bit one runs its 32-bit RetroArch and cores
  for the session (`compat_ra32`).
- Under Emu/NDS it hooks `drastic`, `drastic64` and `dsperate`. In a session
  on an AArch64 device a DSperate launch runs Netplay's bundled DSperate
  (`cores/dsperate/aarch64`), since spruce's 3.3 lacks the LAN fixes. The A30
  build carries the DraStic radio, but DS on the A30 and Mini family is
  untested. DraStic and DSperate use
  their own radio transport, not hybrid LinkPlay's UDP inputs.
- X ends a session and leaves inactive passthrough wrappers. Turn Off Netplay
  also removes the bind mounts. Neither action uninstalls emulator files.

## Validation and remaining limits

The Brick (host) and RG35XX H (guest) passed FC, SFC, MD, PS and DS
(DSperate with DSperate) on 2026-11-12 over house Wi-Fi; the DS race ran at
about 35 fps there, below the harness's 50 fps bar (DS wants ad hoc). The A30
and Mini Flip were exercised on 2026-11-11.

The original autonomous suite and transport comparison are historical runs.
Stock DSperate summaries establish session/peer replies; they provide no
pacing windows and must not be reported as measured FPS. Updated fixtures do
not retroactively change old report verdicts. See
[the broker plan](../analysis/devbridge-test-broker.md) and
[test instructions](../../testing/spruce/README.md).

Other models' controller/display behavior, PS across architectures on NextUI,
and DS games outside the recorded fixtures remain untested. Network and
emulator performance should be attributed to the actual game, protocol and run
rather than generalized from a single result.
