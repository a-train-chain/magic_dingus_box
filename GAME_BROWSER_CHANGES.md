# Game Browser Implementation - Changes Summary

## What Changed

The game playlists are now separated from video playlists for a cleaner user experience.

### User Experience

**Before:**
- All playlists (videos and games) showed in the main UI
- Games and videos mixed together

**After:**
- Main UI shows ONLY video playlists
- Game playlists accessed through: Settings → Video Games → Browse Game Libraries
- **Two-level game browsing**: Select system → See individual games → Launch specific game
- Clean separation between video and game content

### How It Works

1. **Automatic Categorization**: Playlists are automatically categorized when loaded:
   - **Game Playlists**: Contain ONLY `emulated_game` items
   - **Video Playlists**: Contain ANY `local` or `youtube` items

2. **Individual Game Selection** (Different from video playlists!): 
   - Press button 4 to open settings
   - Navigate to "Video Games" → "Browse Game Libraries"
   - See game systems/collections (NES, N64, PS1, etc.)
   - Select a system to view all games in that system
   - Select a specific game to launch it
   - Game exits → Returns to main UI (no auto-progression like videos)

3. **Mixed Playlists**: If you mix videos and games in one playlist, it appears in the main UI (since it has video content)

## Files Modified

### `/magic_dingus_box/library/models.py`
- Added `is_game_playlist()` method: Returns True if playlist contains ONLY games
- Added `is_video_playlist()` method: Returns True if playlist contains ANY videos

### `/magic_dingus_box/ui/settings_menu.py`
- Added `BROWSE_GAMES` to MenuSection enum
- Added game browser state tracking: `game_browser_active`, `game_browser_selected`
- **Added individual game viewing state**: `viewing_games_in_playlist`, `current_game_playlist_index`, `selected_game_in_playlist`
- Updated `navigate()` to handle three levels: menus, systems, and individual games
- Added `enter_game_browser()`, `exit_game_browser()`, `enter_game_list()`, and `exit_game_list()` methods
- Updated Video Games submenu to include BROWSE_GAMES section

### `/magic_dingus_box/ui/settings_renderer.py`
- Updated `render()` to accept `game_playlists` parameter
- **Added `_render_individual_games()` method** to display games within a selected playlist
- Added `_render_game_browser()` method to display game systems/playlists
- Shows game playlist titles with game counts
- Shows individual game titles with system badges
- Displays "No game playlists found" / "No games in this collection" messages when empty
- Updates header text based on current view (Systems vs specific game collection)

### `/magic_dingus_box/main.py`
- Load all playlists and separate into `video_playlists` and `game_playlists`
- Main UI uses `video_playlists` (only videos shown)
- Updated playlist reload logic to maintain separation
- **Added two-level game selection**: System selection → Individual game selection
- Handle button 4 differently when viewing games (back to systems vs close menu)
- **Launch only the selected game** (create temporary single-item playlist)
- Pass `game_playlists` and game count to settings menu navigation
- No auto-progression for games (unlike video playlists)

## Example Playlists

The three example game playlists are already configured correctly:
- `retro_games.yaml` - Only games → Shows in game browser
- `n64_classics.yaml` - Only games → Shows in game browser  
- `ps1_collection.yaml` - Only games → Shows in game browser

Your video playlists:
- `danny_gatton.yaml` - Only videos → Shows in main UI
- `wes_montgomery.yaml` - Only videos → Shows in main UI

## Testing

To test the new feature:

1. Start the app: `./scripts/run_dev.sh`
2. Main UI should show only video playlists
3. Press button 4 (quick press)
4. Navigate to "Video Games" and press Enter
5. Select "Browse Game Libraries" and press Enter
6. You should see game systems/collections (Retro Games, N64 Classics, PS1 Collection)
7. Select a system and press Enter
8. You should now see individual games in that system
9. Select a specific game and press Enter to launch (will show warning if ROMs not present)
10. Press button 4 to go back to systems list (or close if already at systems)

## Navigation Flow

```
Main UI (Videos Only)
  ↓ [Button 4]
Settings Menu
  ↓ [Select "Video Games"]
Video Games Submenu
  ↓ [Select "Browse Game Libraries"]
Game Systems Browser
  ├─ Retro Game Collection (4 games)
  ├─ N64 Classics (4 games)  ← Select this
  └─ PlayStation Classics (4 games)
  ↓ [Select "N64 Classics"]
Individual Games in N64 Classics
  ├─ Super Mario 64  ← Select this
  ├─ Legend of Zelda: Ocarina of Time
  ├─ GoldenEye 007
  └─ Mario Kart 64
  ↓ [Select "Super Mario 64"]
Super Mario 64 Launches → Returns to UI when done

Note: Button 4 while viewing games → Back to systems list
      Button 4 while at systems list → Close settings
```

## Benefits

1. **Cleaner Main UI**: Only video content at startup
2. **Organized**: Games have their own dedicated space
3. **Individual Game Selection**: Browse and pick specific games (not auto-play through a list)
4. **Flexible**: Can still mix videos/games if desired (shows in main UI)
5. **Automatic**: No need to manually categorize - system detects playlist type
6. **Seamless**: Same smooth experience for launching games

## Key Differences from Video Playlists

**Video Playlists (Main UI):**
- Select a playlist → First video auto-plays
- Next/Previous buttons cycle through videos in playlist
- Auto-progression to next video when current ends
- Auto-advance to next playlist when playlist ends

**Game "Playlists" (Settings Browser):**
- Select a system → See list of individual games
- Select a specific game → Only that game launches
- No next/previous - single game launches
- No auto-progression - exits to main UI when game closes
- Think of them as "game collections" not "playlists"

All existing features remain intact - video playback, sample mode, volume transitions, etc.

