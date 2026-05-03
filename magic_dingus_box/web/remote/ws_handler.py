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


def handle_connection(ws, *, uinput_writer, data_dir: Path,
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
            elif t == "hello":
                pass  # already handshook
    finally:
        if broadcaster is not None and hasattr(broadcaster, "remove_queue"):
            broadcaster.remove_queue(conn.queue)
        _remove(conn)
