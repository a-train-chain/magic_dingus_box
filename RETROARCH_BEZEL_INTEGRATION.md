# RetroArch Bezel Integration

## Feature: Unified Bezel Experience

Your selected bezel now appears in BOTH the Magic Dingus Box UI AND when playing games in RetroArch!

## How It Works

### Seamless Bezel Continuity

1. **Select a bezel** in Magic Dingus Box settings (e.g., "NES TV")
2. **UI displays** with that bezel frame
3. **Launch a game** from the game browser
4. **RetroArch launches** with the SAME bezel automatically
5. **Visual continuity** - no jarring transition!

### Technical Implementation

When you launch a game, the Magic Dingus Box:
1. Checks your current display mode
2. If in "Modern (Bezel)" mode, gets your selected bezel style
3. Passes the bezel PNG path to RetroArch via `--overlay` flag
4. RetroArch renders the game with your bezel

### Command Line Example

When you launch a game, RetroArch is called like:

```bash
retroarch \
  -L parallel_n64_libretro \
  /path/to/Super\ Mario\ 64.n64 \
  --fullscreen \
  --overlay /path/to/assets/bezels/n64_tv.png \
  --overlay-opacity 1.0 \
  --overlay-scale 1.0
```

## User Experience

### Before (Without Bezel Passthrough)
```
Magic Dingus Box UI: Beautiful NES TV bezel
                ↓
        Launch game
                ↓
RetroArch: Plain black borders (no bezel)
                ↓ Jarring transition!
```

### After (With Bezel Passthrough) ✅
```
Magic Dingus Box UI: Beautiful NES TV bezel
                ↓
        Launch game
                ↓
RetroArch: SAME NES TV bezel!
                ↓ Seamless experience!
```

## Bezel Behavior by Display Mode

### CRT Native Mode
- **Magic Dingus Box UI**: No bezel (fullscreen)
- **RetroArch**: No overlay passed
- **Result**: Clean fullscreen in both

### Modern Clean Mode
- **Magic Dingus Box UI**: No bezel (clean pillarbox)
- **RetroArch**: No overlay passed
- **Result**: Clean pillarbox in both

### Modern with Bezel Mode ⭐
- **Magic Dingus Box UI**: Your selected bezel (e.g., N64 TV)
- **RetroArch**: SAME bezel automatically!
- **Result**: Perfect visual continuity!

## Examples

### Playing NES Games

**Settings:**
- Display Mode: Modern (Bezel)
- Bezel Style: NES TV

**Experience:**
1. Browse games with NES TV bezel in UI ✅
2. Select Super Mario Bros. 3
3. Game launches with NES TV bezel ✅
4. RetroArch menu (F1) shows with NES TV bezel ✅
5. Exit game → Return to UI with NES TV bezel ✅

**Completely unified look!**

### Playing N64 Games

**Settings:**
- Display Mode: Modern (Bezel)
- Bezel Style: N64 TV

**Experience:**
1. UI with N64 TV bezel
2. Launch Super Mario 64
3. Game plays with N64 TV bezel
4. Perfect thematic consistency!

### Mixed Content

**Settings:**
- Bezel Style: Retro TV 1 (generic)

**Experience:**
- Works with all games and videos
- Consistent retro TV look throughout
- Even when switching between different game systems

## Configuration

### Automatic (Default)

No configuration needed! The system automatically:
- Detects if bezel mode is active
- Finds the selected bezel file
- Passes it to RetroArch
- Falls back gracefully if bezel missing

### Manual Override (Advanced)

You can force a specific bezel for games:

```bash
# Set environment variable
export MAGIC_GAME_OVERLAY=/path/to/custom_bezel.png

# RetroArch will use this instead of auto-detected
```

## Bezel Resolution

### Automatic Scaling

RetroArch automatically scales the overlay to match the screen:
- 1080p screen: Bezel scaled to 1920x1080
- 4K screen: Bezel scaled to 3840x2160
- Always matches your display

### Bezel Quality

The RetroArch bezels are high-resolution:
- Original: 1920x1080 or higher
- Scale up well to 4K
- Scale down fine for smaller displays
- Always look sharp!

## Troubleshooting

### Bezel doesn't appear in RetroArch

**Check:**
1. Display mode is "Modern (Bezel)" not "Modern (Clean)"
2. Bezel file exists in `assets/bezels/`
3. Check logs for "Will use bezel overlay in game" message
4. Try different bezel style

### Bezel looks different in RetroArch

**Why:**
- RetroArch may apply different scaling
- RetroArch has its own overlay settings
- Some bezels are designed specifically for certain aspect ratios

**Solution:**
- Use bezels from the 16x9 collection (we already do)
- These work best for modern widescreen displays

### Game launches but no bezel

**Possible causes:**
- RetroArch doesn't support overlays in this core
- Overlay file path has spaces/special characters
- RetroArch config overrides command-line overlay

**Solution:**
- Check RetroArch logs: `~/.config/retroarch/retroarch.log`
- Try a different bezel
- Test overlay manually in RetroArch first

## Benefits

### Visual Consistency
✅ Same bezel in UI and gameplay  
✅ No jarring transitions  
✅ Professional presentation  
✅ Thematic coherence (NES bezel for NES games, etc.)

### User Experience
✅ Set once, applies everywhere  
✅ No separate RetroArch configuration  
✅ Automatic bezel selection  
✅ Works on both Mac and Pi

### Authenticity
✅ Looks like everything runs on one CRT TV  
✅ Complete retro aesthetic  
✅ Immersive experience  
✅ True to the "Magic Dingus Box" concept

## Recommended Bezel Selections

### For Specific Systems

**Playing mostly NES games?**
- Bezel Style: NES TV
- Authentic NES-era television

**Playing mostly N64 games?**
- Bezel Style: N64 TV
- 90s television aesthetic

**Playing mostly PS1 games?**
- Bezel Style: PlayStation TV
- Late 90s TV design

### For Mixed Content

**Variety of games and videos?**
- Bezel Style: Retro TV 1 or Retro TV 2
- Generic retro TV works for everything

**Modern aesthetic?**
- Bezel Style: Modern TV
- Cleaner, less vintage look

## Testing

Try this sequence:

1. Set Display Mode: Modern (Bezel)
2. Set Bezel Style: NES TV
3. Restart Magic Dingus Box
4. See NES TV bezel in UI ✅
5. Launch Super Mario Bros. 3
6. Game plays with NES TV bezel ✅
7. Press F1 to open RetroArch menu
8. Menu shows with NES TV bezel ✅
9. Exit game
10. Return to UI with NES TV bezel ✅

**Perfect continuity throughout!**

## Future Enhancements

Possible additions:
- Auto-select bezel based on game system (NES game = NES TV bezel)
- Per-playlist bezel override
- Custom bezel per game
- Bezel animations (screen flicker, power-on effect)

## Credits

This feature uses RetroArch's built-in overlay system:
- RetroArch overlay documentation: https://docs.libretro.com/guides/libretro-overlays/
- Bezel images from overlay-borders project
- Seamlessly integrated into Magic Dingus Box!

---

**Your Magic Dingus Box now maintains the same beautiful CRT TV aesthetic from UI to gameplay and back!** 🎮📺✨

