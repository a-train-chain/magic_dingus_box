from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple


@dataclass
class PlaylistItem:
    title: str
    source_type: str  # "local", "youtube", or "emulated_game"
    artist: Optional[str] = None  # Artist name (for music videos/songs)
    path: Optional[str] = None
    url: Optional[str] = None
    start: Optional[float] = None
    end: Optional[float] = None
    tags: List[str] = field(default_factory=list)
    # Emulator fields
    emulator_core: Optional[str] = None  # e.g., "nestopia_libretro", "mupen64plus-next_libretro"
    emulator_system: Optional[str] = None  # e.g., "NES", "N64", "PS1"


@dataclass
class Playlist:
    title: str
    curator: str
    description: str = ""
    loop: bool = False
    items: List[PlaylistItem] = field(default_factory=list)
    source_path: Optional[Path] = None  # Path of the YAML file
    
    def is_game_playlist(self) -> bool:
        """Check if this playlist contains only games (no videos)."""
        if not self.items:
            return False
        return all(item.source_type == "emulated_game" for item in self.items)
    
    def is_video_playlist(self) -> bool:
        """Check if this playlist contains any video content."""
        if not self.items:
            return False
        return any(item.source_type in ("local", "youtube") for item in self.items)

