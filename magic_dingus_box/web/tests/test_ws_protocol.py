"""Tests for the Phone Remote _debug/press HTTP endpoint and WS endpoint.
Covers auth gating added in the pre-deploy review."""
import json
import queue
import threading
import time
import hmac as _hmac
import hashlib
import pytest
from admin import create_app
from remote.uinput_writer import UinputWriter, EV_KEY, BTN_SOUTH
from remote import auth as remote_auth
from remote import ws_handler


def _make_paired_cookie(tmp_path, secret: str = "test-key") -> str:
    """Seed a paired device and return a valid mdb_remote cookie value."""
    paired = tmp_path / "paired_remotes.json"
    device_id = "test-device-abc"
    paired.write_text(json.dumps({
        "schema": 1,
        "devices": [{"id": device_id, "nickname": "Test", "paired_at": 1, "last_seen": 1}],
    }))
    issued_at = int(time.time())
    sig = _hmac.new(secret.encode(), f"{device_id}|{issued_at}".encode(), hashlib.sha256).hexdigest()
    return f"{device_id}.{issued_at}.{sig}"


@pytest.fixture
def app(tmp_path):
    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    app.config["SECRET_KEY"] = "test-key"
    return app


# ── WS fixture infrastructure ────────────────────────────────────────────────

class FakeTextInputWriter:
    """Captures TextInputWriter calls for verification.
    Each entry: (method, payload_or_None, device_id)."""
    def __init__(self):
        self.calls = []

    def type_char(self, c, *, device_id):
        self.calls.append(("type_char", c, device_id))

    def key_special(self, k, *, device_id):
        self.calls.append(("key_special", k, device_id))

    def clear(self, *, device_id):
        self.calls.append(("clear", None, device_id))


class _FakeWS:
    """Fake WebSocket that drives handle_connection in a background thread."""

    def __init__(self):
        self._in: queue.Queue = queue.Queue()   # messages to feed into the handler
        self._out: queue.Queue = queue.Queue()  # messages the handler sends out
        self._closed = threading.Event()

    # ── called by handle_connection ───────────────────────────────────
    def receive(self, timeout=None):
        try:
            item = self._in.get(timeout=timeout)
            if item is _CLOSE_SENTINEL:
                self._closed.set()
                return None
            return item
        except queue.Empty:
            return None

    def send(self, data):
        self._out.put(data)

    def close(self):
        self._closed.set()

    # ── called by test code ───────────────────────────────────────────
    def push(self, msg: dict):
        self._in.put(json.dumps(msg))

    def drain(self, timeout=0.2) -> list:
        msgs = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                msgs.append(json.loads(self._out.get(timeout=0.05)))
            except queue.Empty:
                break
        return msgs

    def shutdown(self):
        self._in.put(_CLOSE_SENTINEL)


_CLOSE_SENTINEL = object()


class _WsTestApp:
    """Wraps a running handle_connection thread. Exposes send_recv() and writers."""

    def __init__(self, fake_ws, uinput_writer, text_input_writer):
        self._ws = fake_ws
        self.uinput_writer = uinput_writer
        self.text_input_writer = text_input_writer

    def send_recv(self, msg: dict, timeout: float = 0.3) -> list:
        """Push one WS message; collect any responses within *timeout*."""
        self._ws.push(msg)
        return self._ws.drain(timeout=timeout)

    def close(self):
        self._ws.shutdown()


def _make_paired_cookie_dev1(tmp_path, secret: str = "test-key") -> str:
    """Seed device 'dev1' and return a valid mdb_remote cookie."""
    from pathlib import Path
    paired = Path(tmp_path) / "paired_remotes.json"
    device_id = "dev1"
    paired.write_text(json.dumps({
        "schema": 1,
        "devices": [{"id": device_id, "nickname": "Dev1", "paired_at": 1, "last_seen": 1}],
    }))
    issued_at = int(time.time())
    sig = _hmac.new(secret.encode(), f"{device_id}|{issued_at}".encode(), hashlib.sha256).hexdigest()
    return f"{device_id}.{issued_at}.{sig}"


