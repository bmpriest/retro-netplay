#!/bin/sh
# Empty shared libraries named as netplay.elf's NEEDED entries, for a spruce
# device that has none of them (the Mini Flip: no GPU, and SDL only inside
# spruce's PyUI). On spruce netplay.elf never draws - the PyUI front end does -
# and binds every SDL/GL call lazily, so the loader only needs the names.
#
#   stub-libs.sh OUT_DIR CC LIB...
out=$1 cc=$2; shift 2
mkdir -p "$out" || exit 1
echo > "$out/stub.c"
for lib; do
	"$cc" -shared -nostdlib -Wl,-soname,"$lib" -o "$out/$lib" "$out/stub.c" || exit 1
done
rm -f "$out/stub.c"
