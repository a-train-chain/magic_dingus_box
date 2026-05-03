"""Verifies the broadcaster fans out status changes to all queues, and
suppresses re-sends when mtime hasn't changed."""
import json
import time
from pathlib import Path
from queue import Queue

import pytest

from remote.status_broadcaster import StatusBroadcaster


def write_status(path: Path, screen="playlist"):
    path.write_text(json.dumps({"schema": 1, "ts": time.time(), "screen": screen}))


def _drain(q: Queue):
    """Pop all items from a queue (non-blocking)."""
    out = []
    while True:
        try:
            out.append(q.get_nowait())
        except Exception:
            break
    return out


def test_broadcaster_fans_out_to_all_queues(tmp_path):
    p = tmp_path / "kiosk_status.json"
    write_status(p, screen="playlist")
    q1, q2 = Queue(), Queue()
    b = StatusBroadcaster(p, [q1, q2], interval_s=0.05)
    b.start()
    time.sleep(0.15)  # let initial broadcast happen
    write_status(p, screen="playback")
    time.sleep(0.25)  # let mtime change broadcast
    b.stop()
    msgs1 = _drain(q1)
    msgs2 = _drain(q2)
    assert any(m["data"]["screen"] == "playback" for m in msgs1)
    assert any(m["data"]["screen"] == "playback" for m in msgs2)


def test_broadcaster_suppresses_unchanged_mtime(tmp_path):
    p = tmp_path / "kiosk_status.json"
    write_status(p)
    q = Queue()
    b = StatusBroadcaster(p, [q], interval_s=0.05)
    b.start()
    time.sleep(0.4)  # multiple ticks, file unchanged
    b.stop()
    msgs = _drain(q)
    # Initial read produces 1 message; subsequent ticks should not re-send.
    assert len(msgs) == 1
