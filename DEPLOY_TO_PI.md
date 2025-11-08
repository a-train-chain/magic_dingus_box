# Deploy Video Performance Fix to Raspberry Pi

## 🚀 One-Command Update (Fastest!)

```bash
# SSH to your Pi, then run:
cd ~/magic_dingus_box && ./scripts/update_pi.sh
```

This single script will:
- ✅ Pull latest changes from GitHub
- ✅ Check GPU memory allocation
- ✅ Deploy updated service files
- ✅ Restart all services
- ✅ Show status and verify everything works

**First time? You may need to make it executable:**
```bash
chmod +x ~/magic_dingus_box/scripts/update_pi.sh
```

---

## Manual Deployment Steps

If you prefer to do it step-by-step:

### 1. SSH into Your Pi

**Option A: Using Hostname (Easiest - No IP needed!)**
```bash
# Default Raspberry Pi hostname
ssh alexanderchaney@magicpi.local

# Or if you changed your hostname
ssh alexanderchaney@<your-hostname>.local
```

**Option B: Using IP Address**
```bash
ssh alexanderchaney@<your-pi-ip-address>
```

**Option C: Find Your Pi's IP Address**
```bash
# On Mac/Linux, scan your network
arp -a | grep -i "b8:27:eb\|dc:a6:32\|e4:5f:01"  # Raspberry Pi MAC prefixes

# Or use nmap (install with: brew install nmap)
nmap -sn 192.168.1.0/24 | grep -B 2 "Raspberry Pi"
```

**Option D: Physical Access (No Network Needed)**
- Connect keyboard and monitor directly to your Pi
- Login at the console
- Then follow the same steps below

### 2. Navigate to Your Project Directory
```bash
cd /path/to/magic_dingus_box  # Adjust to your actual path
```

### 3. Pull Latest Changes from GitHub
```bash
git pull origin main
```

This will pull:
- Updated `systemd/magic-mpv.service` with optimized settings
- New `PI_VIDEO_PERFORMANCE_FIX.md` troubleshooting guide

### 4. Check GPU Memory (Critical!)
```bash
vcgencmd get_mem gpu
```

**If it shows less than 128MB, you MUST increase it:**

```bash
# Find which config file exists on your system:
ls /boot/config.txt /boot/firmware/config.txt 2>/dev/null

# Edit the one that exists (newer Pi OS uses /boot/firmware):
sudo nano /boot/firmware/config.txt
# OR for older Pi OS:
sudo nano /boot/config.txt
```

Add or modify this line:
```
gpu_mem=256
```

Save (Ctrl+O, Enter) and exit (Ctrl+X).

**If you changed GPU memory, reboot now:**
```bash
sudo reboot
```

Wait for Pi to reboot, then SSH back in.

### 5. Deploy Updated Service File
```bash
cd /path/to/magic_dingus_box

# Copy updated service to system
sudo cp systemd/magic-mpv.service /etc/systemd/system/

# Reload systemd to recognize changes
sudo systemctl daemon-reload

# Restart services
sudo systemctl restart magic-mpv.service
sudo systemctl restart magic-ui.service
```

### 6. Verify Services Started Successfully
```bash
# Check MPV service status
sudo systemctl status magic-mpv.service

# Check UI service status
sudo systemctl status magic-ui.service
```

Both should show "active (running)" in green.

### 7. Monitor for Issues
```bash
# Watch live logs
journalctl -u magic-mpv.service -f
```

Look for:
- ✅ "Using hardware decoding" (good!)
- ❌ "Failed to initialize" (bad - see troubleshooting)
- ❌ "decoder format not supported" (bad - see troubleshooting)

Press Ctrl+C to exit log view.

### 8. Test Video Playback

Play one of your videos and verify:
- ✅ Video plays smoothly without lag
- ✅ Audio and video stay in sync
- ✅ No stuttering or dropped frames

## If Video Still Lags

Read the full troubleshooting guide:
```bash
cat PI_VIDEO_PERFORMANCE_FIX.md
```

Or view it on GitHub: [PI_VIDEO_PERFORMANCE_FIX.md](PI_VIDEO_PERFORMANCE_FIX.md)

## Quick Hardware Acceleration Test

Test hardware decoding manually:
```bash
mpv --hwdec=auto --vo=gpu ~/dev_data/media/intro.mp4
```

Watch the terminal output for "Using hardware decoding" message.

## Common Issues

### Issue: "Device not found" for HDMI audio
**Solution:** Check your HDMI audio device name:
```bash
aplay -l
```

Look for your HDMI device and update `/etc/magic_dingus_box.env` if needed.

### Issue: "Failed to initialize video decoder"
**Solution:** Check if H.264 codec is enabled:
```bash
vcgencmd codec_enabled H264
```

Should return `H264=enabled`. If not, add to `/boot/config.txt`:
```
start_x=1
```

Then reboot.

### Issue: High CPU usage, still laggy
**Solution:** Your videos might be too high resolution. Convert to 480p:
```bash
ffmpeg -i input.mp4 -vf scale=-2:480 -c:v libx264 -preset fast -crf 23 -c:a copy output.mp4
```

## Rollback (If Needed)

If the new settings cause issues, rollback to previous version:

```bash
cd /path/to/magic_dingus_box
git log --oneline  # See recent commits
git checkout <previous-commit-hash> systemd/magic-mpv.service
sudo cp systemd/magic-mpv.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl restart magic-mpv.service
```

## Success Checklist

- [ ] GPU memory is 256MB or higher
- [ ] Git pull completed successfully
- [ ] Service files copied to `/etc/systemd/system/`
- [ ] Systemd daemon reloaded
- [ ] Both services restarted without errors
- [ ] Hardware decoding confirmed in logs
- [ ] Video plays smoothly with audio in sync

## Finding or Setting Your Pi's Hostname

### Check Current Hostname (On the Pi)
```bash
hostname
# Or
hostname -I  # Shows IP address
```

### Set a Custom Hostname (Optional)
```bash
# Use raspi-config
sudo raspi-config
# Navigate to: System Options > Hostname

# Or edit directly
sudo hostnamectl set-hostname my-magic-box
sudo reboot
```

After setting hostname, you can SSH with:
```bash
ssh alexanderchaney@my-magic-box.local
```

### Why .local Works
Raspberry Pi OS includes Avahi/mDNS which broadcasts the hostname on your local network. The `.local` suffix tells your computer to look for the device via mDNS instead of DNS.

**Requirements:**
- Pi must be on the same network as your computer
- mDNS/Avahi must be running on Pi (enabled by default)
- Your Mac/Linux supports mDNS (built-in on macOS, Avahi on Linux)

**If .local doesn't work:**
- Wait 30 seconds after Pi boots for mDNS to advertise
- Check if Pi is on the same network/subnet
- On some routers, mDNS is blocked - use IP address instead
- Windows users: Install Bonjour Print Services for .local support

## Need More Help?

See the comprehensive guide: `PI_VIDEO_PERFORMANCE_FIX.md`

