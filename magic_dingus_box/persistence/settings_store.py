"""Persistent settings storage for user preferences."""
from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any, Dict, Optional


class SettingsStore:
    """Manages persistent user settings stored as JSON."""
    
    def __init__(self, settings_file: Path):
        """Initialize settings store.
        
        Args:
            settings_file: Path to settings JSON file
        """
        self.settings_file = Path(settings_file)
        self._log = logging.getLogger("settings")
        self._settings: Dict[str, Any] = {}
        self.load()
    
    def load(self) -> None:
        """Load settings from file."""
        if not self.settings_file.exists():
            self._log.info("No settings file found, using defaults")
            self._settings = {}
            return
        
        try:
            with self.settings_file.open("r") as f:
                self._settings = json.load(f)
            self._log.info(f"Loaded settings from {self.settings_file}")
        except Exception as e:
            self._log.warning(f"Failed to load settings: {e}, using defaults")
            self._settings = {}
    
    def save(self) -> None:
        """Save settings to file."""
        try:
            # Ensure parent directory exists
            self.settings_file.parent.mkdir(parents=True, exist_ok=True)
            
            with self.settings_file.open("w") as f:
                json.dump(self._settings, f, indent=2)
            self._log.info(f"Saved settings to {self.settings_file}")
        except Exception as e:
            self._log.warning(f"Failed to save settings: {e}")
    
    def get(self, key: str, default: Any = None) -> Any:
        """Get a setting value.
        
        Args:
            key: Setting key
            default: Default value if key doesn't exist
            
        Returns:
            Setting value or default
        """
        return self._settings.get(key, default)
    
    def set(self, key: str, value: Any) -> None:
        """Set a setting value and save.
        
        Args:
            key: Setting key
            value: Setting value
        """
        self._settings[key] = value
        self.save()
    
    def ensure_defaults(self, defaults: Dict[str, Any]) -> None:
        """Ensure provided defaults exist without overwriting existing values.
        
        Args:
            defaults: Mapping of key->default_value
        """
        changed = False
        for k, v in defaults.items():
            if k not in self._settings:
                self._settings[k] = v
                changed = True
        if changed:
            self.save()
    
    def get_display_mode(self) -> str:
        """Get display mode setting."""
        return self.get("display_mode", "crt_native")
    
    def set_display_mode(self, mode: str) -> None:
        """Set display mode setting."""
        self.set("display_mode", mode)
    
    def get_modern_resolution(self) -> str:
        """Get modern display resolution setting."""
        return self.get("modern_resolution", "auto")
    
    def set_modern_resolution(self, resolution: str) -> None:
        """Set modern display resolution setting."""
        self.set("modern_resolution", resolution)
    
    def get_show_bezel(self) -> bool:
        """Get CRT bezel visibility setting."""
        return self.get("show_bezel", False)
    
    def set_show_bezel(self, show: bool) -> None:
        """Set CRT bezel visibility setting."""
        self.set("show_bezel", show)

