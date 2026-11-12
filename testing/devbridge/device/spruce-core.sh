#!/bin/sh
#
# Pick spruce's RetroArch core for one game, and put spruce's choice back.
#
#   spruce-core.sh set EMU ROM_FILE CORE    e.g. set GBA "Mario Kart.zip" mgba
#   spruce-core.sh restore EMU
#
# spruce keeps a per-game core in Emu/EMU/config.json, under
# menuOptions.Emulator.overrides keyed by the ROM's file name. It also caches
# the core each game last ran with (Saves/spruce/last_core_run), and when that
# differs it stops the launch to ask whether to move the save over; nothing
# answers that in a test. So `set` records the new core there too. `set` saves
# both first (once, so a second set cannot save the modified copies) and
# `restore` puts them back.

EMU=$2
CFG=/mnt/SDCARD/Emu/$EMU/config.json
SAVED=/tmp/netplay-devbridge/spruce-core-$EMU
JQ=/mnt/SDCARD/spruce/bin64/jq
# bin64/jq exists on the A30 too, but it is a 64-bit binary there: probe it.
"$JQ" --version >/dev/null 2>&1 || JQ=/mnt/SDCARD/spruce/bin/jq

case $1 in
set)
	ROM=$3 CORE=$4
	CACHE="/mnt/SDCARD/Saves/spruce/last_core_run/$EMU/$ROM"
	[ -f "$CFG" ] || { echo "spruce-core: no $CFG" >&2; exit 1; }
	. /mnt/SDCARD/spruce/scripts/emu/lib/core_mappings.sh 2>/dev/null
	folder=$(get_core_folder "${CORE}_libretro.so" 2>/dev/null)
	[ -n "$folder" ] || { echo "spruce-core: spruce does not know core $CORE" >&2; exit 1; }
	if [ ! -d "$SAVED" ]; then
		mkdir -p "$SAVED" && cp "$CFG" "$SAVED/config.json" || exit 1
		printf '%s\n' "$ROM" > "$SAVED/rom"
		[ -f "$CACHE" ] && cp "$CACHE" "$SAVED/cache"
	fi
	# The key spruce reads this device's per-game core from, found the way its
	# get_core_override finds it: the platform's own key (Emulator_MiyooMini on
	# the Miyoo Mini family, whose choices live apart), else the key whose
	# "devices" list names this device, else plain Emulator.
	KEY=$(
		. /mnt/SDCARD/spruce/scripts/helperFunctions.sh
		. /mnt/SDCARD/spruce/scripts/emu/lib/general_functions.sh
		EMU_NAME=$EMU EMU_JSON_PATH=$CFG GAME=$ROM
		RA_BUILD=$(get_effective_ra_build)
		for n in $(platform_names); do
			"$JQ" -e ".menuOptions.Emulator_$n" "$CFG" >/dev/null 2>&1 && { echo "Emulator_$n"; exit 0; }
		done
		k=$(emu_option_key_for_device)
		echo "${k:-Emulator}"
	) 2>/dev/null
	KEY=$(printf '%s\n' "$KEY" | tail -1)
	case "$KEY" in Emulator|Emulator_[A-Za-z0-9]*) ;; *) KEY=Emulator ;; esac
	"$JQ" --arg key "$KEY" --arg game "$ROM" --arg core "$CORE" '.menuOptions[$key].overrides[$game] = $core' \
		"$SAVED/config.json" > "$CFG.tmp" && mv "$CFG.tmp" "$CFG" || exit 1
	mkdir -p "$(dirname "$CACHE")" && printf '%s\n' "$folder" > "$CACHE" && sync || exit 1
	echo "spruce-core: $EMU/$ROM -> $CORE ($folder, $KEY)"
	;;
restore)
	[ -d "$SAVED" ] || { echo "spruce-core: nothing saved for $EMU"; exit 0; }
	CACHE="/mnt/SDCARD/Saves/spruce/last_core_run/$EMU/$(cat "$SAVED/rom")"
	cp "$SAVED/config.json" "$CFG" || exit 1
	if [ -f "$SAVED/cache" ]; then cp "$SAVED/cache" "$CACHE"; else rm -f "$CACHE"; fi
	sync && rm -rf "$SAVED" || exit 1
	echo "spruce-core: $EMU restored"
	;;
*) echo "usage: spruce-core.sh set EMU ROM CORE | restore EMU" >&2; exit 2 ;;
esac
