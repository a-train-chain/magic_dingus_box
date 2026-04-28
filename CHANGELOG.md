# Changelog

All notable changes to Magic Dingus Box will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.4.1] - 2026-04-27

Patch release with two clone-quality fixes caught during the v1.4.0 image's first flash-test on a fresh Pi (`magicpi-9768`). No source-code refactors, no new features — just two papercuts that would have shipped to every operator otherwise.

### Fixed
- **Cloned Pis without a wired faceplate boot to a dead kiosk** (`a4f3430`): the standby watcher's `reconcile_initial_state()` read GPIO 3 as HIGH and treated it as "switch in OFF position — stop services," killing the kiosk service ~1 second after it started. The HIGH read is ambiguous though — it can mean either "switch wired and OFF" OR "no switch wired at all and the line is floating via the kernel pull-up." The latter applies to every fresh clone before faceplate assembly, AND to anyone who flashes the image to a bare Pi 4 with no switch. Watcher now only acts on actual GPIO transitions during runtime; HIGH at boot is logged but ignored. LOW at boot still calls `start_services` (unambiguous — line is actively pulled to ground). Trade-off: an operator who power-cycles their box with the switch already in OFF position will see the kiosk briefly come up before they re-flip the switch off, but that's strictly better than "boot to a black screen with no recovery path."
- **`first_boot.sh` now updates `/etc/hosts` to match the regenerated hostname** (`9c3cb9c`): cloud-init's `manage_etc_hosts=True` writes a `127.0.1.1 magicpi magicpi` line that systemd's NSS-files resolver uses for self-lookup. `first_boot.sh` Step 3 was already calling `hostnamectl set-hostname magicpi-XXXX` to give cloned Pis unique hostnames, but it wasn't touching `/etc/hosts`. The mismatch made every `sudo` (and many hostname-self-lookup code paths) emit `sudo: unable to resolve host magicpi-XXXX: Temporary failure in name resolution` and pay a small DNS-timeout delay. Cosmetic in isolation but accumulated across boot scripts and operator commands. Now sed-replaces the `127.0.1.1` line to match the new hostname; falls back to appending if no entry exists.

Both fixes verified live on `magicpi-9768` during the v1.4.0 image's flash test before re-cloning to v1.4.1.

## [1.4.0] - 2026-04-27

The "ready for the golden image" release. Closes out a long testing pass with end-to-end verification of every kiosk surface (controllers, all 7 emulator cores, bezel cycling, web admin, Media Browser pipeline) plus a stack of fixes uncovered along the way. Tagged at `1.4.0` because it ships several user-facing features (Media Browser auto-pause-on-playback, audio normalization in upload, 2-player support, tighter Library UI) on top of the bezel/PS-pad/overclock work that was already pending in the prior `[Unreleased]` block.

