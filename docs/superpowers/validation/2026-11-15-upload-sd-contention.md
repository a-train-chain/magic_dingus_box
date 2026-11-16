# Playback stutter during a large upload: SD-card contention

**Date:** 2026-11-15, 22:42–23:47 PST, magicpi5 (Pi 5, 2 GB, SanDisk 64 GB
SD card, kernel 6.18.34+rpt-rpi-v8), with the owner's go-ahead.
**Follows:** finding 1 of `2026-11-15-pi5-checklist-run.md` (rows 14.5 and
16.38): Channel Guide playback stalled and jumped while a 2.57 GB upload was
written to the SD card.
**Fix:** `magic_dingus_box/web/upload_io.py`, the PR that adds this file.

## Root cause

Three things combine:

1. **Every upload was written to the card twice.** werkzeug spooled the
   request body to `data/upload_temp`, then the route copied the whole spool
   (`FileStorage.save`) to a second file before ffmpeg started. That is 2.57 GB
   read back and 2.57 GB written again, at full card speed.
2. **The writes are unpaced.** Both go through the page cache. The kernel's
   flusher wrote back 100+ MB of dirty data at a time (`Dirty` peaked at
   114–172 MB), and kiosk reads queued behind it for seconds.
3. **Nothing on this kernel ranks the kiosk's reads higher.** The block
   scheduler on `mmcblk0` is `mq-deadline`. No cgroup has an `io.weight`
   file, `io.cost.qos` is absent and there is no writeback throttling
   (`wbt_lat_usec`), so the kiosk unit's `IOWeight=1000` does nothing. An I/O
   class set on the uploader does not reach the flusher's writes (case c3
   below).

The page-cache flood also evicted the kiosk's own memory pages: the
kiosk's major page faults rose by 24–26 during every unpaced 1 GiB write,
and by 0 during every paced one.

## Method

