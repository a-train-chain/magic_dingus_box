# Data Synchronization Guide

## Overview

The Magic Dingus Box web interface and physical device share the same data directories, ensuring **real-time bidirectional synchronization**. Changes made in the web app appear on the device within 1-2 seconds, and vice versa.

## How Synchronization Works

### The PlaylistWatcher

Your Magic Dingus Box includes a `PlaylistWatcher` component that:
- Monitors the `playlists/` directory every **1.5 seconds**
- Detects file changes via modification timestamp (`st_mtime`)
- Automatically reloads playlists when changes are detected
- Runs in a background thread (daemon)

**Location:** `magic_dingus_box/library/watcher.py`

### Data Flow

```
Web Interface                    Device
     |                              |
     | POST /admin/playlists/x     |
     |----------------------------->|
     |                              |
     |      Save playlist.yaml      |
     |         (mtime changes)      |
     |                              |
     |                    PlaylistWatcher detects
     |                              |
     |                    Reloads playlists
     |                              |
     |                    Updates UI (1-2 seconds)
```

## YAML Format Compatibility

The web interface generates YAML files that **exactly match** the format expected by the Magic Dingus Box loader.

### Expected Format

```yaml
title: My Playlist
curator: Your Name
description: Optional description
loop: false
items:
  - title: Video Name
    source_type: local
    path: dev_data/media/video.mp4
    
  - title: Game Name
    source_type: emulated_game
    path: dev_data/roms/n64/game.n64
    emulator_core: parallel_n64_libretro
    emulator_system: N64
```

### Field Handling

**Required Fields:**
- `title` - Playlist title (string)
- `curator` - Creator name (string, defaults to "Unknown")
- `loop` - Loop playlist (boolean: `true` or `false`)
- `items` - Array of playlist items

**Item Required Fields:**
- `title` - Item title (string)
- `source_type` - Type: `"local"`, `"youtube"`, or `"emulated_game"`
- `path` - File path (for local/emulated_game)

**Optional Fields (only included if present):**
- `description` - Playlist description
- `url` - Video URL (for YouTube items)
- `start` - Start time in seconds (float)
- `end` - End time in seconds (float)
- `tags` - Array of tags (strings)
- `emulator_core` - Emulator core (for games)
- `emulator_system` - System name (for games)

### What Gets Cleaned Up

The web interface automatically removes:
- ❌ Empty arrays (`tags: []`)
- ❌ Null/undefined values
- ❌ Fields with no data
- ❌ Extra whitespace

This ensures YAML files are **clean and readable**, matching your existing playlists.

## Real-Time Sync Examples

### Creating a New Playlist

1. **Web Interface:** User creates "Summer Hits 2025"
2. **Backend:** Saves to `playlists/summer_hits_2025.yaml`
3. **File System:** File modification timestamp updates
4. **PlaylistWatcher:** Detects change within 1.5 seconds
5. **Device:** Reloads playlist library
6. **UI:** "Summer Hits 2025" appears in playlist selector

**Time to sync:** 1-2 seconds ⚡

### Editing an Existing Playlist

1. **Web Interface:** User clicks "Edit" on "N64 Classics"
2. **API:** Fetches `playlists/n64_classics.yaml` as JSON
3. **Form:** Populates with current data
4. **User:** Reorders games, adds Mario Kart
5. **Web Interface:** Saves updated playlist
6. **Backend:** Overwrites `n64_classics.yaml` with new data
7. **File System:** Modification timestamp updates
8. **PlaylistWatcher:** Detects change
9. **Device:** Reloads with new order

**Time to sync:** 1-2 seconds ⚡

### Deleting a Playlist

1. **Web Interface:** User clicks "Delete" on "Old Playlist"
2. **Confirmation:** User confirms deletion
3. **API:** `DELETE /admin/playlists/old_playlist.yaml`
4. **Backend:** Removes file from filesystem
5. **File System:** File no longer exists
6. **PlaylistWatcher:** Detects file is gone
7. **Device:** Removes playlist from library
8. **UI:** Playlist disappears from device

**Time to sync:** 1-2 seconds ⚡

### Uploading Media/ROMs

1. **Web Interface:** User drags video to upload zone
2. **API:** `POST /admin/upload` saves to `media/`
3. **File System:** New file appears
4. **Web Interface:** Refreshes media list
5. **Device:** Files are available for playlists

**Note:** Files are immediately available but won't appear in UI until added to a playlist.

## Data Directories

All content is stored in shared directories:

