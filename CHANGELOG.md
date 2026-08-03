# Changelog

All notable changes to Magic Dingus Box will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added (Media Browser)
- **"For You" tab** — personalized recommendations seeded from the Radarr
  library: up to 8 random library titles fan out to TMDB's recommendations
  endpoint (with the similar-films fallback), merged and ranked by how many
  seeds agree, library titles excluded, capped at 100. Re-sampling on
  shuffle, 6-hour expiry, and boot gives genuinely different-but-personal
  results each time.
- **SHUFFLE row in the filter overlay** (Popular / Top Rated / For You) —
  reloads the grid from a random page of the chart (or of the filtered
  discover result), so the same page-1 titles stop appearing every visit.
  Committing filter edits and shuffling in one gesture works; closing the
  overlay without edits no longer refetches the tab.
- **Browse grids refresh themselves after 6 hours** (stale-while-revalidate:
  the old grid stays on screen and is replaced only when a fresh page
  actually arrives — a failed refresh changes nothing).
- TMDB list endpoints now report success/failure and total pages
  (`TmdbList`), so an empty page and a dead egress are distinguishable.

### Removed
- **The synthetic "Movies" playlist no longer appears in the main menu.**
  Since Media Browser V2 the kiosk scanned the Radarr library at boot
  and injected an auto-generated "Movies" row into the playlist wheel
  once anything was downloaded — mixing the whole movie library (and
  its full-length titles) into a surface meant for curated playlists
  and Master Shuffle's auto-advance pool. Removed by operator decision:
  movies play only through Settings → Media Browser → Library.

### Fixed
- **Graceful return from RetroArch.** Exiting any game no longer replays the
  gold "NOW LOADING" frame, black-screens the TV twice, or flashes a green
  progress plate. The exit path now restores the kiosk's real boot mode
  directly (the old code forced 640x480 and the main loop immediately set it
  back — two HDMI resyncs per exit), holds the frozen launch plate briefly so
  the TV's HDMI re-lock doesn't swallow the transition, dissolves it to black
  over 250ms, does the input/audio restore work under black, and fades the
  menu up from black. The bezel stays solid through the whole round trip.
  Launch-side visuals are unchanged.

### Fixed (Content Manager visuals)
- **The faceplate bezel drew itself twice.** A legacy `body::after`
  rule (gold ring + cream line via `outline-offset` + screw heads as
  background SVGs) coexisted with the faceplate shell that draws the
  same frame from `body.faceplate::before/::after` + real screw
  elements. The un-overridden legacy properties leaked into the
  faceplate's cream ring: a third hairline ~19px into the panel and a
  second, mispositioned set of corner screws — visible on every phone
  (a desktop-only pass had already suppressed them at ≥1024px). The
  legacy rule is deleted; one frame, matching the Remote page.
- **The sub-tab strip no longer draws its own sunken tray.** Videos and
  Games stacked a second outlined tray (identical chrome to the main
  tab bar) directly under the navigation; the sub-tab keys now sit
  straight on the plate so the main tray is the only tray on any tab.

### Added
- **Restoring a backup now updates the running kiosk immediately.** The
  web admin's Restore used to write settings.json that the kiosk's next
  operator-action save would silently clobber with stale in-memory
  state — the restore looked successful and then evaporated. The
  restore endpoint now drops a reload marker; the kiosk polls it (~1s),
  re-reads settings.json, re-runs the per-board audio reconcile,
  applies volume/audio output live, and shows a "Settings restored"
  toast — or "restart to apply display mode" when the restored display
  mode differs, since the DRM mode and logical canvas are chosen at
  boot. Verified live end-to-end on the box.

### Changed (glyph atlas + thumbnails)
- **Game-browser thumbnails decode off the render thread.** Selecting a
  game used to stbi_load the thumbnail PNG (disk read + full decode,
  tens of ms on a Pi 4) synchronously mid-frame — a visible hitch on
  every selection change while scrolling the list. The decode now runs
  on a worker (same candidate-path search, including the disc/version/
  region filename fallbacks) and the render thread uploads the pixels
  when they land — the thumbnail appears a frame or two later instead
  of stalling the frame, and a fast scroll chases the newest selection
  rather than queueing stale decodes.
- **Text rendering moved from per-glyph textures to shelf-packed atlas
  pages with batched draws.** Every glyph used to own its own GL
  texture, and `draw_text` paid one texture bind + vertex-buffer upload
  + draw call PER GLYPH PER FRAME — a full menu spent hundreds of
  driver round-trips a frame on text alone. Glyphs now rasterize
  (identically — same stb_truetype path, same RGBA conversion) into
  1024×1024 shared pages with a 1px transparent border against LINEAR-
  sampling bleed, and `draw_text` accumulates all quads into a single
  buffer upload + draw call per atlas page (in practice one per call).
  Atlas pages are the only glyph GL objects, freed/rebuilt on
  cleanup/reset_gl exactly like the other texture caches.

### Fixed (hardening batch 10 — SD wear, display policy, Wi-Fi UI)
- **kiosk_status.json writes drop from ~432k/day to a 2s heartbeat at
  idle.** The status file was rewritten 5×/second unconditionally —
  every write a temp-file + rename cycle of SD metadata churn — even
  when nothing but the embedded timestamp changed. The writer now
  serializes without the timestamp as a change-detection key: real
  content changes (playback position, screen transitions, game
  sessions) still write immediately, identical bodies write only every
  2 seconds. Both freshness consumers stay honest — verify_box asserts
  the embedded ts is under 5s old, and the phone remote's broadcaster
  (which polls mtime) also gets fewer no-op parse wakeups.
- **Interlaced display modes are no longer preferred over progressive.**
  `pick_mode` compared raw refresh, so a TV advertising 1080i60
  alongside only lower-rate progressive timings landed the whole kiosk
  on an interlaced mode — which the pipeline never deinterlaces.
  Progressive now outranks interlaced (below the EDID-preferred rule,
  so interlaced-native panels are still honored; interlaced-only sizes
  remain selectable). Four new mode-selection tests.
- **The Settings INFO screen stops forking nmcli on the render
  thread.** Its once-per-second rebuild called the synchronous Wi-Fi
  status getters — 1-3 fork + D-Bus round-trips per second, 50-300ms
  each on a loaded Pi 4B, a visible stutter on the very screen an
  operator watches during setup. Status reads now come from a cached
  snapshot refreshed off-thread on a 3s TTL, warmed at boot and
  invalidated the moment a connect/forget changes ground truth.

### Fixed (hardening batch 9 — render performance)
- **The pairing/Content Manager QR code renders as one cached texture**
  instead of re-running the QR encoder and issuing ~500 per-module
  quads (each a buffer upload + draw call) every frame. Rebuilt only
  when the payload changes; freed with the other texture caches on
  cleanup/GL reset. Visual output unchanged (GL_NEAREST reproduces the
  crisp module squares).
- **CRT / composite / bloom shader passes stop looking up uniforms by
  string every frame** (~20 driver round-trips/frame). Locations are
  cached per (program, name) and the cache is invalidated wherever
  programs are deleted — GL recycles program ids, so a stale entry
  would silently target the wrong uniform after a reset.
- **A missing bezel or marquee asset no longer costs 3 disk probes and
  a stderr line per frame, forever.** Failed paths are remembered and
  skipped; one fresh attempt is allowed per GL reset in case the asset
  arrived since.
- **Dropped dead mipmap generation** on logo/thumbnail/system-logo
  uploads — their min filter is GL_LINEAR, so the generated chain could
  never be sampled; it only cost upload time on every texture load.

### Fixed (hardening batch 8 — platform/input layer)
- **Unplugged keyboards and rotary encoders no longer leak.** The
  hotplug drop path was gated on `is_joystick`, so a non-joystick
  device that vanished leaked its fd + libevdev handle and stayed in
  the poll loop as a dead node forever. Any device reporting `-ENODEV`
  is now dropped.
- **Event-buffer overflow can no longer latch a stuck d-pad.** On
  `SYN_DROPPED` the poller read past the marker with the NORMAL flag,
  discarding libevdev's resync replay — a direction held at overflow
  time whose release fell in the gap scrolled the menu forever. The
  resync stream is now drained properly and the d-pad latches cleared
  (the same protective reset the unplug path uses).
- **A wedged NetworkManager can no longer hang the kiosk.** WifiManager
  subprocesses defaulted to NO timeout; nmcli D-Bus stalls blocked the
  calling thread forever — permanently latching the scan/connect flags
  ("scanning…" until reboot) or freezing the render thread into the
  watchdog. Default is now 15s (the existing timeout+kill machinery,
  just actually engaged). Worker threads are also tracked and joined
  instead of detached — a detached worker capturing the singleton could
  outlive it at process exit and publish into freed memory.
- **DRM CRTC snapshots stop leaking across RetroArch cycles.** The
  saved-CRTC allocation was freed only on the restore path; the
  handoff path left it live with the pointer dangling into the next
  `initialize()`, which overwrote it — one leaked snapshot (plus a
  stale-fd restore hazard) per game session.
- Removed GpioManager's dead encoder-CLK initialization (read a GPIO
  line the request never asked for; nothing consumed the value) and
  corrected its startup log — encoder rotation comes via evdev, only
  the push-switch is GPIO.

### Fixed (hardening batch 7 — Media Browser thread safety)
- **All four HTTP clients' error strings are now race-free.** Screens
  read `last_error()` / `peek_error()` on the render thread while their
  workers run client calls that write it — every accessor returned a
  bare reference to the live `std::string` (torn reads, use-after-free
  of the old buffer). Radarr, TMDB, Prowlarr, and qBittorrent clients
  now write through a mutex-guarded `set_error()` and return copies.
- **Quick-add from the playback overlay no longer freezes the movie.**
  It ran `get_quality_profiles` + `add_movie` (two 5s-timeout calls)
  inline while video played — ~10s frozen picture worst-case, and on a
  Pi 4B (Radarr docker-paused during playback) the freeze was
  guaranteed. Both calls now run on a worker that composes the outcome
  toast; the overlay shows "Adding…" meanwhile.
- **Browse/Search background workers are reaped as they finish** via a
  shared `WorkerPool` (the Prowlarr done-flag pattern extracted). The
  screens previously kept every finished page-fetch/lookup thread
  pinned — stack and kernel task — until process exit.
- **QueueScreen stops re-downloading the entire Radarr library every
  1.5 seconds.** The full library (heaviest Radarr response, fetched
  ~57,000×/day) only feeds poster backfill and the awaiting list — it
  now refreshes on a 30s TTL, with an immediate bypass when the queue
  contains a movie the snapshot doesn't know. Queue + live qBit
  telemetry keep the 1.5s cadence.

### Fixed (hardening batch 6 — Media Browser render thread)
- **Confirm Remove no longer risks a watchdog kill.** The 4-step
  cleanup (queue cancel → history walk + qBittorrent purges →
  `remove_movie`) chained 3+ sequential HTTP calls on the render
  thread; a hung Radarr blew past `WatchdogSec=10` and systemd killed
  the kiosk mid-remove. The identical steps now run on a worker thread
  with a "Removing…" banner, actions gated while in flight, a
  same-movie guard on completion, and the state invalidation +
  Library navigation applied on the render thread.
- **The Filter view's genre load no longer blocks the render thread.**
  `get_genres()` was called synchronously ("only ~200ms" — the happy
  path; the TMDB client's retry ladder holds a dead egress up to ~76s).
  Now single-flight on a worker; the picker shows an empty genre list
  until the result lands.
- **Prowlarr's release search can no longer freeze the UI on a hung
  upstream.** `search_async` force-joined its oldest worker on the
  render thread once more than 4 were tracked — up to the full 30s
  search timeout. Workers now flag their own completion and are reaped
  with instant joins only.
- **Artwork fetch failures back off.** A 404 poster or an egress outage
  re-enqueued the same URL every UI frame, forever, with per-attempt
  log spam. Network failures now hold the URL for 30s before retry, and
  repeated 4xx responses mark it dead through the same counter the
  decode-failure poison already used.

### Fixed (hardening batch 5)
- **`systemctl stop` now shuts the kiosk down cleanly.** Every OTA
  restart delivered SIGTERM to a process with no handler — the kiosk
  died mid-frame, `STOPPING=1` was never sent, and the GL/EGL/DRM/
  pipeline cleanup after the main loop never ran even though it
  existed. A SIGTERM/SIGINT handler (SA_RESTART, sig_atomic_t flag) now
  requests a normal loop exit; `TimeoutStopSec=5` still bounds a wedged
  cleanup.
- **Production builds no longer compile the test suite.** New
  `BUILD_TESTS` option (default ON — dev builds and CI unchanged);
  update.sh's on-Pi OTA rebuild and the release workflow pass
  `-DBUILD_TESTS=OFF`, skipping every Catch2 suite, the MB test CLI,
  and the Catch2 GitHub fetch — real minutes per OTA on a Pi, and one
  less network dependency in the update path.
- **Concurrent phone-remote writers can no longer corrupt shared
  files.** Two phones seeking at once raced on one fixed
  `seek_request.json.tmp` staging name; `paired_remotes.json` had three
  unsynchronized writers sharing one `.tmp` (a lost race could drop a
  freshly paired phone). Both now stage through unique `mkstemp` files;
  the pairing trust store is also fsync'd before rename so a power cut
  can't zero it and silently unpair every phone.
- **Removed the dead MPV-era files in `src/video/`** (`controller.cpp`
  — an un-compiled divergent twin of the real `app::Controller` —
  plus `mpv_player` and `mpv_renderer`), which invited edits to files
  nothing builds.
- **`stb_image`'s implementation moved out of `renderer.cpp`** (the
  project's largest TU) into its own tiny TU — renderer edits stop
  recompiling the whole decoder on every build.

### Fixed (hardening batch 4)
- **Formatting a new drive can no longer detach the live movie
  library.** The storage-prepare endpoint lazy-unmounted `/mnt/ssd`
  unconditionally before touching ANY disk — formatting a second drive
  while the MOVIES drive was in use yanked the library out from under
  the running containers (downloads kept writing into a detached
  filesystem; imports failed with nothing in any log). It now refuses
  outright to create a second MOVIES-labeled disk (fstab mounts by
  label, so which one wins after a reboot would be arbitrary) and only
  releases `/mnt/ssd` when the target itself is the MOVIES drive.
  Wipe-and-rebuild of the current drive still works.
- **Interrupted uploads no longer corrupt the library.** Media and ROM
  uploads saved straight onto the final filename from byte 0 — a
  dropped transfer left a truncated file that listed as real content,
  and re-uploading over an existing file destroyed the original the
  moment the transfer STARTED. Uploads now stage into a same-directory
  dot-tmp file and atomically replace on success only. Backup restores
  likewise now use the fsync'd atomic writer for playlists, settings,
  and device identity instead of bare `write_bytes`.
- **The fuzzy video-path fallback can no longer resolve to the wrong
  title.** `find_fuzzy_match` accepted any file merely STARTING with
  the target name, so a playlist entry for "Title.mp4" silently played
  "Title 2.mp4" when the real file went missing — winner decided by
  directory-iteration order. The stem must now be followed by a
  recognized suffix boundary: exact end, ` (` (ID/year), ` [` (yt-dlp
  source IDs — what shipped media actually uses), or `.` (encode
  variants). 15 unit tests pin the rule.
- **Rotary volume no longer stalls the render thread.** Each detent
  forked amixer twice, synchronously, on the render thread (~10-30ms
  per fork on a loaded Pi 4) — a fast spin queued several events in one
  frame and stalled rendering >100ms exactly while the user watched the
  volume slider. The software volume still applies per-detent; the
  amixer pair is deferred to the next state tick, coalescing bursts to
  one apply per frame and skipping identical values entirely.
- **deploy_cpp.sh builds with `-j2` on a Pi 4B** (detected from the
  device tree) — `-j4` OOMs the 1.5 GB board and presents as a random
  compiler crash; update.sh learned this long ago and deploy never did.
- **pairing_audit.log is capped at 512 KB** (rotates to the newest
  256 KB on a line boundary). Nothing ever pruned it, and its
  diagnostic value is the recent tail.
- **The C++ unit suites and the web pytest suite now run in CI** on
  every push (`test-local.yml`: cpp-unit via `-DBUILD_KIOSK=OFF` +
  ctest, web-pytest from the repo root). Until now they ran in no
  workflow at all — the drift they exist to catch was invisible unless
  someone remembered a manual run.

### Changed
- **Games now launch ONLY from Settings → Video Games — never from
  main-menu playlists.** Playlists are partitioned between the two UI
  surfaces at load time (`PlaylistLoader::split_for_ui`): the main menu
  sees only a playlist's video items, the Settings game browser only its
  game items, and a mixed video+game playlist simply appears in both
  places. Previously a mixed playlist rode the main menu whole (any
  video item made it a "video playlist"), so a finished video could
  auto-advance into a RetroArch launch with nobody in the room — and
  its games were meanwhile invisible to the Settings browser (which
  required ALL items to be games). Auto-advance, next/prev, and Master
  Shuffle can now never encounter a game item, as data rather than as
  scattered guards. Covered by 8 unit tests and verified on hardware
  with a live mixed playlist (appears on both surfaces; counts restore
  on removal). Pure video and pure game playlists behave exactly as
  before.

### Fixed
- **Settings and controller-profile saves now survive a power cut.**
  Both used tmp-write + atomic rename with no fsync, so a power cut
  could journal the rename ahead of the file's data and leave a
  zero-length file — and `peek_is_crt_native()` treats an unreadable
  settings.json as CRT_NATIVE, silently rebooting a MODERN_TV 1080p
  unit into 720p CRT mode with no error anywhere. A captured controller
  profile (minutes of wizard work) could vanish the same way. Saves now
  fsync the temp file's data before the rename and the directory after
  (`utils/fsync_util.h`), warn-only on failure so save semantics are
  unchanged. Deliberately NOT applied to StatusWriter's 5-per-second
  status file — fsync there would burn SD lifespan for a file that's
  rewritten 200ms later.
- **One kiosk systemd unit, not three.** The canonical unit
  (`magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service`:
  Type=notify, WatchdogSec=10, Restart=on-failure, EnvironmentFile for
  the Media Browser keys, scheduling priority) was installed only by
  deploy_cpp.sh. `scripts/setup_pi.sh` installed a stale root-level copy
  — Type=simple with NO Restart= (a crash meant a dead box until
  power-cycle) and no EnvironmentFile — and `setup_boot_service.sh`
  generated a third variant from a heredoc (User=root, Restart=always,
  and the After=network-online.target ordering the canonical unit
  documents as a measured ~14s black-screen regression). Both installers
  now install the canonical file (setup_boot_service.sh refuses to run
  from a non-/opt checkout rather than enable a unit with wrong paths),
  and the stale root-level copy is deleted.
- **Partial transcodes can no longer appear in the media library.**
  ffmpeg encoded straight into `media_dir/<name>.mp4`, so for the whole
  multi-minute encode a growing, moov-less (unplayable) file was listed
  by the Content Manager and addable to a playlist — and a service
  restart or power cut mid-encode stranded it there forever,
  indistinguishable from a real video. Encodes now go to a
  `<name>.mp4.part` staging name (invisible to every `*.mp4` glob, with
  an explicit `-f mp4` since the muxer can no longer be inferred from
  the extension) and are published with an atomic same-directory
  `os.replace` only when ffmpeg exits 0; failures unlink the staging
  file, and startup sweeps crashed leftovers alongside upload_temp.
  Verified with a real encode on the box: output probes as valid mp4,
  no `.part` residue, final file appears atomically.
- **One flaky core download no longer aborts every remaining core
  install.** install_cores.sh runs under `set -e`, and its download
  loop's failure branch was dead code: a failed bare `wget` killed the
  whole script mid-loop, so a single transient network error left every
  remaining core uninstalled — worst from update.sh's OTA cores
  bootstrap, which then continued with games that cannot launch (and
  `--pi` bench mode masked the bug by shipping the function without the
  `set -e` prologue). The wget/unzip pair now runs as the `if` condition
  with `--timeout=30 --tries=2`, failed downloads log ✗ and move on, and
  the temp zip is always cleaned up. Pinned by a bats test that runs the
  real script with a stubbed failing download (verified on the Pi: 9/10
  cores install around a failed one).
- **Orphaned upload/transcode temp files are swept at web-admin
  startup.** `data/upload_temp` (where TMPDIR points, holding transcode
  inputs, probe files, and import staging ZIPs) was never cleaned after
  a crash: job state is in-memory and the workers are daemon threads, so
  a service restart or power cut mid-job stranded up to 8GB per incident
  on the SD card, forever, with nothing pointing at the cause.
  create_app now empties the directory at startup — nothing in it can be
  live then — and logs the megabytes reclaimed. Verified live on the
  box: planted 32MB orphan swept on restart with a journal line.
- **tests/local/update_rsync_excludes.bats works again** — the suite
  went silently red when the 2026-07-30 OTA fix added
  `--include thumbnails/systems/***` lines to update.sh's rsync blocks
  (the block parser only tolerated `--exclude`). The parser now accepts
  both, and a new test pins the include's presence and its
  before-the-blanket-exclude ordering in all four blocks.
- **Backups now actually contain the device settings.** The web admin's
  Backup/Restore derived the settings.json path as
  `magic_dingus_box_cpp/config` — a directory the kiosk never reads —
  so settings were silently omitted from every backup ZIP ever made,
  and a restore wrote them where nothing would find them while
  reporting success. Both endpoints now use the kiosk's real
  `/opt/magic_dingus_box/config` (pinned by regression tests, verified
  against the live box). Backups made before this fix restore fine —
  they just never had settings inside to begin with.
- **migrate_box.sh backups can no longer capture torn SQLite
  databases.** Backup mode rsynced `services/config/` while Radarr /
  Prowlarr / qBittorrent were still writing (WAL mode) — an
  inconsistency that SQLite would only report at restore time, onto the
  freshly flashed box, after the owner's original SD was wiped. The
  backup now stops those three containers (~30s; Gluetun stays up),
  copies, and restarts them — with an EXIT trap so the owner's stack
  comes back even if a copy fails mid-way. Also fixed in passing: the
  `.env` rsync always died on the missing `services/` parent directory
  (rsync only creates the last path component), so backup mode had
  never actually survived past the content copy on a provisioned box.
  Verified end-to-end against real hardware: clean-stop copy checkpoints
  the WAL (no `-wal`/`-shm` sidecars in the copy) and both databases
  pass `PRAGMA integrity_check`.
- **The main-menu "Movies" playlist scans Radarr's real library root.** The
  synthetic Movies playlist defaulted to `/mnt/ssd/library/Movies` — a
  subdirectory Radarr never wrote to (movies land directly at
  `library/<Title (Year)>/`; setup_services.sh dropped the empty `Movies/`
  dir long ago) — so the row silently never appeared on the main menu no
  matter how many movies were downloaded. The default now lives in
  `PlaylistLoader::kMoviesLibraryRoot = "/mnt/ssd/library"`, pinned by a
  unit test to Radarr's actual root (must agree with
  `RadarrClient::Config::host_library_prefix`), with scan tests covering
  the one-subdir-per-movie layout, the legacy empty `Movies/` leftover,
  non-movie clutter, and case-insensitive extensions
  (`tests/app/test_movies_library.cpp`).

