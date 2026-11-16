# 1.12.0 Pi 5 hardware checklist run — magicpi5, 2026-11-15 evening

Owner-approved remote run of the Pi 5 column of
`2026-11-15-release-1.12.0-readiness.md`, 20:46–21:55 box time (one 23-minute
pause for the CRT engine session). Box: magicpi5 (Pi 5, 2 GB), main at
`2edc334`, kiosk binary `1e858f05…`, Modern TV, MDB-KV19 bezel, enhanced CRT,
Channel Guide style, TV on **HDMI 0** (DRM `HDMI-A-1`).

## Method

- **Input:** one long-lived virtual gamepad (the web admin's
  `UinputWriter`, kept open by a root transient unit for the whole run, so no
  press is lost while the kiosk opens a new evdev node).
- **Seeing the TV:** `data/screenshot_request` captures of the full
  1920x1080 composite; each capture was deleted from the box after fetching
  (the kiosk keeps only the newest 10 and the owner had 2).
- **State:** `data/kiosk_status.json` and the kiosk journal.
- **Content:** synthetic test clips (16:9, 4:3, 9:16 with marked edges and
  4:3 guides; a corrupt file; a 2.57 GB 1080p clip) uploaded through the
  Content Manager API like any upload, in a temporary "MDB Test Clips" channel.
- **Sound:** judged from ALSA/PulseAudio state (which process owns the HDMI
  0 PCM and whether it is RUNNING), not by ear.
- **Owner state:** settings, playlists, media_info and the time zone were
  snapshotted first and restored at the end, verified identical; test files
  and the channel were deleted through the Content Manager.
  `verify_box.sh --with-services` afterwards: 42 passed / 0 failed / 1 warning
  (no Dreamcast BIOS, known).

## Findings for the owner

1. **Playback runs fast during a large upload's SD-write phase (14.5, 16.38).**
   While a 2.57 GB upload was being spooled and copied on the SD card,
   channel playback from the same card hit read stalls and then dropped
   frames to catch up: position advanced 29.6 s in 10 s, 39.6 s in 21 s and
   39.4 s in 35 s of wall time, and one >3 s stall triggered the designed
   reload at the live point. No watchdog kill, no failed item for a good
   file. During the following CPU-bound transcode (ffmpeg ~3 cores, load up
   to 12.9) playback kept real time (ratio 0.93–1.05), so this is SD-card
   contention, not CPU. It is the single-card limitation CLAUDE.md describes;
   whether a viewer-visible stutter during big uploads is acceptable is an
   owner call.
2. **In Channel Guide style, Settings opens only from the guide.** BLACK /
   B during full-screen playback does nothing (`ui_available` needs the UI
   visible), so the owner guide's "Press B to open Settings" does not work
   while a channel plays. Open the guide first (OK), then B. The owner guide
   §12 now says so.
3. **Rapid presses are dropped in Settings and the game library** at 0.4 s
   spacing; 0.6–0.7 s is reliable. Worth a look if real users tap quickly.

## Results

