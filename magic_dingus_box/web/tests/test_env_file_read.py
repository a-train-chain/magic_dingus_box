"""_read_env_file must never mistake an UNREADABLE services/.env for an empty one.

Field bug: on a box whose .env was root-owned 0600, the web admin (User=magic)
got PermissionError, _read_env_file swallowed it and returned {}, and the VPN
Reconfigure route merged only the WireGuard keys into that {} and wrote it
back — erasing QBITTORRENT_ADMIN_PASSWORD and every API key. setup_services.sh
then generated a qBit password qBittorrent did not have, and the kiosk,
qbit-port-sync and password-sync all lost qBit auth.

Contract pinned here: {} ONLY when the file is missing; any other read error
raises, and the setup route fails the request without writing anything.
"""
from __future__ import annotations

import json
import os
import shutil
import tempfile
from pathlib import Path

import pytest

import admin
from admin import EnvFileReadError, _read_env_file, create_app

_SYSTEM_TMP = tempfile.gettempdir()

# Synthetic WireGuard config — keys are placeholders that belong to nobody.
FAKE_CONF = """[Interface]
PrivateKey = cGxhY2Vob2xkZXJwbGFjZWhvbGRlcnBsYWNlaG9sZGVyMDA=
Address = 10.2.0.2/32

[Peer]
PublicKey = cHViYmxpY3BsYWNlaG9sZGVycHViYmxpY3BsYWNlaG8wMDA=
AllowedIPs = 0.0.0.0/0
Endpoint = 203.0.113.10:51820
"""

ORIGINAL_ENV = (
    "QBITTORRENT_ADMIN_PASSWORD=keep-me-secret\n"
    "RADARR_API_KEY=keep-me-too\n"
    "WIREGUARD_PRIVATE_KEY=old\n"
)


def _can_make_unreadable(path: Path) -> bool:
    """chmod 000 is meaningless to root; skip rather than false-pass."""
    try:
        path.read_text()
    except PermissionError:
        return True
    return False


@pytest.fixture
def scratch():
    base = tempfile.mkdtemp(dir=_SYSTEM_TMP, prefix="mdb-envread-test-")
    try:
        yield Path(base)
    finally:
        for p in Path(base).rglob("*"):
            try:
                p.chmod(0o700)
            except OSError:
                pass
        shutil.rmtree(base, ignore_errors=True)


def test_missing_file_is_empty(scratch: Path):
    assert _read_env_file(scratch / "nope" / ".env") == {}


def test_readable_file_parses(scratch: Path):
    env = scratch / ".env"
    env.write_text('# c\nA=1\nB="two words"\n\nnoequals\n')
    assert _read_env_file(env) == {"A": "1", "B": "two words"}


def test_unreadable_file_raises_not_empty(scratch: Path):
    env = scratch / ".env"
    env.write_text(ORIGINAL_ENV)
    env.chmod(0o000)
    if not _can_make_unreadable(env):
        pytest.skip("running as root — permissions are not enforced")
    with pytest.raises(EnvFileReadError):
        _read_env_file(env)


def test_directory_in_place_of_file_raises(scratch: Path):
    env = scratch / ".env"
    env.mkdir()
    with pytest.raises(EnvFileReadError):
        _read_env_file(env)


def test_setup_route_refuses_and_never_writes_an_unreadable_env(
        scratch: Path, monkeypatch):
    # Layout the app derives paths from:
    #   SERVICES_ENV          = data_dir.parent.parent / "services" / ".env"
    #   SETUP_SERVICES_SCRIPT = data_dir.parent / "scripts" / "setup_services.sh"
    root = scratch / "opt"
    data_dir = root / "cpp" / "data"
    for sub in ("playlists", "media", "roms", "upload_temp"):
        (data_dir / sub).mkdir(parents=True)
    scripts = root / "cpp" / "scripts"
    scripts.mkdir(parents=True)
    (scripts / "setup_services.sh").write_text("#!/bin/bash\nexit 0\n")
    services = root / "services"
    services.mkdir()
    env = services / ".env"
    env.write_text(ORIGINAL_ENV)

    settings = scratch / "settings.json"
    settings.write_text(json.dumps({"playback": {"media_browser_unlocked": True}}))
    monkeypatch.setattr(admin, "MEDIA_BROWSER_SETTINGS_PATH", str(settings))
    monkeypatch.setattr(admin, "_require_nopasswd_sudo", lambda: None)
    monkeypatch.setenv("MAGIC_DISABLE_CSRF", "1")

    original_tempdir = tempfile.tempdir
    try:
        app = create_app(data_dir, config={"TESTING": True})
        client = app.test_client()
        # After create_app (which starts its own broadcaster thread): the
        # setup job must never be spawned for an unreadable .env.
        started = []
        monkeypatch.setattr(admin.threading, "Thread",
                            lambda *a, **k: started.append(1))
        # The setup script now runs as a detached job, not a thread.
        monkeypatch.setattr(admin.DetachedJobs, "launch",
                            lambda *a, **k: started.append(1))

        env.chmod(0o000)
        if not _can_make_unreadable(env):
            pytest.skip("running as root — permissions are not enforced")

        resp = client.post("/admin/media-browser/setup",
                           data={"config_text": FAKE_CONF, "provider": "custom"})
        assert resp.status_code == 500
        body = resp.get_json()
        assert body["error"]["code"] == "env_read_failed"
        assert not started

        env.chmod(0o600)
        assert env.read_text() == ORIGINAL_ENV, ".env must be untouched"
    finally:
        tempfile.tempdir = original_tempdir
        os.environ.pop("MAGIC_DISABLE_CSRF", None)