@pytest.fixture
def ws_test_app(tmp_path):
    """Fixture: drive handle_connection in a thread with fakes injected."""
    from pathlib import Path

    # Build a minimal Flask app context so handle_connection can call
    # current_app.config.get("STATUS_BROADCASTER") without crashing.
    flask_app = create_app(data_dir=tmp_path)
    flask_app.config["TESTING"] = True
    flask_app.config["SECRET_KEY"] = "test-key"

    cookie_value = _make_paired_cookie_dev1(tmp_path)

    fake_ws = _FakeWS()
    fake_uinput = UinputWriter(
        device=type("FakeDev", (), {
            "write": lambda self, t, c, v: None,
            "syn":   lambda self: None,
        })()
    )
    fake_text = FakeTextInputWriter()

    def _run():
        with flask_app.test_request_context(
            "/admin/remote/ws",
            headers={"Cookie": f"mdb_remote={cookie_value}"},
        ):
            ws_handler.handle_connection(
                fake_ws,
                uinput_writer=fake_uinput,
                text_input_writer=fake_text,
                data_dir=Path(tmp_path),
                verify_cookie=remote_auth.verify_cookie,
            )

    t = threading.Thread(target=_run, daemon=True)
    t.start()

    # Wait for hello_ack to confirm the handler is running and authed
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        try:
            msg = json.loads(fake_ws._out.get(timeout=0.1))
            if msg.get("t") == "hello_ack":
                break
        except queue.Empty:
            continue

    wrapper = _WsTestApp(fake_ws, fake_uinput, fake_text)
    yield wrapper
    wrapper.close()
    t.join(timeout=1.0)


def test_press_via_debug_endpoint_writes_to_uinput(app, tmp_path):
    captured = []
    class FakeDev:
        def write(self, t, c, v): captured.append((t, c, v))
        def syn(self): captured.append(("SYN",))

    fake_writer = UinputWriter(device=FakeDev())
    app.config["UINPUT_WRITER"] = fake_writer

    cookie_value = _make_paired_cookie(tmp_path)
    client = app.test_client()
    client.set_cookie(domain="localhost", key="mdb_remote", value=cookie_value)
    rv = client.post("/admin/remote/_debug/press?btn=OK&phase=tap")
    assert rv.status_code == 200
    # OK = BTN_SOUTH; tap = down then up
    assert (EV_KEY, BTN_SOUTH, 1) in captured
    assert (EV_KEY, BTN_SOUTH, 0) in captured


def test_press_with_unknown_button_returns_error(app, tmp_path):
    fake_writer = UinputWriter(device=type("F", (), {"write": lambda *a, **k: None, "syn": lambda *a, **k: None})())
    app.config["UINPUT_WRITER"] = fake_writer

    cookie_value = _make_paired_cookie(tmp_path)
    client = app.test_client()
    client.set_cookie(domain="localhost", key="mdb_remote", value=cookie_value)
    rv = client.post("/admin/remote/_debug/press?btn=BOGUS&phase=tap")
    assert rv.status_code == 400


def test_unauthenticated_debug_press_returns_401(app):
    """No cookie → 401; verifies the auth gate added in pre-deploy review."""
    client = app.test_client()
    rv = client.post("/admin/remote/_debug/press?btn=OK&phase=tap")
    assert rv.status_code == 401


# ── WS text-input dispatch tests ─────────────────────────────────────────────

def test_ws_type_char_routes_to_text_input_writer(ws_test_app):
    """type_char message → TextInputWriter.type_char(c, device_id)"""
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "type_char", "c": "s"})
    assert fake_text.calls == [("type_char", "s", "dev1")]


def test_ws_key_special_routes(ws_test_app):
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "key_special", "k": "backspace"})
    ws_test_app.send_recv({"t": "key_special", "k": "enter"})
    ws_test_app.send_recv({"t": "key_special", "k": "cancel"})
    assert fake_text.calls == [
        ("key_special", "backspace", "dev1"),
        ("key_special", "enter", "dev1"),
        ("key_special", "cancel", "dev1"),
    ]


def test_ws_clear_routes(ws_test_app):
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "clear"})
    assert fake_text.calls == [("clear", None, "dev1")]


def test_ws_type_char_filters_non_ascii(ws_test_app):
    """Emoji / multi-byte chars dropped at WS edge — never reach writer."""
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "type_char", "c": "🍕"})
    ws_test_app.send_recv({"t": "type_char", "c": ""})
    ws_test_app.send_recv({"t": "type_char", "c": "ab"})
    assert fake_text.calls == []  # all three dropped


def test_ws_key_special_filters_unknown_key(ws_test_app):
    fake_text = ws_test_app.text_input_writer
    fake_text.calls.clear()
    ws_test_app.send_recv({"t": "key_special", "k": "nonsense"})
    assert fake_text.calls == []
