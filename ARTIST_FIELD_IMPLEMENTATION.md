# Artist Field Implementation

## Overview

Added an `artist` field to playlist items, positioned right after `title`. This allows proper separation of song titles and artist names for music video playlists.

## What Changed

### Data Model Updates

**1. `magic_dingus_box/library/models.py`**
- Added `artist: Optional[str] = None` field to `PlaylistItem` dataclass
- Positioned after `source_type` for logical ordering

**2. `magic_dingus_box/library/loader.py`**
- Updated playlist parser to read `artist` field from YAML
- Added `artist=raw.get("artist")` to item parsing

### Backend Updates

**3. `magic_dingus_box/web/admin.py`**
- Updated `format_playlist_yaml()` function
- Always includes `artist` field right after `title`
- Outputs `artist: ''` if empty for consistency

### Frontend Updates

**4. `magic_dingus_box/web/static/manager.js`**

**Added/Updated Functions:**
- `addItemToPlaylist()` - Includes empty artist field by default
- `cleanPlaylistItems()` - Always includes artist (even if empty)
- `renderPlaylistItems()` - Displays artist name in playlist items
- `editPlaylistItem()` - NEW function to edit title and artist via prompts

**UI Changes:**
- Playlist items now show: `[Icon] Title - Artist`
- Added "Edit" button to each playlist item
- Added "Remove" button (replaces old "✕ Remove" text)
- Artist displayed in italics, dimmed color
- Shows "No artist" if empty

**5. `magic_dingus_box/web/static/style.css`**

**New Styles:**
- `.playlist-item-content` - Flex container for item layout
- `.playlist-item-info` - Contains icon, title, artist
- `.item-icon` - Song/game icon (📹/🎮)
- `.item-title` - Bold, teal color
- `.item-artist` - Italic, gray, smaller font
- `.playlist-item-actions` - Container for Edit/Remove buttons
- `.btn-edit` - Teal accent button
- `.btn-remove` - Red error button

**Mobile Responsive:**
- Stacks title/artist vertically on small screens
- Larger touch targets (44px min) for buttons
- Text wraps on mobile instead of truncating

### Existing Playlists Updated

All 5 playlists updated with `artist` field:

**Video Playlists:**
1. **danny_gatton.yaml**
   - Properly separated titles and artists
   - e.g., `title: Back Home In Indiana` + `artist: Vince Gill with Danny Gatton...`

2. **wes_montgomery.yaml**
   - Cleaned up titles (removed artist names)
   - e.g., `title: Windy` + `artist: Wes Montgomery`

**Game Playlists:**
3. **n64_classics.yaml** - Added `artist: ''` (games don't have artists)
4. **ps1_collection.yaml** - Added `artist: ''`
5. **retro_games.yaml** - Added `artist: ''`

## YAML Structure

### New Format

```yaml
title: Playlist Name
curator: Curator Name
description: ''
loop: false
items:
  - title: Song Title
    artist: Artist Name      # ← NEW FIELD
    source_type: local
    path: dev_data/media/video.mp4
```

### Field Order

For each item:
1. `title` - Song/video/game title
2. `artist` - Artist name (right after title)
3. `source_type` - Type of content
4. `path` - File path
5. Other optional fields...

## User Workflow

### Creating New Playlists

1. Drag videos/ROMs into playlist
2. Items appear with empty artist field
3. Click "Edit" button on any item
4. Enter title and artist in prompts
5. Save playlist - artist included in YAML

### Editing Existing Playlists

1. Click "Edit" on playlist
2. Items load with current artist data
3. Click "Edit" on individual items
4. Update title and/or artist
5. Save - changes sync to device

### Mobile Usage

- Touch and hold to drag items
- Tap "Edit" button to edit title/artist
- Prompts appear for text entry
- Large 44px buttons for easy tapping

## Example Usage

### Video Playlist

**Before (filename-based):**
```yaml
- title: Wes Montgomery - Windy.mp4
  source_type: local
  path: dev_data/media/Wes Montgomery - Windy.mp4
```

**After (separated fields):**
```yaml
- title: Windy
  artist: Wes Montgomery
  source_type: local
  path: dev_data/media/Wes Montgomery - Windy.mp4
```

### Game Playlist

```yaml
- title: Super Mario 64
  artist: ''                           # Empty for games
  source_type: emulated_game
  path: dev_data/roms/n64/Super Mario 64.n64
  emulator_core: parallel_n64_libretro
  emulator_system: N64
```

## UI Display

### Desktop View

```
[📹] Windy - Wes Montgomery                    [Edit] [✕]
[📹] Four On Six (1965) - Wes Montgomery       [Edit] [✕]
[🎮] Super Mario 64 - No artist                [Edit] [✕]
```

### Mobile View (Stacked)

```
[📹] Windy
     Wes Montgomery                            [Edit] [✕]
     
[🎮] Super Mario 64
     No artist                                  [Edit] [✕]
```

## Benefits

✅ **Cleaner titles** - No more "Artist - Song.mp4" filenames  
✅ **Proper metadata** - Artist info stored separately  
✅ **Better display** - Title and artist shown distinctly  
✅ **Easy editing** - Click Edit to update title/artist  
✅ **Consistent structure** - All playlists have artist field  
✅ **Backward compatible** - Empty artist is valid  

## Future Enhancements

Possible improvements:
- [ ] Inline editing (click title/artist to edit directly)
- [ ] Auto-populate artist from filename patterns
- [ ] Artist autocomplete based on existing entries
- [ ] Bulk edit multiple items at once
- [ ] Album field for additional metadata
- [ ] Year/genre fields

## Testing Checklist

- [x] Artist field appears in YAML output
- [x] Loader parses artist field correctly
- [x] Web UI displays artist in playlist items
- [x] Edit button opens prompts for title/artist
- [x] Changes save to YAML properly
- [x] Empty artist shows as "No artist"
- [x] Mobile layout stacks title/artist
- [x] Touch targets are large enough (44px)
- [x] Existing playlists load with artist data
- [x] New items default to empty artist
- [x] Games show empty artist appropriately

---

**Implementation complete!** Your playlists now properly separate song titles and artist names. 🎵

