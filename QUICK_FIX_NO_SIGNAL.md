# Fix "No Signal" on Monitor

If your monitor shows "No Signal" after booting your Pi, it's a boot configuration issue. Follow these steps:

## Quick Fix (Run on Your Pi)

```bash
ssh alexanderchaney@magicpi.local

cd ~/magic_dingus_box
git pull origin main

# Run the HDMI fix script
./scripts/fix_hdmi.sh

# Reboot (REQUIRED!)
sudo reboot
```

## What This Fixes

The script automatically:
- ✅ Sets GPU memory to 512MB (required for video decoding)
- ✅ Enables H.264 hardware codec
- ✅ **Forces HDMI port 0 output** (port closest to power/USB-C)
- ✅ Sets resolution to 1920x1080 @ 60Hz
- ✅ Disables any conflicting composite video settings
- ✅ Creates a backup of your original config

**Note:** Make sure your HDMI cable is plugged into **HDMI port 0** (the port closest to the power/USB-C port on Pi 4/5)!

## After Reboot

Your monitor should show:
1. ✅ Boot messages
2. ✅ Desktop (with auto-login)
3. ✅ Magic Dingus Box UI automatically

## Verify It Worked

```bash
ssh alexanderchaney@magicpi.local

# Check GPU memory
vcgencmd get_mem gpu
# Should show: gpu=512M

# Check HDMI status
tvservice -s
# Should show active HDMI mode

# Check services are running
sudo systemctl status magic-ui.service magic-mpv.service
```

## If You Still Have Issues

### Issue: Different resolution needed

Edit `/boot/firmware/config.txt` and change `hdmi_mode`:

```bash
sudo nano /boot/firmware/config.txt
```

**Common HDMI modes:**
- `hdmi_mode=82` - 1920x1080 @ 60Hz (default)
- `hdmi_mode=85` - 1280x720 @ 60Hz
- `hdmi_mode=16` - 1024x768 @ 60Hz
- `hdmi_mode=4` - 1280x720 @ 60Hz (CEA)

Full list: `tvservice -m CEA` or `tvservice -m DMT`

### Issue: Monitor still says "no signal"

Try different HDMI cable or port, then:

```bash
# Check if Pi detects display
tvservice -s

# Force different mode
sudo tvservice -e "DMT 82"
fbset -depth 32
```

### Issue: Services running but no UI visible

Enable auto-login:

```bash
sudo raspi-config nonint do_boot_behaviour B4
sudo reboot
```

## Manual Boot Config (If Script Fails)

```bash
sudo nano /boot/firmware/config.txt
```

Add these lines:

```
# GPU and video decoding
gpu_mem=512
start_x=1

# Force HDMI port 0 output (port closest to power)
hdmi_force_hotplug:0=1
hdmi_drive:0=2
hdmi_group:0=2
hdmi_mode:0=82

# Disable composite (if present)
#enable_tvout=1
#sdtv_mode=0
```

Save (Ctrl+O, Enter) and exit (Ctrl+X), then:

```bash
sudo reboot
```

## Testing Hardware Decoding

After fixing HDMI, verify videos play smoothly:

```bash
# Check hardware decoding is active
journalctl -u magic-mpv.service | grep -i 'hwdec\|hardware\|v4l2'

# Should see:
# "Using hardware decoding (v4l2m2m)"
```

Your videos should now:
- ✅ Play smoothly without lag
- ✅ Audio and video perfectly in sync
- ✅ No stuttering or dropped frames

## Need More Help?

See full guides:
- `PI_VIDEO_PERFORMANCE_FIX.md` - Video performance troubleshooting
- `DEPLOY_TO_PI.md` - Complete deployment guide
- `boot_config_template.txt` - Full boot config reference

