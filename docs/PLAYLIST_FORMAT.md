# Playlist Format

Authoritative reference for YAML playlists consumed by Magic Dingus Box.

## Top-level fields
```yaml
title: My Playlist
curator: Alex Chaney
description: ''
loop: false
items: []
```

- `title` (string): Display name.
- `curator` (string): Creator or owner.
- `description` (string): Optional; keep even if blank for consistency.
- `loop` (bool): Loop playlist.
- `items` (list): See below.

## Items
```yaml
- title: Windy
  artist: Wes Montgomery
  source_type: local  # local | youtube | emulated_game
  path: media/Wes Montgomery - Windy.mp4
  start: 0
  end: 215
  tags:
    - jazz
```

- `title` (string): Required.
- `artist` (string): Optional, recommended for music videos; always included (blank OK).
- `source_type` (string): `local`, `youtube`, or `emulated_game`.
- `path` (string): Required for `local` and `emulated_game`.
- `url` (string): For `youtube`.
- `start`/`end` (float): Optional trimming.
- `tags` (list[string]): Optional categories.

### Emulated games
```yaml
- title: Super Mario 64
  artist: ''
  source_type: emulated_game
  path: roms/n64/Super Mario 64.z64
  emulator_core: mupen64plus_next_libretro
  emulator_system: N64
```

- `emulator_core` (string): Core name.
- `emulator_system` (string): Human label (NES/N64/PS1/etc.).

## Notes
- The web admin always outputs clean, normalized YAML ordering and includes `artist` (blank if unknown).
- Game-only playlists appear in the Settings → Video Games browser; mixed playlists appear in the main UI.

