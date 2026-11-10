#!/bin/sh
#
# This device's armGBA option file for a test, and putting the user's back.
#
#   armgba-setup.sh MODE [KEY=VALUE...]   MODE: together | separate | off
#   armgba-setup.sh restore
#
# Writes RetroArch's armGBA.opt with Link Mode MODE, two players and the
# player numbers off (they would sit on every reference screen), plus any
# armgba_* KEY=VALUE given. Joining a session agrees these with the other
# device (the host's win); a file written after joining needs
# `ctl.sh sync` on the guest before the game opens, then
# `check.sh armgba MODE` on both to confirm what was agreed.
#
# The user's own file is saved once, the first time, and `restore` puts it
# back (or removes the test's when there was none).

D=/mnt/SDCARD/RetroArch/.retroarch/config/armGBA
OPT=$D/armGBA.opt
SAVED=/tmp/netplay-devbridge/armgba-opt

case $1 in
restore)
	[ -d "$SAVED" ] || { echo "armgba-setup: nothing saved"; exit 0; }
	if [ -f "$SAVED/armGBA.opt" ]; then cp "$SAVED/armGBA.opt" "$OPT"; else rm -f "$OPT"; fi
	sync && rm -rf "$SAVED" || exit 1
	echo "armgba-setup: restored"
	;;
together|separate|off|gamecube)
	MODE=$1; shift
	mkdir -p "$D" || exit 1
	if [ ! -d "$SAVED" ]; then
		mkdir -p "$SAVED" || exit 1
		[ -f "$OPT" ] && cp "$OPT" "$SAVED/armGBA.opt"
	fi
	{
		printf 'armgba_link_mode = "%s"\n' "$MODE"
		printf 'armgba_link_players = "2"\n'
		printf 'armgba_player_numbers = "disabled"\n'
		for kv; do printf '%s = "%s"\n' "${kv%%=*}" "${kv#*=}"; done
	} > "$OPT.tmp" && mv "$OPT.tmp" "$OPT" && sync || exit 1
	echo "armgba-setup: Link Mode $MODE"
	;;
*) echo "usage: armgba-setup.sh together|separate|off [KEY=VALUE...] | restore" >&2; exit 2 ;;
esac
