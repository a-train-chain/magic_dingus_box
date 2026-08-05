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
   `scripts/install_deps.sh` (on-Pi builds), the apt list in
   `.github/workflows/release.yml` (the CI release binary), and the
   README dependency list. An OPTIONAL CMake dep that silently changes
   runtime behavior is a trap — libsystemd being absent in CI compiled
   out sd_notify and made systemd kill a perfectly healthy binary on
   every box (caught live, v1.7.2). If a capability is load-bearing,
   either make the dep REQUIRED or add a release-blocking `strings`
   assertion to the workflow next to the existing three (aarch64,
   READY=1, prowlarr).
5. **`config.txt` model-specific settings live under `[pi4]` / `[pi5]`
   conditional sections, never `[all]`.** Current split: `[pi5]` has
   `v3d_freq=1000` + `kernel=kernel8.img` (4 KB pages — flycast dies
   without it); `[pi4]` has `gpu_mem=76`.
6. **OS floor is Trixie (Debian 13) on both boards** — libgpiod 2.x API
   and the CI binary's glibc. Anything older can neither build nor run
   the kiosk (v1.6.4 was the last Bookworm release).
7. **Before a release that touches the platform layer**, run the Mac
   suites (all 8), and validate on real hardware of BOTH boards when the
   change plausibly differs between them — the sd_notify failure was
   invisible in every off-Pi test.

Full background: `scripts/golden_image/CLONING.md` "One image, two
boards" and `OTA_UPDATE_GUARANTEES.md`.

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
  - `renderer` - Immediate-mode 2D renderer (quads, text, alpha blending)
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
2. Fork RetroArch process with generated config and per-core controller mapping
3. Block on waitpid() until RetroArch exits
4. Re-acquire DRM master (5 retries) → Re-init input (3 retries) → Restore EGL context → Rebuild GL resources

### Audio System

- PulseAudio for routing (HDMI/Headphone/Auto selection)
- `init_audio.sh` configures PulseAudio default sink BEFORE app starts
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

See `magic_dingus_box_cpp/docs/PLAYLIST_FORMAT.md` for full schema reference.

## Controls

### Main UI
- **DPad/Axis X**: Navigate playlists
- **A/Enter/Space**: Select playlist item
- **Z**: Play/Pause
- **L/R Triggers**: Seek ±10s
- **C-Stick**: Seek ±5s
- **Rotary Encoder**: Velocity-sensitive video seeking with progress bar
- **B**: Settings menu
- **Q/Esc**: Quit

### In RetroArch
- Per-core button mappings live in `controller_mapping.cpp` (semantic tables) combined with a pad's `PhysicalProfile`; player 1 and player 2 resolve independently from whichever pad is on each port
- **Exit a game — direct quit, no RetroArch menu** (owner decision 2026-08-03; the RA menu "isn't needed at all" on a kiosk): N64-style pads = hold **Z + press Start**; PS-style pads = hold **Select + press Start**. Bound from PHYSICAL controls in the style preambles (`SemanticMapping::exit_emulator`) so per-core RetroPad slot remaps can never move the gesture — the mupen table repurposes the select SLOT for N64 L, which is exactly how the old start+select combo silently became Start+L1 in N64 games. RetroArch honors these hotkeys from the PLAYER 1 pad only (accepted limitation with two pads). Auto-save-on-exit makes direct quit safe.
- Auto-save state on exit, auto-load on start

## RetroArch Cores

10 cores installed via `--cores` flag (`scripts/install_cores.sh`). OTA also self-heals cores: `update.sh` scans the box's live playlists for referenced `emulator_core` values and runs `install_cores.sh` if any `.so` is missing from the runtime cores dir.

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
- Triggered via web admin `/admin/update/*` endpoints (`version`, `check`, `install`, `status/<job_id>`, `rollback`) — NOT `/api/update/*`

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
episode totals.

### Playback hardware notes

- **Pi 4**: `v4l2h264dec` (hardware H.264) is rank-promoted and used by default
- **Pi 5**: has NO hardware H.264 decoder (BCM2712 dropped the block); the rank promotions are no-ops there and playbin falls through to `avdec_h264` (libav software, ~20% CPU for 1080p on the A76s — validate headroom on the 2GB board)
- `v4l2slh265dec` (V4L2 stateless hardware HEVC, both boards) is **disabled** — SAND pixel-format negotiation bug with GStreamer 1.22-era Bookworm; falls back to `avdec_h265` (software, ~30-50% of one core for 1080p 8-bit Main profile). NOTE (2026-07-22): production Pis actually run **Trixie** with GStreamer 1.26, where the SAND fix landed — the disable is now conservative and re-testable (see CLONING.md "HEVC experiment")
- AV1 has no hardware decoder; software-decode at 1080p+ is unwatchable
- Required system package: `gstreamer1.0-libav` (codified in `scripts/install_deps.sh`)

