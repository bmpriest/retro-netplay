#!/bin/sh
#
# Keep the two test devices in step, run on one device at a time.
#
#   sync.sh game ROM_FILE     this device must be running ROM_FILE now
#   sync.sh prompt-off        spruce: never stop a launch to ask about saves
#   sync.sh prompt-restore    put that setting back
#
# Two failure modes made a two-device run worse than useless: spruce stops
# a launch whose core differs from the ROM's last one to ask whether to
# transfer the save (one device sits at the prompt while the other plays),
# and nothing checked that both devices ended up in the same game (one in
# Tetris, the other in Mario Kart, each still "in game"). prompt-off removes
# the question for the length of a run - the runner sets it before a test
# and restores it after - and `game` is injected by run.sh right after each
# WAIT_FOR game, so a mismatch fails the test at the launch instead of
# after minutes of mismatched play.
#
# SYNC_PROC_ROOT / SYNC_SPRUCE_CONFIG / SYNC_STATE_DIR exist for the host
# test (test_sync_guard.py); on a device the defaults are the real paths.

PROC_ROOT=${SYNC_PROC_ROOT:-/proc}
STATE_DIR=${SYNC_STATE_DIR:-/tmp/netplay-devbridge}

case $1 in
game)
	ROM=$2
	running=
	for cmdline in "$PROC_ROOT"/[0-9]*/cmdline; do
		[ -f "$cmdline" ] || continue
		args=$(tr '\0' ' ' < "$cmdline" 2>/dev/null) || continue
		case $args in
		*RetroArch*|*minarch*|*retroarch*)
			case $args in
			*"$ROM"*) echo "sync: running $ROM"; exit 0 ;;
			esac
			# Remember the first other game this device is in.
			[ -n "$running" ] || running=$args ;;
		esac
	done
	if [ -n "$running" ]; then
		echo "sync: FAIL: expected $ROM, this device runs: $running"
	else
		echo "sync: FAIL: expected $ROM, no game is running on this device"
	fi
	exit 1
	;;
prompt-off|prompt-restore)
	CFG=${SYNC_SPRUCE_CONFIG:-/mnt/SDCARD/Saves/spruce/spruce-config.json}
	BAK=$STATE_DIR/spruce-config.prompt.bak
	[ -f "$CFG" ] || { echo "sync: no spruce config, prompt setting skipped"; exit 0; }
	JQ=/mnt/SDCARD/spruce/bin64/jq
	# bin64/jq exists on the A30 too, but it is a 64-bit binary there: probe it.
	"$JQ" --version >/dev/null 2>&1 || JQ=/mnt/SDCARD/spruce/bin/jq
	"$JQ" --version >/dev/null 2>&1 || JQ=jq
	if [ "$1" = prompt-off ]; then
		mkdir -p "$STATE_DIR"
		[ -f "$BAK" ] || cp "$CFG" "$BAK" || exit 1
		"$JQ" '.menuOptions."Emulator Settings".keepSavesBetweenCores.selected = "Always"' \
			"$BAK" > "$CFG.tmp" && mv "$CFG.tmp" "$CFG" || exit 1
		echo "sync: save-transfer prompt off (keepSavesBetweenCores=Always)"
	else
		[ -f "$BAK" ] || { echo "sync: nothing to restore"; exit 0; }
		cp "$BAK" "$CFG" && rm -f "$BAK" || exit 1
		echo "sync: save-transfer prompt restored"
	fi
	;;
*)
	echo "usage: sync.sh game ROM_FILE | prompt-off | prompt-restore" >&2
	exit 2
	;;
esac
