"""Content Manager launcher: gunicorn when the box has it, Werkzeug otherwise.

WHY: the web admin ran on Werkzeug's development server — one thread per
request with no upper bound, no worker supervision, and a server its own
authors say not to use in production. gunicorn's threaded ("gthread")
worker is the production server flask-sock documents for the Phone Remote's
WebSocket, so that is what we run when it is installed.

THE FALLBACK IS LOAD-BEARING. systemd unit files are never refreshed by an
OTA (see OTA_UPDATE_GUARANTEES.md), so every fielded box keeps launching
`python3 -m magic_dingus_box.web.wsgi` forever — this module is reached
through that same command (wsgi.py's __main__ hands over to main() here).
A box that took the OTA before python3-gunicorn got installed, or whose
apt step failed offline, must still bring the Content Manager up, so a
missing (or broken) gunicorn degrades to the exact server it ran before.
`MAGIC_WEB_SERVER=werkzeug` forces the old server for debugging.

Design constraints, each checked against the code rather than assumed:

* ONE worker process. Much of the app's state is per-process: the CSRF
  token table, the transcode/update job registries, the Network Doctor
  single-flight, the pairing lock that bounds the 5-guess budget, the list
  of live Phone Remote connections, and — decisive — the /dev/uinput
  virtual gamepad. Two workers would mean two gamepads (the kiosk binds
  whichever it scanned first) and CSRF tokens that only work half the time.
* Threads, not processes, carry concurrency. Each connected phone holds one
  thread for the life of its WebSocket, so the pool must comfortably exceed
  "phones in the house + stale sockets awaiting the 25 s ping teardown +
  uploads + the page's own polling". 32 costs a few hundred KB of RSS on a
  1.5 GB Pi 4B (threads are created on demand) and is still a bound, which
  the Werkzeug server never had. Idle keep-alive connections do NOT hold a
  thread in gthread — they wait in the worker's selector.
* gthread's --timeout is a HEARTBEAT, not a request deadline: the worker's
  main loop notifies the arbiter about once a second regardless of what its
  request threads are doing, so a 40-minute multi-GB upload or a WebSocket
  open all evening is never killed by it. It only fires if the worker's
  main loop itself wedges. Generous (120 s) because a kill takes the
  in-process transcode/job registries with it, and a Pi 4B under ffmpeg
  load can starve the main loop for seconds.
* create_app() runs exactly once per serving process: in the WORKER (the
  arbiter never imports admin.py), and not twice through wsgi.py's module
  body. Its startup work — sweeping upload_temp, opening /dev/uinput —
  must not run in a process that then forks or execs.
"""
from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path

DEFAULT_DATA_DIR = "/opt/magic_dingus_box/magic_dingus_box_cpp/data"
DEFAULT_BIND = "0.0.0.0:5000"
DEFAULT_THREADS = 32
MIN_THREADS = 4
MAX_THREADS = 128
WORKER_HEARTBEAT_TIMEOUT_S = 120
GRACEFUL_TIMEOUT_S = 10
KEEPALIVE_S = 5


def data_dir() -> Path:
    return Path(os.getenv("MAGIC_DATA_DIR", DEFAULT_DATA_DIR))


def build_app():
    """Construct the Flask app. Called once per serving process."""
    try:
        from magic_dingus_box.web.admin import create_app
    except ImportError:  # flat import (tests put web/ on sys.path)
        from admin import create_app  # type: ignore[no-redef]
    app = create_app(data_dir())
    print(f"[web] Content Manager app built in pid {os.getpid()}", flush=True)
    return app


def _gunicorn_usable(find_spec=importlib.util.find_spec) -> bool:
    try:
        return (find_spec("gunicorn") is not None
                and find_spec("gunicorn.workers.gthread") is not None
                and find_spec("gunicorn.app.base") is not None)
    except (ImportError, ValueError):
        return False


