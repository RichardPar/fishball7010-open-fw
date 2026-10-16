#!/bin/bash
# Build the complete SD-card boot set for a Fishball-7010-class board: Tezuka + Maia SDR with the label fixes, the CPU-tuned status scripts
# and classifier v2. Nothing is downloaded; you supply the inputs.
#
#   scripts/build-all.sh --vendor <dir> --tezuka <release.zip|dir> --xsa <tezuka.xsa> [options]
#
# Inputs
#   --vendor DIR     directory with BOOT.bin and uEnv.txt copied unchanged from the board's ORIGINAL SD card
#   --tezuka PATH    Tezuka fishball release (the .zip, or the unpacked folder) from https://github.com/F5OEO/tezuka_fw/releases
#                    (needs sdimg/uImage, sdimg/devicetree.dtb, sdimg/uramdisk.image.xz)
#   --xsa FILE       tezuka.xsa of the fishball7010 board (tezuka_fw/board/tezuka/fishball7010/bitstream/tezuka.xsa); the release does not
#                    ship the .bit that the FIT needs
# Options
#   --out DIR        output folder for the five card files (default: build/set)
#   --no-tuning      keep Tezuka's stock api_controller.sh / watchconsoletx.sh / update.sh
#   --no-classifier  keep Tezuka's stock classifier instead of building classifier v2
#   --write DEV      afterwards format and write the card with scripts/write-sdcard.sh (needs sudo; asks you to type YES)
#   -h, --help
#
# Host tools: mkimage (u-boot-tools), dtc, xz, cpio, unzip, python3, and for the classifier gcc + arm-linux-gnueabihf-gcc.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(dirname "$HERE")
OUT="$REPO/build/set"; VENDOR=""; TEZUKA=""; XSA=""; TUNING=1; CLASSIFIER=1; WRITE=""

usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit "${1:-0}"; }
die() { echo "error: $*" >&2; exit 1; }
step() { echo; echo "== $*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --vendor) VENDOR=${2:?}; shift 2;;
    --tezuka) TEZUKA=${2:?}; shift 2;;
    --xsa) XSA=${2:?}; shift 2;;
    --out) OUT=${2:?}; shift 2;;
    --no-tuning) TUNING=0; shift;;
    --no-classifier) CLASSIFIER=0; shift;;
    --write) WRITE=${2:?}; shift 2;;
    -h|--help) usage 0;;
    *) echo "unknown option: $1" >&2; usage 1;;
  esac
done
[ -n "$VENDOR" ] && [ -n "$TEZUKA" ] && [ -n "$XSA" ] || { echo "missing --vendor, --tezuka or --xsa" >&2; usage 1; }

step "checking tools and inputs"
need="mkimage dtc xz cpio unzip python3 sha1sum"; [ "$CLASSIFIER" = 1 ] && need="$need gcc arm-linux-gnueabihf-gcc arm-linux-gnueabihf-strip"
for t in $need; do command -v "$t" >/dev/null || die "missing tool: $t"; done
for f in BOOT.bin uEnv.txt; do [ -f "$VENDOR/$f" ] || die "$VENDOR/$f not found (copy it from the original SD card)"; done
[ -f "$XSA" ] || die "xsa not found: $XSA"

W="$REPO/build/work"; rm -rf "$W"; mkdir -p "$W" "$OUT"
if [ -d "$TEZUKA" ]; then REL=$TEZUKA
else
  [ -f "$TEZUKA" ] || die "tezuka release not found: $TEZUKA"
  unzip -q -o "$TEZUKA" -d "$W/release"; REL="$W/release"
fi
SD=$(find "$REL" -type d -name sdimg | head -1); [ -n "$SD" ] || die "no sdimg/ folder inside $TEZUKA"
for f in uImage devicetree.dtb uramdisk.image.xz; do [ -f "$SD/$f" ] || die "$SD/$f missing"; done
echo "release: $SD"

step "kernel + Maia bitstream -> FIT uImage"
# the release uImage is LZMA-compressed; the vendor U-Boot has no LZMA support, so store the kernel raw
dd if="$SD/uImage" bs=64 skip=1 status=none | xz -dc -F lzma > "$W/Image"
unzip -p "$XSA" system_top.bit > "$W/system_top.bit" || die "system_top.bit not found in $XSA"
"$HERE/make-fit.sh" "$W/Image" "$W/system_top.bit" "$OUT/uImage" 2>&1 | grep -v "unit_address_vs_reg\|unit addresses"

