"""Zero-friction hardening of the web admin (no auth added — owner decision).

One test group per fix:
  1. Host-header allowlist (DNS rebinding)
  2. delete_videos containment
  3. emulator_core validation on every write path
  4. /admin/upload extension allowlist
  5. per-board transcode concurrency
  6. multi-disc M3U generator path
  7. package extraction capped by free space
  8. pairing attempt counter under concurrency
  9. one Network Doctor run at a time
 10. ws_handler ignores malformed input
"""
from __future__ import annotations

import io
import json
import threading
import time
import zipfile
from pathlib import Path

import pytest
import yaml

import admin
from admin import (
    _host_is_allowed,
    _invalid_emulator_core,
    _max_transcodes_for,
    create_app,
)


# ── 1. Host allowlist ────────────────────────────────────────────────────────

@pytest.mark.parametrize("host", [
    "localhost", "localhost:5000", "127.0.0.1", "192.168.1.42:5000",
    "10.55.0.1", "[::1]:5000", "[fe80::1]", "magicpi-ab12.local",
    "magicpi-ab12.local:5000", "magicpi-ab12", "dingus.box", "dingus.box:5000",
    "DINGUS.BOX.", "magicpi-ab12.lan", "box.home.arpa", "",
])
def test_host_allowlist_accepts_lan_names(host):
    assert _host_is_allowed(host)


@pytest.mark.parametrize("host", [
    "evil.com", "evil.com:5000", "rebind.attacker.net", "dingus.box.evil.com",
    "magicpi.local.evil.com",
])
def test_host_allowlist_rejects_foreign_names(host):
    assert not _host_is_allowed(host)


def test_host_allowlist_own_hostname_and_env_override():
    assert _host_is_allowed("mybox.example.org", own_names={"mybox.example.org"})
    assert _host_is_allowed("kiosk.corp.example", extra=(".corp.example",))
    assert _host_is_allowed("exact.example", extra=("exact.example",))
    assert not _host_is_allowed("other.example", extra=("exact.example",))


@pytest.mark.parametrize("host", [
    "magicpi-ab12.fritz.box", "MagicPi-AB12.Fritz.Box:5000",
    "magicpi-ab12.attlocal.net", "magicpi-ab12.router", "magicpi-ab12.fritz.box.",
    # Already accepted for any first label (not publicly delegated).
    "magicpi-ab12.lan", "magicpi-ab12.home", "magicpi-ab12.localdomain",
])
def test_host_allowlist_accepts_own_name_under_router_suffix(host):
    assert _host_is_allowed(host, own_label="magicpi-ab12")


@pytest.mark.parametrize("host", [
    # The attacker controls the suffix: own hostname + arbitrary domain.
    "magicpi-ab12.evil.com", "magicpi-ab12.box", "magicpi-ab12.net",
    # Router suffixes sit under publicly registered domains: only THIS
    # box's name is accepted under them.
    "magicpi-ffff.fritz.box", "other.attlocal.net", "x.router",
    # Suffix must match whole labels, exactly.
    "magicpi-ab12.evil.fritz.box", "magicpi-ab12.notfritz.box",
    "magicpi-ab12.fritz.box.evil.com", "magicpi-ab12x.fritz.box",
    "fritz.box", "attlocal.net",
])
def test_host_allowlist_router_suffix_requires_own_name(host):
    assert not _host_is_allowed(host, own_label="magicpi-ab12")


def test_host_allowlist_router_suffix_off_without_own_label():
    assert not _host_is_allowed("magicpi-ab12.fritz.box")
    assert not _host_is_allowed(".fritz.box", own_label="")


def test_router_suffixed_own_name_passes_end_to_end(temp_data_dir, monkeypatch):
    monkeypatch.setattr(admin.socket, "gethostname", lambda: "magicpi-ab12")
    c = create_app(temp_data_dir).test_client()
    ok = c.get("/admin/csrf-token", headers={"Host": "magicpi-ab12.fritz.box:5000"})
    assert ok.status_code == 200
    bad = c.get("/admin/csrf-token", headers={"Host": "magicpi-ab12.evil.com"})
    assert bad.status_code == 403


