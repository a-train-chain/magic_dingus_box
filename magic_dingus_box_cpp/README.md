# Magic Dingus Box C++ Kiosk Engine

The C++ kiosk binary for Magic Dingus Box: a Raspberry Pi 4B retro-gaming + video-playback kiosk that runs directly on DRM/KMS without X11 or Wayland.

Video frames and UI overlays are composited in a single OpenGL ES context, so alpha blending is always correct and there's no compositor between us and the framebuffer.

## Source layout

```
magic_dingus_box_cpp/
├── CMakeLists.txt            # Build (FetchContent for spdlog + Catch2)
├── README.md                 # This file
├── src/
│   ├── main.cpp              # Entry point and main loop
│   ├── platform/             # DRM/KMS, GBM, EGL, evdev input, GPIO
│   │   ├── drm_display       #  ↳ mode setting, CRTC management
│   │   ├── gbm_context       #  ↳ GBM surface for EGL
│   │   ├── egl_context       #  ↳ OpenGL ES 3.0 context, swap chain
│   │   ├── input_manager     #  ↳ evdev events, joysticks, rotary encoder
│   │   ├── gpio_manager      #  ↳ power button + LEDs via libgpiod
│   │   └── sequence_detector #  ↳ Media Browser unlock chord detector
│   ├── video/                # GStreamer playback + GL upload
│   │   ├── gst_player        #  ↳ pipeline lifecycle, playback control
│   │   └── gst_renderer      #  ↳ GL texture upload from GStreamer frames
│   ├── ui/                   # Immediate-mode 2D renderer + screens
│   │   ├── renderer          #  ↳ quads, text, alpha blending, CRT shaders
│   │   ├── theme             #  ↳ palette + layout constants
│   │   ├── font_manager      #  ↳ stb_truetype rasterization → GL textures
│   │   ├── settings_menu     #  ↳ Settings UI state machine
│   │   ├── virtual_keyboard  #  ↳ on-screen QWERTY for Wi-Fi setup
│   │   ├── qrcodegen         #  ↳ QR code generation
│   │   └── toast             #  ↳ Media Browser ephemeral notifications
│   ├── app/                  # Application logic
│   │   ├── app_state.h       #  ↳ global state (playlists, playback, settings)
│   │   ├── controller        #  ↳ video/audio control, RetroArch orchestration
│   │   ├── playlist_loader   #  ↳ YAML playlist parsing
│   │   ├── settings_persistence  # ↳ JSON settings storage with atomic write
│   │   └── sample_mode       #  ↳ demo/auto-play mode for kiosk attract loop
│   ├── retroarch/            # Game emulation
│   │   ├── retroarch_launcher    # ↳ DRM handoff, config gen, process lifecycle
│   │   └── controller_detector   # ↳ vendor/product ID → ControllerType
│   ├── media_browser/        # Optional movie browser (TMDB/Radarr/Prowlarr/qBit)
│   │   ├── tmdb_client       #  ↳ movie discovery
│   │   ├── radarr/           #  ↳ library + queue management
│   │   ├── prowlarr/         #  ↳ release search
│   │   ├── qbittorrent/      #  ↳ live download progress
│   │   ├── torrent/          #  ↳ session helpers
│   │   ├── artwork/          #  ↳ poster/backdrop cache
│   │   └── ui/               #  ↳ browse / detail / queue / playback screens
│   └── utils/                # Cross-cutting helpers
│       ├── config            #  ↳ centralized path configuration
│       ├── path_resolver     #  ↳ asset path resolution
│       ├── wifi_manager      #  ↳ Wi-Fi scan/connect via nmcli
│       └── logger            #  ↳ spdlog wrapper, rotating file sink
├── assets/                   # Fonts, bezels, logos, intro video
├── data/                     # Playlists, ROMs (gitignored), saves, states, thumbnails
├── services/                 # Media Browser docker-compose stack + .env template
├── systemd/                  # All systemd unit files
├── scripts/                  # Deploy, install, OTA, audio init, USB gadget setup
└── build/                    # CMake output (gitignored)
```

## Rendering pipeline

Both video frames and UI overlays render to the same OpenGL ES context within a single frame:

1. **Video** (if active): GStreamer decoder → `appsink` → `gst_renderer` uploads frame to a GL texture → drawn as a quad.
2. **UI**: `renderer` draws the playlist list, settings overlays, bezels, etc., with alpha blending.
3. **CRT pipeline (opt-in)**: scene rendered to an FBO, then post-processed through Gaussian scanlines, Lottes-style aperture-grille mask, RGB convergence, phosphor glow, and luma-driven halation, then composited to the front buffer.
4. EGL swaps buffers.

This guarantees correct compositing without any window manager or compositor in the loop.

## Video backend

**GStreamer is the only video backend.** Older mentions of libmpv refer to a removed earlier prototype — it's no longer in the repo, no longer in CMake, and `playbin` (`gstreamer-1.0` + `gstreamer-app-1.0` + `gstreamer-video-1.0` + `gstreamer-gl-1.0` + `gstreamer1.0-libav`) handles all playback.

