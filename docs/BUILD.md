# Build instructions

Goal: an SD card boot set that runs Tezuka + Maia SDR on a Fishball-7010-class board with the optimised classifier.
The card holds five files: `BOOT.bin`, `uImage`, `devicetree.dtb`, `uramdisk.image.gz`, `uEnv.txt`.

## Host tools
`mkimage` (u-boot-tools), `dtc`, `xz`, `cpio`, `gcc`, `arm-linux-gnueabihf-gcc` (static libc), `python3` with `numpy scipy matplotlib` (tests only).

## 1. Take what the vendor card already has
Copy `BOOT.bin` and `uEnv.txt` from the board's original SD card **unchanged**. `BOOT.bin` carries the FSBL (DDR/MIO setup for this exact board),
the ADI U-Boot and the vendor bitstream (replaced at boot by step 3). Do not regenerate it with bootgen: a 2026 bootgen build wrote zeros for the
FSBL length and U-Boot load address and the board did not boot. Keep a copy of the whole original card as the fallback.

## 2. Get the Tezuka pieces (fishball target)
Either use a release from <https://github.com/F5OEO/tezuka_fw/releases> (fishball / Z7010) or build it (`./build.sh fishball` in that repo).
You need:
- the kernel as a raw, **uncompressed** `Image` (the release `uImage` is LZMA-wrapped: `dumpimage -T kernel -o Image.lzma uImage && unlzma Image.lzma`;
  the vendor U-Boot has no LZMA support)
- `system_top.bit` (unzip it from `tezuka.xsa`, board `fishball7010`)
- the fishball `devicetree.dtb`
- the root filesystem `cpio.xz`; wrap it if it has no U-Boot header (`mkimage -l` shows one if present):
  `mkimage -A arm -O linux -T ramdisk -C none -a 0 -e 0 -n tezuka -d rootfs.cpio.xz uramdisk.image.gz`

## 3. Assemble the boot set
```sh
scripts/make-fit.sh Image system_top.bit set/uImage          # FIT: kernel + FPGA bitstream (fpga@1 loaded by U-Boot)
scripts/dtb-bootargs.sh fishball.dtb set/devicetree.dtb      # kernel command line lives in the DTB (see below)
cp uramdisk.image.gz set/uramdisk.image.gz
cp vendor/BOOT.bin vendor/uEnv.txt set/
```
Why these shapes: the vendor `uEnv.txt` hook `if test -n $uenvcmd` never fires for multi-word values and `setenv bootargs` is ignored on this
sdboot path, so the FPGA is loaded from the FIT and the bootargs (including `uio_pdrv_genirq.of_id=uio_pdrv_genirq`, needed for Maia's UIO devices,
and `maxcpus=2`) are baked into the DTB.

## 4. Build and install the classifier
```sh
cd classifier && ./build.sh                  # produces ./classifier (static ARM, ~420 kB) and ./classifier-native
mkdir -p /tmp/ov/usr/bin && cp classifier /tmp/ov/usr/bin/
cd .. && scripts/patch-ramdisk.sh set/uramdisk.image.gz /tmp/ov set/uramdisk.image.gz "tezuka classifier v2"
```
`patch-ramdisk.sh` appends the overlay as a second cpio archive; later files win. The stock `S61classifier` init script already starts
`/usr/bin/classifier`; it only runs if the dashboard flag `classifier_enabled` is 1 (stored in the U-Boot environment in flash).
Optional CPU-tuning patches for the Tezuka shell scripts are in `patches/` (`*.patch`, applied to the files under `/root` in the rootfs; use the
same overlay mechanism). See `docs/NOTES.md` for how each was measured.

## 5. Write the card
```sh
sudo scripts/write-sdcard.sh /dev/mmcblk0 set/        # FAT32, 120 MiB, label PLUTOBOOT; asks for YES before erasing
```
Or copy the five files onto an existing FAT32 card. Set the board's boot-mode switch to SD, power on, browse to https://192.168.2.1
(use http:// for the temperature readouts). Root login: whatever password database the radio's flash holds (see `docs/NOTES.md`).

## Classifier tests (no radio needed)
```sh
cd classifier
python3 spec_dump.py fm98                                   # Python reference output for a capture
./classifier-native -A 0 -t captures/fm98.bin -F <fc_Hz> -S <fs_Hz> -B <rfbw_Hz> -N <floats_per_frame>   # values are in captures/fm98.json
python3 synth_test.py                                       # synthetic signals + noise false-alarm checks
```
With `-A 0` (no Wi-Fi clustering) the C output equals `spec_dump.py` line for line. On a live radio:
`python3 mqtt_watch.py 10 'state/classifier/#'`, or run `./classifier-native -H <radio-ip> -v` on the PC for a read-only view of what it detects.

## Options of `classifier`
`-H host -p port -P path` (maia-httpd, default localhost:80 /waterfall), `-c 0.40` confidence floor for the primary label, `-i 1.0` interval (s),
`-A 0|1` Wi-Fi clustering, `-v` verbose; `-f`/`-m` are accepted and ignored (old template options).
