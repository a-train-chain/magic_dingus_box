from __future__ import annotations

import logging
from pathlib import Path
from typing import Optional

from ..library.models import Playlist, PlaylistItem
from .mpv_client import MpvClient
from .retroarch_launcher import RetroArchLauncher


class PlaybackController:
    def __init__(self, mpv_client: MpvClient, settings_store=None, assets_dir=None) -> None:
        self._log = logging.getLogger("controller")
        self.mpv = mpv_client
        self.playlist: Optional[Playlist] = None
        self.index: int = 0
        self.loop: bool = False
        self.paused: bool = False
        # RetroArch launcher for emulated games
        self.retroarch = RetroArchLauncher()
        # Settings and assets for bezel overlay
        self.settings_store = settings_store
        self.assets_dir = assets_dir

    # Playlist control
    def load_playlist(self, playlist: Playlist) -> None:
        self.playlist = playlist
        self.index = 0
        self.loop = playlist.loop
        self._apply_loop()

    def play_current(self) -> None:
        item = self._current_item()
        if item is None:
            return
        
        # Handle emulated games
        if item.source_type == "emulated_game":
            if item.path and item.emulator_core:
                resolved = self._resolve_local_path(item)
                if resolved is None:
                    self._log.warning("ROM not found: %s", item.path)
                    return
                
                # Get current bezel overlay if in bezel mode
                overlay_path = None
                if self.settings_store and self.assets_dir:
                    display_mode = self.settings_store.get_display_mode()
                    if display_mode == "modern_bezel":
                        bezel_style = self.settings_store.get("bezel_style", "retro_tv_1")
                        bezel_file = f"{bezel_style}.png"
                        bezel_path = self.assets_dir / "bezels" / bezel_file
                        if bezel_path.exists():
                            overlay_path = str(bezel_path)
                            self._log.info(f"Will use bezel overlay in game: {bezel_style}")
                
                # Launch RetroArch - this blocks until game exits
                self._log.info("Launching game: %s", item.title)
                self.retroarch.launch_game(resolved, item.emulator_core, overlay_path)
                self._log.info("Returned from game: %s", item.title)
            else:
                self._log.warning("Emulated game missing path or core: %s", item.title)
            return
        
        # Handle video playback
        if item.source_type == "local" and item.path:
            resolved = self._resolve_local_path(item)
            if resolved is None:
                self._log.warning("Item path not found: %s", item.path)
                return
            self.mpv.load_file(resolved, item.start, item.end)
            self.paused = False
            # Actively unpause to ensure playback starts even if mpv defaulted to paused
            self.mpv.resume()
        else:
            self._log.warning("Unsupported source_type=%s", item.source_type)

    def next_item(self) -> None:
        if not self.playlist or not self.playlist.items:
            return
        self.index = (self.index + 1) % len(self.playlist.items)
        self.play_current()

    def previous_item(self) -> None:
        if not self.playlist or not self.playlist.items:
            return
        self.index = (self.index - 1) % len(self.playlist.items)
        self.play_current()

    # Playback control
    def toggle_pause(self) -> None:
        self.mpv.toggle_pause()
        self.paused = not self.paused

    def seek_relative(self, seconds: float) -> None:
        self.mpv.seek(seconds)

    def seek_absolute(self, timestamp: float) -> None:
        """Seek to an absolute timestamp in the current video.
        
        Args:
            timestamp: Absolute time position in seconds
        """
        self.mpv.seek_absolute(timestamp)

    def toggle_loop(self) -> None:
        self.loop = not self.loop
        self._apply_loop()

    # Status helpers for UI
    def status_text(self) -> str:
        if not self.playlist:
            return "Idle"
        item = self._current_item()
        if item is None:
            return self.playlist.title
        state = "Paused" if self.paused else "Playing"
        return f"{state}: {item.title}"

    def elapsed_and_duration(self):
        elapsed = self.mpv.get_property("time-pos")
        duration = self.mpv.get_property("duration")
        try:
            return float(elapsed) if elapsed is not None else None, float(duration) if duration is not None else None
        except Exception:
            return None, None

    def is_at_end(self) -> bool:
        """Check if the current track has ended.
        
        Returns True when mpv reports end-of-file. If loop-file is enabled,
        mpv will never report eof-reached (file loops forever), so this will
        naturally return False for looping tracks.
        """
        if not self.playlist:
            return False
        eof = self.mpv.get_property("eof-reached")
        return eof is True

    # Internals
    def _current_item(self):
        if not self.playlist or not self.playlist.items:
            return None
        return self.playlist.items[self.index]

    def _apply_loop(self) -> None:
        self.mpv.set_loop_file(self.loop)

    def _resolve_local_path(self, item: PlaylistItem) -> Optional[str]:
        """Resolve item.path to an absolute filesystem path.

        Priority:
        1) Relative to the playlist YAML directory (common in dev)
        2) Relative to current working directory
        Returns None if the file cannot be resolved.
        """
        try:
            if not item.path:
                return None
            p = Path(item.path).expanduser()
            if p.is_absolute() and p.exists():
                return str(p)
            # Try relative to playlist file
            if self.playlist and self.playlist.source_path is not None:
                candidate = (self.playlist.source_path.parent / p).resolve()
                if candidate.exists():
                    return str(candidate)
            # Fallback: relative to CWD
            candidate = (Path.cwd() / p).resolve()
            if candidate.exists():
                return str(candidate)
            # Also try sibling media/ under the playlist directory for dev parity
            if self.playlist and self.playlist.source_path is not None:
                candidate = (self.playlist.source_path.parent / ".." / "media" / p.name).resolve()
                if candidate.exists():
                    return str(candidate)
        except Exception:
            pass
        return None