Notable Pi 4 specifics:

- `v4l2h264dec` (Pi 4 hardware H.264) is rank-promoted in [`gst_player.cpp`](src/video/gst_player.cpp) and used by default — most playback uses ~3% of one core.
- `v4l2slh265dec` (Pi 4 hardware HEVC) is **explicitly disabled** at startup. It has a SAND-format negotiation bug on Pi 4 with mainline GStreamer (tracked at gstreamer/gstreamer#9247, mainline-only fix). HEVC content falls back to `avdec_h265` (software) at ~30–50% of one core for 1080p 8-bit Main profile.
- AV1: software decode only, unwatchable at 1080p+ on a Pi 4. Avoided in the Media Browser quality profile.

## Build

On the Pi (or cross-compile target):

```bash
cd magic_dingus_box_cpp
mkdir -p build && cd build
cmake ..
make -j2          # on a Pi: each cc1plus peaks near 600 MB, so -j4 pushes a
                  # 1.5 GB Pi 4B into the OOM killer and a 2 GB Pi 5 into swap
                  # while the kiosk and containers are still running.
                  # Cross-compiling on a dev machine: use -j$(nproc).
```

The kiosk binary lands at `build/magic_dingus_box_cpp`. Test binaries (`test_media_browser`, `test_media_browser_unit`) are built only when `-DENABLE_MEDIA_BROWSER=ON` is set.

System dependencies are installed by [`scripts/install_deps.sh`](scripts/install_deps.sh) — run it on a fresh Pi before the first build.

## Deploy from your dev machine

```bash
# Sync code only (PI_HOST defaults to magic@magicpi.local)
./scripts/deploy_cpp.sh

# Sync + compile on Pi
./scripts/deploy_cpp.sh --build

# Sync + build + launch interactively over SSH
./scripts/deploy_cpp.sh --build --test

# Sync + install RetroArch + 7 cores from apt
./scripts/deploy_cpp.sh --cores

# One-time USB gadget setup so 10.55.0.1 can replace WiFi for fast deploys
./scripts/deploy_cpp.sh --usb-gadget

# All-at-once provisioning of a fresh Pi for Media Browser
./scripts/deploy_cpp.sh --media-browser
```

Environment overrides: `PI_HOST=magic@<addr>` (default `magic@magicpi.local`), `PI_DIR=/opt/magic_dingus_box` (default — almost always leave it).

## Run

The kiosk runs as a systemd service:

```bash
sudo systemctl start magic-dingus-box-cpp.service
sudo journalctl -u magic-dingus-box-cpp.service -f   # follow logs
```

The service runs as the `magic` user (member of `video`, `input`, `gpio` groups) with `Type=notify` + `WatchdogSec=10` + `Restart=on-failure` for crash recovery.

For ad-hoc runs (typically only when debugging):

```bash
sudo /opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp
```

## RetroArch integration

7 cores supported (installed via [`scripts/install_cores.sh`](scripts/install_cores.sh) or `--cores`):

| System | Core | Notes |
|---|---|---|
| NES | `nestopia_libretro` | Digital input, analog-to-dpad mapping |
| SNES | `snes9x2010_libretro` | Digital input |
| Genesis / Mega Drive | `genesis_plus_gx_libretro` | 3/6-button support |
| PS1 | `pcsx_rearmed_libretro` | Analog pad type, requires `scph5501.bin` BIOS |
| PC Engine | `mednafen_pce_fast_libretro` | I/II + turbo buttons |
| Atari 7800 | `prosystem_libretro` | 2-button |
| Arcade | `fbneo_libretro` | 6-button layout |

BIOS location: `~/.config/retroarch/system/`. Per-core controller mappings live in [`src/retroarch/retroarch_launcher.cpp`](src/retroarch/retroarch_launcher.cpp), split by controller type via the `get_mapping_n64_adapter()` and `get_mapping_ps_style()` helpers (dispatched through `get_mapping(ControllerType, core_name)`).

Save files: `data/saves/<CoreName>/`. Save states: `data/states/<CoreName>/`. Auto-save on exit + auto-load on start = seamless kiosk experience.

In-game hotkey: **Z + Start** toggles the RetroArch menu (mapped uniformly across all cores).

## Reference assets

The kiosk reads playlists from `data/playlists/*.yaml` and ROMs from `data/roms/<system>/`. The Content Manager web UI ([`magic_dingus_box/web/`](../magic_dingus_box/web/)) uploads to these paths over HTTP rather than requiring SSH.

The `dev_data/` directory exists for a developer-only mode: when `MAGIC_DATA_DIR=dev_data` is set, the kiosk reads from a checked-in fixture set instead of the operator's real content. Useful for screenshots and CI golden-image runs.

## License

Same as the main Magic Dingus Box project.
