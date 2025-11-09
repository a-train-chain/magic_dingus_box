from __future__ import annotations

import logging
import os
import subprocess
import threading
import time
from typing import Any, Optional


class GStreamerClient:
    """GStreamer-based video player client using subprocess (gst-launch-1.0).
    
    Provides an API compatible with MpvClient for seamless replacement.
    Uses GStreamer with hardware-accelerated H.264 decoding on Raspberry Pi.
    Uses subprocess approach to avoid Python bindings installation issues.
    """

    def __init__(self, audio_device: str = "plughw:CARD=vc4hdmi0,DEV=0") -> None:
        self._log = logging.getLogger("gstreamer")
        
        self.audio_device = audio_device
        
        # State tracking
        self.current_path: Optional[str] = None
        self.current_start: Optional[float] = None
        self.current_end: Optional[float] = None
        self._process: Optional[subprocess.Popen] = None
        self._paused = False
        self._volume = 100
        self._loop_enabled = False
        self._playback_start_time = 0.0
        self._duration = 0.0
        self._position = 0.0
        self._eof_reached = False
        self._video_disabled = False
        
        self._log.info("GStreamer client initialized (subprocess mode)")

    def _build_pipeline(self, path: str, start: Optional[float] = None, end: Optional[float] = None) -> list[str]:
        """Build gst-launch-1.0 command line arguments with EXPLICIT hardware acceleration.
        
        Uses explicit pipeline with v4l2h264dec to force hardware decode.
        This is more reliable than playbin's auto-selection which often picks software decoder.
        
        Args:
            path: Path to video file
            start: Start time in seconds (optional)
            end: End time in seconds (optional)
            
        Returns:
            List of command arguments
        """
        uri = f"file://{os.path.abspath(path)}"
        cmd = ["gst-launch-1.0", "-q", "playbin"]
        cmd.append(f"uri={uri}")
        
        # Use playbin with custom decoder bin to FORCE hardware decode
        # Create a decoder bin that explicitly uses v4l2h264dec
        if not self._video_disabled:
            # Custom decoder bin: h264parse -> v4l2h264dec (HARDWARE) -> videoconvert -> glimagesink
            # This bypasses decodebin's auto-selection and forces hardware decoder
            cmd.append("video-sink=h264parse ! v4l2h264dec ! videoconvert ! queue max-size-buffers=200 max-size-time=2000000000 max-size-bytes=0 ! glimagesink sync=true max-lateness=-1")
        else:
            # Audio only
            cmd.append("flags=0x00000006")
        
        # Audio sink
        volume_normalized = self._volume / 100.0
        if "plughw:" in self.audio_device:
            alsa_device = "default:CARD=vc4hdmi0"
        else:
            alsa_device = self.audio_device
        cmd.append(f"audio-sink=audioconvert ! volume volume={volume_normalized} ! alsasink device={alsa_device} buffer-time=200000")
        
        return cmd

    def load_file(self, path: str, start: Optional[float] = None, end: Optional[float] = None) -> None:
        """Load a video file for playback.
        
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
        self._eof_reached = False
        
        # Build and launch pipeline
        cmd = self._build_pipeline(path, start, end)
        
        # Set environment variables to optimize performance
        env = os.environ.copy()
        # Try to prefer hardware decoder, but allow software fallback for compatibility
        env["GST_PLUGIN_FEATURE_RANK"] = "v4l2h264dec:MAX"
        # Disable debug output for performance
        env["GST_DEBUG"] = "0"
        # Optimize video decoding - use all available CPU cores
        env["GST_VIDEO_DECODER_MAX_THREADS"] = "4"
        # Increase buffer sizes for smoother playback (reduce stuttering on high-res video)
        env["GST_BUFFER_SIZE"] = "4194304"  # 4MB buffer for 1440x1080@60fps
        # Enable OpenGL for glimagesink
        env["GST_GL_API"] = "opengl"
        # Optimize memory allocation
        env["GST_ALLOC_TRACE"] = "0"
        # Optimize decoder for high-resolution video
        env["GST_VIDEO_DECODER_MAX_LATENCY"] = "100000000"  # 100ms latency
        # Optimize for real-time playback
        env["GST_PLAY_FLAGS"] = "0x00000007"  # Video + Audio + Text
        
        try:
            # Create process with optimized settings
            # Escape shell special characters properly
            import shlex
            # Build command string with proper escaping
            cmd_parts = []
            for part in cmd:
                if "!" in part or " " in part:
                    # Quote parts with special characters
                    cmd_parts.append(shlex.quote(part))
                else:
                    cmd_parts.append(part)
            cmd_str = " ".join(cmd_parts)
            self._process = subprocess.Popen(
                cmd_str,
                shell=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                stdin=subprocess.DEVNULL,
                env=env
            )
            self._log.info(f"GStreamer pipeline started (PID={self._process.pid})")
            self._paused = False
            
            # Start thread to monitor stderr for debugging
            def monitor_stderr():
                if self._process and self._process.stderr:
                    try:
                        for line in iter(self._process.stderr.readline, b''):
                            if line:
                                line_str = line.decode('utf-8', errors='ignore').strip()
                                if 'ERROR' in line_str or 'WARNING' in line_str:
                                    self._log.warning(f"GStreamer: {line_str}")
                    except Exception:
                        pass
            
            threading.Thread(target=monitor_stderr, daemon=True).start()
            
            # If start time specified, seek after a short delay
            if start is not None:
                threading.Timer(0.5, lambda: self.seek_absolute(start)).start()
        except Exception as exc:
            self._log.error(f"Failed to start GStreamer pipeline: {exc}")
            self._process = None

    def pause(self) -> None:
        """Pause playback (not supported with gst-launch-1.0 subprocess)."""
        self._paused = True
        self._log.warning("Pause not fully supported with gst-launch-1.0 subprocess")
        # We can't pause a running gst-launch-1.0 process easily
        # Would need to use gst-launch-1.0 with a control socket or restart

    def resume(self) -> None:
        """Resume playback."""
        self._paused = False
        # Resume is handled by restarting if needed
        if self.current_path and not self.is_playing():
            self.load_file(self.current_path, self.current_start, self.current_end)

    def toggle_pause(self) -> None:
        """Toggle pause state."""
        if self._paused:
            self.resume()
        else:
            self.pause()

    def seek(self, seconds: float) -> None:
        """Seek relative to current position (not supported with subprocess).
        
        Args:
            seconds: Seconds to seek forward (positive) or backward (negative)
        """
        self._log.warning("Relative seek not supported with gst-launch-1.0 subprocess")
        # Would need to restart pipeline with new start time

    def seek_absolute(self, timestamp: float) -> None:
        """Seek to an absolute timestamp (not supported with subprocess).
        
        Args:
            timestamp: Absolute time position in seconds
        """
        self._log.warning("Absolute seek not supported with gst-launch-1.0 subprocess")
        # Would need to restart pipeline with new start time

    def set_loop_file(self, enabled: bool) -> None:
        """Enable or disable file looping.
        
        Args:
            enabled: True to loop, False to play once
        """
        self._loop_enabled = enabled
        # Looping would require restarting the pipeline
        if enabled and self.current_path and not self.is_playing():
            self.load_file(self.current_path, self.current_start, self.current_end)

    def set_volume(self, level: int) -> None:
        """Set volume level (0-100).
        
        Args:
            level: Volume level 0-100
        """
        self._volume = max(0, min(100, level))
        # Volume change requires restarting pipeline
        if self.current_path and self.is_playing():
            self.load_file(self.current_path, self.current_start, self.current_end)

    def enable_audio_fade(self, fade_duration: float = 1.0) -> None:
        """Enable audio fade-in (not supported with playbin subprocess).
        
        Args:
            fade_duration: Duration of fade-in in seconds (default: 1.0)
        """
        self._log.debug(f"Audio fade requested (not implemented in subprocess mode)")

    def set_property(self, name: str, value: Any) -> None:
        """Set a property (for compatibility with mpv API).
        
        Args:
            name: Property name
            value: Property value
        """
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
            # Enable/disable video track
            if value == "no" or value is False:
                self._video_disabled = True
                # Restart pipeline without video
                if self.current_path:
                    self.load_file(self.current_path, self.current_start, self.current_end)
                self._log.info("Video track disabled")
            elif value == "auto" or value is True:
                self._video_disabled = False
                # Restart pipeline with video
                if self.current_path:
                    self.load_file(self.current_path, self.current_start, self.current_end)
                self._log.info("Video track enabled")
        elif name in ("speed", "audio-device"):
            # Handle other properties
            if name == "audio-device":
                self.audio_device = str(value)
        else:
            self._log.debug(f"Property {name}={value} not implemented")

    def get_property(self, name: str) -> Any:
        """Get a property value.
        
        Args:
            name: Property name
            
        Returns:
            Property value or None
        """
        if name == "time-pos":
            # Estimate position based on start time
            if self._playback_start_time > 0:
                elapsed = time.time() - self._playback_start_time
                if self.current_start:
                    return self.current_start + elapsed
                return elapsed
            return None
        elif name == "duration":
            # Can't get duration easily from subprocess
            # Would need to probe file first
            return None
        elif name == "eof-reached":
            # Check if process has exited
            if self._process:
                return self._process.poll() is not None
            return True
        elif name == "pause":
            return self._paused
        elif name == "volume":
            return self._volume
        elif name == "loop-file":
            return self._loop_enabled
        elif name == "audio-device-list":
            # Return a mock list for compatibility
            return [{"name": f"alsa:{self.audio_device}", "description": "ALSA device"}]
        else:
            self._log.debug(f"Property {name} not implemented")
            return None

    def stop(self) -> None:
        """Stop playback."""
        if self._process:
            try:
                self._process.terminate()
                try:
                    self._process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self._process.kill()
                    self._process.wait()
            except Exception as exc:
                self._log.warning(f"Error stopping process: {exc}")
            finally:
                self._process = None
                self._paused = False
                self._eof_reached = False
                self._log.info("Playback stopped")

    def is_playing(self) -> bool:
        """Check if playback is active.
        
        Returns:
            True if playing, False otherwise
        """
        if not self._process:
            return False
        return self._process.poll() is None
