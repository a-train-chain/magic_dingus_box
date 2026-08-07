"""The /connect landing page — the unified kiosk QR target.

Both kiosk QR codes (the Content Manager Info screen and the phone-remote
pairing screen) land on /connect, which offers the two destinations in
words. These tests pin the page's ONLY contract: where its two buttons
point, as a function of the optional ?code= parameter. The pairing
mechanics themselves (handle_pair_param, HMAC cookies, the in-app 6-digit
form) are covered by test_pairing.py / test_remote_e2e.py and must be
unaffected by this page — the last test proves the QR-scan pairing path
still works end-to-end when entered via the /connect button target.
"""
import json
import time
from pathlib import Path

from admin import create_app


def _client(tmp_path: Path):
    app = create_app(data_dir=tmp_path)
    app.config["SECRET_KEY"] = "test-key"
    return app.test_client()


def test_connect_without_code_points_remote_button_at_admin_remote(tmp_path):
    """No ?code= → the remote button goes to /admin/remote (which shows
    the in-app 6-digit form when unpaired)."""
    rv = _client(tmp_path).get("/connect")
    assert rv.status_code == 200
    body = rv.get_data(as_text=True)
    assert 'href="/admin/remote"' in body
    assert 'href="/"' in body  # Content Manager button always present
    assert "pair=" not in body


def test_connect_with_code_points_remote_button_at_pair_flow(tmp_path):
    """A well-formed ?code= is forwarded into the EXISTING pairing flow on
    the root route — /?pair=CODE&tab=remote, where handle_pair_param lives."""
    rv = _client(tmp_path).get("/connect?code=123456")
    assert rv.status_code == 200
    body = rv.get_data(as_text=True)
    # Flask/Jinja HTML-escapes attribute values, so & renders as &amp;.
    assert "/?pair=123456&amp;tab=remote" in body
    assert 'href="/"' in body


def test_connect_with_malformed_code_falls_back_to_admin_remote(tmp_path):
    """A malformed code is treated as absent, never echoed into a link —
    the customer still gets a working page with the in-app form path."""
    client = _client(tmp_path)
    for bad in ("12345", "1234567", "12345a", "%3Cscript%3E", "......"):
        rv = client.get(f"/connect?code={bad}")
        assert rv.status_code == 200
        body = rv.get_data(as_text=True)
        assert 'href="/admin/remote"' in body
        assert "pair=" not in body


def test_connect_button_target_actually_pairs(tmp_path):
    """Following the remote button's href from a coded /connect page runs
    the unchanged handle_pair_param flow: cookie issued, device recorded."""
    (tmp_path / "pairing_session.json").write_text(json.dumps({
        "schema": 1, "code": "222333",
        "issued_at": int(time.time()), "expires_at": int(time.time()) + 60,
        "attempts_remaining": 5, "nonce": "x" * 32,
    }))
    client = _client(tmp_path)

    rv = client.get("/connect?code=222333")
    assert "/?pair=222333&amp;tab=remote" in rv.get_data(as_text=True)

    rv2 = client.get("/?pair=222333&tab=remote", follow_redirects=False)
    assert rv2.status_code in (302, 303)
    assert "mdb_remote=" in rv2.headers.get("Set-Cookie", "")
    paired = json.loads((tmp_path / "paired_remotes.json").read_text())
    assert len(paired["devices"]) == 1
