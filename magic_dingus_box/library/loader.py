from __future__ import annotations

import logging
from pathlib import Path
from typing import List

import yaml

from .models import Playlist, PlaylistItem


class PlaylistLibrary:
    def __init__(self, directory: Path) -> None:
        self.directory = Path(directory)

    def load_playlists(self) -> List[Playlist]:
        playlists: List[Playlist] = []
        if not self.directory.exists():
            return playlists
        for path in sorted(self.directory.glob("*.y*ml")):
            try:
                with path.open("r", encoding="utf-8") as fh:
                    data = yaml.safe_load(fh) or {}
                playlist = self._parse_playlist(data, path)
                playlists.append(playlist)
            except Exception as exc:
                logging.getLogger(__name__).warning("Failed to load %s: %s", path, exc)
        return playlists

    def _parse_playlist(self, data: dict, source_path: Path) -> Playlist:
        title = str(data.get("title", source_path.stem))
        curator = str(data.get("curator", ""))
        description = str(data.get("description", ""))
        loop = bool(data.get("loop", False))

        items: List[PlaylistItem] = []
        for raw in data.get("items", []) or []:
            try:
                item = PlaylistItem(
                    title=str(raw.get("title", "Untitled")),
                    source_type=str(raw.get("source_type", "local")),
                    artist=raw.get("artist"),
                    path=raw.get("path"),
                    url=raw.get("url"),
                    start=self._opt_float(raw.get("start")),
                    end=self._opt_float(raw.get("end")),
                    tags=list(raw.get("tags", []) or []),
                    emulator_core=raw.get("emulator_core"),
                    emulator_system=raw.get("emulator_system"),
                )
                items.append(item)
            except Exception as exc:
                logging.getLogger(__name__).warning("Bad item in %s: %s", source_path, exc)

        return Playlist(
            title=title,
            curator=curator,
            description=description,
            loop=loop,
            items=items,
            source_path=source_path,
        )

    def _opt_float(self, value):  # type: ignore[no-untyped-def]
        if value is None:
            return None
        try:
            return float(value)
        except Exception:
            return None

