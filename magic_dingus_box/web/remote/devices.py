"""Read/write paired_remotes.json (the device list)."""
from __future__ import annotations

import json
import os
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
    # Use path.name + ".tmp" rather than .with_suffix(".tmp") to avoid
    # replacing the real suffix: paired_remotes.json → paired_remotes.json.tmp
    tmp = path.parent / (path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(tmp, path)


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
