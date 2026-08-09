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


# ---------------------------------------------------------------------------
# Durable install tokens (iOS home-screen install pairing).
#
# An installed home-screen web app gets a SEPARATE cookie jar from Safari,
# so the pairing cookie Safari holds never reaches the installed app. The
# bridge: the dynamic manifest (served only to a validly-paired Safari
# session) embeds a per-device token in start_url; the installed app
# presents it on first launch and trades it for its own mdb_remote cookie.
#
# The token is DERIVED, not stored: HMAC(flask_secret, device_id|salt),
# where the salt is a random per-device value in paired_remotes.json.
# Consequences, each load-bearing:
#   - Nothing usable as a bearer credential sits at rest (the salt alone
#     is useless without flask_secret.key) — stronger than "hashed at
#     rest", and paired_remotes.json in isolation discloses nothing.
#   - The SAME plaintext is reproducible on every manifest fetch. iOS may
#     fetch the manifest again between install and first launch; a
#     rotate-on-fetch scheme would strand the installed icon with a dead
#     token — the exact failure this feature exists to remove.
#   - Revocation needs no extra bookkeeping: redemption resolves against
#     live records only, so reap_revocations / revoke_device killing the
#     record kills the token, and first_boot.sh wiping paired_remotes.json
#     + flask_secret.key on clones kills every token with the pairings.
#
# NEVER log or return token values in errors. Prove presence by length
# or count only.
# ---------------------------------------------------------------------------

def _derive_device_token(device_id: str, salt: str) -> str:
    secret = current_app.config["SECRET_KEY"].encode("utf-8")
    msg = f"device_token|{device_id}|{salt}".encode("utf-8")
    return hmac.new(secret, msg, hashlib.sha256).hexdigest()


def device_token_for(device_id: str) -> Optional[str]:
    """The durable install token for a paired device (lazily minting the
    salt for records paired before this feature). None if no such device."""
    salt = devices_mod.ensure_token_salt(_devices_path(), device_id)
    if salt is None:
        return None
    return _derive_device_token(device_id, salt)


def redeem_device_token(submitted: str) -> Optional[str]:
    """Return the device_id whose install token matches, else None.

    Only the GET /admin/remote redeem path may call this — the token
    authenticates exactly one action (re-issuing the pairing cookie to
    the installed app's jar), never any other endpoint.
    """
    if not submitted or len(submitted) > 256:
        return None
    # Drain any kiosk-issued revocations first so a just-revoked device
    # cannot redeem inside the broadcaster's 200 ms reap window.
    try:
        reap_revocations(_data_dir())
    except Exception:
        pass
    matched: Optional[str] = None
    for d in devices_mod.list_devices(_devices_path()):
        device_id = d.get("id")
        salt = d.get("token_salt")
        if not device_id or not salt:
            continue
        expected = _derive_device_token(device_id, salt)
        # Constant-time per candidate; keep scanning all records rather
        # than early-exiting so match position is not observable.
        if hmac.compare_digest(expected, submitted):
            matched = device_id
    ip = request.remote_addr or "?"
    # Audit outcome only — the empty code_attempt masks to "****" so no
    # fragment of the token ever reaches pairing_audit.log.
    _audit("token_redeemed" if matched else "token_rejected", "", ip)
    return matched


# Cap on pairing_audit.log. Nothing ever pruned it, so on an appliance
# with years of uptime (or a hostile LAN device hammering /pair) it grew
# without bound on the SD card. 512 KB holds ~5000 recent entries — far
# more than any pairing investigation needs; the diagnostic value of this
# log is "did the phone's request ARRIVE at all" (see CLAUDE.md's pairing
# notes), which the recent tail answers.
_AUDIT_MAX_BYTES = 512 * 1024
_AUDIT_KEEP_BYTES = 256 * 1024


def _audit(outcome: str, code_attempt: str, ip: str) -> None:
    line = json.dumps({
        "ts": int(time.time()),
        "ip": ip,
        "outcome": outcome,
        "code": (code_attempt[:2] + "****") if len(code_attempt) >= 2 else "****",
    })
    path = _audit_path()
    try:
        if path.exists() and path.stat().st_size > _AUDIT_MAX_BYTES:
            # Keep the newest chunk, aligned to a line boundary. Plain
            # truncate-rewrite (not rename) so an open tail keeps working;
            # audit lines are diagnostics, so a lost line during the
            # rewrite window is acceptable where unbounded growth is not.
            tail = path.read_bytes()[-_AUDIT_KEEP_BYTES:]
            nl = tail.find(b"\n")
            if nl >= 0:
                tail = tail[nl + 1:]
            path.write_bytes(tail)
    except OSError:
        pass  # rotation is best-effort; the append below still runs
    with path.open("a") as f:
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
    # NB: Do NOT unlink the queue file yet. If the process dies between
    # unlink and os.replace below, the revocation would be permanently lost
    # (the kiosk only writes the queue once per UI dismiss). Unlink AFTER
    # the paired_remotes.json save commits.
    ids = {ln.strip() for ln in text.splitlines() if ln.strip()}
    if not ids:
        rev_path.unlink(missing_ok=True)
        return 0
    paired = Path(data_dir) / "paired_remotes.json"
    if not paired.exists():
        rev_path.unlink(missing_ok=True)
        return 0
    try:
        data = json.loads(paired.read_text())
    except (OSError, json.JSONDecodeError):
        # Malformed paired_remotes.json — leave the queue in place so the
        # operator can investigate; don't silently drop the revocation.
        return 0
    before = len(data.get("devices", []))
    data["devices"] = [d for d in data.get("devices", []) if d.get("id") not in ids]
    removed = before - len(data["devices"])
    if removed > 0:
        tmp = paired.parent / (paired.name + ".tmp")
        tmp.write_text(json.dumps(data, indent=2))
        os.replace(tmp, paired)
    # Save committed (or no-op if no matches) — now safe to consume the queue.
    rev_path.unlink(missing_ok=True)
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
        abort(410, "Code expired. Open Settings → Connect a Device on the kiosk.")

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
