# Assets Directory

## Bezels

The bezel images in `bezels/` are from the RetroArch overlay-borders project.

### Source
- **Repository**: https://github.com/libretro/overlay-borders
- **Collection**: NyNy77 1080 Bezel Collection
- **License**: Various (see individual bezel licenses in source repository)

### Attribution
These bezels are created by the RetroArch/libretro community. We use them under their respective open-source licenses.

### Bezel Files
- `nes_tv.png` - Nintendo Entertainment System with TV frame
- `n64_tv.png` - Nintendo 64 with TV frame
- `ps1_tv.png` - Sony PlayStation with TV frame
- `retro_tv_1.png` - Generic retro TV 1
- `retro_tv_2.png` - Generic retro TV 2
- `tv_retro_1.png` - Vintage TV set
- `tv_modern_1.png` - Modern TV set

All bezels are 1920x1080 PNG images with transparent centers.

## Usage

Bezels are automatically loaded and scaled to match your display resolution. Select bezel style in: **Settings → Display → Bezel Style**

## Adding Custom Bezels

1. Add PNG file to `assets/bezels/` (must have transparent center)
2. Update `bezels.json` with new entry
3. Restart app
4. New bezel will appear in settings menu

