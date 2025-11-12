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

    def load_current(self, start_playback: bool = False) -> None:
        """Load the current item without starting playback (for seamless transitions).
        
        Args:
            start_playback: If True, start playback immediately. If False, load but keep paused.
        """
        item = self._current_item()
        if item is None:
            return
        
        # Handle emulated games (always start immediately, no transition needed)
        if item.source_type == "emulated_game":
            if not item.path:
                self._log.warning("Emulated game missing path: %s", item.title)
                return
            
            # Use emulator_core from playlist item, or fallback to RETROARCH_CORES
            from .retroarch_launcher import RETROARCH_CORES
            core = item.emulator_core
            if not core and item.emulator_system:
                # Fallback to default core for system if not specified
                core = RETROARCH_CORES.get(item.emulator_system.upper())
                if core:
                    self._log.info("Using default core for %s: %s", item.emulator_system, core)
            
            if not core:
                self._log.warning("Emulated game missing core: %s (system: %s)", item.title, item.emulator_system)
                return
            
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
            self._log.info("Launching game: %s with core: %s", item.title, core)
            self._log.info("DEBUG: item.emulator_core=%s, core variable=%s, item.emulator_system=%s", 
                          item.emulator_core, core, item.emulator_system)
            self.retroarch.launch_game(resolved, core, overlay_path)
            self._log.info("Returned from game: %s", item.title)
            return
        
        # Handle video playback
        if item.source_type == "local" and item.path:
            resolved = self._resolve_local_path(item)
            if resolved is None:
                self._log.warning("Item path not found: %s", item.path)
                return
            self.mpv.load_file(resolved, item.start, item.end)
            # Ensure normal playback speed (fix slow-motion issues)
            try:
                self.mpv.set_property("speed", 1.0)
                # Set video scaling to fill screen height with margins (letterboxing/pillarboxing)
                self.mpv.set_property("video-zoom", 0.0)  # Reset zoom
                self.mpv.set_property("panscan", 0.0)  # No pan/scan - show full video with margins
                self.mpv.set_property("video-aspect", -1)  # Use video's native aspect ratio
                # Force window to fill screen (non-blocking)
                try:
                    self.mpv.set_property("window-scale", 1.0)  # Scale to window size
                except Exception:
                    pass
            except Exception:
                pass
            
            if start_playback:
                self.paused = False
                self.mpv.resume()
            else:
                # Keep paused - transition manager will handle showing and resuming
                self.paused = True
                self.mpv.pause()
        else:
            self._log.warning("Unsupported source_type=%s", item.source_type)

    def play_current(self) -> None:
        """Load and start playback of current item (for direct playback without transitions)."""
        self.load_current(start_playback=True)

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
        mpv will loop automatically (handled by checking state).
        """
        if not self.playlist:
            return False
        eof = self.mpv.get_property("eof-reached")
        # If looping is enabled and we reach the end, restart the current track
        if eof and self.loop:
            self.play_current()
            return False
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
        1) Prefer .30fps.* version if it exists (for smoother playback on Pi)
        2) Relative to the playlist YAML directory (common in dev)
        3) Relative to current working directory
        Returns None if the file cannot be resolved.
        """
        def check_30fps_version(path: Path) -> Optional[str]:
            """Check if a .30fps.* version exists and return it if found."""
            if not path.exists():
                return None
            # Check for .30fps.* version in the same directory
            parent = path.parent
            name_no_ext = path.stem
            ext = path.suffix
            # Try .30fps.{ext} pattern
            fps_version = parent / f"{name_no_ext}.30fps{ext}"
            if fps_version.exists():
                return str(fps_version)
            return None
        
        try:
            if not item.path:
                return None
            p = Path(item.path).expanduser()
            
            # Check absolute path first
            if p.is_absolute():
                # Check for 30fps version first
                fps_path = check_30fps_version(p)
                if fps_path:
                    return fps_path
                if p.exists():
                    return str(p)
            
            # Try relative to playlist file
            if self.playlist and self.playlist.source_path is not None:
                candidate = (self.playlist.source_path.parent / p).resolve()
                fps_path = check_30fps_version(candidate)
                if fps_path:
                    return fps_path
                if candidate.exists():
                    return str(candidate)
            
            # Fallback: relative to CWD
            candidate = (Path.cwd() / p).resolve()
            fps_path = check_30fps_version(candidate)
            if fps_path:
                return fps_path
            if candidate.exists():
                return str(candidate)
            
            # Also try sibling media/ under the playlist directory for dev parity
            if self.playlist and self.playlist.source_path is not None:
                candidate = (self.playlist.source_path.parent / ".." / "media" / p.name).resolve()
                fps_path = check_30fps_version(candidate)
                if fps_path:
                    return fps_path
                if candidate.exists():
                    return str(candidate)
            
            # On Pi: map dev_data/ paths to /data/ paths
            if str(p).startswith("dev_data/"):
                # Replace dev_data/ with /data/
                data_path = Path("/data") / str(p)[len("dev_data/"):]
                fps_path = check_30fps_version(data_path)
                if fps_path:
                    return fps_path
                if data_path.exists():
                    return str(data_path)
        except Exception:
            pass
        return None

