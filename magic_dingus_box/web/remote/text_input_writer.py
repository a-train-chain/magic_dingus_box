"""Appends Phone Remote text-input events to a JSON-Lines queue file
that the kiosk's main loop drains each frame.

Mirrors the role of UinputWriter (button presses → evdev events) for
text input: each phone keystroke arrives as a WS message and is
serialized into one JSONL line under flock. Concurrent connections
from multiple phones are safe — flock(LOCK_EX) serializes the writes.

Per-device rate limit (50 events/sec) prevents a malicious or
misbehaving paired phone from filling the queue file."""
from __future__ import annotations

import fcntl
import json
import threading
import time
from pathlib import Path
from typing import Dict


# Per-device rate limit. Sustained human typing tops out at ~5-10
# char/sec; 50 is a comfortable ceiling that catches paste flurries
# without leaving room for abuse.
_MAX_EVENTS_PER_SECOND = 50
_RATE_WINDOW_S = 1.0


class _Bucket:
    """Token bucket per device. Reset on the next event after the window."""
    __slots__ = ("count", "window_start")

    def __init__(self) -> None:
        self.count = 0
        self.window_start = 0.0


class TextInputWriter:
    def __init__(self, queue_path: Path) -> None:
        self._path = Path(queue_path)
        # Ensure parent dir exists; touch the file so flock has a target.
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._path.touch(exist_ok=True)
        self._seq = 0
        self._seq_lock = threading.Lock()
        self._buckets: Dict[str, _Bucket] = {}
        self._buckets_lock = threading.Lock()

    # ── public API ────────────────────────────────────────────────────

    def type_char(self, c: str, *, device_id: str) -> None:
        """Append a `type_char` event. `c` must be a single ASCII
        printable character; non-conforming inputs are dropped silently
        (the WS handler is the primary filter; this is defense in depth).
        """
        if len(c) != 1 or not c.isprintable() or ord(c) >= 0x80:
            return
        self._append({"t": "type_char", "c": c}, device_id)

    def key_special(self, k: str, *, device_id: str) -> None:
        """Append a `key_special` event. `k` must be one of
        ('backspace', 'enter', 'cancel'); others dropped."""
        if k not in ("backspace", "enter", "cancel"):
            return
        self._append({"t": "key_special", "k": k}, device_id)

    def clear(self, *, device_id: str) -> None:
        """Append a `clear` event."""
        self._append({"t": "clear"}, device_id)

    # ── internals ─────────────────────────────────────────────────────

    def _next_seq(self) -> int:
        with self._seq_lock:
            self._seq += 1
            return self._seq

    def _check_rate(self, device_id: str) -> bool:
        """Return True if event allowed; False if it should be dropped."""
        now = time.time()
        with self._buckets_lock:
            b = self._buckets.setdefault(device_id, _Bucket())
            if now - b.window_start >= _RATE_WINDOW_S:
                b.window_start = now
                b.count = 0
            if b.count >= _MAX_EVENTS_PER_SECOND:
                return False
            b.count += 1
            return True

    def _append(self, event: dict, device_id: str) -> None:
        if not self._check_rate(device_id):
            return
        event["seq"]    = self._next_seq()
        event["ts"]     = time.time()
        event["device"] = device_id
        line = json.dumps(event) + "\n"
        with open(self._path, "ab") as f:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX)
            try:
                f.write(line.encode("utf-8"))
            finally:
                fcntl.flock(f.fileno(), fcntl.LOCK_UN)
