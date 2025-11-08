# Deploy Video Performance Fix to Raspberry Pi

## Quick Deployment Steps

### 1. SSH into Your Pi
```bash
ssh alexanderchaney@<your-pi-ip-address>
```

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
sudo nano /boot/config.txt
# Or on newer Pi OS:
sudo nano /boot/firmware/config.txt
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

## Need More Help?

See the comprehensive guide: `PI_VIDEO_PERFORMANCE_FIX.md`

