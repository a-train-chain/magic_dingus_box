"""
Paused-for-playback marker: the kiosk's playback_services_pause.sh stops
mdb_radarr / mdb_prowlarr / mdb_byparr to free RAM while a game or movie
runs. Radarr and Prowlarr (.NET) never survive the 2 s SIGTERM grace, so
docker reports "Exited (137)" — which the Content Manager used to display
raw, reading exactly like a crash/OOM (caused a support question,
2026-07-31).

The script now maintains a marker file for the duration of the pause, and
/admin/media-browser/status tags affected containers with
paused_for_playback so the frontend can label them honestly.

Two halves under test:
  * endpoint side — the flag is set only for (marker present) AND
    (container in the pause set) AND (docker status says Exited).
  * script side — `pause` creates the marker before stopping, `unpause`
    removes it after starting. Exercised with a stubbed `docker` on PATH.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import admin as admin_module

REPO_ROOT = Path(__file__).parents[3]
PAUSE_SCRIPT = (
    REPO_ROOT / "magic_dingus_box_cpp" / "scripts" / "playback_services_pause.sh"
)

# The three containers the kiosk stops for playback. Gluetun and qBittorrent
# intentionally stay up (VPN netns + active downloads).
PAUSED_SET = {"mdb_radarr", "mdb_prowlarr", "mdb_byparr"}

DOCKER_PS_PAUSED = "\n".join([
    "mdb_gluetun\tUp 17 minutes (healthy)",
    "mdb_radarr\tExited (137) 6 minutes ago",
    "mdb_prowlarr\tExited (137) 6 minutes ago",
    "mdb_qbittorrent\tUp 17 minutes",
    "mdb_byparr\tExited (143) 6 minutes ago",
])

DOCKER_PS_ALL_UP = "\n".join([
    "mdb_gluetun\tUp 17 minutes (healthy)",
    "mdb_radarr\tUp 2 minutes",
    "mdb_prowlarr\tUp 2 minutes",
    "mdb_qbittorrent\tUp 17 minutes",
    "mdb_byparr\tUp 2 minutes",
])


# NOTE: fixtures here deliberately derive paths from temp_data_dir rather
# than pytest's tmp_path. create_app() globally repoints TMPDIR into the
# app's upload_temp dir; tmp_path's session-cached basetemp would land
# inside it and evaporate with the first test's teardown.


@pytest.fixture
def unlocked_mb(temp_data_dir, monkeypatch):
    """Satisfy the Layer 1 unlock gate via a temp settings.json."""
    settings = temp_data_dir / "mb_settings.json"
    settings.write_text(json.dumps(
        {"playback": {"media_browser_unlocked": True}}))
    monkeypatch.setattr(admin_module, "MEDIA_BROWSER_SETTINGS_PATH",
                        str(settings))
    return settings


@pytest.fixture
def pause_marker(temp_data_dir, monkeypatch):
    """Point the endpoint at a temp marker path; yield it (not yet created)."""
    marker = temp_data_dir / "mdb_playback_services_paused"
    monkeypatch.setattr(admin_module, "PLAYBACK_PAUSE_MARKER", marker)
    return marker


@pytest.fixture
def fake_docker_ps(monkeypatch):
    """Stub subprocess.run inside admin for `docker ps`; configurable stdout."""
    state = {"stdout": DOCKER_PS_PAUSED}
    real_run = admin_module.subprocess.run

    def run(cmd, *args, **kwargs):
        if cmd[:2] == ["docker", "ps"]:
            return subprocess.CompletedProcess(
                cmd, 0, stdout=state["stdout"], stderr="")
        if cmd and cmd[0] == "docker":
            # Anything else docker-flavored (e.g. the gluetun exec for VPN
            # info) pretends the daemon is unreachable — irrelevant here.
            return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="")
        return real_run(cmd, *args, **kwargs)

    monkeypatch.setattr(admin_module.subprocess, "run", run)
    return state


def _containers(client):
    resp = client.get("/admin/media-browser/status")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["ok"] is True
    return {c["name"]: c for c in body["data"]["containers"]}


class TestStatusEndpointPausedFlag:
    def test_marker_plus_exited_flags_paused(
            self, client, unlocked_mb, pause_marker, fake_docker_ps):
        pause_marker.write_text("2026-07-31T17:35:08Z\n")
        by_name = _containers(client)
        for name in PAUSED_SET:
            assert by_name[name]["paused_for_playback"] is True, name
            # Raw docker state must stay visible as secondary detail.
            assert by_name[name]["status"].startswith("Exited")
        assert by_name["mdb_gluetun"]["paused_for_playback"] is False
        assert by_name["mdb_qbittorrent"]["paused_for_playback"] is False

    def test_no_marker_means_no_paused_flag(
            self, client, unlocked_mb, pause_marker, fake_docker_ps):
        # Same Exited statuses, but no marker: a genuinely dead container
        # must keep looking alarming.
        by_name = _containers(client)
        assert all(not c["paused_for_playback"] for c in by_name.values())

    def test_marker_with_running_containers_not_flagged(
            self, client, unlocked_mb, pause_marker, fake_docker_ps):
        # Race window: status polled right after unpause restarted the
        # containers but before the script removed the marker. Up beats
        # the marker.
        pause_marker.write_text("2026-07-31T17:35:08Z\n")
        fake_docker_ps["stdout"] = DOCKER_PS_ALL_UP
        by_name = _containers(client)
        assert all(not c["paused_for_playback"] for c in by_name.values())

    def test_marker_with_missing_container_not_flagged(
            self, client, unlocked_mb, pause_marker, fake_docker_ps):
        # Stack torn down (e.g. MB reset) while a stale marker lingers:
        # "not found" is not the pause signature and must not be dressed
        # up as one.
        pause_marker.write_text("2026-07-31T17:35:08Z\n")
        fake_docker_ps["stdout"] = "mdb_gluetun\tUp 17 minutes (healthy)"
        by_name = _containers(client)
        assert all(not c["paused_for_playback"] for c in by_name.values())


class TestPauseScriptMarker:
    """Run the real script with a stubbed docker; assert marker lifecycle."""

    MARKER = Path("/tmp/mdb_playback_services_paused")

    @pytest.fixture(autouse=True)
    def stub_docker(self):
        # Explicit TemporaryDirectory instead of tmp_path — see module note.
        import tempfile as _tempfile
        with _tempfile.TemporaryDirectory() as tmpdir:
            stub_bin = Path(tmpdir) / "bin"
            stub_bin.mkdir()
            docker = stub_bin / "docker"
            # `inspect` succeeding makes every container "exist"; all other
            # subcommands are accepted no-ops.
            docker.write_text("#!/bin/bash\nexit 0\n")
            docker.chmod(0o755)
            self.env = dict(os.environ, PATH=f"{stub_bin}:{os.environ['PATH']}")
            self.MARKER.unlink(missing_ok=True)
            yield
            self.MARKER.unlink(missing_ok=True)

    def _run(self, action):
        return subprocess.run(
            ["bash", str(PAUSE_SCRIPT), action],
            env=self.env, capture_output=True, text=True, timeout=30)

    def test_pause_creates_marker(self):
        result = self._run("pause")
        assert result.returncode == 0, result.stderr
        assert self.MARKER.exists()

    def test_unpause_removes_marker(self):
        self.MARKER.write_text("stale\n")
        result = self._run("unpause")
        assert result.returncode == 0, result.stderr
        assert not self.MARKER.exists()

    def test_pause_then_unpause_round_trip(self):
        self._run("pause")
        assert self.MARKER.exists()
        self._run("unpause")
        assert not self.MARKER.exists()
