# Magic Dingus Box - Deployment Guide

Complete instructions for installing Magic Dingus Box on new Raspberry Pi 4B (2GB) units.

There are two paths: setting up the **first Pi** (master unit), then **cloning to new Pis** via golden image.

---

## Part 1: Setting Up the Master Pi

### Step 1: Flash Raspberry Pi OS Lite

1. Open [Raspberry Pi Imager](https://www.raspberrypi.com/software/)
2. **Choose Device** → Raspberry Pi 4
3. **Choose OS** → Raspberry Pi OS (other) → **Raspberry Pi OS Lite (64-bit)** (Bookworm)
4. **Choose Storage** → your SD card
5. Click **Next** → **Edit Settings**:

| Setting | Value |
|---------|-------|
| Hostname | `magicpi` |
| Username | `magic` |
| Password | *(your choice)* |
| WiFi SSID | *(your network)* |
| WiFi Password | *(your password)* |
| Locale | *(your timezone)* |
| **Services tab** | Enable SSH → Use password authentication |

6. **Save** → **Yes** → **Yes** to flash

### Step 2: Boot & Connect

1. Insert SD card into Pi 4B
2. Connect HDMI and power
3. Wait ~1-2 minutes for first boot
4. From your Mac:

```bash
ssh magic@magicpi.local
```

If `.local` doesn't resolve, find the Pi's IP on your router and use `ssh magic@<IP>`.

### Step 3: Deploy & Build

**From your Mac** (not SSH'd into the Pi), in the project directory:

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/magic_dingus_box_suite/magic_dingus_box "

# Deploy code, install all dependencies, build (~15-20 min on 2GB Pi)
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build

# Install RetroArch + all 7 emulator cores
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --cores
```

### Step 4: System Setup

**SSH into the Pi:**

```bash
ssh magic@magicpi.local

# Install systemd services, optimize boot, set permissions
sudo /opt/magic_dingus_box/scripts/setup_pi.sh
```

### Step 5: Deploy ROMs & Thumbnails

**From your Mac:**

```bash
# Copy ROMs
rsync -avz --progress "/Users/alexanderchaney/Desktop/MDB Starting Roms/roms/" \
  magic@magicpi.local:/opt/magic_dingus_box/magic_dingus_box_cpp/data/roms/

# Copy thumbnails
rsync -avz --progress "/Users/alexanderchaney/Desktop/MDB Starting Roms/thumbnails/" \
  magic@magicpi.local:/opt/magic_dingus_box/magic_dingus_box_cpp/data/thumbnails/
```

**PS1 BIOS** (required for PS1 games):

```bash
scp /path/to/scph5501.bin magic@magicpi.local:/home/magic/.config/retroarch/system/
```

### Step 6: Optional Setup

**From the Pi (SSH):**

```bash
# USB-C direct connection (access Pi at http://10.55.0.1:5000)
sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_usb_gadget.sh
```

### Step 7: Reboot & Test

```bash
sudo reboot
```

After ~30 seconds the kiosk starts on HDMI with all 149 games. Web manager available at `http://magicpi.local:5000`.

**Verify everything works:**
- Navigate playlists with controller/keyboard
- Launch a game from each system
- Open the web manager in a browser
- Check the QR code in Settings → Content Manager

---

## Part 2: Creating the Golden Image (One Time)

Once the master Pi is fully working and tested.

### Recommended: live SD cloning (no SD removal)

Since v1.4.x the recommended flow clones the source Pi's SD card over SSH
without ever physically removing it. The source Pi loses no permanent
data and is only offline for ~1 minute total. See
[`scripts/golden_image/CLONING.md`](../../scripts/golden_image/CLONING.md)
for the full operator workflow; the short version:

```bash
# From your Mac, in the project directory:
./scripts/golden_image/clone_live_sd.sh
# Defaults to magic@magicpi.local → ~/golden_image_YYYY-MM-DD.img.gz

# Or with explicit args:
./scripts/golden_image/clone_live_sd.sh \
    --pi magic@magicpi-abcd.local \
    --output ~/Desktop/my-golden.img.gz
```

The script orchestrates:
1. SSH connectivity + sudo NOPASSWD verification on the Pi
2. SD-size + Mac-disk-space pre-flight (needs ~50% of card size compressed)
3. `prepare_for_cloning.sh` on the Pi (stops kiosk, snapshots per-Pi
   identity files, drops `in_progress` marker, syncs disks)
4. `dd | gzip` over a single persistent SSH session (`pv` provides a
   progress bar if installed)
5. `restore_after_cloning.sh` on the Pi (restores identity files,
   restarts the kiosk + Docker stack, removes the marker)

A trap handler ensures step 5 fires even on Ctrl+C or network drop —
the source Pi is never left in the half-prepared state.

### Legacy: SD-removal + `create_image.sh`

If you can't use the live-cloning flow (e.g., Pi is unreachable over
network), the older manual path still works. **Stop the kiosk and run
`prepare_golden_image.sh` first**, which intentionally wipes operator
content with a destructive-action prompt — safe for redistribution
images, NOT what you want for a customer-shipping golden image:

```bash
ssh magic@magicpi.local
sudo /opt/magic_dingus_box/scripts/golden_image/prepare_golden_image.sh
sudo shutdown -h now
```

Then remove the SD card and image it from your Mac:

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/magic_dingus_box_suite/magic_dingus_box "
./scripts/golden_image/create_image.sh
```

This auto-detects the SD card, reads it via `dd`, gzips the output.
Press Ctrl+T during the dd to see progress. Add `--shrink` to also pass
the image through PiShrink (requires Docker Desktop).

Note: `create_image.sh` produces an image of the **wiped** Pi —
`prepare_golden_image.sh`'s wipe is part of that flow. Use the live
cloning script instead if you want the **fully-loaded** golden image
(operator's curated playlists, saves, ROMs, etc., all included for
showcase distribution).

---

## Part 3: Flashing New Pis (Repeat for Each Unit)

### Step 1: Flash the Image

Insert a blank SD card into your Mac:

```bash
./scripts/golden_image/flash_image.sh magic_dingus_box_golden_v1.3.0_YYYYMMDD.img.gz
```

The script will:
1. Auto-detect the SD card (or prompt you to select it)
2. Ask for confirmation (type `yes`)
3. Flash the image via `dd` (takes 5-15 minutes)
4. Eject the SD card

**Alternative:** Use Raspberry Pi Imager → Choose OS → Use custom → select the `.img.gz` file.

### Step 2: Boot the New Pi

1. Insert the SD card into the new Pi 4B
2. Connect HDMI and power
3. First boot takes ~30-60 seconds while it automatically:
   - Expands the filesystem to fill the SD card
   - Regenerates SSH host keys (unique to this unit)
   - Generates a unique device ID
   - Sets a unique hostname (e.g., `magicpi-a3f2`)
4. The kiosk starts with all 149 games ready to play

### Step 3: Upload Video Content

Each new Pi starts with a clean slate for video playlists. Upload content for that user via:

- **QR Code:** Open Settings → Content Manager on the Pi's display, scan the QR code with a phone
- **Browser:** Navigate to `http://magicpi-XXXX.local:5000` (the hostname is shown in the Settings menu)
- **USB Direct:** If USB gadget mode is enabled, connect USB-C cable and go to `http://10.55.0.1:5000`

---

## Content Structure

The `MDB Starting Roms` folder on your Mac is deployment-ready at:

```
/Users/alexanderchaney/Desktop/MDB Starting Roms/
├── roms/
│   ├── arcade/         19 ROMs + samples/
│   ├── atari7800/      20 ROMs
│   ├── genesis/        20 ROMs
│   ├── nes/            20 ROMs
│   ├── pcengine/       15 ROMs
│   ├── ps1/            40 .chd + 4 .m3u (multi-disc)
│   └── snes/           20 ROMs
└── thumbnails/
    ├── arcade/         19 cover images
    ├── atari7800/      20 cover images
    ├── genesis/        20 cover images
    ├── nes/            20 cover images
    ├── pcengine/       15 cover images
    ├── ps1/            35 cover images
    └── snes/           20 cover images
```

149 total games across 7 systems. All playlist paths are verified to match.

### Multi-Disc PS1 Games

These games have `.m3u` playlist files that reference individual disc `.chd` files:

| Game | Discs |
|------|-------|
| Final Fantasy VII | 3 discs |
| Gran Turismo 2 | 2 discs |
| Metal Gear Solid | 2 discs |
| Resident Evil 2 | 2 discs |

The `.m3u` files are already created in `roms/ps1/`.

---

## Key Paths on the Pi

| What | Path |
|------|------|
| Install directory | `/opt/magic_dingus_box` |
| C++ binary | `/opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp` |
| Game ROMs | `/opt/magic_dingus_box/magic_dingus_box_cpp/data/roms/` |
| Thumbnails | `/opt/magic_dingus_box/magic_dingus_box_cpp/data/thumbnails/` |
| Playlists | `/opt/magic_dingus_box/magic_dingus_box_cpp/data/playlists/` |
| Video media | `/opt/magic_dingus_box/magic_dingus_box_cpp/data/media/` |
| Game saves | `/opt/magic_dingus_box/magic_dingus_box_cpp/data/saves/` |
| Settings | `/opt/magic_dingus_box/config/settings.json` |
| Device identity | `/opt/magic_dingus_box/magic_dingus_box_cpp/data/device_info.json` |
| RetroArch cores | `/home/magic/.config/retroarch/cores/` |
| PS1 BIOS | `/home/magic/.config/retroarch/system/scph5501.bin` |
| Web manager | `/opt/magic_dingus_box/magic_dingus_box/web/` |

---

## Services

| Service | Purpose |
|---------|---------|
| `magic-dingus-box-cpp.service` | Main kiosk application |
| `magic-dingus-web.service` | Web content manager (port 5000) |
| `magic-first-boot.service` | One-time setup on cloned Pis (disables itself) |
| `led-boot-sequence.service` | LED chase animation during boot |
| `led-shutdown-animation.service` | LED flicker during shutdown |
| `power-switch-check.service` | GPIO3 power switch check at boot |

### Useful Commands

```bash
# Check service status
sudo systemctl status magic-dingus-box-cpp
sudo systemctl status magic-dingus-web

# View logs
sudo journalctl -u magic-dingus-box-cpp -f
sudo journalctl -u magic-dingus-web -f

# Restart services
sudo systemctl restart magic-dingus-box-cpp
sudo systemctl restart magic-dingus-web
```

---

## Updating Deployed Units

### OTA Update (from the Pi)

```bash
# Check for updates
/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh check

# Install update
sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh install

# Rollback if something breaks
sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh rollback
```

### Manual Update (from your Mac)

```bash
# Push code changes + rebuild
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

---

## Troubleshooting

### Can't SSH to Pi
- Try `ping magicpi.local` — if no response, find IP on your router
- Ensure you're on the same WiFi network
- For cloned Pis, hostname is `magicpi-XXXX.local` (check Settings → Content Manager on the display)

### Kiosk doesn't start after boot
```bash
ssh magic@magicpi.local
sudo journalctl -u magic-dingus-box-cpp --no-pager -n 50
```

### No audio
```bash
ssh magic@magicpi.local
pactl info  # Check PulseAudio is running
pactl list sinks short  # Check available sinks
```

### PS1 games crash on launch
- Verify BIOS file exists: `ls /home/magic/.config/retroarch/system/scph5501.bin`
- Must be the SCPH-5501 BIOS (North America)

### Web manager not accessible
```bash
sudo systemctl status magic-dingus-web
sudo systemctl restart magic-dingus-web
```