```
/data/  (or dev_data/ on macOS)
├── playlists/           ← YAML files watched by PlaylistWatcher
│   ├── danny_gatton.yaml
│   ├── n64_classics.yaml
│   └── wes_montgomery.yaml
├── media/               ← Video files
│   ├── concert1.mp4
│   └── concert2.mp4
└── roms/                ← ROM files
    ├── n64/
    │   ├── mario64.n64
    │   └── zelda.n64
    ├── nes/
    └── ps1/
```

## Sync Guarantees

### ✅ What IS Synchronized

- **Playlist creation** - New playlists appear on device
- **Playlist updates** - Changes to title, items, order
- **Playlist deletion** - Removed playlists disappear
- **File uploads** - Videos/ROMs available immediately
- **Metadata changes** - Title, curator, description, loop

### ⚠️ What Requires Page Refresh

- **Device list** - Discovering new devices (30-second auto-refresh)
- **Media list** - After uploading new videos
- **ROM list** - After uploading new ROMs
- **Existing playlists list** - After save/delete operations

These refresh automatically when you navigate between tabs or manually reload.

## Troubleshooting

### "Changes don't appear on device"

**Check:**
1. Is the Magic Dingus Box running?
2. Wait 1-2 seconds for PlaylistWatcher
3. Check console for YAML syntax errors
4. Verify file was actually saved (check filesystem)

**Solution:** PlaylistWatcher runs every 1.5 seconds. Be patient!

### "Playlist looks different on device"

**Cause:** YAML formatting might differ from display
**Solution:** The data is the same, just rendered differently

### "Deleted playlist still shows"

**Cause:** Web interface cache
**Solution:** Refresh the playlists tab or reload page

### "File upload succeeded but can't add to playlist"

**Cause:** Content lists need refresh
**Solution:** Switch tabs or click refresh button (if added)

## Technical Details

### PlaylistWatcher Implementation

```python
class PlaylistWatcher:
    def __init__(self, directory, on_change, interval_seconds=1.5):
        # Polls directory for changes every 1.5 seconds
        # Compares st_mtime (modification time) of files
        # Calls on_change() when difference detected
```

### File Detection

The watcher tracks:
- **New files** - Added to playlist library
- **Modified files** - Reloaded with new data
- **Deleted files** - Removed from library

### Sync Latency

- **Minimum:** 0 seconds (immediate file write)
- **Maximum:** 1.5 seconds (watcher poll interval)
- **Average:** ~0.75 seconds (halfway through poll cycle)

## Best Practices

### For Fast Sync

1. **Keep playlists small** - Faster to reload
2. **Use descriptive filenames** - Easier to track
3. **Avoid rapid edits** - Let sync complete between changes
4. **Monitor device logs** - Watch for reload messages

### For Clean YAML

1. **Let the web interface handle formatting** - Don't hand-edit YAML
2. **Use the Edit button** - Preserves format perfectly
3. **Fill in optional fields only when needed** - Keeps files clean
4. **Avoid special characters** - Stick to alphanumeric + spaces

### For Reliable Sync

1. **Check file permissions** - Ensure device can write to data dir
2. **Don't move files manually** - Use web interface or API
3. **Keep device running** - PlaylistWatcher needs to be active
4. **Monitor disk space** - Full disk prevents writes

## API Endpoints for Sync

### Playlist Operations

- `GET /admin/playlists` - List all (with metadata)
- `GET /admin/playlists/<name>` - Get full content
- `POST /admin/playlists/<name>` - Create or update
- `DELETE /admin/playlists/<name>` - Delete

### Media Operations

- `GET /admin/media` - List all videos
- `POST /admin/upload` - Upload video
- `DELETE /admin/media/<path>` - Delete video

### ROM Operations

- `GET /admin/roms` - List ROMs by system
- `POST /admin/upload/rom/<system>` - Upload ROM
- `DELETE /admin/roms/<system>/<file>` - Delete ROM

All operations trigger filesystem changes that PlaylistWatcher detects!

## Monitoring Sync

### Device Logs

Watch for these messages:
```
INFO: PlaylistWatcher started
DEBUG: Detected playlist change
INFO: Reloaded 5 playlists
```

### Web Console

Check browser console for:
```javascript
Playlist saved!
Device: Living Room Box
Response: {"ok": true, "filename": "my_playlist.yaml"}
```

## Future Enhancements

Possible improvements:
- WebSocket for instant notifications
- Optimistic UI updates (don't wait for sync)
- Conflict resolution (simultaneous edits)
- Sync status indicator in UI
- Manual refresh button

---

**The sync system works automatically!** Just make changes in the web interface and watch them appear on your device within 1-2 seconds. No manual intervention required! 🎉