### Added
- **Three custom MDB bezels** (MDB-1974 wood-grain console, MDB-1986 Memphis neon, MDB-KV19 sleek black broadcast monitor) shipped as the new defaults in [bezels.json](magic_dingus_box_cpp/assets/bezels/bezels.json) with matching RetroArch overlay `.cfg` sidecars.
- **RetroArch bezel overlay in Modern TV mode**: launching a game in `MODERN_TV` mode now writes a 1920×1080 RetroArch config with a 4:3 `custom_viewport` at `(251, 10, 1415, 1059)` (the geometric intersection of all bezel families' transparent cutouts) and `input_overlay` pointing at the user's selected bezel `.cfg`. The bezel frames the game so a modern 16:9 TV presents retro titles inside their period-appropriate "screen". CRT_NATIVE mode launches at 640×480 with no overlay (byte-identical to pre-feature behavior).
- **Auto-detected dual controller mappings**: kiosk now scans `/dev/input/js*` and reads VID/PID from sysfs at game-launch time, classifying as `N64_ADAPTER` (`0e6d:111d`), `PS_STYLE_DRAGONRISE` (`0079:0006`), or `UNKNOWN`. Dispatches to per-core mapping tables tailored to each controller. PS-style pad gets PS1 1:1 mapping, classic SNES face button positions, Genesis A/B/C, FBNeo 6-button fighter layout, etc. Unknown / no-controller falls back to N64 mapping.
- **Kiosk UI input support for PS-style USB pads**: button codes 288–299 now recognized alongside N64 codes. Detects 8-bit-axis controllers (`abs_min == 0`, `abs_max ≤ 255`) and treats `ABS_X/Y` extremes as digital D-pad presses. `ABS_Z`/`ABS_RZ` (right stick on these pads when in analog mode) explicitly ignored.
- **Tier 1 performance overclock**: CPU 1.8 → 2.0 GHz, V3D GPU 500 → 600 MHz, GPU memory split 76 → 256 MB, force_turbo, performance governor pinned via new `magic-cpu-performance.service` (one-shot at boot). Net ~+15% headroom for heavier cores like PS1 at 1080p output. CPU/GPU clocks set in `/boot/firmware/config.txt` (captured by golden image).
- **Two-player support across all 7 cores**: launcher now emits `input_player2_*` button mappings alongside player 1, plus `pcsx_rearmed_pad2type` so the PS1 BIOS recognizes the second pad. Verified live with Twisted Metal 4 split-screen Battle. Hotkeys (RA menu toggle / exit combo) intentionally stay player-1-only so both controllers don't fight over them.
- **Auto-pause torrents during movie playback**: PlaybackScreen now calls qBittorrent's `/torrents/stop` (qBit 5.x; with `/pause` fallback for 4.x) when a movie starts, and `/start`/`/resume` when the user exits. Eliminates random-IO contention on USB-flash media that was making rotary scrubbing freeze. Only resumes torrents the kiosk paused — never re-starts ones the operator manually stopped before entering playback.
- **Smart movie release scoring with seeder filter**: indexer-level `minimumSeeders=5` filter rejects dead-swarm releases at search time. Codified in `setup_services.sh` Step 14b so cloned Pis inherit the threshold via the standard re-provisioning flow. Net effect: Radarr never picks a release that won't actually progress, even if its tracker reports cached "9 seeders exist" but no peer is actually online.
- **Audio normalization in upload pipeline (Retro Ripper match)**: video upload UI's "Normalize audio volume" checkbox now actually fires the FFmpeg `loudnorm` filter — both `/admin/upload-and-transcode` and `/admin/smart-upload` previously dropped the form field on the floor. Loudnorm parameters changed from broadcast quiet (`I=-23:LRA=7:tp=-2`) to match the Retro Ripper companion (`I=-16:TP=-1:LRA=11`) so curator-supplied content and self-uploaded phone clips on the same playlist sound consistent. Default on. Smart-upload's "direct copy" shortcut is suppressed when normalize is requested so the loudnorm pass actually runs.
- **Port-80 redirect to Content Manager**: tiny Python HTTP service on port 80 issues 302 redirects to `:5000`, letting operators type `magicpi.local` (or any IP the Pi is reachable at) without remembering the port number. Runs as `content-manager-redirect.service` with `CAP_NET_BIND_SERVICE`. Preserves path + query + fragment.
- **Zero-config DHCP for plugged-in Mac/PC via dnsmasq**: when an operator plugs their laptop into the Pi's USB-C, the Pi now acts as a DHCP server on `usb0` and auto-assigns `10.55.0.10+` to the host. No manual interface configuration. Config in `scripts/data/dnsmasq-usb0.conf`, scoped to usb0 only (port=0 disables the DNS half so it doesn't fight systemd-resolved on wlan0).
- **Kiosk standby switch via GPIO 3**: `kiosk-standby-watcher.service` runs `gpiomon` on GPIO 3 and stops kiosk + Content Manager + Docker stack on rising edge (switch OFF), starts them on falling edge (switch ON). Replaces the previous full-poweroff behavior — Pi stays running so it boots back into the kiosk in seconds when the operator flips the switch back on.
- **Wi-Fi connection feedback Toasts + "Connecting..." status**: the Wi-Fi sub-menu now shows "Connecting to <SSID>..." the moment the operator presses Enter on the password prompt, with a Toast on success/failure once `nmcli` finishes. Previously the sub-menu sat silent for 5–10 sec and then suddenly flipped to "Connected" with no in-progress feedback.
- **Idempotent setting-menu re-entry**: `enter_submenu()` preserves transient state (selected_index, scroll_offset, confirm-dialog flags) when called with the same section it's already in, instead of resetting them. Fixes a regression where pressing Disconnect → Confirm was a one-frame flicker because the SELECT handler re-ran `enter_submenu(WIFI)` and clobbered the just-flipped confirm flag.
- **Auto-resume save state for cloned Pis** (cloned-image only): added `libretro_info_path` to the generated RetroArch config so `core_info_list` populates correctly. Without it, RA's `command_event_save_auto_state()` early-returned at the savestate-support check (reading 0 from a zero-initialized `core_info_t`) and `.state.auto` files never got written despite `savestate_auto_save = "true"` being honored. SRAM in-game saves were unaffected because they go through a different code path.
- **Defensive WiFi credential wipe on cloned Pis**: `first_boot.sh` Step 6c removes `/etc/NetworkManager/system-connections/*.nmconnection` so cloned Pis don't auto-attempt to join the source operator's home network. New operator joins via the kiosk's Wi-Fi setup UI in ~30 seconds.

