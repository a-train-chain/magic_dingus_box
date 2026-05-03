"""Watches kiosk_status.json mtime; fans out parsed snapshots to all queues.

The C++ kiosk writes the file atomically every 200 ms (5 Hz). This thread
polls the mtime at the same cadence. If mtime changed, it parses and sends
{"t": "status", "data": <snapshot>} to every connected phone's queue.
mtime-only check skips JSON parsing on idle ticks (cheap)."""
from __future__ import annotations

import json
import threading
import time
from pathlib import Path
from queue import Queue, Full
from typing import Iterable, List

from . import auth as _auth


class StatusBroadcaster:
    def __init__(self, path: Path, queues: Iterable[Queue], interval_s: float = 0.2):
        self._path = Path(path)
        self._queues: List[Queue] = list(queues)
        self._interval = interval_s
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_mtime = 0.0
        self._lock = threading.Lock()

    def add_queue(self, q: Queue) -> None:
        with self._lock:
            self._queues.append(q)

    def remove_queue(self, q: Queue) -> None:
        with self._lock:
            try:
                self._queues.remove(q)
            except ValueError:
                pass

    def start(self) -> None:
        if self._thread is not None:
            return  # idempotent
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name="StatusBroadcaster")
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)

    def _run(self) -> None:
        while not self._stop.wait(self._interval):
            # Drain any kiosk-issued revocations before broadcasting status.
            try:
                _auth.reap_revocations(self._path.parent)
            except Exception:
                pass

            try:
                mtime = self._path.stat().st_mtime
            except FileNotFoundError:
                continue
            if mtime <= self._last_mtime:
                continue
            self._last_mtime = mtime
            try:
                data = json.loads(self._path.read_text())
            except (json.JSONDecodeError, OSError):
                continue
            msg = {"t": "status", "data": data}
            with self._lock:
                snapshot = list(self._queues)
            for q in snapshot:
                try:
                    q.put_nowait(msg)
                except Full:
                    # Slow client — drop the frame rather than block the broadcaster.
                    pass
