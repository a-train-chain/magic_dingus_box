# Emulator Integration - Quick Start Guide

## What's Been Implemented

Your Magic Dingus Box now has full RetroArch emulator support with a beautiful slide-in settings menu!

### New Features

1. **Settings Menu** - Press button 4 (quick press) when UI is visible
   - Slides in from the right (1/3 screen width)
   - Navigate with encoder, select with button
   - Sections: Video Games, Display, Audio, System Info

2. **Game Browser** - Access games through Settings → Video Games → Browse Game Libraries
   - Clean separation: Main UI shows only videos
   - Game playlists accessed through settings menu
   - Seamless transitions - no visible emulator
   - Returns to UI automatically when game exits

3. **Example Playlists** - Three ready-to-use game playlists:
   - `retro_games.yaml` - Mixed NES/SNES games
   - `n64_classics.yaml` - Nintendo 64 games
   - `ps1_collection.yaml` - PlayStation 1 games
   - **Note**: Game playlists only appear in the Video Games browser, not in the main UI

## Getting Started

### 1. Install RetroArch

**macOS (development):**
```bash
brew install --cask retroarch
```

Then open RetroArch app and install cores via:
`Online Updater → Core Downloader`

Install these:
- Nintendo - NES / Famicom (FCEUmm)
- Nintendo - SNES / SFC (Snes9x - Current)
- Nintendo - Nintendo 64 (Mupen64Plus-Next)
- Sony - PlayStation (PCSX ReARMed)

**Raspberry Pi:**
```bash
sudo apt install retroarch libretro-fceumm libretro-snes9x libretro-mupen64plus libretro-pcsx-rearmed
```

### 2. Add Your ROMs

Create the ROM directory structure:
```bash
mkdir -p dev_data/roms/{nes,snes,n64,ps1}
```

Then copy your ROM files:
```
dev_data/roms/
├── nes/       # .nes files
├── snes/      # .smc or .sfc files
├── n64/       # .z64 or .n64 files
└── ps1/       # .cue + .bin files
```

### 3. Test It Out

1. Start the application:
   ```bash
   ./scripts/run_dev.sh
   ```

2. Notice the main UI shows only video playlists (games are hidden)

3. Press button 4 to open the settings menu

4. Navigate to "Video Games" and press Enter

5. Select "Browse Game Libraries" and press Enter

6. You'll see your game playlists - select one and press Enter to launch!

7. Play the game, then exit to return to the UI

## Creating Your Own Game Playlists

Example format:
```yaml
title: My Games
curator: Your Name
loop: false
items:
  - title: Game Name
    source_type: emulated_game
    path: roms/nes/GameFile.nes
    emulator_core: fceumm_libretro
    emulator_system: NES
```

**Core reference:**
- NES: `fceumm_libretro`
- SNES: `snes9x_libretro`
- N64: `mupen64plus_next_libretro`
- PS1: `pcsx_rearmed_libretro`

### How Playlists Are Categorized

The system automatically categorizes playlists:

- **Game Playlists**: Contain ONLY `emulated_game` items → Show in Video Games browser only
- **Video Playlists**: Contain ANY `local` or `youtube` items → Show in main UI

This means:
- Pure game playlists only appear in Settings → Video Games → Browse Game Libraries
- Video playlists (including mixed video/game) appear in the main UI
- You CAN mix videos and games in one playlist (it will show in main UI)

## Keyboard Controls

Updated button 4 behavior:
- **Quick press**: Open/close settings menu
- **Hold (0.3s+)**: Enter sample mode (when in normal mode)

All other controls remain the same!

## Mixing Videos and Games (Optional)

You CAN mix videos and games in one playlist if desired:

```yaml
title: Entertainment Mix
curator: Alex
items:
  - title: Concert Video
    source_type: local
    path: media/concert.mp4
  
  - title: Super Mario Bros
    source_type: emulated_game
    path: roms/nes/Super Mario Bros.nes
    emulator_core: fceumm_libretro
    emulator_system: NES
```

**Note**: Mixed playlists will show in the MAIN UI (not in the game browser), since they contain video content.

## What's Preserved

All existing features work exactly as before:
✓ Video playback with mpv
✓ Sample mode with markers
✓ Volume transitions (75% menu / 100% video)
✓ Auto-progression between tracks
✓ UI fade animations
✓ GPIO input (on Pi)
✓ Web admin interface

## Troubleshooting

**"RetroArch not found"** - Install RetroArch per instructions above

**"ROM not found"** - Check your ROM path in the YAML matches actual file location

**Game won't launch** - Verify core is installed:
```bash
# macOS
ls ~/Library/Application\ Support/RetroArch/cores/

# Linux
ls /usr/lib/libretro/
```

**Settings menu won't open** - Only works when UI is visible (not during video playback)

## Technical Details

### Files Modified
- `magic_dingus_box/library/models.py` - Added emulator fields
- `magic_dingus_box/library/loader.py` - Parse emulator fields from YAML
- `magic_dingus_box/player/controller.py` - Handle game launches
- `magic_dingus_box/inputs/abstraction.py` - New SETTINGS_MENU event
- `magic_dingus_box/inputs/keyboard.py` - Button 4 quick press
- `magic_dingus_box/config.py` - Added roms_dir
- `magic_dingus_box/main.py` - Settings menu integration

### Files Created
- `magic_dingus_box/player/retroarch_launcher.py` - Game launcher
- `magic_dingus_box/ui/settings_menu.py` - Menu state manager
- `magic_dingus_box/ui/settings_renderer.py` - Menu renderer
- `dev_data/playlists/retro_games.yaml` - Example playlist
- `dev_data/playlists/n64_classics.yaml` - Example playlist
- `dev_data/playlists/ps1_collection.yaml` - Example playlist

### How Games Launch

1. User selects game from playlist
2. Controller detects `source_type: emulated_game`
3. RetroArch launcher resolves ROM path
4. Pygame window remains open
5. RetroArch subprocess launches in fullscreen
6. Process blocks until game exits
7. Control returns to Magic Dingus Box
8. UI is ready for next selection

The transition is seamless - the user never sees RetroArch's interface, just the game!

## Next Steps

1. Install RetroArch and cores
2. Add some ROM files to test
3. Launch the app and try opening the settings menu (button 4)
4. Try launching a game from the example playlists
5. Create your own custom game/video playlists

Enjoy your upgraded Magic Dingus Box! 🎮✨

