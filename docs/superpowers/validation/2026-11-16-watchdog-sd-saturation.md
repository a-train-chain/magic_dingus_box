# Watchdog kill when the SD card is saturated: GStreamer state changes on the render loop

**Date:** 2026-11-16.
**Follows:** "Follow-ups" in `2026-11-15-upload-sd-contention.md`. On
magicpi5 at 23:37 on 2026-11-15, during the main-code 2.57 GB upload, the
stall watchdog started its reload at 23:37:30 and systemd's watchdog
SIGABRTed the kiosk at 23:37:41 (`WatchdogSec=10`).
**Fix:** `magic_dingus_box_cpp/src/video/pipeline_control.h` and the
`GstPlayer` changes in the PR that adds this file.

## Root cause

Every GStreamer state change ran on the render loop, and a state change
can wait on the SD card:

1. **`stop()` waits for the read in flight.** Setting a playing pipeline
   to NULL deactivates the demuxer's sink pad, which joins its streaming
   thread (`gst_pad_stop_task`). That thread is inside a `read()` of the
   media file, and a read is uninterruptible. Under heavy write-back one
   read waited 4–7 s in the probe of the upload test.
2. **A stall is exactly when a read is stuck.** The stall watchdog fires
   because the position stopped moving, and the position stopped moving
   because the streaming thread is waiting on the card. So its reload's
   first call, `stop()`, blocked for the rest of that read.
3. **`load_file()` opens the file on the calling thread.** Going from NULL
   to PLAYING starts `filesrc` in the state change; playbin opened the file
   three times there. An `open()` whose inode is not cached waits on the
   card too.

Other render-loop calls on the same path were made safe at the same time:

- **Seeks:** a FLUSH seek takes the demuxer's stream lock, so it also
  waits for the read in flight. This covers the deferred start seek of
  every channel join and every knob scrub.
- **Volume:** `get_volume()` asked pulsesink, which waits for a PulseAudio
  round trip. It ran on every video start (`Controller::update_state`).
  `set_volume()` takes pulsesink's and playsink's locks, which a state
  change holds.

### What the box's journal shows

