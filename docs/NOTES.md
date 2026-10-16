# Pluto-clone (Zynq-7010 + AD9363, 2T2R) open firmware rebuild

Status: rebuilt firmware boots from SD and works (USB + network, RX-A verified at 800-900 MHz,
TX1-A verified at low power). Not yet tested: RX-B, TX2, gigabit Ethernet, SD slot inside the radio,
>3.8 GHz, flashing to internal QSPI.

## Layout
- `SDCARD/`        original files from the vendor SD card (untouched, known-good fallback)
- `SDCARD-new/`    rebuilt boot set: BOOT.bin (vendor original), uImage, devicetree.dtb, uramdisk.image.gz, uEnv.txt
- `extracted/`     blobs/sources pulled from the vendor BOOT.bin and dtb (fsbl.elf, system_top.bit.bin, u-boot.elf, devicetree.dts)
- `plutosdr-fw/`   ADI plutosdr-fw v0.39 + pinned linux / u-boot-xlnx / buildroot / hdl trees (build/ holds outputs)
- `results/`       RF test plots (869 MHz capture, TX tone test, 800-900 MHz sweep and survey)
- `write-sdcard.sh` partition + format + copy SDCARD-new to a card (run with sudo)

Removed after use: Xilinx bootgen (clone github.com/Xilinx/bootgen if needed) and F5OEO tezuka_fw
(reference only; github.com/F5OEO/tezuka_fw, Maia-based, not used).

## Findings
- Vendor card = ADI v0.39-era firmware. U-Boot rebuilt from public source (commit 90401ce9ce) is
  byte-identical to the vendor's. FSBL and bitstream are vendor binaries, reused unchanged.
- Vendor device tree recompiles byte-identical from extracted/devicetree.dts.
- Vendor kernel was Linux 5.15; the rebuild is the ADI 6.1 kernel pinned by v0.39. Both boot.
- Driver reports "ad9361" only because the dtb compatible says so; the driver cannot tell
  AD936x variants apart (product ID 0x08 for all).

## Rebuild
    cd plutosdr-fw
    export CPATH=<dir with gmp.h/mpc.h/mpfr.h>      # libgmp-dev etc. (kernel gcc plugin)
    make build/rootfs.cpio.gz                        # Buildroot + legal-info (needs legal-info for LICENSE.html)
    make build/zImage build/u-boot.elf
Then wrap with mkimage: uImage (-T kernel -a 0x8000 -e 0x8000 -C none), uramdisk.image.gz (-T ramdisk -C gzip),
dtb compiled from extracted/devicetree.dts.

## Warning: do not use bootgen's output as BOOT.bin
bootgen (2026 build) wrote zeros for the FSBL length and U-Boot load/exec address in the Zynq header,
so the board did not boot. Use the vendor BOOT.bin; its three payloads are byte-identical to the rebuild.
The bad image is kept as extracted/BOOT.bin.bootgen-bad.

## GPL material
Source for every Buildroot package + licences: plutosdr-fw/buildroot/output/legal-info
Kernel/U-Boot/Buildroot/HDL: public ADI repos at the commits pinned by plutosdr-fw v0.39.
Only closed piece: the vendor FPGA bitstream (extracted/system_top.bit.bin).

## RF test notes
libiio host tools were unpacked from Ubuntu .debs into the session scratchpad (no root needed);
radio is at ip:192.168.2.1. TX tests used -70 to -80 dB attenuation; always switch the DDS off after.
Root login on this build is root / analog (public ADI default).


## Maia SDR on this board (working, Oct 2026)
The board is a **Fishball 7010** clone (HamGeek / OpenSourceSDR Lab; Zynq 7z010clg400): its vendor FSBL MIO/DDR init
matches Tezuka's `fishball7010` target on all 46 MIO registers. Pluto+ Maia bitstreams are NOT suitable.
Working boot set: `SDCARD-maia-final/` (diagnostic variant with an unauthenticated :8099 status server: `SDCARD-fishball7/`).
- `BOOT.bin`  vendor original (FSBL, ADI U-Boot, vendor bitstream); the vendor FPGA image is replaced at boot by the FIT below
- `uImage`    FIT: Tezuka Linux 6.12.77 (raw Image, uncompressed) + Fishball Maia bitstream (.bit from tezuka.xsa), `fpga = "fpga@1"`
- `devicetree.dtb` Tezuka's fishball dtb with `/chosen/bootargs` added (incl. `uio_pdrv_genirq.of_id=uio_pdrv_genirq`, `maxcpus=2`)
- `uramdisk.image.gz` Tezuka rootfs (uImage wrapper around cpio.xz); `uEnv.txt` = vendor original, unmodified
Lessons: the vendor uEnv.txt hook `if test -n $uenvcmd` never fires for multi-word values, and U-Boot's `setenv bootargs`
is not applied on this sdboot path, so load the FPGA via a FIT `fpga` entry and put bootargs in the dtb. The vendor U-Boot has no
LZMA, so Tezuka's lzma uImage must be decompressed and wrapped uncompressed. The web UI is https://192.168.2.1 (Tezuka Dashboard).
Root password: the rootfs restores a saved password database from the radio's internal flash (/mnt/jffs2), so the build's
default (`analog`) does not work if the vendor/previous owner set one. Earlier "pinout mismatch" conclusion for the Pluto+ bitstream
was wrong: the bitstream was never loaded in that test.

