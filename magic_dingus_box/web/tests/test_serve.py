"""The production launcher (serve.py) and the real servers behind it.

Unit tests cover the server choice and options. The integration tests start
the launcher exactly the way magic-dingus-web.service does
(`python3 -m magic_dingus_box.web.wsgi`) and talk to it over real sockets:
the security hooks, the Phone Remote WebSocket (including a reconnect
storm), a streamed upload, and that create_app() ran exactly once.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

import serve

REPO_ROOT = Path(__file__).resolve().parents[3]


# ---------------------------------------------------------------- choice

def _spec(available):
    def find_spec(name):
        return object() if name.split(".")[0] in available else None
    return find_spec


def test_prefers_gunicorn_when_importable():
    assert serve.choose_server({}, _spec({"gunicorn"})) == "gunicorn"


def test_falls_back_to_werkzeug_without_gunicorn():
    assert serve.choose_server({}, _spec(set())) == "werkzeug"


def test_override_forces_werkzeug():
    env = {"MAGIC_WEB_SERVER": "werkzeug"}
    assert serve.choose_server(env, _spec({"gunicorn"})) == "werkzeug"


def test_override_gunicorn_still_falls_back_when_missing():
    # A forced/typo'd value must never leave a box without a Content Manager.
    for value in ("gunicorn", "GUNICORN", "bogus"):
        env = {"MAGIC_WEB_SERVER": value}
        assert serve.choose_server(env, _spec(set())) == "werkzeug"


def test_find_spec_errors_mean_unusable():
    def boom(name):
        raise ImportError("half-installed")
    assert serve.choose_server({}, boom) == "werkzeug"


# --------------------------------------------------------------- options

def test_single_worker_gthread():
    opts = serve.gunicorn_options({})
    # In-process state (uinput gamepad, CSRF tokens, job registries,
    # pairing lock, WS connection list) requires exactly one worker.
    assert opts["workers"] == 1
    assert opts["worker_class"] == "gthread"
    assert opts["threads"] == serve.DEFAULT_THREADS
    assert opts["max_requests"] == 0
    assert opts["preload_app"] is False
    assert opts["bind"] == ["0.0.0.0:5000"]
    assert opts["timeout"] >= 60


@pytest.mark.parametrize("raw,expected", [
    ("16", 16), ("1", serve.MIN_THREADS), ("100000", serve.MAX_THREADS),
    ("abc", serve.DEFAULT_THREADS), ("", serve.DEFAULT_THREADS),
])
def test_threads_override_is_clamped(raw, expected):
    assert serve.gunicorn_options({"MAGIC_WEB_THREADS": raw})["threads"] == expected


def test_heartbeat_dir_prefers_ram():
    assert serve._heartbeat_dir(lambda p: True, lambda p, m: True) == "/dev/shm"
    assert serve._heartbeat_dir(lambda p: False, lambda p, m: True) is None


def test_options_are_valid_gunicorn_settings():
    gunicorn_config = pytest.importorskip("gunicorn.config")
    cfg = gunicorn_config.Config()
    for key, value in serve.gunicorn_options({}).items():
        assert key in cfg.settings, key
        cfg.set(key, value)
    assert cfg.worker_class_str == "gthread"


# ------------------------------------------------------------ integration

def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


SECRET = "integration-test-secret"

# A stand-in python-evdev for the server subprocess, so the Phone Remote
# path runs end to end on a dev machine with no /dev/uinput. Every UInput()
# construction and every event is appended to FAKE_UINPUT_LOG, which is how
# the tests prove the virtual gamepad is opened exactly once.
_FAKE_EVDEV = """
import collections, os
AbsInfo = collections.namedtuple(
    "AbsInfo", "value min max fuzz flat resolution")
class ecodes:
    EV_KEY = 1
    EV_ABS = 3
def _log(line):
    with open(os.environ["FAKE_UINPUT_LOG"], "a") as f:
        f.write(line + "\\n")
class UInput:
    def __init__(self, caps, name="", phys=""):
        _log("open %d" % os.getpid())
    def write(self, t, c, v):
        _log("ev %d %d %d" % (t, c, v))
    def syn(self):
        pass
