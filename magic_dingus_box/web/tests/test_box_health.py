"""Box health: verify_box.sh output parsing, the runner, and the endpoints."""
import json
import os
import subprocess
import time
from pathlib import Path
from unittest.mock import patch

import pytest

import box_health
from box_health import HealthRunner, headline, parse_verify_box

# Captured shape of a real run (colours off, as when stdout is not a tty),
# trimmed, with a FAIL carrying detail lines and several WARNs.
SAMPLE = """\

== Platform ==
  [PASS] board: Raspberry Pi 5 Model B Rev 1.0
  [PASS] detection: Platform: Raspberry Pi 5
  [PASS] clock 2400MHz, 52.1'C, no throttling

== First boot & boot config ==
  [PASS] no first-boot log (source / hand-provisioned box)
  [PASS] magic-first-boot.service not enabled (disabled)

== Display ==
  [PASS] mode: modern_tv -> 1920x1080
  [PASS] logical canvas canary: set_content_viewport(960, 720)

== Audio ==
  [WARN] no TV reporting audio on HDMI (TV off, or a CRT converter without audio EDID?) — default sink alsa_output.hdmi

== Content ==
  [PASS] video playlists: 41/41 files present
  [PASS] game playlists: 120/120 ROMs present
  [WARN] no Dreamcast BIOS (dc_boot.bin) — flycast will use its HLE BIOS (REIOS)

== Kiosk ==
  [PASS] service active
  [FAIL] 2 failed unit(s)
         qbit-port-sync.service      loaded failed failed qBit port sync
         magic-dingus-smoke-test.service loaded failed failed Smoke test
  [PASS] status file fresh + on a real screen

== Storage & Media Browser ==
  [WARN] movie drive not mounted — Movies shows 'drive not connected' (expected if unplugged)
  [FAIL] SD card 91% full — 2.4G free

== RESULT ==
  11 passed, 2 failed, 3 warnings
  NOT SHIPPABLE — fix the FAILs above
"""


def test_parse_sections_and_counts():
    r = parse_verify_box(SAMPLE, exit_code=1)
    names = [s["name"] for s in r["sections"]]
    assert names == ["Platform", "First boot & boot config", "Display", "Audio",
                     "Content", "Kiosk", "Storage & Media Browser"]
    assert (r["passed"], r["failed"], r["warnings"]) == (11, 2, 3)
    assert r["script_totals"] == {"passed": 11, "failed": 2, "warnings": 3}
    assert r["shippable"] is False
    assert r["exit_code"] == 1


def test_parse_levels_text_and_details():
    r = parse_verify_box(SAMPLE, exit_code=1)
    kiosk = next(s for s in r["sections"] if s["name"] == "Kiosk")
    failed = kiosk["checks"][1]
    assert failed["level"] == "fail"
    assert failed["text"] == "2 failed unit(s)"
    assert failed["details"][0].startswith("qbit-port-sync.service")
    assert len(failed["details"]) == 2
    assert "details" not in kiosk["checks"][2]
    audio = next(s for s in r["sections"] if s["name"] == "Audio")
    assert audio["checks"][0]["level"] == "warn"


def test_parse_strips_ansi_and_handles_shippable():
    out = ("\n\x1b[1m== Platform ==\x1b[0m\n  \x1b[32m[PASS]\x1b[0m board: Pi 5\n"
           "\n\x1b[1m== RESULT ==\x1b[0m\n  1 passed, 0 failed, 0 warnings\n  SHIPPABLE\n")
    r = parse_verify_box(out, exit_code=0)
    assert r["sections"][0]["checks"] == [{"level": "pass", "text": "board: Pi 5"}]
    assert r["shippable"] is True


def test_parse_garbage_is_empty_not_an_error():
    r = parse_verify_box("sudo: a password is required\n", exit_code=1)
    assert r["sections"] == [] and r["failed"] == 0


def test_headlines():
    assert headline({"failed": 0, "warnings": 0, "exit_code": 0}) == "Everything looks good"
    assert headline({"failed": 0, "warnings": 2, "exit_code": 0}).startswith(
        "Everything important looks good (2 notes)")
    assert headline({"failed": 2, "warnings": 0, "exit_code": 1}) == "2 problems found"
    assert headline({"failed": 1, "warnings": 0, "exit_code": 1}) == "1 problem found"
    assert headline({"error": "x"}) == "The health check could not run"


# ----------------------------------------------------------------- runner

class FakeRun:
    def __init__(self, stdout=SAMPLE, rc=1, stderr="", delay=0.0, exc=None):
        self.calls = []
        self.stdout, self.rc, self.stderr, self.delay, self.exc = stdout, rc, stderr, delay, exc

    def __call__(self, argv, **kw):
        self.calls.append((argv, kw))
        if self.delay:
            time.sleep(self.delay)
        if self.exc:
            raise self.exc
        return subprocess.CompletedProcess(argv, self.rc, self.stdout, self.stderr)


def _runner(tmp_path, fake, script=True):
    s = tmp_path / "scripts" / "verify_box.sh"
    if script:
        s.parent.mkdir(parents=True, exist_ok=True)
        s.write_text("#!/bin/bash\n")
    return HealthRunner(script=s, cache_path=tmp_path / "data" / box_health.CACHE_NAME, run=fake)


