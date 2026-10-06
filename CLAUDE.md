# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Magic Dingus Box is a retro gaming and video playback kiosk for Raspberry Pi 4B and Raspberry Pi 5. The board is detected at runtime (`src/platform/platform_profile.{h,cpp}` reads `/proc/device-tree/model`); audio sinks, the GPIO header chip, and video-decode expectations all resolve dynamically — never hardcode Pi 4 sink names, `/dev/gpiochip0`, or `v4l2h264dec` availability. ONE golden image serves both boards: `PlatformProfile::unsupported_game_systems` + `PlaylistLoader::filter_for_platform` hide Pi 5-only systems (N64, Dreamcast) on a Pi 4B, `first_boot.sh` prunes their ROMs from Pi 4 clones, and `config.txt` uses `[pi4]`/`[pi5]` conditional sections — see `scripts/golden_image/CLONING.md` "One image, two boards". It consists of:

1. **C++ Kiosk Engine** (`magic_dingus_box_cpp/`) - Primary application using DRM/KMS for true kiosk mode with direct GPU access, no X11/Wayland
2. **Python Web Admin** (`magic_dingus_box/web/`) - Flask-based remote playlist/content management interface

## Dual-board contract (Pi 4B + Pi 5) — read before adding ANY feature

Units of BOTH board types are sold from ONE golden image and update from
ONE release artifact. Every change must compile for and run on both. The
rules, each earned by a real bug:

1. **Board differences resolve at RUNTIME, never at compile time.**
   `platform::PlatformProfile` (detected once from
   `/proc/device-tree/model`) is the only legitimate branch point. Never
   `#ifdef` a board, never hardcode a sink name, GPIO chip path, decoder
   element, or clock. If your feature needs a per-board value, add a
   field to `PlatformProfile` with explicit values for Pi4 / Pi5 /
   Unknown + unit tests in `tests/platform/` (pure logic — they run on
   the Mac).
2. **A Pi 5-only feature needs a gate, not an assumption.** Game systems:
   add the token to the Pi 4 profile's `unsupported_game_systems` (the
   kiosk menu filter), add the content paths to `first_boot.sh` Step 6e
   (Pi 4 disk pruning), and remember the OTA playlist sync's
   content-existence gate keeps the playlist off boxes without the ROMs.
   Other feature classes: branch on `profile.model` and make sure the
   Unknown (dev-machine) path does something sane.
3. **The performance envelope is the Pi 4B's.** 1.5 GB RAM (`make -j2`
   on-Pi builds), hardware H.264 decode but NO spare CPU for software
   codecs, emulation ceiling = PS1 (N64/Dreamcast are gated OFF).
   The Pi 5 (2 GB, software-decodes everything comfortably, runs
   N64/DC) is the roomy target — if it fits the Pi 4, it fits both.
4. **New system dependencies go in THREE places or they will bite:**
   `magic_dingus_box_cpp/scripts/install_deps.sh` (on-Pi builds), the apt list in
   `.github/workflows/release.yml` (the CI release binary), and the
   README dependency list. An OPTIONAL CMake dep that silently changes
   runtime behavior is a trap — libsystemd being absent in CI compiled
   out sd_notify and made systemd kill a perfectly healthy binary on
   every box (caught live, v1.7.2). If a capability is load-bearing,
   either make the dep REQUIRED or add a release-blocking `strings`
   assertion to the workflow next to the existing ones (aarch64,
   READY=1, prowlarr, HAVE_GPIOD, /connect?code=). The same checks run
   on every push in test-local.yml's `kiosk-build` job.
5. **`config.txt` model-specific settings live under `[pi4]` / `[pi5]`
   conditional sections, never `[all]`.** Current split: `[pi5]` has
   `v3d_freq=1000` + `kernel=kernel8.img` (4 KB pages — flycast dies
   without it); `[pi4]` has `gpu_mem=76`.
6. **OS floor is Trixie (Debian 13) on both boards** — libgpiod 2.x API
   and the CI binary's glibc. Anything older can neither build nor run
   the kiosk (v1.6.4 was the last Bookworm release).
7. **Before a release that touches the platform layer**, run the Mac
   suites (all 9 with Media Browser ON, 8 with it OFF), and validate on real hardware of BOTH boards when the
   change plausibly differs between them — the sd_notify failure was
   invisible in every off-Pi test.

Full background: `scripts/golden_image/CLONING.md` "One image, two
boards" and `OTA_UPDATE_GUARANTEES.md`.

### Platform policy override (TEST-ONLY, pre-release rehearsal)

`MDB_PLATFORM_POLICY_OVERRIDE=pi4` makes a **Pi 5** run the Pi 4B's
SOFTWARE POLICIES while keeping its real HARDWARE facts — it exists only
to rehearse Pi 4B logic before Pi 4B hardware is on the bench. Never
ship or clone a box with it set: **`verify_box.sh` FAILS** while it is
present in either unit's Environment, `services/.env`, a running
process's environment, or `kiosk_status.json`.

- **What switches** (the POLICY rows of the classification table in
  `platform_profile.h`): N64/Dreamcast systems + cores hidden, 64 MB
  poster budget, `pause_services_during_movie=true` /
  `trickle_torrents_during_video=false` (so `service_quiet_mode()` is
  always FullPause), and in the web admin one concurrent transcode with
  the ultrafast/CRF 28 encoder tier. **What stays real:** model, GPIO
  chip, analog-audio availability, rotary pulse rate, and everything
  probed outside the profile (decoders, sinks, page size, DRM/Vulkan).
- **Not covered:** board-gated *setup-time* writes — `setup_services.sh`'s
  Pi 4B preferred-size quality definitions and `first_boot.sh`'s Pi 4 ROM
  pruning still key on the real device tree (they persist state that
  would outlive the rehearsal).
- Only the exact value `pi4` is honored, only on a Pi 5; any other value
  (or a Pi 4B / unknown board) is ignored with a WARN. Active state logs
  one WARN `PLATFORM POLICY OVERRIDE ACTIVE: running Pi 4B policies on
  <model>` and publishes `"platform_policy_override": "pi4"` in
  `kiosk_status.json` (null otherwise).
- The kiosk and the web admin each read their OWN unit environment, so
  set it in BOTH:
  ```bash
  # enable
  for u in magic-dingus-box-cpp magic-dingus-web; do
    sudo mkdir -p /etc/systemd/system/$u.service.d
    printf '[Service]\nEnvironment=MDB_PLATFORM_POLICY_OVERRIDE=pi4\n' \
      | sudo tee /etc/systemd/system/$u.service.d/zz-policy-override.conf
  done
  sudo systemctl daemon-reload
  sudo systemctl restart magic-dingus-web magic-dingus-box-cpp
  # (or interactively: sudo systemctl edit magic-dingus-box-cpp / magic-dingus-web)

  # disable
  sudo rm -f /etc/systemd/system/magic-dingus-{box-cpp,web}.service.d/zz-policy-override.conf
  sudo systemctl daemon-reload
  sudo systemctl restart magic-dingus-web magic-dingus-box-cpp
  ```
  Never disable it with `systemctl revert`: that deletes EVERY drop-in
  for the unit, including the memory-tuning and audio ones
  `setup_memory_tuning.sh` / `setup_audio_service.sh` installed.

## Build Commands

### C++ Build (on Pi or cross-compile)
```bash
cd magic_dingus_box_cpp
mkdir -p build && cd build
cmake ..
make -j2          # on a Pi: each cc1plus peaks near 600 MB, so -j4 pushes a
                  # 1.5 GB Pi 4B into the OOM killer and a 2 GB Pi 5 into swap
                  # while the kiosk and containers are still running.
                  # Cross-compiling on a dev machine: use -j$(nproc).
```
`deploy_cpp.sh` and `deploy_fixes.sh` pick this automatically from `MemTotal`
(>= 4 GB gets `-j4`); `MAKE_JOBS` overrides.

### Deployment (from dev machine to Pi)
```bash
# Sync code only
./magic_dingus_box_cpp/scripts/deploy_cpp.sh

# Sync + build
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build

# Sync + build + test run
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --test

# Sync + build + install RetroArch cores
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --cores

# Setup USB Ethernet Gadget for fast uploads
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --usb-gadget
```

Environment variables: `PI_HOST` (default: `magic@magicpi.local`), `PI_DIR` (default: `/opt/magic_dingus_box`), `MEDIA_BROWSER` (default: `true` — the deploy target is the production Pi which always uses MB; set to `false` to build without it for debugging the OFF code path).

Note: rsync uses `--checksum` so file content (not mtime) determines whether to transfer. Without this, rsync's preserve-mtime default fooled cmake's incremental build into skipping rebuilds on Pi-side compilation — see commit `824ee88`.

### Running
```bash
# First-time (as root for DRM access)
sudo ./build/magic_dingus_box_cpp

# Production (add user to groups)
sudo usermod -a -G video,input $USER
# Re-login, then run without sudo
```

## Architecture

### C++ Source Structure (`magic_dingus_box_cpp/src/`)

- **`main.cpp`** - Entry point, main loop: poll input → update state → render video → render UI → swap buffers
- **`media_browser/mb_host`** - `MediaBrowserHost`: the kiosk side of the Media Browser — owns the MB screens, modals and dispatcher; main.cpp's loop calls it at fixed points (unlock sequence → `handle_input` → redraw-gate inputs → `render` → `tick_watch_state`). main() keeps the service clients/stores and passes references in. Screen-to-screen hand-offs are the pure table in `media_browser/ui/mb_transition.h` (unit-tested).
- **`platform/`** - Hardware abstraction
  - `drm_display` - DRM/KMS display init, mode setting, CRTC management
  - `gbm_context` - GBM surface for EGL
  - `egl_context` - OpenGL ES 3.0 context, swap chain
  - `input_manager` - evdev event processing, joystick/keyboard mapping, rotary encoder support
  - `gpio_manager` - GPIO access (power button, LEDs)
- **`video/`** - Video playback (GStreamer backend)
  - `gst_player` - GStreamer pipeline management, playback control
  - `gst_renderer` - GL texture rendering from GStreamer video frames
- **`ui/`** - User interface
  - `renderer` - Immediate-mode 2D renderer (quads, text, alpha blending), implemented across `renderer.cpp` (core/primitives), `renderer_shaders`, `renderer_text`, `renderer_main_menu`, `renderer_settings`, `renderer_mb`, `renderer_crt` and `renderer_batch`; private shared bits in `renderer_internal.h`. Primitives inside a `Renderer::BatchScope` (the whole `render(state)` pass, the MB screen + modals, the toast) accumulate into one CPU vertex batch (`ui_batch.h`, GL side `renderer_batch.cpp`) and are submitted per texture run; every non-batched GL site in the `renderer*.cpp` files calls `flush_ui_batch()` first, so draw order is unchanged. `MDB_BATCH_UI=0` restores one draw per primitive for A/B; the journal logs UI draw calls/frame per minute.
  - Redraw gate (`app/redraw_gate.h`, wired in `main.cpp`): render/swap/flip are skipped on iterations where nothing on screen can change — the main menu, an idle open Settings menu (`SettingsMenuManager::is_static_for_redraw`), and MB Browse/Search/Library/Detail/SeriesDetail when idle (`MbScreen::wants_continuous_redraw` + `redraw_signature`). The static menu with CRT flicker/interlacing draws every other vblank (`ui/crt_time.h`). `MDB_REDRAW_GATE=0` disables. A new screen or animation must either keep `wants_continuous_redraw()` true or put its state in the signature.
  - `theme` - Color palette and layout constants
  - `font_manager` - stb_truetype font rasterization → GL textures
  - `settings_menu` - Settings UI state machine
  - `virtual_keyboard` - On-screen QWERTY keyboard
  - `qrcodegen` - QR code generation for WiFi setup
