"""Smoke test that the bare WS endpoint accepts press messages and routes
them to the uinput writer. Auth is added in Phase C."""
import pytest
from admin import create_app
from remote.uinput_writer import UinputWriter, EV_KEY, BTN_SOUTH


@pytest.fixture
def app(tmp_path):
    app = create_app(data_dir=tmp_path)
    app.config["TESTING"] = True
    return app


def test_press_via_debug_endpoint_writes_to_uinput(app):
    captured = []
    class FakeDev:
        def write(self, t, c, v): captured.append((t, c, v))
        def syn(self): captured.append(("SYN",))

    fake_writer = UinputWriter(device=FakeDev())
    app.config["UINPUT_WRITER"] = fake_writer

    client = app.test_client()
    rv = client.post("/admin/remote/_debug/press?btn=OK&phase=tap")
    assert rv.status_code == 200
    # OK = BTN_SOUTH; tap = down then up
    assert (EV_KEY, BTN_SOUTH, 1) in captured
    assert (EV_KEY, BTN_SOUTH, 0) in captured


def test_press_with_unknown_button_returns_error(app):
    fake_writer = UinputWriter(device=type("F", (), {"write": lambda *a, **k: None, "syn": lambda *a, **k: None})())
    app.config["UINPUT_WRITER"] = fake_writer

    client = app.test_client()
    rv = client.post("/admin/remote/_debug/press?btn=BOGUS&phase=tap")
    assert rv.status_code == 400
