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
    # Redirects somewhere with a Set-Cookie containing mdb_remote
    assert rv.status_code in (302, 303)
    assert "mdb_remote" in rv.headers.get("Set-Cookie", "")


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
