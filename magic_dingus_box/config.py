from __future__ import annotations

import json
import os
import sys
import time
import uuid
from pathlib import Path


class AppConfig:
    """Centralized runtime configuration.

    Values may be overridden by environment variables to simplify dev/testing.
    """

    def __init__(self) -> None:
        self.platform = "macos" if sys.platform == "darwin" else "linux"

        # Root of the repository (two levels up from this file)
        self.repo_root = Path(__file__).resolve().parents[1]

        # Data directories: on Pi use /data, on macOS use ./dev_data
        default_data_dir = Path("/data") if self.platform == "linux" else self.repo_root / "dev_data"
        self.data_dir = Path(os.getenv("MAGIC_DATA_DIR", str(default_data_dir))).resolve()
        self.playlists_dir = self.data_dir / "playlists"
        self.media_dir = self.data_dir / "media"
        self.logs_dir = self.data_dir / "logs"
        self.roms_dir = self.data_dir / "roms"

        # UI settings
        self.screen_width = 720
        self.screen_height = 480
        # Enable fullscreen for proper display
        self.fullscreen = True
        self.enable_scanlines = True

        # mpv IPC socket
        default_socket = "/run/magic/mpv.sock" if self.platform == "linux" else "/tmp/mpv-magic.sock"
        self.mpv_socket = os.getenv("MPV_SOCKET", default_socket)

        # Audio device: HDMI by default on Pi; on macOS let mpv choose
        self.audio_device = os.getenv(
            "MAGIC_AUDIO_DEVICE",
            "auto" if self.platform == "linux" else "auto",
        )

        # Optional modules/features
        self.enable_web_admin = os.getenv("MAGIC_ENABLE_WEB_ADMIN", "1") != "0"

        # Timing/behavior
        self.overlay_fade_seconds = 4.0

        # Branding
        self.product_title = os.getenv("MAGIC_TITLE", "Magic Dingus Box")

        # Video rendering preferences
        # Options: crop (center-crop to 4:3), pad (letterbox/pillarbox to 4:3), stretch (not recommended)
        self.video_fit = os.getenv("MAGIC_VIDEO_FIT", "crop")
        self.target_aspect = os.getenv("MAGIC_TARGET_ASPECT", "4/3")
        self.scale_width = int(os.getenv("MAGIC_SCALE_WIDTH", "720"))
        
        # Display mode settings
        self.display_mode = os.getenv("MAGIC_DISPLAY_MODE", "crt_native")
        self.show_crt_bezel = os.getenv("MAGIC_SHOW_BEZEL", "0") == "1"
        self.modern_resolution = self._parse_resolution(
            os.getenv("MAGIC_MODERN_RES", "auto")
        )
        
        # Settings file location
        self.settings_file = self.data_dir / "settings.json"
        
        # Device identity
        self.device_info_file = self.data_dir / "device_info.json"
        self.device_id = self._get_or_create_device_id()
        self.device_name = self._get_device_name()

    def _parse_resolution(self, res_string: str) -> tuple:
        """Parse resolution string like '1920x1080' to tuple (1920, 1080).
        
        Args:
            res_string: Resolution in format 'WIDTHxHEIGHT' or 'auto'
            
        Returns:
            Tuple of (width, height), or (1920, 1080) for 'auto' as placeholder
        """
        if res_string == "auto":
            # Return placeholder - actual resolution detected at runtime
            return (1920, 1080)
        
        try:
            parts = res_string.split('x')
            return (int(parts[0]), int(parts[1]))
        except Exception:
            # Fallback to 1080p if parsing fails
            return (1920, 1080)
    
    def ensure_data_dirs(self) -> None:
        """Create data directories if writable (useful on macOS dev).

        On the Pi, the root FS may be read-only; /data should be RW if present.
        """
        for path in (self.playlists_dir, self.media_dir, self.logs_dir, self.roms_dir):
            try:
                path.mkdir(parents=True, exist_ok=True)
            except Exception:
                # Ignore if not creatable (e.g., read-only). Logging setup will handle fallbacks.
                pass
    
    def _get_or_create_device_id(self) -> str:
        """Get existing device ID or create a new one.
        
        Returns:
            UUID string identifying this device
        """
        if self.device_info_file.exists():
            try:
                data = json.loads(self.device_info_file.read_text())
                return data.get('device_id', str(uuid.uuid4()))
            except Exception:
                pass
        
        # Create new device ID
        device_id = str(uuid.uuid4())
        self._save_device_info(device_id, None)
        return device_id
    
    def _get_device_name(self) -> str:
        """Get device name (user-editable).
        
        Returns:
            Human-friendly device name
        """
        if self.device_info_file.exists():
            try:
                data = json.loads(self.device_info_file.read_text())
                return data.get('device_name', 'Magic Dingus Box')
            except Exception:
                pass
        return 'Magic Dingus Box'
    
    def _save_device_info(self, device_id: str, device_name: str | None) -> None:
        """Save device info to disk.
        
        Args:
            device_id: UUID for this device
            device_name: User-friendly name, or None to use default
        """
        try:
            self.device_info_file.parent.mkdir(parents=True, exist_ok=True)
            data = {
                'device_id': device_id,
                'device_name': device_name or 'Magic Dingus Box',
                'created_at': time.time()
            }
            self.device_info_file.write_text(json.dumps(data, indent=2))
        except Exception:
            pass