def test_foreign_host_header_gets_403(client):
    rv = client.get("/admin/csrf-token", headers={"Host": "evil.com"})
    assert rv.status_code == 403
    assert rv.get_json()["error"]["code"] == "FORBIDDEN_HOST"


def test_lan_host_headers_pass(client):
    for host in ("localhost", "192.168.1.5:5000", "magicpi-ab12.local:5000",
                 "dingus.box"):
        rv = client.get("/admin/csrf-token", headers={"Host": host})
        assert rv.status_code == 200, host


def test_magic_allowed_hosts_env(temp_data_dir, monkeypatch):
    monkeypatch.setenv("MAGIC_ALLOWED_HOSTS", "box.example.com, .mine.test")
    app = create_app(temp_data_dir)
    c = app.test_client()
    assert c.get("/admin/csrf-token", headers={"Host": "box.example.com"}).status_code == 200
    assert c.get("/admin/csrf-token", headers={"Host": "a.mine.test"}).status_code == 200
    assert c.get("/admin/csrf-token", headers={"Host": "evil.com"}).status_code == 403


# ── 2. delete_videos containment ─────────────────────────────────────────────

def test_delete_videos_cannot_escape_media_dir(client, temp_data_dir):
    victim = temp_data_dir / "flask_secret.key"
    assert victim.exists()  # created by create_app
    config_victim = temp_data_dir / "settings_victim.json"
    config_victim.write_text("{}")
    real_video = temp_data_dir / "media" / "clip.mp4"
    real_video.write_bytes(b"v")
    (temp_data_dir / "media" / "sub").mkdir()

    playlist = {
        "title": "Evil",
        "items": [
            {"title": "a", "source_type": "local",
             "path": "media/sub/../../flask_secret.key"},
            {"title": "b", "source_type": "local",
             "path": "media/sub/../../settings_victim.json"},
            {"title": "c", "source_type": "local", "path": "media/clip.mp4"},
        ],
    }
    (temp_data_dir / "playlists" / "evil.yaml").write_text(yaml.safe_dump(playlist))

    rv = client.delete("/admin/playlists/evil.yaml?delete_videos=true")
    assert rv.status_code == 200
    assert victim.exists()
    assert config_victim.exists()
    assert not real_video.exists()  # the legitimate video is still deleted


# ── 3. emulator_core validation ──────────────────────────────────────────────

def _game_playlist(core):
    return {"title": "Games", "items": [
        {"title": "g", "source_type": "emulated_game",
         "path": "data/roms/n64/g.z64", "emulator_core": core}]}


@pytest.mark.parametrize("core", ["mupen64plus_next_libretro", "auto",
                                  "pcsx_rearmed_libretro", "", None])
def test_valid_emulator_cores_accepted(core):
    assert _invalid_emulator_core(_game_playlist(core)) is None


@pytest.mark.parametrize("core", ["../../tmp/evil", "a/b", "x.so", "core name",
                                  "a\nb", 5, ["x"]])
def test_invalid_emulator_cores_detected(core):
    assert _invalid_emulator_core(_game_playlist(core)) is not None


def test_put_playlist_rejects_traversal_core_json(client, temp_data_dir):
    rv = client.post("/admin/playlists/games.yaml",
                     json=_game_playlist("../../../tmp/evil"))
    assert rv.status_code == 400
    assert not (temp_data_dir / "playlists" / "games.yaml").exists()


def test_put_playlist_rejects_traversal_core_raw_yaml(client, temp_data_dir):
    rv = client.post("/admin/playlists/games.yaml",
                     data=yaml.safe_dump(_game_playlist("../evil")),
                     content_type="text/yaml")
    assert rv.status_code == 400
    assert not (temp_data_dir / "playlists" / "games.yaml").exists()


