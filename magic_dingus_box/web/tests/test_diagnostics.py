"""Diagnostics bundle: what goes in, and — above all — what never does."""
import io
import json
import zipfile
from pathlib import Path
from unittest.mock import patch

import pytest

import diagnostics
from diagnostics import BundleBuilder, known_secret_values, scrub_kiosk_status

WG_KEY = "yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk="
QBIT_PW = "Zq8vRk2mWp4xYt7nLs3c"
ARR_KEY = "0123456789abcdef0123456789abcdef"
SONARR_KEY = "fedcba9876543210fedcba9876543210"
TMDB_KEY = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
FLASK = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
WIFI_PSK = "correct-horse-battery-staple"
TYPED = "MyWifiPass99"

ALL_SECRETS = [WG_KEY, QBIT_PW, ARR_KEY, SONARR_KEY, TMDB_KEY, FLASK, WIFI_PSK, TYPED]


@pytest.fixture
def box(tmp_path):
    install = tmp_path / "opt"
    data = install / "magic_dingus_box_cpp" / "data"
    data.mkdir(parents=True)
    (install / "VERSION").write_text("1.9.20\n")
    (install / "services").mkdir()
    (install / "services" / ".env").write_text("\n".join([
        f"WIREGUARD_PRIVATE_KEY={WG_KEY}",
        f"QBITTORRENT_ADMIN_PASSWORD={QBIT_PW}",
        f"MDB_QBIT_PASS={QBIT_PW}",
        f"RADARR_API_KEY={ARR_KEY}",
        f"SONARR_API_KEY={SONARR_KEY}",
        "TZ=America/New_York",
    ]) + "\n")
    (data / "flask_secret.key").write_text(FLASK)
    (data / "paired_remotes.json").write_text(json.dumps(
        {"devices": [{"id": "dev1", "token_salt": "s" * 32}]}))
    (data / "kiosk_status.json").write_text(json.dumps({
        "ts": 1, "screen": "settings",
        "text_input": {"active": True, "title": "Wi-Fi password", "buffer": TYPED},
    }))
    (data / "box_health_last.json").write_text(json.dumps({"failed": 0, "headline": "ok"}))
    (data / "pairing_audit.log").write_text("2026-10-01 wrong_code 123456 192.168.1.20\n")
    home = tmp_path / "home"
    home.mkdir()
    (home / "retroarch_launcher.log").write_text(
        "Launching mupen64plus_next on /data/roms/n64/mario.z64\n"
        f"env TMDB_API_KEY={TMDB_KEY}\n")
    (home / "retroarch_launcher.log.1").write_text("older run\n")
    tmdb = tmp_path / "tmdb_api_key"
    tmdb.write_text(TMDB_KEY + "\n")
    return {"install": install, "data": data, "home": home, "tmdb": tmdb}


JOURNAL = "\n".join([
    "Oct 01 kiosk[812]: Platform: Raspberry Pi 5 Model B Rev 1.0",
    f"Oct 01 kiosk[812]: qbit: logging in as admin with {QBIT_PW}",
    f"Oct 01 kiosk[812]: GET http://localhost:7878/api/v3/queue?apikey={ARR_KEY} -> 200",
    f"Oct 01 kiosk[812]: curl -H 'X-Api-Key: {SONARR_KEY}' http://localhost:8989/api/v3/series",
    f"Oct 01 web[90]: WIREGUARD_PRIVATE_KEY={WG_KEY}",
    f"Oct 01 kiosk[812]: nmcli dev wifi connect HomeNet password {WIFI_PSK}",
    f"Oct 01 kiosk[812]: tmdb https://api.themoviedb.org/3/movie/550?api_key={TMDB_KEY}",
    f"Oct 01 web[90]: secret loaded {FLASK}",
    "Oct 01 kiosk[812]: Final Display Mode: 1920x1080",
]) + "\n"


class FakeRun:
    def __init__(self):
        self.calls = []

    def __call__(self, argv, timeout=None):
        self.calls.append(list(argv))
        if "journalctl" in argv:
            return 0, JOURNAL
        if "docker" in argv:
            return 0, "NAMES IMAGE STATUS\nmdb_radarr hotio/radarr Up 2 hours\n"
        return 0, f"output of {' '.join(argv)}\n"


def _build(box, run=None, **kw):
    builder = BundleBuilder(
        data_dir=box["data"], install_dir=box["install"],
        known_secrets=known_secret_values(box["install"], box["data"], [box["tmdb"]]),
        run=run or FakeRun(), which=lambda name: "/usr/bin/docker",
        home=box["home"], proc_root=box["install"] / "noproc", **kw)
    out = box["install"] / "bundle.zip"
    builder.build(out)
    return zipfile.ZipFile(out), builder


def test_bundle_contains_what_support_needs(box):
    zf, _ = _build(box)
    names = set(zf.namelist())
    for expected in ("README.txt", "system.txt", "systemctl_status.txt",
                     "journal/magic-dingus-box-cpp.txt", "journal/magic-dingus-web.txt",
                     "journal/magic-dingus-audio.txt", "journal/kernel.txt",
                     "box_health_last.json", "kiosk_status.json",
                     "logs/retroarch_launcher.log", "logs/retroarch_launcher.log.1",
                     "logs/pairing_audit.log", "docker_ps.txt"):
        assert expected in names, expected
    system = zf.read("system.txt").decode()
    assert "version: 1.9.20" in system
    journal = zf.read("journal/magic-dingus-box-cpp.txt").decode()
    assert "Platform: Raspberry Pi 5" in journal
    assert "Final Display Mode: 1920x1080" in journal
    assert "-> 200" in journal  # the diagnosis survives the redaction


