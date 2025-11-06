# Web UI Guide

Manage devices, upload videos/ROMs, and build playlists from any browser.

## Start the server
- The admin server starts with the app when `MAGIC_ENABLE_WEB_ADMIN=1` (default).
- Default address: `http://localhost:8080` (or the device IP).
- Optional header auth: set `MAGIC_ADMIN_TOKEN` and send `X-Magic-Token`.

## Tabs & features
- Device: auto-discovery (IP and stats), manual IP connect.
- Videos: drag & drop upload, list library, sizes, modified times.
- ROMs: upload by system (NES/SNES/N64/PS1), grouped views.
- Playlists: create/edit YAML-backed playlists with drag-and-drop, mixed videos and games, edit title/artist, reorder.

## Playlist format (brief)
- Top-level: `title`, `curator`, `description`, `loop`, `items`.
- Each item: `title`, `artist`, `source_type` (`local`|`youtube`|`emulated_game`), `path` or `url`, optional `start`/`end`, `tags`.
- Games: add `emulator_core`, `emulator_system`.

See `docs/PLAYLIST_FORMAT.md` for complete details.

## API endpoints (subset)
- `GET /admin/device/info` — device identity and stats
- `GET /admin/playlists` — list playlists
- `GET /admin/playlists/<name>` — get playlist JSON
- `POST /admin/playlists/<name>` — create/update (JSON or YAML)
- `DELETE /admin/playlists/<name>` — delete
- `GET /admin/media` — list media
- `POST /admin/upload` — upload media
- `GET /admin/roms` — list ROMs
- `POST /admin/upload/rom/<system>` — upload ROM

## Limits & security
- Upload cap via `MAGIC_MAX_UPLOAD_MB` (default 2048 MB).
- Optional token via `MAGIC_ADMIN_TOKEN`.
- For production, consider running behind a reverse proxy and enabling HTTPS.

