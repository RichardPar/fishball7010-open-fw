# fishball7010-open-fw

Open firmware for the cheap "TZT Pluto 7010 / 70 MHz-6 GHz" SDR clones (Zynq XC7Z010-CLG400 + AD9363, 2R2T, 1 GB RAM, gigabit Ethernet,
SD slot). The seller of the board this was developed on would not supply the GPL source for the shipped firmware, so this project rebuilds
a working open stack and goes further: it runs the [Tezuka](https://github.com/F5OEO/tezuka_fw) firmware with
[Maia SDR](https://maia-sdr.org) from the SD card, and replaces Tezuka's signal classifier with a new, much cheaper emitter detector.

The board turned out to be a **Fishball 7010** clone (its FSBL MIO/DDR setup matches Tezuka's `fishball7010` target on all 46 MIO registers).

## What is in here

| Path | What |
|---|---|
| `classifier/` | **Classifier v2**: emitter detector/classifier (C, static ARM binary) with its Python reference spec, captures and tests |
| `classifier-fast/` | Earlier optimised version of Tezuka's template classifier (kept for reference/comparison) |
| `patches/` | CPU-tuning patches for Tezuka's `api_controller.sh`, `watchconsoletx.sh`, `update.sh` plus the test scripts used to measure them |
| `scripts/` | Boot-set assembly: FIT image, device-tree bootargs, ramdisk overlay, SD card writer |
| `docs/BUILD.md` | Step-by-step build instructions |
| `docs/NOTES.md` | Full project log: findings, pitfalls, measurements |
| `reference/` | Decompiled vendor device tree |
| `results/` | RF test plots (869 MHz capture, TX tone test, 800-900 MHz sweep) |

Not included (and never committed): the vendor `BOOT.bin` (FSBL, U-Boot, vendor bitstream), Tezuka release binaries, kernel/ramdisk images,
card images. You supply the `BOOT.bin` from your own card; everything else is built or downloaded as described in `docs/BUILD.md`.

## Status

- Boots from SD; Maia SDR web UI and Tezuka Dashboard work (https://192.168.2.1); labels corrected to AD9363 / Zynq-7010.
- RX-A verified 800-900 MHz and 2.4 GHz, TX1-A verified at low power. Not tested: RX-B, TX2, SD slot inside the radio, flashing to QSPI.
- CPU: stock Tezuka pins a core (classifier) and forks ~130 processes per status cycle. With the patches and classifier v2 the CPU metric
  idles at 11-17 (one busy core = 100), ~4 forks/s.

## Classifier v2

Each second it averages the `/waterfall` frames, reads fc/fs from the Maia API, finds wide blocks (median filter + edge segmentation) and narrow
emitters (prominence peaks with valley-limited bandwidth), measures them in Hz, and names them from bandwidth, shape, burstiness, frequency band,
channel raster (FM 100 kHz, GSM 200 kHz) and known board-clock harmonics (reported as spurs, not signals). Bursty ISM peaks are clustered into a
single Wi-Fi emitter. No templates. It publishes Tezuka's existing `state/classifier/*` topics plus `emitters` (JSON list), `emitter_count`,
`noise_floor_db`, `center_hz`, `sample_rate_hz`.

Validation (see `classifier/`): identical output to the Python spec on 16 real captures (aggregation off); 9/9 synthetic checks (FM, GSM, LTE,
DVB-T, DAB, LoRa-like bursts, Wi-Fi, CW; noise-only false alarms 0/40, 0/20 with a tilted floor). Known weak spots: weak DAB ensembles;
contiguous GSM/UMTS carriers can read as an "LTE 5 MHz" block. The synthetic signals are models, not over-the-air recordings.

```sh
cd classifier && ./build.sh          # native test binary + static ARM binary (needs arm-linux-gnueabihf-gcc)
```

## Licence

GPL-3.0 (see `LICENSE`). The classifier reuses the WebSocket/MQTT plumbing of Tezuka's classifier (GPL-3.0); Tezuka, Maia SDR and the ADI
trees keep their own licences.