## CPU tuning (Oct 2026)
Cause of the slow/hot radio: (1) the experimental Tezuka classifier busy-looped one core (disabled by overriding
/etc/init.d/S61classifier in the SD ramdisk, v4+); (2) `api_controller.sh` (status publisher) forked ~130 short-lived
processes and sent ~100 retained MQTT messages every 2 s. v5/v6 patch the deployed script (see opt-notes/):
publish-if-changed in the polling loop (full refresh every 16 s), fork-free sysfs/proc reads, cached fw_printenv,
debugfs register access instead of iio_attr, rarely-changing values computed less often, new topic state/main/forks_per_s.
Measured over MQTT (CPU metric: one fully busy core reads 100): v4 48-56 idle; v5 31-41 idle; v6 mean 26 (17-36) while a
dashboard session was active. Live MQTT rate 35/s (v4) -> 9.6/s (v6). All 96 original topics still published, values identical
apart from live readings. ~26 forks/s remain (source not yet identified; needs a children-time probe).
Slow-looking waterfall is a separate matter: spectrometer output rate = input rate / (FFT size x integrations); at 2.5 MS/s with
610 integrations it is 1 frame/s. PATCH /api/spectrometer {"output_sampling_frequency":10.0} sets ~10 frames/s.
Pitfall when measuring over MQTT: use keepalive 0 (or send PINGREQ), otherwise the broker drops the client after ~45 s.
Cards: SDCARD-maia-v4 (classifier off), -v5 (script round 1), -v6 (round 2, current), -diag5/-diag6 (probe sets, unauthenticated :8099).

## Optimised classifier (classifier-fast/, card set SDCARD-maia-v7)
The classifier was the main CPU burner (see classifier-fast/README for the analysis). Rewritten matching (prefix sums +
NEON dot product, ~12x faster per correlation on x86 with identical results on 68 test frames), one persistent mqtt_pubd
pipe instead of a fork-per-value, and a native WebSocket client (no libwebsockets, static ARM binary). v7 = v6 +
the new /usr/bin/classifier, with the stock S61classifier restored (the v4-v6 stub that disabled it is gone).
Not yet measured on the radio. Fallback: SDCARD-maia-v6 (classifier stays disabled).

## CPU tuning, final results (card set SDCARD-maia-v8 = clean, nothing extra running)
Profiling method that worked: per-process CPU over 20 s windows from /proc/<pid>/stat (own = utime+stime, children = cutime+cstime;
child time is where short-lived `cat/grep/awk/ip` forks show up) plus /proc/stat 'processes' for the fork rate, and a bash
`xtrace` of 8 real dump_data cycles to see exactly which commands the status script runs.
Findings (v7 window, 16.5% of both cores busy, 20 new processes/s): api_controller.sh 7.2%, watchconsoletx.sh 2.9%
(forked ~8x/s for a once-a-second PTT poll), maia-httpd 2.7%, optimised classifier 1.8%, update.sh 0.7% (`cat` once a second).
v8 changes: watchconsoletx.sh and update.sh without per-second forks (PTT register/LED/log only touched on a change or if the
LED is moved by hand); api_controller.sh: single-test publish(), inline sysfs loops, no `ip link show wg0`, network and static blocks
only on change / every 32nd cycle. Patches and tests: opt-notes/ (api_controller-v8.patch, watchconsoletx.patch, update.patch, test_*.sh).
Measured over MQTT with the classifier ON and the spectrum at 25 fps:
  v4 (classifier off)      cpu metric 48-56   | stock classifier on: 97 (pinned)
  v6                       mean 26, ~26 forks/s | v7 (fast classifier on) mean 39, ~25 forks/s
  v8                       mean 12 (10-17), median 4 forks/s, chip 44 C / FPGA 46 C at 280 s uptime
All 96 baseline topics still published; values identical apart from live readings.
Note: the dashboard's classifier toggle writes classifier_enabled to the radio's U-Boot environment in internal flash (done at
least once during testing); the flag currently reads on, so the classifier starts at boot.

## Classifier v2 (classifier-v2/, card set SDCARD-maia-v9)
The template-matching classifier was replaced by an emitter detector. Each second it averages the /waterfall frames, reads fc/fs from
/api/ad9361, finds wide blocks (median filter, edge segmentation: LTE/DVB-T/DAB/Wi-Fi) and narrow emitters (prominence peaks with
valley-limited bandwidth: FM/GSM/CW/bursts), measures them in Hz and labels them from bandwidth, shape, burstiness, band and channel
raster; board-clock harmonics and the LO carrier are labelled as spurs; bursty ISM peaks are clustered into one Wi-Fi emitter.
Old topics (label, confidence, freq_offset, bandwidth_bins, frame_bins, template_id, feature_*) are still published; new ones:
state/classifier/emitters (JSON, <=16), emitter_count, noise_floor_db, center_hz, sample_rate_hz.
Files: detect4.py + classify.py (Python spec), c/part*.inc + ws_part.inc -> classifier.c (build.sh; static ARM binary `classifier`),
spec_dump.py (reference output), synth.py/synth_test.py (synthetic signals), mqtt_watch.py (topic viewer).
Validation: C output identical to the Python spec on 16 real captures (aggregation off); 9/9 synthetic checks (FM, GSM, LTE, DVB-T, DAB,
LoRa-like bursts, Wi-Fi, CW, noise false alarms 0/40 and 0/20 with tilted floor). Live on the radio (v9, 2.4 GHz): CPU metric 11-17, ~4 forks/s.
Known weak spots: weak DAB ensembles, contiguous GSM/UMTS carriers can read as an LTE 5 MHz block. Fallback card: SDCARD-maia-v8.
