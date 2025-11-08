# Raspberry Pi Boot Configuration

## Required for All Setups (HDMI or Composite)

Edit `/boot/config.txt` (or `/boot/firmware/config.txt` on newer Pi OS):

```bash
sudo nano /boot/config.txt
```

**Add these lines (REQUIRED for video playback):**
```
# GPU memory for hardware video decoding
gpu_mem=512

# Enable H.264 hardware codec
start_x=1
```

## HDMI Output (Default/Recommended)

No additional configuration needed. HDMI works by default with the GPU settings above.

## NTSC Composite Output (Retro TV)

**Add these additional lines for composite output:**
```
enable_tvout=1
sdtv_mode=0       # NTSC
sdtv_aspect=1     # 4:3
hdmi_ignore_hotplug=1
```

## After Editing

```bash
# Save and reboot
sudo reboot

# After reboot, verify GPU memory
vcgencmd get_mem gpu
# Should show: gpu=512M
```

See `boot_config_template.txt` for complete configuration options and explanations.

