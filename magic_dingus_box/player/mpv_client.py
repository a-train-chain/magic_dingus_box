from __future__ import annotations

import json
import logging
import os
import socket
import time
from typing import Any, Optional


class MpvClient:
    """Lightweight client for mpv IPC over UNIX socket.

    Methods are tolerant to connection errors; commands are best-effort.
    """

    def __init__(self, socket_path: str) -> None:
        self.socket_path = socket_path
        self._log = logging.getLogger("mpv")
        self._sock: Optional[socket.socket] = None
        self._request_id = 0

    def _connect(self) -> bool:
        if self._sock is not None:
            return True
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(0.5)
            sock.connect(self.socket_path)
            self._sock = sock
            self._log.info("Connected to mpv at %s", self.socket_path)
            return True
        except Exception as exc:
            self._log.debug("mpv connect failed: %s", exc)
            self._sock = None
            return False

    def _send(self, command: list[Any], expect_response: bool = False) -> Any:
        if not self._connect():
            return None
        self._request_id += 1
        payload = {"command": command, "request_id": self._request_id}
        line = json.dumps(payload) + "\n"
        try:
            assert self._sock is not None
            self._sock.sendall(line.encode("utf-8"))
            if not expect_response:
                return None
            # Read lines until matching request_id
            buff = b""
            while True:
                chunk = self._sock.recv(4096)
                if not chunk:
                    break
                buff += chunk
                while b"\n" in buff:
                    msg, buff = buff.split(b"\n", 1)
                    if not msg:
                        continue
                    data = json.loads(msg.decode("utf-8"))
                    if data.get("request_id") == self._request_id:
                        return data
        except Exception as exc:
            self._log.debug("mpv send failed: %s", exc)
            # Drop connection for next attempt
            try:
                if self._sock is not None:
                    self._sock.close()
            finally:
                self._sock = None
        return None

    # Public commands
    def load_file(self, path: str, start: Optional[float] = None, end: Optional[float] = None) -> None:
        args: list[Any] = ["loadfile", path, "replace"]
        if start is not None:
            args.append(f"start={start}")
        if end is not None:
            args.append(f"end={end}")
        self._send(args)

    def pause(self) -> None:
        self.set_property("pause", True)

    def resume(self) -> None:
        self.set_property("pause", False)

    def toggle_pause(self) -> None:
        self._send(["cycle", "pause"])  # type: ignore[arg-type]

    def seek(self, seconds: float) -> None:
        self._send(["seek", seconds, "relative"])

    def seek_absolute(self, timestamp: float) -> None:
        """Seek to an absolute timestamp in the current file.
        
        Args:
            timestamp: Absolute time position in seconds
        """
        self._send(["seek", timestamp, "absolute"])

    def set_loop_file(self, enabled: bool) -> None:
        self.set_property("loop-file", "inf" if enabled else "no")

    def set_volume(self, level: int) -> None:
        """Set volume level (0-100)."""
        self.set_property("volume", max(0, min(100, level)))

    def enable_audio_fade(self, fade_duration: float = 1.0) -> None:
        """Enable audio fade-in for all tracks.
        
        Args:
            fade_duration: Duration of fade-in in seconds (default: 1.0)
        """
        # Set audio filter to fade in at the start of every track
        fade_filter = f"afade=t=in:d={fade_duration}"
        self.set_property("af", fade_filter)

    def playlist_next(self) -> None:
        self._send(["playlist-next", "weak"])  # no error if single item

    def playlist_prev(self) -> None:
        self._send(["playlist-prev", "weak"])  # no error if single item

    def set_property(self, name: str, value: Any) -> None:
        self._send(["set_property", name, value])
    
    def set_properties_batch(self, properties: dict[str, Any]) -> None:
        """Set multiple properties in a single IPC call for better performance.
        
        Args:
            properties: Dictionary of property names to values
        """
        if not properties:
            return
        # mpv doesn't have a native batch command, but we can send multiple
        # set_property commands in quick succession without waiting for responses
        # This reduces round-trip overhead
        for name, value in properties.items():
            self._send(["set_property", name, value], expect_response=False)

    def get_property(self, name: str) -> Any:
        resp = self._send(["get_property", name], expect_response=True)
        if isinstance(resp, dict) and resp.get("error") == "success":
            return resp.get("data")
        return None

    def stop(self) -> None:
        """Stop playback (clears the current file)."""
        self._send(["stop"])  # type: ignore[list-item]
    
    def set_fullscreen(self, enabled: bool) -> None:
        """Set fullscreen mode.
        
        Args:
            enabled: True to enable fullscreen, False to disable
        """
        self.set_property("fullscreen", enabled)
    
    def get_fullscreen(self) -> bool:
        """Get current fullscreen state."""
        return bool(self.get_property("fullscreen") or False)

    def playlist_clear(self) -> None:
        """Clear the playlist."""
        self._send(["playlist-clear"])  # type: ignore[list-item]
    
    def get_playlist(self) -> list[dict[str, Any]]:
        """Get current playlist.
        
        Returns:
            List of playlist items, each with 'filename' and optionally 'current' flag
        """
        playlist = self.get_property("playlist")
        if isinstance(playlist, list):
            return playlist
        return []
    
    def set_video_enabled(self, enabled: bool) -> None:
        """Enable or disable video track.
        
        Args:
            enabled: True to enable video, False to disable (audio-only mode)
        """
        self.set_property("video", "auto" if enabled else "no")