### Fixed
- **Games launched outside the Settings browser no longer get killed by
  the systemd watchdog.** The launch bracketing (watchdog disable,
  phone-remote status writes, GPIO restart-button polling, media-stack
  quiet mode, artwork pause) lived only in the Settings game-browser
  SELECT branch; the four other routes into a game item — main-UI SELECT
  on a mixed playlist, NEXT/PREV, auto-advance, Master Shuffle — left
  WatchdogSec=10 armed while the main thread blocked in waitpid, so
  systemd SIGABRT'd the kiosk (and the running game) ~10s in. The
  bracketing now runs inside `Controller::load_playlist_item` via
  session hooks installed once at startup, exception-safe on every exit
  path.
- **Media Browser playback exits to the movie's page at natural end
  instead of freezing on the last frame.** A GStreamer pipeline stays in
  PLAYING after posting EOS, so `update_state()`'s state poll overwrote
  the EOS flag within the same call and the player could never report
  "stopped" — `state.video_active` stayed latched true and the
  natural-end detector never fired. An EOS latch (cleared on stop, new
  load, or a seek out of EOS) fixes it; scrubbing back from the end
  still resumes playback.
- **qBittorrent session expiry no longer wedges the kiosk.** The 403
  re-login path in `http_get` called the request lambda while holding
  the non-recursive cookie mutex the lambda itself locks — a guaranteed
  self-deadlock whenever the SID went stale (~1h expiry, or any qBit
  restart under the Gluetun cascade). The wedged worker then blocked the
  next render-thread qBit call forever, and the watchdog killed the
  kiosk mid-"Play movie". The lock is now scoped to the cookie reset +
  re-login only.
- **qBittorrent pause/resume/delete no longer report success on auth
  failure.** `http_post` only checked the curl transport code, so a
  stale-but-present SID got a 403 that looked like success: `pause_all`
  logged "paused all torrents" while torrents kept hammering the drive
  during playback, and Confirm Remove counted purges that never
  happened. It now mirrors `http_get` — status check, one re-login
  retry, `last_error_` set on every failure — which also makes the
  documented qBit 4.x endpoint fallback actually trigger on 404.

## [1.7.2] - 2026-07-30

The release-infrastructure release: the OTA pipeline was audited end to
end ahead of the Pi 5 golden image, and one golden image now serves both
Pi 4B and Pi 5 units.

### Fixed (OTA pipeline)
- **Media Browser is no longer compiled out of OTA builds.** Both the CI
  release binary and update.sh's on-Pi rebuild now pass
  `-DENABLE_MEDIA_BROWSER=ON` (the option defaults OFF and was only ever
  set by deploy_cpp.sh; the clean-build fix removed the CMake cache that
  masked this — the very next OTA would have removed the movie kiosk
  from every box).
- **The Release workflow builds green for the first time** — the
  MB-OFF-only compile error (`ui::Toast` include inside the MB `#ifdef`)
  broke every tagged build v1.6.2 → v1.7.1, so no release ever shipped
  assets. v1.7.2 ships the source tarball + a pre-built MB-ON ARM64
  binary (installs in ~1 min instead of an 8-10 min on-Pi compile).
- **Phone Remote pairing state survives OTA** (paired_remotes.json,
  flask_secret.key, pairing session/audit — previously deleted by the
  install rsync's `--delete` on every update, silently unpairing every
  phone).
- **System-tile thumbnails actually update via OTA** (an `--include` for
  `data/thumbnails/systems/` ahead of the blanket thumbnails exclude —
  the guarantees doc always promised this; per-game cover art stays
  preserved).
- **Installer hardening**: the source tarball is selected by name
  (never by asset order), the extracted tree is sanity-checked before
  any rsync, and the pre-built binary is `ldd`-probed so an
  incompatible-OS binary falls back to source compile instead of
  wedging the box in an install/rollback loop.

### Added (OTA pipeline)
- **Add-only playlist sync with two gates** — new default playlists
  reach existing boxes, but never duplicate a system the box already
  has a playlist for, and never before the box has the content.
- **Emulator-core self-healing** — after install, update.sh scans the
  box's playlists for referenced cores and runs install_cores.sh if any
  are missing (how N64/Dreamcast cores reach fielded Pi 5 boxes).

### Added (dual-board golden image)
- **One golden image serves Pi 4B and Pi 5.** The kiosk hides Pi 5-only
  game systems (N64, Dreamcast) on a Pi 4B at runtime
  (`PlatformProfile::unsupported_game_systems` +
  `PlaylistLoader::filter_for_platform`); first_boot.sh prunes their
  ROMs from Pi 4 clones to reclaim SD space; config.txt gained
  `[pi4]`/`[pi5]` conditional sections; setup_services.sh keeps the
  lean Radarr size preferences on Pi 4B.
- **White-glove migration** (`scripts/golden_image/migrate_box.sh`) for
  the fielded Bookworm-era Pi 4B units frozen at v1.6.4 (Trixie is the
  OS floor since v1.7.0 — the libgpiod 2.x migration).
- Tracked playlists reconciled with the golden image (`games_*` set +
  the four default video playlists; N64/Dreamcast playlists now in the
  repo so OTA can deliver them).

RetroArch performance-headroom round: quiet the media stack during game
sessions, reclaim PS1 audio latency, add heavy-scene video options, and
cool the boot config. Video contract (Vulkan/khr_display, viewports,
bezels) and the two shipped pads' emitted mappings unchanged byte-for-byte.

### Added
- **Playback stall watchdog** — a live box wedged on 2026-07-29 and stayed
  wedged for seven hours. A burst of navigation input (a controller pinned
  against something) tipped a playlist switch into its timeout path; the
  recovery restored the kiosk's "playing" flags but left GStreamer PAUSED.
  PulseAudio held the proof — the kiosk's own sink-input read `Corked: yes`
  while the kiosk reported `is_paused = false`. Position sat at 0.00, the TV
  showed a still frame, and nothing detected the mismatch. The box then
  reloaded item after item, confirming each at position 0, which is what had
  the fan running.

  `app::PlaybackStallWatchdog` now checks the one signal that separates
  "wedged" from "fine": a video the kiosk believes is playing must advance.
  Position still for 3s while playback is expected → restart playback, retried
  no more than once per 8s so it cannot hammer `play()` into never succeeding.
  Pure logic with an injected clock, because the real failure is only
  reproducible by corking an audio stream.

  Verified on hardware by reproducing the wedge (`pactl suspend-sink`): the
  watchdog fired 4 times at exactly the 8s cadence. Note the recovery ACTION is
  not isolated by that test — an administratively suspended sink cannot be
  resumed by the client, so `play()` had no path to succeed. What is proven is
  detection, plus zero false positives across a full 9-core launch/return
  sweep, which is the risk that actually matters (a watchdog that restarts
  video mid-playback would be worse than the bug).
- **Game launch screen rebuilt so it cannot look broken** — it used a rotating
  square and a sine-pulsed text alpha, both purely time-driven. The kiosk can
  only draw until it hands DRM master to RetroArch, and the frame presented at
  that instant stays on the panel until RetroArch takes over the display, so
  the freeze left a spinner stopped at a random angle. Measured on hardware:
  of a 4.3s launch, **0.43s was animated and 3.90s was one frozen frame** — the
  spinner was stuck for 90% of the load. A stopped spinner reads as "hung",
  which is the worst possible thing to leave on screen.

  Two changes. First, everything that needs no display now happens BEFORE the
  handoff: input teardown, the udev controller wake-up, and (via the new
  `LaunchOptions::before_fork` hook) the launcher's ~0.6s of script generation.
  The kiosk now animates through all of it and releases the display in the
  instruction before `fork()`. Second, the screen no longer depends on motion
  at all — a titled plate showing the game and system, with a chunky segmented
  progress bar stepped by real launch phases, driven to FULL on the last frame.
  The frozen image is a completed bar, which reads as "about to start", and it
  is honest: everything the kiosk controls really has finished by then.

  Result, same title, measured: **1.62s animated / 3.11s frozen** — the
  animation runs ~4x longer and the remainder is almost entirely RetroArch's
  own initialisation, which no amount of code can draw over. 9/9 cores pass
  after the launch-path change.

  The same plate now also covers the RETURN from a game, which previously
  showed the stale launch frame: re-acquiring DRM master puts the kiosk's own
  last framebuffer back on the scanout, and that still read "STARTING" with a
  full bar — so exiting a game looked like launching one. It flips to
  "RETURNING" with a green frame and bar (gold going in, green coming back, so
  direction is readable without reading a word), repaints as soon as the
  display is back, and steps through the restore phases.

  One thing that does NOT work, tried and reverted: painting that first return
  frame *before* `set_mode()`. RetroArch leaves the display on its own mode,
  which does not match the kiosk's EGL surface, and `present_frame` then blocks
  on a page-flip event that never arrives — launches went 4.3s to as much as
  15.8s and returns 3.4s to 5.7s. The paint has to come after the mode restore.
- **Dreamcast is playable** — `kernel=kernel8.img` under `[pi5]` in
  `config.txt` switches the Pi 5 from its default 16 KB memory pages to
  4 KB. flycast hardcodes a 4096-byte page size for Linux/aarch64
  (`core/stdclass.h` gives a runtime page size to Android and a 16384
  constant to Apple silicon; everything else falls through to
  `#define PAGE_SIZE 4096`), so `virtmem::region_unlock()` rounded
  addresses to a 4 KB boundary and called `mprotect()`, which returns
  `EINVAL` on a 16 KB-page kernel — flycast treats that as fatal and
  hits a `DEBUGBREAK`. Every Dreamcast launch died ~4s in, 100%
  reproducible, with nothing wrong in the core, the ROM, or the config.
  Confirmed three ways: `getconf PAGESIZE` = 16384, a standalone
  `mprotect` repro returning errno 22 on this board, and disassembly of
  `region_unlock` showing `and x0, x0, #0xfffffffffffff000`. Rebuilding
  flycast does *not* help — the constant is selected by platform macros
  at compile time, not from the host. `kernel8.img` and its module tree
  ship in the stock image, so this is a supported switch, and it is a
  whole-system change: every process now runs 4 KB pages.
  `emulator_smoke_test` is 9/9 after it, with no regression in the other
  eight cores.
- **Dreamcast core options** — `write_dreamcast_core_options()` pins
  threaded rendering, native 640x480, USA/NTSC and real-BIOS-preferred,
  switches the cable type to progressive VGA (the core default is
  composite, which is right for a CRT and wrong for an HDMI panel),
  gives every game its own VMU instead of sharing eight save files
  library-wide, and disables DCNet so a shipped box does not open
  connections to a third-party cloud relay nobody agreed to. Keys and
  values were read out of the shipped `flycast_libretro.so`'s
  `retro_core_option_v2_definition` table rather than its UI labels —
  those differ, and the difference matters: the cable-type value is the
  bare token `"VGA"`, not the `"VGA (RGB)"` the menu shows, and
  RetroArch would have dropped the longer form silently.
- **`verify_box.sh` checks the page size** when Dreamcast ROMs are
  present. This is the one check that separates "Dreamcast works" from
  "Dreamcast is dead on this box", and it is invisible everywhere else —
  the core loads, the ROMs verify, and the failure only appears once a
  game actually starts.
- **Raspberry Pi 5 groundwork: platform profile** — new
  `platform::PlatformProfile` (`src/platform/platform_profile.{h,cpp}`)
  detects the board from `/proc/device-tree/model` at startup and
  carries per-model hardware facts (analog-audio availability, GPIO
  header chip labels). Covered by a new portable Catch2 target
  (`test_platform_unit`, `tests/platform/`) plus
  `tests/local/audio_sink_resolution.bats`.
- **Dynamic PulseAudio sink resolution** — audio sinks are resolved at
  runtime against `pactl list short sinks` instead of the hardcoded
  Pi 4 bus addresses (`platform-fef00700.hdmi` /
  `platform-fe00b840.mailbox`), in both the kiosk binary
  (`AudioSettings::resolve_output_sink()`) and `init_audio.sh` (new
  `scripts/resolve_audio_sink.sh`). A "headphone" preference on a board
  with no analog jack degrades to HDMI instead of silence; USB DACs and
  I2S HATs are picked up as analog sinks.
- **GPIO header chip discovery by label** — `GpioManager` scans
  `/dev/gpiochip0..7` and selects the header chip by gpiod label
  (`pinctrl-bcm2711` on Pi 4, `pinctrl-rp1` on Pi 5) instead of
  hardcoding `/dev/gpiochip0`, surviving the Pi 5's chip-number
  shuffles across kernel releases.
- **RetroArch launch contract carries the board model** —
  `LaunchOptions.pi_model` (from `AppState.platform_profile`) gives
  Pi-5-specific video tuning a single landing spot. The emitted
  contract is deliberately identical on both boards until the Pi 4
  V3D swapchain workarounds are re-benchmarked on Pi 5's V3D 7.1;
  a parity test pins that decision.
- **Pi 5 bring-up checklist** in `scripts/golden_image/CLONING.md` —
  base OS, `[pi4]`/`[pi5]` config.txt sections, overlay carry-over
  (incl. the hand-baked `rotary-encoder` line that must be copied
  from the Pi 4 image), power/cooling, and a first-boot validation
  list. Docs (`README`, `CLAUDE.md`, `gst_player.cpp` decode notes)
  updated for dual-board reality.