def test_bundle_never_contains_secrets(box):
    zf, _ = _build(box)
    for name in zf.namelist():
        assert Path(name).name not in diagnostics.FORBIDDEN_NAMES, name
        body = zf.read(name).decode("utf-8", "replace")
        for secret in ALL_SECRETS:
            assert secret not in body, (name, secret)
    assert not any(n.endswith((".env", "flask_secret.key", "paired_remotes.json"))
                   for n in zf.namelist())


def test_vpn_event_log_is_bundled_and_redacted(box, tmp_path):
    # gluetun_cascade_restart.sh's tunnel event log: support's first
    # question about stalled downloads is "how often does the VPN drop?".
    # It holds only timestamps + fixed words, but every bundled file gets a
    # redaction test — so plant secrets in it and prove they don't survive.
    vpn = tmp_path / "vpn_events.log"
    vpn.write_text("\n".join([
        "1800000000 watch",
        "1800000600 unhealthy portfwd",
        "1800000900 healthy",
        f"1800001000 restart WIREGUARD_PRIVATE_KEY={WG_KEY}",
        f"1800001100 unhealthy tunnel {QBIT_PW}",
    ]) + "\n")
    zf, _ = _build(box, vpn_events_path=vpn)
    body = zf.read("logs/vpn_events.log").decode()
    assert "1800000600 unhealthy portfwd" in body
    assert "1800000900 healthy" in body
    for secret in ALL_SECRETS:
        assert secret not in body, secret


def test_missing_vpn_event_log_is_skipped(box, tmp_path):
    zf, _ = _build(box, vpn_events_path=tmp_path / "absent.log")
    assert "logs/vpn_events.log" not in zf.namelist()


def test_kiosk_status_buffer_is_scrubbed():
    out = json.loads(scrub_kiosk_status(json.dumps(
        {"text_input": {"active": True, "buffer": TYPED}, "screen": "x"})))
    assert out["text_input"]["buffer"] == "[REDACTED]"
    assert out["screen"] == "x"
    assert scrub_kiosk_status("{not json").startswith("[kiosk_status.json")


def test_commands_are_fixed_and_journal_uses_sudo_n(box):
    run = FakeRun()
    _build(box, run=run)
    journal_calls = [c for c in run.calls if "journalctl" in c]
    assert journal_calls and all(c[:2] == ["sudo", "-n"] for c in journal_calls)
    assert ["sudo", "-n", "journalctl", "-u", "magic-dingus-box-cpp.service", "-n", "2000",
            "--no-pager", "-o", "short-iso"] in run.calls


def test_journal_falls_back_without_sudo(box):
    calls = []

    def run(argv, timeout=None):
        calls.append(argv)
        if argv[:2] == ["sudo", "-n"]:
            return 1, "sudo: a password is required\n"
        return 0, "plain journal line\n"
    zf, _ = _build(box, run=run)
    assert "plain journal line" in zf.read("journal/magic-dingus-web.txt").decode()


def test_forbidden_file_is_refused(box):
    builder = BundleBuilder(data_dir=box["data"], install_dir=box["install"],
                            run=FakeRun(), home=box["home"],
                            extra_files={"oops.txt": box["install"] / "services" / ".env"})
    with pytest.raises(ValueError):
        builder.build(box["install"] / "x.zip")


def test_caps(box, monkeypatch):
    monkeypatch.setattr(diagnostics, "FILE_CAP_BYTES", 1000)
    monkeypatch.setattr(diagnostics, "TOTAL_CAP_BYTES", 6000)
    (box["home"] / "retroarch_launcher.log").write_text("line of launcher log\n" * 5000)
    zf, builder = _build(box)
    for info in zf.infolist():
        if info.filename != "README.txt":   # the manifest itself, uncapped
            assert info.file_size <= 1100, info.filename
    total = sum(i.file_size for i in zf.infolist() if i.filename != "README.txt")
    assert total <= 6000
    assert any("SKIPPED" in m for m in builder.manifest)


def test_endpoint_streams_zip_and_cleans_up(app, client, temp_data_dir):
    with patch.object(diagnostics, "_default_run", lambda argv, timeout=25: (0, "ok\n")):
        rv = client.get("/admin/diagnostics/bundle")
    assert rv.status_code == 200
    assert rv.mimetype == "application/zip"
    assert "diagnostics_" in rv.headers["Content-Disposition"]
    zf = zipfile.ZipFile(io.BytesIO(rv.data))
    assert "README.txt" in zf.namelist()
    rv.close()
    leftovers = [p for p in (temp_data_dir / "upload_temp").iterdir()
                 if p.name.startswith("mdb_diag_")]
    assert leftovers == []


def test_endpoint_single_flight(app, client):
    import threading
    gate = threading.Event()
    real = diagnostics.build_to_tempfile

    def slow(builder, tmp_dir=None):
        gate.wait(5)
        return real(builder, tmp_dir)
    results = {}
    with patch.object(diagnostics, "build_to_tempfile", slow), \
         patch.object(diagnostics.BundleBuilder, "build",
                      lambda self, out: zipfile.ZipFile(out, "w").close() or out):
        t = threading.Thread(target=lambda: results.setdefault(
            "a", app.test_client().get("/admin/diagnostics/bundle").status_code))
        t.start()
        import time
        time.sleep(0.2)
        results["b"] = client.get("/admin/diagnostics/bundle").status_code
        gate.set()
        t.join(5)
    assert results == {"a": 200, "b": 429}
