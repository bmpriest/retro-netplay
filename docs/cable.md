# Brick cable bridge development status

`bin/tg5040/netplay-cable.elf` carries IPv4 packets over a USB-C data cable.
The **top** Brick port is the USB host; the **bottom** port is the gadget. Run
one daemon with `--role host` and the other with `--role gadget`. The netplay
Host/Join choice is independent of these electrical roles.

The gadget adds `ffs.cable` beside the firmware's `ffs.adb` and preserves its
USB identity. `--stop` asks a running daemon to exit, then repairs the stock
gadget. `--repair` handles a daemon lost to SIGKILL. Both commands also restore
the saved AXP2202 charger and input-current settings. A live bridge heartbeat,
not USB enumeration alone, marks `cable.status` as `up`.
`--probe` performs a read-only search for a matching cable interface on the
top-port USB bus.

The gadget's battery charger is disabled while a bridge peer is present. USB
still requires the host port to supply VBUS for enumeration and device power;
the charger change prevents battery charging from the other Brick.

Phase 3 passed on two Bricks on 2026-11-14 (gadget: NextUI Brick, bottom
socket; host: spruce Brick, top socket):

- 50 arm/stop cycles: link up in 0.8-2.2 s every time; after each stop `g1`
  held only `ffs.adb`, the UDC was rebound, adbd kept its pid and charging
  resumed.
- `kill -9` while linked leaves `ffs.cable` linked and the UDC unbound (adb
  dead); `--repair` restores the stock gadget, adb and the charger.
- A pulled cable drops both ends to `wait` within ~2 s and restores charging;
  re-plugged, the link is `up` ~1 s after enumeration with no daemon restart.
- Link: 1.2-1.9 ms ping, ~20 Mbit/s TCP each way, high speed (512-byte
  packets). The fork's 0.3-0.7 ms was not reached; the spruce host idles at
  408 MHz (its governor is read-only), which is the likely difference.

Two bugs found and fixed there: the gadget kept the full-speed packet size
it read before the host enumerated it (now re-read on the slow tick), and a
bind could leave the sunxi UDC `not attached` with VBUS present after logging
`failed to start g1: -19` (2 in 4 restarts after `kill -9`, also seen on a
plain start). A bind now has to attach while VBUS is present, or it is undone
and retried; 10 of 10 kill/restart runs then came up, two after a retry.

A daemon killed with SIGKILL leaves `cable.status` saying `up`. The broker's
reader (`netsetup.c`) already requires a live `cable.pid` running
`netplay-cable.elf`; any other reader must do the same.

The app does not yet offer Host > USB cable.
