from __future__ import annotations

import logging
import threading
import time
from pathlib import Path
from typing import Callable, Optional


class PlaylistWatcher:
    """Simple directory watcher using mtime polling to avoid heavy deps.

    On systems with watchdog available, this can be replaced in the future.
    """

    def __init__(self, directory: Path, on_change: Callable[[], None], interval_seconds: float = 1.5) -> None:
        self.directory = Path(directory)
        self.on_change = on_change
        self.interval_seconds = interval_seconds
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._last_state: dict[str, float] = {}

    def start(self) -> None:
        if self._thread is not None:
            return
        self._snapshot()
        t = threading.Thread(target=self._loop, name="PlaylistWatcher", daemon=True)
        self._thread = t
        t.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)

    def _snapshot(self) -> dict[str, float]:
        state: dict[str, float] = {}
        if self.directory.exists():
            for p in self.directory.glob("*.y*ml"):
                try:
                    state[str(p)] = p.stat().st_mtime
                except Exception:
                    pass
        self._last_state = state
        return state

    def _loop(self) -> None:
        log = logging.getLogger(__name__)
        while not self._stop.is_set():
            try:
                time.sleep(self.interval_seconds)
                current = self._snapshot()
                if current != self._last_state:
                    self.on_change()
            except Exception as exc:
                log.debug("watcher error: %s", exc)

