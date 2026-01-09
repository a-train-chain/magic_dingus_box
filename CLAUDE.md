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
  - `input_manager` - evdev event processing, joystick/keyboard mapping
  - `gpio_manager` - GPIO access (power button, LEDs)
- **`video/`** - Video playback
  - `gst_player`, `gst_renderer` - GStreamer pipeline and GL texture rendering
  - `mpv_player`, `mpv_renderer` - Legacy MPV integration
- **`ui/`** - User interface
  - `renderer` - Immediate-mode 2D renderer (quads, text, alpha blending)
  - `theme` - Color palette and layout constants
  - `font_manager` - stb_truetype font rasterization → GL textures
  - `settings_menu` - Settings UI state machine
  - `virtual_keyboard` - On-screen QWERTY keyboard
  - `qrcodegen` - QR code generation for WiFi setup
- **`app/`** - Application logic
  - `app_state.h` - Global state (playlists, playback, settings)
  - `controller` - High-level video/audio control
  - `playlist_loader` - YAML playlist parsing
  - `settings_persistence` - YAML settings storage
- **`retroarch/`** - Game emulation
  - `retroarch_launcher` - VT switching, process launch, display restoration
- **`utils/`** - Utilities
  - `path_resolver` - Asset path resolution
  - `wifi_manager` - WiFi scanning/connection via nmcli

### Rendering Pipeline

Both video and UI render to the same OpenGL ES context:
1. Video: GStreamer renders frame to default framebuffer
2. UI: Renderer draws overlay with alpha blending
3. EGL swaps buffers

This guarantees correct compositing without X11/compositor overhead.

### Web Admin (`magic_dingus_box/web/`)

- `admin.py` - Flask routes for device discovery, playlist CRUD, content uploads
- `static/manager.js` - Frontend: device discovery, playlist builder, file uploads
- Data directory: `/opt/magic_dingus_box/magic_dingus_box_cpp/data` (configurable via `MAGIC_DATA_DIR`)

## Key Dependencies

C++ (via pkg-config): `libdrm`, `libgbm`, `libegl`, `libgles2`, `libevdev`, `libgpiod`, `yaml-cpp`, `jsoncpp`, `gstreamer-1.0`, `gstreamer-app-1.0`, `gstreamer-video-1.0`, `gstreamer-gl-1.0`

Header-only: `stb_truetype.h` (download to `src/ui/` before building)

Python: Flask (for web admin only)

## Playlist Format

YAML files in `data/playlists/`. See `magic_dingus_box_cpp/docs/PLAYLIST_FORMAT.md` for schema.

## Controls

- **DPad/Axis X**: Navigate playlists
- **A/Enter/Space**: Select playlist
- **Z**: Play/Pause
- **L/R Triggers**: Seek ±10s
- **C-Stick**: Seek ±5s
- **B**: Settings menu
- **Q/Esc**: Quit

## RetroArch Cores

Pre-installed via `--cores` flag:
- NES: `nestopia_libretro`
- N64: `mupen64plus-next_libretro`
- PS1: `pcsx_rearmed_libretro`

## Additional Documentation

Extensive docs in `magic_dingus_box_cpp/docs/`:
- `ARCHITECTURE.md` - System design
- `DISPLAY_MODES_USAGE.md` - CRT/Modern TV modes
- `RETROARCH_INTEGRATION.md` - Emulator setup
- `WEB_UI_GUIDE.md` - Web admin usage
- `DATA_SYNC_GUIDE.md` - Content synchronization
