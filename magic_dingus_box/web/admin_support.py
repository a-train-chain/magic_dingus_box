"""Support tools that work on every box (no Media Browser gate except the
optional services sweep): the Network Doctor, Box health (verify_box.sh,
see box_health.py) and the redacted diagnostics bundle (diagnostics.py).
"""
from __future__ import annotations

import json
import subprocess
import threading

from flask import jsonify, request, send_file

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    import box_health
    import diagnostics
    from admin_common import error_response, strip_ansi, success_response
    from admin_tmdb import _tmdb_key_file
    from redact import Redactor
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from . import box_health
    from . import diagnostics
    from .admin_common import error_response, strip_ansi, success_response
    from .admin_tmdb import _tmdb_key_file
    from .redact import Redactor


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    data_dir = ctx.data_dir
    get_device_info = ctx.get_device_info
    require_csrf = ctx.require_csrf
    # Defined/re-exported in admin.py; tests monkeypatch admin.<name>, so
    # these are shims that look the name up in admin at call time.
    _media_browser_locked_response = ctx._media_browser_locked_response
    _media_browser_unlocked = ctx._media_browser_unlocked

    # One Network Doctor run at a time. Each run is a ~20-45 s probe ladder
    # (curl, DNS, ping) and every GET used to spawn its own — a few open tabs,
    # a double-click, or anyone on the LAN looping the URL stacked them up on
    # a Pi 4B that has no CPU to spare. A request that arrives while a run is
    # in flight waits for THAT run and returns its result (same answer, one
    # set of probes); if it can't get one it gets a 429.
    _doctor_lock = threading.Lock()
    _doctor_state: dict = {"seq": 0, "result": None}
    DOCTOR_WAIT_SECONDS = 60

    @app.get("/admin/network/doctor")
    def network_doctor():  # type: ignore[no-redef]
        if _doctor_lock.acquire(blocking=False):
            try:
                resp, status = _run_network_doctor()
                try:
                    body = resp.get_json()
                except Exception:
                    body = None
                _doctor_state["result"] = (body, status) if body is not None else None
                _doctor_state["seq"] += 1
                return resp, status
            finally:
                _doctor_lock.release()

        seq_at_arrival = _doctor_state["seq"]
        if _doctor_lock.acquire(timeout=DOCTOR_WAIT_SECONDS):
            _doctor_lock.release()
            result = _doctor_state["result"]
            if _doctor_state["seq"] != seq_at_arrival and result is not None:
                return jsonify(result[0]), result[1]
        resp, status = error_response(
            "BUSY", "A network test is already running — try again in a moment.",
            status=429)
        resp.headers["Retry-After"] = "30"
        return resp, status

    def _run_network_doctor():
        """Run the Network Doctor ladder and return its verdict.

        Deliberately NOT behind the Media Browser gate: network problems hit
        games-only boxes and unprovisioned units too, and this endpoint is
        most valuable precisely when nothing else works. Read-only probes,
        bounded at ~20s by the script itself; the subprocess timeout is the
        backstop. Runs synchronously — the UI shows a spinner for the
        duration; concurrent requests share one run (see _doctor_lock).
        """
        doctor = data_dir.parent / "scripts" / "network_doctor.sh"
        if not doctor.exists():
            return error_response("NOT_AVAILABLE", "network_doctor.sh not found", status=500)
        try:
            # Invoked via bash rather than executed directly: the script
            # shipped mode 0644 in the v1.9.6/v1.9.7 tarballs, which turned
            # the owner's only network diagnostic into a 500 reading
            # "[Errno 13] Permission denied: .../network_doctor.sh". The
            # mode is fixed in git as of v1.9.8; this makes it stop
            # mattering.
            result = subprocess.run(
                ["/bin/bash", str(doctor), "--json"],
                capture_output=True, text=True, timeout=45
            )
            # The script exits 1 when any rung failed — that is a RESULT,
            # not an error; the JSON on stdout is valid either way.
            payload = json.loads(result.stdout)
            return jsonify(payload), 200
        except subprocess.TimeoutExpired:
            return error_response("TIMEOUT", "Network test timed out", status=504)
        except json.JSONDecodeError:
            return error_response(
                "PARSE_ERROR",
                strip_ansi(result.stderr)[:300] or "Doctor produced no parseable output",
                status=500)
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)

    # ===== BOX HEALTH (verify_box.sh) + DIAGNOSTICS BUNDLE =====
    #
    # Box health runs the box's own pre-ship acceptance test and shows its
    # verdict in plain language — the same answer a technician gets over
    # SSH, without SSH. The diagnostics bundle is the zip support asks for
    # first. Neither is behind the Media Browser gate (games-only boxes
    # need support too); only the optional --with-services sweep is, since
    # it inspects the Movies stack. See box_health.py / diagnostics.py.
    install_dir = data_dir.parent.parent

    def _known_secrets() -> list:
        return diagnostics.known_secret_values(
            install_dir, data_dir, extra_paths=[_tmdb_key_file()])

    health_runner = box_health.HealthRunner(
        script=data_dir.parent / "scripts" / "verify_box.sh",
        cache_path=data_dir / box_health.CACHE_NAME,
        redactor_factory=lambda: Redactor(_known_secrets()),
    )
    app.config["HEALTH_RUNNER"] = health_runner

    @app.post("/admin/health/run")
    @require_csrf
    def health_run():  # type: ignore[no-redef]
        body = request.get_json(silent=True) or {}
        with_services = body.get("with_services") is True
        if with_services and not _media_browser_unlocked():
            return _media_browser_locked_response()
        runner = app.config["HEALTH_RUNNER"]
        started = runner.start(with_services=with_services)
        data = runner.status()
        data["already_running"] = not started
        return success_response(
            data=data,
            message="Health check started" if started else "A health check is already running")

    @app.get("/admin/health/status")
    def health_status():  # type: ignore[no-redef]
        data = app.config["HEALTH_RUNNER"].status()
        data["services_check_available"] = _media_browser_unlocked()
        return success_response(data=data)

    _diag_lock = threading.Lock()

    @app.route("/admin/diagnostics/bundle", methods=["GET", "POST"])
    def diagnostics_bundle():  # type: ignore[no-redef]
        # Read-only, but it shells out ~25 times; one build at a time.
        if not _diag_lock.acquire(blocking=False):
            resp, status = error_response(
                "BUSY", "A diagnostics file is already being prepared — try "
                "again in a moment.", status=429)
            resp.headers["Retry-After"] = "15"
            return resp, status
        try:
            builder = diagnostics.BundleBuilder(
                data_dir=data_dir, install_dir=install_dir,
                known_secrets=_known_secrets())
            handle = diagnostics.build_to_tempfile(builder)
        except Exception as e:
            return error_response(
                "INTERNAL_ERROR", f"Could not build the diagnostics file: {type(e).__name__}",
                status=500)
        finally:
            _diag_lock.release()
        name = diagnostics.bundle_filename(get_device_info().get("device_name", "box"))
        return send_file(handle, mimetype="application/zip", as_attachment=True,
                         download_name=name, max_age=0)
