#!/bin/sh
#
# Builds the 32v64 harness for one spruce device into build/32v64/TARGET/:
# determinism, statecheck and corecmp as .32 (armhf, the A30 toolchain) and
# .64 (aarch64), plus the LD_PRELOAD helpers. The 64-bit toolchain follows
# the device the run is on: TARGET=h700 (default) or TARGET=tg5040 (Brick).
#
#   [TARGET=tg5040] testing/spruce/32v64/build.sh
#
# Then copy build/32v64/* to /mnt/SDCARD/cc3264/bin on the device along with
# this folder's scripts and cell lists (see README.md).

set -e
cd "$(dirname "$0")/../../.."
TARGET=${TARGET:-h700}
case $TARGET in
h700) IMAGE64=ghcr.io/loveretro/h700-toolchain:latest ;;
tg5040) IMAGE64=ghcr.io/loveretro/tg5040-toolchain:latest ;;
*) echo "TARGET: h700 or tg5040" >&2; exit 2 ;;
esac
OUT=build/32v64/$TARGET
mkdir -p "$OUT"
H=testing/spruce/32v64
I="-I shim/include"

build() { # image, suffix, extra preload source (or "")
	docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -w /src "$1" sh -c "
		set -e; CC=\${CROSS_COMPILE}gcc
		\$CC -O2 -std=gnu99 $I shim/test/determinism.c -o $OUT/determinism.$2 -ldl
		\$CC -O2 -std=gnu99 $I shim/test/statecheck.c -o $OUT/statecheck.$2 -ldl
		\$CC -O2 -std=gnu99 $I testing/corecmp/corecmp.c -o $OUT/corecmp.$2 -ldl
		\$CC -O2 -shared -fPIC $H/fixedtime.c -o $OUT/fixedtime.$2.so -ldl
		for s in $3; do \$CC -O2 -shared -fPIC $H/\$s.c -o $OUT/\$s.$2.so -ldl; done"
}

build nextui-my282-toolchain:local 32 "alignfix busdump"
build "$IMAGE64" 64 ""
ls -la "$OUT"