- **`app/`** - Application logic
  - `app_state.h` - Global state (playlists, playback, settings)
  - `controller` - High-level video/audio control, RetroArch launch/return orchestration
  - `playlist_loader` - YAML playlist parsing
  - `settings_persistence` - YAML settings storage
  - `sample_mode` - Sample/demo mode for kiosk auto-play
- **`retroarch/`** - Game emulation
  - `retroarch_launcher` - DRM/KMS handoff, config generation (incl. video config via `write_video_config()`), process lifecycle. It no longer owns button mappings: it resolves one mapping per controller port and emits the `input_playerN_*` lines through `write_player_binds()`.
  - `controller_mapping` - the mapping layer, in two halves. **Semantic tables** (`semantic_n64_style()` / `semantic_ps_style()`) say which *logical* control drives each RetroPad slot for a given core — "RetroPad B ← the Cross button" — using `LogicalControl` values, never physical button numbers. **`build_mapping(SemanticMapping, PhysicalProfile)`** marries a semantic table to a concrete pad's physical layout to produce the `ControllerMapping` the launcher emits. `get_mapping(ControllerType, core_name)` remains the public dispatch entrypoint (signature unchanged); `resolve_mapping_for_pad()` is the per-pad form used at launch. Also owns `write_player_binds()`.
  - `controller_profile` - `PhysicalProfile`: where each `LogicalControl` physically lives on one pad model (evdev code + RetroArch bind token). Ships `builtin_n64_adapter_profile()` / `builtin_dragonrise_profile()` for the two known pads, and loads/saves operator-captured profiles keyed by USB VID/PID in `config/controller_profiles.json`. Resolution order is captured → builtin → legacy fallback. Also derives the kiosk's menu-navigation overlay for a pad.
  - `logical_controls` - the `LogicalControl` vocabulary (separate PS-style and N64-style sets) plus the wizard's per-style prompt order.
  - `joydev_index` - converts a raw evdev code + the device's capability lists into the RetroArch udev bind token (`"5"`, `"h0up"`, `"+2"`). The kiosk reads evdev codes; RetroArch configs want joystick indices — nothing else bridges the two.
  - `capture_session` - pure state machine behind the Controller Setup wizard: walks the per-style prompt list, decides when a press or stick deflection counts, rejects duplicates, supports skip/redo. No I/O.
  - `controller_detector` - USB controller probing (vendor/product IDs → `ControllerType` enum). `detect_primary_controller()` returns the first recognized pad; `detect_connected_controllers()` returns one entry per `/dev/input/js*` in port order, which is what per-port resolution consumes. Split out of `retroarch_launcher` in v1.4.0.
