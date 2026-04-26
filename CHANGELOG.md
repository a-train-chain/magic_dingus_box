# Changelog

All notable changes to Magic Dingus Box will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] - 2026-04-18

### Fixed (post-2026-04-18)
- **QR code shows wrong USB URL**: settings menu QR code was hardcoded to `http://192.168.7.1:5000` — the legacy USB gadget address. Current `usb-gadget-network.service` assigns `10.55.0.1`, so the QR was encoding an IP that didn't exist on the gadget network. Now reads the live `usb0` IPv4 via `getifaddrs` and builds the URL from whatever's actually assigned.



### Added
- **Three custom MDB bezels** (MDB-1974 wood-grain console, MDB-1986 Memphis neon, MDB-KV19 sleek black broadcast monitor) shipped as the new defaults in [bezels.json](magic_dingus_box_cpp/assets/bezels/bezels.json) with matching RetroArch overlay `.cfg` sidecars.
- **RetroArch bezel overlay in Modern TV mode**: launching a game in `MODERN_TV` mode now writes a 1920×1080 RetroArch config with a 4:3 `custom_viewport` at `(251, 10, 1415, 1059)` (the geometric intersection of all bezel families' transparent cutouts) and `input_overlay` pointing at the user's selected bezel `.cfg`. The bezel frames the game so a modern 16:9 TV presents retro titles inside their period-appropriate "screen". CRT_NATIVE mode launches at 640×480 with no overlay (byte-identical to pre-feature behavior).
- **Auto-detected dual controller mappings**: kiosk now scans `/dev/input/js*` and reads VID/PID from sysfs at game-launch time, classifying as `N64_ADAPTER` (`0e6d:111d`), `PS_STYLE_DRAGONRISE` (`0079:0006`), or `UNKNOWN`. Dispatches to per-core mapping tables tailored to each controller. PS-style pad gets PS1 1:1 mapping, classic SNES face button positions, Genesis A/B/C, FBNeo 6-button fighter layout, etc. Unknown / no-controller falls back to N64 mapping.
- **Kiosk UI input support for PS-style USB pads**: button codes 288–299 now recognized alongside N64 codes. Cross opens settings menu, Square selects, Triangle play/pause, L1/R1 prev/next, D-pad navigates. Detects 8-bit-axis controllers (`abs_min == 0`, `abs_max ≤ 255`) and treats `ABS_X/Y` extremes as digital D-pad presses. `ABS_Z`/`ABS_RZ` (right stick on these pads when in analog mode) explicitly ignored.
- **Tier 1 performance overclock**: CPU 1.8 → 2.0 GHz, V3D GPU 500 → 600 MHz, GPU memory split 76 → 256 MB, force_turbo, performance governor pinned via new `magic-cpu-performance.service` (one-shot at boot). Net ~+15% headroom for heavier cores like PS1 at 1080p output. CPU/GPU clocks set in `/boot/firmware/config.txt` (captured by golden image). Service file in [magic_dingus_box_cpp/systemd/magic-cpu-performance.service](magic_dingus_box_cpp/systemd/magic-cpu-performance.service) and auto-installed by `deploy_cpp.sh`.

### Fixed
- **Bezel disappearing after RetroArch exit**: removed a stale `static loaded_bezel_path` tracker in the main render loop that blocked `load_bezel()` from being called after `reset_gl()` destroyed the bezel texture. The renderer's own dedupe handles repeat calls correctly.
- **Bezel obscuring RetroArch in-game menu**: set `input_overlay_hide_in_menu = "true"` so the bezel auto-hides when the RetroArch menu opens (Z+Start hotkey).
- **Loading screen z-order**: the brief "Loading..." overlay during the kiosk → RetroArch handoff now renders the bezel on top of the loading text instead of the loading text covering the bezel — continuous visual framing through the transition.

### Improved
- **PS1 core (`pcsx_rearmed`) tuning**: SPU reverb disabled, SPU interpolation off — small CPU savings with no audible impact on most titles. Combined with the global `video_threaded = true` (was false), notably smoother on Pi 4B.
- **RetroArch launcher refactor**: extracted duplicated video-config blocks from `launch_drm()` and `open_core_downloader_direct()` into a single `write_video_config(stream, opts)` helper that branches on `display_mode`. Per-core controller mapping logic split into `get_mapping_n64_adapter()` and `get_mapping_ps_style()` with a `get_mapping(type, core_name)` dispatcher.
- **RetroArch autoconfig file** now matches whichever controller is detected (`0e6d_111d.cfg` for N64, `0079_0006.cfg` for PS-style, none for UNKNOWN).

## [1.3.0] - 2026-02-22

### Added
- 148 curated games across 7 systems: Arcade (16), Atari 7800 (20), NES (20), PC Engine (15), SNES (20), Sega Genesis (20), PlayStation 1 (34), with cover art thumbnails
- Multi-disc PS1 support via .m3u files (Final Fantasy VII, Metal Gear Solid, Gran Turismo 2, Resident Evil 2)
- D-pad hold-to-repeat for accelerated menu navigation
- Analog stick-to-D-pad axis mappings for SNES, Genesis, Atari 7800, and PC Engine cores

### Fixed
- **Critical:** OTA updates now preserve game saves (`data/saves/`, `data/states/`) during install, rollback, and backup
- RetroArch shell escaping uses single-quote wrapping (fixes games with apostrophes like "Tony Hawk's Pro Skater")
- Intro video no longer cuts off early
- Controller hotkey combo (Z + Start) restored for RetroArch menu toggle
- WiFi connection timeout (30s) with specific error messages instead of hanging
- WiFi shell injection eliminated (fork/execvp for nmcli commands)
- Input device last-resort recovery after RetroArch exits
- Playlist loader validates emulated_game items at load time (skips entries missing core or path)
- Empty ROM path validation before RetroArch launch
- File descriptor close limit uses sysconf() instead of hardcoded 256
- Removed dead keepalive process cleanup code and duplicate config writes in RetroArch launcher
- Removed broken Double Dragon and Metal Slug from arcade playlist

### Improved
- Master Shuffle uses GL-drawn crossed-arrows icon instead of text marker
- Now-playing playlist shown with green text instead of dot indicator
- Playlists numbered from 01 after Master Shuffle entry
- Code quality: security, performance, and reliability hardening pass

## [1.2.0] - 2026-02-21

### Added
- Systemd watchdog with automatic crash recovery (Type=notify, 10s watchdog)
- Now-playing indicator (accent dot) on current playlist in playlist list
- Error overlay banner for game launch failures (4-second auto-dismiss)
- Master shuffle history with "Previous" support (up to 10 entries)
- EOS callback infrastructure for GStreamer end-of-stream events

### Fixed
- **Security:** Shell injection in WiFi manager (SSID/password via fork/execvp instead of popen)
- **Security:** Tightened delete_media path validation to media directories only
- Atomic settings file write (temp file + rename) prevents corruption on crash
- Missing settings file no longer treated as error on first run
- GStreamer play() no longer blocks forever (3-second timeout replaces infinite wait)
- RetroArch uses isolated temp config (/tmp/retroarch_mdb.cfg) instead of overwriting user config
- Removed exit(0) in core downloader that killed the entire application
- Hardcoded XDG_RUNTIME_DIR replaced with dynamic getuid() in RetroArch launcher
- Replaced std::system("chmod") with POSIX chmod() calls
- Virtual keyboard text centering uses actual font metrics
- format_time() handles negative seconds gracefully
- Game scroll offset resets correctly when switching playlists
- WiFi saved networks reconnect without prompting for password
- WiFi disconnect requires two-press confirmation
- Removed dead "Controllers" menu link from Games submenu
- Removed const_cast antipattern in settings menu (uses mutable members)
- Analog stick auto-repeat fires immediately on direction change
- Playlist switching timeout reduced from 5s to 2s
- Watchdog paused during RetroArch gameplay to prevent false kills

### Improved
- RGB mask CRT shader uses smoothstep-based subpixel column darkening
- WiFi scan results cross-reference saved connections for accurate status

## [1.1.0] - 2026-02-08

### Added
- Rotary encoder video seeking with velocity-sensitive progress bar
- Playlist package import/export (ZIP with videos + playlist YAML)
- Playlist deletion with orphaned video cleanup
- Alphabetical playlist sorting on device

### Fixed
- Audio output setting (HDMI/Headphone) now persists across reboots
- Audio output configured before intro video plays (no mid-intro switching)
- Upload size limited to 8GB default (prevents storage exhaustion)
- ZIP extraction size limited to 10GB (prevents ZIP bomb attacks)
- XSS vulnerability in notification messages
- Playlist existence check now runs before extracting videos in package import

### Improved
- Content Manager UI with drag-and-drop playlist management
- YAML formatting handles non-string types safely
- Deploy script uses configurable paths instead of hardcoded values

## [1.0.7] - 2026-01-18

### Fixed
- OTA rollback functions no longer stop web service (same fix as 1.0.6 for main install)
- Rollback rsync now uses --no-group --no-owner to avoid permission errors
- Rollback handles rsync exit codes 23/24 gracefully

## [1.0.6] - 2026-01-18

### Fixed
- OTA update now works reliably - web service stays running during update
- Only C++ app is stopped during update, web service restarts at the end

## [1.0.5] - 2026-01-18

### Fixed
- OTA update subprocess now survives when web service is stopped during update

## [1.0.4] - 2026-01-18

### Changed
- Test release to verify OTA update system works end-to-end

## [1.0.3] - 2026-01-18

### Fixed
- OTA update rsync permission errors (added --no-group --no-owner flags)
- OTA update content directory detection for release tarballs
- Handle rsync exit codes 23/24 gracefully

## [1.0.2] - 2026-01-18

### Fixed
- OTA update backup directory permissions (now uses user-writable location)

## [1.0.1] - 2026-01-18

### Added
- Audio output selection (HDMI, Headphone Jack, Auto) via PulseAudio
- Game volume offset control for RetroArch (-3dB, -6dB, -12dB options)
- Audio submenu in Settings for easy audio configuration
- Audio settings persistence (saved to config file)

### Improved
- Video transcoding now normalizes audio to -23 LUFS (EBU R128 broadcast standard)
- Consistent volume levels across all transcoded videos
- System volume control via ALSA Master/PCM mixers

## [1.0.0] - 2026-01-16

### Added
- Initial stable release
- C++ kiosk engine with DRM/KMS for true kiosk mode
- Video playback via GStreamer with GL texture rendering
- RetroArch integration for NES, N64, and PS1 emulation
- Web admin interface for remote content management
- Playlist management with YAML format
- Video transcoding with CRT (640x480) and modern (720p) presets
- Smart upload with automatic transcode detection
- USB Ethernet Gadget support for fast content transfers
- WiFi configuration via on-screen menu
- QR code display for easy web admin access
- USB/WiFi network detection with priority handling
- M3U playlist generation for multi-disc PS1 games
- Backup and restore functionality
- Over-the-air (OTA) update system via GitHub Releases

### Technical Details
- OpenGL ES 3.0 rendering with immediate-mode 2D UI
- evdev input handling for controllers and keyboards
- stb_truetype font rasterization
- YAML-based playlist and settings persistence
- Flask-based REST API for web admin
- Systemd service integration for auto-start