def test_put_playlist_accepts_real_core(client, temp_data_dir):
    rv = client.post("/admin/playlists/games.yaml",
                     json=_game_playlist("mupen64plus_next_libretro"))
    assert rv.status_code == 200


def test_import_playlist_rejects_traversal_core(client, temp_data_dir):
    body = yaml.safe_dump(_game_playlist("../../evil")).encode()
    rv = client.post("/admin/playlists/import",
                     data={"file": (io.BytesIO(body), "games.yaml")},
                     content_type="multipart/form-data")
    assert rv.status_code == 400
    assert not list((temp_data_dir / "playlists").glob("*.yaml"))


def test_import_package_rejects_traversal_core(client, temp_data_dir):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("playlist.yaml", yaml.safe_dump(_game_playlist("../../evil")))
        zf.writestr("roms/n64/g.z64", b"rom")
    buf.seek(0)
    rv = client.post("/admin/playlists/import-package",
                     data={"file": (buf, "p.zip")},
                     content_type="multipart/form-data")
    assert rv.status_code == 400
    assert not (temp_data_dir / "roms" / "n64" / "g.z64").exists()
    assert not list((temp_data_dir / "playlists").glob("*.yaml"))


def test_restore_skips_playlist_with_traversal_core(client, temp_data_dir):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("playlists/evil.yaml", yaml.safe_dump(_game_playlist("../x")))
        zf.writestr("playlists/good.yaml",
                    yaml.safe_dump(_game_playlist("nestopia_libretro")))
    buf.seek(0)
    client.post("/admin/restore", data={"file": (buf, "backup.zip")},
                content_type="multipart/form-data")
    assert not (temp_data_dir / "playlists" / "evil.yaml").exists()
    assert (temp_data_dir / "playlists" / "good.yaml").exists()


# ── 4. /admin/upload extension allowlist ─────────────────────────────────────

@pytest.mark.parametrize("name", ["evil.sh", "x.html", "lib.so", "noext"])
def test_upload_rejects_non_video(client, temp_data_dir, name):
    rv = client.post("/admin/upload",
                     data={"file": (io.BytesIO(b"x"), name)},
                     content_type="multipart/form-data")
    assert rv.status_code == 400
    assert not (temp_data_dir / "media" / name).exists()


@pytest.mark.parametrize("name", ["clip.mp4", "clip.MKV", "a.webm", "b.mov", "c.avi"])
def test_upload_accepts_video(client, temp_data_dir, name):
    rv = client.post("/admin/upload",
                     data={"file": (io.BytesIO(b"x"), name)},
                     content_type="multipart/form-data")
    assert rv.status_code == 200
    assert (temp_data_dir / "media" / name).exists()


# ── 5. transcode concurrency ─────────────────────────────────────────────────

def test_max_transcodes_per_board():
    assert _max_transcodes_for("pi4", None) == 1
    assert _max_transcodes_for("unknown", None) == 1
    assert _max_transcodes_for("pi5", None) == 2
    assert _max_transcodes_for("pi4", "3") == 3
    assert _max_transcodes_for("pi5", "1") == 1
    assert _max_transcodes_for("pi4", "junk") == 1
    assert _max_transcodes_for("pi4", "0") == 1


def test_app_uses_single_transcode_off_pi5(app, monkeypatch):
    # Dev machine / CI: no Pi device-tree → 'unknown' → Pi 4B envelope.
    assert app.config["MAX_TRANSCODES"] == 1


# ── 6. M3U generator path ────────────────────────────────────────────────────

def test_m3u_generator_runs_after_ps1_upload(client, temp_data_dir):
    marker = temp_data_dir / "m3u_ran"
    script = temp_data_dir.parent / "scripts" / "generate_m3u_playlists.sh"
    script.write_text(f'#!/bin/bash\necho "$1" > "{marker}"\n')
    script.chmod(0o644)  # runs via bash, so the mode bit must not matter
    rv = client.post("/admin/upload/rom/ps1",
                     data={"file": (io.BytesIO(b"cue"), "Game (Disc 1).cue")},
                     content_type="multipart/form-data")
    assert rv.status_code == 200
    deadline = time.monotonic() + 5
    while not marker.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    assert marker.exists(), "generator never ran — script path does not resolve"
    assert marker.read_text().strip().endswith("roms/ps1")


