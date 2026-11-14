#!/bin/sh
# Host-side checks for the USB cable framing and status contract.
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp /tmp/netplay-cableproto.XXXXXX)"
trap 'rm -f "$out"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -Wall -Wextra -Werror -Iapp \
    app/cableproto.c testing/cableproto-unit.c -o "$out"
"$out"
