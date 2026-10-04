"""paired_remotes.json has ONE writer discipline: devices.py's lock + its
fsync'd unique-tempfile _save_atomic.

Three writers used to read-modify-write it independently — pairing
(devices.add_device), the nickname page (admin.remote_name) and the kiosk's
revocation queue (auth.reap_revocations, on the status-broadcaster thread).
Atomic renames stop torn files but not LOST UPDATES: a nickname save that
read the file before a pairing committed wrote the new phone back out of
existence. reap_revocations also used a fixed ".tmp" name with no fsync —
the very pattern _save_atomic exists to replace (this file is the
phone-remote trust store; a power cut that zeroes it unpairs every phone).
"""
from __future__ import annotations

import json
import threading
import time
from pathlib import Path

from remote import auth, devices


def _slow_load(monkeypatch, delay=0.02):
    real = devices._load

    def slow(path):
        data = real(path)
        time.sleep(delay)  # widen the read->write window
        return data

    monkeypatch.setattr(devices, "_load", slow)


def test_concurrent_writers_lose_no_updates(tmp_path, monkeypatch):
    path = tmp_path / "paired_remotes.json"
    first = devices.add_device(path, "first")
    _slow_load(monkeypatch)

    threads = [threading.Thread(target=devices.add_device, args=(path, f"p{i}"))
               for i in range(6)]
    threads.append(threading.Thread(target=devices.rename_device,
                                    args=(path, first, "Renamed")))
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    data = json.loads(path.read_text())
    assert len(data["devices"]) == 7
    assert {d["nickname"] for d in data["devices"]} >= {"Renamed", "p0", "p5"}


def test_reap_revocations_goes_through_the_locked_atomic_saver(tmp_path, monkeypatch):
    path = tmp_path / "paired_remotes.json"
    keep = devices.add_device(path, "keep")
    gone = devices.add_device(path, "gone")
    (tmp_path / "pending_revocations.txt").write_text(gone + "\n")

    saves = []
    real_save = devices._save_atomic

    def spy(p, data):
        saves.append(p)
        return real_save(p, data)

    monkeypatch.setattr(devices, "_save_atomic", spy)
    assert auth.reap_revocations(tmp_path) == 1
    assert saves == [path]
    assert [d["id"] for d in json.loads(path.read_text())["devices"]] == [keep]
    assert not (tmp_path / "pending_revocations.txt").exists()
    assert not (tmp_path / "paired_remotes.json.tmp").exists()


def test_reap_leaves_queue_when_trust_store_is_malformed(tmp_path):
    path = tmp_path / "paired_remotes.json"
    path.write_text("{not json")
    q = tmp_path / "pending_revocations.txt"
    q.write_text("abc\n")
    assert auth.reap_revocations(tmp_path) == 0
    assert q.exists(), "a revocation must never be silently dropped"
    assert path.read_text() == "{not json", "and the store must not be rewritten"


def test_reap_racing_a_pairing_keeps_the_new_phone(tmp_path, monkeypatch):
    path = tmp_path / "paired_remotes.json"
    gone = devices.add_device(path, "gone")
    (tmp_path / "pending_revocations.txt").write_text(gone + "\n")
    _slow_load(monkeypatch, delay=0.05)

    added = []
    t1 = threading.Thread(target=lambda: added.append(devices.add_device(path, "new")))
    t2 = threading.Thread(target=auth.reap_revocations, args=(tmp_path,))
    t2.start()
    time.sleep(0.01)
    t1.start()
    t1.join()
    t2.join()
    ids = [d["id"] for d in json.loads(path.read_text())["devices"]]
    assert ids == added
