# Raspberry Pi Game Deployment Guide

This guide ensures your game emulation features work correctly on Raspberry Pi 4B.

## Pre-Deployment Checklist

### 1. Verify Playlists Use Correct Paths

Your playlists should use relative paths that work on both Mac (dev) and Pi (production):

**✅ Correct** (works on both):
```yaml
path: dev_data/roms/nes/Super Mario Bros. 3.nes
```

**❌ Wrong** (only works on Mac):
```yaml
path: /Users/alexanderchaney/Documents/Projects/magic_dingus_box/dev_data/roms/nes/...
```

The code automatically resolves `dev_data/` to:
- Mac: `<repo>/dev_data/`
- Pi: `/data/`

### 2. Core Names Are Automatically Adjusted

Your YAML files use core names like:
```yaml
emulator_core: parallel_n64_libretro
```

The launcher automatically strips `_libretro` suffix on Linux:
- **macOS**: Uses `parallel_n64_libretro` (looks in `~/Library/Application Support/RetroArch/cores/`)
- **Pi**: Uses `parallel_n64` (looks in `/usr/lib/*/libretro/`)

**No changes needed to your YAML files!** ✅

## Raspberry Pi Setup

### 1. Install RetroArch and Cores

```bash
# SSH into your Pi
ssh pi@your-pi-address

# Install RetroArch
sudo apt update
sudo apt install -y retroarch

# Install cores for your games
sudo apt install -y \
    libretro-fceumm \
    libretro-snes9x \
    libretro-parallel-n64 \
    libretro-pcsx-rearmed
```

### 2. Verify Core Installation

```bash
# Check installed cores
ls /usr/lib/*/libretro/

# You should see:
# fceumm_libretro.so          (NES)
# snes9x_libretro.so          (SNES)
# parallel_n64_libretro.so    (N64)
# pcsx_rearmed_libretro.so    (PS1)
```

### 3. Transfer ROMs

```bash
# On your Mac, sync ROMs to Pi
rsync -avz --progress \
  ~/Documents/Projects/magic_dingus_box/dev_data/roms/ \
  pi@your-pi:/data/roms/

# Verify on Pi
ssh pi@your-pi
ls -R /data/roms/
```

### 4. Transfer Playlists

```bash
# Sync game playlists
rsync -avz --progress \
  ~/Documents/Projects/magic_dingus_box/dev_data/playlists/*.yaml \
  pi@your-pi:/data/playlists/
```

## Expected Behavior on Pi

### Game Launch Sequence

1. User navigates to game in settings menu
2. User selects game
3. Magic Dingus Box UI freezes (expected - pygame still running in background)
4. RetroArch launches in fullscreen
5. Game plays
6. User quits game via RetroArch menu (F1 → Close Content)
7. RetroArch exits
8. Magic Dingus Box UI unfreezes and reappears

### Display Behavior

- **With NTSC/Composite Output**: RetroArch will output to composite just like your videos
- **Resolution**: RetroArch auto-adjusts to your configured 720x480 output
- **Aspect Ratio**: Games maintain their original aspect ratio

## Testing on Pi

### Test Checklist

```bash
# SSH into Pi
ssh pi@your-pi

# Test RetroArch directly first
retroarch -L parallel_n64 /data/roms/n64/Super\ Mario\ 64.n64 --fullscreen

# If that works, test through Magic Dingus Box
# Access your Pi via VNC or direct connection
# Navigate: Settings → Video Games → Browse Game Libraries
# Select game and launch
```

### Common Issues and Fixes

#### Issue: "Core not found"
```bash
# Check which core is missing
ls /usr/lib/*/libretro/ | grep -i <system>

# Install the missing core
sudo apt install libretro-<corename>

# Common core package names:
# NES:  libretro-fceumm
# SNES: libretro-snes9x  
# N64:  libretro-parallel-n64 or libretro-mupen64plus
# PS1:  libretro-pcsx-rearmed
```

#### Issue: "ROM not found"
```bash
# Verify ROM path matches YAML
cat /data/playlists/n64_classics.yaml
ls -la /data/roms/n64/

# Paths must match exactly (including spaces and capitalization)
```

#### Issue: N64 games run slowly
```bash
# N64 emulation is demanding on Pi 4
# Try these in RetroArch settings:

# Option 1: Lower resolution
# In game, press F1 → Settings → Video → Set to 640x480

# Option 2: Enable threaded video
# Settings → Video → Threaded Video: ON

# Option 3: Try different core
# Install: sudo apt install libretro-mupen64plus
# Update YAML: emulator_core: mupen64plus_libretro
```

#### Issue: RetroArch exits immediately
```bash
# Check RetroArch logs
tail -100 ~/.config/retroarch/retroarch.log

# Common causes:
# - Missing BIOS files (PS1)
# - Corrupted ROM
# - Wrong core for file type
```

#### Issue: Can't exit game, stuck in RetroArch
```bash
# Method 1: SSH in and kill process
ssh pi@your-pi
pkill -9 retroarch

# Method 2: Configure hotkey to quit
# Edit: ~/.config/retroarch/retroarch.cfg
# Add: input_exit_emulator = "escape"
```

## Performance Optimization

### Pi 4B Recommended Settings

**For best performance on Pi 4B:**

1. **Overclock** (optional, but helps N64):
```bash
sudo nano /boot/config.txt
# Add:
over_voltage=2
arm_freq=1750
```

2. **Disable unnecessary services**:
```bash
sudo systemctl disable bluetooth
sudo systemctl disable wifi  # if using ethernet
```

3. **RetroArch performance settings**:
Edit `~/.config/retroarch/retroarch.cfg`:
```
video_threaded = "true"
video_vsync = "false"  # Can help FPS
audio_sync = "false"   # Reduces latency
```

## Controller Configuration

### USB Controller Setup

1. Plug in USB controller before starting Magic Dingus Box
2. First game launch will prompt for controller config
3. Follow on-screen instructions
4. Config saved to `~/.config/retroarch/autoconfig/`

### GPIO Button Integration

To use your hardware buttons with games:

1. Create a GPIO-to-keyboard mapper:
```bash
# Install evdev
sudo apt install python3-evdev

# Map GPIO buttons to keyboard keys
# RetroArch will see them as keyboard input
```

2. Alternative: Use RetroPie's GPIO driver
```bash
# Install retropie-manager-mk_arcade_joystick_rpi
# This creates a virtual joystick from GPIO
```

## Backup and Restore

### Save States

RetroArch save states are stored separately:
```bash
# Backup save states
rsync -avz pi@your-pi:~/.config/retroarch/states/ ./backup/states/

# Restore save states
rsync -avz ./backup/states/ pi@your-pi:~/.config/retroarch/states/
```

### Game Saves

In-game saves (battery saves) are stored:
```bash
# Location
~/.config/retroarch/saves/

# Backup command
rsync -avz pi@your-pi:~/.config/retroarch/saves/ ./backup/saves/
```

## Final Verification

Run through this checklist on your Pi:

- [ ] RetroArch installed and in PATH
- [ ] All cores installed for your games
- [ ] ROMs copied to `/data/roms/`
- [ ] Playlists copied to `/data/playlists/`
- [ ] Can launch a game manually with `retroarch -L <core> <rom>`
- [ ] Game appears in Magic Dingus Box settings menu
- [ ] Game launches from Magic Dingus Box UI
- [ ] Game displays correctly on your output (NTSC/composite)
- [ ] Can quit game and return to Magic Dingus Box UI
- [ ] Controller works in game

If all checkboxes are ✅, your game integration is ready for deployment!