step "device tree (bootargs, AD9363 label)"
"$HERE/dtb-bootargs.sh" "$SD/devicetree.dtb" "$OUT/devicetree.dtb"

step "ramdisk overlay"
dd if="$SD/uramdisk.image.xz" bs=64 skip=1 status=none | xz -dc > "$W/stock.cpio"
OV="$W/overlay"; mkdir -p "$OV"
# 1. label fixes (served dashboard and the boot script that rewrote the model back to AD9361)
python3 "$HERE/cpio-extract.py" "$W/stock.cpio" "$OV" root/index.html etc/init.d/S23udc
sed -i -e 's/AD9361/AD9363/g; s/Zynq-7020/Zynq-7010/g; s/XC7Z020/XC7Z010/g' "$OV/root/index.html"
python3 - "$OV/etc/init.d/S23udc" <<'PY'
import sys
p = sys.argv[1]; t = open(p).read()
old = '\t\tMODEL=$(echo "$model" | sed "s/AD936[34]/$AD936X_TYPE_CAP/")\n'
new = ('\t\t# Keep the board name as given in the device tree (AD9363). The driver stays in ad9361 mode for the 70 MHz-6 GHz range.\n'
       '\t\tMODEL=$model\n')
if old not in t: sys.exit('S23udc: expected line not found (different Tezuka version?)')
open(p, 'w').write(t.replace(old, new))
PY
echo "labels: AD9363 / Zynq-7010"
# 2. CPU-tuned scripts; a patch is only applied if the stock file is the exact version it was written against
if [ "$TUNING" = 1 ]; then
  P="$REPO/patches"
  python3 "$HERE/cpio-extract.py" "$W/stock.cpio" "$W/orig" root/api_controller.sh root/watchconsoletx.sh sbin/update.sh
  apply() {  # <path in rootfs> <original in patches/> <optimised in patches/>
    if [ "$(sha1sum < "$W/orig/$1")" = "$(sha1sum < "$P/$2")" ]; then
      mkdir -p "$OV/$(dirname "$1")"; cp "$P/$3" "$OV/$1"; chmod 755 "$OV/$1"; echo "tuned: $1"
    else echo "skipped: $1 differs from the version the patch was written for (Tezuka $(basename "$REL")); not applying"; fi
  }
  apply root/api_controller.sh   api_controller.original.sh   api_controller.optimised-v8.sh
  apply root/watchconsoletx.sh   watchconsoletx.original.sh   watchconsoletx.optimised.sh
  apply sbin/update.sh           update.original.sh           update.optimised.sh
fi
# 3. classifier v2
if [ "$CLASSIFIER" = 1 ]; then
  (cd "$REPO/classifier" && ./build.sh >/dev/null) || die "classifier build failed"
  mkdir -p "$OV/usr/bin"; install -m 755 "$REPO/classifier/classifier" "$OV/usr/bin/classifier"
  echo "classifier v2: $(wc -c < "$OV/usr/bin/classifier") bytes (static ARM)"
fi
find "$OV" -type d -exec chmod 755 {} +
"$HERE/patch-ramdisk.sh" "$SD/uramdisk.image.xz" "$OV" "$OUT/uramdisk.image.gz" "Tezuka fishball + fixes"

step "vendor files"
install -m 644 "$VENDOR/BOOT.bin" "$VENDOR/uEnv.txt" "$OUT/"

step "result"
ls -l "$OUT"
mkimage -l "$OUT/uImage" 2>/dev/null | sed -n '1,3p'
echo
echo "Boot set ready in $OUT"
echo "Write it:  sudo scripts/write-sdcard.sh /dev/mmcblkN $OUT      (formats the card)"
echo "  or copy the five files onto an existing FAT32 card (see README, 'Write the SD card')."
if [ -n "$WRITE" ]; then step "writing $WRITE"; sudo "$HERE/write-sdcard.sh" "$WRITE" "$OUT"; fi
