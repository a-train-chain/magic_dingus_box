from __future__ import annotations

import logging
import subprocess
import time
from typing import Any, Optional


class VlcClient:
    """VLC media player client using cvlc subprocess.
    
    Provides an API compatible with MpvClient for seamless replacement.
    Enables hardware-accelerated video playback on Raspberry Pi via DRM/KMS.
    Uses cvlc command-line for direct-to-kernel video output (no X11 required).
    """

    def __init__(self, audio_device: str = "plughw:CARD=vc4hdmi0,DEV=0") -> None:
        self._log = logging.getLogger("vlc")
        
        # Base cvlc command with DRM/KMS output
        self.base_cmd = [
            "cvlc",
            "--fullscreen",
            "--vout=drm",  # Direct DRM/KMS output (headless compatible)
            "--drm-vout-display=HDMI-A-1",  # Primary HDMI
            "--aout=alsa",  # ALSA audio
            f"--alsa-audio-device={audio_device}",
            "--avcodec-hw=mmal",  # Hardware decoding
            "--no-video-title-show",
            "--no-osd",
            "--no-spu",  # No subtitles
            "--play-and-exit",  # Exit when done (for non-looping)
        ]
        
        # Track state
        self.current_path: Optional[str] = None
        self.current_start: Optional[float] = None
        self.current_end: Optional[float] = None
        self._process: Optional[subprocess.Popen] = None
        self._paused = False
        self._volume = 100
        self._loop_enabled = False
        self._playback_start_time = 0.0
        
        self._log.info("VLC client initialized with DRM/KMS output")

    def load_file(self, path: str, start: Optional[float] = None, end: Optional[float] = None) -> None:
        """Load and play a video file using cvlc subprocess.
        
        Args:
            path: Path to video file
            start: Start time in seconds (optional)
            end: End time in seconds (optional)
        """
        self._log.info(f"Loading file: {path} (start={start}, end={end})")
        
        # Stop any existing playback
        self.stop()
        
        self.current_path = path
        self.current_start = start
        self.current_end = end
        self._playback_start_time = time.time()
        
        # Build cvlc command
        cmd = self.base_cmd.copy()
        
        # Add start/end time options
        if start is not None:
            cmd.append(f"--start-time={start}")
        if end is not None:
            cmd.append(f"--stop-time={end}")
        
        # Add loop option if enabled
        if self._loop_enabled:
            cmd.append("--loop")
        
        # Add file path
        cmd.append(path)
        
        # Launch cvlc process
        try:
            self._process = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL
            )
            self._log.info(f"cvlc process started (PID={self._process.pid})")
        except Exception as exc:
            self._log.error(f"Failed to start cvlc: {exc}")
            self._process = None

    def pause(self) -> None:
        """Pause playback (not supported with cvlc subprocess)."""
        self._paused = True
        self._log.warning("Pause not supported with cvlc subprocess - stopping instead")
        self.stop()

    def resume(self) -> None:
        """Resume playback (restart file if stopped)."""
        if self._paused and self.current_path:
            # Restart from beginning if paused
            self.load_file(self.current_path, self.current_start, self.current_end)
            self._paused = False
        self._log.debug("Resumed (restarted playback)")

    def toggle_pause(self) -> None:
        """Toggle pause state (stop/restart with cvlc)."""
        if self._paused or not self.is_playing():
            self.resume()
        else:
            self.pause()
        self._log.debug(f"Pause toggled: {self._paused}")

    def seek(self, seconds: float) -> None:
        """Seek relative to current position (restart from new position).
        
        Args:
            seconds: Number of seconds to seek (positive or negative)
        """
        if self.current_path and self.current_start is not None:
            # Restart from new position
            new_start = max(0, self.current_start + seconds)
            self.load_file(self.current_path, new_start, self.current_end)
            self._log.debug(f"Seeked {seconds}s by restarting at {new_start}s")
        else:
            self._log.warning("Cannot seek - no current file or start position")

    def seek_absolute(self, timestamp: float) -> None:
        """Seek to an absolute timestamp (restart from position).
        
        Args:
            timestamp: Absolute time position in seconds
        """
        if self.current_path:
            self.load_file(self.current_path, timestamp, self.current_end)
            self._log.debug(f"Seeked to {timestamp}s by restarting")

    def set_loop_file(self, enabled: bool) -> None:
        """Set whether the current file should loop.
        
        Args:
            enabled: True to enable looping, False to disable
        """
        self._loop_enabled = enabled
        # VLC doesn't have direct loop-file property, we'll handle it in is_at_end()
        self._log.debug(f"Loop {'enabled' if enabled else 'disabled'}")

    def set_volume(self, level: int) -> None:
        """Set volume level (tracked but not changeable with cvlc subprocess).
        
        Args:
            level: Volume level (0-100)
        """
        self._volume = max(0, min(100, level))
        # cvlc subprocess doesn't support runtime volume control
        # Volume would need to be set at launch time
        self._log.debug(f"Volume tracked: {self._volume} (cvlc doesn't support runtime volume change)")

    def enable_audio_fade(self, fade_duration: float = 1.0) -> None:
        """Enable audio fade-in (not supported by VLC in the same way as mpv).
        
        Args:
            fade_duration: Duration of fade-in in seconds
        """
        # VLC doesn't support audio fade filters the same way as mpv
        # This is a no-op for compatibility
        pass

    def set_property(self, name: str, value: Any) -> None:
        """Set a property (for compatibility with mpv API).
        
        Args:
            name: Property name
            value: Property value
        """
        # Handle special properties
        if name == "pause":
            if value:
                self.pause()
            else:
                self.resume()
        elif name == "volume":
            self.set_volume(int(value))
        elif name == "loop-file":
            self.set_loop_file(value == "inf" or value is True)
        elif name == "video":
            # Enable/disable video track for audio-only mode
            # With cvlc subprocess, we can't disable video track at runtime
            # We'll need to stop/restart the process
            if value == "no" or value is False:
                self._log.info("Stopping cvlc for audio-only mode (subprocess limitation)")
                self.stop()
            elif value == "auto" or value is True:
                # Restart playback if we have a current file
                if self.current_path and not self.is_playing():
                    self._log.info("Restarting cvlc to enable video")
                    self.load_file(self.current_path, self.current_start, self.current_end)
        elif name in ("input-vo-keyboard", "input-default-bindings", "hwdec", 
                      "vd-lavc-dr", "vd-lavc-fast", "speed", "window-scale", 
                      "wid", "fullscreen", "ontop"):
            # These are mpv-specific properties - ignore for compatibility
            pass
        else:
            self._log.debug(f"Property {name}={value} not implemented (cvlc subprocess)")

    def get_property(self, name: str) -> Any:
        """Get a property value (limited with cvlc subprocess).
        
        Args:
            name: Property name
            
        Returns:
            Property value or None
        """
        if name == "time-pos":
            # Estimate current position based on elapsed time
            if self.is_playing() and self.current_start is not None:
                elapsed = time.time() - self._playback_start_time
                return self.current_start + elapsed
            return None
        elif name == "duration":
            # Duration not available with cvlc subprocess
            return None
        elif name == "pause":
            return self._paused
        elif name == "eof-reached":
            # Check if process has ended
            if self._process is None:
                return True
            return self._process.poll() is not None
        elif name == "idle-active":
            # Check if not playing
            return not self.is_playing()
        elif name == "path":
            return self.current_path
        elif name == "audio-device-list":
            # Return empty list for compatibility
            return []
        elif name == "ao":
            # Return empty list
            return []
        elif name == "wid":
            # Window ID not applicable for DRM output
            return None
        else:
            self._log.debug(f"Property {name} not implemented (cvlc subprocess)")
            return None

    def stop(self) -> None:
        """Stop playback and terminate cvlc process."""
        if self._process is not None:
            try:
                self._process.terminate()
                self._process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._process.kill()
            except Exception as exc:
                self._log.warning(f"Error stopping cvlc: {exc}")
            finally:
                self._process = None
        
        self.current_path = None
        self.current_start = None
        self.current_end = None
        self._log.debug("Playback stopped")

    def is_playing(self) -> bool:
        """Check if cvlc process is currently running.
        
        Returns:
            True if playing, False otherwise
        """
        if self._process is None:
            return False
        return self._process.poll() is None

    def cleanup(self) -> None:
        """Release VLC resources."""
        try:
            self.stop()
            self._log.info("VLC client cleaned up")
        except Exception as exc:
            self._log.warning(f"Cleanup error: {exc}")

