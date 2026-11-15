"""Media Browser reset takes the shared compose lock around `compose down`.

Every actor that starts/stops/recreates the stack's containers takes
/run/lock/mdb-compose.lock (flock, bounded wait, proceed WITHOUT the lock on
timeout — never a deadlock). The reset route used to run `compose down`
unlocked, so a cascade re-link or storage re-attach could recreate a
container it was removing.

Under test, with subprocess.run monkeypatched (no docker, no sudo):
  * the lock is HELD while `compose down` runs, and released afterwards;
  * a lock held by someone else for longer than the wait does not block the
    reset — `compose down` still runs, and the step log says so;
  * the file wipes run after the lock is released (only the down is locked).
"""
from __future__ import annotations

import fcntl
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import admin as admin_module
# COMPOSE_LOCK_PATH / COMPOSE_LOCK_WAIT_S are module state of the ops module:
# patch them on their owner (admin does not re-export them).
import admin_media_browser_ops


@pytest.fixture
def unlocked_mb(temp_data_dir, monkeypatch):
    settings = temp_data_dir / "mb_settings.json"
    settings.write_text(json.dumps(
        {"playback": {"media_browser_unlocked": True}}))
    monkeypatch.setattr(admin_module, "MEDIA_BROWSER_SETTINGS_PATH",
                        str(settings))
    monkeypatch.setattr(admin_module, "_require_nopasswd_sudo", lambda: None)


@pytest.fixture
def lock_path(temp_data_dir, monkeypatch):
    path = temp_data_dir / "mdb-compose.lock"
    monkeypatch.setattr(admin_media_browser_ops, "COMPOSE_LOCK_PATH", path)
    monkeypatch.setattr(admin_media_browser_ops, "COMPOSE_LOCK_WAIT_S", 1.0)
    return path


def _lock_is_free(path: Path) -> bool:
    """True if a fresh fd can take the flock right now (it is not held)."""
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False
    finally:
        os.close(fd)


@pytest.fixture
def recorded_runs(monkeypatch, lock_path):
    """Stub subprocess.run; record each call and whether the lock was free."""
    calls = []

    def run(cmd, *args, **kwargs):
        free = _lock_is_free(lock_path) if lock_path.exists() else True
        calls.append((list(cmd), free))
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

    monkeypatch.setattr(admin_module.subprocess, "run", run)
    return calls


def _reset(client):
    return client.post("/admin/media-browser/reset",
                       data=json.dumps({"confirm": "RESET"}),
                       content_type="application/json")


def _services_dir(temp_data_dir: Path) -> Path:
    # admin_media_browser: SERVICES_DIR = data_dir.parent.parent / "services"
    return temp_data_dir.parent.parent / "services"


class TestResetComposeLock:
    def test_compose_down_runs_under_the_lock(
            self, client, unlocked_mb, lock_path, recorded_runs):
        resp = _reset(client)
        assert resp.status_code == 200, resp.get_json()
        downs = [(c, free) for c, free in recorded_runs
                 if c[-3:] == ["compose", "down", "--remove-orphans"]]
        assert len(downs) == 1
        # Lock held (not free) while compose down ran ...
        assert downs[0][1] is False
        # ... and released once the route returned.
        assert _lock_is_free(lock_path)
        steps = resp.get_json()["data"]["steps"]
        assert "compose_down" in steps
        assert "compose_lock_unavailable" not in steps

    def test_busy_lock_times_out_and_reset_still_runs(
            self, client, unlocked_mb, lock_path, recorded_runs):
        holder = os.open(lock_path, os.O_RDONLY | os.O_CREAT, 0o666)
        try:
            fcntl.flock(holder, fcntl.LOCK_EX)
            resp = _reset(client)
        finally:
            os.close(holder)
        assert resp.status_code == 200, resp.get_json()
        steps = resp.get_json()["data"]["steps"]
        # Bounded wait, then proceed without the lock — never a deadlock.
        assert "compose_lock_unavailable" in steps
        assert "compose_down" in steps
        assert steps.index("compose_lock_unavailable") < steps.index("compose_down")

    def test_file_wipes_run_after_the_lock_is_released(
            self, client, unlocked_mb, lock_path, recorded_runs, temp_data_dir):
        services = _services_dir(temp_data_dir)
        (services / "config" / "radarr").mkdir(parents=True, exist_ok=True)
        (services / "config" / "radarr" / "radarr.db").write_text("x")
        resp = _reset(client)
        assert resp.status_code == 200, resp.get_json()
        wipes = [(c, free) for c, free in recorded_runs if "find" in c]
        assert wipes, recorded_runs
        assert all(free for _, free in wipes)

    def test_unusable_lock_path_proceeds_without_it(
            self, client, unlocked_mb, monkeypatch, recorded_runs,
            temp_data_dir):
        monkeypatch.setattr(admin_media_browser_ops, "COMPOSE_LOCK_PATH",
                            temp_data_dir / "no" / "such" / "dir" / "x.lock")
        resp = _reset(client)
        assert resp.status_code == 200, resp.get_json()
        steps = resp.get_json()["data"]["steps"]
        assert "compose_lock_unavailable" in steps
        assert "compose_down" in steps
