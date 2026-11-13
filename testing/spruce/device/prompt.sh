#!/bin/sh
#
# The answer half of Netplay's invitation prompt on its own (spruce's
# App/Netplay): netplay.elf reads it from the grabbed pad and draws nothing;
# the PyUI front end draws the question.
#
#   prompt.sh [--accept L] [--decline L] [--browse L] [--timeout S]
#   prompt.sh --nudge      a D-pad down/up on the pad, so a resumed menu repaints
#
# Prints accept|decline|browse|timeout; exits 0, 1, 3 or 2.

. /mnt/SDCARD/spruce/scripts/helperFunctions.sh >/dev/null 2>&1
. /mnt/SDCARD/App/Netplay/launcher/firmware.sh
pak=$NETPLAY_PAK
PLATFORM=$(np_spruce_platform "$PLATFORM") || { echo "error no Netplay build for this device" >&2; exit 1; }
LOGS_PATH=$NETPLAY_LOGS
export PLATFORM LOGS_PATH
mkdir -p "$LOGS_PATH"
cd "$pak" || exit 1
[ "${1:-}" = --nudge ] && exec "$pak/bin/$PLATFORM/netplay.elf" --nudge
exec "$pak/bin/$PLATFORM/netplay.elf" --prompt "$@"
