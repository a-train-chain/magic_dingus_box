# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

Magic Dingus Box is a retro gaming and video playback kiosk for Raspberry Pi 4B and Raspberry Pi 5, both served by ONE golden image and ONE release artifact. The board is detected at runtime (`src/platform/platform_profile.{h,cpp}`). It consists of:

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

Environment variables: `PI_HOST` (default: `magic@magicpi.local`), `PI_DIR` (default: `/opt/magic_dingus_box`)

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
  - `retroarch_launcher` - DRM/KMS handoff, config generation (incl. video config via `write_video_config()`), process lifecycle. Per-core button mappings live here in two helper variants — `get_mapping_n64_adapter()` (USB N64-style adapter) and `get_mapping_ps_style()` (PlayStation-style pads); the dispatch goes through `get_mapping(ControllerType, core_name)`.
  - `controller_detector` - USB controller probing (vendor/product IDs → `ControllerType` enum) so the launcher knows which mapping helper to call. Split out of `retroarch_launcher` in v1.4.0.
- **`utils/`** - Utilities
  - `config` - Centralized path configuration (base paths, RetroArch paths, save dirs)
  - `path_resolver` - Asset path resolution
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
- Features: video transcoding (CRT 640x480 / Modern 720p presets), playlist package import/export (ZIP), system monitoring
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
- Per-core button mappings (N64 controller → RetroPad) defined in `retroarch_launcher.cpp`
- **Z + Start**: Toggle RetroArch menu (hotkey combo for all cores)
- Auto-save state on exit, auto-load on start

## RetroArch Cores

7 cores installed via `--cores` flag (`scripts/install_cores.sh`):

| System | Core | Notes |
|--------|------|-------|
| NES | `nestopia_libretro` | Digital input, analog-to-dpad mapping |
| SNES | `snes9x2010_libretro` | Digital input |
| Genesis/Mega Drive | `genesis_plus_gx_libretro` | 3/6-button support |
| PS1 | `pcsx_rearmed_libretro` | Analog pad type, requires BIOS (`scph5501.bin` in system dir) |
| PC Engine | `mednafen_pce_fast_libretro` | I/II + turbo buttons |
| Atari 7800 | `prosystem_libretro` | 2-button |
| Arcade | `fbneo_libretro` | 6-button layout |

BIOS location: `~/.config/retroarch/system/`
Core location: `libretro_cores/` (app directory) or `/usr/lib/aarch64-linux-gnu/libretro/` (system)

## OTA Updates

- `scripts/update.sh` checks GitHub API for latest release
- Downloads tarball, backs up current installation, extracts update
- Rollback support if update fails
- Triggered via web admin `/api/update/*` endpoints

## Media Browser (Movie Playback + Downloads)

The Media Browser is a sub-mode of the kiosk that provides movie discovery, downloads, and playback through a Radarr / Prowlarr / qBittorrent / Gluetun stack. Architecture summary:

### Data flow

- TMDB → BrowseScreen / SearchScreen (movie discovery)
- Radarr → DetailScreen (library state, queue management)
- Prowlarr → AVAILABILITY readout on Detail (release-search seeders, async background thread)
- qBittorrent → QueueScreen live overlay (real-time progress; Radarr's queue cache is 30-60s stale)
- Gluetun → VPN tunnel for all torrent traffic with NAT-PMP port forwarding
- GStreamer playbin → PlaybackScreen (hardware H.264 decode preferred, software HEVC fallback)

### Playback hardware notes

- `v4l2h264dec` (Pi 4 hardware H.264) is rank-promoted and used by default
- `v4l2slh265dec` (Pi 4 hardware HEVC) is **disabled** — has a pixel-format negotiation bug on this kernel; falls back to `avdec_h265` (software, ~30-50% of one core for 1080p 8-bit Main profile)
- AV1 has no hardware decoder; software-decode at 1080p+ is unwatchable
- Required system package: `gstreamer1.0-libav` (codified in `scripts/install_deps.sh`)

### Quality configuration (3-layer enforcement)

1. **Quality profile "Any"** — only allows 720p/1080p HDTV/WEB/Bluray (no SD, no 4K, no Remux)
2. **Custom Format scoring** (sums vs `minFormatScore = -200`):
   - AV1: -1000 / Remux: -500 / HEVC 1080p+: -250 / HDR: -200 (all rejected)
   - x264: +50 / Trusted groups (YIFY/GalaxyRG/RARBG/SURGE): +30 (preferred)
3. **Quality definition size limits**: 720p ≤60 MB/min, 1080p ≤100 MB/min

Net effect: every grab is x264 H.264 in the 720p-1080p range, 1-3 GB typical, hardware-decoded smoothly.

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
- **Active indexers** (Prowlarr → Radarr): TPB, YTS, LimeTorrents, TorrentDownload (with `cloudflare` tag → Byparr,
which replaces FlareSolverr for current Cloudflare challenge formats). Plus 5 pre-configured but disabled (Demonoid, EZTV, Internet Archive, Magnetz, Torrent Downloads) for future enable.
- **Gluetun healthcheck**: `wget http://localhost:8000/v1/publicip/ip` (gluetun's own internal endpoint). Avoids the previous `ifconfig.me` healthcheck which flapped due to Cloudflare challenges on ProtonVPN exit IPs.
- **Skip-when-unconfigured**: `magic-dingus-services.service` has `ConditionPathExists=/opt/magic_dingus_box/services/.env` so unprovisioned Pis cleanly skip the Docker stack instead of fail-looping. `setup_services.sh` is fully idempotent and rebuilds the entire stack from codified fixtures in `scripts/data/*.json` (Custom Formats, indexers, Byparr proxy, Apps integration, download client, quality definitions, qBit category) — fresh deploys reproduce the source Pi's exact configuration except for per-Pi secrets.

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

### Live SD cloning

`scripts/golden_image/clone_live_sd.sh` clones a running Pi's SD card to a `.img.gz` over SSH without removing the SD physically. Three Pi-side scripts (`prepare_for_cloning.sh`, `restore_after_cloning.sh`, `first_boot.sh` Step 6) handle prepare/restore + per-Pi state cleanup on the cloned image. Source Pi loses no data; total downtime ~1 minute. See `scripts/golden_image/CLONING.md` for full operator workflow.

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