"""


@pytest.fixture
def live_server(tmp_path, request):
    server = request.param
    data = tmp_path / "data"
    data.mkdir()
    (data / "flask_secret.key").write_text(SECRET)
    (data / "upload_temp").mkdir()
    (data / "upload_temp" / "orphan.bin").write_bytes(b"x" * 4096)
    (data / "paired_remotes.json").write_text(json.dumps({
        "schema": 1,
        "devices": [{"id": "dev1", "nickname": "Phone", "user_agent": "t",
                     "paired_at": int(time.time()), "last_seen": int(time.time())}],
    }))
    fake = tmp_path / "fakemods" / "evdev"
    fake.mkdir(parents=True)
    (fake / "__init__.py").write_text(_FAKE_EVDEV)
    uinput_log = tmp_path / "uinput.log"
    port = _free_port()
    env = dict(os.environ)
    env.update({
        "MAGIC_DATA_DIR": str(data),
        "MAGIC_WEB_BIND": f"127.0.0.1:{port}",
        "MAGIC_WEB_SERVER": server,
        "MAGIC_JOB_STATE_DIR": str(tmp_path / "jobs"),
        "MAGIC_DISABLE_CSRF": "1",
        "PYTHONPATH": os.pathsep.join([str(tmp_path / "fakemods"), str(REPO_ROOT)]),
        "FAKE_UINPUT_LOG": str(uinput_log),
        "TMPDIR": str(tmp_path),
    })
    env.pop("FLASK_SECRET_KEY", None)
    log = tmp_path / "server.log"
    with open(log, "wb") as out:
        proc = subprocess.Popen(
            [sys.executable, "-u", "-m", "magic_dingus_box.web.wsgi"],
            cwd=str(REPO_ROOT), env=env, stdout=out, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 30
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail(f"server exited early:\n{log.read_text()}")
        try:
            _get(base, "/admin/health")
            break
        except (urllib.error.URLError, ConnectionError, OSError):
            time.sleep(0.2)
    else:
        proc.kill()
        pytest.fail(f"server never answered:\n{log.read_text()}")
    yield {"base": base, "port": port, "data": data, "log": log, "proc": proc,
           "uinput_log": uinput_log}
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()


def _get(base, path, headers=None):
    req = urllib.request.Request(base + path, headers={"Host": "localhost", **(headers or {})})
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status, r.read()


def _status(base, path, headers=None):
    try:
        return _get(base, path, headers)[0]
    except urllib.error.HTTPError as e:
        return e.code


def _cookie() -> str:
    issued = int(time.time())
    sig = hmac.new(SECRET.encode(), f"dev1|{issued}".encode(), hashlib.sha256).hexdigest()
    return f"mdb_remote=dev1.{issued}.{sig}"


def _needs(*mods):
    for m in mods:
        pytest.importorskip(m)


SERVERS = ["gunicorn", "werkzeug"]


@pytest.mark.parametrize("live_server", SERVERS, indirect=True)
def test_security_hooks_and_single_startup(live_server):
    base = live_server["base"]
    assert _status(base, "/admin/health") == 200
    # Host allowlist (DNS rebinding) still enforced by the app hook.
    assert _status(base, "/admin/health", {"Host": "evil.example"}) == 403
    # Sec-Fetch-Site cross-site fetch still refused.
    assert _status(base, "/admin/csrf-token", {"Sec-Fetch-Site": "cross-site"}) == 403
    log = live_server["log"].read_text()
    # create_app ran exactly once: one build line, one upload_temp sweep.
    assert log.count("Content Manager app built in pid") == 1, log
    assert log.count("[upload_temp] swept") == 1, log
    assert not (live_server["data"] / "upload_temp" / "orphan.bin").exists()
    opens = [l for l in live_server["uinput_log"].read_text().splitlines()
             if l.startswith("open")]
    assert len(opens) == 1, opens


@pytest.mark.parametrize("live_server", ["gunicorn"], indirect=True)
def test_launcher_runs_gunicorn(live_server):
    pytest.importorskip("gunicorn")
    log = live_server["log"].read_text()
    assert "serving with gunicorn" in log, log
    assert "Booting worker" in log, log


@pytest.mark.parametrize("live_server", SERVERS, indirect=True)
def test_phone_remote_ws_and_reconnect_storm(live_server):
    _needs("flask_sock", "simple_websocket")
    import simple_websocket

    url = f"ws://127.0.0.1:{live_server['port']}/admin/remote/ws"
    headers = {"Cookie": _cookie()}

    def roundtrip(ws):
        hello = json.loads(ws.receive(timeout=10))
        assert hello["t"] == "hello_ack"
        ws.send(json.dumps({"t": "seek", "pos": 0.5}))
        deadline = time.time() + 10
        while time.time() < deadline:
            msg = json.loads(ws.receive(timeout=10))
            if msg.get("t") == "ack":
                assert msg["of"] == "seek"
                return
        pytest.fail("no seek ack")

    # Many sequential reconnects — the phone does this on every wake/WiFi blip.
    for _ in range(25):
        ws = simple_websocket.Client.connect(url, headers=headers)
        roundtrip(ws)
        ws.close()

    # Several phones held open at once, and the Content Manager still answers.
    held = [simple_websocket.Client.connect(url, headers=headers) for _ in range(5)]
    for ws in held:
        roundtrip(ws)
    assert _status(live_server["base"], "/admin/health") == 200
    # A button press reaches the (one) virtual gamepad.
    held[0].send(json.dumps({"t": "press", "btn": "OK", "phase": "tap"}))
    for ws in held:
        ws.close()
    assert (live_server["data"] / "seek_request.json").exists()
    deadline = time.time() + 5
    while time.time() < deadline:
        events = live_server["uinput_log"].read_text()
        if "ev 1 304 1" in events:
            break
        time.sleep(0.1)
    assert "ev 1 304 1" in events and "ev 1 304 0" in events
    assert sum(1 for l in events.splitlines() if l.startswith("open")) == 1


@pytest.mark.parametrize("live_server", SERVERS, indirect=True)
def test_streamed_upload(live_server):
    """A large multipart body streamed in chunks reaches disk intact."""
    import http.client

    boundary = "----mdbtestboundary"
    size = 48 * 1024 * 1024
    head = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
            f"filename=\"big.mp4\"\r\nContent-Type: video/mp4\r\n\r\n").encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    conn = http.client.HTTPConnection("127.0.0.1", live_server["port"], timeout=120)
    conn.putrequest("POST", "/admin/upload", skip_host=True)
    conn.putheader("Host", "localhost")
    conn.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
    conn.putheader("Content-Length", str(len(head) + size + len(tail)))
    conn.endheaders()
    conn.send(head)
    chunk = b"\0" * (1024 * 1024)
    for _ in range(size // len(chunk)):
        conn.send(chunk)
    conn.send(tail)
    resp = conn.getresponse()
    body = resp.read()
    assert resp.status == 200, body
    out = live_server["data"] / "media" / "big.mp4"
    assert out.stat().st_size == size
