import json
import os
import socket
import tempfile
import threading
import time

from magic_dingus_box.player.mpv_client import MpvClient


def _run_fake_mpv_socket(path: str, stop_event: threading.Event):
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(1)
    srv.settimeout(0.2)
    try:
        while not stop_event.is_set():
            try:
                conn, _ = srv.accept()
            except Exception:
                continue
            with conn:
                conn.settimeout(0.2)
                buff = b""
                while not stop_event.is_set():
                    try:
                        chunk = conn.recv(4096)
                    except Exception:
                        break
                    if not chunk:
                        break
                    buff += chunk
                    while b"\n" in buff:
                        line, buff = buff.split(b"\n", 1)
                        if not line:
                            continue
                        req = json.loads(line.decode("utf-8"))
                        rid = req.get("request_id", 0)
                        cmd = req.get("command", [])
                        if cmd[:1] == ["get_property"] and len(cmd) >= 2:
                            resp = {"error": "success", "request_id": rid, "data": 123.0}
                            conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))
                        else:
                            # Ack others without data
                            resp = {"error": "success", "request_id": rid}
                            conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))
    finally:
        try:
            srv.close()
        finally:
            if os.path.exists(path):
                os.unlink(path)


def test_mpv_client_get_property():
    with tempfile.TemporaryDirectory() as td:
        sock_path = os.path.join(td, "mpv.sock")
        stop = threading.Event()
        t = threading.Thread(target=_run_fake_mpv_socket, args=(sock_path, stop), daemon=True)
        t.start()
        try:
            client = MpvClient(sock_path)
            # send a property request and expect 123.0 from fake server
            val = client.get_property("time-pos")
            assert val == 123.0
        finally:
            stop.set()
            t.join(timeout=1.0)

