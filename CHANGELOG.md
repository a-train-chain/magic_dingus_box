# Changelog

All notable changes to Magic Dingus Box will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
