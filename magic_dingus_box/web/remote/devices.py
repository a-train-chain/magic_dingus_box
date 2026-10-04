"""Read/write paired_remotes.json (the device list)."""
from __future__ import annotations

import json
import os
import secrets
import tempfile
import threading
import time
import uuid
from pathlib import Path
from typing import Iterable, Optional

# Every read-modify-write of paired_remotes.json holds this lock. Atomic
# renames (below) prevent TORN files, not LOST UPDATES: pairing, the nickname
# page, last-seen touches and the kiosk's revocation reaper run on different
# threads, and a writer that read the file before another committed would
# write that other change back out of existence — e.g. a nickname save
# silently unpairing the phone paired a moment earlier. All writers go
# through this module (admin.remote_name and auth.reap_revocations included).
# RLock: ensure_token_salt etc. may be reached from inside another locked op.
# In-process only; the kiosk never writes this file (it queues revocations
# in pending_revocations.txt instead).
_lock = threading.RLock()


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
    with _lock:
        data = _load(path)
        device_id = uuid.uuid4().hex
        data["devices"].append({
            "id": device_id,
            "nickname": nickname or "Phone",
            "user_agent_hint": user_agent_hint,
            "paired_at": int(time.time()),
            "last_seen": int(time.time()),
            # Seed for the durable install token (see auth.device_token_for).
            # The bearer token itself is HMAC-derived from this + the Flask
            # secret at request time, so nothing usable-as-a-credential sits
            # in this file. Re-pairing creates a new record → new salt → new
            # token; revocation deletes the record → the token dies with it.
            "token_salt": secrets.token_urlsafe(32),
        })
        _save_atomic(path, data)
        return device_id


def rename_device(path: Path, device_id: str, nickname: str) -> bool:
    """Set a device's nickname. False if no such device (nothing written)."""
    with _lock:
        data = _load(path)
        for d in data.get("devices", []):
            if d.get("id") == device_id:
                d["nickname"] = nickname
                _save_atomic(path, data)
                return True
        return False


def revoke_many(path: Path, device_ids: Iterable[str]) -> int:
    """Remove every listed device; returns how many were removed.

    Unlike the other writers this refuses to treat a malformed file as
    empty: raises ValueError instead, so the caller (the revocation reaper)
    keeps its queue for an operator rather than rewriting the trust store.
    """
    ids = set(device_ids)
    with _lock:
        if not path.exists():
            return 0
        try:
            data = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as e:
            raise ValueError(f"unreadable {path.name}: {e}") from e
        if not isinstance(data, dict) or not isinstance(data.get("devices", []), list):
            raise ValueError(f"malformed {path.name}")
        devs = data.get("devices", [])
        kept = [d for d in devs if not (isinstance(d, dict) and d.get("id") in ids)]
        removed = len(devs) - len(kept)
        if removed:
            data["devices"] = kept
            _save_atomic(path, data)
        return removed


def list_devices(path: Path) -> list:
    """All device records (read-only snapshot)."""
    return list(_load(path).get("devices", []))


def ensure_token_salt(path: Path, device_id: str) -> Optional[str]:
    """Return the device's token salt, lazily minting one for records
    paired before durable install tokens existed. None if no such device."""
    with _lock:
        data = _load(path)
        for d in data.get("devices", []):
            if d.get("id") == device_id:
                salt = d.get("token_salt")
                if not salt:
                    salt = secrets.token_urlsafe(32)
                    d["token_salt"] = salt
                    _save_atomic(path, data)
                return salt
        return None


def find_device(path: Path, device_id: str) -> Optional[dict]:
    data = _load(path)
    for d in data["devices"]:
        if d["id"] == device_id:
            return d
    return None


def touch_last_seen(path: Path, device_id: str) -> None:
    with _lock:
        data = _load(path)
        for d in data["devices"]:
            if d["id"] == device_id:
                d["last_seen"] = int(time.time())
                break
        _save_atomic(path, data)


def revoke_device(path: Path, device_id: str) -> bool:
    with _lock:
        data = _load(path)
        before = len(data["devices"])
        data["devices"] = [d for d in data["devices"] if d["id"] != device_id]
        if len(data["devices"]) != before:
            _save_atomic(path, data)
            return True
        return False
