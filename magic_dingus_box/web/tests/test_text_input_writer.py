"""Unit tests for TextInputWriter — appends JSONL events to a queue file
under flock, with a per-process monotonic seq counter and a per-connection
rate limit."""
from __future__ import annotations

import json
import threading
import time
from pathlib import Path

import pytest

from remote.text_input_writer import TextInputWriter


@pytest.fixture
def writer(tmp_path: Path) -> TextInputWriter:
    return TextInputWriter(queue_path=tmp_path / "queue.jsonl")


def _read_lines(path: Path) -> list[dict]:
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        if line.strip():
            out.append(json.loads(line))
    return out


def test_type_char_appends_one_event(writer, tmp_path):
    writer.type_char("a", device_id="dev1")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert len(events) == 1
    assert events[0]["t"] == "type_char"
    assert events[0]["c"] == "a"
    assert events[0]["device"] == "dev1"
    assert events[0]["seq"] >= 1
    assert events[0]["ts"] > 0


def test_seq_is_monotonic(writer, tmp_path):
    writer.type_char("a", device_id="d")
    writer.type_char("b", device_id="d")
    writer.key_special("backspace", device_id="d")
    events = _read_lines(tmp_path / "queue.jsonl")
    seqs = [e["seq"] for e in events]
    assert seqs == sorted(seqs)
    assert len(set(seqs)) == 3  # all distinct


def test_key_special_writes_correct_shape(writer, tmp_path):
    writer.key_special("enter", device_id="d")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert events[0]["t"] == "key_special"
    assert events[0]["k"] == "enter"


def test_clear_writes_correct_shape(writer, tmp_path):
    writer.clear(device_id="d")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert events[0]["t"] == "clear"


def test_concurrent_writes_no_torn_lines(tmp_path):
    """Two threads writing 50 events each should produce 100 well-formed
    JSON lines with no interleaved bytes (flock works)."""
    path = tmp_path / "queue.jsonl"
    w = TextInputWriter(queue_path=path)

    def hammer(tag: str):
        for i in range(50):
            w.type_char(chr(97 + (i % 26)), device_id=tag)

    t1 = threading.Thread(target=hammer, args=("A",))
    t2 = threading.Thread(target=hammer, args=("B",))
    t1.start(); t2.start()
    t1.join(); t2.join()

    events = _read_lines(path)
    assert len(events) == 100
    # Every line parsed cleanly (no torn writes).


def test_rate_limit_drops_excess(writer, tmp_path):
    """50 events/sec/device. 60 fired in <1s — last 10 should drop silently."""
    for _ in range(60):
        writer.type_char("x", device_id="dev1")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert len(events) == 50  # capped


def test_rate_limit_per_device(writer, tmp_path):
    """Per-device buckets — dev1 hitting limit doesn't affect dev2."""
    for _ in range(60):
        writer.type_char("a", device_id="dev1")
    for _ in range(10):
        writer.type_char("b", device_id="dev2")
    events = _read_lines(tmp_path / "queue.jsonl")
    dev1 = [e for e in events if e["device"] == "dev1"]
    dev2 = [e for e in events if e["device"] == "dev2"]
    assert len(dev1) == 50
    assert len(dev2) == 10


def test_rate_limit_window_resets(tmp_path):
    """After the 1s window, full quota available again."""
    w = TextInputWriter(queue_path=tmp_path / "queue.jsonl")
    for _ in range(50):
        w.type_char("x", device_id="d")
    time.sleep(1.05)
    w.type_char("y", device_id="d")
    events = _read_lines(tmp_path / "queue.jsonl")
    assert len(events) == 51