- **Game quiet mode** — launching any game now pauses qBittorrent
  torrents and stops the Radarr/Prowlarr/Byparr containers for the whole
  session (mirrors movie playback's existing behavior), restoring them
  on return. Serialized async worker (`GameQuietMode`); launch is never
  delayed, and a fast pause→resume flip can never strand services
  paused. No-op on unprovisioned Pis.
- **`ArtworkCache::clear_textures()`** — poster textures + queued
  uploads are dropped at game launch (cache rebuilds lazily from disk on
  return), so they don't sit in RAM/GPU memory during gameplay.
- **VPN monitor game-session awareness** — health polling skips game
  sessions (Radarr is intentionally down) plus a 90 s post-session grace
  while containers restart, so quiet mode never triggers a false
  "tunnel down" toast.
- **Smoke-test dynarec assertion** — PS1 smoke runs now fail loudly if
  the core ever silently drops the ari64 dynarec for Lightrec or the
  interpreter (multi-x slowdown guard).
- **"Searching indexers now…" download indicator** — the Queue screen's
  AWAITING RELEASE section now distinguishes movies Radarr is actively
  searching for right this second (pulsing green, live count in the
  section header) from those passively waiting for the next 30-minute
  sweep. Closes the "I just added it, why is nothing happening?" gap:
  the add-time search runs immediately, and now the user sees it. Reads
  Radarr's running-command list (`/api/v3/command`).
- **Controller Setup wizard** (Settings → Controller Setup): press-each-button
  capture for any USB gamepad, walking the operator through every logical
  control for their pad's style (PlayStation-style or N64-style) with
  skip/redo support (`CaptureSession`). Captured profiles are keyed by USB
  VID/PID and stored in `config/controller_profiles.json` — configure a
  controller model once and every future plug-in of that model is
  recognized automatically. Profiles resolve independently per port, so two
  *different* controller models can drive player 1 and player 2
  simultaneously in a two-player game. The same captured profile also
  drives the kiosk's own menu navigation, so a third-party pad the box
  previously could not navigate menus with now works immediately after
  capture — no restart needed. Survives OTA updates (see
  `OTA_UPDATE_GUARANTEES.md`).
- **Settings → System → Reset Controller Setup** — the wizard's undo.
  Erases the captured profile store so every pad falls back to its
  built-in/legacy mapping. Two-press confirm, and the row is hidden when
  nothing has been captured. Without it a bad or incomplete capture could
  only be corrected by completing all 24 wizard steps again, on the pad the
  bad capture had just broken — the profile file is deliberately immune to
  OTA updates, so there was no path back to the shipped mapping at all.

### Changed
- **Settings → Audio Output cycle is platform-aware** — the Headphone
  option is skipped on boards without a 3.5mm jack (Pi 5), and a stale
  `"headphone"` value loaded from settings.json (e.g. a golden image
  cloned from a Pi 4) is coerced to Auto at startup.
- **`init_audio.sh` sets the default sink after PulseAudio starts**
  (via `resolve_audio_sink.sh`) rather than baking a hardcoded sink
  name into `default.pa` before startup; `module-stream-restore
  restore_device=false` still keeps streams following the default.
- **PS1 audio latency 64 → 48 ms** (alsathread) — 30 s THPS2 ALSA soak
  showed zero underrun retriggers at 48 ms; `spu_thread` stays enabled.
- **`video_frame_delay_auto = "true"`** (all cores, both display modes) —
  RetroArch backs the 4 ms frame delay off automatically in heavy scenes
  instead of stuttering.
- **Per-title PS1 performance overrides** — THPS4 runs with
  `psxclock = 65` + `nostalls = enabled`: its slowdown is authentic PS1
  engine chug (30 fps target, low-20s in big parks on real hardware)
  faithfully reproduced by the emulator at <20% of one Pi core; the
  overclock lets the engine hit its frame target. Scoped per-title
  (filename match) because both knobs can break timing-sensitive games.
  (An earlier `gpu_thread_rendering = "async"` attempt was removed —
  this core build has no THREAD_RENDERING support, so it was inert.)
- **Boot config:** `force_turbo=1` removed (idle downclock → ~6 °C
  cooler idle; `performance` governor still pins ARM at 2 GHz in use)
  and `gpu_mem` 128 → 76 (KMS/V3D allocates from CMA, not firmware
  memory). Applied on the source Pi + recorded in the golden-image doc;
  rollback at `/boot/firmware/config.txt.bak-headroom`.

- **Fake-download warnings (Media Browser)** — two new user-facing
  signals for scam releases (observed live: three fake "The Odyssey
  2026" grabs the day after its theatrical premiere):
  - Detail shows an **"IN THEATERS — no digital release exists yet;
    downloads found now are almost always fakes"** banner for library
    movies whose Radarr status is tba/announced/inCinemas.
  - Detail shows a **"FILE LOOKS WRONG — X min file vs Y min
    expected"** banner (and Library shows the BAD RELEASE badge) when
    an imported file's measured duration deviates >25% from the
    movie's runtime — catches renamed trailers/junk that slip past
    pre-grab scoring. Radarr's `minimumAvailability=announced` stays
    (deliberate: early real releases still grab the moment they
    exist); these warnings cover the fake-release window instead.

- Controller mapping internals refactored into semantic tables + physical
  profiles (`build_mapping()`); output for the two shipped pads is
  snapshot-locked and unchanged. Player-bind emission moved to the
  Mac-testable `write_player_binds()`.

- **Wi-Fi setup overhaul (new-location flow)** — scanning now streams
  results onto the screen as they're found (animated "Scanning…" header
  with a live count) instead of a fixed 4s "Please wait"; the scan
  itself adapts (polls up to 12s, finishes early once results settle),
  fixing the near-empty first scan on a cold cache — exactly the
  fresh-boot-at-a-new-house case. Disconnect is now named ("Disconnect
  from <SSID>"), hidden when nothing is connected, and toasts its
  outcome. A failed saved-network reconnect now offers "Enter New
  Password" right on the failure screen instead of a dead-end message.

- **Media Browser screen helpers deduplicated, and Library's storage
  readout drops a decimal** — four helpers had been copy-pasted across
  the nine Marquee screens: the ellipsis truncator (**6** copies), the
  Knuth-hash poster tint (**6**, under three different names —
  `poster_tint_for_tmdb`, `library_tint_for_tmdb`, `tint_for_queue_id`),
  the two cubic easings (2 each), and the byte formatter (3). This is the
  same shape as the "Recently added" bug below: two copies of one
  function drift, the one missing a check silently breaks something, and
  no test can see either because both live in translation units that
  name `ui::Renderer` — `Renderer` pulls in GLES, GLES exists in no test
  target, so the whole TU cannot compile outside the kiosk binary — and
  because each copy sat in an anonymous namespace with no external
  linkage to reach it by. Only the truncator names a `Renderer` itself;
  the tint, the two easings and the byte formatter are pure and name
  nothing, and were untestable purely by association with the file they
  happened to live in. They are
  now one implementation each in `media_browser/ui/mb_ui_utils.{h,cpp}`,
  which deliberately names no `Renderer` — that is what makes them the
  first UI helpers in this directory with actual unit tests (20 cases
  pinning the exact tint RGB for specific ids, every unit boundary, the
  truncator's edge cases, and — as present behavior, not as correct
  behavior — the mojibake the truncator emits when its byte-wise cut
  lands inside a multi-byte UTF-8 sequence; **that last one is now fixed
  and those expectations rewritten — see "Truncated text no longer
  mangles accented, CJK and emoji titles" under Fixed**). The ~20
  existing `truncate_to_width` call sites are untouched: a one-line
  forwarding overload in `mb_chrome` carries their exact signature.

  **User-visible:** the byte formatter's three copies disagreed on
  rounding. Queue and the release picker dropped the decimal at 100 of
  the chosen unit; Library always printed one. The 2-of-3 majority wins,
  so Library's "N titles · X used · Y free" line and the same readout in
  its slide-in overlay now say **`500 MB` where they said `500.0 MB`**.
  Queue and the release picker are unchanged.

  Library's copy actually moved the decimal in *both* directions, and the
  other direction is the opposite of "always printed one": below 1024 B
  it short-circuited to `std::to_string(bytes) + " B"`, so it printed
  **no** decimal at all — `99 B`, `1 B` — where the shared helper now
  says `99.0 B`, `1.0 B` (and `-5 B` becomes `0 B`). Unreachable in
  practice rather than a risk worth weighing: Library's two inputs are
  `std::filesystem::space().available` (0, or ≥ 4096 on ext4) and a sum
  of movie `file_size_bytes`, and neither lands in the 1–99 byte window
  or goes negative. Noted because the majority rule was applied here
  too, not special-cased.

  The formatter's `<= 0` behavior was deliberately **not** unified. The
  three copies rendered it as `"0 B"`, `"?"` and a plain integer, and
  those are different *meanings*, not drift — the release picker's `"?"`
  means "the indexer reported no size", Queue's `"0 B"` means "size
  known, nothing transferred yet". Collapsing them would have destroyed
  information, so the shared helper covers `bytes > 0` and each caller
  keeps its own guard at the call site.

  `library_screen.cpp` now defines no local UI helper at all, which
  finishes what the dead-scaffolding removal below started — the two
  uncalled statics deleted there were duplicates of exactly two of these
  four helpers.

- **The Library grid's filter and sort logic is now tested** — it was
  inlined in `LibraryScreen::rebuild_view()`, and `library_screen.cpp`
  belongs to no test target: it names `ui::Renderer`, `Renderer` pulls in
  GLES, and GLES exists in no test binary and not at all on the mac dev
  box. So the rules deciding which movies an operator sees, and in what
  order, had **zero** automated coverage. That is not a hypothetical gap —
  it is precisely why the "Recently added" cutoff bug below shipped and
  sat there: a well-formed-but-wrong date turned the filter into a
  pass-everything filter, and there was no test that could have noticed,
  because there was no test.

  The decision logic moved to `media_browser/ui/library_view.{h,cpp}` as
  two pure functions — "does this row survive this filter" and "filter
  then sort" — which name no `Renderer` and compile into
  `test_media_browser_unit`. The split is along the impurity line rather
  than a feature line: reading the clock for the 30-day cutoff, the
  latched warn when that read fails, and clamping the grid cursor all
  stay in the screen, because none of them can be a pure function of
  their arguments. The cutoff crosses the seam as a
  `(string, bool valid)` pair, which is what makes the show-all fallback
  reachable: the branch the bug lived in used to require a machine whose
  `gmtime_r` had actually failed, and is now the first case in the test
  file.

  **No behavior change.** Every rule is preserved verbatim, including the
  ones that look like mistakes and are not: the `Unwatched` filter still
  keeps every row (a deliberate placeholder — the kiosk tracks no watched
  history yet, and an empty grid would read as "nothing is here" rather
  than "not implemented"), the `!recent_cutoff_valid ||` short-circuit
  stays a real branch rather than being "simplified" into a comparison
  against an empty string, and `Title` sorting stays `strcasecmp`, so
  `apple` still precedes `Banana`. 21 cases / 71 assertions, written
  before the implementation, pin all of that plus the `>=` cutoff
  boundary, the year sort's case-insensitive title tiebreak, sizes past
  the 32-bit boundary, an empty library across all 16 filter/sort pairs,
  and the borrowed-pointer contract by address.

### Fixed
- **The box could not explain its own reboots** — Raspberry Pi OS ships
  `/usr/lib/systemd/journald.conf.d/40-rpi-volatile-storage.conf` with
  `Storage=volatile` to spare the SD card, so the journal lived entirely in RAM
  and every reboot erased it. Found the hard way: a Pi 5 rebooted mid-deploy on
  2026-07-29 and the cause is permanently unknowable — `/var/log/journal` existed
  but held zero journal files, `journalctl --disk-usage` reported 6.5 MB all under
  `/run`, and `journalctl --list-boots` knew exactly one boot. No prior boot, no
  shutdown reason, no watchdog history. On a fielded unit that is the difference
  between "it rebooted, here's why" and a shrug.

  `install_deps.sh` now installs a drop-in overriding that default, with caps that
  mitigate the very thing it was protecting: 200M total, 20M per file, 10 files,
  one month retention, and journald compresses by default. A kiosk emits a few MB
  per boot, so this buys several boots of history for negligible flash wear. If SD
  longevity ever becomes pressing, lower `SystemMaxUse` rather than reverting to
  volatile — losing all history is not a wear-levelling strategy.

  Lives in `install_deps.sh` specifically because `update.sh` re-runs it on every
  OTA, so **already-shipped boxes self-heal** rather than only new clones getting
  it.

  **The filename prefix is load-bearing.** Drop-ins apply in lexical order and the
  last assignment wins. The first attempt was named `10-mdb-persistent.conf` and
  was silently beaten by the distro's `40-`; `systemd-analyze cat-config` showed
  `Storage=persistent` followed by `Storage=volatile`. Hence `99-`. The script's
  success check verifies an on-disk journal actually appeared rather than assuming
  the write worked, and its failure message points at the load order.
- **PS1 gets its second analog stick and its stick clicks** — on a
  PlayStation-style pad, PS1 ran with `core_option_pad_type = "analog"`, so
  `pcsx_rearmed` presented a DUALSHOCK to the game: two sticks, two stick
  clicks. Only one of those sticks was ever bound. A pad captured through the
  Controller Setup wizard (SHANWAN "Android Gamepad", 2563:0526) yielded 24
  controls, and inspecting the binds a launched PS1 game actually received
  showed **18 of the 24 reaching it**. Missing: the whole right stick
  (captured as `-2`/`+2` on X, `-3`/`+3` on Y) and L3/R3 (`13`/`14`).

  The pad was never at fault. The wizard captured all 24 correctly and the
  physical profile stored them; the gap was downstream, in two different
  places:

  `semantic_ps_style()`'s PS1 branch set face buttons, L1/R1/L2/R2,
  Select/Start and the D-pad, and simply never set `r_up`/`r_down`/`r_left`/
  `r_right` — so `build_mapping()`'s right-stick block had nothing to
  resolve and left the four `r_*_axis` fields empty. Only the two N64
  mappings had ever populated them, which is why the right stick worked on
  N64 (as the C-button cluster) and nowhere else.

  L3/R3 were worse: **the plumbing did not exist at all.**
  `ControllerMapping` had no `l3_btn`/`r3_btn` fields and
  `write_player_binds()` emitted no `l3`/`r3` lines, so no pad on any core
  had ever been able to reach a stick click. RetroArch has always exposed
  them (`input_playerN_l3_btn` / `input_playerN_r3_btn`), and the
  `LogicalControl` vocabulary has always had `L3`/`R3` — the wizard was
  asking for two controls that had nowhere to go.

  Practical effect: dual-analog PS1 titles were broken. Ape Escape is built
  entirely around both sticks and is unplayable with one; right-stick camera
  games had no camera.

  PS1 ONLY, deliberately. Every other PS-style mapping was audited and left
  alone, because each is already right: NES, SNES, Genesis, Atari 7800, PC
  Engine and Arcade have neither a second stick nor stick clicks; the N64's
  right stick is correctly spent on the C cluster and the console has no
  stick clicks; the Dreamcast has one stick plus two analog triggers. A
  dedicated test asserts every one of those cores keeps emitting EMPTY
  `l3_btn`/`r3_btn` — present but empty, since RetroArch treats an empty
  value differently from an absent line — so the fix cannot leak into a
  console that never had the hardware.

  `stick_to_dpad` was deliberately NOT cleared for PS1, even though the
  adjacent N64 branch clears it. The two are unrelated: `stick_to_dpad`
  governs the `*_axis` D-pad binds and reads the LEFT stick, a disjoint set
  of RetroArch settings on disjoint axes (0/1 vs 2/3) from the right-stick
  fields this change adds. The N64 branch clears it to match what the legacy
  N64 table emitted, not because it gained a right stick. Clearing it here
  would have removed four currently-emitted binds and stopped the left stick
  doubling as the D-pad on every fielded box — a behavior change PS1 never
  asked for.

  A partially-captured profile degrades cleanly rather than guessing: L3/R3
  route through the same kind-aware `put_btn` contract as the face buttons,
  so a control the operator skipped, or one captured as an axis, emits `""`.
  The shipped built-in DragonRise profile is exactly that case — it has a
  real right stick but no L3/R3 — and it now gets the stick with empty
  clicks.

  The `[mapping_snapshot]` golden was regenerated, which needs justifying
  because it had been byte-identical since it was created. 34 lines moved,
  all accounted for: **33 additions** of a new `l3r3=,` line, one per golden
  entry (11 cores x 3 controller types), EMPTY in all 33 because neither
  built-in pad has an L3/R3 binding — so no core gains a stick click through
  `get_mapping()`; and **1 modification**, `rs_axis=,,,` to
  `rs_axis=+2,-2,+3,-3` on `PS|pcsx_rearmed_libretro` alone, which is the
  fix itself. Nothing else: no face buttons, no D-pad, no left stick, no
  hotkeys, no `pad_type`, no `analog_dpad_mode`. The new fields were added to
  the snapshot's serializer on purpose (as their own line rather than
  appended to the face-button line, so a genuine face-button regression
  cannot hide inside a line the reviewer already expects to move) — leaving
  them out would have put two shipping fields outside the safety net.

  Two other byte-exact tests moved for the same reason and are annotated
  inline: the production-hardware capture regression and the
  legacy-equivalence transcription each gained two lines per player. Neither
  could have contained them — L3/R3 had no plumbing when that hardware
  capture was taken or when that legacy block was written.
- **Truncated text no longer mangles accented, CJK and emoji titles** —
  `media_browser::ui::truncate_to_width` cut on BYTES
  (`text.substr(0, n)`, n counted down one byte at a time), so any cut
  landing inside a multi-byte UTF-8 sequence emitted an orphaned lead
  byte before the `"..."`. `ui::decode_utf8` returns U+FFFD for that
  orphan, so the kiosk drew a **replacement box**. The cut now snaps back
  to a codepoint boundary via a new `ui::utf8_floor_boundary` in
  `ui/text_utf8.h`.

  Not theoretical, and not rare: truncation is the *common* case in the
  poster grid, and TMDB titles are full of non-ASCII — `Amélie`,
  `Léon: The Professional`, `千と千尋の神隠し`. Worse, the screens embed
  `•` and `…` themselves when composing metadata lines, so a
  genre/runtime row could be mangled on a box whose library is 100%
  ASCII titles.

  CJK was by far the worst hit: at 3 bytes per character, two byte
  offsets in three land mid-sequence. Measured over every truncating
  width, the old cut produced a replacement box for
  `千と千尋の神隠し` at **14 of 24** widths, `Alien 🎬 Extra` at 3 of 16,
  `Drama • Fantasy` at 2 of 17, and `Amélie Poulain` /
  `Léon: The Professional` at 1 each (a single `é` gives exactly one bad
  offset) — 21 bad (string, width) pairs across the six strings the sweep
  test covers, and 0 for the pure-ASCII control.

  The `mb_ui_utils` tests that **pinned** this as present behavior now
  assert the clean output instead — that was their stated purpose, and
  the diff is the record of exactly which strings changed. Added: one
  case per sequence length (2-byte `é`, 3-byte CJK, 4-byte emoji), a
  regression guard that a cut already *on* a boundary is not over-trimmed
  (the obvious wrong fix — "always walk back one codepoint" — silently
  drops a character that fit), and the re-decode sweep above, which
  proves *absence* of U+FFFD in a way no hand-picked expectation set can.

  **Also now O(log n) in `measure` calls.** The old scan called `measure`
  once per byte — a font/GL round-trip each on the kiosk — which the
  header had flagged as costing "hundreds of text-width calls per frame"
  for a long synopsis. A binary search over codepoint boundaries takes a
  1000-byte synopsis from **~950 calls to 11**, pinned by a
  call-counting test (the return value is identical either way, so a
  regression to the linear scan would otherwise be invisible). This
  assumes rendered width is non-decreasing in prefix length — true for
  the LTR text the kiosk draws, false only under a shaper doing negative
  kerning or RTL reordering, which this kiosk has no engine for. Stated
  in the header rather than left implicit.

  **Deliberately NOT grapheme-aware.** Boundaries are codepoints, so a
  combining accent or emoji ZWJ sequence can still be split between its
  codepoints. Full UAX #29 segmentation needs property tables — a real
  dependency for a text stack that is one `stb_truetype.h` — and
  `font_manager.cpp` already draws one glyph per codepoint with no
  cluster composition, so a combining mark renders as a separate spacing
  glyph whether or not it is cut. Cutting mid-cluster is therefore no
  worse on screen than cutting anywhere else; cutting mid-codepoint
  produced an actual replacement box. The invalid-UTF-8 class of failure
  is gone, which is the part that was visibly broken.

  Contract otherwise unchanged and still pinned: `<=` not `<` on the fits
  check, and the bare `"..."` fallthrough when even one codepoint plus
  the ellipsis will not fit.

- **The same byte-wise cut in 12 more places** — auditing for siblings
  turned up `while (width > budget) s.pop_back();` loops, which is the
  identical defect one character at a time: `cap_lines` in
  `playback_overlay.cpp` and its copy `truncate_wrapped` in
  `detail_screen.cpp`, the overlay's title / CAST / DIRECTOR / similar-title
  trims (8 sites), `library_screen.cpp`'s two-line title fallback (2), and
  the game-loading panel title in `ui/renderer.cpp` — that last one outside
  the Media Browser entirely, so RetroArch loading screens were exposed too.
  All now call `ui::utf8_pop_back`. Surgical by design: only the pop is
  codepoint-aware now, no restructuring, no comparison-operator changes.

  `mb_settings_screen.cpp` and `mb_chrome.cpp` were checked and do no
  slicing of their own — they call the shared truncator, so they were
  fixed by the change above. `browse_screen.cpp` / `search_screen.cpp`
  likewise: the dedupe commit had already pointed their two-line title
  fallback at `truncate_to_width`, and `library_screen.cpp` /
  `playback_overlay.cpp` are the two that still hand-roll it — worth
  collapsing, but a behavior change (the hand-rolled version returns the
  *full overflowing* title when nothing fits, where the shared helper
  returns `"..."`) and so left alone here.

  The word-wrap helper `wrap_text_overlay` needed no fix: it splits on
  `' '`, and a space byte cannot occur inside a multi-byte sequence.

  New `ui::utf8_*` primitives live in `ui/text_utf8.h` — header-only over
  `<cstdint>`/`<string>`, no GL, no link tail — and are tested in
  `tests/ui/test_text_utf8.cpp`, i.e. in `test_ui_unit`, which builds in
  **both** `ENABLE_MEDIA_BROWSER=ON` and `OFF`. That matters because
  `ui/renderer.cpp` uses them and is not a Media Browser file.

- **`ctest` silently skipped the largest test binary in the repo** —
  `test_media_browser_unit` was created with `add_executable` but never
  registered with `add_test`. Every other test target has a
  registration; this one had none, so `ctest` reported "8 tests passed"
  while quietly omitting **the entire Media Browser suite** (148 cases /
  4948 assertions as of this entry). The binary was still compiled on
  every build — it just never ran unless someone remembered to invoke
  `./test_media_browser_unit` by hand.

  That is the worst failure mode for a test suite: a green `ctest` that
  reads as full coverage. Anything the Media Browser tests would have
  caught — the Radarr/Prowlarr parsers, the release picker, the download
  watchdog, the VPN health monitor, the shared UI helpers — could regress
  through a clean run. Now `add_test(NAME MediaBrowserUnit ...)`, so
  `ctest` reports **9** with `ENABLE_MEDIA_BROWSER=ON` and 8 with it OFF
  (the target does not exist when the flag is off, which is correct).

  The neighbouring `test_media_browser` target — same directory, no
  `_unit` suffix — is deliberately **still** unregistered, and a comment
  now says why so nobody "finishes the job" later. Despite the name it is
  not a test binary: it is a subcommand-driven CLI for exercising the
  subsystems by hand on hardware (`test_media_browser tmdb-search alien`).
  Invoked bare it prints help and exits **2**, so registering it would
  add a permanently failing test, and every real subcommand needs API
  keys plus a live TMDB / Radarr / libtorrent stack. Live scaffolding
  worth keeping, just not through `ctest`.

- **`ENABLE_MEDIA_BROWSER=OFF` did not build at all** — `main.cpp` calls
  `ui::Toast::show()` from the display-mode change path, which is core kiosk code
  compiled in every configuration, but `#include "ui/toast.h"` sat inside
  `#ifdef MEDIA_BROWSER_ENABLED`. With the flag off the compile failed on
  `'ui::Toast' has not been declared` — not degraded behavior, no binary.

  It hid behind a two-part blind spot: production always deploys
  `MEDIA_BROWSER=true`, and on macOS the OFF config never builds the kiosk target
  at all (it needs DRM/GLES), so configuring OFF on a dev machine builds only
  test binaries — none of which include `main.cpp`. The break reproduced solely
  when building the kiosk binary on a Pi with the flag off, the one combination
  nobody runs. Half of it had already been found once and half-fixed:
  `CMakeLists.txt` appends `toast.cpp` to `UI_SOURCES` unconditionally with a
  comment explaining why, which resolved the matching *link* error and left the
  *include* error standing.

  Audited the rest of that block rather than just the failing line: of the 20
  headers gated behind `MEDIA_BROWSER_ENABLED` in `main.cpp`, `ui/toast.h` was
  the only one referenced from non-gated code.
- **Dead Media Browser scaffolding removed** — `MEDIA_BROWSER_UI_SOURCES` in
  `CMakeLists.txt` was defined and consumed by no target, with a comment implying
  it kept those files out of the test binary. It did nothing, and it sent anyone
  tracing where the MB screens get compiled down a dead end. All 8 files it named
  are already listed in `KIOSK_MEDIA_BROWSER_SOURCES`. Also deleted two uncalled
  statics in `library_screen.cpp` that warned `-Wunused-function` on every Pi
  build: `poster_tint_for_tmdb()` was a byte-for-byte duplicate of
  `library_tint_for_tmdb()` further down the same file (the one `render()`
  actually calls), and `truncate_to_width()` was superseded when the screen moved
  to `chrome::draw_poster_card()`.
- **"Recently added" could silently show the whole library** —
  `LibraryScreen::rebuild_view()` built its 30-day cutoff by calling `gmtime_r`
  and **discarding the return value**. On failure `gmtime_r` returns nullptr and
  leaves the `tm` zero-initialized, so `strftime` produced
  `"1900-01-00T00:00:00Z"` — a well-formed string that every real `added_at`
  compares greater than. The filter passed every row while looking like it had
  filtered, with no crash and nothing in the log. The comparison is a bare
  string `>=` (ISO-8601 sorts chronologically as text), which is what let a
  garbage date fail so quietly.

  The formatter is now one shared `utils::iso8601_utc(std::time_t)` — extracted
  rather than duplicated, because the second copy added for the controller
  wizard's `captured_at` was already drifting from this one (it checked both
  error returns; this one checked neither). It lives in `utils/` because its two
  callers are `retroarch/` and `media_browser/` and pointing either at the other
  would couple two independent subsystems.

  On failure the filter now falls back to **show-all with a logged warning**,
  which is deliberate: an empty grid reads as "your library is empty", a scarier
  and more misleading failure on an appliance than an unfiltered one. Note the
  fallback had to be an explicit branch, not just a better formatter — the
  helper returns `""` on failure and `added_at >= ""` is true for every row, so
  a mechanical swap would have reproduced the same silent pass-everything bug
  through a different route.

  The helper's contract is "either empty, or exactly 20 characters" — enforced
  with a width check, not merely documented, because any off-width stamp is
  well-formed enough to pass a caller's is-it-empty check and then sort wrongly
  against real 4-digit dates. That check is on width rather than on a year range
  because `%Y` is precisely where the two libcs disagree; measured for year 1,
  Darwin zero-pads to `"0001-01-01T00:00:00Z"` (20 chars) while glibc/aarch64
  emits `"1-01-01T00:00:00Z"` (17). A year-range contract would have been a
  false claim on one platform; "20 chars or nothing" holds on both.

  `library_screen.cpp` compiles only into the kiosk binary (it needs a GL
  Renderer, so it is in no test target), so this call site is verified by
  compilation and by the shared helper's unit tests, not by a test of the filter
  itself.
- **Captured controller profiles now record when they were captured** —
  `PhysicalProfile::captured_at` was plumbed end-to-end (declared as ISO-8601,
  written by `profiles_to_json`, parsed back by `profiles_from_json`) but the
  wizard's save path hardcoded `""`, so every profile on every box carried an
  empty stamp. It is the only signal for "was this pad captured before or after
  the mapping change I am debugging" — with two pads captured on the bench Pi
  and both stamped empty, there was no way to order them. Now stamped via a new
  `utils::iso8601_utc(std::time_t)`, which takes the instant as a parameter
  so the format is assertable in a unit test, and uses `gmtime_r` — UTC because
  the stamp carries a literal `Z` and a Z-suffixed local time is not imprecise
  but wrong, and the `_r` form because the kiosk runs background threads that
  would race `gmtime`'s shared static. Profiles captured before this change keep
  their empty stamp until re-captured; the parser already tolerates it.

  Read the stamp as approximate on a cold box: a Pi has no RTC, so a capture
  done before NTP syncs is stamped from fake-hwclock's saved value and can sit
  hours or days early. Still strictly more informative than the empty string it
  replaces, but it is not evidence of ordering across a reboot.
- **Deploys and OTA updates no longer link stale object files** — both
  `deploy_cpp.sh` and `update.sh` keep `build/` out of their rsync, so it is a
  long-lived directory holding objects from previous source trees. Incremental
  `make` is not safe across a change that alters a struct layout: objects
  compiled against the old header link cleanly against objects compiled against
  the new one, and the resulting ODR violation corrupts the heap. The build
  succeeds and the kiosk segfaults at startup inside an unrelated destructor —
  unattended, on a fielded box, that is a brick. `update.sh` now always builds
  clean (rollback is unaffected; the backup taken beforehand still includes
  `build/`). `deploy_cpp.sh` fingerprints every header plus `CMakeLists.txt`
  and wipes `build/` only when that fingerprint moves, so ordinary `.cpp` edits
  still build incrementally; `--clean` forces it.
- **A failed kiosk restart no longer latches the unit down** — `deploy_cpp.sh`
  used `systemctl restart`, which could return before the old process released
  DRM master, the input-device grabs, and its PulseAudio child, so the
  replacement raced the corpse and died; `Restart=always` then tripped
  systemd's start-rate limiter and the unit stuck in "start request repeated
  too quickly" even after a known-good binary was restored. The deploy now
  stops, waits for the process to actually exit, clears any latched failure,
  starts, confirms the service came up, and prints the journal if it did not.
- **Captured analog controls no longer land in digital bind fields** —
  `build_mapping()` picked the RetroArch field from the semantic table and
  the token from the pad's profile without checking the two agreed. On a
  pad whose d-pad is an analog axis pair with no hat, and whose triggers are
  analog (the class the wizard exists to support), a capture emitted
  `left_btn = "-0"` and `right_btn = "+0"` — and RetroArch reads anything
  not starting with `h` as a button INDEX, so both resolved to physical
  button 0. Bindings now follow the profile's kind: an axis d-pad goes to
  the `*_axis` fields, and no analog token can reach a `*_btn` field. The
  two shipped pads emit byte-identical configs (33/33 snapshot goldens
  unchanged).
- **The wizard can no longer save a pad-disabling profile** — capturing one
  control and skipping the rest wrote a near-empty profile that
  unconditionally shadows the built-in one for that VID/PID (i.e. for both
  players, since the shipped pads share an ID) and left the RetroArch menu
  hotkey unbound. Saving now requires at minimum the four d-pad directions
  plus confirm and Start, and the TEST screen names what is still missing.
- **A failed profile save no longer reports success** — a read-only `/opt`
  or a full SD card produced "Saved!" and a success toast over a write that
  never happened. The store's real result is now propagated, with a
  retryable failure state.
- **Wizard footers name controls that actually work** — they advertised a
  gamepad "B: cancel" that cannot fire (raw capture diverts every joystick
  for the whole run) and never named the phone remote's real cancel. Every
  phase now lists only live controls, by the labels printed on the box and
  on the phone.
- **Games were never rendered into the bezel's screen cutout** — three
  separate faults in the same six lines, none of which produce a log line:
  1. The settings were emitted as `video_custom_viewport_*`. RetroArch's
     names carry no `video_` prefix (`custom_viewport_x/y/width/height`), so
     it did not recognise any of them — and `video_custom_viewport_enable`
     is not a setting at all. The viewport was dead config.
  2. `aspect_ratio_index` was `22` with a comment claiming that meant
     "custom viewport". `22` is `ASPECT_RATIO_CORE`; custom is `23`. So the
     picture was sized from whatever aspect each CORE reported — which is
     why the consoles did not agree with each other: SNES reports 1.306,
     Dreamcast 1.333, and each drew a different rectangle.
  3. `custom_viewport_x/y` are an offset from CENTRE on this driver, not
     absolute screen coordinates. Setting x to the cutout's absolute left
     edge (251) pushed the picture 251px right and put the bezel's control
     panel inside the playfield.
  Now `aspect_ratio_index = 23` with the viewport at offset `0,0`, sized to
  **fill the full 1080 height at whatever the picture's true aspect is** —
  `1440x1080` (exact 4:3, `1440*3 == 1080*4`) for every core that has no
  border to crop, wider only for the N64 titles carrying a measured overscan
  crop. Geometry is therefore never stretched, and the picture always touches
  top and bottom. Excess width tucks under the bezel, which is an overlay
  drawn on top of the video — the same overscan a real CRT had. Clamped to
  the 1920 panel width, past which RetroArch would letterbox to fit and the
  fill-the-height guarantee would be lost. Verified by capturing the
  composited frame off the Pi over RetroArch's network command interface and
  measuring it.
- **N64 games left black margins inside the bezel** — many N64 titles draw
  less than the full framebuffer; a CRT's overscan hid it, a flat panel does
  not. It is per title: Banjo-Kazooie borders three sides, Super Mario 64
  only the bottom, Mario Kart 64 none, so one global crop would fix Banjo and
  clip Mario Kart. GLideN64's `mupen64plus-EnableOverscan` already defaults
  to Enabled but all four offsets default to `0`, so nothing was cropped.
  Added a per-title crop table (same shape as the existing `kPs1TitleOverrides`).
  Every value measured, not estimated: each title launched with the bezel
  disabled and offsets zeroed, ~10 frames captured across a minute via
  RetroArch's network `SCREENSHOT` command, frames too dark to measure
  discarded, and the MINIMUM border taken — the minimum because cropping past
  the smallest border any frame showed would clip real picture. Units are
  320x240 N64 pixels (4.5 screen px each), confirmed by cropping a known 20
  units and watching a 67px border go to exactly zero.
- **CRT Native pointed at a zero-sized viewport** — it emitted
  `aspect_ratio_index = 23` (custom) while writing no `custom_viewport_*`
  values at all. RetroArch falls back to full screen, so the picture was
  right by accident rather than by instruction. Now `0` (`ASPECT_RATIO_4_3`),
  which is the same picture on a 640x480 framebuffer with a defined reason.
- **The backup N64 core would have been sent a crop it cannot perform** —
  `parallel_n64` implements NONE of the five GLideN64 overscan options (its
  shipped `.so` contains zero of them where `mupen64plus_next` contains all
  five), so it hands over the full uncropped frame. The viewport, however, was
  sized from the crop table keyed on the ROM — so launching a cropped title on
  the backup core would have widened the viewport over an uncropped picture and
  stretched it horizontally, exactly the distortion the design exists to
  prevent. Both the options and the viewport now key off the core as well as
  the ROM; `parallel_n64` gets no overscan keys and a plain 4:3.
- **`prepare_for_cloning.sh` stashed one TMDB key path, `first_boot.sh` wiped
  a glob** — the clone got cleaned at first boot while a stray
  `tmdb_api_key.bak` on the source Pi rode along inside the `.img.gz` itself,
  which is the one artifact these secrets must never survive in. The stash list
  now expands globs so the two agree.
- **Multi-disc games outside PS1 got no `.m3u`** — the Content
  Manager's auto-generation was gated on `system == 'ps1'` AND invoked
  `generate_m3u_playlists.sh` with no argument, so it always scanned the
  PS1 directory no matter what had just been uploaded. Dreamcast ships
  two-disc titles of its own (Resident Evil - Code - Veronica, Skies of
  Arcadia) and flycast reads `.m3u` exactly like pcsx_rearmed does;
  without this, the discs stayed separate playlist entries that could
  not be swapped between and kept separate saves. Now driven by a
  `MULTI_DISC_SYSTEMS` set with the system's own ROM directory passed
  through, and the generator recognises `.gdi`/`.cdi` alongside the
  formats it already handled.
- **`verify_box.sh` failed a box for having no Dreamcast BIOS** — now a
  WARN. It is not a blocker (flycast falls back to its HLE BIOS and the
  shipped library boots on it) and not something an image can fix
  (`dc_boot.bin` is Sega firmware and cannot be redistributed). The
  check also only looked at the top of the system directory, reporting
  "missing" on a box with the BIOS installed in the `dc/` subdirectory
  flycast's own documentation describes.
- **`test_app_unit` could not build on macOS** — the target compiles
  `playlist_loader.cpp` for the playlist trim/loop parsing tests, but
  the yaml-cpp probe lived inside a Linux-only block, so
  `YAMLCPP_INCLUDE_DIRS` came back empty and the build died on
  `'yaml-cpp/yaml.h' file not found`. The suite had quietly stopped
  being part of a Mac test run. Now probed unconditionally (not
  `REQUIRED`), with the target skipped and a warning printed when
  yaml-cpp is genuinely absent.
- **Dreamcast box art was not gitignored** — every other system's
  `data/thumbnails/<system>/` directory is excluded; `dreamcast/` was
  missing from the list, so 20 PNGs of scanned cover art would have
  been committed on the next `git add`.
- **Phone remote can now quit games** — the QUIT_GAME chord (KEY_Z +
  Start on the virtual "MagicDingus Phone Remote" gamepad) was silently
  ignored in-game: the virtual pad has no manual joypad binds and
  autoconfig is disabled, so no RetroArch bind ever saw it. The launch
  contract now emits `input_exit_emulator = "z"`
  (`write_remote_quit_config()`), which RetroArch's udev keyboard path
  delivers from the virtual device (verified on hardware). UI launch
  test: 14/14 chord exits.
- **Pi 5 game audio routed to the empty HDMI port** —
  `detect_alsa_device()`'s PRIORITY-1 regex used a `^` anchor that
  never matches mid-string in std::regex, so every launch fell through
  to a hardcoded `plughw:1,0`: correct on Pi 4 only by card-ordering
  luck (vc4hdmi0 = card 1 there) and silently wrong on Pi 5 (card 1 =
  vc4hdmi1, the empty port). Now selects `sysdefault:CARD=vc4hdmi0` by
  NAME via the tested `retroarch::pick_hdmi_alsa_device()`.

- **Open (passwordless) Wi-Fi networks never connected** — the UI's
  empty-password path routed them through "activate saved profile",
  which always failed with "unknown connection" for a never-saved
  network. Connect flow now takes an explicit fresh-vs-saved flag and
  open networks connect with a single press (no keyboard).
- **SSIDs containing ':' parsed with a stray backslash** (nmcli -t
  escaping was never undone) and could not be connected to.
- **Smoke harness:** waits for the settings menu to actually close
  between games instead of a blind settle — the quiet-mode container
  resume right at return-to-menu could stale the status file long
  enough to make the next navigation start from a closed menu.
- **missing-search timer:** the boot catch-up run fired before Radarr
  was accepting connections and died on an uncaught connection reset,
  silencing missing-movie retries for 4h after every boot. Now waits
  for Radarr readiness (up to 120s) and treats socket errors as a
  clean retry-next-timer exit.

## [1.6.4] - 2026-04-30

Marquee filter overlays, search focus toggle, universal exit modal, playback overlay, and footer hint vocabulary pass. v1.6.3 redesigned the input grammar and added the Library overlay; this release extends that foundation with per-tab Discover filters on Browse, a keyboard↔grid focus toggle on Search, a BTN2 exit modal across all MB screens, a translucent playback overlay for browsing similar films without stopping the movie, and a full refresh of every footer hint label to match the finalized v1.6.4 binding vocabulary.

### Added
- **Discover-style filter overlay** for Popular and Top Rated tabs (BTN4 to open). Filter by genre, decade, min rating, runtime, original language, and sort order. Per-tab independent state; persisted to `config/settings.json`.
- **Search keyboard ↔ grid focus toggle** (BTN4). Type a query, press BTN4 to jump to the results grid, navigate posters with rotary, press to open Detail.
- **Universal exit modal** — BTN2 across all MB screens opens an "Exit Marquee?" yes/no modal. Quick-exit via BTN2 again. Replaces the contextual back button (tab navigation handles all within-MB nav).
- **Playback overlay** — rotary press during MB playback opens a translucent bottom panel showing detail header + similar-films strip (TMDB `/movie/{id}/similar`). Rotary press on a similar film quick-adds it via Radarr without disrupting playback.
- **6-tab Marquee strip** with Queue between Library and Settings (carried over from unreleased v1.6.3).
- **Library filter overlay** (sort + filter, slide-in panel, BTN4) (carried over from unreleased v1.6.3).
- **Icon-based footer hints** with squares for buttons + circle/octagon for rotary; per-screen labels for every input on every screen (carried over from unreleased v1.6.3).

### Changed
- **BTN2 grammar:** was "back" (v1.6.3 unreleased) → now "exit-to-kiosk" with confirm modal across all MB screens. **Exception: BTN2 remains play/pause during MB movie playback** (per user request).
- **BTN4 grammar on Detail:** was no-op → now "back to originating list" (replaces the role BTN2 used to play on Detail).
- **Footer hint vocabulary refreshed** across every MB screen call site to match v1.6.4 bindings. Key changes from unreleased v1.6.3 labels:
  - All screens: BTN2 label "Back" → "Exit".
  - Browse (Popular/TopRated): BTN4 "Filter" → "Filters", Rotary "Posters" → "Browse", RotaryPress "Open" → "Detail".
  - Browse (other categories): Rotary "Posters" → "Browse", RotaryPress "Open" → "Detail".
  - Search (kbd focus): Rotary "Keys" → "Type".
  - Search (grid focus): Rotary "Posters" → "Browse", RotaryPress "Open" → "Detail".
  - Library (all states): BTN4 "Filter" → "Sort+Filter", Rotary "Posters" → "Browse", RotaryPress "Open" → "Detail".
  - Queue: Rotary "Queue" → "Browse", RotaryPress "Detail"/"Confirm" → "—".
  - Settings (browse): Rotary "Rows" → "Select".
  - Settings (edit): BTN2 "Cancel" → "Exit", RotaryPress "Save" → "Confirm".
  - Detail: BTN4 "—" → "Back", Rotary "Buttons" → "Action", RotaryPress "Activate" → "Confirm".
  - Playback (no overlay): BTN2 "Pause" → "Pause/Play", BTN4 "Stop" → "—", Rotary "Seek" → "Scrub", RotaryPress "Info" → "Open Menu".
  - Playback overlay: Rotary "Scroll" → "Browse Similar", RotaryPress "Add" → "Quick Add".
- **TMDB Discover endpoint switching is hybrid:** when all filters are at "Any", the canonical `/movie/popular` and `/movie/top_rated` endpoints are still used (preserves existing behavior).
- **TMDB `with_genres` now uses pipe (`|`) for OR semantics** rather than comma (AND).

### Internal
- New components: `mb_filter_overlay`, `mb_exit_modal`, `playback_overlay`.
- Extended `tmdb_client::DiscoverFilter` for the full Discover param surface.
- Added `tmdb_client::get_similar()` for playback overlay pre-fetch.
- 12 new persisted fields in `DisplaySettings` (6 per Popular/TopRated tab).

### Notes for operators
- **The 6-tab strip layout is the new norm.** `Popular · Top Rated · Search · Library · Queue · Settings`. BTN1/BTN3 walk left/right with no wrap. Search now sits between Top Rated and Library — if you OTA-upgrade and your Pi was previously on Search, it stays there; the only change is that BTN3 from Search now lands on Library (not Settings).
- **BTN2 = exit-to-kiosk modal** across all Marquee menu screens. The modal requires a second BTN2 press (or rotary confirm) to actually leave — a single accidental tap is safe. PlaybackScreen is the exception — BTN2 stays play/pause during movie-watching.
- **The new `display.mb_library_*` keys persist across OTA upgrades and reboots** — see [OTA_UPDATE_GUARANTEES.md § v1.6.3 addition](OTA_UPDATE_GUARANTEES.md). New installs default to Recent / All. The "Unwatched" filter is a placeholder ("(soon)") until watched-history tracking lands; selecting it accepts the click but doesn't change the visible library.
- **12 new Discover filter fields** (`display.mb_popular_filter_*` and `display.mb_toprated_filter_*`) live under `config/*` and are preserved across OTA. On the first upgrade where they're absent, the load path applies canonical defaults (all "Any") — no visual regression.

## [1.6.2] - 2026-04-30

Marquee polish + Media Browser CRT preferences. v1.6.1 closed the design-fidelity gaps from v1.6.0; this release tackles the next round of Marquee refinements that surfaced during operator testing — strict palette compliance, navigation tightening, real TMDB poster art on every grid, a separate CRT-overlay store for the Media Browser menus, a working playback inset with the wood frame, and a noticeable performance win on movie cold-start.

### Added
- **MovieSettings → "Wood frame during playback" toggle** ([`mb_settings_screen.{h,cpp}`](magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.h), [`app_state.h`](magic_dingus_box_cpp/src/app/app_state.h)). Boolean toggle in the Library section that flips the wood-frame overlay on/off during movie playback. When ON (default), the video is inset 40 px on every side to fit cleanly inside the cabinet's interior. When OFF, the frame is hidden and the video fills the full 1280×720 framebuffer. Persisted as `display.mb_playback_show_frame` in `config/settings.json`.
- **MovieSettings → "CRT overlay" section** — 7 new rows (`Scanlines`, `Warmth`, `Glow`, `RGB mask`, `Bloom`, `Interlacing`, `Flicker`), each cycling **Off / Low / Medium / High** on rotary click via `DisplaySettings::cycle_setting`. The values write back to a parallel `mb_*_intensity` storage independent from the kiosk's home-menu CRT settings, so operators can dial in a different CRT look for the Marquee menus than they have on the home menu. Persisted as `display.mb_<name>_intensity` keys; on first load (upgrade path) the values inherit from the corresponding kiosk values so existing operators see no visual regression.
- **MovieSettings → "Auto-grab on add" toggle** — boolean toggle in the Library section that controls whether quick-add fires a Radarr search immediately. Stored in `LocalPrefs::auto_grab_on_add`.
- **MovieSettings "Refresh library" action row** — placeholder action that surfaces a "not implemented in MVP" banner on click. Hint text shows `▶ rescans <root_folder>` so the action's intent reads even though the implementation is deferred.
- **Section headers in MovieSettings** ([`mb_settings_screen.cpp`](magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp)). `RowKind::SectionHeader` renders gold ZenDots heading + thin steel-blue underline rule, and rows are now organized into 6 sections: **Library** (Quality preference / Auto-grab / Wood frame / Storage / Refresh), **Downloads** (sliders + bulk actions), **Sources** (Indexers), **CRT overlay** (the new 7-row block), **Diagnostics** (Services), **Danger zone** (Hide Movies feature). The cursor auto-skips section headers during navigation.
- **MovieSettings two-mode rotary navigation**. Default mode: rotary CW/CCW walks rows vertically. Click rotary on a cyclable row (Quality preference / sliders) → enters edit mode (focused row gets a 2 px gold rectangle border matching the active-tab vocabulary on the chrome strip), rotary then adjusts the value, click again exits. Click on a toggle (Auto-grab / Wood frame) flips inline. Click on an action row (Refresh / Retry / Pause / Resume / Hide Movies) executes immediately. CRT-overlay rows cycle Off/Low/Medium/High on click without entering edit mode.
- **`gst_renderer::set_render_inset(x, y, w, h)`** ([`gst_renderer.{h,cpp}`](magic_dingus_box_cpp/src/video/gst_renderer.h)). Lets callers push the video render into a sub-region of the framebuffer instead of the full screen. Aspect-preserve / letterbox / fill-width math runs WITHIN the inset rect. Used by main.cpp's Marquee playback path to inset the video 40 px on every side so the wood-frame overlay sits cleanly OUTSIDE the movie content. Default (`set_render_inset(0,0,0,0)`) uses the full framebuffer — legacy callers (intro video, playlist playback) see no change.
- **Fill-width pillarbox elimination for Marquee playback** ([`gst_renderer.cpp::render_quad`](magic_dingus_box_cpp/src/video/gst_renderer.cpp)). When the inset is active and the source is NARROWER than the canvas (e.g. 16:9 source in our 1.875:1 inset rect), the renderer fills the canvas WIDTH and lets the height overflow above and below the inset rect. The wood frame asset covers the overflow band cleanly, so the movie reads as completely filling the cabinet interior with no left/right black bars. The wider-than-canvas case (2.39:1 / scope) still letterboxes inside the cabinet — zoom-fill there would crop ~25% of the picture per side. Legacy paths preserve the standard pillarbox behavior.
- **Settings tab on the Marquee 5-tab strip**. The strip is now `Popular · Top Rated · Library · Search · Settings` (Now Playing dropped — see Changed). Settings transitions to MovieSettings via the dispatcher; PREV from Settings lands on Search.
- **Marquee CRT effects on menu screens** ([`main.cpp`](magic_dingus_box_cpp/src/main.cpp)). The legacy procedural CRT overlay (`render_crt_effects`) now applies on Browse / Library / Search / Detail / Queue / MovieSettings, gated off only during Playback. Reads from the new `mb_*_intensity` store via a per-frame snapshot/restore swap of the kiosk fields the renderer expects, so the home menu's CRT look is unaffected.
- **`EnvironmentFile=-/opt/magic_dingus_box/services/.env`** in the kiosk systemd unit ([`magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service`](magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service)). Optional load (the `-` prefix means "skip if missing") so unprovisioned Pis still boot the kiosk cleanly. Combined with a 3-stage Prowlarr key resolution (env → bare env → direct .env file parse) in `mb_settings_screen.cpp`, the Settings → Indexers row now correctly reads the operator's API keys regardless of whether systemd loads the env file or the kiosk falls back to parsing it directly.

### Changed
- **Wood frame overlay asset** ([`assets/marquee/marquee_frame.png`](magic_dingus_box_cpp/assets/marquee/marquee_frame.png)) replaced with a polished mahogany-cabinet variant matching v7 of the design source. 1280×720 RGBA, 224 535 bytes.
- **Playback viewport inset 30 → 40 px** ([`main.cpp`](magic_dingus_box_cpp/src/main.cpp)). Now matches the wood-frame asset's edge thickness exactly — video fills the cabinet interior with no gap, no overlap. The earlier 30 px value let wood pixels overlap the outermost movie pixels by 10 px, which read as crop on test content. Also routed through `gst_renderer.set_render_inset` instead of a bare `glViewport()` call (the earlier approach was silently overwritten by gst_renderer's own internal viewport call at the end of `render_quad`).
- **DetailScreen header rule moved up** ([`detail_screen.cpp`](magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp)). The 2 px steel-blue rule that previously sat between the right-column body and the action button row now sits directly under the chrome `"Feature Presentation"` header (matching PlaybackScreen's `NOW PLAYING — <title>` title-underline idiom). The action row is now visually anchored by the color-coded button borders themselves, no separator needed.
- **DetailScreen layout tightened to fit inside the wood frame.** `kPaddingX` 32 → 60 (matches `chrome::kSafeInset_px`), poster 320×480 → 280×420 (keeps 2:3), poster Y 84 → 144 (under chrome header + 24 px breathing room), section divider Y 588 → 570, action row top Y 608 → 582. Buttons end at Y=634, clearing the chrome footer-hint band at Y=646. Custom `"FEATURE PRESENTATION"` header replaced with `chrome::draw_screen_header("Feature Presentation", sub_info="<year> · BTN4 back")` for visual continuity with every other Marquee screen.
- **MovieSettings layout tightened to fit inside the wood frame.** `kPaddingX` 32 → 60, `kListTopY` 84 → 160 (chrome header bottom + 40 px), `list_bottom` `h−32` → `h−86` (clears the chrome footer band cleanly).
- **SearchScreen results grid harmonized with Browse / Library** ([`search_screen.{h,cpp}`](magic_dingus_box_cpp/src/media_browser/ui/search_screen.h)). Switched from a 5-col layout with bespoke poster cells to the same 9-col `chrome::draw_poster_card` system the other Marquee grids use. Cell width / poster height computed dynamically from safe-area width (matches Browse / Library byte-for-byte at 720p). Title meta wraps to 2 lines below the card; year shows on the bottom-right inside the card with the semi-transparent dark backing pill. `kPaddingX` 32 → 60 so the grid + keyboard sit inside the 40 px wood frame with a 20 px breathing gap.
- **Real TMDB poster artwork now renders on every grid** ([`mb_chrome.{h,cpp}`](magic_dingus_box_cpp/src/media_browser/ui/mb_chrome.h)). `chrome::draw_poster_card` gained an optional `poster_url` parameter; when set, the card calls `mb_draw_poster_or_tint` to draw the cached TMDB image (with the deterministic tint as fallback while the artwork cache fetches). All 3 callers (Browse, Library, Search) updated to pass their movies' URL fields. Pre-v1.6.2 the cards were tint-only — the chrome introduction in v1.6.0 inadvertently dropped real artwork rendering.
- **5-tab strip reordering: Now Playing dropped, Settings added.** New order: `Popular · Top Rated · Library · Search · Settings`. TMDB's `now_playing` endpoint overlapped almost completely with `popular`, so collapsing them removes a confusing duplicate. The `Category::NowPlaying` enum value, `label_for_category` arm, and `tmdb_.get_now_playing()` load arm in `browse_screen.cpp` are kept as dead code (no path can set `category_ = NowPlaying` anymore — defensive); future cleanup can remove them.
- **Library / Search / MovieSettings tab strip in chrome header.** All three screens now render the canonical 5-tab strip via `chrome::draw_screen_header`, with their own tab marked active (gold border + gold label). Search PREV → Library, NEXT → Settings; Settings PREV → Search, NEXT dead-end (rightmost tab); Library PREV → Browse, NEXT → Search.
- **MB background fill switched from `bg` to `bg_lift`** ([`renderer.cpp::mb_fill_background`](magic_dingus_box_cpp/src/ui/renderer.cpp)). The deeper bg (#1F191F) read as too cold/clinical for the Marquee menus; bg_lift (#2A232A) gives a warmer cabinet-like base that the gold accents and chrome card tints sit better on. Operator direction.
- **Strict 7-color Marquee palette compliance.** Every `th.accent2` / `th.action` reference in MB source files (18 sites across browse/detail/library/mb_chrome/mb_settings/playback/queue/search) replaced with `th.dim`. The Marquee design palette is `bg / bg-lift / fg / dim / accent / success (highlight1) / hot (highlight2)`; the kiosk-side steel blue stays in the home-menu UI (which still uses `theme.action`). Section dividers, header underlines, search "▶" prefix, status indicators, etc. are all `dim` now.
- **Playback HUD repositioned inside the wood frame** ([`playback_screen.cpp`](magic_dingus_box_cpp/src/media_browser/ui/playback_screen.cpp)). `NOW PLAYING — <title>` heading moved from y=38 (under top wood band) to y=70 with rule at y=90; bottom-right control hint moved from y≈696 (under bottom wood band) to anchored above y=h−56. Both overlays are now legible inside the cabinet during the title's 3-second fade.
- **Cursor marker direction flip** ([`mb_settings_screen.cpp::draw_cursor_marker`](magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp)). The blinking gold triangle on the LEFT side of focused settings rows now points RIGHT (▶, toward the label text) instead of LEFT (▸ pointing away). Action button cursors (Retry/Pause/Resume/Hide Movies — drawn inside a button on its right edge) keep their original LEFT-pointing direction since the tip points inward at the button label there. New `point_right` boolean on the helper, default `false` for back-compat.

### Fixed
- **Playback viewport inset wasn't actually applying.** `gst_renderer.render()` calls its own `glViewport()` at the end of `render_quad` using `width_ / height_` (the full screen dims), which silently overwrote any outer viewport main.cpp had set. Earlier 30/50/40 px attempts via bare `glViewport()` calls had no visible effect — that's why playback always looked full-screen regardless of the inset value. Routed through the new `set_render_inset` API instead, which applies inside `render_quad`'s glViewport call so it actually takes effect.
- **Stale ring artifact between wood frame and inset video.** When the inset shrunk the viewport, the band of pixels between the wood frame's inner edge and the new video edge kept the previous frame's stale content (the per-frame clear is gated on `!state.video_active`, which is false during active playback). Now main.cpp explicitly clears the framebuffer to black before applying the inset for Marquee playback, so the gap shows clean black or is cleanly covered by the wood frame.
- **MB grids stopped showing TMDB poster artwork after v1.6.0.** The chrome rework swapped grid cells from `mb_draw_poster_or_tint` to `chrome::draw_poster_card`, which only drew the placeholder tint — no real images. Posters returned to grid cells once `chrome::draw_poster_card` learned to take a `poster_url`.
- **MovieSettings "Indexers" row was unconditionally showing "Prowlarr API key not configured"** — `mb_settings_screen.cpp` only checked `MDB_PROWLARR_API_KEY` env var, but the codified setup writes `PROWLARR_API_KEY` (no prefix) to `services/.env`, and the systemd unit pre-v1.6.2 didn't load that file. Fixed by adding the 3-stage resolution chain (matches main.cpp's RADARR/PROWLARR resolution exactly): MDB-prefixed env → bare env → direct `.env` file parse. Combined with the new `EnvironmentFile=` line on the systemd unit, the env path is now the primary route on configured Pis and the file-parse path is the unprovisioned fallback.
- **MovieSettings rows extending under the wood frame.** Same root cause as DetailScreen — `kPaddingX = 32` was less than the wood frame's 40 px edge. Fixed by bumping to 60 (`chrome::kSafeInset_px`).
- **DetailScreen body extending under the wood frame.** Same root cause and fix as MovieSettings.
- **SearchScreen result cells not fitting / overlapping the chrome footer.** `grid_bottom` was anchored to `h - kHintMarginBottom - font_small - 8` which let the last row's title meta line render under the footer chips. Now anchored to `h - 86` (matches Settings/Detail conventions).
- **DetailScreen action button row crowding the chrome footer.** Buttons used to end at y=660 (chrome footer band starts at y=646). Fixed by moving the row up to y=582 (ending at y=634) so the chrome footer hints have 12 px of clearance.
- **`current_mb_screen != Screen::Playback` gate now guards `pump_artwork()`** ([`main.cpp`](magic_dingus_box_cpp/src/main.cpp)). TMDB image uploads are the most expensive non-decode work in the per-frame budget. Skipping them during playback eliminated the cold-start QoS frame drops on `1080p H.264` content (was ~25 drops in the first 3 seconds; now zero on a Pi 4 with active playback).

### Performance
- **Cold-start frame drops: ~25 → 0** for `1080p H.264 (BrRip x264 -YIFY)` content on a Pi 4 with the full Marquee chrome + CRT-overlay stack active. Achieved by gating `pump_artwork()` on `!Playback`. Pending TMDB poster fetches stay queued silently and resume the moment the operator returns to a menu screen. No visible behavior change.
- **Pi temperature under playback: 73°C → 71°C** (2°C drop) during the same workload, attributable to the same CPU/GPU work removed from the per-frame critical path.

### Notes for operators
- **The 7-color Marquee palette is now the contract for the Media Browser.** `bg / bg-lift / fg / dim / accent / success / hot`. Steel blue (#5884B1, the kiosk's `theme.action`) is no longer used inside `magic_dingus_box_cpp/src/media_browser/`; it's still in use on the kiosk's home menu UI. Future MB UI work should stick to the 7-color palette.
- **`mb_*_intensity` settings inherit from kiosk on first load.** The first time an upgraded Pi loads `config/settings.json` after this build, the seven `mb_<crt-effect>_intensity` keys are absent — they default to the corresponding kiosk values (so an operator who had scanlines at 0.5 on their home menu sees scanlines at 0.5 on the Marquee menus too on the first frame after upgrade). The `mb_playback_show_frame` key defaults to `true` for the same continuity reason. After the operator changes any value through MovieSettings, the divergence persists.
- **Performance recommendations.** The cold-start `pump_artwork` skip improved drop rate to zero in testing, but the Pi 4 still has limited thermal headroom (`vcgencmd get_throttled` showed `0xe0000` — historical throttle bits were set during this session). Active cooling (fan + heatsink) is recommended for sustained 1080p playback workloads.
- **OTA continuity.** The new `mb_*` keys are part of `config/settings.json` which falls under the `config/*` rsync exclude — operators OTA-upgrading from any prior 1.6.x build keep their existing settings, and the kiosk-value-inheritance fallback in load handles the upgrade case automatically. The new `EnvironmentFile=-/opt/magic_dingus_box/services/.env` line in the systemd unit propagates via the standard `systemd/**` rsync.

## [1.6.1] - 2026-04-29

Marquee design-fidelity pass. After v1.6.0 landed, three high-visibility components diverged from the design file: poster cells were plain colored tints with no title overlay, DetailScreen action buttons used a single gold border for all buttons regardless of intent (the design has color-coded Ok/Warn/Action variants), and the Marquee Settings screen header didn't match the rest of the Marquee chrome. This release closes those gaps.

### Added
- **`chrome::draw_button(label, kind, focused)`** ([`mb_chrome.{h,cpp}`](magic_dingus_box_cpp/src/media_browser/ui/mb_chrome.h)) — the design's `.btn` component. Bordered (2 px) rect on `theme.bg` with the label and border rendered in the semantic kind color: `Ok` = green (`highlight1`), `Warn` = red (`highlight2`), `Action` = steel blue (`action`), `Neutral` = dim. 18 px mono label, 18 px horizontal padding, 10 px vertical. Focused buttons get the standard 2 px gold focus ring at +2 px offset.
- **`chrome::draw_poster_card(...)`** — composed poster tile that draws as a designed object even before TMDB artwork loads: solid tint fill, top + bottom cream dash accents, ZenDots title overlay (crude word-wrap on whitespace for long titles), small year bottom-left, IN LIBRARY (green) or %-progress (red) badge inset top-left.
- **LibraryScreen sort sub-tabs** — visual-only `Recent / Title / Year / Size` strip right-aligned on the stats line. `Recent` renders active (cream `fg`); others dim. Sort cycling still deferred per operator direction; these are stub labels until the cycling input is wired.

### Changed
- **DetailScreen action button row** ([`detail_screen.cpp`](magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp)) now uses `chrome::draw_button` per-button with intent-mapped colors: Play / Add to Library = Ok (green), Search Releases / Search Again / Retry = Action (blue), Remove / Confirm Remove = Warn (red), More Info = Neutral. Buttons are pre-measured so the row centers horizontally with consistent 16 px gaps. The previous blinking ◂ focus marker was replaced with the standard 2 px gold focus ring for visual consistency with poster cells, settings rows, etc.
- **BrowseScreen + LibraryScreen poster cells** swapped from `mb_draw_poster_or_tint` / `mb_fill_rect` placeholders to `chrome::draw_poster_card`. Each cell now carries the full Marquee styling regardless of artwork load state.
- **Marquee Settings header + footer** ([`mb_settings_screen.cpp`](magic_dingus_box_cpp/src/media_browser/ui/mb_settings_screen.cpp)) — header replaced with `chrome::draw_screen_header("Settings", sub_info="Movies · Magic Dingus Box")` so it matches Browse / Library / Detail visually. Footer replaced with `chrome::draw_footer_hints({A=Change, BTN2=Refresh, BTN4=Back})`. The row body itself (label / value / cycling arrows / focused-row `bg_lift` fill) is NOT yet redesigned — see Deferred.

### Deferred (still diverging from design)
- **Settings row visual rewrite** — gold section headers with bottom rule, label-left/value-right rows with cycling arrow indicators (`‹ ›`), focused-row `bg_lift` fill + gold border. The current row rendering still works; it just doesn't match the design. Bigger rewrite touching the 12 `RowKind` cases.
- **DetailScreen body layout** — the design's poster-left + content-right grid with backdrop hero behind a `bg-80` overlay. Current rendering keeps the existing single-column structure.
- **SearchScreen keyboard styling** — bordered keys with consistent focus ring (currently uses the existing keyboard render).
- **QueueScreen VPN status indicator** — right-aligned `VPN: gluetun · NL · 4 active` text in the header.
- **PlaybackScreen HUD** — design's specific scrub-bar + time-triangle layout.

## [1.6.0] - 2026-04-29

**Marquee design system** — full visual redesign of the Media Browser. The kiosk's main playlist UI, settings, RetroArch flow, and all other surfaces are deliberately untouched per operator direction; the new visual language is contained to the Movies feature and gives it a distinct identity from the rest of the kiosk.

### Added
- **Wood-grain "TV cabinet" frame** ([`magic_dingus_box_cpp/assets/marquee/marquee_frame.png`](magic_dingus_box_cpp/assets/marquee/marquee_frame.png), [`renderer.cpp::render_marquee_frame`](magic_dingus_box_cpp/src/ui/renderer.cpp)). A 1280×720 RGBA overlay with transparent center and a polished mahogany frame painted on the outer ~40 px (mitered corners, beveled inner highlight). Drawn AFTER the Media Browser's content render and BEFORE toasts so the frame visually "covers" the outer pixels of the canvas, producing a clean inset content area without needing to rework the CRT shader's render region. Loaded once and re-uploaded after RetroArch's GL teardown via the existing `reset_gl()` cleanup path.
- **Shared `mb_chrome` chrome library** ([`media_browser/ui/mb_chrome.{h,cpp}`](magic_dingus_box_cpp/src/media_browser/ui/mb_chrome.h)) — design tokens (kFrameInset_px=40, kSafeInset_px=60, kHeaderHeight_px=60, spacing scale 4/8/16/24/40, kFocusBorder_px=2), a TabSpec/TabState model, and primitives every Marquee screen composes from: `draw_screen_header()` (title + N-tab strip), `draw_footer_hints()` (bordered-key + dim-label pattern), `draw_focus_ring()` (2 px gold, +2 px offset, no glow/scale), `draw_lib_badge()` and `draw_dl_badge()` (IN LIBRARY / progress chips on poster cells).
- **5-tab Marquee strip on Browse + Library**: Popular · Now Playing · Top Rated · Library · Search. Library and Search are transition-only; selecting them returns the corresponding `Screen` value to the dispatcher. BrowseScreen retains its `category_` across transitions so the user resumes wherever they were when stepping back from Library or Search.
- **LibraryScreen stats line**: `<N> titles · X.X GB used · Y.Y GB free` (Movie file_size_bytes summed; free space from `std::filesystem::space("/mnt/ssd/library")`).
- **`bg_lift` design token** (#2A232A) added to `theme.h`/`theme.cpp` — the Marquee design's only allowed off-bg fill, used for focused-row backgrounds and progress-bar troughs.

### Changed
- **Tab navigation grammar** ([`browse_screen.cpp`](magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp), [`library_screen.cpp`](magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp)). BTN1 (yellow, `InputAction::PREV`) and BTN3 (green, `InputAction::NEXT`) now navigate horizontally between the 5 Marquee tabs (previously unused on these screens). Movement stops at the strip ends with no wrap. The rotary encoder + D-pad LEFT/RIGHT (`InputAction::ROTATE`) walks posters one cell at a time row-major; D-pad UP/DOWN (`InputAction::ROTATE_VERTICAL`) walks one row at a time. SELECT (rotary click + gamepad A) opens detail. BTN2 (red, `PLAY_PAUSE`) preserved as the quick-add shortcut on BrowseScreen. BTN4 (black, `SETTINGS_MENU`) preserved as back/exit.
- **BrowseScreen** simplified from a 9-chip strip (Popular, Now Playing, Top Rated, Upcoming, Filter, Search, Library, Queue, Settings) to the 5-tab Marquee strip. The Filter and Upcoming code paths plus the `cycle_filter_value`/`ensure_genres_loaded`/`reload_filter_results`/`run_reload_filter_page` helpers remain in the file as dead code (cleanup deferred to a follow-up). 9-column poster grid with 2 visible rows; cell width derived dynamically from the safe area; per-cell IN LIBRARY badge and "Title · Year" meta line. File trimmed from 1232 → 832 lines.
- **LibraryScreen** rewired to render the same 5-tab strip with the Library tab marked active, plus the stats line and a 9-column owned-only grid. BTN1 returns to BrowseScreen (which resumes wherever it was); BTN3 transitions to SearchScreen. File trimmed from 679 → 489 lines. Sort cycling (Recent / Title / Year / Size) deferred per operator direction — the existing All / Unwatched / MissingUpgrades / Recent filter logic stays in place for re-enabling later.
- **Footer hints** on Detail, Search, and Queue screens now use the `mb_chrome::draw_footer_hints` bordered-key pattern. Function bodies, action button state machines (Detail's NotInLibrary/InLibraryNoFile/InLibraryWithFile modes + Confirm-Remove two-stage flow), virtual-keyboard nav grammar, and qBit cancel flow all preserved verbatim.

### Fixed (functional)
- **Movie viewport inset during Marquee playback** ([`main.cpp`](magic_dingus_box_cpp/src/main.cpp)). When `current_screen == MediaBrowser && current_mb_screen == Playback`, the gst_renderer viewport insets by 30 px on every edge so the wood-frame overlay doesn't crop important movie content. The 10 px gap between the frame's inner edge (40 px) and the video edge (30 px) deliberately produces a "screen behind wood" look rather than "movie cropped flush against wood". Other video-render paths (intro video, regular kiosk playlist video, RetroArch handoff) retain their fullscreen viewport — the inset is gated to Marquee playback only.

### Notes for operators
- **Existing kiosk surfaces unchanged.** The wood frame, new tokens, and 5-tab strip are gated to `AppScreen::MediaBrowser` only. Main playlist UI, settings menu, virtual keyboard, audio picker, RetroArch handoff, boot intro, and the web Content Manager all render exactly as they did in v1.5.4. The bezel selection feature (11 user-selectable PNGs in settings) still applies to those screens.
- **CRT shader behavior in Marquee.** The Marquee screens never engage the CRT shader pipeline (the existing `begin_scene_fbo()` `AppScreen` gate ensures this), so movie playback inside Marquee shows a clean unfiltered image. Operators who have CRT effects enabled in settings continue to see them on the main playlist UI.
- **Quick-add (BTN2 red) preserved on BrowseScreen.** The existing one-press add-focused-poster-to-library flow with HD-1080p quality profile selection and disk-space pre-flight is unchanged.
- **Prowlarr availability readout on DetailScreen** preserved unchanged. The seeders / file size / source group line still appears under the meta strip when the movie is not yet in library.

### Deferred (follow-ups)
- LibraryScreen sort tabs (Recent / Title / Year / Size) — visual sub-strip not yet wired; defaults to All.
- VPN status indicator in QueueScreen header (gluetun · NL · 4 active) — header text not yet wired.
- Marquee EntryTile inside the main playlist UI — operator direction is to leave the playlist UI untouched. Marquee remains accessible via the existing Settings menu route.
- Cleanup pass for dead code in BrowseScreen (Filter / Upcoming / cycle_filter_value / etc.) — left in place for now.

## [1.5.4] - 2026-04-29

Pre-1.6 hardening release. A multi-domain audit of the merged-in v1.5.x feature work surfaced 28 findings; 23 were addressed in the initial hardening commit + deploy-fix, and a follow-up sweep tackled 4 of the 5 originally-deferred items (only #5 — the `manager.js` inline-onclick → data-attr refactor — remains, since it's a multi-day frontend hygiene project best done alongside the upcoming Media Browser UI redesign).

### Why this release

Before pivoting to the next feature cycle (Media Browser UI redesign + new playlist UX), v1.5.x got a thorough hygiene pass: every confirmed bug and security exposure that surfaced during the audit landed in main, with full Pi runtime smoke-tests after each change. v1.5.4 is the "shipping-clean baseline" before new feature work begins. No new user-facing functionality; pure correctness, security, and infrastructure hardening.

### OTA upgrade path

Operators on v1.5.x will OTA forward to v1.5.4 directly via the standard `update.sh` flow. The fix to `update.sh`'s build-error handling (the `2>&2` → `2>&1` typo) is in this release, so v1.5.4 is the **last release where a failed OTA build silently produces a black-screen Pi** — operators currently running v1.5.0–v1.5.3 will get diagnostic output on any future OTA build failure once they're on v1.5.4. Recovery from a black-screen Pi mid-OTA still requires manual SSH on the source version; v1.5.4 prevents the next occurrence rather than fixing already-broken Pis.

The `setup_services.sh` chown fix is consequential for any operator provisioning Media Browser **on a fresh Pi for the first time** post-v1.5.4 — Docker container writes to `/mnt/ssd` actually work now. Operators with existing Media Browser stacks won't notice (their `/mnt/ssd` was set up before the bug bit; downloads have been working).


### Fixed — deferred-items follow-up sweep
- **GStreamer bus messages are now actually delivered** ([`magic_dingus_box_cpp/src/video/gst_player.cpp`](magic_dingus_box_cpp/src/video/gst_player.cpp)). The previous `gst_bus_add_watch()` registration sat on the default GLib main context, but the kiosk has no `g_main_loop_run` anywhere — the watch never fired and EOS / ERROR / STATE_CHANGED / DURATION_CHANGED messages were silently swallowed. Replaced with a `gst_bus_pop()` drain loop inside `update_state()`, called every render-thread tick. The existing `bus_call()` handler is unchanged; it's just dispatched from a polling site instead of GLib's watch system. Mid-playback decoder errors now log diagnostic context, EOS callbacks fire on the bus path (in addition to the existing position-polling fallback), and future features that rely on bus-message delivery can be wired up cleanly. Removed the now-unused `bus_watch_id_` member and its constructor/cleanup references.
- **RetroArch launcher: shell-safe-quote all C++→shell interpolations** ([`magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp`](magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp)). The launcher generates a bash script that runs RetroArch; eight call sites interpolated C++ values (paths, ALSA device, core name, controller-mapping name) directly into shell-evaluated contexts — `mkdir -p "..."` and various `echo '...'` statements outside any heredoc. Today's values are all programmatic identifiers with no realistic chance of containing problematic characters, but the pattern was a future-proofing landmine. Added a `shell_sq_escape()` helper in the anonymous namespace with a doc-comment explaining when to use it (and when not to — values inside `cat > ... << 'EOF' ... EOF` heredocs are already literal-safe and don't need escaping). The two `$(date)`-using echo statements were converted to `printf '%s ... %s\n' "$(date)" 'escaped-value'` form so the date still evaluates at run time but the interpolated value is fully isolated from shell expansion.
- **`controller.cpp` retry loop null guard** ([`magic_dingus_box_cpp/src/app/controller.cpp`](magic_dingus_box_cpp/src/app/controller.cpp)). The playlist-load retry loop calls `player_->stop()` unconditionally; `player_` is set at construction and never reassigned today, so the call is reachable-only-if-non-null in practice, but a future refactor that adds a real null-reset path would silently introduce a crash here. Added an `if (player_)` guard with an explanatory comment.
- **bats test coverage for `update.sh` rsync `--exclude` consistency** ([`tests/local/update_rsync_excludes.bats`](tests/local/update_rsync_excludes.bats)). Six new assertions enforce the contract between the four rsync blocks in `update.sh` (backup, install, internal rollback, user rollback): the rollback blocks must be identical to each other, every install exclude EXCEPT `build/*` must also be in both rollbacks (else rollback clobbers operator content), install must exclude `build/*` (so a freshly-built binary isn't partially overwritten), rollbacks must NOT exclude `build/*` (so the pre-update binary gets restored), and every backup exclude must also be an install exclude. Local-tier test count: 31 (was 25).

### Fixed — security
- **`GET /admin/playlists/<name>` path traversal** ([`magic_dingus_box/web/admin.py`](magic_dingus_box/web/admin.py)). The handler read `playlists_dir / name` directly, bypassing the `_sanitize_filename` + `resolve()` containment that POST and DELETE both use. A request to `../../config/settings.json` would have returned the kiosk's full settings JSON. Now applies the same containment as the other handlers.
- **`GET /static/<path:filename>` arbitrary file disclosure** ([`magic_dingus_box/web/admin.py`](magic_dingus_box/web/admin.py)). `send_file(static_dir / filename)` followed `..` segments out of the static directory. Switched to `send_from_directory`, which uses Flask's `safe_join` and 404s on escape attempts.
- **`update.sh` silently swallowed every OTA build error** ([`magic_dingus_box_cpp/scripts/update.sh`](magic_dingus_box_cpp/scripts/update.sh) lines 114, 118). `cmake .. > /dev/null 2>&2` and `make -j2 2>&2` redirected stderr **to stderr** (no-op) instead of `2>&1`. Failed builds left a black-screen Pi with zero diagnostic output — the same failure mode as the v1.5.1 silent-exit bug. Now `2>&1` so build errors surface in the journal and the OTA progress JSON.
- **`setup_services.sh` chowned `/mnt/ssd` as `root:root`** ([`magic_dingus_box_cpp/scripts/setup_services.sh`](magic_dingus_box_cpp/scripts/setup_services.sh)). The script runs via `sudo`, so `$(whoami)` was `root`. Docker containers (PUID/PGID = magic) then EACCES'd on every download write, breaking fresh-Pi Media Browser provisioning at the storage step. Now uses the already-resolved `${TARGET_USER}` (from `$SUDO_USER`).
- **qBittorrent admin credentials passed via curl `-d` argv** ([`magic_dingus_box/web/admin.py`](magic_dingus_box/web/admin.py)). `username=...&password=...` lived in `/proc/<pid>/cmdline` and was readable by any local process. Now passed via stdin (`-d @-`).
- **ZIP bomb mitigation only checked declared `file_size`** ([`magic_dingus_box/web/admin.py`](magic_dingus_box/web/admin.py)). The size came from the central directory, which is attacker-controlled. A crafted ZIP with `file_size = 0` in headers but gigabytes of compressed data passed the pre-check and filled the SD card. Now streams extraction with a hard byte counter and aborts mid-write if the cap would be exceeded, deleting the partial file.

### Fixed — runtime correctness (kiosk C++)
- **`gst_renderer` null-deref on `gst_structure_get_string("format")`** ([`magic_dingus_box_cpp/src/video/gst_renderer.cpp`](magic_dingus_box_cpp/src/video/gst_renderer.cpp)). Caps renegotiation mid-stream (and some passthrough elements) can produce caps without a `format` field; the prior code segfaulted in `strcmp` and spdlog formatting on the resulting nullptr. Skips the frame instead.
- **`WifiManager` async-claim race** ([`magic_dingus_box_cpp/src/utils/wifi_manager.cpp`](magic_dingus_box_cpp/src/utils/wifi_manager.cpp)). `scan_networks_async` and `connect_async` did `if (is_scanning_) return; is_scanning_ = true;` — non-atomic, so two rapid menu opens could double-spawn detached worker threads. Replaced with `compare_exchange_strong`.
- **`AppState::apply_output()` shell-injection landmine** ([`magic_dingus_box_cpp/src/app/app_state.h`](magic_dingus_box_cpp/src/app/app_state.h)). The `pactl move-sink-input` step interpolated `sink_name` into a `/bin/sh -c` pipeline string. Currently safe (sink_name is a hardcoded constant), but a future caller deriving it from input would have had a clean shell-injection vector. Replaced with a `pipe(2)`-captured `pactl list short sink-inputs` followed by per-ID `execlp pactl move-sink-input` — no shell anywhere.

### Fixed — systemd / boot reliability
- **Kiosk service no longer auto-restarts after segfault** ([`magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service`](magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service)). The unit had `WatchdogSec=10` + `Restart=on-watchdog` but the explicit `Restart=on-failure` was commented out, so a clean segfault exit (which doesn't trigger the watchdog) left a permanently black screen until manual SSH. Added `Restart=on-failure` (`RestartSec=5`); RetroArch integration unaffected because the kiosk `waitpid()`s on RetroArch and returns via clean exit, not failure.
- **`StartLimitIntervalSec`/`StartLimitBurst` placement warning gone** — moved from `[Service]` (where systemd silently ignored them and logged "Unknown key" on every daemon-reload) to `[Unit]` where they belong.
- **`init_audio.sh` hardcoded UID 1000** ([`magic_dingus_box_cpp/scripts/init_audio.sh`](magic_dingus_box_cpp/scripts/init_audio.sh)). Pi OS conventionally puts the magic user at UID 1000 on a fresh install but a non-default ordering can land it at 1001+, in which case audio failed silently because `XDG_RUNTIME_DIR` pointed at a directory the user couldn't access. Now resolved via `id -u magic` once at script start and used everywhere.
- **`first_boot.sh` Step 6c (WiFi wipe) and 6d (cloning_backup wipe) lifted out of the `python3` guard** ([`scripts/golden_image/first_boot.sh`](scripts/golden_image/first_boot.sh)). They were nested inside `if [[ -f $SETTINGS_PATH ]] && command -v python3`, so a Pi missing python3 (rare) or with no `settings.json` (more common — happens before the kiosk has run once) silently skipped both the WiFi-credential wipe and the cloning-backup marker cleanup. The result was cloned Pis inheriting the source operator's home WiFi password and a stale `in_progress` marker that blocked future re-cloning. Both steps are now top-level, unconditional.

### Fixed — deployment & infra
- **`deploy_cpp.sh` Step 1.7 restart guard** ([`magic_dingus_box_cpp/scripts/deploy_cpp.sh`](magic_dingus_box_cpp/scripts/deploy_cpp.sh)). The kiosk service was unconditionally restarted at install time, but if no kiosk binary was on the Pi yet (first deploy without `--build`), the unit ended in `failed` state. The next deploy then had to run `systemctl reset-failed` first, hiding any real failure. Now gated on `[ -x ".../build/magic_dingus_box_cpp" ]`.
- **`deploy_cpp.sh` Step 1.58: always sync the Media Browser compose file** (commit `e9384b7`). Previously folded into Step 1.7c, gated behind `--media-browser`, so routine `--build` deploys never updated `services/docker-compose.yml`. The Pi silently drifted: containers kept running via Docker's `restart: unless-stopped`, but the `magic-dingus-services.service` unit failed every boot with "no configuration file provided" and any `docker compose down` had no recovery path. Now always synced (with `--exclude '.env'` to preserve operator credentials, no `--delete` to preserve `config/`).
- **`setup_usb_gadget.sh` modules-load handling** ([`magic_dingus_box_cpp/scripts/setup_usb_gadget.sh`](magic_dingus_box_cpp/scripts/setup_usb_gadget.sh)). The exact-match grep `"modules-load=dwc2,g_ether"` returned false if any *other* `modules-load=` was already present (partial prior runs, conflicting setup steps). The append then created a second `modules-load=` token, which the kernel only honors one of — meaning `g_ether` could silently fail to load. Now handles three cases: exact match (no-op), partial match (merge into existing parameter), no `modules-load=` at all (insert).
- **`prepare_golden_image.sh` verification block referenced undefined `$VIDEO_PLAYLISTS`** ([`scripts/golden_image/prepare_golden_image.sh`](scripts/golden_image/prepare_golden_image.sh) line 577). The real array is `LEGACY_PLAYLISTS`; the typo expanded to empty so the loop never executed and the check always reported PASS even when legacy playlists were still on disk. Renamed.
- **`docker-compose.yml` flaresolverr pinned** ([`magic_dingus_box_cpp/services/docker-compose.yml`](magic_dingus_box_cpp/services/docker-compose.yml)). Was `:latest` despite the file's "all images pinned for reproducibility" header invariant. A silent `docker compose pull` upgrade could break Prowlarr's Cloudflare-bypass integration. Now pinned to `v3.4.6`.

### Fixed — Python web admin
- **Long-running job dicts grow unboundedly** ([`magic_dingus_box/web/admin.py`](magic_dingus_box/web/admin.py)). `transcode_jobs`, `update_jobs`, and `media_browser_jobs` were module-level dicts with no eviction; on a kiosk uptime measured in months they accumulated indefinitely. Added a shared `_prune_terminal_jobs` helper that evicts terminal-state entries older than 1 hour, called at the top of each job-creating route.

### Fixed — build / docs
- **`CMakeLists.txt`: `option(ENABLE_MEDIA_BROWSER)` declared after first use** ([`magic_dingus_box_cpp/CMakeLists.txt`](magic_dingus_box_cpp/CMakeLists.txt)). Three `if(ENABLE_MEDIA_BROWSER)` source-list blocks fired before the option declaration, so a fresh-cache `cmake` silently dropped `sequence_detector.cpp` and `toast.cpp` from the build even when the flag was set on the command line. Masked by the CMake cache surviving across re-runs. Moved `option()` to the top of the file.
- **`-Wno-dangling-reference` for spdlog** — GCC 13+ flags 70+ false-positive dangling-reference warnings inside spdlog/fmt's internal `arg_mapper` template instantiations. The noise drowned out real warnings on every build. Suppressed at the kiosk-target level for GCC 13+.
- **CLAUDE.md drift** — `source_type` playlist values now reflect the actual loader (`local` / `video` / `youtube` / `emulated_game`, not just `video`). The RetroArch section now mentions `controller_detector` and the `get_mapping_n64_adapter` / `get_mapping_ps_style` split (introduced in v1.4.0 but never reflected in the doc).
- **`AppState::AudioOutput` enum comments** updated from the old `amixer numid=3` references (Pi-specific ALSA control, not the path the codebase actually takes) to the actual PulseAudio sink names.

## [1.5.3] - 2026-04-28

Reverts the `first_boot.sh` Step 6e wipe that v1.5.2 introduced, after operator clarification that the **golden image is meant to ship fully-loaded**, not as a fresh-defaults starter image.

### Reverted
- **`first_boot.sh` no longer wipes `data/saves/`, `data/states/`, or `data/media/` on cloned Pis.** The Magic Dingus Box golden-image workflow ships a fully-curated showcase experience — the source operator's RetroArch saves, save states, and uploaded videos are part of the product the cloned Pi is meant to deliver. Every flashed Pi should boot into the same production-ready state the source Pi has, including:
  - 4 curated video playlists (Sacred_Steel, Chill_Guitar_for_Good_Vibes, Obscure_Guitar_for_Ruining_Parties, The_Nostalgia_Channel)
  - 31 .mp4 source videos (1.9 GB) referenced by those playlists
  - 11 RetroArch SRAM saves across NES / SNES / PS1 / PCEngine
  - All 161 ROMs, 150 thumbnails, 7 cores, all bezels, intro video
- **Per-Pi state still wiped** by `first_boot.sh` (correct behavior — these MUST differ between physical units): `services/.env`, `services/config/*`, `media_browser_unlocked` flag, WiFi credentials, cloning-backup leftovers, device UUID + hostname.
- If you need a "fresh defaults" image without operator content (e.g., for redistribution to other product lines), use the existing `prepare_golden_image.sh` script on the source Pi — that script intentionally wipes operator content with a clear destructive-action prompt.

### Why this release
v1.5.2 went the wrong direction on a product-philosophy decision: I assumed cloned Pis should arrive "clean" and the operator's library should not propagate. The operator clarified that the curated content (videos, custom playlists, gameplay saves) IS the product — the golden image is a showcase distribution mechanism, not a starter template. v1.5.3 restores that behavior. v1.5.2's release notes have been updated to mark it superseded.

### OTA upgrade path
Operators on v1.5.2 should OTA forward to v1.5.3 immediately if they intend to clone their Pi as a golden image. Operators on v1.5.0 or v1.5.1 will OTA directly to v1.5.3 (skipping v1.5.2) — the GitHub `/releases/latest` API returns v1.5.3 now. v1.5.0 OTA bug from `update.sh get_binary_url` is still present on Pis at v1.5.0; see v1.5.1 release notes for the manual recovery procedure.

## [1.5.2] - 2026-04-28

Cloning hygiene patch. Extends `first_boot.sh` to wipe three more categories of operator-specific content from the cloned image so a fresh-flashed Pi doesn't inherit the source operator's gameplay state, save states, or uploaded videos.

### Changed
- **`first_boot.sh` Step 6e: wipe inherited operator-content directories on the cloned Pi.** Three new wipes, all on the cloned Pi only (the source Pi keeps its content):
  - `data/saves/<core>/*.srm` — RetroArch SRAM saves. Source operator's Zelda character / Mario progress / etc. won't carry over to the next operator's Pi.
  - `data/states/<core>/*.state.auto` — Auto-resume save states. Same fix.
  - `data/media/*` — Operator-uploaded videos. Often gigabytes of operator-curated content (Content Manager → Upload flow) that has no business on a stranger's Pi and inflates every flashed image (mp4 compresses poorly through gzip).
- The directories themselves are kept (only their contents are wiped), so RetroArch and the kiosk's media-upload path don't have to recreate them on first use.
- All three paths remain in update.sh's OTA preservation list — they're still untouched by future OTAs on real operator Pis. `first_boot.sh` is the only stage that can distinguish "fresh clone state" from "operator's working Pi", which is the right place for this cleanup.

### Why this release
v1.5.1 closed the silent-OTA-exit bug. With OTA reliable, the next workflow gap is "I want to clone my working Pi as a starter image for new operators, but I don't want them to receive my saves/uploaded videos." Pre-v1.5.2, the workflow required manual stash-and-restore around `clone_live_sd.sh`. Post-v1.5.2, the source Pi keeps everything; flashed Pis automatically arrive clean.

## [1.5.1] - 2026-04-28

OTA-path patch release. Fixes two bugs in `update.sh`'s `get_binary_url()` that caused **a silent mid-install exit on every v1.5.0 → next-version OTA**, leaving the kiosk in a non-running state. Discovered while testing the v1.4.0 → v1.5.0 OTA against a Pi with the full Media Browser stack — the rsync portion (and the operator-content preservation that the v1.4.3 release added) worked perfectly, but the script silently terminated before the rebuild + service-restart steps. **Anyone updating away from v1.5.0 should upgrade through this version.**

### Fixed
- **`update.sh install` no longer exits silently after the rsync stage.** Two compounding bugs in `get_binary_url()`:
  - **Double-`v` URL.** Callers may pass either `v1.5.0` or `1.5.0`; the function unconditionally prepended `v` via `v${version}`, producing `/releases/tags/vv1.5.0` → 404 from GitHub.
  - **`grep` no-match killed the script.** The `grep | head | sed` pipeline that extracts a binary asset URL exits with status 1 when `grep` finds zero matches (which is the *common* case — most v1.x.y releases ship source tarballs only, no pre-compiled binary asset). Under the script-level `set -euo pipefail`, that nonzero pipeline propagated and terminated the entire install. VERSION had already been rsync'd to the new value, but the rebuild + service-restart steps never ran. Operator-visible symptom: kiosk goes black after the update progress bar reached "checking_binary 35%" and never comes back; no error reported to the web admin's update job. Same family of bug as the v1.4.3 SIGPIPE fix in `check_update()` — the v1.4.3 fix didn't cover this second function.
  - Fix: strip a single leading `v` at the function entry, and run the parse pipeline in a subshell with `set +e +o pipefail` so a no-match grep no longer kills the parent script. Subshell pattern (rather than `set +e` directly inside the function) is deliberate — it scopes the flag relaxation to `get_binary_url` only, so the install path's strict mode remains in force for the rest of the script and a rollback still triggers cleanly on any real install failure that occurs later.

### Recovery for Pis stuck mid-OTA on v1.4.0 → v1.5.0

If you ran v1.4.0/v1.4.1/v1.4.2 → v1.5.0 OTA before v1.5.1 was published and your kiosk is sitting at v1.5.0 VERSION but inactive, SSH in and finish what update.sh started:

```bash
cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake .. && make -j2 && \
  sudo systemctl daemon-reload && \
  sudo systemctl restart magic-dingus-box-cpp.service
```

Operator content (`services/.env`, `services/config/*`, ROMs, saves, settings) is intact — the rsync's exclude lists worked correctly; only the post-rsync rebuild step was missed.

## [1.5.0] - 2026-04-28

Visual quality release: a complete rewrite of the CRT effects shader pipeline with five compounding upgrades, fully opt-in via a runtime "CRT Engine: Classic | Enhanced" toggle in Settings → Display. The Classic engine is bit-for-bit identical to v1.4.3, so an OTA from any earlier version produces zero visible change until the operator flips the toggle. Every effect was upgraded; the existing 7 sliders (Scanlines, Color Warmth, Phosphor Glow, RGB Mask, Screen Bloom, Interlacing, Flicker) now drive physically-motivated CRT physics rather than alpha-blended overlays.

### Added
- **Render-to-FBO substrate.** When the Enhanced engine is active, kiosk video + UI are routed into an offscreen RGBA8 FBO at HDMI mode size; a final composite shader reads that scene texture and produces final pixels. This is the precondition for every effect that reacts to scene content (which the v1.4.3 procedural overlay couldn't do at all).
- **Lottes-style aperture-grille subpixel mask.** Replaces the v1.4.3 column-darkening with proper per-channel R/G/B subpixel attenuation (Trinitron stripes). A red object now actually paints the R subpixels along its column.
- **Brightness-modulated Gaussian scanlines.** Beam width grows with luma (`σ = 0.30 + 0.15·L³`), so dark areas show crisp black gaps and bright areas show soft gaps where lines fade — the highlight-bloom-into-adjacent-lines character of a real CRT. Replaces the static sine-wave pattern.
- **Color Warmth as gamma + temperature shift.** The slider now drives a gamma boost (2.2 → 2.4 at full intensity) plus a per-channel D65 → ~5000K temperature shift `(1.00, 0.92, 0.82)`. Punchy, deeper blacks; warm without dyeing the screen orange.
- **RGB convergence error.** The Phosphor Glow slider now also drives quadratic-radial RGB beam misalignment — R drifts outward, B drifts inward, with offset growing toward the corners. Visible color fringing at the edges of the screen, just like an aged consumer CRT.
- **Luma-driven halation/bloom.** The Screen Bloom slider now produces a real two-pass dual-Kawase downsample chain (1/2-res → 1/4-res, with luma threshold ~0.7 on the first pass) screen-blended back into the composite. White text on dark backgrounds glows; the previous "fake center hotspot" white glow that ignored content is gone.
- **`CRT Engine: Classic | Enhanced` toggle** in Settings → Display. Live A/B comparison without touching slider values; persisted to `config/settings.json` as `display.enhanced_crt_enabled` and preserved across OTAs (in update.sh's `config/*` exclude list).

### Performance
- 60-second sustained-load measurement on Pi 4B with all 7 effects at MAX intensity + Enhanced engine + video playback running: V3D GPU stayed pegged at full 600 MHz throughout, SoC temperature ranged 62.3–64.2 °C (no rise across the run), `vcgencmd get_throttled` reported `0x0` at every 10-second sample point. Frame budget never approached 16.6 ms / frame at 1080p. Composite cost (FBO+shader) is estimated < 1 ms for Phases 1-4 active and ~1.5 ms additional when Phase 5 halation kicks in.
- All bloom/scene FBOs are lazily allocated only when their gating slider is non-zero — slider-OFF stays at the same cost as the legacy path.
- Cleanup paths (`reset_gl()` for RetroArch handoff, plus `cleanup()`) handle every new resource correctly. RetroArch launch/return cycles tested.

### Fixed (caught during the rewrite)
- **`gst_renderer::render_quad()` was hard-binding `GL_FRAMEBUFFER, 0` mid-render** ("Ensure we are drawing to the default framebuffer"), which would have broken the Enhanced engine by clobbering the scene FBO and sending video to the default framebuffer where the composite would then overwrite it with a black FBO. Removed the explicit bind with a long-form comment so the bug can't be silently reintroduced. Caller now owns target-framebuffer selection.

### Tuned
- **Effect cycle now correctly reads Off → Low → Medium → High.** Previously the cycle values (`0.15`/`0.30`/`0.50`) and the label thresholds (`≤0.35` Low / `≤0.6` Medium / `>0.6` High) disagreed, so a fresh cycle showed `Off → Low (15%) → Low (30%) → Medium (50%)` and the operator never saw a "High" label. New cycle values are `0` / `0.25` / `0.50` / `0.75` (clean off / quarter / half / three-quarters); thresholds placed at midpoints (`0.05` / `0.375` / `0.625`) so cycle output always lands in its correct tier. Migration is gentle — operators on legacy `0.15`/`0.30`/`0.50` values read as the right tier today and step up to clean values on next click.
- **Effect sublabels refreshed** to describe what the Enhanced engine actually does:
  - Phosphor Glow: "Radial glow" → "Vignette + RGB convergence"
  - Screen Bloom: "Bright glow" → "Phosphor halation"
  - Color Warmth: "Temperature" → "Warm tone + gamma"
  - Scanlines: "CRT lines" → "Horizontal CRT lines"
  - RGB Mask: "RGB stripes" → "Subpixel R/G/B stripes"
  - Interlacing: "Video lines" → "Alternate-line darken"
  - Flicker: "Subtle pulse" → "Brightness wobble"
  - CRT Engine: "v1.4.3 vs new pipeline" → "Classic / Enhanced"
- Internal `CYCLE_PHOSPHOR_MASK` enum renamed to `CYCLE_RGB_MASK` to match the operator-visible "RGB Mask" label (the original name predated the v1.4.0 rename).

### Reversible
A `v1.4.3-pre-crt-rework` git tag marks the pre-rework state. If the new shader is undesired:
- Operator-level: flip "CRT Engine" back to Classic in Settings → Display.
- Source-level: `git checkout v1.4.3-pre-crt-rework -- magic_dingus_box_cpp/src/ui/renderer.{h,cpp}` and rebuild.

## [1.4.3] - 2026-04-28

OTA-path patch release. Caught while verifying the v1.4.2 update flow end-to-end. **Anyone updating from v1.0.x → v1.4.3 should upgrade through this version, NOT v1.4.2 directly** — v1.4.2's update.sh would wipe operator content that v1.4.3 properly preserves.

### Fixed
- **OTA update preserves operator content (`services/.env`, `services/config/*`, `data/thumbnails/*`).** The rsync `--exclude` lists in update.sh's backup, install, and rollback paths missed three paths that contain operator-specific state. Updating from v1.4.2 (or earlier) would silently wipe:
  - `services/.env` — per-Pi WireGuard private key, ProtonVPN credentials, auto-generated Radarr/Prowlarr API keys, qBit admin password. Operator's Media Browser would die and they'd have to redo the entire WG-config drop + setup_services.sh flow.
  - `services/config/{radarr,prowlarr,qbittorrent,gluetun,flaresolverr}/*` — per-Pi stack runtime state (Radarr library DB, Prowlarr indexer sync history, qBit fastresume + cookies, Gluetun VPN runtime). Operator would lose every movie they've added.
  - `data/thumbnails/*` — game cover art populated externally by `deploy_cpp.sh` from the operator's local thumbnails folder (gitignored, so the GitHub release tarball doesn't contain them). Operator would lose all 157 game cover images.
- **`update.sh check` was silently failing.** Some shell + GitHub-payload combinations triggered a `SIGPIPE`-from-`head`-killing-upstream-`grep` cascade inside `check_update()` that the script's `set -euo pipefail` propagated as a fatal error, killing the function before its `cat << EOF` JSON output. The web admin's "Check for Updates" button received an unparseable error response. Relaxed `set -e/pipefail` inside `check_update()` only; the install path's strict mode stays in force so a mid-extract failure still triggers clean rollback.
- **GitHub Releases for v1.4.0 and v1.4.1 were missing.** Both versions were tagged in git but never published as GitHub *Releases*, which is what `update.sh` actually queries (`/releases/latest`). An out-of-date Pi running v1.0.x to v1.3.0 would see "v1.3.0 is latest" (the last published Release) and never offer an update. v1.4.2 published a Release for the first time since v1.3.0; v1.4.3 continues that going forward.

## [1.4.2] - 2026-04-28

Patch release with two CRT-TV display fixes caught during physical CRT-TV verification on the v1.4.1 Pi. Both bugs share the same root class — code that assumed the HDMI mode dims (1280×720) match the renderer's logical content viewport, when in CRT_NATIVE the kiosk uses a 640×480 logical space inside that 1280×720 framebuffer.

### Fixed
- **Pillarboxed video on CRT TV.** The aspect-ratio-preserving math in `GstRenderer::render_quad()` (added in v1.4.0 for Modern TV's Media Browser playback so 2.35:1 movies don't stretch vertically) was running in CRT_NATIVE mode too — its only gate was `letterbox_mode_=false`. Result: 4:3 transcoded videos got 160px pillar bars added on the Pi side, then the CRT TV's built-in HDMI→4:3 converter added ANOTHER round of bars at the receiving end, producing a window-boxed image with margins on all four sides. Added `GstRenderer::set_aspect_preserve(bool)` (defaults true so Modern TV behavior is unchanged); `main.cpp` flips it false in CRT_NATIVE so `render_quad` falls through to its original fill-the-framebuffer behavior. CRT TV now receives a fully-filled 1280×720 signal and its downstream converter handles aspect correction on the receiving end.
- **Toast notifications rendered off-screen in CRT mode.** `Toast::render` was being called with `mode.width/height` (the raw HDMI mode dims, 1280×720), but the renderer's projection matrix is set up for the *content viewport* dims (640×480 in CRT, 960×720 in Modern TV when letterbox is active). The "Wi-Fi connected" panel computed its position as `(1280-480)/2 = 400` logical, then that 400 was interpreted in 640×480 space → projected to 62% from the left, ending up in the lower-right corner with the right side clipped off-screen. Added `Renderer::get_width()/get_height()` returning the active content-viewport dims; main.cpp passes those to `Toast::render` instead. Side benefit: also fixes a previously-unreported ~6% horizontal off-center in Modern TV's main-playlist letterboxed mode (Renderer at 960×720, caller was passing 1280×720).

### Tooling
- `clone_live_sd.sh` gained a `--yes / -y` flag for non-interactive runs. The `Continue with live clone? [y/N]` prompt's `read -r` doesn't reliably accept piped stdin in all environments, so harness-driven invocations would silently abort. The DRY_RUN path already had a parallel skip; this just adds a flag-driven bypass for the same pattern. Used during the v1.4.0/v1.4.1 cloning runs.

## [1.4.1] - 2026-04-27

Patch release with two clone-quality fixes caught during the v1.4.0 image's first flash-test on a fresh Pi (`magicpi-9768`). No source-code refactors, no new features — just two papercuts that would have shipped to every operator otherwise.

### Fixed
- **Cloned Pis without a wired faceplate boot to a dead kiosk** (`a4f3430`): the standby watcher's `reconcile_initial_state()` read GPIO 3 as HIGH and treated it as "switch in OFF position — stop services," killing the kiosk service ~1 second after it started. The HIGH read is ambiguous though — it can mean either "switch wired and OFF" OR "no switch wired at all and the line is floating via the kernel pull-up." The latter applies to every fresh clone before faceplate assembly, AND to anyone who flashes the image to a bare Pi 4 with no switch. Watcher now only acts on actual GPIO transitions during runtime; HIGH at boot is logged but ignored. LOW at boot still calls `start_services` (unambiguous — line is actively pulled to ground). Trade-off: an operator who power-cycles their box with the switch already in OFF position will see the kiosk briefly come up before they re-flip the switch off, but that's strictly better than "boot to a black screen with no recovery path."
- **`first_boot.sh` now updates `/etc/hosts` to match the regenerated hostname** (`9c3cb9c`): cloud-init's `manage_etc_hosts=True` writes a `127.0.1.1 magicpi magicpi` line that systemd's NSS-files resolver uses for self-lookup. `first_boot.sh` Step 3 was already calling `hostnamectl set-hostname magicpi-XXXX` to give cloned Pis unique hostnames, but it wasn't touching `/etc/hosts`. The mismatch made every `sudo` (and many hostname-self-lookup code paths) emit `sudo: unable to resolve host magicpi-XXXX: Temporary failure in name resolution` and pay a small DNS-timeout delay. Cosmetic in isolation but accumulated across boot scripts and operator commands. Now sed-replaces the `127.0.1.1` line to match the new hostname; falls back to appending if no entry exists.

Both fixes verified live on `magicpi-9768` during the v1.4.0 image's flash test before re-cloning to v1.4.1.

## [1.4.0] - 2026-04-27

The "ready for the golden image" release. Closes out a long testing pass with end-to-end verification of every kiosk surface (controllers, all 7 emulator cores, bezel cycling, web admin, Media Browser pipeline) plus a stack of fixes uncovered along the way. Tagged at `1.4.0` because it ships several user-facing features (Media Browser auto-pause-on-playback, audio normalization in upload, 2-player support, tighter Library UI) on top of the bezel/PS-pad/overclock work that was already pending in the prior `[Unreleased]` block.

### Added
- **Three custom MDB bezels** (MDB-1974 wood-grain console, MDB-1986 Memphis neon, MDB-KV19 sleek black broadcast monitor) shipped as the new defaults in [bezels.json](magic_dingus_box_cpp/assets/bezels/bezels.json) with matching RetroArch overlay `.cfg` sidecars.
- **RetroArch bezel overlay in Modern TV mode**: launching a game in `MODERN_TV` mode now writes a 1920×1080 RetroArch config with a 4:3 `custom_viewport` at `(251, 10, 1415, 1059)` (the geometric intersection of all bezel families' transparent cutouts) and `input_overlay` pointing at the user's selected bezel `.cfg`. The bezel frames the game so a modern 16:9 TV presents retro titles inside their period-appropriate "screen". CRT_NATIVE mode launches at 640×480 with no overlay (byte-identical to pre-feature behavior).
- **Auto-detected dual controller mappings**: kiosk now scans `/dev/input/js*` and reads VID/PID from sysfs at game-launch time, classifying as `N64_ADAPTER` (`0e6d:111d`), `PS_STYLE_DRAGONRISE` (`0079:0006`), or `UNKNOWN`. Dispatches to per-core mapping tables tailored to each controller. PS-style pad gets PS1 1:1 mapping, classic SNES face button positions, Genesis A/B/C, FBNeo 6-button fighter layout, etc. Unknown / no-controller falls back to N64 mapping.
- **Kiosk UI input support for PS-style USB pads**: button codes 288–299 now recognized alongside N64 codes. Detects 8-bit-axis controllers (`abs_min == 0`, `abs_max ≤ 255`) and treats `ABS_X/Y` extremes as digital D-pad presses. `ABS_Z`/`ABS_RZ` (right stick on these pads when in analog mode) explicitly ignored.
- **Tier 1 performance overclock**: CPU 1.8 → 2.0 GHz, V3D GPU 500 → 600 MHz, GPU memory split 76 → 256 MB, force_turbo, performance governor pinned via new `magic-cpu-performance.service` (one-shot at boot). Net ~+15% headroom for heavier cores like PS1 at 1080p output. CPU/GPU clocks set in `/boot/firmware/config.txt` (captured by golden image).
- **Two-player support across all 7 cores**: launcher now emits `input_player2_*` button mappings alongside player 1, plus `pcsx_rearmed_pad2type` so the PS1 BIOS recognizes the second pad. Verified live with Twisted Metal 4 split-screen Battle. Hotkeys (RA menu toggle / exit combo) intentionally stay player-1-only so both controllers don't fight over them.
- **Auto-pause torrents during movie playback**: PlaybackScreen now calls qBittorrent's `/torrents/stop` (qBit 5.x; with `/pause` fallback for 4.x) when a movie starts, and `/start`/`/resume` when the user exits. Eliminates random-IO contention on USB-flash media that was making rotary scrubbing freeze. Only resumes torrents the kiosk paused — never re-starts ones the operator manually stopped before entering playback.
- **Smart movie release scoring with seeder filter**: indexer-level `minimumSeeders=5` filter rejects dead-swarm releases at search time. Codified in `setup_services.sh` Step 14b so cloned Pis inherit the threshold via the standard re-provisioning flow. Net effect: Radarr never picks a release that won't actually progress, even if its tracker reports cached "9 seeders exist" but no peer is actually online.
- **Audio normalization in upload pipeline (Retro Ripper match)**: video upload UI's "Normalize audio volume" checkbox now actually fires the FFmpeg `loudnorm` filter — both `/admin/upload-and-transcode` and `/admin/smart-upload` previously dropped the form field on the floor. Loudnorm parameters changed from broadcast quiet (`I=-23:LRA=7:tp=-2`) to match the Retro Ripper companion (`I=-16:TP=-1:LRA=11`) so curator-supplied content and self-uploaded phone clips on the same playlist sound consistent. Default on. Smart-upload's "direct copy" shortcut is suppressed when normalize is requested so the loudnorm pass actually runs.
- **Port-80 redirect to Content Manager**: tiny Python HTTP service on port 80 issues 302 redirects to `:5000`, letting operators type `magicpi.local` (or any IP the Pi is reachable at) without remembering the port number. Runs as `content-manager-redirect.service` with `CAP_NET_BIND_SERVICE`. Preserves path + query + fragment.
- **Zero-config DHCP for plugged-in Mac/PC via dnsmasq**: when an operator plugs their laptop into the Pi's USB-C, the Pi now acts as a DHCP server on `usb0` and auto-assigns `10.55.0.10+` to the host. No manual interface configuration. Config in `scripts/data/dnsmasq-usb0.conf`, scoped to usb0 only (port=0 disables the DNS half so it doesn't fight systemd-resolved on wlan0).
- **Kiosk standby switch via GPIO 3**: `kiosk-standby-watcher.service` runs `gpiomon` on GPIO 3 and stops kiosk + Content Manager + Docker stack on rising edge (switch OFF), starts them on falling edge (switch ON). Replaces the previous full-poweroff behavior — Pi stays running so it boots back into the kiosk in seconds when the operator flips the switch back on.
- **Wi-Fi connection feedback Toasts + "Connecting..." status**: the Wi-Fi sub-menu now shows "Connecting to <SSID>..." the moment the operator presses Enter on the password prompt, with a Toast on success/failure once `nmcli` finishes. Previously the sub-menu sat silent for 5–10 sec and then suddenly flipped to "Connected" with no in-progress feedback.
- **Idempotent setting-menu re-entry**: `enter_submenu()` preserves transient state (selected_index, scroll_offset, confirm-dialog flags) when called with the same section it's already in, instead of resetting them. Fixes a regression where pressing Disconnect → Confirm was a one-frame flicker because the SELECT handler re-ran `enter_submenu(WIFI)` and clobbered the just-flipped confirm flag.
- **Auto-resume save state for cloned Pis** (cloned-image only): added `libretro_info_path` to the generated RetroArch config so `core_info_list` populates correctly. Without it, RA's `command_event_save_auto_state()` early-returned at the savestate-support check (reading 0 from a zero-initialized `core_info_t`) and `.state.auto` files never got written despite `savestate_auto_save = "true"` being honored. SRAM in-game saves were unaffected because they go through a different code path.
- **Defensive WiFi credential wipe on cloned Pis**: `first_boot.sh` Step 6c removes `/etc/NetworkManager/system-connections/*.nmconnection` so cloned Pis don't auto-attempt to join the source operator's home network. New operator joins via the kiosk's Wi-Fi setup UI in ~30 seconds.

### Changed
- **Library screen matches Browse screen**: 9-column grid (was 5), focus-only border (was every-poster gold border), left-aligned titles (was centered with marker-zone reservation), no blinking ◂ cursor (the focused border + accent-color title carry that signal). Corner status dot (green = good file, gold = upgrade available, red = missing) preserved. Net effect: Library and Browse feel like the same UI; visual clutter dropped, focus indicator is unmissable.
- **Modern TV mode shows more rows**: main playlist list (8 → up to 14), settings menu items (7 → up to 10), game-browser playlist (8 → up to 11). Each call site derives `max_visible` from `height_ + item_height` and floors at the original CRT-tuned value so CRT layout is byte-identical. Renderer publishes the on-screen row count back to the input handler so scrolling only kicks in when the selection actually goes off-screen — previously you'd press DOWN onto "Back" and the top row would disappear despite obviously fitting.
- **Audio loudnorm parameters match Retro Ripper**: `I=-23:LRA=7:tp=-2` → `I=-16:TP=-1:LRA=11`. See "Audio normalization in upload pipeline" in Added for full rationale.
- **PS-pad button mapping (operator preference)**: Cross now SELECT, Circle now MENU. Square is unassigned. Reflects the user-facing remap landed during Phase 2 testing — locked in before Phase 2 sign-off.
- **Per-Pi reset on cloned-Pi first boot now also wipes** `services/.env`, `services/config/{radarr,prowlarr,qbittorrent,gluetun,flaresolverr}/*`, the `media_browser_unlocked` flag in `settings.json`, and (now) WiFi credentials. Operator restores fresh per-Pi state via the Content Manager UI on the cloned Pi (drops in a new WireGuard config; `setup_services.sh` rebuilds everything else from the codified fixtures in `scripts/data/`).
- **`deploy_cpp.sh` now syncs the top-level `scripts/golden_image/` directory**. Previously these scripts (`first_boot.sh`, `prepare_for_cloning.sh`, `restore_after_cloning.sh`, `prepare_golden_image.sh`) lived outside `magic_dingus_box_cpp/` and silently drifted between repo and Pi over time. Caught during the pre-clone audit when the Pi turned out to have an outdated `first_boot.sh` that would have leaked the source's VPN credentials and Media Browser unlock state to every clone.
- **Settings menu MENU button = "back one level"**: previously always closed the menu entirely; now treats MENU as a back-stack pop, only closing when at the top level. Mirrors how operators expect nested settings UIs to behave.

### Fixed
- **QR code encodes Wi-Fi IP instead of usb0 gadget IP** (the headline bug operators kept hitting): `WifiManager::get_ip_address()` was running `hostname -I` and taking the first token. On a Pi with the USB-Gadget service running, `usb0`'s static `10.55.0.1` consistently lists before `wlan0`'s real Wi-Fi IP, so the "Wi-Fi URL" displayed in the Content Manager submenu (and encoded in the QR) was secretly the USB IP. Replaced with `getifaddrs()` + explicit `wlan0` lookup.
- **VPN port forwarding (NAT-PMP) silently broken** for the entire stack: `FIREWALL_OUTBOUND_SUBNETS=192.168.0.0/16,10.0.0.0/8,172.16.0.0/12` looked correct but secretly shadowed ProtonVPN's `10.2.0.1` WireGuard gateway — Gluetun's iptables routed NAT-PMP requests out the LAN interface instead of through the tunnel, and the port-forwarding service gave up after 9 retries (`read udp …:5351: i/o timeout`). Result: dashboard read "VPN port unavailable" forever, qBit was firewalled, torrents downloaded slowly. Narrowed `FIREWALL_OUTBOUND_SUBNETS` to specific `/24` and `/16` LAN subnets that don't include `10.2.0.0/16`. Verified live: gluetun obtains forwarded port 38764 within 1 sec of restart, qbit-port-sync picks it up, dashboard flips to "✓ synced".
- **Two-player not working**: launcher emitted only `input_player1_*` bindings. With autoconfig disabled (we delete the file to force manual mappings for predictable behavior across DragonRise/Microntek pads), a 2nd identical pad showed up at `/dev/input/js1` but generated no in-game effect. See "Two-player support across all 7 cores" in Added for the fix.
- **Modern TV settings menu would scroll prematurely**: `move_selection()` had a hardcoded `max_visible_items=7` that didn't match the renderer's now-dynamic row count. Selecting "Back" with 9 rows visible would still bump scroll_offset to 1 and pop "Video Games" off the top. Renderer now publishes `max_visible_items` back to the input handler.
- **QR code shows wrong USB URL** (legacy bug): hardcoded to `http://192.168.7.1:5000`. Now reads live `usb0` IPv4 via `getifaddrs`.
- **`usb0` carrier check was IPv4-only**: settings menu marked usb0 "active" any time the gadget service had assigned `10.55.0.1`, even with no cable plugged in. Now requires `IFF_RUNNING` flag in addition to IPv4 presence.
- **Content Manager submenu QR could go stale**: now auto-rebuilds once per second while open so the QR + label reflect carrier-state changes (cable plugged/unplugged) without requiring the user to navigate out and back in.
- **Wi-Fi scan returned only 1 network**: `nmcli --rescan yes` doesn't actually wait. Fixed with explicit `nmcli dev wifi rescan` + 4-second sleep + plain `list` query.
- **Wi-Fi Disconnect was a no-op**: the SELECT handler re-ran `enter_submenu(WIFI)` after the action lambda flipped `wifi_disconnect_confirm_=true`, immediately resetting the flag back to false. See "Idempotent setting-menu re-entry" in Added for the structural fix.
- **Movie playback aspect ratio was stretched**: `gst_renderer.cpp` ignored source aspect — widescreen movies looked vertically stretched on a 16:9 TV. Now letterboxes 2.35:1 / 2.39:1 movies, pillarboxes 4:3 content, handles anamorphic DVDs correctly via pixel-aspect-ratio extraction from GStreamer caps.
- **Bezel disappearing after RetroArch exit**: removed a stale `static loaded_bezel_path` tracker in the main render loop that blocked `load_bezel()` from being called after `reset_gl()` destroyed the bezel texture. The renderer's own dedupe handles repeat calls correctly.
- **Bezel obscuring RetroArch in-game menu**: set `input_overlay_hide_in_menu = "true"` so the bezel auto-hides when the RetroArch menu opens (Z+Start hotkey).
- **Loading screen z-order**: the brief "Loading..." overlay during the kiosk → RetroArch handoff now renders the bezel on top of the loading text instead of the loading text covering the bezel — continuous visual framing through the transition.
- **Phase 0 test #33 (video playlist linkage) false-failed** on newly-uploaded videos because the test cd'd to `$PI_INSTALL_ROOT` instead of `$PI_INSTALL_ROOT/data`, so playlist-relative paths like `media/foo.mp4` didn't resolve.
- **Reorder warning** in `SettingsMenuManager` constructor init list (initialized fields out of declaration order — compiler initializes in declaration order regardless, so a mismatched init list is misleading at best, footgun at worst).

### Improved
- **PS1 core (`pcsx_rearmed`) tuning**: SPU reverb disabled, SPU interpolation off — small CPU savings with no audible impact on most titles. Combined with the global `video_threaded = true` (was false), notably smoother on Pi 4B.
- **RetroArch launcher refactor**: extracted duplicated video-config blocks from `launch_drm()` and `open_core_downloader_direct()` into a single `write_video_config(stream, opts)` helper that branches on `display_mode`. Per-core controller mapping logic split into `get_mapping_n64_adapter()` and `get_mapping_ps_style()` with a `get_mapping(type, core_name)` dispatcher.
- **RetroArch autoconfig file** now matches whichever controller is detected (`0e6d_111d.cfg` for N64, `0079_0006.cfg` for PS-style, none for UNKNOWN).
- **"or visit magicpi.local on any device"** hint added below the Content Manager QR for users without a phone camera handy.

### Pre-image testing pass
- All 7 cores launched, played, exited cleanly: NES, SNES, Genesis, PS1 (incl. 2-player Twisted Metal 4), PC Engine, Atari 7800, Arcade
- All 4 video bezels (1974, 1986, KV19, plus a Vintage TV) cycled correctly in Modern TV mode
- Content Manager web admin drag-and-drop verified for both video and game playlists
- Media Browser end-to-end: TMDB browse → search → add to library → indexer search → grab → download via VPN tunnel → playback with auto-pause → confirm-remove orphan-proof cleanup
- Pre-image checklist [tests/manual/pre_image_checklist.md](tests/manual/pre_image_checklist.md) updated with sign-off notes for Phases 0-4, 8-10
- Phase 5 (CRT mode) deferred until a CRT TV is available — code path validated via mode round-trip but visual verification pending
- Phase 6 (15-min PS1 endurance) optional, not blocking

## [1.3.0] - 2026-02-22

### Added
- 148 curated games across 7 systems: Arcade (16), Atari 7800 (20), NES (20), PC Engine (15), SNES (20), Sega Genesis (20), PlayStation 1 (34), with cover art thumbnails
- Multi-disc PS1 support via .m3u files (Final Fantasy VII, Metal Gear Solid, Gran Turismo 2, Resident Evil 2)
- D-pad hold-to-repeat for accelerated menu navigation
- Analog stick-to-D-pad axis mappings for SNES, Genesis, Atari 7800, and PC Engine cores

### Fixed
- **Critical:** OTA updates now preserve game saves (`data/saves/`, `data/states/`) during install, rollback, and backup
- RetroArch shell escaping uses single-quote wrapping (fixes games with apostrophes like "Tony Hawk's Pro Skater")
- Intro video no longer cuts off early
- Controller hotkey combo (Z + Start) restored for RetroArch menu toggle
- WiFi connection timeout (30s) with specific error messages instead of hanging
- WiFi shell injection eliminated (fork/execvp for nmcli commands)
- Input device last-resort recovery after RetroArch exits
- Playlist loader validates emulated_game items at load time (skips entries missing core or path)
- Empty ROM path validation before RetroArch launch
- File descriptor close limit uses sysconf() instead of hardcoded 256
- Removed dead keepalive process cleanup code and duplicate config writes in RetroArch launcher
- Removed broken Double Dragon and Metal Slug from arcade playlist

### Improved
- Master Shuffle uses GL-drawn crossed-arrows icon instead of text marker
- Now-playing playlist shown with green text instead of dot indicator
- Playlists numbered from 01 after Master Shuffle entry
- Code quality: security, performance, and reliability hardening pass

## [1.2.0] - 2026-02-21

### Added
- Systemd watchdog with automatic crash recovery (Type=notify, 10s watchdog)
- Now-playing indicator (accent dot) on current playlist in playlist list
- Error overlay banner for game launch failures (4-second auto-dismiss)
- Master shuffle history with "Previous" support (up to 10 entries)
- EOS callback infrastructure for GStreamer end-of-stream events

### Fixed
- **Security:** Shell injection in WiFi manager (SSID/password via fork/execvp instead of popen)
- **Security:** Tightened delete_media path validation to media directories only
- Atomic settings file write (temp file + rename) prevents corruption on crash
- Missing settings file no longer treated as error on first run
- GStreamer play() no longer blocks forever (3-second timeout replaces infinite wait)
- RetroArch uses isolated temp config (/tmp/retroarch_mdb.cfg) instead of overwriting user config
- Removed exit(0) in core downloader that killed the entire application
- Hardcoded XDG_RUNTIME_DIR replaced with dynamic getuid() in RetroArch launcher
- Replaced std::system("chmod") with POSIX chmod() calls
- Virtual keyboard text centering uses actual font metrics
- format_time() handles negative seconds gracefully
- Game scroll offset resets correctly when switching playlists
- WiFi saved networks reconnect without prompting for password
- WiFi disconnect requires two-press confirmation
- Removed dead "Controllers" menu link from Games submenu
- Removed const_cast antipattern in settings menu (uses mutable members)
- Analog stick auto-repeat fires immediately on direction change
- Playlist switching timeout reduced from 5s to 2s
- Watchdog paused during RetroArch gameplay to prevent false kills

### Improved
- RGB mask CRT shader uses smoothstep-based subpixel column darkening
- WiFi scan results cross-reference saved connections for accurate status

## [1.1.0] - 2026-02-08

### Added
- Rotary encoder video seeking with velocity-sensitive progress bar
- Playlist package import/export (ZIP with videos + playlist YAML)
- Playlist deletion with orphaned video cleanup
- Alphabetical playlist sorting on device

### Fixed
- Audio output setting (HDMI/Headphone) now persists across reboots
- Audio output configured before intro video plays (no mid-intro switching)
- Upload size limited to 8GB default (prevents storage exhaustion)
- ZIP extraction size limited to 10GB (prevents ZIP bomb attacks)
- XSS vulnerability in notification messages
- Playlist existence check now runs before extracting videos in package import

### Improved
- Content Manager UI with drag-and-drop playlist management
- YAML formatting handles non-string types safely
- Deploy script uses configurable paths instead of hardcoded values

## [1.0.7] - 2026-01-18

### Fixed
- OTA rollback functions no longer stop web service (same fix as 1.0.6 for main install)
- Rollback rsync now uses --no-group --no-owner to avoid permission errors
- Rollback handles rsync exit codes 23/24 gracefully

## [1.0.6] - 2026-01-18

### Fixed
- OTA update now works reliably - web service stays running during update
- Only C++ app is stopped during update, web service restarts at the end

## [1.0.5] - 2026-01-18

### Fixed
- OTA update subprocess now survives when web service is stopped during update

## [1.0.4] - 2026-01-18

### Changed
- Test release to verify OTA update system works end-to-end

## [1.0.3] - 2026-01-18

### Fixed
- OTA update rsync permission errors (added --no-group --no-owner flags)
- OTA update content directory detection for release tarballs
- Handle rsync exit codes 23/24 gracefully

## [1.0.2] - 2026-01-18

### Fixed
- OTA update backup directory permissions (now uses user-writable location)

## [1.0.1] - 2026-01-18

### Added
- Audio output selection (HDMI, Headphone Jack, Auto) via PulseAudio
- Game volume offset control for RetroArch (-3dB, -6dB, -12dB options)
- Audio submenu in Settings for easy audio configuration
- Audio settings persistence (saved to config file)

### Improved
- Video transcoding now normalizes audio to -23 LUFS (EBU R128 broadcast standard)
- Consistent volume levels across all transcoded videos
- System volume control via ALSA Master/PCM mixers

## [1.0.0] - 2026-01-16

### Added
- Initial stable release
- C++ kiosk engine with DRM/KMS for true kiosk mode
- Video playback via GStreamer with GL texture rendering
- RetroArch integration for NES, N64, and PS1 emulation
- Web admin interface for remote content management
- Playlist management with YAML format
- Video transcoding with CRT (640x480) and modern (720p) presets
- Smart upload with automatic transcode detection
- USB Ethernet Gadget support for fast content transfers
- WiFi configuration via on-screen menu
- QR code display for easy web admin access
- USB/WiFi network detection with priority handling
- M3U playlist generation for multi-disc PS1 games
- Backup and restore functionality
- Over-the-air (OTA) update system via GitHub Releases

### Technical Details
- OpenGL ES 3.0 rendering with immediate-mode 2D UI
- evdev input handling for controllers and keyboards
- stb_truetype font rasterization
- YAML-based playlist and settings persistence
- Flask-based REST API for web admin
- Systemd service integration for auto-start
