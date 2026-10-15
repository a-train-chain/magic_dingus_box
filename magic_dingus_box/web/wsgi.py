"""WSGI entry point for the Content Manager.

`python3 -m magic_dingus_box.web.wsgi` is what magic-dingus-web.service
runs on every box — and that command line must keep working forever: the
unit converge (converge_system_units.sh) keeps installed units current,
but an updater older than it never rewrites unit files, and a rollback run
by such an updater can leave ANY previously installed unit behind. So the
__main__ path here is the launcher: it hands over to serve.main(), which
runs gunicorn when installed and falls back to the Werkzeug server
otherwise.

The app object is built ONLY on import (e.g. `gunicorn ...wsgi:app` by
hand). Under __main__ it must not be: create_app() opens /dev/uinput and
sweeps upload_temp, and serve.main() builds the app itself in the process
that will serve it — building it here too would run that startup twice.
"""
try:
    from magic_dingus_box.web import serve
except ImportError:  # pragma: no cover - flat layout
    import serve  # type: ignore[no-redef]

DATA_DIR = serve.data_dir()

if __name__ == "__main__":
    raise SystemExit(serve.main())
else:
    app = serve.build_app()