### Changed
- **Library screen matches Browse screen**: 9-column grid (was 5), focus-only border (was every-poster gold border), left-aligned titles (was centered with marker-zone reservation), no blinking ◂ cursor (the focused border + accent-color title carry that signal). Corner status dot (green = good file, gold = upgrade available, red = missing) preserved. Net effect: Library and Browse feel like the same UI; visual clutter dropped, focus indicator is unmissable.
- **Modern TV mode shows more rows**: main playlist list (8 → up to 14), settings menu items (7 → up to 10), game-browser playlist (8 → up to 11). Each call site derives `max_visible` from `height_ + item_height` and floors at the original CRT-tuned value so CRT layout is byte-identical. Renderer publishes the on-screen row count back to the input handler so scrolling only kicks in when the selection actually goes off-screen — previously you'd press DOWN onto "Back" and the top row would disappear despite obviously fitting.
- **Audio loudnorm parameters match Retro Ripper**: `I=-23:LRA=7:tp=-2` → `I=-16:TP=-1:LRA=11`. See "Audio normalization in upload pipeline" in Added for full rationale.
- **PS-pad button mapping (operator preference)**: Cross now SELECT, Circle now MENU. Square is unassigned. Reflects the user-facing remap landed during Phase 2 testing — locked in before Phase 2 sign-off.
- **Per-Pi reset on cloned-Pi first boot now also wipes** `services/.env`, `services/config/{radarr,prowlarr,qbittorrent,gluetun,flaresolverr}/*`, the `media_browser_unlocked` flag in `settings.json`, and (now) WiFi credentials. Operator restores fresh per-Pi state via the Content Manager UI on the cloned Pi (drops in a new WireGuard config; `setup_services.sh` rebuilds everything else from the codified fixtures in `scripts/data/`).
- **`deploy_cpp.sh` now syncs the top-level `scripts/golden_image/` directory**. Previously these scripts (`first_boot.sh`, `prepare_for_cloning.sh`, `restore_after_cloning.sh`, `prepare_golden_image.sh`) lived outside `magic_dingus_box_cpp/` and silently drifted between repo and Pi over time. Caught during the pre-clone audit when the Pi turned out to have an outdated `first_boot.sh` that would have leaked the source's VPN credentials and Media Browser unlock state to every clone.
- **Settings menu MENU button = "back one level"**: previously always closed the menu entirely; now treats MENU as a back-stack pop, only closing when at the top level. Mirrors how operators expect nested settings UIs to behave.

