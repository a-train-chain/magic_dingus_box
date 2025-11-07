# Magic Dingus Box

Countertop video jukebox UI for Raspberry Pi (NTSC composite) using Pygame and mpv.

## Dev quickstart (macOS)

1. Install mpv (Homebrew): `brew install mpv`
2. Create and activate a venv, then install deps: `pip install -r requirements.txt`
3. Run the dev environment:
   ```bash
   ./scripts/run_dev.sh
   ```

**Or** manually start in two terminals:

3a. Start mpv in one terminal:
   ```bash
   mpv --idle=yes --geometry=720x480+0+0 --no-osd-bar --keep-open=yes \
     --input-ipc-server=/tmp/mpv-magic.sock \
     --vf=scale=720:480:force_original_aspect_ratio=increase,crop=720:480,setdar=4/3
   ```
3b. Start the UI in another terminal:
   ```bash
   export MPV_SOCKET=/tmp/mpv-magic.sock
   python -m magic_dingus_box.main
   ```

### Keyboard controls (simulating hardware encoder + buttons):
- **Arrow Left/Right**: Rotate encoder (navigate playlist or seek ±1s)
- **Enter/Space**: Push encoder (select/toggle menu)
- **1**: Backward button (press: previous track, hold: rewind)
- **2**: Play/Pause button
- **3**: Forward button (press: next track, hold: fast-forward)
- **4**: Settings menu button (press: open settings, hold: enter sample mode)
- **Q/Escape**: Quit

### UI behavior:
- Initial startup: Solid background with playlist menu
- Press Enter to select → 1-second fade-out → video plays at 100% volume
- Press Enter while video playing → 1-second fade-in → menu appears with 50% transparent background, volume drops to 75%
- Press Enter while menu visible → fade-out → menu disappears, volume back to 100%

Notes:
- On macOS, mpv and the UI run in separate windows (no embedding support)
- On Pi with X11, mpv embeds into the pygame window for true overlay
- Local dev data directory is `dev_data/` by default.
- For the bezel-on-top CRT effect (video behind the frame), the full illusion is only exact on Linux/Pi with X11 embedding. On macOS dev, mpv is a separate window and can appear above; you can reduce this by starting mpv with `--ontop=no`, but the full effect requires Linux/Pi.

## Emulator Support (RetroArch)

Magic Dingus Box supports launching emulated games seamlessly through RetroArch. Games appear in playlists alongside videos and can be launched with the same interface.

### Installing RetroArch

**On macOS (for development):**
```bash
brew install --cask retroarch
```

Then open RetroArch and install cores:
1. Open RetroArch application
2. Go to: Online Updater → Core Downloader
3. Install these cores:
   - **Nintendo (NES)**: `Nintendo - NES / Famicom (FCEUmm)`
   - **Super Nintendo (SNES)**: `Nintendo - SNES / SFC (Snes9x - Current)`
   - **Nintendo 64**: `Nintendo - Nintendo 64 (Mupen64Plus-Next)`
   - **PlayStation 1**: `Sony - PlayStation (PCSX ReARMed)`

**On Raspberry Pi (Linux):**
```bash
sudo apt update
sudo apt install retroarch

# Install cores
sudo apt install \
    libretro-fceumm \
    libretro-snes9x \
    libretro-mupen64plus \
    libretro-pcsx-rearmed
```

### ROM Directory Structure

Place your ROM files in the `dev_data/roms/` directory (or `/data/roms/` on Pi):

```
dev_data/roms/
├── nes/
│   ├── Super Mario Bros 3.nes
│   └── Legend of Zelda.nes
├── snes/
│   └── Super Mario World.smc
├── n64/
│   ├── Super Mario 64.z64
│   └── GoldenEye 007.z64
└── ps1/
    ├── Crash Bandicoot.cue
    ├── Crash Bandicoot.bin
    └── ...
```

**Note**: PS1 games typically use `.cue` + `.bin` files. Reference the `.cue` file in your playlist.

### Configuring Game Playlists

Add games to playlists using the `emulated_game` source type:

```yaml
title: Retro Game Collection
curator: Alex Chaney
loop: false
items:
  - title: Super Mario Bros. 3
    source_type: emulated_game
    path: roms/nes/Super Mario Bros 3.nes
    emulator_core: fceumm_libretro
    emulator_system: NES
    
  - title: Super Mario 64
    source_type: emulated_game
    path: roms/n64/Super Mario 64.z64
    emulator_core: mupen64plus_next_libretro
    emulator_system: N64
```

**Core Names:**
- NES: `fceumm_libretro`
- SNES: `snes9x_libretro`
- N64: `mupen64plus_next_libretro`
- PS1: `pcsx_rearmed_libretro`

### Mixed Playlists

You can mix videos and games in the same playlist:

```yaml
title: Mixed Entertainment
curator: Alex Chaney
items:
  - title: Concert Video
    source_type: local
    path: media/concert.mp4
    
  - title: Super Mario 64
    source_type: emulated_game
    path: roms/n64/Super Mario 64.z64
    emulator_core: mupen64plus_next_libretro
    emulator_system: N64
```

### Playing Games

1. Navigate to a game in your playlist
2. Press **Enter/Space** to launch
3. Game launches in fullscreen
4. Play with your controller (RetroArch auto-configures most USB controllers)
5. Exit game (usually **Start+Select** or RetroArch menu)
6. Returns seamlessly to the Magic Dingus Box UI

