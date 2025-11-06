# Hardened systemd units (optional)

These unit files provide a more locked-down runtime. They are opt-in and do not replace the defaults.

## Files
- `magic-mpv.hardened.service`
- `magic-ui.hardened.service`

Key hardening options:
- Run as non-root user `magic`/group `magic`
- `PrivateTmp=true`, `ProtectSystem=full`, `ProtectHome=true`, `NoNewPrivileges=true`
- Restrict writes to `/data` via `ReadWritePaths=/data`
- Use `EnvironmentFile=/etc/magic_dingus_box.env`

## Example /etc/magic_dingus_box.env
```
# Audio device for mpv
AUDIO_DEVICE=alsa:device=hw:1,0
#AUDIO_DEVICE=alsa/sysdefault:CARD=RP1AudioOut

# Web admin
MAGIC_ENABLE_WEB_ADMIN=0
MAGIC_ADMIN_PORT=8080
#MAGIC_ADMIN_TOKEN=secret123
MAGIC_MAX_UPLOAD_MB=2048

# Data / IPC
MAGIC_DATA_DIR=/data
MPV_SOCKET=/run/magic/mpv.sock

# Display
MAGIC_DISPLAY_MODE=crt_native
MAGIC_MODERN_RES=auto
```

## Usage
1. Create `magic` system user and `/data` directories.
2. Copy the hardened service files to `/etc/systemd/system/`.
3. Create `/etc/magic_dingus_box.env` with the values above (adjust for your device).
4. `sudo systemctl daemon-reload`
5. `sudo systemctl enable magic-mpv.hardened.service magic-ui.hardened.service`
6. `sudo systemctl start magic-mpv.hardened.service magic-ui.hardened.service`

> Tip: Keep the default units available for debugging or development.