### Playback contention guard (torrents vs. the video pipeline) — per-board split

Torrent piece-writes contend with GStreamer's reads on the library
medium, so playback quiets qBittorrent — but HOW is per-board
(`PlatformProfile::trickle_torrents_during_video`, branch in
`PlaybackScreen::enter()/leave()`):

- **Pi 5 movies: trickle, not pause.** Playback engages qBit's
  *alternative speed limits* (~2 MiB/s (bytes-unit field — see qbittorrent_client.h) down / 256 KiB/s up) instead of
  `pause_all()` — the SSD library and spare CPU absorb a trickle, so
  downloads keep progressing through a 2-hour film instead of the swarm
  being stopped. qBit 5.x has no explicit-set endpoint for the mode
  (verified live on 5.0.3: GET `/transfer/speedLimitsMode` returns
  "0"/"1", POST `/transfer/toggleSpeedLimitsMode` flips), so
  `QbittorrentClient::set_alt_speed_limits_enabled()` reads first and
  toggles only on mismatch (idempotent), and only reports success when
  the re-read final state matches the request.
- **Pi 4B (and Unknown boards) movies: full `pause_all()`**, unchanged —
  USB-flash media has no random-IO headroom to trickle into.
- **Games: full `pause_all()` on EVERY board** (`GameQuietMode` in
  main.cpp) — games need the CPU/RAM back, not just disk quiet. The
  trickle flag is movie-scoped by design.
- **Startup clears the cap (crash recovery):** main.cpp's MB init calls
  `set_alt_speed_limits_enabled(false)` unconditionally and re-applies
  the trickle rates via `configure_alt_speed_limits(1536, 256)`
  (preferences `alt_dl_limit`/`alt_up_limit`, KiB/s) — a kiosk crash
  mid-movie must never leave downloads silently capped, and shipped
  boxes converge on retuned rates via OTA. Both best-effort: qBit may
  still be down at kiosk start; failures log and never block startup.
- `PlaybackScreen::leave()` clears only what enter() set
  (`qbit_alt_limited_by_us_` / `qbit_was_paused_by_us_` are the consent
  records) — an operator's own alt-limits or manual pauses are never
  flipped.

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

### Service operations

