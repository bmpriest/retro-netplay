#!/bin/sh
#
# corecmp-pair.sh NAME CORE ROM OPT [EXTRA]
#
# Per-frame traces (RAM, video, audio, state) of spruce's 32-bit (A) and
# 64-bit (B) builds of CORE, in testing/corecmp's run-pair layout, written to
# cc/NAME. Compare them on the PC with testing/corecmp/compare.py. Used where
# whole-state hashes are noise (gambatte, gearboy, Genesis Plus GX, chimera,
# pcsx): the traces show whether the game itself differs.
#
#   a, a2   A twice from power-on          b, b2   B twice from power-on
#   aa, bb  each loads its own state at H  ab, ba  each loads the other's
#
# Split across two devices like cell.sh: ARCHS (32 = A, 64 = B) picks the
# builds run here; PHASE=native runs a/a2/aa or b/b2/bb, PHASE=cross runs ab
# or ba and needs the other device's a.state/b.state in cc/NAME first.
# Environment as cell.sh: FRAMES (3600), SAVE_AT (1800), LIB32, PRELOAD32.

ROOT=/mnt/SDCARD/cc3264
B=$ROOT/bin
RA=/mnt/SDCARD/RetroArch/.retroarch
name=$1 core=$2 rom=$3 opt=$4 extra=$5
out=$ROOT/cc/$name
ARCHS=${ARCHS:-32 64}
PHASE=${PHASE:-all}
[ "$PHASE" = cross ] || rm -rf "$out"
mkdir -p "$out"
vars=""
[ -n "$opt" ] && [ "$opt" != "-" ] && [ -f "$opt" ] &&
	vars=$(sed -n 's/^\([A-Za-z0-9_.-]*\) *= *"\(.*\)"$/\1=\2/p' "$opt" | tr '\n' ';')
# extras first: the tools take the first value for a key and keep only the
# first 32 (corecmp: 1024 bytes), so appended extras could be lost
export CORECMP_VARS="$extra$vars" SYSDIR=$ROOT/sys
# fixedtime.so pins the clock, but RTC games convert it to local time: devices
# in different time zones would see different hours.
export TZ=UTC0
F=${FRAMES:-3600} H=${SAVE_AT:-1800}
A32="$RA/cores/$core" A64="$RA/cores64/$core"
here() { case " $ARCHS " in *" $1 "*) return 0 ;; esac; return 1; }
r() {
	n=$1 a=$2 c=$3; shift 3
	X=""; [ "$a" = 32 ] && [ -n "$PRELOAD32" ] && X=" $B/$PRELOAD32.32.so"
	if [ "$a" = 32 ] && [ -n "$LIB32" ]; then
		LD_LIBRARY_PATH=$LIB32 LD_PRELOAD="$B/fixedtime.$a.so$X" "$B/corecmp.$a" "$c" "$rom" "$F" \
			"$out/$n.trace" "$@" > "$out/$n.log" 2>&1
	else
		LD_PRELOAD="$B/fixedtime.$a.so$X" "$B/corecmp.$a" "$c" "$rom" "$F" "$out/$n.trace" "$@" \
			> "$out/$n.log" 2>&1
	fi
	echo $? > "$out/$n.rc"
}
t0=$(date +%s)
if [ "$PHASE" != cross ]; then
	if here 32; then
		r a  32 "$A32" --save-at "$H" "$out/a.state"
		r a2 32 "$A32"
		r aa 32 "$A32" --load "$out/a.state" "$H"
	fi
	if here 64; then
		r b  64 "$A64" --save-at "$H" "$out/b.state"
		r b2 64 "$A64"
		r bb 64 "$A64" --load "$out/b.state" "$H"
	fi
	printf '%s\n%s\n%s\n%s %s\n' "$A32" "$A64" "$rom" "$F" "$H" > "$out/pair.txt"
fi
if [ "$PHASE" != native ]; then
	here 32 && r ab 32 "$A32" --load "$out/b.state" "$H"
	here 64 && r ba 64 "$A64" --load "$out/a.state" "$H"
fi
if [ "$PHASE" = all ]; then
	echo $(( $(date +%s) - t0 )) > "$out/seconds"
	echo done > "$out/done"
else
	echo $(( $(date +%s) - t0 )) > "$out/seconds.$PHASE.$(echo $ARCHS | tr ' ' _)"
	echo done > "$out/done.$PHASE.$(echo $ARCHS | tr ' ' _)"
fi
