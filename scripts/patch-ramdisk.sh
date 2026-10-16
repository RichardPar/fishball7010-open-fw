#!/bin/sh
# Overlay files onto a U-Boot ramdisk image (uImage wrapper around an xz-compressed cpio) without unpacking the root filesystem.
# usage: patch-ramdisk.sh <in uramdisk.image.gz> <overlay-dir> <out uramdisk.image.gz> ["image name"]
# Files in <overlay-dir> are appended as a second newc cpio archive (owner root); the kernel unpacks concatenated archives in order,
# so later entries overwrite earlier ones. Needs: mkimage (u-boot-tools), xz, cpio.
set -eu
IN=${1:?input image}; OV=${2:?overlay dir}; OUT=${3:?output image}; NAME=${4:-patched ramdisk}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
dd if="$IN" of="$T/in.xz" bs=64 skip=1 status=none            # strip the 64-byte uImage header
xz -dc "$T/in.xz" > "$T/in.cpio"
( cd "$OV" && find . | cpio -o -H newc -R 0:0 --quiet ) > "$T/extra.cpio"
# CRC32 (not the xz default CRC64) because the kernel's built-in xz decoder is built without CRC64
cat "$T/in.cpio" "$T/extra.cpio" | xz -6 -T2 --check=crc32 > "$T/out.xz"
mkimage -A arm -O linux -T ramdisk -C none -a 0 -e 0 -n "$NAME" -d "$T/out.xz" "$OUT" > /dev/null
echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
