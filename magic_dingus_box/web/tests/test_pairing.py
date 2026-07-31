"""End-to-end pairing tests: code validation, brute-force lockout, cookie issue, revoke."""
import json
import time
from pathlib import Path

import pytest

from admin import create_app


@pytest.fixture
def app(tmp_path):
    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    app.config["SECRET_KEY"] = "test-secret-key"
    return app


@pytest.fixture
def client(app):
    return app.test_client()


def write_session(tmp_path: Path, code="847291", attempts=5, expires_in=120):
    payload = {
        "schema": 1,
        "code": code,
        "issued_at": int(time.time()),
        "expires_at": int(time.time()) + expires_in,
        "attempts_remaining": attempts,
        "nonce": "abc123" * 8,
    }
    p = tmp_path / "pairing_session.json"
    p.write_text(json.dumps(payload))
    return p


def test_pair_with_correct_code_issues_cookie(client, app, tmp_path):
    write_session(tmp_path)
    rv = client.get("/?pair=847291&tab=remote", follow_redirects=False)
    # Redirects to the nickname-prompt page with a Set-Cookie containing mdb_remote
    assert rv.status_code in (302, 303)
    assert "mdb_remote" in rv.headers.get("Set-Cookie", "")
    assert "/admin/remote/name" in rv.headers.get("Location", "")


def test_pair_with_wrong_code_decrements_attempts(client, app, tmp_path):
    p = write_session(tmp_path, attempts=5)
    rv = client.get("/?pair=000000")
    assert rv.status_code == 401
    data = json.loads(p.read_text())
    assert data["attempts_remaining"] == 4
    # Cookie not set
    assert "mdb_remote" not in rv.headers.get("Set-Cookie", "")


def test_five_wrong_attempts_deletes_session(client, tmp_path):
    p = write_session(tmp_path, attempts=1)
    rv = client.get("/?pair=000000")
    assert rv.status_code == 401
    assert not p.exists()


def test_expired_code_returns_410(client, tmp_path):
    write_session(tmp_path, expires_in=-10)
    rv = client.get("/?pair=847291")
    assert rv.status_code == 410


def test_no_session_file_returns_410(client):
    rv = client.get("/?pair=847291")
    assert rv.status_code == 410


def test_revoked_device_rejects_cookie(client, app, tmp_path):
    write_session(tmp_path)
    rv = client.get("/?pair=847291&tab=remote", follow_redirects=False)
    assert rv.status_code in (302, 303)

    # Find the device id and remove from paired_remotes.json
    paired_path = tmp_path / "paired_remotes.json"
    data = json.loads(paired_path.read_text())
    assert len(data["devices"]) == 1
    device_id = data["devices"][0]["id"]
    data["devices"] = []
    paired_path.write_text(json.dumps(data))

    # The cookie still HMAC-verifies but the device lookup misses → 401
    cookie = rv.headers["Set-Cookie"].split(";")[0]
    name, value = cookie.split("=", 1)
    client.set_cookie(domain="localhost", key=name, value=value)
    rv2 = client.get("/admin/remote/protected_check")
    assert rv2.status_code == 401


def test_nickname_submission_updates_device(client, app, tmp_path):
    write_session(tmp_path)
    # First pair (sets cookie)
    rv = client.get("/?pair=847291&tab=remote", follow_redirects=False)
    cookie = rv.headers["Set-Cookie"].split(";")[0]
    name, value = cookie.split("=", 1)
    client.set_cookie(domain="localhost", key=name, value=value)

    # GET the form
    rv2 = client.get("/admin/remote/name")
    assert rv2.status_code == 200
    assert b"Paired" in rv2.data

    # POST a nickname
    rv3 = client.post("/admin/remote/name?tab=remote",
                      data={"nickname": "Alex's iPhone"},
                      follow_redirects=False)
    assert rv3.status_code in (302, 303)
    assert "/?tab=remote" in rv3.headers["Location"]

    # Verify paired_remotes.json was updated
    paired = json.loads((tmp_path / "paired_remotes.json").read_text())
    assert paired["devices"][0]["nickname"] == "Alex's iPhone"


def test_nickname_page_without_cookie_redirects_home(client, app, tmp_path):
    rv = client.get("/admin/remote/name")
    assert rv.status_code in (302, 303)
    assert rv.headers["Location"] == "/"


def test_reap_revocations_removes_matching_devices(tmp_path):
    # Seed paired_remotes.json with two devices
    paired = tmp_path / "paired_remotes.json"
    paired.write_text(json.dumps({
        "schema": 1,
        "devices": [
            {"id": "aaa", "nickname": "iPhone", "paired_at": 1, "last_seen": 1},
            {"id": "bbb", "nickname": "Pixel",  "paired_at": 2, "last_seen": 2},
        ],
    }))
    # Kiosk writes a revocation for "aaa"
    (tmp_path / "pending_revocations.txt").write_text("aaa\n")

    from remote.auth import reap_revocations
    removed = reap_revocations(tmp_path)
    assert removed == 1
    data = json.loads(paired.read_text())
    assert len(data["devices"]) == 1
    assert data["devices"][0]["id"] == "bbb"
    # Revocation file is consumed
    assert not (tmp_path / "pending_revocations.txt").exists()


def test_reap_revocations_is_noop_when_file_missing(tmp_path):
    from remote.auth import reap_revocations
    assert reap_revocations(tmp_path) == 0


def test_audit_log_rotates_at_size_cap(tmp_path, monkeypatch):
    """pairing_audit.log must not grow without bound — nothing else prunes
    it, and an appliance runs for years (or a hostile LAN device hammers
    /pair). Rotation keeps the newest tail and the fresh entry."""
    from remote import auth as auth_mod

    monkeypatch.setattr(auth_mod, "_data_dir", lambda: tmp_path)
    log = tmp_path / "pairing_audit.log"

    filler = (b'{"ts": 0, "ip": "10.0.0.9", "outcome": "bad_code", '
              b'"code": "12****"}\n')
    log.write_bytes(filler * ((auth_mod._AUDIT_MAX_BYTES // len(filler)) + 10))
    oversized = log.stat().st_size
    assert oversized > auth_mod._AUDIT_MAX_BYTES

    auth_mod._audit("paired", "123456", "10.0.0.7")

    assert log.stat().st_size <= auth_mod._AUDIT_KEEP_BYTES + 4096
    lines = log.read_bytes().splitlines()
    assert b'"outcome": "paired"' in lines[-1]
    # Every surviving line is intact JSON — rotation must cut on a line
    # boundary, not mid-record.
    import json as _json
    for ln in lines:
        _json.loads(ln)
