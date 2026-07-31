# Magic Dingus Box

A retro gaming and video playback kiosk for Raspberry Pi 4B and Raspberry Pi 5.

Magic Dingus Box has two halves:

1. **Retro gaming + video playback** — always works, no internet
   required after setup. Plays NES / SNES / Genesis / PS1 / PCE /
   Atari 7800 / Arcade games via RetroArch, plus local videos and
   YouTube clips.

2. **Movie Media Browser** — discovers and downloads movies via a
   Radarr / Prowlarr / qBittorrent stack. **Requires a VPN**
   (ProtonVPN with WireGuard recommended). See
   [docs/MEDIA_BROWSER_VPN_SETUP.md](magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md)
   for setup.

Without a VPN configured, the Media Browser is fully hidden from
both the kiosk UI and the web Content Manager. Operators must
explicitly unlock it (kiosk-side secret sequence) *and* drop a
working WireGuard config (web admin) before the feature appears.

## What's in this repo

| Path | Purpose |
|---|---|
| [`magic_dingus_box_cpp/`](magic_dingus_box_cpp/) | C++ kiosk engine (DRM/KMS + EGL + GStreamer + UI). Primary application. |
| [`magic_dingus_box/web/`](magic_dingus_box/web/) | Flask Content Manager web admin (LAN-only). Playlist editing, video uploads, ROM management, OTA updates, Media Browser provisioning. |
| [`scripts/golden_image/`](scripts/golden_image/) | Live SD-card cloning toolchain. Produces `.img.gz` files from a running source Pi without removing the SD card. Includes the on-Pi `first_boot.sh` that resets per-Pi state on flashed clones. |
| [`tests/`](tests/) | Two-tier test suite (local-tier bats run anywhere; pi-tier bats run from a dev machine against a Pi over SSH). |
| [`magic_dingus_box_cpp/services/`](magic_dingus_box_cpp/services/) | Media Browser companion-services compose stack (Radarr + Prowlarr + qBittorrent + Gluetun VPN + FlareSolverr). Optional, gated behind a chord-sequence unlock. |
| [`CHANGELOG.md`](CHANGELOG.md) / [`VERSION`](VERSION) | Release history (Keep-a-Changelog format) and the single-source version pin used by OTA. |
| [`OTA_UPDATE_GUARANTEES.md`](OTA_UPDATE_GUARANTEES.md) | The contract describing what an OTA update preserves vs. replaces — read before editing any rsync `--exclude` list. |
| [`CLAUDE.md`](CLAUDE.md) | Architecture notes and project conventions (also serves as agent guidance). |

## What it does

- **Plays curated video playlists** at full-screen with a configurable CRT-effect shader pipeline (scanlines, aperture-grille mask, RGB convergence + phosphor glow, luma-driven halation) — opt-in via `Settings → Display → CRT Engine: Enhanced`.
- **Launches retro-game ROMs** via RetroArch with per-core controller mappings for 7 systems: NES, SNES, Genesis/Mega Drive, PC Engine, Atari 7800, PlayStation 1, and arcade (FBNeo). Auto-saves SRAM and save states on exit, auto-loads on next launch.
- **Routes input** from rotary encoder, GPIO buttons, and USB controllers (currently supports an N64-style USB adapter and PlayStation-style pads with an internal `controller_detector` distinguishing the two).
- **Routes audio** through PulseAudio with selectable HDMI / 3.5mm headphone output and a per-game RetroArch volume offset.
- **Updates over the air** from GitHub Releases via [`magic_dingus_box_cpp/scripts/update.sh`](magic_dingus_box_cpp/scripts/update.sh), with backup + rollback support and a documented preservation contract for operator content.
- **Clones live** to a `.img.gz` for a golden-image distribution flow — fresh Pis boot, run `first_boot.sh` once, and end up with their own identity (UUID, hostname, fresh WiFi setup) while inheriting the source's curated content (videos, playlists, ROMs, saves, thumbnails).
- **Optional Media Browser** for movie discovery + download (TMDB / Radarr / Prowlarr / qBittorrent over a Gluetun-protected VPN tunnel with NAT-PMP port forwarding). Hidden by default behind a chord-sequence unlock; configured per-Pi via the Content Manager web UI without ever needing SSH.

## Quick start

If you're setting up a fresh master Pi from scratch, see [`magic_dingus_box_cpp/docs/DEPLOYMENT_GUIDE.md`](magic_dingus_box_cpp/docs/DEPLOYMENT_GUIDE.md). For an existing Pi:

```bash
# From your dev machine, with PI_HOST set (defaults to magic@magicpi.local)
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build       # rsync + compile on Pi
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build --test  # ...then launch interactively over SSH
```

USB-gadget transport (much faster than WiFi for big rebuilds):

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --usb-gadget   # one-time Pi setup, then reboot Pi
PI_HOST=magic@10.55.0.1 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Tests:

```bash
./tests/run_all.sh             # both tiers
./tests/run_all.sh --local-only  # no Pi needed
```

See [`tests/README.md`](tests/README.md) for tiers, helpers, and the pre-image gate.

## Build & dependencies

The kiosk engine targets Raspberry Pi OS Lite 64-bit on **Trixie (Debian 13)**. Bookworm is no longer supported as of v1.7.0 — the Pi 5 groundwork (2026-07-20) moved `gpio_manager` to the libgpiod 2.x API, which Bookworm does not ship, and the CI release binary links Trixie's glibc. **v1.6.4 is the last Bookworm-compatible release**; Bookworm-era Pi 4B units cannot OTA past it (the update fails cleanly and rolls back) and need an OS migration to rejoin the release train. Required system packages — install via [`magic_dingus_box_cpp/scripts/install_deps.sh`](magic_dingus_box_cpp/scripts/install_deps.sh):

- `libdrm`, `libgbm`, `libegl`, `libgles2` — DRM/KMS + EGL + OpenGL ES rendering substrate
- `libevdev` — input device events
- `libgpiod` — power button and LED GPIO
- `libsystemd` — sd_notify watchdog integration (optional, auto-detected)
- `gstreamer-1.0` + `gstreamer-app-1.0` + `gstreamer-video-1.0` + `gstreamer-gl-1.0` + `gstreamer1.0-libav` — video pipeline
- `yaml-cpp`, `jsoncpp` — config and playlist parsing

Plus header-only deps fetched at build time: `stb_truetype.h`, `stb_image.h`, `spdlog` (via CMake `FetchContent`), `Catch2` (test targets only).

The web admin requires Python 3 + Flask only; service launched by [`magic_dingus_box_cpp/systemd/magic-dingus-web.service`](magic_dingus_box_cpp/systemd/magic-dingus-web.service).

## License

Same as the main Magic Dingus Box project.
