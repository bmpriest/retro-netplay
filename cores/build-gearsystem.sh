#!/bin/sh
set -eu

# build-gearsystem.sh SRC OUT
#
# Gearsystem with its Game Gear link (3.9.20+): two consoles and the
# Gear-to-Gear cable inside one core. Netplay runs it as a shared-screen
# lockstep session in which each device shows its own console, so the cable
# never leaves the process and only inputs cross the network.
#
# One AArch64 build serves every 64-bit device, like the compatibility cores:
# both ends of a session must run identical code. Its version names the
# Netplay patch set, as the core reports it to the identity check. libstdc++ is static because
# the devices' runtimes differ.
src=${1:?usage: build-gearsystem.sh SRC OUT}
out=${2:?usage: build-gearsystem.sh SRC OUT}
dir="$src/platforms/libretro"
flags='-O2 -fPIC -march=armv8-a -mcpu=cortex-a53'
make -C "$dir" platform=unix clean >/dev/null 2>&1 || true
CFLAGS="$flags" CXXFLAGS="$flags" LDFLAGS='-static-libstdc++ -static-libgcc' \
	make -C "$dir" platform=unix CC="${CROSS_COMPILE}gcc" CXX="${CROSS_COMPILE}g++" \
	AR="${CROSS_COMPILE}ar" GIT_VERSION="${GEARSYSTEM_VERSION:-3.9.25-netplay1}" -j"${JOBS:-4}"
mkdir -p "$(dirname "$out")"
cp "$dir/gearsystem_libretro.so" "$out"
"${CROSS_COMPILE}strip" --strip-unneeded "$out"