The kiosk's own lines from the kill (`journalctl -u
magic-dingus-box-cpp`, box time, read-only):

| time | line | where in the code |
|---|---|---|
| 23:37:30.515 | `Playback stalled at 450.542s … reloading channel 0 at its live point` | `on_channel_stall` |
| 23:37:30.515 | `Channel 0: joining … item 3 at 3.5s` | `load_on_air`, just before the load call |
| 23:37:34.599 | `Starting playlist transition...` | first line of `load_item_media` |
| — | (`Stopped playback, loading new file...` never arrives) | after `stop()` returns |
| 23:37:41.872 | `Watchdog timeout (limit 10s)!`, SIGABRT | systemd |

What it shows:

- **`stop()` held the loop until the SIGABRT.** The line after it never
  arrived. journald reads a stream after the writer dies, so that line was
  never written.
- **The ~4.1 s between "joining" and "Starting playlist transition" is
  probably journald, not the kiosk.** The journal stamps a line when
  journald *reads* it, and under the same saturation journald itself lags.
  In the hardware baseline below, the kiosk's last lines were stamped 1.5 s
  after systemd's SIGABRT. On the fixed binary, the new diagnostics timed
  that stretch (path resolution) directly: 254 ms at worst, and that was
  one page fault, not a slow `stat()`.

**Under write saturation, kiosk journal timestamps are not evidence of
where time went.** Use the durations the diagnostics log.

The off-box reproduction below shows the `stop()` mechanism directly, with
the real `GstPlayer` code.

## Reproduction off the box

`magic_dingus_box_cpp/dev/sd_stall_repro/run.sh [--ref <git-ref>]` runs
the real `GstPlayer` in the pisim container (Debian Trixie, arm64,
GStreamer 1.26.2, the boxes' versions):

- the media file's `read()`/`pread()`/`open()` are delayed by an
  interposed libc wrapper, as a saturated card delays them;
- the harness waits until the position has been frozen for 3 s, like the
  stall watchdog;
- it then runs the reload in `Controller::load_item_media`'s order (stop,
  200 ms, load at the frozen position, set volume, play, ≤1 s start poll,
  read volume) and times every call;
- a "render loop" that calls `update_state()` and pulls samples runs
  between the calls.

| scenario | code | `stop()` | `load_file()` | longest call on the loop | verdict |
|---|---|---|---|---|---|
| every media read takes 15 s | origin/main | **10.758 s** | 0.001 s | 10.758 s | watchdog kill |
| reads 6 s, opens 6 s | origin/main | 1.774 s | **18.001 s** | 18.001 s | watchdog kill |
| every media read takes 15 s | this fix | 0.000 s | 0.000 s | 0.000 s | loop never blocked |
| reads 6 s, opens 6 s | this fix | 0.000 s | 0.000 s | 0.000 s | loop never blocked |

With the fix, the longest loop iteration during and after the reload was
24–42 ms. The control thread logged `pipeline stop took 10.77 s (render
loop unaffected)`, and once the reads were fast again the stream came up
and played on. Where it lands is the same as before: the join seek snaps
to the keyframe before the requested position (measured old against new,
identical).

## The fix

`GstPlayer` hands every stop, load, play, pause, seek and volume change to
one control thread (`video::PipelineControl`). The render loop only queues
them.

- **Coalesced queue.** A stop or load makes everything not yet started
  moot. A load keeps the stop queued before it. A stalled card never
  builds a backlog: a tune, a boundary, a reload and a retune queued behind
  a stuck stop run as one stop and the latest load.
- **The settle is kept.** `load_item_media` stops, waits 200 ms, then loads,
  so the DRM driver can release the old video's buffers. The control thread
  starts that load no earlier than the stop's real completion plus the gap
  the caller left (`load_not_before`, capped at 1 s). On a healthy card that
  is the old timing exactly, and the hardware still sees NULL, ≥200 ms,
  PLAYING.
- **While a command is pending, the player leaves the pipeline alone.**
  `update_state()` skips its bus drain, state poll and queries, and the
  getters return the last values. The stream was already forgotten at the
  call, as before.
- **No stale frames.** The renderer pulls no sample until the control
  thread has taken the pipeline through NULL for the latest load
  (`frames_current()`). Until then the appsink holds the old stream's
  frames, and the stream gate would have drawn one as the new stream's
  first.
- **Deadlines that count from a load's return hold while it is pending**
  (`PlaylistPlayback::hold_load_deadlines_while_busy`). This covers the 8 s
  channel start bound and the 2 s stuck-switch timeout. Before, that wait
  was inside the load call, so the deadlines started after it; without the
  hold, a switch's timeout would `stop()` a load that is only waiting for
  the card. The failed-item rules are otherwise unchanged.
- **Worker logging goes to the journal** (`std::cerr`), never spdlog: its
  file sink is on the card and shares one mutex with the loop.
- **A wedged command still ends in a restart.** Before, a wedged state
  change (a decoder deadlock, a dead card) froze the loop, and the watchdog
  restarted the kiosk. Now the loop would keep pinging. So once one command
  has been in hand for 30 s (`kWedgedAfter`), main.cpp stops pinging, logs
  the pipeline's state, and systemd restarts the kiosk as before.
- **The stall watchdog ignores a busy pipeline.** The player does not
  query a pipeline its control thread is changing, so the position reads
  frozen. A seek waiting on the card is not a stall.
- **The end state matches the old sequence.** A stop that supersedes a
  load that never ran carries its URI. The pipeline ends as the old
  synchronous load-then-stop left it, and a later `play()` restarts that
  stream.
- **Teardown drops waiting work.** `cleanup()` (game launch, shutdown)
  drops everything not started, including a load waiting out its settle.
- **Commands go before volume changes** on the control thread.
- **Seek rejections are tagged with the seek's serial**, so a late
  rejection from the previous stream cannot clear the next one's seek.

These follow-ups came from an independent review of the change. The same
review found no data race or stale-stream bug on the common paths.

### Diagnostics for what is left

`utils/stall_report.h`:

- A render-loop iteration of 1 s or more is logged with the thread's major
  page faults during it. Game sessions run inside one iteration with the
  watchdog disarmed, so they are exempt.
- So is a `load_playlist_item` missing-file check, or
  `load_file_with_resolution`'s path resolution, that takes 250 ms or
  more.

A fault count of `+0` means a blocking call; more than that means the
kiosk's own pages were read back from the card. The fix differs: move the
call off the loop, or keep the pages resident.

### Still on the render loop

- `load_item_media`'s 200 ms settle and its ≤1 s start poll (the existing
  TODO there). Both are bounded.
- Path resolution (`missing_video_file`, `load_file_with_resolution`) and
  `weakly_canonical` for the media key. On hardware under saturation this
  stayed fast (see Hardware), so it stays on the loop: moving it would
  change when a missing file is refused, which the failed-item rules
  depend on.
- **Major page faults.** The page-cache flood evicts the kiosk's own
  pages, and the first time the loop runs rarely-used code (a playlist
  transition) it reads them back from the saturated card: 0.25–3 s per
  fault on magicpi5. Threads cannot fix this. It is the follow-up below.

## `IOWeight=1000`

The kiosk unit's comment said `IOWeight=1000` keeps the movie's reads
ahead of background writes. On the Pi kernel it does nothing:

- the SD card's scheduler is `mq-deadline`;
- the kernel has no `io.cost` and no BFQ group scheduling;
- so no cgroup has an `io.weight` file
  (`2026-11-15-upload-sd-contention.md`, magicpi5).

Both boards boot the same `kernel8.img` (`config.txt` `[pi5]
kernel=kernel8.img`), so this holds for the Pi 4B too.

The setting stays: it is harmless, and it would take effect on a kernel
with either feature. Removing it would also change whether systemd enables
the io controller for the kiosk's cgroup, which nobody has measured. Only
the comment changed, saying what it does and does not do. This is a
unit-file edit, so every delivery path converges it
(`converge_system_units.sh`, kiosk row `on-change none`: daemon-reload,
no restart). It changes no directive, so it is safe with any older tree.

## Tests

- `tests/video/test_pipeline_control.cpp` (Mac): the coalescing rules, the
  settle, a held command never blocking the caller, a newer stop
  superseding a load waiting out its settle, volume as latest-wins state,
  an executor exception, references released on drop. Ran 20 times in a
  row, all passing.
- `tests/app/test_playlist_playback.cpp`: the channel start bound and the
  List switch timeout hold while `pipeline_busy()`. Both new cases fail
  without `hold_load_deadlines_while_busy`.
- `dev/sd_stall_repro/run.sh`: the table above.
- `tests/utils/test_stall_report.cpp`: the loop and section timers.

## Hardware (magicpi5, 2026-11-16 12:32–12:45, owner-approved)

**Setup:** Pi 5 2 GB, the same SanDisk card. Channel Guide (Live) playing
channel 00. Each run wrote 2 GiB of zeros to the root filesystem,
unpaced, through the page cache (`dd bs=1M conv=fsync`): 153–172 s of
saturation, with `Dirty` peaking at 167–212 MB, like an unpaced upload.
Snapshots of settings, playlists, media_info and the time zone were taken
first and were identical afterwards.

| run | binary | kiosk | what the journal and diagnostics show |
|---|---|---|---|
| baseline | origin/main (1e858f05) | **SIGABRT by the watchdog** 51 s into the write, restarted | `Playback stalled at 3554.84s … reloading channel 0`, `Starting playlist transition...`, then `Watchdog timeout (limit 10s)!` |
| fix 1 | this PR (d13cf650) | **same process, NRestarts 0** | a boundary load during the write: `pipeline stop took 4.98 s`, `pipeline load took 4.63 s`, `pipeline seek took 2.39 s`, all on the control thread with the render loop unaffected. The stream came up and played on. Longest loop iteration 4383 ms (+2 major faults, including the designed 1.2 s settle and start poll), next 1249 ms (+1). |
| fix 2 | this PR | **same process, NRestarts 0** | no stall during the write; a load just after it: path check 254 ms (+1 fault), longest loop iteration 1050 ms (+1). |

- **`dd` alone reproduces the kill** on origin/main, with no upload. This
  answers the "any other heavy writer" follow-up.
- **With the fix, the GStreamer calls that waited on the card waited on
  the control thread.** Together (12 s) they are more than the watchdog's
  10 s.
- **What still holds the render loop under saturation is page faults**
  (0.25–3 s each, 1–2 per transition) and the designed 1.2 s. In these
  runs that was 4.4 s at worst, well inside the 10 s.

**Restored:** the original binary (1e858f05), the kiosk playing channel 00,
no test files left. The baseline's watchdog restart was the only
unplanned restart.

## Follow-ups

- **Render-loop page faults under a page-cache flood.** They are 1–3 s each
  and not yet a kill, but they are the remaining stall. Options:
  - keep the kiosk's code resident (`mlock` of its own text and the hot
    GStreamer/glib libraries — the Pi 4B's 1.5 GB envelope decides how
    much);
  - pace every bulk writer, as uploads now are. An OTA rsync and a package
    import's extraction are not paced yet.
- **`load_item_media`'s 200 ms settle and ≤1 s start poll** (its TODO) can
  now move off the loop. The control thread already measures the settle
  from the stop's real completion, so the render-thread wait could become
  a per-frame state machine. That still needs a hardware check on both
  boards.