def choose_server(env=None, find_spec=importlib.util.find_spec) -> str:
    """Return "gunicorn" or "werkzeug".

    MAGIC_WEB_SERVER=werkzeug forces the old server; =gunicorn (or unset)
    prefers gunicorn but still falls back when it is not importable — a
    typo'd override must never leave a box without its Content Manager.
    """
    env = os.environ if env is None else env
    forced = (env.get("MAGIC_WEB_SERVER") or "").strip().lower()
    if forced == "werkzeug":
        return "werkzeug"
    return "gunicorn" if _gunicorn_usable(find_spec) else "werkzeug"


def _int_env(env, name: str, default: int, lo: int, hi: int) -> int:
    try:
        value = int(str(env.get(name, "")).strip())
    except ValueError:
        return default
    return max(lo, min(hi, value))


def _heartbeat_dir(isdir=os.path.isdir, access=os.access):
    """gunicorn's worker heartbeat touches a temp file every loop (~1 s).
    The web unit points TMPDIR at the SD card, so without this that is a
    metadata write to flash every second, forever. /dev/shm is RAM."""
    if isdir("/dev/shm") and access("/dev/shm", os.W_OK):
        return "/dev/shm"
    return None


def gunicorn_options(env=None) -> dict:
    env = os.environ if env is None else env
    opts = {
        "bind": [env.get("MAGIC_WEB_BIND") or DEFAULT_BIND],
        "workers": 1,                  # see module docstring — never > 1
        "worker_class": "gthread",
        "threads": _int_env(env, "MAGIC_WEB_THREADS", DEFAULT_THREADS,
                            MIN_THREADS, MAX_THREADS),
        "timeout": WORKER_HEARTBEAT_TIMEOUT_S,
        "graceful_timeout": GRACEFUL_TIMEOUT_S,
        "keepalive": KEEPALIVE_S,
        # Never recycle the worker: it owns the uinput gamepad and every
        # in-flight job's state.
        "max_requests": 0,
        "preload_app": False,
        # Query strings on this app are short, but a long ?path= on a media
        # route should not 414 where Werkzeug accepted it.
        "limit_request_line": 8190,
        "accesslog": None,             # Werkzeug's per-request lines were
        "errorlog": "-",               # pure journal noise (1 Hz polls)
        "loglevel": "info",
        "proc_name": "magic-dingus-web",
    }
    hb = _heartbeat_dir()
    if hb:
        opts["worker_tmp_dir"] = hb
    return opts


def _run_gunicorn(options: dict) -> None:
    from gunicorn.app.base import BaseApplication

    class _ContentManager(BaseApplication):
        def __init__(self, opts):
            self._opts = opts
            super().__init__()

        def load_config(self):
            for key, value in self._opts.items():
                if key in self.cfg.settings and value is not None:
                    self.cfg.set(key, value)

        def load(self):
            # Runs in the WORKER after fork, so create_app's startup work
            # (upload_temp sweep, /dev/uinput open, status broadcaster
            # thread) happens once, in the process that serves requests.
            return build_app()

    _ContentManager(options).run()


def _run_werkzeug(env=None) -> None:
    env = os.environ if env is None else env
    bind = env.get("MAGIC_WEB_BIND") or DEFAULT_BIND
    host, _, port = bind.rpartition(":")
    app = build_app()
    # threaded=True is REQUIRED, not an optimization: the Phone Remote's
    # WebSocket route (flask-sock) runs a receive loop for the whole life of
    # the connection; with Werkzeug's default single worker one connected
    # phone hung every other request until it disconnected.
    app.run(host=host or "0.0.0.0", port=int(port or 5000), threaded=True)


def main(argv=None) -> int:
    server = choose_server()
    if server == "gunicorn":
        print("[web] serving with gunicorn (gthread, 1 worker)", flush=True)
        try:
            _run_gunicorn(gunicorn_options())
            return 0
        except ImportError as e:
            # find_spec saw the package but importing it failed (a broken
            # or half-installed gunicorn). Keep the Content Manager up.
            print(f"[web] gunicorn unusable ({e}); falling back to Werkzeug",
                  file=sys.stderr, flush=True)
    else:
        print("[web] gunicorn not installed — serving with the Werkzeug "
              "server (install python3-gunicorn)", flush=True)
    _run_werkzeug()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