### Settings Menu

Press **Button 4** (quick press) when the UI is visible to open the settings menu. From here you can:
- Access video game configuration
- Adjust display settings (scanlines, brightness)
- Configure audio settings
- View system information

Example playlists are provided in `dev_data/playlists/`:
- `retro_games.yaml` - NES games
- `n64_classics.yaml` - N64 games
- `ps1_collection.yaml` - PlayStation 1 games

### Game Controls

See **[GAME_CONTROLS.md](GAME_CONTROLS.md)** for:
- How to pause games
- Accessing RetroArch menu during gameplay
- Saving/loading game states
- Returning to Magic Dingus Box UI

### Raspberry Pi Deployment

See **[PI_GAME_DEPLOYMENT.md](PI_GAME_DEPLOYMENT.md)** for:
- Complete Pi setup instructions
- Core installation and verification
- Performance optimization
- Troubleshooting guide

### Modern Display Support

Want to use Magic Dingus Box on a laptop or modern TV?

See **[MODERN_DISPLAY_DESIGN.md](MODERN_DISPLAY_DESIGN.md)** for:
- Display modes (CRT native, Modern with/without bezel)
- 4:3 content properly centered on any resolution
- Optional retro CRT TV frame graphic
- Settings menu integration

**Try the demo:** `python test_modern_display.py`

### CRT Effects & Professional Bezels

See **[CRT_EFFECTS_GUIDE.md](CRT_EFFECTS_GUIDE.md)** for:
- 7 professional RetroArch CRT TV bezel images
- Enhanced scanlines with adjustable intensity
- Color warmth control (CRT phosphor temperature)
- Screen bloom and phosphor glow effects
- All toggleable in Display Settings

## Deployment (Raspberry Pi OS Lite)

- One-command setup and autostart:
  ```bash
  # On the Pi, from the repo root
  bash scripts/setup_pi.sh
  ```
  - Installs X11 (LightDM/Openbox), Python deps, mpv, git-lfs
  - Deploys app to `/opt/magic_dingus_box` with a venv
  - Enables systemd services (`magic-mpv`, `magic-ui`) for HDMI output
  - Default audio device: HDMI (override via `MAGIC_AUDIO_DEVICE`)

- Composite (NTSC) via `/boot/config.txt` with mpv embedded in pygame window.
- `/data` is used for playlists, media, and logs.
- Requires X11 (Openbox) for window embedding; mpv renders into the pygame window via `--wid`.
- Systemd services start `mpv` (IPC server) and the UI on boot.

See `NTSC_config.md` for composite video settings.

### X11 Setup for Embedding
The UI uses `--wid` to embed mpv into the pygame window. You'll need a minimal X11 environment:
```bash
sudo apt install --no-install-recommends xserver-xorg xinit openbox
```

Start X with the UI app via `~/.xinitrc` or systemd graphical target.

### Troubleshooting
- mpv IPC not connecting: ensure `magic-mpv.service` is active and `/run/magic/mpv.sock` exists.
- No audio: confirm USB DAC appears as ALSA card 1 (`aplay -l`) or override `MAGIC_AUDIO_DEVICE`.
- Composite output missing: recheck `/boot/config.txt` and that HDMI is ignored.
- Permissions on `/data`: ensure partition is RW and directories exist.

See `magic.plan.md` for full design.

## Documentation
- Display modes usage: `DISPLAY_MODES_USAGE.md`
- CRT and bezels: `docs/CRT_AND_BEZELS.md`
- Web UI guide: `docs/WEB_UI_GUIDE.md`
- Playlist format: `docs/PLAYLIST_FORMAT.md`
- Archived design/change logs: `docs/archive/`


## Production Environment & Configuration

### Python Version
- Tested on Python 3.11+. Minimum 3.9 due to `pathlib.Path.is_relative_to`.

### Environment Variables
You can set these via shell or an env file (see examples in `systemd/README_HARDENED.md`). Defaults preserve current behavior.

- `MAGIC_DATA_DIR` — Path for playlists/media/logs (default `/data` on Linux, `dev_data` on macOS)
- `MPV_SOCKET` — mpv IPC socket path (default `/run/magic/mpv.sock` on Linux, `/tmp/mpv-magic.sock` on macOS)
- `MAGIC_DISPLAY_MODE` — `crt_native` | `modern_clean` | `modern_bezel`
- `MAGIC_MODERN_RES` — `auto` or `WIDTHxHEIGHT` (e.g. `1920x1080`)
- `MAGIC_ENABLE_WEB_ADMIN` — `1` to enable, `0` to disable (default `1`)
- `MAGIC_ADMIN_PORT` — Admin HTTP port (default `8080`)
- `MAGIC_MAX_UPLOAD_MB` — Max upload size in MB for web admin (default `2048`)
- `MAGIC_ADMIN_TOKEN` — If set, requires `X-Magic-Token` header on admin APIs
- `MAGIC_AUDIO_DEVICE` — mpv audio device string (Linux example: `alsa:device=hw:1,0`)

### Hardened systemd units (optional)
Hardened unit files are provided in `systemd/*.hardened.service`. Create `/etc/magic_dingus_box.env` to set runtime variables (e.g., `AUDIO_DEVICE`). See `systemd/README_HARDENED.md` for details.

