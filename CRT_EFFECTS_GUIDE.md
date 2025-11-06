# CRT Effects & Bezel Guide

## Overview

Your Magic Dingus Box now includes professional RetroArch CRT TV bezels and pygame-based CRT visual effects for an authentic retro aesthetic!

## Professional CRT TV Bezels

### What's Included

7 professional bezel designs from RetroArch:

1. **Retro TV 1** (Default) - Classic retro television
2. **NES TV** - Nintendo Entertainment System with period-appropriate TV
3. **N64 TV** - Nintendo 64 with 90s TV set
4. **PlayStation TV** - Sony PlayStation with matching TV
5. **Vintage** - Vintage television set
6. **Modern** - Modern television design
7. **Retro TV 2** - Alternative retro TV
8. **Simple (Built-in)** - Procedurally generated fallback

### How to Change Bezel Style

1. Press **Button 4** → **Display**
2. Set Mode to "Modern (Bezel)"
3. Select **"Bezel Style: [Current]"**
4. Press **Enter** to cycle through styles
5. **Restart** app to see new bezel

## CRT Visual Effects

### Available Effects

#### 1. Scanlines
**What it does**: Adds horizontal dark lines mimicking CRT electron beam scan

**Options**:
- OFF - No scanlines
- Light (15%) - Subtle lines
- Medium (30%) - Balanced (recommended)
- Heavy (50%) - Strong CRT effect

**Cycle**: Settings → Display → Scanlines  
**Apply**: Immediately (no restart)

#### 2. Color Warmth
**What it does**: Adds warm orange/yellow tint simulating CRT phosphor color temperature

**Options**:
- OFF - No tint
- Cool (25%) - Slightly warm
- Neutral (50%) - Balanced warm tint
- Warm (75%) - Heavy warm tint

**Cycle**: Settings → Display → Color Warmth  
**Apply**: Immediately

#### 3. Screen Bloom
**What it does**: Brightens highlights, simulates CRT light bleeding

**Options**: ON / OFF

**Toggle**: Settings → Display → Screen Bloom  
**Apply**: Immediately

#### 4. Phosphor Glow
**What it does**: Adds subtle colored glow around edges

**Options**: ON / OFF

**Toggle**: Settings → Display → Phosphor Glow  
**Apply**: Immediately

## Recommended Settings

### For Authentic CRT Look (Modern Display)

```
Mode: Modern (Bezel)
Bezel Style: Retro TV 1
Scanlines: Medium (30%)
Color Warmth: Neutral (50%)
Screen Bloom: ON
Phosphor Glow: OFF
```

### For Clean Retro Look

```
Mode: Modern (Clean)
Scanlines: Light (15%)
Color Warmth: OFF
Screen Bloom: OFF
Phosphor Glow: OFF
```

### For Maximum Authenticity

```
Mode: Modern (Bezel)
Bezel Style: NES TV (or system-specific)
Scanlines: Heavy (50%)
Color Warmth: Warm (75%)
Screen Bloom: ON
Phosphor Glow: ON
```

### For Actual CRT TV

```
Mode: CRT Native
Scanlines: OFF (real CRT has its own)
Color Warmth: OFF
Screen Bloom: OFF
Phosphor Glow: OFF
```

## Effect Details

### Enhanced Scanlines

- Horizontal dark lines every 2 pixels
- Subtle RGB offset on alternating lines (phosphor simulation)
- Adjustable darkness
- Performance: <1ms per frame

### Color Warmth

- Warm orange/yellow tint overlay
- Simulates CRT phosphor color temperature
- Vintage TVs had warmer color than modern displays
- Performance: <0.5ms per frame

### Screen Bloom

- Brightens already-bright pixels
- Simulates CRT light bleeding and glow
- Very subtle effect
- Performance: <1ms per frame

### Phosphor Glow

- Adds colored glow around screen edges
- Uses your theme colors (magenta/teal)
- Simulates CRT phosphor edge effects
- Performance: <1ms per frame

## Performance

All effects combined:
- **Overhead**: ~3-5ms per frame
- **Target FPS**: 60 FPS maintained
- **Total frame budget**: 16.67ms
- **Remaining**: ~11-13ms for other rendering

Tested on:
- ✅ MacBook Pro (M1/M2)
- ✅ Raspberry Pi 4B

## Usage Tips

### Bezel Selection

Match bezel to content:
- Playing NES games? Use "NES TV" bezel
- Playing N64 games? Use "N64 TV" bezel
- Playing PS1 games? Use "PlayStation TV" bezel
- Mixed content? Use "Retro TV 1" (generic)

### Effect Combinations

**Subtle & Modern**:
- Scanlines: Light
- All others: OFF

**Balanced Retro**:
- Scanlines: Medium
- Warmth: Neutral
- Bloom/Glow: OFF

**Maximum Vintage**:
- Scanlines: Heavy
- Warmth: Warm
- Bloom: ON
- Glow: ON

### For Development

Use minimal effects so you can see UI clearly:
- Scanlines: OFF or Light
- Everything else: OFF

### For CRT TV Users

Effects are redundant on real CRTs:
- Mode: CRT Native
- All effects: OFF

## Troubleshooting

### Bezel doesn't appear
- Check mode is "Modern (Bezel)" not "Modern (Clean)"
- Verify assets/bezels/ directory has PNG files
- Check logs for loading errors
- Fallback to "procedural" bezel if images missing

### Effects look too strong
- Reduce scanline intensity
- Reduce color warmth
- Turn off bloom/glow
- Find your personal preference

### Performance issues
- Turn off bloom and glow (most expensive)
- Use lighter scanlines
- Reduce warmth to 0

### Bezel blocks content
- This shouldn't happen (bezel has transparent center)
- Try a different bezel style
- Report as bug if it persists

## Adding Custom Bezels

Want to use your own bezel images?

1. Create 1920x1080 PNG with transparent center
2. Save to `assets/bezels/my_bezel.png`
3. Add entry to `assets/bezels/bezels.json`:
```json
{
  "id": "custom1",
  "name": "My Custom Bezel",
  "file": "my_bezel.png",
  "description": "My custom TV frame"
}
```
4. Restart app
5. Select in Display Settings

## Technical Details

### Effect Application Order

1. Screen Bloom (brighten highlights first)
2. Color Warmth (apply tint)
3. Phosphor Glow (edge effects)
4. Scanlines (final overlay)

This order produces the best visual result.

### Bezel Loading

- Bezels are loaded once at startup
- Scaled to match your display resolution
- Cached for performance
- Falls back to procedural if file missing

### Settings Persistence

All settings saved to `dev_data/settings.json`:
```json
{
  "display_mode": "modern_bezel",
  "bezel_style": "retro_tv_1",
  "scanlines_mode": "medium",
  "color_warmth": 0.5,
  "screen_bloom": false,
  "phosphor_glow": false
}
```

## Credits

Bezel images from **RetroArch overlay-borders** project:
- Repository: https://github.com/libretro/overlay-borders
- Collection: NyNy77 1080 Bezel Collection
- License: Various (see assets/README.md)
- Thanks to the RetroArch/libretro community!

## Examples

### Before (Procedural Bezel)
- Simple geometric shapes
- Basic wood/plastic colors
- Functional but not realistic

### After (RetroArch Bezel Images)
- Photorealistic CRT TV frames
- Curved screens
- Authentic plastic/wood textures
- Brand logos and details
- Screen reflections

The visual upgrade is dramatic! 🎮✨