- **Read probe** (a stand-in for GStreamer's `filesrc`): reads a cold 400 MB
  file on the same card, 32 KiB at a time at 512 KiB/s, at nice -10 like the
  kiosk, and records each read's latency. It ran from 10 s before each write
  load until 15 s after it.
  - **max wait**: the slowest read.
  - **starved**: the total of (wait − 0.167 s) over reads slower than
    0.167 s. 0.167 s is about how much video a 5-buffer decoder queue holds.
  - With no writer, the slowest read was 16 ms and starved was 0.
- **Write load:** 1 GiB per case, shaped like each upload phase:
  - spool: 64 KiB writes, either at card speed or capped at 8 MiB/s like Wi-Fi;
  - copy: `FileStorage.save`'s 16 KiB copy of a cold file.
- **Pacing variants tested:**
  - `sync_file_range` (write and wait) every N MiB, then `POSIX_FADV_DONTNEED`;
  - the idle I/O class for the writer (`ioprio_set`).
- **The real kiosk** played a Channel Guide channel from the SD card
  throughout. Its journal lines "Playback stalled … reloading" (a stall of
  more than 3 s) are counted per case.
- **Real uploads:** the checklist's 2.57 GB synthetic H.264 file, sent to
  `POST /admin/upload-and-transcode` from the Mac over Wi-Fi.
  - The kiosk was sampled from `kiosk_status.json` every 3 s.
  - Its reported position follows the pipeline clock, so it keeps advancing
    through a stall. The journal and major faults are the reliable signals.

## Results: 1 GiB write loads

| case | writer | MiB/s | max wait | reads > 0.5 s | reads > 1 s | starved | kiosk >3 s stalls |
|---|---|---|---|---|---|---|---|
| c1 | spool, unpaced (main) | 14.1 | 4.24 s | 26 | 19 | 42.6 s | 1, counted as a failed item |
| c9 | spool, unpaced, at Wi-Fi rate 8 MiB/s | 8.0 | 4.86 s | 26 | 15 | 35.2 s | 1 reload |
| c2 | copy, unpaced (main's second copy) | 9.8 | 3.73 s | 33 | 21 | 42.8 s | 0 |
| c3 | spool, idle class only | 13.3 | 7.38 s | 31 | 26 | 57.3 s | 1 reload |
| c15 | spool, unpaced; reader in the **realtime** class | 15.4 | 3.54 s | 29 | 20 | 44.0 s | 1 reload; then no picture for 3.7 min (below) |
| c16 | spool, unpaced; **BFQ** scheduler | 15.6 | 2.28 s | 17 | 12 | 22.9 s | 0 |
| c4 | spool, 8 MiB slices + drop | 10.6 | 2.10 s | 24 | 11 | 24.6 s | 0 |
| c5 | spool, 8 MiB slices + drop + idle | 12.8 | 1.32 s | 19 | 8 | 15.8 s | 0 |
| c6 | spool, 2 MiB slices + drop + idle | 10.9 | 1.28 s | 3 | 1 | 3.5 s | 0 |
| **c7** | **spool, 1 MiB slices + drop + idle (the fix)** | **10.7** | **0.35 s** | **0** | **0** | **1.2 s** | **0** |
| c13 | spool, 512 KiB slices + drop + idle | 12.5 | 0.66 s | 3 | 0 | 2.0 s | 0 |
| c14 | spool, 1 MiB slices + drop, no idle class | 10.6 | 1.29 s | 7 | 2 | 7.3 s | 0 |
| c10 | 8 MiB/s, 8 MiB slices + drop + idle | 8.0 | 1.53 s | 14 | 5 | 12.0 s | 0 |
| c11 | 8 MiB/s, 2 MiB slices + drop + idle | 8.0 | 0.82 s | 2 | 0 | 3.2 s | 0 |
| c12 | 8 MiB/s, 1 MiB slices + drop + idle | 8.0 | 1.14 s | 3 | 1 | 2.8 s | 0 |

Kiosk major faults: +26 (c1), +25 (c3), +24 (c9) and +10 (c15) for the unpaced
cases; 0 for every paced case except one fault in c13.

What the table shows:

- **The amount of write the card has queued is what kiosk reads wait
  behind.** Slice size is the lever: going from 8 MiB to 1 MiB slices took
  the worst wait from 1.3 s to 0.35 s, at the same throughput.
- **The idle class helps, but only once the uploader does its own write-back.**
  1 MiB slices with it: starved 1.2 s; without it: 7.3 s (c7 vs c14). On its
  own it does nothing (c3).
- **The system-level knobs do not fix it.** Putting the reader in the
  realtime class changed nothing (c15). BFQ halved the starvation but still
  left 2.3 s waits (c16). The kiosk's `IOWeight` has no effect on this
  kernel. These were run-time experiments only; the box went back to
  `mq-deadline`.
- **Pacing costs no real-world speed.** Card speed drops from 14.1 to
  10.7 MiB/s, but a real Wi-Fi upload arrived at 10.4 MiB/s anyway.
- **Unpaced writes can blank a channel.** In c15 the stall reload's new
  stream did not start within 8 s. The kiosk counted the item as failed and,
  as designed, waited out the slot with nothing playing (3.7 min).

## Results: real 2.57 GB uploads, a channel playing

| | main (`c88cc15` web files) | this fix |
|---|---|---|
| Request time | 463 s (5.5 MB/s: spool, then the copy) | **236 s** (10.9 MB/s, spool only) |
| Peak `Dirty` | 114 MB | 1 MB |
| Kiosk journal | stall reload at 23:37:30; **watchdog SIGABRT at 23:37:41** (the reload's playlist transition blocked the main loop > 10 s); systemd restarted the kiosk 8 s later | nothing logged (no stall, reload or failed item) |
| Kiosk major faults | +4 | 0 |
| Transcode input | full copy of the spool | the spool itself, hard-linked; sha256 identical to the Mac's file |

The checklist run earlier that evening (main, same file, same box) took
547 s and logged one stall reload, with visible jumps.

Not covered on hardware: the transcode phase (unchanged code; the checklist
run found playback on time throughout it), Pi 4B, and package imports. A
package import's ZIP extraction still writes unpaced; its spool is paced.

## Follow-ups

- **Watchdog kill when a reload blocks.** Under heavy SD writes, a stall
  reload's playlist transition held the kiosk's main loop for more than 10 s.
  With uploads paced this no longer happened in our runs, but any other
  heavy writer (an OTA rsync, a package import's extraction) can still
  trigger it.
- **`IOWeight=1000` in `magic-dingus-box-cpp.service`** is described as
  keeping movie reads ahead of background writes. On this kernel it does
  nothing.
