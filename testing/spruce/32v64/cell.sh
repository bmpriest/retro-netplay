#!/bin/sh
#
# cell.sh NAME CORE ROM OPT [EXTRA]
#
# Runs determinism and statecheck on spruce's 32-bit (cores/) and 64-bit
# (cores64/) builds of CORE, on this device, and writes logs to out/NAME for
# judge.py. OPT is a RetroArch .opt file (or -) whose values the core is given;
# EXTRA is more KEY=VALUE; pairs.
#
# Per build: determinism twice with the wall clock pinned (self-determinism)
# and once on the real clock (does the core read the time?); statecheck saves
# at SAVE_AT and probes round trip, serialize side effects and host/client
# amplification. Then every build loads every build's state and runs CONT more
# frames (the four cross loads).
#
# Environment:
#   FRAMES (3600) SAVE_AT (1800) CONT (600)
#   LIB32     LD_LIBRARY_PATH for the 32-bit runs (e.g. a copy of
#             spruce/a30/lib, which has GLIBCXX_3.4.32 for armGBA/SwanStation)
#   PRELOAD32 alignfix | busdump: on an arm64 kernel, emulate the 32-bit
#             kernel's misaligned LDM/STM fixups, or just report the fault
#   TIMEOUT   seconds per run (900; ignored where there is no timeout(1))
#
# Split across two devices (32-bit build on a 32-bit device, 64-bit on a
# 64-bit one), each device runs only its own build:
#   ARCHS     builds to run here: "32 64" (default), "32" or "64"
#   PHASE     all (default) | native (cold boots + statecheck save) |
#             cross (the cross loads; needs both st32.bin and st64.bin in
#             out/NAME, so copy the other device's over first)
# Each phase leaves out/NAME/done.PHASE.ARCHS; PHASE=all leaves out/NAME/done.

ROOT=/mnt/SDCARD/cc3264
B=$ROOT/bin
RA=/mnt/SDCARD/RetroArch/.retroarch
name=$1 core=$2 rom=$3 opt=$4 extra=$5
O=$ROOT/out/$name
ARCHS=${ARCHS:-32 64}
PHASE=${PHASE:-all}
[ "$PHASE" = cross ] || rm -rf "$O"
mkdir -p "$O"
F=${FRAMES:-3600}
SAVE_AT=${SAVE_AT:-1800}
CONT=${CONT:-600}
T=${TIMEOUT:-900}

vars=""
if [ -n "$opt" ] && [ "$opt" != "-" ] && [ -f "$opt" ]; then
	vars=$(sed -n 's/^\([A-Za-z0-9_.-]*\) *= *"\(.*\)"$/\1=\2/p' "$opt" | tr '\n' ';')
fi
# extras first: the tools take the first value for a key and keep only the
# first 32, so appended extras could be lost (PCSX-ReARMed.opt has 79)
[ -n "$extra" ] && vars="$extra$vars"
export STATECHECK_VARS="$vars"
export SYSDIR=$ROOT/sys
# fixedtime.so pins the clock, but RTC games convert it to local time: devices
# in different time zones would see different hours.
export TZ=UTC0
[ "$PHASE" = cross ] || printf '%s\n%s\n%s\n%s\n' "$core" "$rom" "$opt" "$vars" > "$O/cell.txt"

dir() { [ "$1" = 32 ] && echo "$RA/cores" || echo "$RA/cores64"; }
# extra preload for a build: the alignment helper for 32-bit, nothing for 64
extra_pre() { [ "$1" = 32 ] && [ -n "$PRELOAD32" ] && echo "$B/$PRELOAD32.32.so"; }
# run ARCH LOGBASE PRELOAD CMD...: on 32-bit with LIB32 set, use those libraries
run() {
	a=$1 log=$2 pre=$3; shift 3
	# GNU/new BusyBox: timeout -s SIG SECS; old BusyBox (Miyoo Mini): -t SECS.
	if timeout -s KILL 5 true 2>/dev/null; then set -- timeout -s KILL "$T" "$@"
	elif timeout -t 5 -s KILL true 2>/dev/null; then set -- timeout -t "$T" -s KILL "$@"
	fi
	if [ "$a" = 32 ] && [ -n "$LIB32" ]; then
		LD_LIBRARY_PATH=$LIB32 LD_PRELOAD=$pre "$@" > "$log.log" 2>&1
	else
		LD_PRELOAD=$pre "$@" > "$log.log" 2>&1
	fi
	echo $? > "$log.rc"
}

t0=$(date +%s)
[ "$PHASE" = cross ] || for A in $ARCHS; do
	C="$(dir $A)/$core"
	X=$(extra_pre $A)
	PRE="$B/fixedtime.$A.so${X:+ $X}"
	export CHECKPOINT=300
	run $A "$O/det$A.a" "$PRE" "$B/determinism.$A" "$C" "$rom" "$F" "$O/det$A.a.bin"
	run $A "$O/det$A.b" "$PRE" "$B/determinism.$A" "$C" "$rom" "$F" "$O/det$A.b.bin"
	run $A "$O/det$A.clock" "$X" "$B/determinism.$A" "$C" "$rom" "$F"
	run $A "$O/sc$A" "$PRE" "$B/statecheck.$A" "$C" "$rom" "$SAVE_AT" --save "$O/st$A.bin" \
		--serprobe 300 --amplify 300
done
[ "$PHASE" = native ] || for S in 32 64; do
	for A in $ARCHS; do
		X=$(extra_pre $A)
		run $A "$O/x$S-$A" "$B/fixedtime.$A.so${X:+ $X}" "$B/statecheck.$A" "$(dir $A)/$core" "$rom" "$CONT" \
			--load "$O/st$S.bin" --save "$O/x$S-$A.bin"
	done
done
rm -f "$O"/det*.b.bin
if [ "$PHASE" = all ]; then
	echo $(( $(date +%s) - t0 )) > "$O/seconds"
	echo done > "$O/done"
else
	echo $(( $(date +%s) - t0 )) > "$O/seconds.$PHASE.$(echo $ARCHS | tr ' ' _)"
	echo done > "$O/done.$PHASE.$(echo $ARCHS | tr ' ' _)"
fi
