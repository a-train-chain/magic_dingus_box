"""TEST-ONLY MDB_PLATFORM_POLICY_OVERRIDE in the web admin.

Mirror of the kiosk's platform::apply_policy_override: with the env var
set to exactly "pi4" on a real Pi 5, the web admin applies the Pi 4B's
software policies (one concurrent transcode, ultrafast/CRF 28). Anything
else leaves the real board in force, and every set value is logged.
"""
from __future__ import annotations

import pytest

import admin
from admin import PLATFORM_POLICY_OVERRIDE_ENV, _policy_pi_model, create_app


@pytest.mark.parametrize("real", ["pi4", "pi5", "unknown"])
@pytest.mark.parametrize("env", [None, ""])
def test_unset_override_is_a_silent_no_op(real, env):
    assert _policy_pi_model(real, env) == (real, "")


def test_pi4_override_on_a_pi5_switches_policy_and_logs_loudly():
    model, line = _policy_pi_model("pi5", "pi4")
    assert model == "pi4"
    assert line.startswith(
        "PLATFORM POLICY OVERRIDE ACTIVE: running Pi 4B policies on "
        "Raspberry Pi 5")


@pytest.mark.parametrize("bad", ["pi5", "PI4", " pi4", "pi4 ", "1", "true"])
def test_invalid_value_is_ignored_with_a_warning(bad):
    model, line = _policy_pi_model("pi5", bad)
    assert model == "pi5"
    assert "IGNORED" in line and "ACTIVE" not in line


def test_override_on_an_actual_pi4_is_a_no_op():
    model, line = _policy_pi_model("pi4", "pi4")
    assert model == "pi4"
    assert "IGNORED" in line


def test_override_on_an_unknown_board_is_ignored():
    model, line = _policy_pi_model("unknown", "pi4")
    assert model == "unknown"
    assert "IGNORED" in line


def _make_app(tmp_path, monkeypatch, board, env):
    monkeypatch.setattr(admin, "_detect_pi_model", lambda: board)
    if env is None:
        monkeypatch.delenv(PLATFORM_POLICY_OVERRIDE_ENV, raising=False)
    else:
        monkeypatch.setenv(PLATFORM_POLICY_OVERRIDE_ENV, env)
    monkeypatch.delenv("MAGIC_MAX_TRANSCODES", raising=False)
    data = tmp_path / "data"
    data.mkdir()
    return create_app(data, config={"TESTING": True})


def test_create_app_pi5_without_override_keeps_pi5_policy(tmp_path, monkeypatch):
    app = _make_app(tmp_path, monkeypatch, "pi5", None)
    assert app.config["MAX_TRANSCODES"] == 2
    assert app.config["PLATFORM_POLICY_OVERRIDE"] is None


def test_create_app_pi5_with_override_runs_pi4_policy(tmp_path, monkeypatch, capsys):
    app = _make_app(tmp_path, monkeypatch, "pi5", "pi4")
    assert app.config["MAX_TRANSCODES"] == 1
    assert app.config["PLATFORM_POLICY_OVERRIDE"] == "pi4"
    assert "PLATFORM POLICY OVERRIDE ACTIVE" in capsys.readouterr().err


def test_create_app_pi5_with_invalid_override_keeps_pi5_policy(tmp_path, monkeypatch, capsys):
    app = _make_app(tmp_path, monkeypatch, "pi5", "pi4b")
    assert app.config["MAX_TRANSCODES"] == 2
    assert app.config["PLATFORM_POLICY_OVERRIDE"] is None
    assert "IGNORED" in capsys.readouterr().err
