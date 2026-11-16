#!/bin/sh
set -eu

# build-gearsystem.sh armv7|aarch64 SRC OUT
#
# Gearsystem with its Game Gear link (3.9.20+): two consoles and the
# Gear-to-Gear cable inside one core. Netplay runs it as a shared-screen
# lockstep session in which each device shows its own console, so the cable
# never leaves the process and only inputs cross the network.
#
# One build per word size serves every device, like the compatibility cores:
# both ends of a session must run identical code (and the two builds compute
# the same game, so a 32-bit device can face a 64-bit one). Its version names the
# Netplay patch set, as the core reports it to the identity check. libstdc++ is static because
# the devices' runtimes differ.
usage='usage: build-gearsystem.sh armv7|aarch64 SRC OUT'
arch=${1:?$usage}
src=${2:?$usage}
out=${3:?$usage}
dir="$src/platforms/libretro"
case "$arch" in
	armv7)   flags='-O2 -fPIC -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard' ;;
	aarch64) flags='-O2 -fPIC -march=armv8-a -mcpu=cortex-a53' ;;
	*) echo "$usage" >&2; exit 2 ;;
esac
make -C "$dir" platform=unix clean >/dev/null 2>&1 || true
CFLAGS="$flags" CXXFLAGS="$flags" LDFLAGS='-static-libstdc++ -static-libgcc' \
	make -C "$dir" platform=unix CC="${CROSS_COMPILE}gcc" CXX="${CROSS_COMPILE}g++" \
	AR="${CROSS_COMPILE}ar" GIT_VERSION="${GEARSYSTEM_VERSION:-3.9.25-netplay1}" -j"${JOBS:-4}"
mkdir -p "$(dirname "$out")"
cp "$dir/gearsystem_libretro.so" "$out"
"${CROSS_COMPILE}strip" --strip-unneeded "$out"