def test_runner_argv_is_fixed(tmp_path):
    fake = FakeRun()
    r = _runner(tmp_path, fake)
    assert r.start() is True
    r.join(5)
    argv, kw = fake.calls[0]
    # The deadline is enforced as root (timeout(1) inside sudo): a
    # Python-side kill only reaches sudo, never the root bash under it.
    assert argv == ["sudo", "-n", "/usr/bin/timeout", "--kill-after=10",
                    str(box_health.RUN_TIMEOUT_S),
                    "/bin/bash", str(tmp_path / "scripts" / "verify_box.sh")]
    # Python's own timeout is only a backstop, strictly later than
    # timeout(1)'s SIGKILL.
    assert kw["timeout"] > box_health.RUN_TIMEOUT_S + box_health.KILL_AFTER_S
    r.start(with_services=True)
    r.join(5)
    argv2, kw2 = fake.calls[1]
    assert argv2[-1] == "--with-services"
    assert argv2[4] == str(box_health.RUN_TIMEOUT_WITH_SERVICES_S)
    assert kw2["timeout"] > box_health.RUN_TIMEOUT_WITH_SERVICES_S + box_health.KILL_AFTER_S


def test_runner_caches_result(tmp_path):
    r = _runner(tmp_path, FakeRun())
    r.start()
    r.join(5)
    st = r.status()
    assert st["running"] is False
    res = st["result"]
    assert res["failed"] == 2 and res["headline"] == "2 problems found"
    assert res["finished_at"].endswith("Z")
    cached = json.loads((tmp_path / "data" / box_health.CACHE_NAME).read_text())
    assert cached["failed"] == 2
    # A fresh runner (web restart) still answers from the cache.
    assert _runner(tmp_path, FakeRun()).status()["result"]["failed"] == 2


def test_runner_is_single_flight(tmp_path):
    fake = FakeRun(delay=0.5)
    r = _runner(tmp_path, fake)
    assert r.start() is True
    assert r.status()["running"] is True
    assert r.start() is False
    r.join(5)
    assert len(fake.calls) == 1
    assert r.start() is True
    r.join(5)


def test_runner_reports_sudo_refusal(tmp_path):
    r = _runner(tmp_path, FakeRun(stdout="", rc=1, stderr="sudo: a password is required\n"))
    r.start()
    r.join(5)
    res = r.status()["result"]
    assert "sudo" in res["error"]
    assert res["headline"] == "The health check could not run"


def test_runner_handles_timeout_and_missing_script(tmp_path):
    exc = subprocess.TimeoutExpired(["x"], 180, output="== Platform ==\n  [PASS] board: x\n")
    r = _runner(tmp_path, FakeRun(exc=exc))
    r.start()
    r.join(5)
    res = r.status()["result"]
    assert "did not finish" in res["error"] and res["passed"] == 1

    r2 = HealthRunner(script=tmp_path / "nope.sh", cache_path=tmp_path / "c.json", run=FakeRun())
    r2.start()
    r2.join(5)
    assert "not installed" in r2.status()["result"]["error"]


def test_runner_reports_root_side_timeout(tmp_path):
    # timeout(1) ended the run: 124 (SIGTERM was enough) or 137 (SIGKILL).
    for rc in (124, 137):
        r = _runner(tmp_path, FakeRun(stdout="== Platform ==\n  [PASS] board: x\n", rc=rc))
        r.start()
        r.join(5)
        res = r.status()["result"]
        assert "did not finish within 3 minutes" in res["error"], rc
        assert res["passed"] == 1 and res["exit_code"] is None
        assert res["headline"] == "The health check could not run"


def test_runner_redacts_output(tmp_path):
    key = "0123456789abcdef0123456789abcdef"
    out = f"== Service stack ==\n  [FAIL] verify_services.sh failed\n         X-Api-Key: {key}\n"
    r = _runner(tmp_path, FakeRun(stdout=out))
    r.start()
    r.join(5)
    assert key not in json.dumps(r.status()["result"])


# -------------------------------------------------------------- endpoints

def test_endpoints(app, client, temp_data_dir):
    fake = FakeRun()
    runner = HealthRunner(script=temp_data_dir.parent / "scripts" / "verify_box.sh",
                          cache_path=temp_data_dir / box_health.CACHE_NAME, run=fake)
    (temp_data_dir.parent / "scripts" / "verify_box.sh").write_text("#!/bin/bash\n")
    app.config["HEALTH_RUNNER"] = runner

    rv = client.get("/admin/health/status")
    assert rv.status_code == 200
    assert rv.get_json()["data"]["result"] is None

    rv = client.post("/admin/health/run", json={})
    assert rv.status_code == 200
    runner.join(5)
    data = client.get("/admin/health/status").get_json()["data"]
    assert data["running"] is False
    assert data["result"]["failed"] == 2
    assert data["services_check_available"] is False


def test_with_services_requires_media_browser_unlock(app, client, temp_data_dir):
    fake = FakeRun()
    app.config["HEALTH_RUNNER"] = HealthRunner(
        script=temp_data_dir / "v.sh", cache_path=temp_data_dir / "c.json", run=fake)
    with patch("admin._media_browser_unlocked", return_value=False):
        rv = client.post("/admin/health/run", json={"with_services": True})
    assert rv.status_code == 403
    assert fake.calls == []
    with patch("admin._media_browser_unlocked", return_value=True):
        rv = client.post("/admin/health/run", json={"with_services": True})
    assert rv.status_code == 200


def test_run_requires_csrf(temp_data_dir):
    from admin import create_app
    os.environ.pop("MAGIC_DISABLE_CSRF", None)
    a = create_app(temp_data_dir)
    rv = a.test_client().post("/admin/health/run", json={})
    assert rv.status_code == 403
