"""Full pair → cookie → press flow with fakes.
This test exercises the integration between auth (pair endpoint), the
HMAC-cookie issue/verify, and the UinputWriter — without requiring a real
/dev/uinput or a kiosk. It complements the per-module tests by proving the
pieces fit together."""
import json
import time
from pathlib import Path

import pytest

from admin import create_app
from remote.uinput_writer import UinputWriter, EV_KEY, BTN_SOUTH


class FakeDev:
    def __init__(self):
        self.events = []

    def write(self, t, c, v):
        self.events.append((t, c, v))

    def syn(self):
        self.events.append(("SYN",))


def _seed_session(data_dir: Path, code="111111"):
    (data_dir / "pairing_session.json").write_text(json.dumps({
        "schema": 1, "code": code,
        "issued_at": int(time.time()), "expires_at": int(time.time()) + 60,
        "attempts_remaining": 5, "nonce": "x" * 32,
    }))


def test_full_pair_then_press_via_debug_endpoint(tmp_path):
    """End-to-end: kiosk writes session → phone visits /?pair=CODE → cookie
    issued → app's uinput writer drives a real fake gamepad."""
    _seed_session(tmp_path, code="111111")

    app = create_app(data_dir=tmp_path)
    app.config["SECRET_KEY"] = "test-key"
    fake = FakeDev()
    app.config["UINPUT_WRITER"] = UinputWriter(device=fake)

    client = app.test_client()

    # 1. Pair
    rv = client.get("/?pair=111111&tab=remote", follow_redirects=False)
    assert rv.status_code in (302, 303)
    cookie = rv.headers.get("Set-Cookie", "")
    assert "mdb_remote=" in cookie

    # 2. Verify the device is registered
    paired = json.loads((tmp_path / "paired_remotes.json").read_text())
    assert len(paired["devices"]) == 1

    # 3. Drive the writer (the WS path is exercised by test_ws_protocol;
    # here we go through the debug HTTP endpoint to keep this test simple).
    rv2 = client.post("/admin/remote/_debug/press?btn=OK&phase=tap")
    assert rv2.status_code == 200

    # 4. Verify uinput received the press
    keydowns = [e for e in fake.events if e[:2] == (EV_KEY, BTN_SOUTH) and e[2] == 1]
    keyups   = [e for e in fake.events if e[:2] == (EV_KEY, BTN_SOUTH) and e[2] == 0]
    assert keydowns, "no keydown emitted to uinput"
    assert keyups, "no keyup emitted to uinput"


def test_revoke_then_reconnect_is_unauthorized(tmp_path):
    """If the kiosk forgets a device, a subsequent protected request fails.
    This is the contract the phone JS relies on to detect and re-pair."""
    from remote.auth import reap_revocations

    _seed_session(tmp_path, code="222222")

    app = create_app(data_dir=tmp_path)
    app.config["SECRET_KEY"] = "test-key"
    client = app.test_client()

    # Pair
    rv = client.get("/?pair=222222", follow_redirects=False)
    cookie_header = rv.headers["Set-Cookie"].split(";")[0]
    name, value = cookie_header.split("=", 1)
    client.set_cookie(domain="localhost", key=name, value=value)

    # Find device id
    paired = json.loads((tmp_path / "paired_remotes.json").read_text())
    device_id = paired["devices"][0]["id"]

    # Sanity: protected check works before revocation
    rv2 = client.get("/admin/remote/protected_check")
    assert rv2.status_code == 200

    # Kiosk writes a revocation
    (tmp_path / "pending_revocations.txt").write_text(device_id + "\n")
    removed = reap_revocations(tmp_path)
    assert removed == 1

    # Now the same cookie should be rejected
    rv3 = client.get("/admin/remote/protected_check")
    assert rv3.status_code == 401
