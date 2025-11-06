"""Bezel asset management and loading."""
from __future__ import annotations

import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

import pygame


@dataclass
class BezelInfo:
    """Information about a bezel asset."""
    id: str
    name: str
    file: Optional[str]
    description: str = ""


class BezelLoader:
    """Manages loading and caching of bezel images."""
    
    def __init__(self, assets_dir: Path):
        """Initialize bezel loader.
        
        Args:
            assets_dir: Path to assets directory containing bezels/
        """
        self.assets_dir = Path(assets_dir)
        self.bezels_dir = self.assets_dir / "bezels"
        self._log = logging.getLogger("bezel_loader")
        self._cache: dict[str, pygame.Surface] = {}
        self._available_bezels: List[BezelInfo] = []
        self._load_bezel_metadata()
    
    def _load_bezel_metadata(self) -> None:
        """Load bezel metadata from bezels.json."""
        metadata_file = self.bezels_dir / "bezels.json"
        if not metadata_file.exists():
            self._log.warning(f"Bezel metadata not found: {metadata_file}")
            return
        
        try:
            with metadata_file.open("r") as f:
                data = json.load(f)
            
            for bezel_data in data.get("bezels", []):
                bezel = BezelInfo(
                    id=bezel_data["id"],
                    name=bezel_data["name"],
                    file=bezel_data.get("file"),
                    description=bezel_data.get("description", "")
                )
                self._available_bezels.append(bezel)
            
            self._log.info(f"Loaded {len(self._available_bezels)} bezel definitions")
        except Exception as e:
            self._log.warning(f"Failed to load bezel metadata: {e}")
    
    def list_available_bezels(self) -> List[BezelInfo]:
        """Get list of available bezels."""
        return self._available_bezels.copy()
    
    def load_bezel(
        self,
        bezel_id: str,
        target_size: Tuple[int, int]
    ) -> Optional[pygame.Surface]:
        """Load and scale a bezel image.
        
        Args:
            bezel_id: Bezel identifier (from bezels.json)
            target_size: Target resolution to scale to
            
        Returns:
            Bezel surface scaled to target size, or None if not found/procedural
        """
        # Check cache first
        cache_key = f"{bezel_id}_{target_size[0]}x{target_size[1]}"
        if cache_key in self._cache:
            return self._cache[cache_key]
        
        # Find bezel info
        bezel_info = None
        for b in self._available_bezels:
            if b.id == bezel_id:
                bezel_info = b
                break
        
        if not bezel_info:
            self._log.warning(f"Bezel not found: {bezel_id}")
            return None
        
        # Procedural bezel (no file)
        if bezel_info.file is None:
            return None
        
        # Load from file
        bezel_path = self.bezels_dir / bezel_info.file
        if not bezel_path.exists():
            self._log.warning(f"Bezel file not found: {bezel_path}")
            return None
        
        try:
            # Load image
            bezel_img = pygame.image.load(str(bezel_path))
            
            # Scale to target resolution with smooth scaling
            scaled_bezel = pygame.transform.smoothscale(bezel_img, target_size)
            
            # Cache it
            self._cache[cache_key] = scaled_bezel
            
            self._log.info(f"Loaded bezel: {bezel_info.name} ({bezel_path.name})")
            return scaled_bezel
            
        except Exception as e:
            self._log.error(f"Failed to load bezel {bezel_path}: {e}")
            return None

