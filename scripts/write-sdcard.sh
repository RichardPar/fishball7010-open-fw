#!/usr/bin/env bash
# Format an SD card with one 120MB FAT32 partition and copy SDCARD-new onto it.
# Usage: sudo ./write-sdcard.sh /dev/mmcblk0
set -euo pipefail
DEV=${1:?usage: sudo $0 /dev/mmcblkN <boot-set-dir>}
SRC=$(realpath "${2:?usage: sudo $0 /dev/mmcblkN <boot-set-dir>}")

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }
[ -b "$DEV" ] || { echo "$DEV is not a block device"; exit 1; }
case "$(basename "$DEV")" in mmcblk[0-9]) ;; *) echo "only whole mmcblk devices accepted"; exit 1;; esac
# built-in SD slots often report removable=0, so check the things that matter instead
[ -z "$(lsblk -nro MOUNTPOINT "$DEV" | tr -d '[:space:]')" ] || { echo "$DEV has mounted partitions - unmount first"; exit 1; }
[ "$(blockdev --getsize64 "$DEV")" -le $((64*1024**3)) ] || { echo "$DEV is larger than 64GB - refusing"; exit 1; }
for f in BOOT.bin uImage devicetree.dtb uramdisk.image.gz uEnv.txt; do [ -f "$SRC/$f" ] || { echo "missing $SRC/$f"; exit 1; }; done

lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "$DEV"
read -rp "ERASE ALL DATA on $DEV and write the new Pluto image? type YES: " a
[ "$a" = YES ] || { echo aborted; exit 1; }

umount "${DEV}"* 2>/dev/null || true
wipefs -a "$DEV"
# one FAT32 (LBA) partition, 1MiB aligned, 120MiB like the original
parted -s "$DEV" mklabel msdos mkpart primary fat32 1MiB 121MiB set 1 lba on
partprobe "$DEV"; sleep 2
P="${DEV}p1"; [ -b "$P" ] || P="${DEV}1"
mkfs.vfat -F 32 -n PLUTOBOOT "$P"
M=$(mktemp -d); mount "$P" "$M"
cp "$SRC"/BOOT.bin "$SRC"/uImage "$SRC"/devicetree.dtb "$SRC"/uramdisk.image.gz "$SRC"/uEnv.txt "$M"/
sync; ls -l "$M"; umount "$M"; rmdir "$M"
echo "Done. Power off the Pluto, insert the card, and set it to boot from SD (see notes)."
