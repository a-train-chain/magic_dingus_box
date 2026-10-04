"""Detached maintenance jobs (OTA install/rollback, Media Browser setup).

These used to be Popen(start_new_session=True) children of Flask, which does
NOT survive a restart of magic-dingus-web.service: systemd's default
KillMode=control-group kills every process in the unit's cgroup, and the unit
is restarted from inside these very jobs (setup_services.sh restarts it; the
unit is Restart=always). On systemd they now run as their own transient unit,
and their progress is derived from an on-disk log so a restarted Flask can
still report it.
"""
from __future__ import annotations

import os
import signal
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import detached_jobs as dj  # noqa: E402
from detached_jobs import DetachedJobs  # noqa: E402


def _wait(jobs: DetachedJobs, job_id: str, timeout: float = 10.0):
    deadline = time.time() + timeout
    lines, offset = [], 0
    while time.time() < deadline:
        new, offset, state, rc = jobs.read(job_id, offset)
        lines += new
        if state != "running":
            return lines, state, rc
        time.sleep(0.05)
    pytest.fail("job did not finish")


def _script(tmp_path: Path, body: str) -> Path:
    p = tmp_path / "job.sh"
    p.write_text("#!/bin/bash\n" + body)
    p.chmod(0o755)
    return p


@pytest.fixture
def popen_jobs(tmp_path):
    return DetachedJobs(tmp_path / "state", mode_resolver=lambda: "popen")


def test_output_and_exit_code_come_from_the_log(popen_jobs, tmp_path):
    script = _script(tmp_path, 'echo one\necho two >&2\nexit 3\n')
    job_id = popen_jobs.launch("ota-install", [str(script)])
    lines, state, rc = _wait(popen_jobs, job_id)
    assert state == "exited"
    assert rc == 3
    assert "one" in lines and "two" in lines  # stderr is merged
    assert not any(dj.EXIT_MARKER in l for l in lines)


def test_a_restarted_flask_can_still_report_a_job(tmp_path):
    """A NEW DetachedJobs over the same state dir (= Flask after a restart,
    with no in-memory state at all) sees running and finished jobs."""
    first = DetachedJobs(tmp_path / "state", mode_resolver=lambda: "popen")
    script = _script(tmp_path, 'echo started\nsleep 0.6\necho done\n')
    job_id = first.launch("ota-install", [str(script)])

    second = DetachedJobs(tmp_path / "state", mode_resolver=lambda: "popen")
    assert second.active(["ota-install"]) == (job_id, "ota-install")
    lines, state, rc = _wait(second, job_id)
    assert (state, rc) == ("exited", 0)
    assert lines[:1] == ["started"] and "done" in lines
    assert second.active(["ota-install"]) is None


def test_a_killed_job_is_lost_not_running_forever(popen_jobs, tmp_path):
    script = _script(tmp_path, 'echo hi\nsleep 30\n')
    job_id = popen_jobs.launch("mb-setup", [str(script)])
    time.sleep(0.2)
    pid = popen_jobs.meta(job_id)["pid"]
    os.killpg(pid, signal.SIGKILL)  # what a cgroup kill does to the job
    lines, state, rc = _wait(popen_jobs, job_id)
    assert state == "lost" and rc is None


def test_unknown_or_malformed_ids_are_none(popen_jobs):
    assert popen_jobs.read("0" * 32) is None
    assert popen_jobs.read("../../etc/passwd") is None
    assert popen_jobs.meta("nonexistent-job-id") is None


def test_systemd_launch_gets_its_own_unit(tmp_path, monkeypatch):
    calls = []

    def fake_run(cmd, **kw):
        calls.append(cmd)

        class R:
            returncode = 0
            stdout = stderr = ""
        return R()

    monkeypatch.setattr(dj.shutil, "which", lambda name: "/usr/bin/" + name)
    real_isdir = os.path.isdir
    monkeypatch.setattr(dj.os.path, "isdir",
                        lambda p: True if p == "/run/systemd/system" else real_isdir(p))
    monkeypatch.setattr(dj.subprocess, "run", fake_run)
    monkeypatch.setenv("MAGIC_SKIP_BUILD", "true")
    jobs = DetachedJobs(tmp_path / "state", mode_resolver=lambda: "auto")

    job_id = jobs.launch("ota-install", ["/opt/x/update.sh", "install", "1.0.8", "u"])
    cmd = calls[-1]
    assert cmd[:3] == ["sudo", "-n", "systemd-run"]
    assert f"--unit=mdb-ota-install-{job_id[:12]}" in cmd
    assert "--collect" in cmd
    assert f"--uid={os.getuid()}" in cmd  # OTA runs as the web user, as before
    log = jobs.log_path(job_id)
    assert f"--property=StandardOutput=append:{log}" in cmd
    assert "--setenv=MAGIC_SKIP_BUILD=true" in cmd
    assert any(c.startswith("--setenv=HOME=") for c in cmd)
    assert cmd[-4:] == ["/opt/x/update.sh", "install", "1.0.8", "u"]
    assert jobs.meta(job_id)["unit"] == f"mdb-ota-install-{job_id[:12]}"

    root_id = jobs.launch("mb-setup", ["/opt/x/setup_services.sh"], as_root=True)
    cmd = calls[-1]
    assert not any(c.startswith("--uid=") for c in cmd)
    assert "--setenv=HOME=/root" in cmd
    assert not any(c.startswith("--setenv=MAGIC_") for c in cmd)  # sudo never passed these
    assert jobs.meta(root_id)["unit"].startswith("mdb-mb-setup-")


def test_systemd_run_refusal_falls_back_to_popen(tmp_path, monkeypatch):
    real_run = dj.subprocess.run

    def fake_run(cmd, **kw):
        if cmd[:3] == ["sudo", "-n", "systemd-run"]:
            class R:
                returncode = 1
                stdout = ""
                stderr = "sudo: a password is required"
            return R()
        return real_run(cmd, **kw)

    monkeypatch.setattr(dj.shutil, "which", lambda name: "/usr/bin/" + name)
    real_isdir = os.path.isdir
    monkeypatch.setattr(dj.os.path, "isdir",
                        lambda p: True if p == "/run/systemd/system" else real_isdir(p))
    monkeypatch.setattr(dj.subprocess, "run", fake_run)
    jobs = DetachedJobs(tmp_path / "state", mode_resolver=lambda: "auto")
    script = _script(tmp_path, 'echo ran\n')
    job_id = jobs.launch("ota-install", [str(script)])
    lines, state, rc = _wait(jobs, job_id)
    assert (state, rc) == ("exited", 0)
    assert "ran" in lines
    assert any("systemd-run unavailable" in l for l in lines)
    assert "pid" in jobs.meta(job_id)


def test_state_dir_is_outside_the_install_tree(tmp_path, monkeypatch):
    """The OTA's rsync --delete would erase a record kept under the data dir
    (or the web unit's TMPDIR, which is also inside /opt/magic_dingus_box)
    while the job is still writing it."""
    monkeypatch.delenv("MAGIC_JOB_STATE_DIR", raising=False)
    data_dir = tmp_path / "opt" / "magic_dingus_box" / "magic_dingus_box_cpp" / "data"
    d = dj.default_state_dir(data_dir)
    assert not str(d).startswith(str(tmp_path))
    assert dj.default_state_dir(data_dir) == d  # stable across restarts
    assert dj.default_state_dir(tmp_path / "other") != d
