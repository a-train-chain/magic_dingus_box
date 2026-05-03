"""Pairing endpoint logic + HMAC cookie issue/verify."""
from __future__ import annotations

import hmac
import hashlib
import json
import os
import time
from pathlib import Path
from typing import Optional

from flask import current_app, request, redirect, abort

from . import devices as devices_mod

COOKIE_NAME = "mdb_remote"
COOKIE_MAX_AGE = 60 * 60 * 24 * 365  # 1 year


def _data_dir() -> Path:
    return Path(current_app.config["DATA_DIR"])


def _session_path() -> Path:
    return _data_dir() / "pairing_session.json"


def _devices_path() -> Path:
    return _data_dir() / "paired_remotes.json"


def _audit_path() -> Path:
    return _data_dir() / "pairing_audit.log"


def _hmac(device_id: str, issued_at: int) -> str:
    secret = current_app.config["SECRET_KEY"].encode("utf-8")
    msg = f"{device_id}|{issued_at}".encode("utf-8")
    return hmac.new(secret, msg, hashlib.sha256).hexdigest()


def issue_cookie(response, device_id: str) -> None:
    issued_at = int(time.time())
    sig = _hmac(device_id, issued_at)
    value = f"{device_id}.{issued_at}.{sig}"
    response.set_cookie(
        COOKIE_NAME, value,
        max_age=COOKIE_MAX_AGE, httponly=True, samesite="Strict", path="/",
    )


def verify_cookie(cookie_value: str) -> Optional[str]:
    """Returns the device_id if valid AND not revoked, else None."""
    if not cookie_value:
        return None
    parts = cookie_value.split(".")
    if len(parts) != 3:
        return None
    device_id, issued_str, sig = parts
    try:
        issued_at = int(issued_str)
    except ValueError:
        return None
    expected = _hmac(device_id, issued_at)
    if not hmac.compare_digest(expected, sig):
        return None
    if devices_mod.find_device(_devices_path(), device_id) is None:
        return None
    return device_id


def _audit(outcome: str, code_attempt: str, ip: str) -> None:
    line = json.dumps({
        "ts": int(time.time()),
        "ip": ip,
        "outcome": outcome,
        "code": (code_attempt[:2] + "****") if len(code_attempt) >= 2 else "****",
    })
    with _audit_path().open("a") as f:
        f.write(line + "\n")


def reap_revocations(data_dir: Path) -> int:
    """Drain pending_revocations.txt, remove matching devices from
    paired_remotes.json. Returns the number of devices revoked.
    Safe to call repeatedly — it's a no-op when the file is missing."""
    rev_path = Path(data_dir) / "pending_revocations.txt"
    if not rev_path.exists():
        return 0
    try:
        text = rev_path.read_text()
    except OSError:
        return 0
    rev_path.unlink(missing_ok=True)
    ids = {ln.strip() for ln in text.splitlines() if ln.strip()}
    if not ids:
        return 0
    paired = Path(data_dir) / "paired_remotes.json"
    if not paired.exists():
        return 0
    try:
        data = json.loads(paired.read_text())
    except (OSError, json.JSONDecodeError):
        return 0
    before = len(data.get("devices", []))
    data["devices"] = [d for d in data.get("devices", []) if d.get("id") not in ids]
    removed = before - len(data["devices"])
    if removed > 0:
        tmp = paired.parent / (paired.name + ".tmp")
        tmp.write_text(json.dumps(data, indent=2))
        os.replace(tmp, paired)
    return removed


def handle_pair_param(submitted_code: str):
    """Called from the admin index handler when ?pair= is present.
    Returns a Flask response, or None to indicate 'not pairing — pass through'."""
    session_path = _session_path()
    ip = request.remote_addr or "?"

    if not session_path.exists():
        _audit("no_session", submitted_code, ip)
        abort(410, "Pairing screen not open on the kiosk.")

    try:
        session = json.loads(session_path.read_text())
    except json.JSONDecodeError:
        _audit("session_corrupt", submitted_code, ip)
        abort(410)

    if int(time.time()) > session["expires_at"]:
        _audit("expired", submitted_code, ip)
        session_path.unlink(missing_ok=True)
        abort(410, "Code expired. Open Settings → Phone Remote on the kiosk.")

    if not hmac.compare_digest(session["code"], submitted_code):
        # Decrement attempts atomically; delete if 0.
        session["attempts_remaining"] -= 1
        if session["attempts_remaining"] <= 0:
            session_path.unlink(missing_ok=True)
            _audit("locked_out", submitted_code, ip)
        else:
            tmp = session_path.parent / (session_path.name + ".tmp")
            tmp.write_text(json.dumps(session))
            os.replace(tmp, session_path)
            _audit("wrong_code", submitted_code, ip)
        abort(401, "Wrong code.")

    # Success — issue cookie, register device, consume session.
    user_agent = request.headers.get("User-Agent", "")
    nickname = "Phone"  # Replaced by Task C.6 nickname-prompt.
    device_id = devices_mod.add_device(_devices_path(), nickname, user_agent)
    session_path.unlink(missing_ok=True)
    _audit("paired", submitted_code, ip)

    # Redirect to a nickname-prompt page; the cookie is set so the prompt
    # can authenticate the device. After the user names it, that page
    # redirects back to /?tab=<target>.
    target = request.args.get("tab", "remote")
    resp = redirect(f"/admin/remote/name?d={device_id}&tab={target}", code=303)
    issue_cookie(resp, device_id)
    return resp
