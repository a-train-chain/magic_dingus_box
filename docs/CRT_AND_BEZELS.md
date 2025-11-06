# CRT Effects and Bezel Guide

This guide consolidates CRT effects and bezel usage for modern displays and CRT-native setups.

## Bezel Styles
- Location: `assets/bezels/` (metadata in `assets/bezels/bezels.json`)
- Styles include: Retro TV 1 (default), NES TV, N64 TV, PlayStation TV, Vintage, Modern, Retro TV 2, and procedural fallback.

### Change Bezel Style
1. Press Button 4 → Display
2. Set Mode to "Modern (Bezel)"
3. Select "Bezel Style" and press Enter to cycle
4. Restart app to apply

## CRT Effects (toggle live)
The app applies effects via `CRTEffectsManager` and exposes them in Display Settings.

- Scanlines: OFF / Light / Medium / Heavy
- Color Warmth: 0.0–0.75
- Phosphor Glow: 0.0–0.75
- RGB Phosphor Mask: 0.0–0.75
- Screen Bloom: 0.0–0.75
- Interlacing: 0.0–0.75
- Screen Flicker: 0.0–0.75

Effects are applied to the 720×480 content surface and then composed to the screen (or under the bezel).

## Display Modes
- CRT Native: 720×480 fullscreen (Pi composite)
- Modern (Clean): Auto-detected resolution, centered 4:3 with pillarbox
- Modern (Bezel): As above, with bezel overlay

Change via Settings → Display or env `MAGIC_DISPLAY_MODE`.

## Notes
- Modern layout includes NTSC pixel aspect correction for authentic 4:3 scaling.
- In bezel mode, UI margins are adjusted for readability inside the frame.

For advanced shader options (OpenGL-based shaders like CRT-Royale), see archived notes in `docs/archive/` for future exploration.

