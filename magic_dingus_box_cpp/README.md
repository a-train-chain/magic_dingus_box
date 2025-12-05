# Magic Dingus Box C++ Kiosk Engine

A true kiosk-mode application for Raspberry Pi 4B (OS Lite 64-bit) that provides guaranteed transparent UI overlay over video playback using DRM/KMS + GBM + EGL + OpenGL ES.

## Overview

This is a complete rewrite of the Magic Dingus Box in C++ for maximum performance and guaranteed overlay behavior. Unlike the Python version which relies on X11 window stacking, this version uses direct DRM/KMS access to own the entire display, ensuring video and UI are always composited correctly in a single OpenGL ES context.

## Project Structure

```
magic_dingus_box_cpp/
├── CMakeLists.txt          # Build configuration
├── README.md               # This file
├── ARCHITECTURE.md         # Technical architecture documentation
├── src/                    # Source code
│   ├── main.cpp           # Entry point and main loop
│   ├── platform/          # DRM/KMS, GBM, EGL, input
│   ├── video/             # libmpv integration
│   ├── ui/                # UI rendering system
│   ├── app/               # Application logic
│   ├── retroarch/         # RetroArch integration
│   └── utils/             # Utilities
├── assets/                 # Fonts, bezels, shaders
├── data/                   # Playlists, settings
└── build/                  # Build output (gitignored)
```

## Dependencies

### Required System Packages (Raspberry Pi OS Lite 64-bit)

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config \
  libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev \
  libevdev-dev libyaml-cpp-dev libjsoncpp-dev
```

### libmpv

This project requires libmpv with render API support. If you've built mpv from source (as documented in the main project), ensure `/usr/local/lib/libmpv.so` is available. Otherwise, install the system package:

```bash
sudo apt install -y libmpv-dev
```

### stb_truetype

The font manager uses `stb_truetype.h` for font rasterization. Download it from:

```bash
cd magic_dingus_box_cpp/src/ui
wget https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
```

Or manually place `stb_truetype.h` in `src/ui/` before building.

## Building

```bash
cd magic_dingus_box_cpp
mkdir -p build
cd build
cmake ..
make -j4
```

The executable will be at `build/magic_dingus_box_cpp`.

## Running

### Prerequisites

- Must run as root or user with access to `/dev/dri/card*`
- Must have access to input devices (`/dev/input/event*`)
- Playlists must be available in `data/playlists/` (symlink from main project's `dev_data/playlists/`)

### Execution

```bash
sudo ./build/magic_dingus_box_cpp
```

Or configure as a systemd service for automatic startup.

## Deployment

### Quick Deployment with Pre-installed Cores

For the fastest setup with games ready to play immediately:

```bash
cd magic_dingus_box_cpp
./scripts/deploy_cpp.sh --cores
```

This automatically:
- Syncs code to your Pi
- Builds the C++ application
- **Installs RetroArch cores for NES, N64, and PS1 games**
- Sets up everything needed for gaming

### Alternative Deployment Options

```bash
# Deploy without cores (download on first use)
./scripts/deploy_cpp.sh --build --test

# Install cores separately later
./scripts/install_cores.sh --pi
```

### Manual Core Installation

If you prefer to install cores manually:

```bash
# On your Pi
sudo apt install -y retroarch
cd /opt/magic_dingus_box
sudo scripts/install_retropie_cores.sh
```

This installs the required cores:
- **NES**: `nestopia_libretro`
- **N64**: `mupen64plus-next_libretro`
- **PS1**: `pcsx_rearmed_libretro`

## Differences from Python Version

- **No X11**: Runs directly on DRM/KMS, no window manager or compositor
- **Guaranteed overlay**: Video and UI are in the same GL context, alpha blending is always correct
- **Lower overhead**: No Python interpreter, no pygame, no X11 compositing
- **True kiosk**: Owns the entire display, no other windows can interfere

## Using Existing Assets

The C++ version can use the same assets and playlists as the Python version:

```bash
# Symlink playlists
ln -s ../../dev_data/playlists magic_dingus_box_cpp/data/playlists

# Copy assets
cp -r ../assets/fonts magic_dingus_box_cpp/assets/fonts
cp -r ../assets/bezels magic_dingus_box_cpp/assets/bezels
```

## Reference Implementation

The Python codebase in `../magic_dingus_box/` serves as the reference specification for:
- UI layout and visual design (`ui/renderer.py`, `ui/theme.py`)
- Application behavior (`main.py`, `player/controller.py`)
- Playlist format (`library/models.py`, `library/loader.py`)
- Input mappings (`inputs/`)

## License

Same as the main Magic Dingus Box project.

