#!/bin/sh
#
# batch.sh CELLS [JOBS]
#
# Runs every cell in CELLS, JOBS (3) at a time, skipping cells already done,
# and logs start/end times to progress.txt. Run on the device from
# /mnt/SDCARD/cc3264, under nohup so it survives the SSH session.
#
# A CELLS line is  NAME|CORE|ROM|OPT|EXTRA
#   ROM   relative to roms/, or @/absolute/path
#   OPT   relative to RetroArch's config/ folder (or OPTDIR), or -
#   EXTRA more core option KEY=VALUE; pairs
# Blank lines and # comments are skipped.
#
# RUN=corecmp-pair.sh runs the corecmp traces instead of cell.sh. Everything
# else in the environment (LIB32, PRELOAD32, FRAMES...) passes through.

cd /mnt/SDCARD/cc3264 || exit 1
J=${2:-3}
RUN=${RUN:-cell.sh}
case $RUN in corecmp-pair.sh) OUTDIR=cc ;; *) OUTDIR=out ;; esac
# cell.sh's marker for this ARCHS/PHASE (see cell.sh)
MARK=done
[ "${PHASE:-all}" = all ] || MARK=done.$PHASE.$(echo ${ARCHS:-32 64} | tr ' ' _)
CFG=${OPTDIR:-/mnt/SDCARD/RetroArch/.retroarch/config}
n=0
while IFS='|' read -r name core rom opt extra; do
	case $name in ''|'#'*) continue ;; esac
	[ -f "$OUTDIR/$name/$MARK" ] && continue
	case "$rom" in @*) rom=${rom#@} ;; *) rom="/mnt/SDCARD/cc3264/roms/$rom" ;; esac
	[ "$opt" = "-" ] || opt="$CFG/$opt"
	echo "$(date +%T) start $name" >> progress.txt
	(
		sh "bin/$RUN" "$name" "$core" "$rom" "$opt" "$extra"
		echo "$(date +%T) end $name $(cat "$OUTDIR/$name/seconds"* 2>/dev/null | tail -1)s" >> progress.txt
	) &
	n=$((n + 1))
	if [ $n -ge "$J" ]; then wait; n=0; fi
done < "$1"
wait
echo "$(date +%T) ALL DONE $1" >> progress.txt
