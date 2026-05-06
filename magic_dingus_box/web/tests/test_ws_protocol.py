"""Tests for the Phone Remote _debug/press HTTP endpoint and WS endpoint.
Covers auth gating added in the pre-deploy review."""
import json
import time
import hmac as _hmac
import hashlib
import pytest
from admin import create_app
from remote.uinput_writer import UinputWriter, EV_KEY, BTN_SOUTH
from remote import auth as remote_auth


def _make_paired_cookie(tmp_path, secret: str = "test-key") -> str:
    """Seed a paired device and return a valid mdb_remote cookie value."""
    paired = tmp_path / "paired_remotes.json"
    device_id = "test-device-abc"
    paired.write_text(json.dumps({
        "schema": 1,
        "devices": [{"id": device_id, "nickname": "Test", "paired_at": 1, "last_seen": 1}],
    }))
    issued_at = int(time.time())
    sig = _hmac.new(secret.encode(), f"{device_id}|{issued_at}".encode(), hashlib.sha256).hexdigest()
    return f"{device_id}.{issued_at}.{sig}"


@pytest.fixture
def app(tmp_path):
    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    app.config["SECRET_KEY"] = "test-key"
    return app


def test_press_via_debug_endpoint_writes_to_uinput(app, tmp_path):
    captured = []
    class FakeDev:
        def write(self, t, c, v): captured.append((t, c, v))
        def syn(self): captured.append(("SYN",))

    fake_writer = UinputWriter(device=FakeDev())
    app.config["UINPUT_WRITER"] = fake_writer

    cookie_value = _make_paired_cookie(tmp_path)
    client = app.test_client()
    client.set_cookie(domain="localhost", key="mdb_remote", value=cookie_value)
    rv = client.post("/admin/remote/_debug/press?btn=OK&phase=tap")
    assert rv.status_code == 200
    # OK = BTN_SOUTH; tap = down then up
    assert (EV_KEY, BTN_SOUTH, 1) in captured
    assert (EV_KEY, BTN_SOUTH, 0) in captured


def test_press_with_unknown_button_returns_error(app, tmp_path):
    fake_writer = UinputWriter(device=type("F", (), {"write": lambda *a, **k: None, "syn": lambda *a, **k: None})())
    app.config["UINPUT_WRITER"] = fake_writer

    cookie_value = _make_paired_cookie(tmp_path)
    client = app.test_client()
    client.set_cookie(domain="localhost", key="mdb_remote", value=cookie_value)
    rv = client.post("/admin/remote/_debug/press?btn=BOGUS&phase=tap")
    assert rv.status_code == 400


def test_unauthenticated_debug_press_returns_401(app):
    """No cookie → 401; verifies the auth gate added in pre-deploy review."""
    client = app.test_client()
    rv = client.post("/admin/remote/_debug/press?btn=OK&phase=tap")
    assert rv.status_code == 401
