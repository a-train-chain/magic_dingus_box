"""Read/write paired_remotes.json (the device list)."""
from __future__ import annotations

import json
import os
import tempfile
import time
import uuid
from pathlib import Path
from typing import Optional


def _load(path: Path) -> dict:
    if not path.exists():
        return {"schema": 1, "devices": []}
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError:
        return {"schema": 1, "devices": []}


def _save_atomic(path: Path, data: dict) -> None:
    # Unique staging file per write (mkstemp), fsync'd before the rename.
    # The old fixed "<name>.tmp" was shared by every writer — pairing,
    # last-seen updates, and revocation reaping run on different threads,
    # and two concurrent saves could promote each other's half-written
    # staging file (losing a freshly paired device) or crash on a
    # FileNotFoundError when the other writer renamed first. fsync
    # matters here too: this file is the phone-remote trust store, and a
    # power cut that zeroes it silently unpairs every phone.
    fd, tmp_name = tempfile.mkstemp(
        dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as fh:
            fh.write(json.dumps(data, indent=2))
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp_name, 0o644)
        os.replace(tmp_name, path)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise


def add_device(path: Path, nickname: str, user_agent_hint: str = "") -> str:
    data = _load(path)
    device_id = uuid.uuid4().hex
    data["devices"].append({
        "id": device_id,
        "nickname": nickname or "Phone",
        "user_agent_hint": user_agent_hint,
        "paired_at": int(time.time()),
        "last_seen": int(time.time()),
    })
    _save_atomic(path, data)
    return device_id


def find_device(path: Path, device_id: str) -> Optional[dict]:
    data = _load(path)
    for d in data["devices"]:
        if d["id"] == device_id:
            return d
    return None


def touch_last_seen(path: Path, device_id: str) -> None:
    data = _load(path)
    for d in data["devices"]:
        if d["id"] == device_id:
            d["last_seen"] = int(time.time())
            break
    _save_atomic(path, data)


def revoke_device(path: Path, device_id: str) -> bool:
    data = _load(path)
    before = len(data["devices"])
    data["devices"] = [d for d in data["devices"] if d["id"] != device_id]
    if len(data["devices"]) != before:
        _save_atomic(path, data)
        return True
    return False
