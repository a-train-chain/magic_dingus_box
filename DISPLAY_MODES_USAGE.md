# Display Modes - Usage Guide

## Overview

Your Magic Dingus Box now supports three display modes, allowing it to work on both vintage CRT TVs and modern displays while maintaining the authentic 4:3 aspect ratio.

## Display Modes

### 1. CRT Native (Default)
- **Resolution**: 720x480 fullscreen
- **Best for**: Raspberry Pi with composite output to CRT TV
- **Appearance**: Full screen, no borders, no framing
- **Performance**: Zero overhead

### 2. Modern Clean
- **Resolution**: Configurable (1080p, 1440p, 4K)
- **Best for**: Development on laptop, Pi connected to HDMI
- **Appearance**: 720x480 content centered with black pillarboxing
- **Performance**: Minimal overhead (one scale operation per frame)

### 3. Modern with CRT Bezel
- **Resolution**: Configurable (1080p, 1440p, 4K)
- **Best for**: Retro aesthetic on modern displays
- **Appearance**: 720x480 content centered with decorative CRT TV frame
- **Performance**: Minimal overhead (one scale + bezel blit per frame)

## How to Change Display Mode

### Via Settings Menu (Runtime)

1. **Press Button 4** to open Settings
2. Navigate to **Display Settings**
3. Select **Display Mode: [Current Mode]**
4. Press Enter to cycle through modes
5. **Restart the app** for changes to take effect

The setting is automatically saved and persists across restarts!

### Via Environment Variables (Before Launch)

```bash
# CRT Native (default)
export MAGIC_DISPLAY_MODE=crt_native
./scripts/run_dev.sh

# Modern Clean
export MAGIC_DISPLAY_MODE=modern_clean
./scripts/run_dev.sh

# Modern with Bezel
export MAGIC_DISPLAY_MODE=modern_bezel
export MAGIC_MODERN_RES=1920x1080
./scripts/run_dev.sh
```

### Via Settings File (Persistent)

Settings are automatically saved to:
- **macOS**: `dev_data/settings.json`
- **Raspberry Pi**: `/data/settings.json`

Example `settings.json`:
```json
{
  "display_mode": "modern_bezel",
  "modern_resolution": "1920x1080",
  "show_bezel": true
}
```

## Display Settings Options

### Display Mode
- Cycles through: **CRT Native** → **Modern (Clean)** → **Modern (Bezel)** → (back to CRT Native)
- Shows current mode in menu
- Requires restart to apply

### Resolution (Modern Modes Only)
- Available when display mode is Modern
- Options: **1920x1080** → **2560x1440** → **3840x2160**
- Cycles through common resolutions
- Requires restart to apply

### CRT Bezel (Modern Modes Only)
- Available when in Modern mode
- **ON**: Shows retro TV frame
- **OFF**: Clean pillarboxed look
- Auto-switches between Modern (Clean) and Modern (Bezel)
- Requires restart to apply

### Scanlines
- Works in all display modes
- Adds authentic CRT scanline effect
- No restart required (planned for future)

## Testing Different Modes

### On Your Mac (Development)

**Test CRT Native:**
```bash
# Edit dev_data/settings.json or delete it to reset to defaults
rm dev_data/settings.json
./scripts/run_dev.sh
# Should open in 720x480 window
```

**Test Modern with Bezel:**
1. Run the app
2. Press Button 4 → Display Settings
3. Select "Display Mode" until it says "Modern (Bezel)"
4. Restart the app
5. Should now see 720x480 content centered with CRT TV frame!

**Test Different Resolutions:**
1. Set mode to Modern
2. Select "Resolution" to cycle through 1080p, 1440p, 4K
3. Restart
4. Window size changes, content stays 4:3

### On Raspberry Pi

**For CRT TV (Composite):**
- Default mode works perfectly
- No changes needed
- Settings file doesn't exist or is set to `crt_native`

**For Modern TV (HDMI):**
```bash
# SSH to Pi
ssh pi@your-pi

# Set modern mode
echo '{"display_mode": "modern_bezel", "modern_resolution": "1920x1080"}' > /data/settings.json

# Restart Magic Dingus Box
sudo systemctl restart magic-ui
```

## What Stays the Same

All your existing features work identically in all modes:
- ✅ Video playback
- ✅ Game launching  
- ✅ Settings menu
- ✅ Sample mode
- ✅ Volume transitions
- ✅ UI fade animations
- ✅ Auto-progression

The content is always rendered at 720x480 (4:3) - only the presentation changes!

## Advantages by Use Case

### Raspberry Pi + CRT TV (Production)
- **Mode**: CRT Native
- **Why**: Direct output, no overhead, authentic experience
- **Setup**: Default, no configuration needed

### Raspberry Pi + HDMI TV
- **Mode**: Modern Clean or Modern with Bezel
- **Why**: Proper 4:3 on widescreen TV
- **Setup**: Set via settings.json or settings menu

### macOS Development
- **Mode**: Modern with Bezel (for coolness) or Modern Clean
- **Why**: See proper 4:3 on laptop screen, looks professional
- **Setup**: Use settings menu to toggle

### Demos/Screenshots
- **Mode**: Modern with Bezel
- **Why**: Looks amazing in screenshots and videos
- **Setup**: Toggle in settings

## Troubleshooting

### Settings don't persist
- Check that `dev_data/settings.json` (or `/data/settings.json` on Pi) is writable
- Check logs for save errors
- Verify `dev_data/` directory exists

### Wrong resolution after restart
- Check `settings.json` file contents
- Delete settings file to reset to defaults
- Check environment variables aren't overriding

### Bezel doesn't show
- Make sure mode is "modern_bezel" not "modern_clean"
- Check that `show_bezel` is true in settings
- Restart app after changing settings

### Content looks stretched
- This should never happen - content is always 4:3
- If it does, file a bug report with your resolution and mode

## Visual Comparison

### CRT Native on 720x480
```
Your content fills entire screen ═══════════════
```

### Modern Clean on 1920x1080
```
█████ Your 4:3 content here █████
Black pillarbox on sides, clean look
```

### Modern Bezel on 1920x1080
```
░░░╔════════════════╗░░░
░░░║ Your content   ║░░░ Wood frame
░░░║ in 4:3 here    ║░░░ Retro TV look
░░░╚════════════════╝░░░
░░░  [O] MAGIC [O]   ░░░ Brand + knobs
```

## Quick Reference

| Mode | Resolution | Framing | Best For |
|------|-----------|---------|----------|
| CRT Native | 720x480 | None | Raspberry Pi + CRT TV |
| Modern Clean | Configurable | Black bars | Modern displays, minimal |
| Modern Bezel | Configurable | CRT TV frame | Retro aesthetic, demos |

All modes maintain perfect 4:3 aspect ratio for your content!

## Next: Try It!

Your display modes are now fully implemented. To test:

1. **Run the app** (defaults to CRT Native)
2. **Press Button 4** → Display Settings
3. **Toggle Display Mode** 
4. **Restart** to see the change

Enjoy your multi-display Magic Dingus Box! 🎮✨

