#!/bin/sh
# Native Brick cable checks: PMIC bit rules and gadget repair order.
set -eu
cd "$(dirname "$0")/.."
root=$(mktemp -d /tmp/netplay-brick-cable.XXXXXX)
trap 'rm -rf "$root"' EXIT HUP INT TERM

${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -Iapp \
    app/brick-power.c testing/test-brick-power.c -o "$root/power-test"
"$root/power-test"

${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -DPLATFORM='"tg5040"' -Iapp \
    app/usbnet.c app/cableproto.c app/brick-power.c testing/cable-host-stubs.c \
    -lpthread -o "$root/netplay-cable.elf"

mkdir -p "$root/sys/1-1" "$root/sys/1-1:1.0" "$root/sys/1-1:1.1"
ln -s '/devices/platform/soc/5200000.ehci1-controller/usb1' "$root/sys/usb1"
printf '1\n' > "$root/sys/1-1/busnum"
printf '2\n' > "$root/sys/1-1/devnum"
printf 'ff\n' > "$root/sys/1-1:1.0/bInterfaceClass"
printf '42\n' > "$root/sys/1-1:1.0/bInterfaceSubClass"
printf '01\n' > "$root/sys/1-1:1.0/bInterfaceProtocol"
printf 'ff\n' > "$root/sys/1-1:1.1/bInterfaceClass"
printf '4e\n' > "$root/sys/1-1:1.1/bInterfaceSubClass"
printf '01\n' > "$root/sys/1-1:1.1/bInterfaceProtocol"
probe=$(NP_CABLE_USB_SYSFS="$root/sys" NP_CABLE_USB_DIR="$root/dev" \
    "$root/netplay-cable.elf" --probe)
test "$probe" = "$root/dev/001/002"
ln -sfn '/devices/platform/soc/5100000.ehci0-controller/usb1' "$root/sys/usb1"
if NP_CABLE_USB_SYSFS="$root/sys" NP_CABLE_USB_DIR="$root/dev" \
   "$root/netplay-cable.elf" --probe >/dev/null; then
	echo 'probe accepted an interface on the wrong USB controller' >&2
	exit 1
fi

gadget="$root/config/usb_gadget/g1"
state="$root/card/.userdata/shared/Netplay"
mkdir -p "$gadget/configs/c.1" "$gadget/functions/ffs.cable" "$state" "$root/ffs"
printf '5100000.udc-controller\n' > "$gadget/UDC"
ln -s "$gadget/functions/ffs.cable" "$gadget/configs/c.1/ffs.cable"

# A linked function with no saved controller must be left untouched.
if SDCARD_PATH="$root/card" NP_CABLE_CONFIGFS="$root/config" \
   NP_CABLE_FFS_DIR="$root/ffs" "$root/netplay-cable.elf" --repair; then
	echo 'repair accepted a linked function without a saved UDC' >&2
	exit 1
fi
test -L "$gadget/configs/c.1/ffs.cable"

printf 'udc=5100000.udc-controller\n' > "$state/usb_restore"
mv "$gadget/UDC" "$gadget/UDC.saved"
mkdir "$gadget/UDC"
if SDCARD_PATH="$root/card" NP_CABLE_CONFIGFS="$root/config" \
   NP_CABLE_FFS_DIR="$root/ffs" "$root/netplay-cable.elf" --repair; then
	echo 'repair unlinked the function when UDC was unreadable' >&2
	exit 1
fi
test -L "$gadget/configs/c.1/ffs.cable"
test -f "$state/usb_restore"
rmdir "$gadget/UDC"
mv "$gadget/UDC.saved" "$gadget/UDC"

# VBUS from a wall charger: the controller was never configured by a host and
# stays "not attached". That is not a failed bind, so the repair must neither
# fail nor sit through rebind attempts.
mkdir -p "$root/udc/5100000.udc-controller"
printf '1\n' > "$root/vbus"
printf 'not attached\n' > "$root/udc/5100000.udc-controller/state"
start=$(date +%s)
SDCARD_PATH="$root/card" NP_CABLE_CONFIGFS="$root/config" NP_CABLE_FFS_DIR="$root/ffs" \
    NP_CABLE_UDC_DIR="$root/udc" NP_CABLE_VBUS="$root/vbus" "$root/netplay-cable.elf" --repair
test $(( $(date +%s) - start )) -le 2 || { echo 'repair retried a charger-only bind' >&2; exit 1; }
test ! -e "$state/usb_restore"
test "$(cat "$gadget/UDC")" = '5100000.udc-controller'

# Again with a host that had configured the controller.
printf 'udc=5100000.udc-controller\n' > "$state/usb_restore"
mkdir -p "$gadget/functions/ffs.cable"
ln -s "$gadget/functions/ffs.cable" "$gadget/configs/c.1/ffs.cable"
printf 'configured\n' > "$root/udc/5100000.udc-controller/state"
SDCARD_PATH="$root/card" NP_CABLE_CONFIGFS="$root/config" NP_CABLE_FFS_DIR="$root/ffs" \
    NP_CABLE_UDC_DIR="$root/udc" NP_CABLE_VBUS="$root/vbus" "$root/netplay-cable.elf" --repair
test ! -e "$gadget/configs/c.1/ffs.cable"
test ! -e "$state/usb_restore"
test "$(cat "$gadget/UDC")" = '5100000.udc-controller'
echo 'Brick cable repair: ok'

# H700 is never the gadget (usbnet.c, cp_gadget_unsafe): --role gadget refuses
# and --role auto goes to the host role, and neither touches g1.
rm -rf "$gadget/functions/ffs.cable" "$state/cable.log" "$state/cable.status"
h700() { PLATFORM=h700 SDCARD_PATH="$root/card" NP_CABLE_CONFIGFS="$root/config" \
    NP_CABLE_FFS_DIR="$root/ffs" NP_CABLE_UDC_DIR="$root/udc" NP_CABLE_VBUS="$root/vbus" \
    NP_CABLE_TUN="$root/no-tun" "$root/netplay-cable.elf" "$@"; }
if h700 --role gadget; then
	echo 'H700 accepted --role gadget' >&2
	exit 1
fi
grep -q 'state=failed' "$state/cable.status"
grep -q 'cannot be the USB gadget' "$state/cable.status"
test ! -e "$gadget/functions/ffs.cable"
h700 --role auto || true
grep -q 'never the USB gadget' "$state/cable.log"
test ! -e "$gadget/functions/ffs.cable"
test "$(cat "$gadget/UDC")" = '5100000.udc-controller'
echo 'H700 never the gadget: ok'