- **`debug/`** - `screenshot_capture`: `touch <data>/screenshot_request` and the next drawn frame (the full composite, read back before the swap) is written to `<data>/screenshots/<UTC>.bmp`, newest 10 kept. The directory is excluded from deploy/OTA rsyncs and scrubbed from golden images (personal content).
- **`utils/`** - Utilities
  - `config` - Centralized path configuration (base paths, RetroArch paths, save dirs)
  - `path_resolver` - Asset path resolution
  - `time_format` - `iso8601_utc(std::time_t)`: the one ISO-8601 UTC formatter. Lives here because its callers are in `retroarch/` (the wizard's `captured_at` stamp) and `media_browser/` (the "recently added" cutoff), and neither subsystem should depend on the other. Output is fixed-width so lexicographic order equals chronological order — both callers compare the result as a plain string. Returns `""` on failure, which callers MUST branch on rather than compare: `""` is less than every non-empty string, so letting it flow into a date comparison silently turns a filter into a pass-everything filter.
  - `wifi_manager` - WiFi scanning/connection via nmcli

### Rendering Pipeline

Both video and UI render to the same OpenGL ES context:
1. Video: GStreamer renders frame to default framebuffer
2. UI: Renderer draws overlay with alpha blending
3. EGL swaps buffers

This guarantees correct compositing without X11/compositor overhead.

### RetroArch Launch/Return Flow

1. Stop GStreamer pipeline → Release DRM master (keep CRTC for Vulkan) → Release input devices
2. fork/exec RetroArch DIRECTLY (no bash wrapper — `retroarch/game_session.{h,cpp}`) with the generated config, per-core controller mapping, and `input_playerN_reserved_device` pinning each player to its pad
3. Supervise (waitpid WNOHANG/WNOWAIT loop, ~50 ms) until RetroArch exits, pinging the systemd watchdog. Every stop — SIGTERM (`systemctl stop`, OTA, reboot, the GPIO restart button), startup timeout — goes through ONE `stop_game_session()`: SIGTERM RetroArch, ≤5 s for its auto-save, then SIGKILL the process group. The SIGTERM handler only sets `retroarch::request_session_stop()`; never kill from the handler
4. Re-acquire DRM master (5 retries) → Re-init input (3 retries) → Restore EGL context → Rebuild GL resources

### Audio System

- PulseAudio for routing (HDMI/Headphone/Auto selection)
- **The TV may be on EITHER HDMI port.** Never hide, ignore or prefer a port by name: the empty port's "Failed to find a working profile" log line is harmless; a hidden real port is a silent box (a `PULSE_IGNORE` rule for vc4hdmi1 did exactly that on a Pi 5, 2026-10-03 — `audio_service.sh prepare` now removes it). Games pick the card whose ELD lists audio descriptors (`retroarch::eld_reports_monitor`); `verify_box.sh` fails a box whose default sink is `auto_null`.
- **PulseAudio is its own system unit, `magic-dingus-audio.service`** (User=magic, Restart=always, `StartLimitIntervalSec=0`), ordered `Before=` the kiosk; the kiosk has `Wants=`+`After=` on it (Wants, never Requires — a sound failure must not block the picture, and Requires would restart the kiosk with every audio restart). Until 2026-10 `init_audio.sh` (kiosk ExecStartPre) ran `pulseaudio --start`, which daemonized INTO the kiosk's cgroup: every kiosk stop/restart/crash SIGKILLed the sound server (tdb corruption risk), every start logged "Found left-over process (pulseaudio)", and the kiosk's MemoryLow/OOMScoreAdjust silently covered it. `scripts/audio_service.sh` does the work: `prepare` (root, `ExecStartPre=+`: mask user-session PA/PipeWire FIRST — they raced and wedged, keep them masked — then linger, remove the old HDMI-hiding udev rule, start `user@<uid>` so `/run/user/<uid>` is logind's tmpfs, wait for the HDMI card), `run` (writes `~/.config/pulse/{default.pa,daemon.conf,client.conf}` with `autospawn = no` so no client can spawn a rogue PA inside its own cgroup, then `exec pulseaudio --daemonize=no --exit-idle-time=-1`; it WAITS for, never kills, a PulseAudio it did not start), `set-sink` (`ExecStartPost=-`: wait for PA, resolve the sink from settings.json via `resolve_audio_sink.sh`). The uid is resolved at runtime — never hardcode it, and `%U` in a system unit is the manager's uid (0), not User='s.
- **Delivery:** `setup_audio_service.sh` installs + enables the unit and writes the kiosk drop-in `magic-dingus-box-cpp.service.d/audio-service.conf` (`Wants`/`After`, an empty `ExecStopPost=` that clears old units' `pulseaudio --kill`, and `XDG_RUNTIME_DIR` from the real uid). It never starts or stops anything. It rides `setup_memory_tuning.sh`, the root hook every path runs (deploy, OTA — including the OLD update.sh on the first OTA that ships it — first_boot, sync_source_box). The unit's `ConditionPathExists=` on `audio_service.sh` makes a rollback to an older tree skip it cleanly.
- `init_audio.sh` (kiosk ExecStartPre) only confirms PA answers and sets the default sink. **Fallback:** if `/etc/systemd/system/magic-dingus-audio.service` is absent (or masked), it runs `audio_service.sh legacy-start` — the old kill-and-start inside the kiosk — so a box without the unit still has sound. Always exits 0.
- **RetroArch is unaffected:** games use ALSA directly (`sysdefault:CARD=vc4hdmiN`); PulseAudio's module-suspend-on-idle releases the device once the kiosk's GStreamer pipeline is torn down. Which cgroup PA lives in does not change that handover. Clone prep (`prepare_for_cloning.sh`, `prepare_golden_image.sh`) stops the audio unit explicitly, since stopping the kiosk no longer stops PA.
- Runtime one-shot `apply_output()` moves active GStreamer stream to correct sink
- Per-game volume offset for RetroArch (dB conversion from system volume)
- Settings persist in `config/settings.json`

### Save System (RetroArch)

- SRAM saves: `data/saves/<CoreName>/` (e.g., `data/saves/PCSX-ReARMed/game.srm`)
- Save states: `data/states/<CoreName>/`
- `sort_savefiles_by_content_enable = true` auto-creates core subdirectories
- Auto-save on exit and auto-load on start enabled for seamless kiosk experience

### Web Admin (`magic_dingus_box/web/`)

- `admin.py` - Flask routes for device discovery, playlist CRUD, content uploads, game ROM management
- `static/manager.js` - Frontend: device discovery, drag-and-drop playlist builder, file uploads
- Features: video transcoding, playlist package import/export (ZIP), system monitoring
- **Transcode presets are MASTERS, not display formats** (retuned 2026-07-26).
  The kiosk scales stored content to whichever display mode is active, so
  keep the best master storage allows and let playback derive the rest.
  Aspect matters as much as resolution: the main kiosk renders playlist
  video into a **4:3** viewport (`vp_w = canvas_h*4/3` → 960x720 at 720p,
  1440x1080 at 1080p output) — the deliberate CRT look — so a 16:9 master
  gets letterboxed *inside* that pillarbox and ends up smaller on screen.
  4:3 masters are therefore correct for playlist content even on a
  widescreen TV.
  | preset | size | aspect | use |
  |---|---|---|---|
  | `crt` | 640x480 | 4:3 | legacy / smallest files |
  | **`crt_hd`** | **960x720** | **4:3** | **default** — 2.25x the old detail, 1:1 at 720p output |
  | `crt_fhd` | 1440x1080 | 4:3 | max detail, 1:1 at 1080p output, ~2.25x the files |
  | `modern` | 1280x720 | 16:9 | genuinely widescreen source material only |
  Note 640x480 was never a pixel-exact CRT path: the Pi 5 has no composite
  output, so CRT rigs go through an HDMI→composite converter and the signal
  is downscaled regardless — a higher-resolution master is strictly better
  for CRT too. Default changes affect NEW uploads only; existing files
  cannot regain detail they never had.
- Data directory: `/opt/magic_dingus_box/magic_dingus_box_cpp/data` (configurable via `MAGIC_DATA_DIR`)
- **Server: gunicorn, ONE worker, gthread threads** (`web/serve.py`, since
  2026-10). `magic-dingus-web.service` still runs
  `python3 -m magic_dingus_box.web.wsgi` — that line is frozen in the field
  because an OTA never rewrites unit files — and `wsgi.py`'s `__main__`
  hands over to `serve.main()`, which runs gunicorn when `python3-gunicorn`
  imports and **falls back to the Werkzeug server** otherwise
  (`MAGIC_WEB_SERVER=werkzeug` forces it). Rules, each load-bearing:
  - **`workers` must stay 1.** The uinput virtual gamepad, CSRF tokens,
    transcode/update job registries, pairing lock, Network Doctor and
    health single-flights and the live WS connection list are all
    per-process; two workers = two gamepads and half-working CSRF.
  - **Threads (default 32, `MAGIC_WEB_THREADS`)** bound concurrency; each
    connected phone holds one for the life of its WebSocket. Idle
    keep-alive connections don't hold a thread in gthread.
  - **`--timeout` (120 s) is a worker heartbeat, not a request deadline**
    in gthread — multi-GB uploads and all-evening WebSockets are never
    killed by it. `max_requests=0`: never recycle the worker.
  - **`create_app()` runs once, in the worker** (the arbiter never imports
    `admin.py`; `wsgi.py` builds `app` only when imported, not under
    `__main__`). Its startup work — upload_temp sweep, `/dev/uinput` open —
    must not run twice. `tests/test_serve.py` boots the real launcher under
    both servers and asserts this, plus Host/Sec-Fetch-Site checks, a WS
    reconnect storm and a streamed upload.
  - Delivery: `install_deps.sh` (fresh installs), `update.sh`
    `ensure_web_server_dep` (OTA — narrow `apt-get install
    python3-gunicorn`, never fatal, run only after the kiosk's verified
    start; the download is `timeout`-bounded, the dpkg install never is),
    and `deploy_cpp.sh` (calls the same function). Heartbeat file in `/dev/shm`, not the SD-card `TMPDIR`.
- **Box health + diagnostics** (Settings tab). `POST /admin/health/run`
  (CSRF) starts `sudo -n /usr/bin/timeout --kill-after=10 <secs> /bin/bash
  scripts/verify_box.sh [--with-services]` (the deadline enforced as root —
  a Python-side kill only reaches `sudo`, leaving the root run alive beside
  the next one) in a single-flight background thread (`box_health.py`; `--with-services`
  only when the Media Browser is unlocked); `GET /admin/health/status`
  returns the parsed sections/checks, counts and a plain-language
  `headline`, cached in `data/box_health_last.json` (wiped by
  `first_boot.sh` on clones; the OTA's rsync --delete drops it on update,
  deliberately). `GET|POST /admin/diagnostics/bundle` (`diagnostics.py`)
  streams a zip of system info, unit status, journal tails, the health
  result, kiosk_status.json, launcher logs and the VPN tunnel event log.
  **Everything in it passes
  `redact.py`**: exact values from `services/.env` / `flask_secret.key` /
  the TMDB key file, secret shapes (env assignments, JSON, headers, URL
  params, JWT, WireGuard keys, long hex), and a whole-line drop for any
  remaining password/psk/secret/token mention; `kiosk_status.json`'s
  `text_input.buffer` (the live TV keyboard — Wi-Fi password screen
  included) is blanked. Never add a file to the bundle without a redaction
  test; `FORBIDDEN_NAMES` refuses the known credential files outright.
- **VPN tunnel line** (2026-10, Media Browser boxes only — the section is
  absent without `services/.env`). The owner's tunnel dropped ~170 times
  over 2026-10-03/04 and only a manual journal dig found it.
  `gluetun_cascade_restart.sh` appends `<epoch> watch|unhealthy
  <portfwd|tunnel>|healthy|restart` to `/var/lib/magic-dingus/vpn_events.log`
  (root writes, 0644, 7 days / 2000 lines, Docker's event time, a failed
  write never touches the watcher; `first_boot.sh` wipes it on clones).
  `scripts/vpn_events_summary.py` (pure, stdlib, `scripts/tests/
  test_vpn_events_summary.py`) turns it into verify_box's "VPN tunnel"
  PASS/WARN — WARN at >= 6 drops or >= 30 min down in 24 h, or down right
  now >= 10 min; never FAIL. verify_box passes gluetun's CURRENT health so
  an outage whose recovery went unrecorded is not "down right now".
  `box_health.headline` names an unreliable tunnel ("...but the VPN tunnel
  is unreliable") and the card opens that section. Down time counts from
  Docker's unhealthy mark (~5 min after the first failed check), so it
  understates real outages by up to that much.

## Key Dependencies

C++ (via pkg-config): `libdrm`, `libgbm`, `libegl`, `libgles2`, `libevdev`, `libgpiod`, `yaml-cpp`, `jsoncpp`, `gstreamer-1.0`, `gstreamer-app-1.0`, `gstreamer-video-1.0`, `gstreamer-gl-1.0`

Header-only: `stb_truetype.h`, `stb_image.h` (in `src/utils/`), `spdlog` (fetched via CMake FetchContent)

Python: Flask (for web admin only)

## Playlist Format

YAML files in `data/playlists/`. Item types accepted by the loader:
- `source_type: local` - Local video file (default when `source_type` is absent)
- `source_type: video` - Legacy alias for `local`; still accepted by the playback dispatch
- `source_type: youtube` - YouTube URL
- `source_type: emulated_game` - RetroArch game (path to ROM, `emulator_core`, `emulator_system`)

Prefer `local` when authoring — that's the canonical default and what `playlist_loader` produces when serializing. The schema is enforced inline in `magic_dingus_box_cpp/src/app/playlist_loader.cpp`; no JSON Schema file is tracked.

`magic_dingus_box_cpp/docs/PLAYLIST_FORMAT.md` exists only in some local checkouts (that directory is git-ignored), so treat `playlist_loader.cpp` as the schema of record.

## Controls

### Main UI
- **DPad/Axis X**: Navigate playlists
- **A/Enter/Space**: Select playlist item
- **Z**: Play/Pause
- **L/R Triggers**: previous / next item while a playlist plays (shuffle history in Master Shuffle); seek ±10s otherwise
- **PS-style pads**: Cross = select, Circle = settings, Triangle = play/pause, L1/R1 = previous/next (`input_manager.cpp` `map_button_to_action`)
- **C-Stick**: Seek ±5s
- **Rotary Encoder**: Velocity-sensitive video seeking with progress bar
- **B**: Settings menu
- **Q/Esc**: Quit

### In RetroArch
- Per-core button mappings live in `controller_mapping.cpp` (semantic tables) combined with a pad's `PhysicalProfile`; player 1 and player 2 resolve independently from whichever pad is on each port
- **Exit a game — direct quit, no RetroArch menu** (owner decision 2026-08-03; the RA menu "isn't needed at all" on a kiosk): N64-style pads = hold **Z + press Start**; PS-style pads = hold **Select + press Start**. Bound from PHYSICAL controls in the style preambles (`SemanticMapping::exit_emulator`) so per-core RetroPad slot remaps can never move the gesture — the mupen table repurposes the select SLOT for N64 L, which is exactly how the old start+select combo silently became Start+L1 in N64 games. RetroArch honors these hotkeys from the PLAYER 1 pad only (accepted limitation with two pads). Auto-save-on-exit makes direct quit safe.
- Auto-save state on exit, auto-load on start

## RetroArch Cores

10 cores installed via `--cores` flag (`magic_dingus_box_cpp/scripts/install_cores.sh`) — the 9 systems below, plus `parallel_n64_libretro` as the N64 backup. OTA also self-heals cores: `update.sh` scans the box's live playlists for referenced `emulator_core` values and runs `install_cores.sh` if any `.so` is missing from the runtime cores dir.

| System | Core | Notes |
|--------|------|-------|
| NES | `nestopia_libretro` | Digital input, analog-to-dpad mapping |
| SNES | `snes9x2010_libretro` | Digital input |
| Genesis/Mega Drive | `genesis_plus_gx_libretro` | 3/6-button support |
| PS1 | `pcsx_rearmed_libretro` | Analog pad type, requires BIOS (`scph5501.bin` in system dir) |
| PC Engine | `mednafen_pce_fast_libretro` | I/II + turbo buttons |
| Atari 7800 | `prosystem_libretro` | 2-button |
| Arcade | `fbneo_libretro` | 6-button layout |
| N64 | `mupen64plus_next_libretro` | Primary N64 core (`parallel_n64_libretro` is the backup with the identical core-option contract); per-title overscan crop in `launch_contract.cpp` |
| Dreamcast | `flycast_libretro` | Requires the 4 KB-page kernel (`kernel=kernel8.img` on Pi 5); no BIOS shipped — falls back to REIOS |

BIOS location: `~/.config/retroarch/system/`
Core location: `libretro_cores/` (app directory) or `/usr/lib/aarch64-linux-gnu/libretro/` (system)

## OTA Updates

- `scripts/update.sh` checks GitHub API for latest release
- Downloads tarball, backs up current installation, extracts update
- Rollback support if update fails
- **Never leaves a box without a kiosk** (2026-10): builds in `build.new/` and swaps it in only after `verify_kiosk_binary`; an "install in progress" marker (`/home/magic/.magic_dingus_box_backup.ota_in_progress`) + `magic-dingus-ota-recovery.service` restore the backup at the next boot after a power cut. The kiosk exits **69** (`src/platform/kiosk_exit.h`) when no display is connected — `update.sh` accepts that as a good start; keep the two in sync. rsync exit 23 is a FAILURE. Details: `OTA_UPDATE_GUARANTEES.md` "2026-10 hardening".
- **The kiosk is stopped once, only for the swap + restart** — not before
  the rsync. A source build (no usable pre-compiled binary) compiles in
  `build.new/` while the OLD kiosk keeps the TV on, when the box can
  afford it: `build_memory_plan` (pure, table-tested in
  `test_update.bats`) decides from board + `MemAvailable` measured with
  the kiosk up — ≥1300 MiB → `-j2`, ≥700 MiB → `-j1`, else pause the Media
  Browser services via `playback_services_pause.sh` and re-measure, else
  the old behaviour (stop the kiosk first, `-j2`). Unknown board or a
  kiosk that isn't `active` → old behaviour. Paused services are resumed
  on every exit path (post-compile, `fail_install`, EXIT trap). The
  compiler runs at `oom_score_adj 1000` + nice 19; an OOM-killed build is
  retried once with the kiosk stopped at `-j1`. The stop must stay BEFORE
  the swap: `build/` is the kiosk's `WorkingDirectory=`. Details:
  `OTA_UPDATE_GUARANTEES.md` "The TV stays on while a source build
  compiles".
- Triggered via web admin `/admin/update/*` endpoints (`version`, `check`, `install`, `status/<job_id>`, `rollback`, `channel`) — NOT `/api/update/*`
- **Update channels: `stable` (default) / `beta`** — one word in `<install>/config/update_channel` (absent = stable; `/config/*` is excluded from every OTA rsync). Stable queries `releases/latest` (GitHub never returns prereleases there — the request is byte-identical to pre-channel updaters, which is why boxes on ≤1.10.0 can never see a beta). Beta queries `releases?per_page=20`, ignores drafts, takes the highest version across stable + beta. Versions are `X.Y.Z` or `X.Y.Z-beta.N` ONLY (`VERSION_RE` in update.sh, `_OTA_VERSION_RE` in admin.py, the tag check in release.yml — change all three together), ordered by update.sh's pure-bash SemVer `version_cmp` (never `sort -V`: it ranks `1.10.1` below `1.10.1-beta.1`). No channel ever offers a downgrade. Set via `update.sh channel [stable|beta]` or the Content Manager's Advanced toggle. Tag `vX.Y.Z-beta.N` → release.yml publishes a GitHub prerelease; betas reuse `## [Unreleased]` (no beta changelog headings). Clones: `prepare_for_cloning.sh` refuses a beta box, `first_boot.sh` deletes the flag, `verify_box.sh` WARNs. Operator workflow: `magic_dingus_box_cpp/docs/RELEASING.md`. Never mention betas in `OWNER_GUIDE.md` (customer-facing).
- **Rehearsing an OTA before a release** (`tests/ota_rehearsal/README.md`): `tests/ota_rehearsal/run.sh` replays OLD→NEW→rollback in arm64 containers (no Pi); `PI_HOST=magic@<ip> tests/ota_rehearsal/hw_rehearsal.sh --yes` does it on ONE real box — downgrade to OLD from real GitHub, strip the system state a field box lacks (`field_state_<old>.txt` + git-derived units/drop-ins), install NEW through the OLD web admin against an on-box fake GitHub, verify, roll back, then restore this checkout and diff against the pre-run snapshot. `--dry-run` prints every command and touches nothing; an aborted run prints its remaining steps and `--restore-only <logdir>` finishes them. A new OLD baseline needs its own `field_state_<X.Y.Z>.txt`.

## Media Browser (Movie Playback + Downloads)

The Media Browser is a sub-mode of the kiosk that provides movie discovery, downloads, and playback through a Radarr / Prowlarr / qBittorrent / Gluetun stack. Architecture summary:

### Data flow

- TMDB → BrowseScreen / SearchScreen (movie discovery)
- Radarr → DetailScreen (library state, queue management)
- Prowlarr → AVAILABILITY readout on Detail (release-search seeders, async background thread)
- qBittorrent → QueueScreen live overlay (real-time progress; Radarr's queue cache is 30-60s stale)
- Gluetun → VPN tunnel for all torrent traffic with NAT-PMP port forwarding
- GStreamer playbin → PlaybackScreen (hardware H.264 decode on Pi 4, software decode on Pi 5, software HEVC fallback on both)

**TV playback (Phase 3):** TV is fully play-capable, not just browsable.
SeriesDetailScreen offers a per-episode picker backed by a live Sonarr
`/episode` fetch (files land between polls, so the picker never trusts a
cached season map), and PlaybackScreen resumes mid-episode via the
`load_file` start parameter. Watch state persists through `WatchStore` —
30-second checkpoints during playback plus mark-on-EOS — into the SQLite
`media_browser.db` (schema migration v3). The store is main-thread-only
by contract, and the DB file is deploy-excluded so a redeploy never wipes
watch history. At episode end, a next-episode countdown (8 s) reloads the
pipeline in place — no screen transition — and at season end an offer
card feeds the existing Start-Season-N monitor+search flow. The Library
grid mixes movies and TV in one rail; its Unwatched filter uses real
per-series watched counts with season 0 (specials) excluded from the
episode totals. The Library keeps every Sonarr series, including
shows with nothing on disk (drawn dimmed with a NOTHING DOWNLOADED tag) —
only Remove takes a show out; origin: Game of Thrones vanished from the
grid after its last season was deleted (2026-08-22).

### Playback hardware notes

- **Pi 4**: `v4l2h264dec` (hardware H.264) is rank-promoted and used by default
- **Pi 5**: has NO hardware H.264 decoder (BCM2712 dropped the block); the rank promotions are no-ops there and playbin falls through to `avdec_h264` (libav software, ~20% CPU for 1080p on the A76s — validate headroom on the 2GB board)
- `v4l2slh265dec` (V4L2 stateless hardware HEVC, both boards) is **disabled** — SAND pixel-format negotiation bug with GStreamer 1.22-era Bookworm; falls back to `avdec_h265` (software, ~30-50% of one core for 1080p 8-bit Main profile). NOTE (2026-07-22): production Pis actually run **Trixie** with GStreamer 1.26, where the SAND fix landed — the disable is now conservative and re-testable (see CLONING.md "HEVC experiment")
- AV1 has no hardware decoder; software-decode at 1080p+ is unwatchable
- Required system package: `gstreamer1.0-libav` (codified in `magic_dingus_box_cpp/scripts/install_deps.sh`)

### Playback contention guard (torrents vs. the video pipeline) — memory-gated

Torrent traffic and the resident service stack contend with the video
pipeline, so playback quiets them — and since 2026-08-11 the HOW is
decided **per session from measured memory**, not statically per board:
`platform::service_quiet_mode(profile, read_mem_available_kib())`,
consumed in `PlaybackScreen::enter()/leave()`. History: the old static
"Pi 5 trickles" split shipped a stutter regression when the stack
outgrew the July measurement behind it (768 MB in zram swap, 300k major
faults in the kiosk mid-movie — freeze, then silent fast-forward
catch-up).

- **FullPause** (Pi 4B/Unknown always; ANY trickle-profile board whose
  MemAvailable at play start is under `kServiceQuietMemFloorKiB` =
  1.5 GiB): qBit `pause_all()` + `playback_services_pause.sh pause`
  stops the arr/Byparr containers. The floor is deliberately above a
  2 GB board's reachable ceiling (~1.3 GiB fresh-boot): Trickle was
  HARDWARE-DISPROVEN there — with upload choked, cgroup MemoryLow
  protecting the kiosk, and zero active downloads at 940 MiB available,
  resident service ticks alone still froze the pipeline ≥3 s about once
  a minute; only the full pause ran clean. Do not lower the floor
  without an equivalent hardware session behind you.
- **Trickle** (trickle-profile boards with ≥1.5 GiB available — i.e.
  4 GB+ units): qBit's *alternative speed limits* engage instead, and
  downloads keep progressing through the movie. Rates are converged at
  every kiosk startup via `configure_alt_speed_limits(2 MiB/s down,
  8 KiB/s up)` (bytes-unit fields — see qbittorrent_client.h). Upload
  is near-zero by design: seeding is the expensive direction (8x read
  amplification measured with no page cache — 122 GB read to upload
  18 GB off the same SSD the movie streams from). Near-zero, not zero:
  0 means UNLIMITED to qBit. qBit 5.x has no explicit-set endpoint for
  the mode, so `set_alt_speed_limits_enabled()` reads first, toggles
  only on mismatch, and re-verifies.
- **Games: full `pause_all()` on EVERY board** (`GameQuietMode` in
  main.cpp) — games need the CPU/RAM back, not just disk quiet.
- **Startup clears the cap (crash recovery):** main.cpp's MB init calls
  `set_alt_speed_limits_enabled(false)` unconditionally — a kiosk crash
  mid-movie must never leave downloads silently capped. Best-effort:
  qBit may still be down at kiosk start; failures log, never block.
- `PlaybackScreen::leave()` clears only what enter() set
  (`app::MovieQuietMode::Consent` in `movie_quiet_mode.h` is the consent
  records) — an operator's own alt-limits or manual pauses are never
  flipped.
- **Box-side half of the same fix** (`setup_memory_tuning.sh`, run by
  deploy_cpp.sh, update.sh OTA hook, first_boot.sh, and
  sync_source_box.sh): kiosk `MemoryLow=512M` drop-in + system.slice
  companion (slice-level protection is REQUIRED or the service-level
  one is silently inert), `vm.page-cluster=0` for zram, and
  `cgroup_enable=memory cgroup_memory=1` appended to cmdline.txt (the
  Pi firmware disables the memory controller by default; reboot
  required to arm). `verify_box.sh` fails a box whose controller or
  kiosk memory.low is missing. The same kiosk drop-in carries
  `OOMScoreAdjust=-500` (the OOM killer takes a self-restarting
  container, never the kiosk; `verify_box.sh` checks it). Because OTA
  never re-installs unit files, this script is also the delivery path
  for in-tree unit fixes on fielded boxes: it writes drop-ins for the
  storage-attach `TimeoutStartSec=600`, the smoke-test
  `TimeoutStartSec=300` and the missing-search timer's `OnBootSec=11min`
  — change one, change both. No container `mem_limit`s, deliberately:
  measured on a 2 GB Pi 5 (2026-10-03) Byparr idles at ~10 MB but peaks
  at 762 MB inside a Cloudflare challenge (a cap breaks the indexers
  behind it), and qBittorrent's ~1 GB peak is reclaimable page cache.
  Instead compose ranks them for the OOM killer: Byparr
  `oom_score_adj: 800`, qBittorrent `300`, kiosk `-500`.

### Quality configuration (3-layer enforcement)

1. **Quality profile "Any"** — only allows 720p/1080p HDTV/WEB/Bluray (no SD, no 4K, no Remux)
2. **Custom Format scoring** (sums vs `minFormatScore = -200`):
   - AV1: -1000 / Remux: -500 / HEVC 1080p+: -250 / HDR: -200 (all rejected)
   - **Release groups, RETUNED FOR PI 5 (2026-07-26)**: split into
     `Quality release groups` (+30: RARBG/SURGE/EVO/FGT/TGx) and
     `Low-bitrate size-optimized groups` (**-30**: YIFY/YTS/GalaxyRG/
     ION10/QxR). These were ONE format at +30, which stacked with
     x264's +50 so a low-bitrate YIFY encode scored +80 and beat a
     better release at +50 — the rules optimized for file SIZE while
     claiming to optimize quality. Correct on the Pi 4B (hardware
     H.264, tight storage); wrong on Pi 5, where 1080p software decode
     measured 36% of 400% CPU (H.264) / 41% (HEVC) — ~3.5 of 4 cores
     idle. YIFY still nets +20 so it stays eligible when nothing
     better exists; it just stops winning. The pre-split format name
     is kept in `SCORE_MAP` scored **0** to neutralize it on boxes
     provisioned before the split (the profile reconciler only
     rescores formats named in the map, so removing the line would
     leave those boxes on the old +30 bias forever).
   - **Scam executables: -10000** (regex matches `.exe/.bat/.scr/.cmd/.com/.vbs/.lnk/.msi/.ps1/.app/.jar/.hta` in title — observed live: malware .exe payloads posted by trash indexers for new theatrical releases)
   - **Scam aggregator branding: -10000** (regex matches `uindex.org`, `fxnow`, `123movies`, `fmovies`, `gomovies`, `putlocker` in title — these prefix patterns reliably correlate with content-is-garbage releases)
   - **English-audio rule: TWO formats at -10000 each** (owner decision
     2026-08-13, "no foreign audio on English content"), in Radarr AND
     Sonarr — Sonarr had no language gate of any kind before this.
     `Release language is not the original` is a **LanguageSpecification**
     with `value: -2` (Original) + `exceptLanguage: true` — it fires when
     any language other than the title's own original language is present,
     so an English show rejects a French release while *Parasite* still
     accepts its Korean one (verified live: 118 Korean-tagged Oldboy
     releases, zero language-rejected; the English-dubbed ones all -10000).
     `Non-English title signals` is the title-regex backstop: non-Latin
     script plus foreign-dub and **multi-audio** markers (`MULTi`,
     `TRUEFRENCH`, `VOSTFR`, `SUBFRENCH`, `VFF/VFQ/VFI/VFB`, `DUAL AUDIO`,
     `GERMAN.DL`, `iTA.ENG`-style language pairs). Both are load-bearing:
     `Game Of Thrones S03 MULTi (1080p) BluRay x264 PopHD` (French audio)
     was parsed as **English** — i.e. as the Original — so the language
     spec passes it and only the regex catches it; conversely
     `Game of Thrones S03 FRENCH LD HDTV` matches no marker the regex
     dares carry (a bare language name marks the ORIGINAL on a foreign
     film, so blanket-rejecting it would make world cinema unobtainable)
     and only the language spec catches it. Both verified against live
     indexer searches on 2026-08-13.
   - x264: +50 / Trusted groups (YIFY/GalaxyRG/RARBG/SURGE): +30 (preferred)
3. **Quality definition size limits**: 720p ≤60 MB/min, 1080p ≤100 MB/min.
   *Preferred* sizes raised 2026-07-26 for Pi 5 (720p 25→40, 1080p
   40→70 MB/min): the box is not decode-limited and the library SSD
   had 175GB free, while actual grabs were landing at 2.5-3.4 Mbps
   against a ~13 Mbps ceiling.
4. **Post-completion auto-blocklist** (`magic-dingus-auto-blocklist.timer`): catches the long-tail of scams where the release title is legit-looking and slipped past layers 1-3, but the actual downloaded content is junk (executable file, "no videos in folder", "unsupported extension"). See Service operations below.

Net effect: every grab is x264 H.264 in the 720p-1080p range, 1-3 GB typical, hardware-decoded smoothly. Scams that get past pre-grab filters are auto-blocklisted within 15 minutes post-completion.

### Family-safe filter (R-rated kept, porn blocked, 3 layers)

1. **TMDB request-side**: `include_adult=false` on all list endpoints
2. **TMDB parser-side**: drops entries with `adult: true`; `parse_movie_detail` returns `nullopt` for adult IDs
3. **Prowlarr search-side**: restricted to Newznab Movies categories (2000-2080); `kAdultMarkers` regex filter on result titles drops porn-studio watermarks (`brazzers`, `bangbros`, `naughty america`, `evil angel`, `kink.com`, `pornhub`, `blacked.com`, `vixen.com`, `xxx parody`, `pornstar`, etc.). Bare anatomical terms intentionally excluded so legitimate films like "Deep Throat (1972)" and "Sex and the City" still surface.

### Confirm Remove flow (4-step orphan-proof cleanup)

1. Cancel any active Radarr queue items for the movie
2. Walk Radarr history → ask qBit to delete every torrent ever associated with the movie (catches finished+seeding torrents that step 1 misses)
3. `Radarr.remove_movie(deleteFiles=true)` — removes movie record + library file
4. Return to Library

### Confirm delete Season N flow (per-season orphan-proof cleanup)

TV's episode picker offers a trailing "Delete Season N…" row (arm on
first press, confirm on second — `series_detail_logic.h`) that runs
the same orphan-proof shape as Confirm Remove above, scoped to one
season instead of the whole series, on a background worker so the
render thread never blocks:

1. Unmonitor the season AND its episodes individually — the two flags
   are independent and `SeasonSearch` skips unmonitored episodes, so a
   season-only unmonitor would leave the episodes armed and make the
   re-monitor on the way back in meaningless. **This does NOT stop the
   re-grab** — see the guard in step 3b
2. Read the season's Sonarr history (authoritative; a failed read
   aborts here, before anything destructive)
3. Cancel the season's live queue rows with `blocklist=true`
   (which already carries `skipRedownload=true`)
3b. **Hold an `AutoRedownloadGuard` across steps 3-6**: switch Sonarr's
   global `autoRedownloadFailed` off, and restore the original value on
   EVERY exit path — normal, abort, and exception. Hardware, 2026-08-13:
   step 4's mark-as-failed makes Sonarr fire an **explicit
   `EpisodeSearch` by episode id**, which bypasses `monitored=false`
   entirely; live deletes re-grabbed 5 s and 4 s later, telling the user
   to press a button to download a season already reading `downloading`.
   There is no per-request opt-out — `skipRedownload` binds on the queue
   DELETE but is silently ignored on `POST /history/failed/{id}` — so
   the global flag is the only lever. If the guard cannot arm, ABORT:
   deleting without suppression is the defect. If the restore is
   defeated after 3 retries, the toast must SAY so — a stuck-off flag
   stops auto-retry of failed downloads for every SERIES on the box
   (Sonarr's `autoRedownloadFailed` is a Sonarr-only config field;
   Radarr has its own separate config and is unaffected), and no
   kiosk screen shows it
4. Mark the season's history records failed, which blocklists the
   release(s) so the same bad copy can't be the answer to the next
   search. **Grabbed records, with imported as a FALLBACK** — not
   both: probe P2 verified `POST /history/failed/{id}` against a
   GRABBED record only, so imported ids are used solely when there is
   no grab record at all (a manually imported release). Trying
   imported first would put the one scenario the fallback exists for
   — Sonarr refusing an imported id — in front of the grabbed ids the
   abort-on-refusal rule would then never reach
5. Purge the season's torrents from qBittorrent with their downloaded
   data (warn-and-continue — a torrent already gone doesn't block the
   rest)
6. Delete the season's episode files, from a fresh listing taken at
   this point — the only destructive step, and always last
7. Return to the season list; toast confirms the season was removed

**Abort rule**: any read or mutation in steps 1-4 that can't get an
authoritative answer aborts before step 6 runs and says why in a
toast — retry is always safe, since nothing destructive has happened
yet. Every abort message is composed by ONE lambda that reads both
cleanup counters, so a step-4 abort still discloses that step 3's
cancels (`removeFromClient=true`) already took their partial data,
and a step-6 abort still discloses step 5's torrent purge. The
success toast likewise names what did NOT happen: with no grabbed and
no imported history record, nothing was blocklisted and the same copy
can come back. A `std::exception` from either client is an abort like
any other (same disclosures, same stuck-OFF warning), never the screen's
generic "something went wrong". The whole sequence lives in
`src/media_browser/season_delete.{h,cpp}` (`run_delete_season` +
`compose_season_delete_toast`), renderer-free and unit-tested in
`tests/media_browser/test_season_delete.cpp`; the screen only spawns,
drains and paints.

**Re-downloading a deleted season**: SELECT on a season row with
nothing on disk and nothing in flight starts that season's download
(`start_season_download`, which monitors the season, re-monitors its
EPISODES — probe P3: season→episode does NOT cascade and SeasonSearch
skips unmonitored episodes — then searches). The season LIST is the
path that matters for a season the button does not propose. The
primary button targets `suggested_season(rows_, episode_watch_)`
(`season_choice.h`: the first season with nothing on disk or in flight
past the furthest one watched or on disk, else the lowest eligible) and
takes two presses — the first opens a `SeasonChooser`
("‹ Season 5 · ~22 GB ›", knob/D-pad steps, SELECT confirms, BTN4
cancels), so any eligible season, including a deleted season 3 above a
never-downloaded 2, is reachable. A show not yet in the library adds
Season 1 with `add_series(monitor=true)`; "Add Season N" with N>1 adds
with monitor `none` and, once the record settles, runs
`start_season_download(N)` — so Season 1 is never grabbed as a side
effect. `set_season_monitored(id, season, true)` also flips the
SERIES-level monitored flag: an unmonitored series has every release
rejected by Sonarr, so a monitor=none add would otherwise download
nothing, invisibly. The same
episode re-monitor runs in the whole-series worker
(`monitor_episodes_for_seasons` is shared by both); without it "Whole
series…" downloaded nothing for a previously deleted season while
reporting success. Whole-series Remove and `WatchStore` history
are untouched by this flow — origin: a Game of Thrones season grabbed
in the wrong language had no smaller unit to remove than the whole
series, which would have destroyed already-watched seasons too.

### Service operations

- **qbit-port-sync.timer** (systemd, on Pi host) — runs every 60s, syncs qBit's listen_port to Gluetun's NAT-PMP forwarded port. Without this, incoming peer connections fail when Gluetun reconnects. Tolerates `port=0` (NAT-PMP not currently leased) by leaving qBit unchanged. Also hosts the **drive-absent guard** (runs before the port fetch, every tick): while `/mnt/ssd` is not a mountpoint — or is mounted but qBit cannot read a token written on the drive through its `/downloads` bind (stale bind awaiting storage-attach) — it stops every *active* torrent on EVERY tick (the kiosk's `resume_all` at game/movie exit and boot recovery, and newly added downloads, would otherwise write into the SD card until full), records the hashes it stopped in `/var/lib/magic-dingus/drive_guard/stopped_hashes` (persistent — the old tmpfs marker was lost on reboot, stranding torrents stopped), and turns on qBit's add-stopped pref (`add_stopped_enabled` / 4.x `start_paused_enabled`, original saved alongside). Release starts exactly the recorded hashes and restores the pref; operator-stopped torrents are never started. The legacy `/tmp/mdb_drive_guard_paused` marker is folded in as an "ALL" sentinel.
- **Gluetun pin: v3.41.3** (`@sha256:fa19cc76…e027`, the multi-arch index digest; `tests/local/compose_image_pins.bats`). Bumped from v3.41.1 on 2026-10-05 after a live capture (18:00–20:15) showed every one of 9 consecutive full-stack restarts was PORT FORWARDING, not the tunnel: Proton's NAT-PMP gateway refused a renewal minutes after granting a port (`[port forwarding] adding port mapping: ... read udp 10.2.0.2:x->10.2.0.1:5351: recvfrom: connection refused`, once `external port changed: 60551 changed to 35802`), v3.41.1's loop logged `[port forwarding] starting` and never obtained a port again, the healthcheck's port clause (`/tmp/.pf_seen`) went unhealthy ~5 min later and the cascade watcher restarted gluetun + 5 dependents ~5 min after that — while in-tunnel traffic (ping 1.1.1.1 ~170 ms, WireGuard handshakes) worked throughout. v3.41.2 "no longer stuck after failed port forwarding" + v3.41.3's deadlock fix let gluetun re-acquire the port itself; the healthcheck's port requirement stays as the backstop. The watcher now records which kind each unhealthy was (`portfwd` = the healthcheck's DNS+TCP+TLS probe still passes inside the tunnel, `tunnel` = it does not) in the VPN event log (Box health above). `WIREGUARD_PERSISTENT_KEEPALIVE_INTERVAL=25s` (Go duration; Proton configs ask for 25, gluetun's default is 0 and it ignores the .conf line).
- **VPN server country** (2026-10): `VPN_COUNTRIES` in `services/.env` → `SERVER_COUNTRIES=${VPN_COUNTRIES-}` (`-` not `:-`: `custom` needs it empty). Content Manager → Media Browser → Advanced (`web/vpn_settings.py`, `static/vpn_settings.js`, one `register()` call in admin.py): `GET|POST /admin/media-browser/vpn-country`, curated list of `protonvpn` country names with >= 6 port-forwarding WireGuard servers in gluetun v3.41.x's list (Netherlands default). POST rewrites ONLY that line (atomic, mode kept), then runs `scripts/recreate_gluetun.sh` as a detached root job in the shared maintenance slot (kind `vpn-country`, single-flight with OTA/setup): `compose up -d --no-deps gluetun` under the compose lock, lock released before waiting for healthy so the cascade can re-link dependents. A failed launch restores the old line. Read-only for `custom` providers. The setup route keeps the current country on Reconfigure (it used to reset to Netherlands). Existing boxes are unchanged until someone picks a country.
- **Required Gluetun setup**: WireGuard config from ProtonVPN dashboard MUST have NAT-PMP toggle ON when generated. `FIREWALL_OUTBOUND_SUBNETS` MUST NOT include `10.0.0.0/8` (would block NAT-PMP routing to the VPN gateway at 10.2.0.1).
- **Active indexers** (Prowlarr → Radarr): TPB, YTS, LimeTorrents, TorrentDownload, Knaben (the latter two with `cloudflare` tag → Byparr, which replaces FlareSolverr for current Cloudflare challenge formats). Plus 5 pre-configured but disabled (Demonoid, EZTV, Internet Archive, Magnetz, Torrent Downloads) for future enable.
- **qBittorrent auth hardening** (Step 7.5 of `setup_services.sh`): the docker image's "bypass authentication for clients on localhost" preference is disabled programmatically and the WebUI password is set to a random value from `services/.env`. Without this, anything connecting from 127.0.0.1 (Radarr, the kiosk binary, anyone with shell) bypasses auth entirely. Step 7.6 mirrors the password to `MDB_QBIT_PASS=` in `.env` so the kiosk's QbittorrentClient (which reads that var via systemd EnvironmentFile=) keeps authenticating. It restarts the kiosk ONLY when the running kiosk's `MDB_QBIT_PASS` (from `/proc/<MainPID>/environ`; fallback: did this run change the line) is missing or stale — it used to restart on every run, killing a movie/game in progress on each Content Manager Reconfigure.
- **Gluetun DNS**: `DNS_SERVER=off` in docker-compose.yml (spelled `DOT=off` before 2026-10-04; gluetun v3.40+ deprecated that alias and maps it to `DNS_SERVER=off`) — switches Gluetun from DNS-over-TLS to plain UDP DNS (1.1.1.1). The DoT path maintains a long-lived TLS pipe to the upstream resolver; when that pipe stalls (observed ~daily on this Pi pre-fix), all DNS queries inside the netns time out for hours and Gluetun's internal healthcheck stays green because it queries a cached-IP endpoint that needs no DNS. Plain UDP is stateless — no pipe to wedge.
- **Gluetun healthcheck**: `wget https://one.one.one.one/cdn-cgi/trace | grep '^ip='` — actually exercises DNS + TCP + TLS through the tunnel. Cloudflare's official `one.one.one.one` hostname never blocks its own infrastructure (no rate-limit flapping). Failure of any layer → unhealthy. Replaces the older `localhost:8000/v1/publicip/ip` check which returned a cached value with no DNS lookup and missed the DNS-wedge state entirely.
- **gluetun-cascade-restart.service** (systemd, on Pi host) — long-running watcher subscribed to `docker events --filter container=mdb_gluetun --filter event=start --filter event=health_status`. Four roles:
  - **netns re-link**: On Gluetun `start` events, runs `docker compose restart` then `up -d` on the four netns-sharing dependents (Radarr/Prowlarr/qBit/Byparr) to refresh port-forwarding DNAT rules that get torn down with the old netns. The restart-then-up-d sequence handles both Gluetun-was-just-restarted (dependents still running but disconnected) and Gluetun-was-recreated (dependents crashed with exit 128, need to be brought back up) cases. **Playback-pause aware**: while `/tmp/mdb_playback_services_paused` exists (maintained by `playback_services_pause.sh` during games/movies), the cascade re-links qBittorrent ONLY and enforces `docker stop` on Radarr/Prowlarr/Byparr — pre-fix, the cascade's `up -d` revived the paused three mid-game and defeated the RAM-freeing pause on the 2 GB boxes (observed live 2026-07-31). The paused three re-link via the kiosk's unpause, which falls back to `compose up -d` when plain `docker start` fails (network_mode pins Gluetun's container ID at create time, so a Gluetun RECREATE mid-pause invalidates plain start).
  - **VPN event log** (2026-10): appends every transition to `/var/lib/magic-dingus/vpn_events.log`, probing in-tunnel traffic once per unhealthy event to tag it `portfwd` or `tunnel` (see Box health + diagnostics).
  - **Auto-recover from unhealthy**: On Gluetun `health_status: unhealthy` events, waits 5 minutes for confirmation (transient DNS blips self-recover), then if still unhealthy issues `docker restart mdb_gluetun`. The resulting `start` event re-enters the netns-relink branch above. Event-parsing uses `IFS= read -r line` + `event_action="${event_action// /}"` to normalize whitespace — Docker formats `health_status: healthy` (with space) which would otherwise miss the case-statement match.
  - **Dependent convergence** (2026-10): on every Gluetun `health_status: healthy` event and every `CONVERGE_INTERVAL_S` (300 s, background loop), `compose up -d` any dependent that is missing/created/exited/dead (e.g. left "Created" when `magic-dingus-services`' `TimeoutStartSec=300` cut a boot-time `compose up` short), and `docker restart` a running dependent whose OWN healthcheck has been unhealthy for `UNHEALTHY_CONFIRM_S` (first sighting stamps `/run/mdb-cascade/unhealthy_since.<svc>`, the next pass ≥5 min later restarts). Skipped while Gluetun is not healthy or `services/.env` is gone (Content Manager reset); paused-for-playback containers are never touched (qBittorrent still is). Pre-fix the healthy branch only logged, so removed/wedged containers stayed down until reboot. Decision function `dependent_action` is unit-tested in `tests/local/cascade_converge_decision.bats`.
- **Shared compose lock** `/run/lock/mdb-compose.lock` (fd 9, `flock`): every actor that starts/stops/recreates the stack's containers takes it — `gluetun_cascade_restart.sh` (cascade 120 s, gluetun restart 120 s, convergence 60 s then skips the round), `playback_services_pause.sh` (20 s, matching the kiosk's cross-worker bound), `storage_attach.sh` (120 s), `migrate_hardlink_layout.sh` (60 s, held from its stop until Radarr is back), `clear_radarr_cooldowns.py` (30 s), `recreate_gluetun.sh` (120 s, released before it waits for healthy). Waits are bounded and a timeout proceeds WITHOUT the lock (pre-lock behavior) — never a deadlock. The pause marker is re-checked INSIDE the lock; `playback_services_pause.sh pause` writes it BEFORE waiting so a lock holder honors it. A new container actor must take the same lock.
- **magic-dingus-clear-cooldowns.service** (systemd oneshot, on Pi host) — runs after `magic-dingus-services` on every boot. For Radarr AND Sonarr (same `IndexerStatus` shape; schema-checked, an unrecognized table is never written): stops the container, nulls out `IndexerStatus.DisabledTill / MostRecentFailure / InitialFailure` and resets `EscalationLevel`, starts it again — in a `finally`, and only if it was running (a paused-for-playback container is cleaned but left stopped). The *arrs persist per-indexer cooldowns up to 24 hours after consecutive failures; without this oneshot, a brief 3 AM network blip locks the indexer chain out until the next morning even though containers report healthy and smoke test passes. Idempotent — an app with no active cooldowns is not touched. Readiness waits share one 90 s deadline so the installed unit's `TimeoutSec=180` still covers the worst case. **Also run after every tunnel recovery** (2026-10-04): the cascade watcher's `health_status: healthy` branch launches the same helper (`clear_cooldowns_after_recovery`: 60 s settle, backgrounded, `flock`-single-flight) — a tunnel that blipped every ~10 min had left the indexers in cooldown for hours with only the boot-time clear.
- **magic-dingus-sync-qbit-password.service** (systemd oneshot, on Pi host) — runs after `magic-dingus-services` on every boot. Re-applies Step 7.5's qBit password sync logic: try login with `.env` value first (happy-path no-op), fall back to docker default `adminadmin`, then `setPreferences` to set the `.env` password and re-disable localhost-auth-bypass. Recovers from the drift state where `docker compose up -d` on a config change SIGKILL'd qBit before it flushed its pending password change to disk — the new container then comes up with the old persisted password and Radarr 401s. It also clears alt-speed limits left stuck ON by a kiosk crash mid-movie and re-pins the alt RATES to exactly the kiosk's `configure_alt_speed_limits` values (2 MiB/s down, **8 KiB/s up**, bytes) — it pinned 1 MiB/s up until 2026-10, and since it runs after the kiosk's startup call it silently overrode the trickle on every box (`qbit_password_fallbacks.bats` asserts both sides agree).
- **magic-dingus-storage-attach.service** (systemd oneshot, `WantedBy=mnt-ssd.mount`)
  — re-links the media containers when the movie drive is attached AFTER
  the Docker stack started. Radarr/qBittorrent bind SUBDIRECTORIES of
  `${STORAGE_ROOT}` (`/library`, `/downloads`) and Docker resolves a bind
  source once, at container start. If the stack came up with the drive
  unplugged, those binds point at empty placeholder dirs on the SD card;
  plugging the drive in later mounts it on the HOST but the running
  containers keep seeing the empty dirs — Radarr reports an empty library
  and imports fail with nothing in any log to explain it. **Mount
  propagation cannot fix this**: the mount event is at the PARENT
  (`/mnt/ssd`) while the binds are on its CHILDREN, and propagation
  carries events down into a bind, never up from above. The unit fires on
  every mount activation and is guarded to no-op unless genuinely stale
  (host library has entries AND the container sees none), so the ordinary
  drive-present boot costs one comparison. It uses explicit
  `compose rm -s -f` + `up -d`, **never `--force-recreate`**: that works
  by renaming the old container to a hash-prefixed name before removing
  it, and a leftover rename makes every subsequent recreate die on
  "Conflict. The container name ... is already in use" — observed on
  hardware, and it would have made the re-link fail permanently and
  silently. Orphaned renames are swept first so an affected box heals
  itself. It probes when EITHER Radarr or qBittorrent is running (Radarr
  alone skipped the re-link whenever the drive came back mid-movie, with
  Radarr paused and qBit still downloading onto the SD card), and under
  the playback pause re-creates Radarr/Sonarr with `compose create`
  (stopped) and starts only qBittorrent. `TimeoutStartSec=600` (was 180,
  which killed the script mid-`up` and left the containers removed): it
  must exceed lock wait 120 + `timeout 120 rm` + `timeout 300 up`.
- **magic-dingus-auto-blocklist.timer** (systemd, on Pi host) — runs every 15 minutes (OnBootSec=90s for cold-boot catch-up; TimeoutSec=300). Two failure classes, for BOTH Radarr and Sonarr: (1) `trackedDownloadStatus=warning` items whose `statusMessages` match known-bad signatures (executable extensions, "no videos in folder", "invalid video file", "unsupported extension", "sample file too large") — the scam-completion case; (2) dead-swarm stalls (2026-08-02 GoT case: TorrentDownload advertised 24 seeders on a 0-seed swarm; the stalled item then rejected all 213 live replacements with "Release in queue already meets cutoff", and neither *arr ever recovers because qBit stalls surface as WARNING, never FAILURE). Stall reap policy: errorMessage "stalled with no connections" + <=2% progress + grab >45 min old + the TWO-STRIKE rule — the same downloadId must be stall-condemned on two runs >=12 min apart with sizeleft unchanged, tracked in `/tmp/mdb_stall_candidates.json` (tmpfs — reboot resets the clock). One observation is never enough: qBit reports stalledDL during the healthy reconnection window after the kiosk's playback contention guard resumes torrents — at every game end on all boards, every movie end on Pi 4B, and on every boot (Pi 5 movie ends don't create this window: the trickle guard only rate-caps, never stops the swarm — see "Playback contention guard" above). Condemned items are DELETEd with `blocklist=true + removeFromClient=true + skipRedownload=true`, then an explicit `MoviesSearch` / per-season `SeasonSearch` fires (with an idempotence guard that skips when an equivalent Sonarr search is already in flight). Season packs are N queue rows sharing one downloadId — exactly one row is deleted per download (siblings 404 by design) while every condemned row's (series, season) is re-searched. Paused torrents are never touched (they carry no errorMessage at all). Missing SONARR_API_KEY skips the Sonarr pass (pre-Sonarr boxes).
- **magic-dingus-missing-search.timer** (systemd, on Pi host) — runs every 4 hours (OnBootSec=11min for cold-boot catch-up — it MUST exceed `missing_search.py`'s 10-minute boot deferral; at 3min the first run always deferred and the next came 4 h later, so boxes on <4 h/day never retried). POSTs `MissingMoviesSearch` to Radarr when any monitored movie has no file yet. Plugs the add-time-miss gap: "Add to Library" sets `addOptions.searchForMovie=true` so Radarr fires exactly ONE auto-search the instant a movie is added; if that single search comes up empty (good release not posted yet, indexer in transient cooldown, Byparr mid-Cloudflare-challenge), Radarr does not retry at a useful cadence (RSS sync only catches brand-new releases going forward). The title then sits with no download until the user manually opens the release picker. Observed live with "Wolfs (2024)" — the +50-scoring x264 YTS release the user later grabbed by hand simply wasn't available at the add-time search instant. This timer retries the missing backlog so those self-heal. Note the *selection* logic was already correct (x264 +50 preferred, HEVC/AV1/foreign/remux rejected by Custom Format scores below the `minFormatScore=-200` floor); the only gap was retry-on-empty. `missing_search.py` is idempotent — a run with zero missing titles is a no-op. As of 2026-08-02 it also runs a Sonarr pass: `MissingEpisodeSearch` (the library-wide missing-episode sweep, Sonarr's mirror of `MissingMoviesSearch`) closes the identical one-shot fragility for TV — add-time `searchForMissingEpisodes` and Start-Season-N's single `SeasonSearch` otherwise have no retry. Missing SONARR_API_KEY skips the pass.
- **Skip-when-unconfigured**: `magic-dingus-services.service` has `ConditionPathExists=/opt/magic_dingus_box/services/.env` so unprovisioned Pis cleanly skip the Docker stack instead of fail-looping. `setup_services.sh` is fully idempotent and rebuilds the entire stack from codified fixtures in `scripts/data/*.json` (Custom Formats, indexers, Byparr proxy, Apps integration, download client, quality definitions, qBit category) — fresh deploys reproduce the source Pi's exact configuration except for per-Pi secrets.
- **Pre-ship acceptance test**: `magic_dingus_box_cpp/scripts/verify_box.sh` — the single
  "is this box shippable?" command. Read-only, ~30s, runs on the Pi,
  exits 0/1. Covers what `verify_services.sh` does not: platform
  detection, clock/thermal/throttle state, display mode vs. the
  persisted setting, the **logical-canvas canary**
  (`set_content_viewport(960, 720)` — if this reads 1440x1080 the UI is
  mis-scaled at 1080p), refresh rate (a 24/30Hz timing clamps the whole
  kiosk), playlist + ROM path resolution (both resolution bases),
  libretro cores actually dlopen-ing at API v1, BIOS presence, kiosk
  service/status freshness, `now_playing` population, storage, and
  container count. `--with-services` chains the service smoke test for a
  full sweep. Every check in it corresponds to a bug that was once found
  by hand.
- **Smoke test**: `magic_dingus_box_cpp/scripts/verify_services.sh` is the hard-assertion health check for the entire stack (indexers, qBit download client wired, quality profile state, Custom Formats including the scam-rejection ones, qBit auth, MDB_QBIT_PASS in .env, Radarr AND Sonarr root folders, no active indexer cooldowns, live indexer search ≥10 results — 15 assertions on a fully provisioned box; the Sonarr checks skip on pre-Sonarr boxes). Runs once at the end of `setup_services.sh` and weekly thereafter via `magic-dingus-smoke-test.timer` (Mon 03:14 with Persistent=true). Failures land in `journalctl -u magic-dingus-smoke-test`. Re-runnable standalone: exits 0/1 for CI/manual debugging. Every curl carries `--max-time` (15 s; 120 s for the live Prowlarr search, which waits on its slowest indexer/Byparr solve) and the unit has `TimeoutStartSec=300` — a wedged *arr accepts the socket and never answers, which used to hang the run (and `setup_services.sh`'s final step) forever. `tests/local/curl_time_bounds.bats` enforces the bound.

### Feature gating

The Media Browser is **VPN-required and hidden by default**, gated
by three independent layers:

1. **Unlocked** — `playback.media_browser_unlocked` flag in
   `config/settings.json`, set by the kiosk-side secret sequence
   (BTN1+BTN3 chord → BTN2 × 3 → rotary click). Gates UI
   *visibility*: when locked, the Settings-menu entry and the web
   admin tab are hidden entirely.
2. **VPN configured** — `WIREGUARD_PRIVATE_KEY` non-empty in
   `services/.env`. Gates *functional* `/admin/media-browser/*`
   endpoints and the kiosk's MB launch path. The Content Manager
   tab is visible at Layer 1 alone (so the operator can drop a
   WireGuard config); the inner functions require Layer 2.
3. **Tunnel healthy** — Radarr `/ping` reachable on
   `localhost:7878`. Polled every 10s by the kiosk's
   `VpnHealthMonitor`; three consecutive failures (~30s) flips the
   in-memory flag and hides MB entries with a "tunnel down" toast.
   Recovery is silent on the first successful poll.

All four torrent-ecosystem services (Prowlarr, Radarr, Byparr,
qBittorrent) share Gluetun's network namespace. When Gluetun is
down, all four are unreachable from the host — Radarr ping is the
single signal that covers the stack.

Cloned Pis start LOCKED — `first_boot.sh` Step 6 resets the unlock
flag during first-boot setup so a fresh Pi inherits no unlock
state from the source.

**Privacy gap (accepted):** the kiosk binary's own TMDB calls exit
via the host network, not via Gluetun, because the C++ binary runs
outside Docker. Metadata only — never touches torrent indexers. See
[MEDIA_BROWSER_VPN_SETUP.md](magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md)
"Privacy notes" for the full threat model.

### Per-Pi setup workflow (no SSH required)

1. Operator opens Content Manager (`http://magicpi-XXXX.local:5000`)
2. Enters secret sequence on the kiosk to unlock Media Browser visibility
3. Refreshes Content Manager → "Media Browser" tab appears
4. Drops in WireGuard `.conf` from ProtonVPN dashboard (NAT-PMP enabled)
5. Backend writes `services/.env`, runs `setup_services.sh` in background, frontend polls progress
6. ~90 seconds later: services healthy, Custom Formats + indexers + integrations all configured

### Golden image — what actually breaks, and the two gates

Hard-won on 2026-08-04, when the first card ever booted from a golden
image failed to reach the kiosk. Read this before touching the clone path.

**Verify the ARTIFACT, never the filesystem.** The scrub used to check
"is the secret file gone?" on the live box and call that clean. A deleted
file is absent from the filesystem and fully present in the image — which
is how a `cloud-init.log` holding the operator's Wi-Fi PSK shipped in
v1.9.3 and passed its own audit. `scan_image_for_secrets.sh` reads the
finished `.img.gz` and is the only credential check that counts;
`clone_live_sd.sh` runs it automatically and refuses to report success on
a hit. Five distinct leak classes were found this way, each needing a
different fix:

| Class | Example found | Why the earlier scrub missed it |
|---|---|---|
| Deleted data in free space | `cloud-init.log` PSK | fill never reached ext4's root reserve (2.4 GB `df` hides) |
| Live files not on the list | poster cache, kiosk log, phone-remote state | list was incomplete |
| Infrastructure metadata | Docker `config.v2.json`, containerd `meta.db` | container env holds `.env` values; `/var/lib/docker` was never inspected |
| Filename as secret | `<SSID>.nmconnection` | deleted names persist in the directory's ALLOCATED block |
| Freed blocks in the fill margin | Prowlarr API key | a bounded fill leaves extents the allocator picks, never written |

Corollaries: stop `dockerd` AND `containerd` before zeroing (a running
daemon rewrites metadata *after* the fill, stranding stale copies in free
space), and drop the ext4 root reserve with `tune2fs -r 0` for the fill,
restoring it via a trap on `EXIT INT TERM HUP` — **HUP matters**, because
the script runs under ssh and a dropped link otherwise skips the trap.

**Boot-test every image on a card that is NOT the source card's size.**
The credential gate says nothing about whether a unit boots. `first_boot.sh`
expands the root partition only when the card has >100 MB of unused tail,
so the source card always skips it and the step was never exercised —
while `parted -s` PROMPTS on a mounted partition, answers *No* in script
mode, and exits 1. Under `set -e` that killed first boot at Step 2 of 7,
so no cloned unit ever regenerated its identity, wiped saves, re-locked
Media Browser or disabled the first-boot service. It kept the source
hostname and collided with the source box on mDNS/DHCP. Now uses
`growpart`, and expansion can no longer abort the boot — a unit that
wastes the tail of its card is an annoyance, one that skips the
credential wipes is a defective product.

`first_boot.sh` writes `/var/log/magic-first-boot.log` and traps ERR with
the failing line number. Both exist because the original failure produced
an EMPTY journal (Step 6c-2 wipes it by design), leaving nothing to
diagnose from.

**`x-systemd.automount` will hang a boot with no drive.** An automount unit
is started at boot even with `noauto` — that is its purpose — and with no
device behind it the boot stalls until the hardware watchdog resets the
board at 60 s, which reads as a reboot loop. `nofail` does not help: it
governs the MOUNT, not the automount. Measured on hardware 2026-08-04:

| `/etc/fstab` options for LABEL=MOVIES | Result |
|---|---|
| `defaults,nofail,x-systemd.automount,device-timeout=5` | never completes |
| `noauto,nofail,x-systemd.automount,device-timeout=5` | never completes |
| line removed entirely | boots 17.1 s |
| **`noauto,nofail` + `udev/99-magic-movies-mount.rules`** | **boots 16.7 s, and a drive still auto-mounts** |

The drive is mounted by a udev rule on device appearance instead. With no
drive the rule never fires, so it cannot delay boot; with a drive it fires
at boot AND on hotplug, and `magic-dingus-storage-attach.service` still
runs via its `WantedBy=mnt-ssd.mount`. Both directions are verified —
attached: mounts, 0 failed units; unattached: rule fires 0 times, 0 failed
units. **Any optional/removable mount added later must follow this
pattern**, because most units never have a drive attached at first boot.

**Inherited source-box state is its own bug class.** Anything true only
because the source box is the source box will ship: the `LABEL=MOVIES`
fstab entry (a unit with no drive must still boot — `noauto,nofail` plus
the udev rule, never an automount), the operator's playlists and uploaded
videos (`SHIP_PLAYLISTS` curates these), and the clone-in-progress marker.
Guards that skip when a key is merely *present* rather than *correct* are
the trap: `if ! grep -q LABEL=MOVIES /etc/fstab` never repaired a stale
line, so a box provisioned before this pattern existed kept the blocking
entry forever — and that is not hypothetical, the source box was still
carrying the automount line on 2026-08-05, one step short of baking the
boot hang into another image.

**Run `scripts/golden_image/sync_source_box.sh --pi magic@<host>` before
every clone.** The out-of-box fixes live in eight different files and
missing any one of them reproduces its bug on every unit sold; a checklist
is how you miss one at 1am. It pushes each file, verifies it by content
hash, normalises the fstab line, reloads udev, rebuilds the kiosk, and
asserts the rebuilt BINARY contains the change — `make` exiting 0 proves
nothing when a stale object or a skipped rebuild exits 0 too. `--pi` is
never defaulted, because `deploy_cpp.sh` defaults to a different box and
two Pis are usually reachable at once; it prints the target's hostname
before touching anything.

**Two more out-of-box classes worth naming**, both invisible on a source
box that has been running for weeks: a *provisioning step that was never
written* (Radarr had no root folder, so the customer's first download died
with "no root folder configured" — Sonarr's equivalent existed, and no
health check covered either until `check_radarr_root_folder()`), and *a
device that only exists after the customer sets it up* (a paired phone
remote registers as a joypad and could claim index 0, the only index
RetroArch accepts for player 1, so every per-core controller mapping
landed on a port with no pad — `controller_detector.cpp` now skips it
during port assignment while keeping it as a UI input).

**One WireGuard key per box — the source's key must never run on a unit**
(2026-10). Two VPN clients on one key knock each other off ProtonVPN in
bursts, on both boxes, invisibly. Units inherit the source's key through
`services/.env` (the 2026-08-04 image's first boot died before its wipe)
AND through Docker's `config.v2.json`, from which `restart: unless-stopped`
starts gluetun with or without `.env`. `scripts/golden_image/source_secrets_lib.sh`
holds the defence: `first_boot.sh` Step 1c deletes `.env` and removes every
inherited container right after the identity reset (before the Step 2 expand;
never fatal — `docker rm -f` when dockerd is up, the container's state dir
when it is down); `prepare_for_cloning.sh` Step 2a records salted
fingerprints (never values) of every secret-named `.env` value + the Flask
secret into `/etc/magic-dingus/source_secret_fingerprints` in the image, and
its leak check refuses a leftover `config.v2.json`; `verify_box.sh` FAILs a
unit whose `.env` or any container (running or stopped) holds one. The SOURCE
never flags itself: `restore_after_cloning.sh` deletes the file there, and
the file carries a salted hash of the source BOARD's SoC serial
(`/proc/device-tree/serial-number`, which no dd copies) that `verify_box.sh`
recognises. Secret-ness is by key NAME (`MDB_FP_SECRET_KEY_RE`), so a new
credential is covered automatically — but a key whose value is legitimately
identical on every box must never match that regex, or every clean unit
fails. Old units / images: CLONING.md "One VPN key per box"
(`source_secrets_lib.sh record -` piped to a unit;
`scan_image_for_secrets.sh --vpn-key`).

### Live SD cloning

`scripts/golden_image/clone_live_sd.sh` clones a running Pi's SD card to a `.img.gz` over SSH without removing the SD physically. Three Pi-side scripts (`prepare_for_cloning.sh`, `restore_after_cloning.sh`, `first_boot.sh` Step 6) handle prepare/restore + per-Pi state cleanup on the cloned image. Source Pi loses no data; total downtime ~1 minute. See `scripts/golden_image/CLONING.md` for full operator workflow.

## Phone Remote

A web-based remote control hosted by the Flask web admin, accessed at `/admin/remote`. Phones pair once via a QR code shown in the kiosk's Settings menu; thereafter the page is HMAC-cookie-authenticated and reconnects automatically. Two input surfaces:

### D-pad (button input)

- Phone JS sends `{t: "press", btn: ..., phase: ...}` over WebSocket
- Flask's `UinputWriter` (`web/remote/uinput_writer.py`) translates to evdev events on a `/dev/uinput` virtual gamepad
- Kiosk's `InputManager` opens the gamepad like any other controller (named `MagicDingus Phone Remote`); button codes are picked to match `map_button_to_action` so presses route directly to existing `InputAction` values (SELECT, SETTINGS_MENU, PREV/NEXT/PLAY_PAUSE for the colored buttons)

### Text input (typing into kiosk text fields from phone OS keyboard)

For typing into the MB Search field or the Wi-Fi password keyboard, phones use the native iOS/Android keyboard instead of D-pad-driving the on-screen keyboard. Auto-detected — when the kiosk's status broadcast reports `text_input.active=true`, the phone swaps from D-pad mode to a text-input section.

End-to-end flow:
- Phone JS `<input>` `input` event → `syncToKiosk(newVal, oldVal)` computes a diff and sends per-keystroke `{t: "type_char", c}` / `{t: "key_special", k: "backspace"}` over WS (paste / multi-delete falls back to `{t: "clear"}` + retype)
- Flask `ws_handler` filters non-ASCII, routes to `TextInputWriter` (`web/remote/text_input_writer.py`)
- `TextInputWriter` appends a JSONL event to `data/text_input_queue.jsonl` under `flock(LOCK_EX)` with per-device 50/sec rate limit
- Kiosk's main loop calls `Controller::poll_text_input_queue(state)` each frame: opens the queue file, acquires `flock`, reads via the locked fd, dispatches each event to `state.active_text_keyboard`'s `type_char` / `backspace` / `clear_buffer` / `commit` methods, truncates under the same lock
- `state.active_text_keyboard` is refreshed once per frame at the top of main.cpp's render loop — it points to whichever `VirtualKeyboard` is `is_active()` (MB Search's or the kiosk's main one for Wi-Fi)
- `SearchScreen::update()` polls `keyboard_.get_text()` against its own `query_` so externally-driven buffer changes (from the queue drainer) trigger the existing 400ms search debouncer

The phone's OS-keyboard "Search/Enter" key only dismisses the OS keyboard via `inputEl.blur()` — it does NOT send a commit/close to the kiosk. Search is debounced live; pressing Enter has no separate "submit" semantic. The text field stays visible on phone (re-tap to reopen), and the kiosk's on-screen keyboard stays alive so D-pad navigation still works for selecting results or further editing.

`text_input_queue.jsonl` is excluded from `deploy_cpp.sh`'s rsync so in-flight events aren't lost across deploys.

### Pairing flow

`Settings → Connect a Device` on the kiosk (the single connection entry —
it replaced the two-row "Content Manager" / "Connect Phone / Computer"
split) shows a QR code, a 6-digit code, the box's typed address, and the
USB-C cable hint in plain text (so a failed scan is recoverable).
**The QR target is built at runtime from the box's real address**
(`ui/pairing_url.h`), preferring the LAN IP over `<hostname>.local` —
mDNS is the fragile link (inconsistent Android support, routers that
block multicast) while a literal IP works for any phone on the subnet,
and a code lives ~2 min so the IP cannot go stale inside that window.
This was hardcoded to `http://magicpi.local:5000/...` until 2026-07-26,
which resolved on no box except one literally named `magicpi` — and
`first_boot.sh` names every clone `magicpi-XXXX`, so pairing was broken
on **every shipped unit**, silently: the phone could not resolve the
host, so no request ever reached the server and `pairing_audit.log`
stayed empty with nothing to diagnose from. If pairing ever fails again,
check that log first — entries mean the phone reached the box (a code or
auth problem); no entries mean it never arrived (address or network). Phone scans → opens `/connect?code=NNNNNN` (the Connect a Device landing page, which hands the code to `/?pair=`) → backend writes a paired-device record + sets HMAC-signed cookie. Pairings persist in `data/paired_remotes.json` (excluded from deploy rsync). The Flask process's HMAC secret lives in `data/flask_secret.key` (also excluded — wiping it would invalidate all paired phones).

## Additional Documentation

Docs live in `magic_dingus_box_cpp/docs/`. A `.gitignore` rule matches that directory, so most files there exist only in some local checkouts; `git ls-files magic_dingus_box_cpp/docs/` lists the ~20 that were committed before the rule and ARE tracked (edits to them commit and ship). Tracked and current enough to rely on:
- `DEPLOYMENT_GUIDE.md` - Deploy workflow (its Pi 4/Bookworm framing predates the Trixie floor and Pi 5 units)
- `MEDIA_BROWSER_*` - Media Browser design, service setup, VPN setup, user guide
- `USB_CONNECTION_GUIDE.md` - USB Ethernet Gadget setup
- `superpowers/` (both under `magic_dingus_box_cpp/docs/` and the root `docs/`) - dated specs and implementation plans per feature

Local-only (not in git; may be stale): `PLAYLIST_FORMAT.md`, `DISPLAY_MODES_USAGE.md`, `GAME_CONTROLS.md`, `DATA_SYNC_GUIDE.md`, `WEB_UI_GUIDE.md`. When they disagree with code, the code wins.
