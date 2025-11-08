# Raspberry Pi Video Performance Fix

## Problem
Audio plays normally but video lags behind - this indicates the CPU cannot decode video fast enough.

## Root Causes
1. **Software video scaling** - The old config used CPU-intensive filters
2. **Insufficient hardware acceleration** - Not fully utilizing GPU video decoder
3. **Limited decoder threads** - Only 2 threads for 720p video
4. **Insufficient GPU memory** - Pi needs adequate GPU memory for video decoding

## Solutions Applied

### 1. Optimized MPV Configuration
The `systemd/magic-mpv.service` has been updated with performance-optimized settings:

**Removed:**
- `--vf=scale=...` - This forced software scaling (very slow!)
- `--hwdec=auto-safe` - Too conservative
- `--vd-lavc-threads=2` - Not enough for 720p

**Added:**
- `--hwdec=auto` - Enables best hardware decoder available
- `--hwdec-codecs=all` - Use hardware for all supported codecs
- `--gpu-context=drm` - Direct rendering for better performance
- `--vd-lavc-threads=4` - More decoder threads
- `--video-sync=audio` - Sync video to audio (prevents drift)
- `--interpolation=no` - Disable frame interpolation (saves CPU)
- `--profile=fast` - Use fast decoding profile

## Required Steps on Your Pi

### Step 1: Check GPU Memory Allocation
Raspberry Pi needs adequate GPU memory for video decoding:

```bash
# Check current GPU memory
vcgencmd get_mem gpu
```

**For 720p video, you need at least 128MB GPU memory.**

If it's less, edit `/boot/config.txt` (or `/boot/firmware/config.txt` on newer Pi OS):

```bash
sudo nano /boot/config.txt
```

Add or modify:
```
gpu_mem=256
```

For Pi 4 or Pi 5 with 4GB+ RAM, use 256MB or even 512MB:
```
gpu_mem=512
```

### Step 2: Check Hardware Acceleration Support

```bash
# Check if H.264 hardware decoder is available
mpv --hwdec=help
```

You should see options like:
- `v4l2m2m` (Raspberry Pi 3/4)
- `drm` (Pi 4/5)
- `mmal` (older Pi models)

### Step 3: Verify H.264 Codec Support

```bash
# Check if your Pi's GPU supports H.264 decode
vcgencmd codec_enabled H264
```

Should return: `H264=enabled`

If disabled, add to `/boot/config.txt`:
```
start_x=1
```

### Step 4: Update MPV Service

On your Pi, copy the updated service file and reload:

```bash
# Copy the updated service file to Pi
sudo cp systemd/magic-mpv.service /etc/systemd/system/

# Reload systemd
sudo systemctl daemon-reload

# Restart the service
sudo systemctl restart magic-mpv.service
sudo systemctl restart magic-ui.service

# Check for errors
sudo systemctl status magic-mpv.service
journalctl -u magic-mpv.service -f
```

### Step 5: Test Hardware Decoding

Run this test command on your Pi to verify hardware decoding works:

```bash
mpv --hwdec=auto --vo=gpu /path/to/your/test/video.mp4
```

Watch for these in the console output:
```
Using hardware decoding (v4l2m2m-copy)  # or similar
```

## Alternative: If Hardware Acceleration Fails

If hardware acceleration still doesn't work, you have three options:

### Option A: Re-encode Videos to Lower Resolution
Convert your videos to 480p which the Pi can decode more easily:

```bash
# Install ffmpeg if needed
sudo apt-get install ffmpeg

# Convert to 480p with faster encoding
ffmpeg -i input.mp4 -vf scale=-2:480 -c:v libx264 -preset fast -crf 23 -c:a copy output.mp4
```

### Option B: Use Software Decoder with Optimizations
If hardware decoder is broken, force software with more threads:

Edit `systemd/magic-mpv.service` and change:
```
--hwdec=auto \
--vd-lavc-threads=4 \
```
to:
```
--hwdec=no \
--vd-lavc-threads=4 \
--vd-lavc-skiploopfilter=all \
--vd-lavc-fast \
```

### Option C: Use H.264 Baseline Profile Videos
Re-encode videos using H.264 Baseline profile (easier to decode):

```bash
ffmpeg -i input.mp4 -c:v libx264 -profile:v baseline -level 3.0 -preset fast -crf 23 -c:a copy output.mp4
```

## Monitoring Performance

After making changes, check mpv stats during playback:

```bash
# In mpv, press 'i' to show stats
# Or check from command line:
journalctl -u magic-mpv.service -f | grep -i "drop\|fps\|decode"
```

Look for:
- **Dropped frames** - Should be 0 or very low
- **Decode time** - Should be under 16ms for 60fps
- **Hardware decoding active** - Should show v4l2m2m or similar

## Expected Results

After these fixes:
- ✅ Video should play smoothly without lag
- ✅ Audio and video should stay in sync
- ✅ No dropped frames
- ✅ Lower CPU usage (check with `htop`)

## Raspberry Pi Model-Specific Notes

### Pi 3 / 3B+
- Use `--hwdec=mmal` or `--hwdec=v4l2m2m`
- 720p H.264 should work but may struggle
- Consider 480p videos for best results
- Set `gpu_mem=256`

### Pi 4 / 4B
- Use `--hwdec=v4l2m2m` or `--hwdec=auto`
- 720p and 1080p H.264 should work well
- Set `gpu_mem=256` or `gpu_mem=512`

### Pi 5
- Use `--hwdec=auto` (best support)
- Can handle 1080p easily, even 4K H.264
- Set `gpu_mem=512`

## Additional Troubleshooting

### Check Current GPU Memory
```bash
vcgencmd get_mem gpu
vcgencmd get_mem arm
```

### Monitor Temperature (Throttling)
```bash
vcgencmd measure_temp
vcgencmd get_throttled
```

If temperature is high (>80°C), your Pi might be thermal throttling. Add cooling.

### Check MPV Logs
```bash
journalctl -u magic-mpv.service --since "5 minutes ago"
```

Look for errors like:
- "Failed to open VDPAU backend"
- "Hardware decoding not available"
- "Decoder format not supported"

## Quick Test Command

Test your video with optimized settings:

```bash
mpv --hwdec=auto --vo=gpu --vd-lavc-threads=4 --video-sync=audio \
    --profile=fast --hwdec-codecs=all ~/path/to/video.mp4
```

If this plays smoothly, the service configuration should work!