- **qbit-port-sync.timer** (systemd, on Pi host) — runs every 60s, syncs qBit's listen_port to Gluetun's NAT-PMP forwarded port. Without this, incoming peer connections fail when Gluetun reconnects. Tolerates `port=0` (NAT-PMP not currently leased) by leaving qBit unchanged.
- **Required Gluetun setup**: WireGuard config from ProtonVPN dashboard MUST have NAT-PMP toggle ON when generated. `FIREWALL_OUTBOUND_SUBNETS` MUST NOT include `10.0.0.0/8` (would block NAT-PMP routing to the VPN gateway at 10.2.0.1).
- **Active indexers** (Prowlarr → Radarr): TPB, YTS, LimeTorrents, TorrentDownload, Knaben (the latter two with `cloudflare` tag → Byparr, which replaces FlareSolverr for current Cloudflare challenge formats). Plus 5 pre-configured but disabled (Demonoid, EZTV, Internet Archive, Magnetz, Torrent Downloads) for future enable.
- **qBittorrent auth hardening** (Step 7.5 of `setup_services.sh`): the docker image's "bypass authentication for clients on localhost" preference is disabled programmatically and the WebUI password is set to a random value from `services/.env`. Without this, anything connecting from 127.0.0.1 (Radarr, the kiosk binary, anyone with shell) bypasses auth entirely. Step 7.6 mirrors the password to `MDB_QBIT_PASS=` in `.env` so the kiosk's QbittorrentClient (which reads that var via systemd EnvironmentFile=) keeps authenticating.
- **Gluetun DNS**: `DOT=off` in docker-compose.yml — switches Gluetun from DNS-over-TLS to plain UDP DNS (1.1.1.1). The DoT path maintains a long-lived TLS pipe to the upstream resolver; when that pipe stalls (observed ~daily on this Pi pre-fix), all DNS queries inside the netns time out for hours and Gluetun's internal healthcheck stays green because it queries a cached-IP endpoint that needs no DNS. Plain UDP is stateless — no pipe to wedge.
- **Gluetun healthcheck**: `wget https://one.one.one.one/cdn-cgi/trace | grep '^ip='` — actually exercises DNS + TCP + TLS through the tunnel. Cloudflare's official `one.one.one.one` hostname never blocks its own infrastructure (no rate-limit flapping). Failure of any layer → unhealthy. Replaces the older `localhost:8000/v1/publicip/ip` check which returned a cached value with no DNS lookup and missed the DNS-wedge state entirely.
- **gluetun-cascade-restart.service** (systemd, on Pi host) — long-running watcher subscribed to `docker events --filter container=mdb_gluetun --filter event=start --filter event=health_status`. Two roles:
  - **netns re-link**: On Gluetun `start` events, runs `docker compose restart` then `up -d` on the four netns-sharing dependents (Radarr/Prowlarr/qBit/Byparr) to refresh port-forwarding DNAT rules that get torn down with the old netns. The restart-then-up-d sequence handles both Gluetun-was-just-restarted (dependents still running but disconnected) and Gluetun-was-recreated (dependents crashed with exit 128, need to be brought back up) cases. **Playback-pause aware**: while `/tmp/mdb_playback_services_paused` exists (maintained by `playback_services_pause.sh` during games/movies), the cascade re-links qBittorrent ONLY and enforces `docker stop` on Radarr/Prowlarr/Byparr — pre-fix, the cascade's `up -d` revived the paused three mid-game and defeated the RAM-freeing pause on the 2 GB boxes (observed live 2026-07-31). The paused three re-link via the kiosk's unpause, which falls back to `compose up -d` when plain `docker start` fails (network_mode pins Gluetun's container ID at create time, so a Gluetun RECREATE mid-pause invalidates plain start).
  - **Auto-recover from unhealthy**: On Gluetun `health_status: unhealthy` events, waits 5 minutes for confirmation (transient DNS blips self-recover), then if still unhealthy issues `docker restart mdb_gluetun`. The resulting `start` event re-enters the netns-relink branch above. Event-parsing uses `IFS= read -r line` + `event_action="${event_action// /}"` to normalize whitespace — Docker formats `health_status: healthy` (with space) which would otherwise miss the case-statement match.
- **magic-dingus-clear-cooldowns.service** (systemd oneshot, on Pi host) — runs after `magic-dingus-services` on every boot. Stops Radarr, nulls out `IndexerStatus.DisabledTill / MostRecentFailure / InitialFailure` and resets `EscalationLevel` in `radarr.db`, restarts Radarr. Radarr persists per-indexer cooldowns up to 24 hours after consecutive failures; without this oneshot, a brief 3 AM network blip locks the indexer chain out until the next morning even though containers report healthy and smoke test passes. Idempotent — if no cooldowns are active the UPDATE affects 0 rows.
- **magic-dingus-sync-qbit-password.service** (systemd oneshot, on Pi host) — runs after `magic-dingus-services` on every boot. Re-applies Step 7.5's qBit password sync logic: try login with `.env` value first (happy-path no-op), fall back to docker default `adminadmin`, then `setPreferences` to set the `.env` password and re-disable localhost-auth-bypass. Recovers from the drift state where `docker compose up -d` on a config change SIGKILL'd qBit before it flushed its pending password change to disk — the new container then comes up with the old persisted password and Radarr 401s.
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
  itself.
- **magic-dingus-auto-blocklist.timer** (systemd, on Pi host) — runs every 15 minutes (OnBootSec=90s for cold-boot catch-up; TimeoutSec=300). Two failure classes, for BOTH Radarr and Sonarr: (1) `trackedDownloadStatus=warning` items whose `statusMessages` match known-bad signatures (executable extensions, "no videos in folder", "invalid video file", "unsupported extension", "sample file too large") — the scam-completion case; (2) dead-swarm stalls (2026-08-02 GoT case: TorrentDownload advertised 24 seeders on a 0-seed swarm; the stalled item then rejected all 213 live replacements with "Release in queue already meets cutoff", and neither *arr ever recovers because qBit stalls surface as WARNING, never FAILURE). Stall reap policy: errorMessage "stalled with no connections" + <=2% progress + grab >45 min old + the TWO-STRIKE rule — the same downloadId must be stall-condemned on two runs >=12 min apart with sizeleft unchanged, tracked in `/tmp/mdb_stall_candidates.json` (tmpfs — reboot resets the clock). One observation is never enough: qBit reports stalledDL during the healthy reconnection window after the kiosk's playback contention guard resumes torrents — at every game end on all boards, every movie end on Pi 4B, and on every boot (Pi 5 movie ends don't create this window: the trickle guard only rate-caps, never stops the swarm — see "Playback contention guard" above). Condemned items are DELETEd with `blocklist=true + removeFromClient=true + skipRedownload=true`, then an explicit `MoviesSearch` / per-season `SeasonSearch` fires (with an idempotence guard that skips when an equivalent Sonarr search is already in flight). Season packs are N queue rows sharing one downloadId — exactly one row is deleted per download (siblings 404 by design) while every condemned row's (series, season) is re-searched. Paused torrents are never touched (they carry no errorMessage at all). Missing SONARR_API_KEY skips the Sonarr pass (pre-Sonarr boxes).
- **magic-dingus-missing-search.timer** (systemd, on Pi host) — runs every 4 hours (OnBootSec=3min for cold-boot catch-up). POSTs `MissingMoviesSearch` to Radarr when any monitored movie has no file yet. Plugs the add-time-miss gap: "Add to Library" sets `addOptions.searchForMovie=true` so Radarr fires exactly ONE auto-search the instant a movie is added; if that single search comes up empty (good release not posted yet, indexer in transient cooldown, Byparr mid-Cloudflare-challenge), Radarr does not retry at a useful cadence (RSS sync only catches brand-new releases going forward). The title then sits with no download until the user manually opens the release picker. Observed live with "Wolfs (2024)" — the +50-scoring x264 YTS release the user later grabbed by hand simply wasn't available at the add-time search instant. This timer retries the missing backlog so those self-heal. Note the *selection* logic was already correct (x264 +50 preferred, HEVC/AV1/foreign/remux rejected by Custom Format scores below the `minFormatScore=-200` floor); the only gap was retry-on-empty. `missing_search.py` is idempotent — a run with zero missing titles is a no-op. As of 2026-08-02 it also runs a Sonarr pass: `MissingEpisodeSearch` (the library-wide missing-episode sweep, Sonarr's mirror of `MissingMoviesSearch`) closes the identical one-shot fragility for TV — add-time `searchForMissingEpisodes` and Start-Season-N's single `SeasonSearch` otherwise have no retry. Missing SONARR_API_KEY skips the pass.
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
- **Smoke test**: `scripts/verify_services.sh` is the hard-assertion health check for the entire stack (5 expected indexers, qBit download client wired, quality profile state, 8 Custom Formats including 2 scam-rejection ones, qBit auth, MDB_QBIT_PASS in .env, no active indexer cooldowns, live indexer search ≥10 results — 9 checks total). Runs once at the end of `setup_services.sh` and weekly thereafter via `magic-dingus-smoke-test.timer` (Mon 03:14 with Persistent=true). Failures land in `journalctl -u magic-dingus-smoke-test`. Re-runnable standalone: exits 0/1 for CI/manual debugging.

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