### Fixed
- **QR code encodes Wi-Fi IP instead of usb0 gadget IP** (the headline bug operators kept hitting): `WifiManager::get_ip_address()` was running `hostname -I` and taking the first token. On a Pi with the USB-Gadget service running, `usb0`'s static `10.55.0.1` consistently lists before `wlan0`'s real Wi-Fi IP, so the "Wi-Fi URL" displayed in the Content Manager submenu (and encoded in the QR) was secretly the USB IP. Replaced with `getifaddrs()` + explicit `wlan0` lookup.
- **VPN port forwarding (NAT-PMP) silently broken** for the entire stack: `FIREWALL_OUTBOUND_SUBNETS=192.168.0.0/16,10.0.0.0/8,172.16.0.0/12` looked correct but secretly shadowed ProtonVPN's `10.2.0.1` WireGuard gateway — Gluetun's iptables routed NAT-PMP requests out the LAN interface instead of through the tunnel, and the port-forwarding service gave up after 9 retries (`read udp …:5351: i/o timeout`). Result: dashboard read "VPN port unavailable" forever, qBit was firewalled, torrents downloaded slowly. Narrowed `FIREWALL_OUTBOUND_SUBNETS` to specific `/24` and `/16` LAN subnets that don't include `10.2.0.0/16`. Verified live: gluetun obtains forwarded port 38764 within 1 sec of restart, qbit-port-sync picks it up, dashboard flips to "✓ synced".
- **Two-player not working**: launcher emitted only `input_player1_*` bindings. With autoconfig disabled (we delete the file to force manual mappings for predictable behavior across DragonRise/Microntek pads), a 2nd identical pad showed up at `/dev/input/js1` but generated no in-game effect. See "Two-player support across all 7 cores" in Added for the fix.
- **Modern TV settings menu would scroll prematurely**: `move_selection()` had a hardcoded `max_visible_items=7` that didn't match the renderer's now-dynamic row count. Selecting "Back" with 9 rows visible would still bump scroll_offset to 1 and pop "Video Games" off the top. Renderer now publishes `max_visible_items` back to the input handler.
- **QR code shows wrong USB URL** (legacy bug): hardcoded to `http://192.168.7.1:5000`. Now reads live `usb0` IPv4 via `getifaddrs`.
- **`usb0` carrier check was IPv4-only**: settings menu marked usb0 "active" any time the gadget service had assigned `10.55.0.1`, even with no cable plugged in. Now requires `IFF_RUNNING` flag in addition to IPv4 presence.
- **Content Manager submenu QR could go stale**: now auto-rebuilds once per second while open so the QR + label reflect carrier-state changes (cable plugged/unplugged) without requiring the user to navigate out and back in.
- **Wi-Fi scan returned only 1 network**: `nmcli --rescan yes` doesn't actually wait. Fixed with explicit `nmcli dev wifi rescan` + 4-second sleep + plain `list` query.
- **Wi-Fi Disconnect was a no-op**: the SELECT handler re-ran `enter_submenu(WIFI)` after the action lambda flipped `wifi_disconnect_confirm_=true`, immediately resetting the flag back to false. See "Idempotent setting-menu re-entry" in Added for the structural fix.
- **Movie playback aspect ratio was stretched**: `gst_renderer.cpp` ignored source aspect — widescreen movies looked vertically stretched on a 16:9 TV. Now letterboxes 2.35:1 / 2.39:1 movies, pillarboxes 4:3 content, handles anamorphic DVDs correctly via pixel-aspect-ratio extraction from GStreamer caps.
- **Bezel disappearing after RetroArch exit**: removed a stale `static loaded_bezel_path` tracker in the main render loop that blocked `load_bezel()` from being called after `reset_gl()` destroyed the bezel texture. The renderer's own dedupe handles repeat calls correctly.
- **Bezel obscuring RetroArch in-game menu**: set `input_overlay_hide_in_menu = "true"` so the bezel auto-hides when the RetroArch menu opens (Z+Start hotkey).
- **Loading screen z-order**: the brief "Loading..." overlay during the kiosk → RetroArch handoff now renders the bezel on top of the loading text instead of the loading text covering the bezel — continuous visual framing through the transition.
- **Phase 0 test #33 (video playlist linkage) false-failed** on newly-uploaded videos because the test cd'd to `$PI_INSTALL_ROOT` instead of `$PI_INSTALL_ROOT/data`, so playlist-relative paths like `media/foo.mp4` didn't resolve.
- **Reorder warning** in `SettingsMenuManager` constructor init list (initialized fields out of declaration order — compiler initializes in declaration order regardless, so a mismatched init list is misleading at best, footgun at worst).

### Improved
- **PS1 core (`pcsx_rearmed`) tuning**: SPU reverb disabled, SPU interpolation off — small CPU savings with no audible impact on most titles. Combined with the global `video_threaded = true` (was false), notably smoother on Pi 4B.
- **RetroArch launcher refactor**: extracted duplicated video-config blocks from `launch_drm()` and `open_core_downloader_direct()` into a single `write_video_config(stream, opts)` helper that branches on `display_mode`. Per-core controller mapping logic split into `get_mapping_n64_adapter()` and `get_mapping_ps_style()` with a `get_mapping(type, core_name)` dispatcher.
- **RetroArch autoconfig file** now matches whichever controller is detected (`0e6d_111d.cfg` for N64, `0079_0006.cfg` for PS-style, none for UNKNOWN).
- **"or visit magicpi.local on any device"** hint added below the Content Manager QR for users without a phone camera handy.

### Pre-image testing pass
- All 7 cores launched, played, exited cleanly: NES, SNES, Genesis, PS1 (incl. 2-player Twisted Metal 4), PC Engine, Atari 7800, Arcade
- All 4 video bezels (1974, 1986, KV19, plus a Vintage TV) cycled correctly in Modern TV mode
- Content Manager web admin drag-and-drop verified for both video and game playlists
- Media Browser end-to-end: TMDB browse → search → add to library → indexer search → grab → download via VPN tunnel → playback with auto-pause → confirm-remove orphan-proof cleanup
- Pre-image checklist [tests/manual/pre_image_checklist.md](tests/manual/pre_image_checklist.md) updated with sign-off notes for Phases 0-4, 8-10
- Phase 5 (CRT mode) deferred until a CRT TV is available — code path validated via mode round-trip but visual verification pending
- Phase 6 (15-min PS1 endurance) optional, not blocking

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
