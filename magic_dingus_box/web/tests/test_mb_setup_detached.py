"""Media Browser setup runs detached and is single-flight with the OTA.

setup_services.sh restarts magic-dingus-web itself; under the unit's default
KillMode=control-group that killed the script mid-provisioning (it was only
start_new_session'd). It now runs as its own systemd unit, its status is
re-derived from an on-disk log (so a restarted Flask still reports it), and a
second maintenance job while one runs is refused with 409 BEFORE .env is
touched.
"""
from __future__ import annotations

import json
import os
import time
from pathlib import Path

import pytest

import admin
from admin import create_app

# Synthetic WireGuard config — keys are placeholders that belong to nobody.
FAKE_CONF = """[Interface]
PrivateKey = cGxhY2Vob2xkZXJwbGFjZWhvbGRlcnBsYWNlaG9sZGVyMDA=
Address = 10.2.0.2/32

[Peer]
PublicKey = cHViYmxpY3BsYWNlaG9sZGVycHViYmxpY3BsYWNlaG8wMDA=
AllowedIPs = 0.0.0.0/0
Endpoint = 203.0.113.10:51820
"""

ORIGINAL_ENV = "QBITTORRENT_ADMIN_PASSWORD=keep-me-secret\nWIREGUARD_PRIVATE_KEY=old\n"


@pytest.fixture
def mb_box(tmp_path, monkeypatch):
    root = tmp_path / "opt"
    data_dir = root / "cpp" / "data"
    for sub in ("playlists", "media", "roms", "upload_temp"):
        (data_dir / sub).mkdir(parents=True)
    scripts = root / "cpp" / "scripts"
    scripts.mkdir(parents=True)
    setup = scripts / "setup_services.sh"
    setup.write_text("#!/bin/bash\necho 'Step 1: provisioning'\n"
                     "sleep \"${MB_SLOW:-0.6}\"\necho 'All done'\n")
    setup.chmod(0o755)
    update = scripts / "update.sh"
    update.write_text("#!/bin/bash\necho '{\"ok\": true, \"stage\": \"downloading\", "
                      "\"progress\": 10, \"message\": \"x\"}'\nsleep 2\n")
    update.chmod(0o755)
    (root / "services").mkdir()
    env = root / "services" / ".env"
    env.write_text(ORIGINAL_ENV)
    (root / "VERSION").write_text("1.0.7\n")

    settings = tmp_path / "settings.json"
    settings.write_text(json.dumps({"playback": {"media_browser_unlocked": True}}))
    monkeypatch.setattr(admin, "MEDIA_BROWSER_SETTINGS_PATH", str(settings))
    monkeypatch.setattr(admin, "_require_nopasswd_sudo", lambda: None)
    monkeypatch.setenv("MAGIC_DISABLE_CSRF", "1")
    # Popen mode would wrap the root job in `sudo -n`, which a dev machine
    # / CI runner may not grant; the privilege is not what is under test.
    real_launch = admin.DetachedJobs.launch
    monkeypatch.setattr(admin.DetachedJobs, "launch",
                        lambda self, kind, argv, **kw: real_launch(
                            self, kind, argv, **{**kw, "as_root": False}))

    def make_app():
        app = create_app(data_dir, config={"TESTING": True})
        app.config["TESTING"] = True
        return app

    return {"make_app": make_app, "env": env}


def _poll_mb(client, job_id, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = client.get(f"/admin/media-browser/setup-status/{job_id}").get_json()["data"]
        if data["status"] not in ("running",):
            return data
        time.sleep(0.1)
    pytest.fail("setup never finished")


def test_setup_status_survives_a_flask_restart(mb_box):
    client = mb_box["make_app"]().test_client()
    r = client.post("/admin/media-browser/setup",
                    data={"config_text": FAKE_CONF, "provider": "custom"})
    assert r.status_code == 200, r.get_json()
    job_id = r.get_json()["data"]["job_id"]

    restarted = mb_box["make_app"]().test_client()
    data = _poll_mb(restarted, job_id)
    assert data["status"] == "success"
    assert data["exit_code"] == 0
    assert "Step 1: provisioning" in data["log_lines"]
    assert "All done" in data["log_lines"]


def test_setup_refused_while_an_ota_runs_and_env_untouched(mb_box):
    client = mb_box["make_app"]().test_client()
    r = client.post("/admin/update/install", json={
        "version": "1.0.8",
        "download_url": "https://github.com/a-train-chain/magic_dingus_box/"
                        "releases/download/v1.0.8/release.tar.gz"})
    assert r.status_code == 200, r.get_json()

    r = client.post("/admin/media-browser/setup",
                    data={"config_text": FAKE_CONF, "provider": "custom"})
    assert r.status_code == 409
    assert "update" in r.get_json()["error"]["message"].lower()
    assert mb_box["env"].read_text() == ORIGINAL_ENV