**Inherited source-box state is its own bug class.** Anything true only
because the source box is the source box will ship: the `LABEL=MOVIES`
fstab entry (a unit with no drive must still boot — `nofail` +
`x-systemd.automount`), the operator's playlists and uploaded videos
(`SHIP_PLAYLISTS` curates these), and the clone-in-progress marker.
Guards that skip when a key is merely *present* rather than *correct* are
the trap: `if ! grep -q LABEL=MOVIES /etc/fstab` never repaired a stale
line, so a box provisioned before `nofail` existed kept the blocking
entry forever.

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

`Settings → Phone Remote` on the kiosk shows a QR code, a 6-digit code,
and the box's address in plain text (so a failed scan is recoverable).
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
auth problem); no entries mean it never arrived (address or network). Phone scans → opens `/pair?code=NNNNNN` → backend writes a paired-device record + sets HMAC-signed cookie. Pairings persist in `data/paired_remotes.json` (excluded from deploy rsync). The Flask process's HMAC secret lives in `data/flask_secret.key` (also excluded — wiping it would invalidate all paired phones).

## Additional Documentation

Extensive docs in `magic_dingus_box_cpp/docs/` (note: `docs/` is gitignored — these live on the Pi locally and are reference material, not tracked in the repo):
- `ARCHITECTURE.md` - System design
- `DISPLAY_MODES_USAGE.md` - CRT/Modern TV modes
- `RETROARCH_INTEGRATION.md` - Emulator setup
- `WEB_UI_GUIDE.md` - Web admin usage
- `DATA_SYNC_GUIDE.md` - Content synchronization
- `PLAYLIST_FORMAT.md` - Playlist YAML schema
- `GAME_CONTROLS.md` - Input mapping details
- `USB_CONNECTION_GUIDE.md` - USB Ethernet Gadget setup
- `MEDIA_BROWSER_PLAYBACK_AND_DOWNLOADS.md` - Deep dive on the Media Browser pipeline (full architecture, scoring tables, quality definitions, network topology, migration notes)
