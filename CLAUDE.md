# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Magic Dingus Box is a retro gaming and video playback kiosk for Raspberry Pi 4B. It consists of:

1. **C++ Kiosk Engine** (`magic_dingus_box_cpp/`) - Primary application using DRM/KMS for true kiosk mode with direct GPU access, no X11/Wayland
2. **Python Web Admin** (`magic_dingus_box/web/`) - Flask-based remote playlist/content management interface

## Build Commands

### C++ Build (on Pi or cross-compile)
```bash
cd magic_dingus_box_cpp
mkdir -p build && cd build
cmake ..
make -j4
```

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
  - `retroarch_launcher` - DRM/KMS handoff, per-core controller mapping, config generation, process lifecycle
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

YAML files in `data/playlists/`. Two item types:
- `source_type: video` - Video playback (path to video file)
- `source_type: emulated_game` - RetroArch game (path to ROM, `emulator_core`, `emulator_system`)

See `magic_dingus_box_cpp/docs/PLAYLIST_FORMAT.md` for full schema.

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

## Additional Documentation

Extensive docs in `magic_dingus_box_cpp/docs/`:
- `ARCHITECTURE.md` - System design
- `DISPLAY_MODES_USAGE.md` - CRT/Modern TV modes
- `RETROARCH_INTEGRATION.md` - Emulator setup
- `WEB_UI_GUIDE.md` - Web admin usage
- `DATA_SYNC_GUIDE.md` - Content synchronization
- `PLAYLIST_FORMAT.md` - Playlist YAML schema
- `GAME_CONTROLS.md` - Input mapping details
- `USB_CONNECTION_GUIDE.md` - USB Ethernet Gadget setup