| Row | Result | Evidence |
|---|---|---|
| 13.8 | PASS | Pair Wireless Controller on the 640x480 canvas: all lines fit, cancel hint visible (c07, c08); no line needed wrapping. |
| 14.5 | PASS / FINDING | Guide Mode, CRT Engine (x4), Scanlines (full cycle) toggled during the upload: UI and settings.json agree (live / True / 0.5). FINDING: channel playback during the SD-write phase ran fast — position advanced 29.6 s in 10 s, 39.6 s in 21 s, 39.4 s in 35 s (frames dropped to catch up after read stalls); one >3 s stall logged "reloading channel 2 at its live point". During the CPU-bound transcode afterwards playback kept real time (ratio 0.93–1.05). No watchdog kill, NRestarts 0. Volume hold not exercised (driver sends taps, not a held chord). |
| 14.6 | PASS | Scanlines Medium→High during the copy phase, then `systemctl stop` at once: stop 0.7 s, Result=success, 0.75 saved; restart resumed the last channel. |
| 14.7 | not run | 30 min of Media Browser playback through an upload. |
| 14.8 | not run | Phone typing during an upload. |
| 14.9 | owner | Reconfigure re-submits the WireGuard config (private key); not entered by automation. |
| 14.10 | PASS so far | No watchdog/SIGABRT/dumped/Failed lines; NRestarts 0 (manual restarts only). |
| 15.1 (Modern TV + bezel) | PASS | 16:9 clip fills the 4:3 area, cropped evenly (a05); 4:3 clip whole (a06). CRT Native / no bezel still to do. |
| 15.2 | PASS | 16:9 playing: UP past the top row lands on "PICTURE ☑ CROP fills the screen / 16:9 SOURCE" (a08); OK -> "whole frame", media_info crop:false at once; the window shows the whole letterboxed frame, no stretch (a09). |
| 15.3 | PASS | CM shows the TV's choice (crop False); CM toggle → TV cropped by the next capture (m16); crop False survived a kiosk restart and played fit (a21). |
| 15.4 | PASS | 4:3 playing: cursor clamps on MASTER SHUFFLE, no Picture control (a07); 9:16 upright. |
| 15.5 | PASS | Media Browser Popular renders as before: tabs, poster grid with artwork, hints (m01). |
| 15.6 | PASS | 6 Content Manager crop toggles during the upload: all HTTP 200 (two took ~2 s), no watchdog. |
| 15.7 | PASS | Fit with enhanced CRT on (m16) and Classic (a23): black bars stay inside the 4:3 opening. |
| 15.9 (MDB bezels) | PASS | Fit picture edges sit inside MDB-KV19's inner edge (a23). Other bezels not checked. |
| 15.12 | PASS | Focus on CROP during 9:16 stayed on CROP into the next 16:9 (mc05); dropped to the top row when the next item was 4:3 (c04). |
| 15.13 | PASS | Backup (16:9 fit) → set 16:9 crop + upload a new clip as fit → restore: 16:9 back to fit, the new clip kept fit; kiosk reloaded settings, media info, playlists at once. |
| 16.1 | PASS | Settings → Display → Menu Style: Channel Guide → List (saved at once, survived restart), List → Channel Guide (survived restart). |
| 16.2 | PASS | Reboot at 21:48:39 → intro first frame 21:49:23 → "Nothing playing: tuning channel 3" 21:49:34.9, joined mid-item (535.9 s), right as the 11 s intro ended; 0 failed units. |
| 16.5 | PASS (D-pad as the knob) | One DOWN = one row (z02, z03); OK tuned 02 and closed the guide, first frame ~350 ms later; OK tunes + closes; OK on the tuned row only closes. |
| 16.6 | PASS | Closed: 02→03→02→01→00→wrap 13→12→13; open: 13→wrap 00, 00→13→12, guide stays open. (kiosk_status names Master Shuffle by its source playlist.) |
| 16.7 | PASS | 18 tunes: join → first frame 171–670 ms (median ~320 ms). |
| 16.8 | PASS | Capture 120 ms into the close: guide dimmed, video at full brightness in the window (a12); full picture after. |
| 16.9 | PASS | Panel progress bar grows 131→186→227 px across captures ~3 s apart (40 s item). |
| 16.10 | PASS | Switching to Channel Guide while MDB Test Clips played joined channel 2 live; after closing Settings the channel was full-screen. |
| 16.11 / 15.11 (render) | PASS | CRT Native: 16:9 fit = whole frame, crop = fills the window, proportions right for non-square pixels (c02, c03). Real 4:3 TV not checked. |
| 16.12 | PASS | MDB-KV19 over the guide at Modern TV 1080p (a01). |
| 16.13 | PASS | POST /admin/system/timezone America/New_York → system + .env TZ changed, the open guide's clock read 12:50 AM with no restart (ze01); Asia/Calcutta → Asia/Kolkata applied; restored America/Los_Angeles, .env hash identical to the pre-test snapshot. |
| 16.14 (Media Browser) | PASS | Leaving the Media Browser (RED → Exit) tuned the last channel live. |
| 16.15 | PASS | Corrupt file: "Could not determine type of stream" → "Channel 2 item failed … resumes with its next scheduled item"; dark for its 20 s slot, next item on time. |
| 16.16 / 16.17 / 16.31 | PASS (evening span) | 20:46–21:47, guide style throughout: 0 watchdog kills (the one "Failed with result 'signal'" is the deliberate SIGKILL), 0 "never started", 3 harmless "stream not up … waiting", 0 GStreamer warnings; the one stall line names "PLAYING (SUCCESS)". The guide was not literally open for a full hour. |
| 16.21 | PASS | No "never started" lines during the upload; only the deliberately corrupt item failed. |
| 16.22 | PASS | Every tune logs "first frame arrived after N suppressed tick(s) — stale-frame flash prevented"; a capture 0.25 s into a tune is black, not a 0:00 frame. |
| 16.23 | PASS (as built) | The "◂" text prefix is no longer drawn (only a guard remains in channel_guide_view.h); the began-before/continues chevrons render as glyphs, no missing-glyph boxes (z01). |
| 16.24 | PASS | 10 captures across a 16:9→4:3 boundary: times change once (9:00–9:00 → 9:00–9:01), no flicker (m15). |
| 16.25 | PASS | Paused at 8.8 s on a 40 s item, guide open ~80 s later: panel keeps "Test 16x9", bar frozen; start/end move later each minute (m24). |
| 16.26 (server half) | PASS | A UTC report on a named zone: changed=false, zone and .env untouched. The Firefox/Tor/Brave browser half needs the owner. |
| 16.27 | PASS (broken intro) | Corrupt file as data/intro/intro.30fps.mov: after the ≤10 s first-frame wait, "Nothing playing: tuning channel 3" (last channel). Note: the kiosk also finds intro copies in data/intro/*.mp4 and build/data/intro/, so a true "no intro" needs all 12 locations empty. |
| 16.28 | PASS | Artist lines (Modern) and grid date fit; CRT grid text fits its rows (a01, c02). |
| 16.29 | PASS | 3 new uploads (crt_hd, crop): keyframes max gap 2.00 s (20/20/15 KF); masters 1280x720, 960x720, 608x1080 (aspect kept). Encode time not compared with before. |
| 16.33 | PASS | CRT Native names "MASTER SHUF…", "MDB TEST CL…" and the owner's own channel names (~12 letters), clear of the numbers (c02). |
| 16.34 / 16.14 | PASS | Each game was launched from the guide → Settings → Video Games; every return was full-screen (guide closed) on the last channel, live. |
| 16.35 | PASS | 30–40 s test items (~34 px) show empty cells, no stray "…" (a01). |
| 16.37 | not run separately | Same behaviour seen past the slot; the inside-the-slot case on a long item was not run. |
| 16.38 | PASS (partial) | One >3 s stall → reload at the live point, no failed item for a good file; but sub-3 s stalls caused visible jumps (see 14.5 finding). |
| 16.39 | PASS | 10/10 launches over a playing guide channel, TV on HDMI 0 (DRM HDMI-A-1): FBNeo Asteroids x3, nestopia x3, genesis_plus_gx x2, snes9x2010 x2. During each game RetroArch owned card0 pcm0p, state RUNNING (48 kHz IEC958). Judged by ALSA state, not by ear. |
| 16.43 | PASS | After each of the 10 games: PulseAudio sink RUNNING, card0 owned by PulseAudio, kiosk video playing. |
| 16.44 | PASS | `systemctl restart` mid-game (NES): "[audio] resumed 1 PulseAudio sink(s)" before the stop; after start "[audio] start-up: 1 of 1 PulseAudio sink(s) listed SUSPENDED … cleared"; sink RUNNING; RetroArch stopped cleanly. |
| 16.45 | PASS | SIGKILL mid-game (Genesis): systemd restarted it (NRestarts 0→1, deliberate); same start-up line; sink RUNNING. |
| 16.47 | n/a on Pi 5 (option works) | Audio Output cycles Auto ↔ HDMI after a game; only the TV's HDMI 0 sink exists, so the stream rightly stays put. The HDMI ↔ Headphone move is a Pi 4B check. |
| **Phase B upload** | — | 2.57 GB synthetic H.264 via /admin/upload-and-transcode from the Mac over Wi-Fi: 547 s for the request (spool + on-card copy, SD saturated), then the crt_hd transcode (ffmpeg nice 0, ~2.5–3 cores, load up to 12.9). 10 GB not attempted: 16 GB free and an upload peaks at ~2x its size. |
| movie-channel guard | PASS | Tuning a movie channel (a film) stopped arr/Byparr (FullPause); leaving the film channel resumed them 120 s later (kReleaseGraceS), marker + claims cleaned. |
| note | not a bug | Bezel "CH 07" is printed in mdb_kv19.png artwork, not a live readout. |
| note | doc gap | In guide style, Settings opens only with the guide open (OK, then BLACK); BLACK during full-screen playback does nothing. OWNER_GUIDE §12 doesn't say so. |
| note | observation | Settings rows drop presses sent 0.4 s apart (0.6 s is reliable). |
| note | setup | The TV is on HDMI 0 (HDMI-A-1). |

## Not run remotely (left for the owner or a later run)

- **Needs the owner in person:** 14.1–14.4 (first-boot SD cards), 14.9
  (Reconfigure re-submits the WireGuard config), 16.18 (power-on after hours
  off), 16.26 browser half (Firefox resistFingerprinting / Tor / Brave), 16.40
  (TV moved to HDMI 1), 16.46 (GPIO restart button), 13.1–13.7 (Bluetooth pads
  in games), 19.1–19.3 (physical knob, SSD unplug, a real download), and real-TV
  confirmation of 15.8 / 15.11.
- **Remote but not reached tonight:** 14.7 (30 min of Media Browser playback
  through an upload), 14.8 (phone typing during an upload), 16.3 / 16.4
  (comparison with mockup A), 16.19 / 16.20 / 16.36 (long-GOP files, knob seek
  to an item's end), 16.30 (tune-in offset on new uploads, not measured), 16.37
  (pause inside a long item's slot), 15.1 without a bezel, 15.8 long titles and
  720p, 15.9 non-MDB bezels.
