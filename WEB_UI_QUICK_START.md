# Web UI Quick Start Guide

## New User Interface - Simple & Focused

The Magic Dingus Box web interface is now organized into **two main tabs** with **collapsible sections** for easy navigation.

## Getting Started

### 1. Connect to Your Device

**Open:** `http://localhost:8080` (or your device's IP from your phone)

```
┌─────────────────────────────────────┐
│ ▼ Device Connection                 │
│   Not Connected                      │
├─────────────────────────────────────┤
│ 🔍 Searching for devices...         │
│                                      │
│ 📺 Magic Dingus Box                 │
│ 10.0.0.196 • macbook                │
│ 5 playlists • 7 videos • 38 ROMs    │
│                                      │
│ [Connect to IP Address]              │
└─────────────────────────────────────┘
```

**Actions:**
1. Wait for device to appear
2. Click on your device
3. Section **auto-collapses** after 1 second
4. Header shows: **"Device Connection: Magic Dingus Box"**

**To change devices:** Click header to reopen device selector

---

## 📹 Videos & Playlists Tab

Everything for managing videos and creating video playlists in ONE place!

### Upload Videos

```
┌─────────────────────────────────────┐
│ ▼ Upload Videos                     │
├─────────────────────────────────────┤
│ [Drag & Drop Zone]                  │
│ Choose video files or drag & drop   │
└─────────────────────────────────────┘
```

**Actions:**
1. Click header to expand
2. Drag video files or click to browse
3. Watch upload progress
4. **Collapse when done** to reduce clutter

### Video Library

```
┌─────────────────────────────────────┐
│ ▼ Video Library (7)                 │
├─────────────────────────────────────┤
│ [Grid of video cards]               │
│ 📹 Concert 1.mp4 (125 MB)           │
│ 📹 Concert 2.mp4 (98 MB)            │
└─────────────────────────────────────┘
```

**Shows:** All your uploaded videos with file sizes

### Create / Edit Video Playlist

```
┌─────────────────────────────────────┐
│ ▼ Create / Edit Video Playlist      │
├─────────────────────────────────────┤
│ Title: [My Playlist]                │
│ Curator: [Your Name]                │
│ Description: [Optional]             │
│ ☐ Loop playlist                     │
│                                      │
│ Available Videos  │ Playlist Items  │
│ 📹 Video 1        │ (drag here)     │
│ 📹 Video 2        │                 │
│                                      │
│ [Save Playlist] [Cancel]            │
└─────────────────────────────────────┘
```

**Actions:**
1. Fill in playlist info
2. Drag videos from left → right
3. Click "Edit" on items to change title/artist
4. Reorder by dragging within playlist
5. Click "Save Playlist"

**Mobile:** Long press (0.3s) to drag items

### Video Playlists

```
┌─────────────────────────────────────┐
│ ▼ Video Playlists (3)               │
├─────────────────────────────────────┤
│ ┌─────────────────────────────────┐ │
│ │ Danny Gatton        [Edit] [Del]│ │
│ │ By Alex • 4 items               │ │
│ └─────────────────────────────────┘ │
└─────────────────────────────────────┘
```

**Actions:**
- Click **Edit** → Loads into builder above
- Click **Delete** → Confirms and removes

---

## 🎮 ROMs & Games Tab

Everything for managing ROMs and creating game playlists!

### Upload ROMs

```
┌─────────────────────────────────────┐
│ ▼ Upload ROMs                       │
├─────────────────────────────────────┤
│ System: [N64 ▼]                     │
│ [Drag & Drop Zone]                  │
│ Choose ROM files                    │
└─────────────────────────────────────┘
```

**Actions:**
1. Select system (NES, SNES, N64, PS1)
2. Drag ROM files or click to browse
3. ROMs organized by system automatically

### ROM Library

```
┌─────────────────────────────────────┐
│ ▼ ROM Library (38)                  │
├─────────────────────────────────────┤
│ ▼ N64 (5)                           │
│   Super Mario 64.n64                │
│   Zelda OOT.n64                     │
│                                      │
│ ▼ NES (4)                           │
│   Super Mario Bros 3.nes            │
└─────────────────────────────────────┘
```

**Shows:** ROMs organized by system (accordion view)

### Create / Edit Game Playlist

```
┌─────────────────────────────────────┐
│ ▼ Create / Edit Game Playlist       │
├─────────────────────────────────────┤
│ Title: [N64 Favorites]              │
│ Curator: [Your Name]                │
│ Description: [Optional]             │
│ ☐ Loop playlist                     │
│                                      │
│ Available ROMs    │ Playlist Items  │
│ N64               │                 │
│ 🎮 Mario 64       │ (drag here)     │
│ 🎮 Zelda OOT      │                 │
│                                      │
│ [Save Playlist] [Cancel]            │
└─────────────────────────────────────┘
```

**Actions:**
- Same workflow as video playlists
- Drag games instead of videos
- System info auto-detected

### Game Playlists

```
┌─────────────────────────────────────┐
│ ▼ Game Playlists (2)                │
├─────────────────────────────────────┤
│ ┌─────────────────────────────────┐ │
│ │ N64 Classics        [Edit] [Del]│ │
│ │ By Alex • 5 items               │ │
│ └─────────────────────────────────┘ │
└─────────────────────────────────────┘
```

**Shows:** Only game playlists (no video playlists cluttering!)

---

## Tips & Tricks

### Collapse Management

**Collapse everything except what you're working on:**
- Uploading? → Close libraries and playlists
- Browsing? → Open library, close upload
- Building playlist? → Close everything else

**Quick collapse:**
- Click any section header to toggle
- Arrow icon shows state (▼ open, ► closed)

### Mobile Optimization

**Best practices on phone:**
1. **Portrait mode:** Sections stack nicely
2. **Landscape mode:** Better for playlist builder (dual panels)
3. **Collapse aggressively:** Keep only 1-2 sections open
4. **Use count badges:** Know what's available without opening

### Desktop Power User

**Maximize screen space:**
1. Keep device selector collapsed (you're connected!)
2. Open library sections to see content
3. Open playlist builder when creating
4. Keep existing playlists open to see what you have

### Editing Playlists

**Seamless workflow:**
1. Find playlist in "Video Playlists" or "Game Playlists"
2. Click **Edit**
3. Builder section **auto-expands** (if collapsed)
4. Form **pre-filled** with current data
5. Items **loaded** into playlist
6. Make changes
7. Click **Save**
8. Builder clears, ready for next playlist

## Keyboard Navigation

**Tab through fields:**
- Title → Curator → Description → Loop checkbox
- Tab into Available Content → Arrow keys to select
- Tab into Playlist Items → Arrow keys to navigate

**Enter to save:**
- Focus on Save button → Press Enter

## Common Workflows

### Upload and Create Playlist (Videos)

1. ▼ Upload Videos → Upload files → Collapse
2. ▼ Video Library → Verify upload → Leave open
3. ▼ Create Video Playlist → Expand
4. Drag videos from library to playlist
5. Fill in metadata
6. Save!

**Time:** 2-3 minutes for 5-video playlist

### Manage Existing Playlists

1. ▼ Video Playlists or ▼ Game Playlists
2. Browse your playlists
3. Click Edit → Auto-expands builder
4. Modify → Save
5. Builder auto-clears

**Time:** 30 seconds to reorder or add items

### Bulk Upload ROMs

1. ▼ Upload ROMs → Expand
2. Select system (e.g., N64)
3. Drag 10 ROM files
4. Upload completes
5. ▼ ROM Library → See all ROMs organized
6. Collapse upload when done

**Time:** 1-2 minutes depending on file sizes

## Visual Guide

### Collapsed Section
```
► Upload Videos
```
_Click to expand_

### Expanded Section
```
▼ Upload Videos
├─────────────────
│ [Content here]
└─────────────────
```
_Click to collapse_

### With Count Badge
```
▼ Video Library (12)
```
_12 videos available_

### With Status
```
► Device Connection
  Magic Dingus Box ✓
```
_Connected, click to reopen_

## Need Help?

**Something not working?**
1. Check if device is connected (reopen Device Connection)
2. Try refreshing the page
3. Check browser console (F12) for errors
4. Verify Magic Dingus Box is running

**Can't find a feature?**
- Playlists moved **into** Videos/ROMs tabs
- Everything is now **context-specific**
- Use **collapse/expand** to navigate

---

**Enjoy the new, cleaner interface!** 🎉

Less scrolling, better organization, easier to use on any device!

