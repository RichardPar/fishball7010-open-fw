#!/bin/sh
# Build the uImage the vendor U-Boot expects: a FIT holding the kernel (uncompressed Image) and the FPGA bitstream.
# usage: make-fit.sh <Image> <system_top.bit> <out uImage>
# The vendor U-Boot has no LZMA support, so the kernel must be stored uncompressed; the FPGA is loaded from the FIT ("fpga = fpga@1").
set -eu
IMG=$(realpath "${1:?kernel Image}"); BIT=$(realpath "${2:?system_top.bit}"); OUT=$(realpath -m "${3:?output}")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
sed "s#@IMAGE@#$IMG#; s#@BIT@#$BIT#" "$(dirname "$0")/maia.its.in" > "$T/maia.its"
mkimage -f "$T/maia.its" "$OUT" > /dev/null
echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
