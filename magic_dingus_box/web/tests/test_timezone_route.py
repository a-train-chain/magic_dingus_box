"""Time zone from the owner's browser: validation, .env rewrite, the route."""
from __future__ import annotations

import io
import os
import stat
import subprocess
import zipfile
from pathlib import Path

import pytest

import timezone_apply
from timezone_apply import rewrite_env_tz, valid_zone


@pytest.fixture
def zoneinfo(tmp_path):
    d = tmp_path / "zoneinfo"
    (d / "America").mkdir(parents=True)
    (d / "America" / "Los_Angeles").write_text("TZif")
    (d / "UTC").write_text("TZif")
    (d / "Etc").mkdir()
    (d / "Etc" / "GMT+5").write_text("TZif")
    secret = tmp_path / "secret"
    secret.write_text("x")
    (d / "Evil").symlink_to(secret)  # symlink escaping the dir
    (d / "Dir").mkdir()
    return d


@pytest.mark.parametrize("name", ["America/Los_Angeles", "UTC", "Etc/GMT+5"])
def test_valid_zone_accepts(zoneinfo, name):
    assert valid_zone(name, zoneinfo) is True


@pytest.mark.parametrize("name", [
    "../etc/passwd", "Foo", "", "America/../UTC", "/etc/passwd", "Evil",
    "Dir", "America/Los_Angeles\n", "UTC; rm -rf /", None, 5,
])
def test_valid_zone_rejects(zoneinfo, name):
    assert valid_zone(name, zoneinfo) is False


def test_rewrite_env_tz_replaces_keeps_other_lines_and_mode(tmp_path):
    env = tmp_path / ".env"
    env.write_text("# c\nA=1\nTZ=UTC\nB=2\n")
    env.chmod(0o600)
    assert rewrite_env_tz(env, "America/Los_Angeles") is True
    assert env.read_text() == "# c\nA=1\nTZ=America/Los_Angeles\nB=2\n"
    assert stat.S_IMODE(env.stat().st_mode) == 0o600


def test_rewrite_env_tz_appends_when_missing(tmp_path):
    env = tmp_path / ".env"
    env.write_text("A=1")
    assert rewrite_env_tz(env, "Etc/GMT+5") is True
    assert env.read_text() == "A=1\nTZ=Etc/GMT+5\n"


def test_rewrite_env_tz_absent_file(tmp_path):
    assert rewrite_env_tz(tmp_path / "nope", "UTC") is False
    assert not (tmp_path / "nope").exists()


@pytest.fixture
def rig(app, temp_data_dir, zoneinfo, monkeypatch):
    calls = []
    state = {"current": "UTC", "set_rc": 0}

    def fake_run(argv, **kw):
        calls.append(list(argv))
        assert not kw.get("shell")
        if argv[:2] == ["sudo", "-n"] and argv[2:] == ["true"]:
            return subprocess.CompletedProcess(argv, 0, "", "")
        if argv[0] == "timedatectl" and argv[1] == "show":
            return subprocess.CompletedProcess(argv, 0, state["current"] + "\n", "")
        if "set-timezone" in argv:
            return subprocess.CompletedProcess(argv, state["set_rc"], "", "boom")
        return subprocess.CompletedProcess(argv, 0, "", "")

    monkeypatch.setattr(timezone_apply, "ZONEINFO_DIR", zoneinfo)
    monkeypatch.setattr(timezone_apply.subprocess, "run", fake_run)
    import admin_media_browser
    monkeypatch.setattr(admin_media_browser.subprocess, "run", fake_run)
    return calls, state


def _set_calls(calls):
    return [c for c in calls if "set-timezone" in c]


def test_route_changes_zone_and_pokes(client, temp_data_dir, rig):
    calls, _ = rig
    env = temp_data_dir.parent.parent / "services" / ".env"
    env.parent.mkdir(parents=True, exist_ok=True)
    env.write_text("TZ=UTC\nK=v\n")
    r = client.post("/admin/system/timezone", json={"zone": "America/Los_Angeles"})
    assert r.status_code == 200
    assert r.get_json()["data"]["changed"] is True
    assert _set_calls(calls) == [
        ["sudo", "-n", "timedatectl", "set-timezone", "America/Los_Angeles"]]
    assert env.read_text() == "TZ=America/Los_Angeles\nK=v\n"
    assert (temp_data_dir / "timezone_reload_request").exists()


def test_route_no_change_when_equal(client, temp_data_dir, rig):
    calls, state = rig
    state["current"] = "America/Los_Angeles"
    r = client.post("/admin/system/timezone", json={"zone": "America/Los_Angeles"})
    assert r.status_code == 200
    assert r.get_json()["data"]["changed"] is False
    assert _set_calls(calls) == []
    assert not (temp_data_dir / "timezone_reload_request").exists()


def test_route_works_without_env_file(client, temp_data_dir, rig):
    r = client.post("/admin/system/timezone", json={"zone": "America/Los_Angeles"})
    assert r.status_code == 200
    assert r.get_json()["data"]["changed"] is True
    assert (temp_data_dir / "timezone_reload_request").exists()


@pytest.mark.parametrize("body", [
    {"zone": "../etc/passwd"}, {"zone": "Foo"}, {"zone": ""}, {}, {"zone": 3},
    {"zone": "Evil"},
])
def test_route_rejects_invalid(client, temp_data_dir, rig, body):
    calls, _ = rig
    r = client.post("/admin/system/timezone", json=body)
    assert r.status_code == 400
    assert r.get_json()["error"]["code"] == "VALIDATION_ERROR"
    assert _set_calls(calls) == []
    assert not (temp_data_dir / "timezone_reload_request").exists()


def test_route_set_failure_is_500_and_no_poke(client, temp_data_dir, rig):
    _, state = rig
    state["set_rc"] = 1
    r = client.post("/admin/system/timezone", json={"zone": "America/Los_Angeles"})
    assert r.status_code == 500
    assert not (temp_data_dir / "timezone_reload_request").exists()


def test_route_requires_csrf(temp_data_dir, rig, monkeypatch):
    monkeypatch.delenv("MAGIC_DISABLE_CSRF", raising=False)
    from admin import create_app
    app = create_app(temp_data_dir, config={"TESTING": True})
    r = app.test_client().post("/admin/system/timezone",
                               json={"zone": "America/Los_Angeles"})
    assert r.status_code in (400, 403)
    assert _set_calls(rig[0]) == []


def test_backup_excludes_the_zone(client, temp_data_dir):
    zf = zipfile.ZipFile(io.BytesIO(client.get("/admin/backup").data))
    assert not any("timezone" in n.lower() or n.endswith(".env") for n in zf.namelist())