# ── 7. package extraction capped by free space ───────────────────────────────

def test_package_extract_capped_by_free_space(client, temp_data_dir, monkeypatch):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("playlist.yaml", yaml.safe_dump({
            "title": "Bomb", "items": [
                {"title": "v", "source_type": "local", "path": "media/big.mp4"}]}))
        # 2 MB of zeros compresses to almost nothing: a tiny ZIP, and the
        # upload-size precondition cannot see it coming.
        zf.writestr("media/big.mp4", b"\0" * (2 * 1024 * 1024))
    buf.seek(0)
    # 512 MB reserve + 1 MB usable → the 2 MB entry must not fit.
    monkeypatch.setattr(admin, "get_free_bytes",
                        lambda _p: 512 * 1024 * 1024 + 1024 * 1024)
    rv = client.post("/admin/playlists/import-package",
                     data={"file": (buf, "p.zip")},
                     content_type="multipart/form-data")
    assert rv.status_code == 507, rv.get_json()
    media = temp_data_dir / "media"
    assert not (media / "big.mp4").exists()
    assert not list(media.glob(".*.part"))
    assert not list((temp_data_dir / "playlists").glob("*.yaml"))


# ── 8. pairing attempts under concurrency ────────────────────────────────────

def test_concurrent_wrong_codes_each_decrement(tmp_path, monkeypatch):
    from remote import auth as remote_auth

    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    app.config["SECRET_KEY"] = "k"
    session = tmp_path / "pairing_session.json"
    session.write_text(json.dumps({
        "schema": 1, "code": "847291", "issued_at": int(time.time()),
        "expires_at": int(time.time()) + 120, "attempts_remaining": 50,
        "nonce": "n"}))

    # Widen the read→write window so an unlocked implementation reliably
    # loses updates.
    real_loads = json.loads

    class _SlowJson:
        def __getattr__(self, name):
            return getattr(json, name)

        @staticmethod
        def loads(*a, **kw):
            out = real_loads(*a, **kw)
            time.sleep(0.02)
            return out

    monkeypatch.setattr(remote_auth, "json", _SlowJson())

    n = 10
    barrier = threading.Barrier(n)
    codes = []

    def worker():
        c = app.test_client()
        barrier.wait()
        codes.append(c.get("/?pair=000000").status_code)

    threads = [threading.Thread(target=worker) for _ in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(10)
    assert codes == [401] * n
    assert real_loads(session.read_text())["attempts_remaining"] == 50 - n


# ── 9. Network Doctor: one run at a time ─────────────────────────────────────

def test_network_doctor_single_flight(client, temp_data_dir):
    counter = temp_data_dir / "doctor_runs"
    doctor = temp_data_dir.parent / "scripts" / "network_doctor.sh"
    doctor.write_text(
        f'#!/bin/bash\necho x >> "{counter}"\nsleep 0.5\n'
        'echo \'{"ok": true, "verdict": "fine", "checks": []}\'\n')

    results = []

    def hit():
        results.append(client.get("/admin/network/doctor"))

    threads = [threading.Thread(target=hit) for _ in range(4)]
    for t in threads:
        t.start()
        time.sleep(0.02)
    for t in threads:
        t.join(15)

    assert len(counter.read_text().splitlines()) == 1
    assert [r.status_code for r in results] == [200] * 4
    assert all(r.get_json()["verdict"] == "fine" for r in results)


def test_network_doctor_runs_again_after_completion(client, temp_data_dir):
    counter = temp_data_dir / "doctor_runs"
    doctor = temp_data_dir.parent / "scripts" / "network_doctor.sh"
    doctor.write_text(f'#!/bin/bash\necho x >> "{counter}"\necho \'{{"ok": true}}\'\n')
    assert client.get("/admin/network/doctor").status_code == 200
    assert client.get("/admin/network/doctor").status_code == 200
    assert len(counter.read_text().splitlines()) == 2


# ── 10. ws_handler robustness ────────────────────────────────────────────────

def _drive_ws(tmp_path, messages, uinput_writer):
    """Run handle_connection over raw frames; return (sent, thread_alive)."""
    import hashlib
    import hmac
    import queue
    from remote import auth as remote_auth
    from remote import ws_handler

    app = create_app(data_dir=tmp_path)
    app.config["SECRET_KEY"] = "test-key"
    (tmp_path / "paired_remotes.json").write_text(json.dumps({
        "schema": 1, "devices": [{"id": "d1", "nickname": "D", "paired_at": 1,
                                  "last_seen": 1}]}))
    ts = int(time.time())
    sig = hmac.new(b"test-key", f"d1|{ts}".encode(), hashlib.sha256).hexdigest()
    cookie = f"d1.{ts}.{sig}"

    inq: queue.Queue = queue.Queue()
    sent: list = []
    done = object()

    class WS:
        def receive(self, timeout=None):
            try:
                item = inq.get(timeout=timeout)
            except queue.Empty:
                return None
            if item is done:
                raise ConnectionError("closed")
            return item

        def send(self, data):
            sent.append(json.loads(data))

        def close(self):
            pass

    class Text:
        def type_char(self, *a, **k): pass
        def key_special(self, *a, **k): pass
        def clear(self, *a, **k): pass

    errors = []

    def run():
        with app.test_request_context("/admin/remote/ws",
                                      headers={"Cookie": f"mdb_remote={cookie}"}):
            try:
                ws_handler.handle_connection(
                    WS(), uinput_writer=uinput_writer, text_input_writer=Text(),
                    data_dir=Path(tmp_path), verify_cookie=remote_auth.verify_cookie)
            except ConnectionError:
                pass
            except Exception as e:  # anything else = the socket died on input
                errors.append(e)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    for m in messages:
        inq.put(m)
    # A trailing valid seek proves the loop survived everything before it.
    inq.put(json.dumps({"t": "seek", "pos": 0.5}))
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and not any(
            s.get("of") == "seek" for s in sent):
        time.sleep(0.02)
    inq.put(done)
    t.join(2)
    return sent, errors


def test_ws_ignores_malformed_frames(tmp_path):
    from remote.uinput_writer import UinputWriter
    dev = type("D", (), {"write": lambda s, *a: None, "syn": lambda s: None})()
    msgs = [
        json.dumps([1, 2, 3]), json.dumps("str"), json.dumps(42), "null",
        json.dumps({"t": "seek", "pos": "abc"}),
        json.dumps({"t": "seek", "pos": None}),
        json.dumps({"t": "seek", "pos": [1]}),
        json.dumps({"t": "seek", "pos": "nan"}),
        json.dumps({"t": "press", "btn": 5}),
        json.dumps({"t": "press", "btn": ["a"]}),
        json.dumps({"t": "press", "btn": "a", "phase": {"x": 1}}),
    ]
    sent, errors = _drive_ws(tmp_path, msgs, UinputWriter(device=dev))
    assert errors == []
    assert any(s.get("of") == "seek" for s in sent)
    seek = json.loads((tmp_path / "seek_request.json").read_text())
    assert seek["pos"] == 0.5


def test_ws_press_without_uinput_reports_error(tmp_path):
    msgs = [json.dumps({"t": "press", "btn": "OK", "phase": "tap"})]
    sent, errors = _drive_ws(tmp_path, msgs, None)
    assert errors == []
    assert any(s.get("code") == "uinput_unavailable" for s in sent)
    assert any(s.get("of") == "seek" for s in sent)
