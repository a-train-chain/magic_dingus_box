"""WebSocket protocol for the phone remote.

Each connected phone gets one Connection. The handler:
  - Verifies the auth cookie on connect (else closes).
  - Reads incoming JSON messages: press, seek, hello.
  - Forwards press → uinput. Forwards seek → seek_request.json.
  - Listens for status broadcasts (status_broadcaster registers per-conn queues — that arrives in Phase E).
"""
from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from queue import Queue, Empty
from threading import Lock
from typing import Callable, List, Optional

from flask import current_app, request


@dataclass
class Connection:
    sock: object              # flask-sock WebSocket
    device_id: str
    queue: "Queue[dict]" = field(default_factory=lambda: Queue(maxsize=64))
    # Buttons/axes this connection has driven 'down' but not yet 'up'.
    # Drained on disconnect so a phone that drops mid-hold can't leave the
    # kiosk seeing a stuck input. Holds ButtonName string values.
    held: set = field(default_factory=set)


_lock = Lock()
_connections: List[Connection] = []


def all_connections() -> List[Connection]:
    with _lock:
        return list(_connections)


def _add(conn: Connection) -> None:
    with _lock:
        _connections.append(conn)


def _remove(conn: Connection) -> None:
    with _lock:
        try:
            _connections.remove(conn)
        except ValueError:
            pass


def handle_connection(ws, *, uinput_writer, text_input_writer, data_dir: Path,
                      verify_cookie: Callable[[str], Optional[str]]):
    """Drive a single connection until the socket closes."""
    cookie = request.cookies.get("mdb_remote", "")
    device_id = verify_cookie(cookie)
    if device_id is None:
        # Close the socket — the phone JS will see onclose and show
        # the "unpaired" UI.
        try:
            ws.close()
        except Exception:
            pass
        return

    conn = Connection(sock=ws, device_id=device_id)
    _add(conn)

    # Zero any input state a previously-dead connection of this (or any)
    # device may have left latched on the shared virtual gamepad, so a new
    # session always starts from a clean, all-released state.
    if uinput_writer is not None and hasattr(uinput_writer, "release_all"):
        try:
            uinput_writer.release_all()
        except Exception:
            pass

    # Phase E will hook the broadcaster here.
    broadcaster = current_app.config.get("STATUS_BROADCASTER")
    if broadcaster is not None and hasattr(broadcaster, "add_queue"):
        broadcaster.add_queue(conn.queue)

    try:
        ws.send(json.dumps({"t": "hello_ack", "schema": 1}))
        while True:
            # Drain outgoing queue (status pushes from the broadcaster, when
            # E.1 lands). Non-blocking.
            try:
                while True:
                    outgoing = conn.queue.get_nowait()
                    ws.send(json.dumps(outgoing))
            except Empty:
                pass

            raw = ws.receive(timeout=0.05)
            if raw is None:
                continue
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue

            t = msg.get("t")
            if t == "press":
                btn = msg.get("btn", "")
                phase = msg.get("phase", "tap")
                try:
                    uinput_writer.press(btn, phase=phase)
                    # Track hold state so a mid-hold disconnect can be
                    # released. 'tap' is self-balancing (down+up in one
                    # call), so it never leaves anything held.
                    if phase == "down":
                        conn.held.add(btn)
                    elif phase == "up":
                        conn.held.discard(btn)
                except ValueError:
                    ws.send(json.dumps({"t": "error",
                                        "code": "bad_button", "msg": btn}))
            elif t == "seek":
                pos = float(msg.get("pos", 0.0))
                pos = max(0.0, min(1.0, pos))
                seek_path = data_dir / "seek_request.json"
                tmp = data_dir / "seek_request.json.tmp"
                tmp.write_text(json.dumps({
                    "schema": 1, "pos": pos, "ts": time.time(),
                }))
                os.replace(tmp, seek_path)
                ws.send(json.dumps({"t": "ack", "of": "seek", "ok": True}))
            elif t == "type_char":
                c = msg.get("c", "")
                if isinstance(c, str) and len(c) == 1 and c.isprintable() and ord(c) < 0x80:
                    text_input_writer.type_char(c, device_id=device_id)

            elif t == "key_special":
                k = msg.get("k", "")
                if isinstance(k, str) and k in ("backspace", "enter", "cancel"):
                    text_input_writer.key_special(k, device_id=device_id)

            elif t == "clear":
                text_input_writer.clear(device_id=device_id)

            elif t == "hello":
                pass  # already handshook
    finally:
        # Release anything this phone was still holding when the socket
        # closed (clean close, timeout, or exception) so the kiosk never
        # keeps seeing a stuck button/axis. Per-control release first
        # (precise); release_all as a belt-and-braces sweep.
        if uinput_writer is not None:
            for btn in list(conn.held):
                try:
                    uinput_writer.press(btn, phase="up")
                except Exception:
                    pass
            conn.held.clear()
            if hasattr(uinput_writer, "release_all"):
                try:
                    uinput_writer.release_all()
                except Exception:
                    pass
        if broadcaster is not None and hasattr(broadcaster, "remove_queue"):
            broadcaster.remove_queue(conn.queue)
        _remove(conn)
