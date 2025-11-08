NTSC Composite Output (Raspberry Pi)

Edit `/boot/config.txt` with the following lines:

enable_tvout=1
sdtv_mode=0       # NTSC
sdtv_aspect=1     # 4:3
hdmi_ignore_hotplug=1

Reboot after changes. The `magic-mpv.service` starts mpv fullscreen; audio is sent to the USB DAC (`hw:1,0`).

