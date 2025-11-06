# Magic Dingus Box - Web Content Manager

## Overview

The Web Content Manager is a responsive web interface for managing your Magic Dingus Box remotely. You can discover multiple devices on your network, upload videos and ROMs, create and edit playlists, and sync content bidirectionally.

## Features

### Multi-Device Discovery
- Automatically discovers Magic Dingus Boxes on your local network
- Displays device stats (playlists, videos, ROMs)
- Switch between multiple devices seamlessly
- Manual IP entry fallback

### Video Management
- Upload videos with drag-and-drop support
- View all uploaded videos
- Real-time upload progress tracking
- Supports MP4, MKV, AVI, MOV, WebM

### ROM Management
- Upload ROMs for NES, SNES, N64, PlayStation
- Organized by system
- Accordion view for easy browsing
- Auto-detection of emulator cores

### Playlist Builder
- Create new playlists from scratch
- Edit existing playlists (pull from device)
- Drag-and-drop content to build playlists
- Drag-and-drop reordering of playlist items
- Mix videos and games in playlists
- Full metadata editing (title, curator, description, loop)
- Auto-detects emulator settings for ROMs

### Responsive Design
- Scales perfectly from mobile to desktop
- Touch-friendly on phones and tablets
- Retro CRT aesthetic matching device UI
- Neon colors and monospace fonts

## Getting Started

### 1. Start Your Magic Dingus Box

The web server starts automatically when you run the device:

```bash
python -m magic_dingus_box.main
```

The web interface will be available at: `http://localhost:8080`

### 2. Access from Another Device

From your phone, tablet, or laptop on the same WiFi network:

1. Find your device's IP address (shown in device discovery)
2. Open browser to: `http://192.168.x.x:8080`
3. The page will automatically discover all Magic Dingus Boxes

### 3. Select Your Device

- The page scans your network for devices
- Click on the device you want to manage
- All content now loads from that device

### 4. Upload Content

**Videos:**
1. Click "Videos" tab
2. Drag files to upload zone or click to browse
3. Watch upload progress
4. Videos appear immediately in list

**ROMs:**
1. Click "ROMs" tab
2. Select system (NES, SNES, N64, PS1)
3. Drag files or click to browse
4. ROMs are organized by system

### 5. Create Playlists

1. Click "Playlists" tab
2. Fill in playlist metadata (title, curator, description)
3. Toggle between Videos and ROMs in "Available Content"
4. Drag content to "Playlist Items" panel
5. Reorder items by dragging within playlist
6. Click "Save Playlist"
7. Playlist appears on device immediately!

### 6. Edit Existing Playlists

1. Scroll to "Existing Playlists"
2. Click "Edit" on any playlist
3. Form populates with existing data
4. Modify items, reorder, change metadata
5. Click "Save" to update
6. Device picks up changes automatically

## Device Identity

Each Magic Dingus Box automatically generates a unique ID on first run. You can customize the device name through the API:

```bash
curl -X POST http://192.168.x.x:8080/admin/device/name \
  -H "Content-Type: application/json" \
  -d '{"name": "Living Room Box"}'
```

## Architecture

### Backend (Flask)
- `magic_dingus_box/web/admin.py` - REST API endpoints
- Serves device info, playlists, media, ROMs
- JSON for API, YAML for playlist storage

### Frontend (Vanilla JS)
- `magic_dingus_box/web/static/index.html` - Structure
- `magic_dingus_box/web/static/style.css` - Retro styling
- `magic_dingus_box/web/static/manager.js` - Application logic

### Device Identity
- `magic_dingus_box/config.py` - UUID generation
- `device_info.json` - Persisted device identity

## API Endpoints

### Device Management
- `GET /admin/device/info` - Get device identity and stats
- `POST /admin/device/name` - Update device name

### Playlists
- `GET /admin/playlists` - List all playlists with metadata
- `GET /admin/playlists/<name>` - Get full playlist content
- `POST /admin/playlists/<name>` - Create or update playlist
- `DELETE /admin/playlists/<name>` - Delete playlist

### Media
- `GET /admin/media` - List all videos
- `POST /admin/upload` - Upload video file
- `DELETE /admin/media/<path>` - Delete video

### ROMs
- `GET /admin/roms` - List ROMs by system
- `POST /admin/upload/rom/<system>` - Upload ROM
- `DELETE /admin/roms/<system>/<filename>` - Delete ROM

## Playlist Format

The web interface creates YAML files compatible with your existing system:

```yaml
title: My Awesome Playlist
curator: Alex Chaney
description: Best games from the 90s
loop: false
items:
  - title: Super Mario 64
    source_type: emulated_game
    path: dev_data/roms/n64/Super Mario 64.n64
    emulator_core: parallel_n64_libretro
    emulator_system: N64
  - title: Concert Video
    source_type: local
    path: dev_data/media/concert.mp4
```

## Troubleshooting

### No Devices Found
- Make sure device is powered on
- Verify you're on the same WiFi network
- Try manual IP entry button
- Check firewall isn't blocking port 8080

### Upload Fails
- Ensure device has write access to data directories
- Check disk space on device
- Verify file formats are supported

### Playlist Not Appearing on Device
- Wait 1-2 seconds for PlaylistWatcher to detect changes
- Check device logs for YAML syntax errors
- Verify paths point to valid files

## Mobile Tips

- Use landscape mode on phones for better layout
- Tap and hold to drag items (may need to wait a moment)
- Pinch to zoom if text is too small
- Add to home screen for app-like experience

## Desktop Tips

- Drag files directly to upload zones
- Use keyboard to navigate forms
- Multiple file selection supported
- Right-click to inspect network requests

## Security Notes

- Web server binds to 0.0.0.0:8080 (all interfaces)
- Intended for local network use only
- No authentication in current version
- Don't expose to public internet without adding auth

## Future Enhancements

- mDNS/Bonjour auto-discovery (zeroconf installed)
- Thumbnail generation for videos
- Playlist templates
- Batch operations
- API key authentication
- YouTube URL support in playlists
- Video start/end time editor

## File Locations

```
magic_dingus_box/
├── config.py                    # Device identity system
├── main.py                      # Passes config to web app
├── web/
│   ├── admin.py                # Flask API with all endpoints
│   └── static/
│       ├── index.html          # Responsive web interface
│       ├── style.css           # Retro CRT styling
│       └── manager.js          # Device discovery & content management
```

## Configuration

The web admin is enabled by default. To disable:

```bash
export MAGIC_ENABLE_WEB_ADMIN=0
```

To change the port:

```bash
export MAGIC_ADMIN_PORT=9000
```

## Development

To test the web interface locally:

```bash
cd /Users/alexanderchaney/Documents/Projects/magic_dingus_box
python -m magic_dingus_box.main
```

Open browser to: `http://localhost:8080`

## Support

The web interface is fully compatible with your existing:
- Playlist format (YAML)
- Directory structure
- PlaylistWatcher (hot reload)
- Video/ROM paths
- Emulator configuration

No changes needed to your physical device setup!

