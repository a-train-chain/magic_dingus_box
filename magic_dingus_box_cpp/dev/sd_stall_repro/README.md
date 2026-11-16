# SD-stall reproduction (off the box)

Reproduces the 2026-11-15 watchdog kill without a Pi: the stall watchdog's
channel reload, on the real `GstPlayer`, while the media file's reads take
seconds — what a Content Manager upload, an OTA rsync or a package import
does to the SD card. Full write-up:
`docs/superpowers/validation/2026-11-16-watchdog-sd-saturation.md`.

```bash
dev/sd_stall_repro/run.sh                    # this checkout, both scenarios
dev/sd_stall_repro/run.sh --ref origin/main  # GstPlayer from any git ref
dev/sd_stall_repro/run.sh -- 8000 0          # one scenario: read ms, open ms
```

Needs Docker. It builds `mdb-sdrepro:trixie` once: the pisim image
(`dev/pisim`, built first if missing) plus `gst-launch`, libav and x264.

## What it does

1. Makes a 60 s 720p H.264 + AAC MP4 with a 2 s GOP, like the transcoder's.
2. Builds `harness.cpp` with `src/video/gst_player.cpp` (the tree under
   test). Two stand-ins: the logger is a stub (`stub/utils/logger.h`), and
   audio goes to a clock-synced `fakesink`, since the container has no
   PulseAudio.
3. Plays the file. Then every `read()`/`pread()` (and optionally `open()`)
   of a path containing `/media/` sleeps for the given time. The harness
   binary defines those libc functions and is linked `-rdynamic`, so
   GStreamer's plugins call its versions.
4. Waits until the position has been frozen for 3 s, like
   `PlaybackStallWatchdog`.
5. Runs the reload in `Controller::load_item_media`'s order: `stop()`,
   200 ms, `load_file()` at the frozen position, `set_volume()`, `play()`,
   a ≤1 s start poll, then `get_volume()`. It times every call.
6. Lets the reads go fast again and reports whether the stream recovers.

Between the calls a "render loop" calls `update_state()` and pulls samples,
as main.cpp does. The verdict line is `RESULT loop-never-blocked` or
`RESULT loop-BLOCKED`. Any call or loop gap over 10 s is what systemd's
`WatchdogSec=10` kills.

## Results on 2026-11-16

| tree | reads 15 s | reads 6 s + opens 6 s |
|---|---|---|
| origin/main | `stop()` blocked 10.76 s | `load_file()` blocked 18.00 s |
| control thread | longest call 0.000 s, longest loop gap 0.025 s | longest call 0.000 s, longest loop gap 0.023 s |

## Limits

- A delayed `read()` stands in for a read queued behind write-back. The
  real thing on the box also delays page faults and metadata reads, which
  this does not model. Hardware validation is still needed.
- No PulseAudio, so pulsesink's own round trips are not exercised.
- No DRM or GL: the frames are pulled and dropped, not drawn.
