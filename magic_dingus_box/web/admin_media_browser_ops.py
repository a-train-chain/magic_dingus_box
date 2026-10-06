"""Media Browser day-2 routes: the health summary (Radarr / qBittorrent /
Gluetun), the TMDB key field, kiosk restart for a new key, service restart
and full reset.
"""
from __future__ import annotations

import json
import os
import subprocess
import tempfile

from flask import request

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import error_response, success_response
    from admin_tmdb import (
        _tmdb_classify_key, _tmdb_key_file, _tmdb_redact, KIOSK_SERVICE,
    )
    from admin_vpn_config import (
        _read_env_file, _vpn_supports_port_forwarding, EnvFileReadError,
    )
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import error_response, success_response
    from .admin_tmdb import (
        _tmdb_classify_key, _tmdb_key_file, _tmdb_redact, KIOSK_SERVICE,
    )
    from .admin_vpn_config import (
        _read_env_file, _vpn_supports_port_forwarding, EnvFileReadError,
    )


def _qbit_get(env: dict, path: str):
    """Authenticated GET against the local qBittorrent WebUI.

    Returns the response body text, or None on any failure (missing/placeholder
    credentials, login failure, curl error, or an empty body). Each caller
    parses the body it expects. Credentials are passed via stdin (`-d @-`),
    never argv, so the password never lands in /proc/<pid>/cmdline. Extracted
    from the identical login + cookie-jar + finally-unlink preamble that
    _qbit_torrent_summary and _qbit_listen_port each carried.
    """
    username = env.get("QBITTORRENT_ADMIN_USERNAME", "admin")
    password = env.get("QBITTORRENT_ADMIN_PASSWORD", "").strip()
    if not password or password.startswith("__"):
        return None
    cookie_jar = tempfile.NamedTemporaryFile(suffix=".cookies", delete=False)
    cookie_jar.close()
    try:
        login = subprocess.run(
            ["curl", "-sS", "--max-time", "5",
             "-c", cookie_jar.name,
             "-d", "@-",
             "http://localhost:8080/api/v2/auth/login"],
            input=f"username={username}&password={password}",
            capture_output=True, text=True, timeout=6,
        )
        if login.returncode != 0 or "Ok." not in (login.stdout or ""):
            return None
        resp = subprocess.run(
            ["curl", "-sS", "--max-time", "5",
             "-b", cookie_jar.name,
             f"http://localhost:8080/api/v2/{path}"],
            capture_output=True, text=True, timeout=6,
        )
        if resp.returncode != 0 or not resp.stdout.strip():
            return None
        return resp.stdout
    except Exception:
        return None
    finally:
        try:
            os.unlink(cookie_jar.name)
        except Exception:
            pass


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    SERVICES_DIR = ctx.SERVICES_DIR
    SERVICES_ENV = ctx.SERVICES_ENV
    _check_media_browser_gates = ctx._check_media_browser_gates
    require_csrf = ctx.require_csrf
    # Defined/re-exported in admin.py; tests monkeypatch admin.<name>, so
    # these are shims that look the name up in admin at call time.
    _atomic_write_text = ctx._atomic_write_text
    _kiosk_started_at = ctx._kiosk_started_at
    _require_nopasswd_sudo = ctx._require_nopasswd_sudo
    _tmdb_verify_key = ctx._tmdb_verify_key
    check_service_status = ctx.check_service_status

    def _radarr_library_count(env: dict) -> int:
        """Count movies in Radarr's library. Returns -1 on failure."""
        api_key = env.get("RADARR_API_KEY", "").strip()
        if not api_key or api_key.startswith("__"):
            return -1
        try:
            result = subprocess.run(
                ["curl", "-sS", "--max-time", "5",
                 "-H", f"X-Api-Key: {api_key}",
                 "http://localhost:7878/api/v3/movie"],
                capture_output=True, text=True, timeout=6,
            )
            if result.returncode != 0 or not result.stdout.strip():
                return -1
            payload = json.loads(result.stdout)
            return len(payload) if isinstance(payload, list) else -1
        except Exception:
            return -1

    def _radarr_queue_summary(env: dict) -> dict:
        """Return {count, active_dl_mbps}. Returns {-1, 0.0} on failure."""
        api_key = env.get("RADARR_API_KEY", "").strip()
        if not api_key or api_key.startswith("__"):
            return {"count": -1, "active_dl_mbps": 0.0}
        try:
            result = subprocess.run(
                ["curl", "-sS", "--max-time", "5",
                 "-H", f"X-Api-Key: {api_key}",
                 "http://localhost:7878/api/v3/queue?pageSize=50"],
                capture_output=True, text=True, timeout=6,
            )
            if result.returncode != 0 or not result.stdout.strip():
                return {"count": -1, "active_dl_mbps": 0.0}
            payload = json.loads(result.stdout)
            records = payload.get("records", []) if isinstance(payload, dict) else []
            # Radarr reports speed in bytes/sec under varying keys depending on
            # version — try the common ones, default to 0.
            total_bps = 0.0
            for r in records:
                for k in ("downloadRate", "downloadSpeed", "speed"):
                    if k in r and isinstance(r[k], (int, float)):
                        total_bps += float(r[k])
                        break
            mbps = round((total_bps * 8) / 1_000_000, 2)
            return {"count": len(records), "active_dl_mbps": mbps}
        except Exception:
            return {"count": -1, "active_dl_mbps": 0.0}

    def _qbit_torrent_summary(env: dict) -> dict:
        """Return {active, seeding} torrent counts. Returns {-1, -1} on failure."""
        body = _qbit_get(env, "torrents/info")
        if body is None:
            return {"active": -1, "seeding": -1}
        try:
            torrents = json.loads(body)
            if not isinstance(torrents, list):
                return {"active": -1, "seeding": -1}
            seeding_states = {"uploading", "stalledUP", "queuedUP", "forcedUP", "checkingUP"}
            seeding = sum(1 for t in torrents if t.get("state") in seeding_states)
            return {"active": len(torrents), "seeding": seeding}
        except Exception:
            return {"active": -1, "seeding": -1}

    def _qbit_listen_port(env: dict) -> int:
        """Return qBit's currently-configured listen_port, or -1 on failure."""
        body = _qbit_get(env, "app/preferences")
        if body is None:
            return -1
        try:
            payload = json.loads(body)
            return int(payload.get("listen_port", -1))
        except Exception:
            return -1

    def _gluetun_forwarded_port() -> int:
        """Return Gluetun's NAT-PMP forwarded port, 0 if unavailable, -1 on failure."""
        try:
            result = subprocess.run(
                ["docker", "exec", "mdb_gluetun", "wget", "-qO-",
                 "http://localhost:8000/v1/openvpn/portforwarded"],
                capture_output=True, text=True, timeout=5,
            )
            if result.returncode != 0 or not result.stdout.strip():
                # /v1/openvpn/portforwarded is the canonical endpoint; older
                # gluetun builds expose /v1/portforward instead. Try that as
                # a fallback before giving up.
                fallback = subprocess.run(
                    ["docker", "exec", "mdb_gluetun", "wget", "-qO-",
                     "http://localhost:8000/v1/portforward"],
                    capture_output=True, text=True, timeout=5,
                )
                if fallback.returncode != 0 or not fallback.stdout.strip():
                    return -1
                payload = json.loads(fallback.stdout)
            else:
                payload = json.loads(result.stdout)
            return int(payload.get("port", 0))
        except Exception:
            return -1

    @app.get("/admin/media-browser/health-summary")
    def media_browser_health_summary():  # type: ignore[no-redef]
        """Aggregate one-shot health snapshot for the State C dashboard.

        Manual-refresh only — no auto-poll on the frontend. Each external
        call has a 5-sec timeout and is wrapped in try/except; any failing
        field returns -1 / "unavailable" without breaking the others.
        """
        if (resp := _check_media_browser_gates()):
            return resp

        try:
            env = _read_env_file(SERVICES_ENV)
        except EnvFileReadError as e:
            return error_response("env_read_failed", str(e), status=500)
        library_count = _radarr_library_count(env)
        queue = _radarr_queue_summary(env)
        qbit = _qbit_torrent_summary(env)

        # Does this box's VPN forward a port AT ALL?
        #
        # Most providers do not, and gluetun only implements it for four of
        # them (of which only ProtonVPN has WireGuard servers). A customer on
        # Mullvad or a self-hosted tunnel has no forwarded port by design —
        # that is a normal, healthy configuration with somewhat slower peer
        # discovery, NOT a fault. Reporting it in red as "unavailable"
        # alongside genuine outages sent people hunting for a broken thing
        # that was never there. Distinguish the two: `unsupported` means
        # there is nothing to report, `unavailable` means a port was
        # expected and is missing.
        provider = (env.get("VPN_SERVICE_PROVIDER") or "").strip().lower()
        pf_requested = (env.get("VPN_PORT_FORWARDING") or "").strip().lower() \
            in ("on", "true", "1", "yes")
        pf_expected = pf_requested and _vpn_supports_port_forwarding(provider)

        if not pf_expected:
            # Skip the docker exec entirely — there is no lease to ask about,
            # and the call would just burn its timeout on every refresh.
            forwarded_port = 0
            qbit_listen = _qbit_listen_port(env)
            port_status = "unsupported"
        else:
            forwarded_port = _gluetun_forwarded_port()
            qbit_listen = _qbit_listen_port(env)
            if forwarded_port <= 0 or qbit_listen == -1:
                port_status = "unavailable"
            elif forwarded_port == qbit_listen:
                port_status = "synced"
            else:
                port_status = "drift"

        return success_response(data={
            "library_count": library_count,
            "queue_count": queue["count"],
            "queue_active_dl_mbps": queue["active_dl_mbps"],
            "qbit_active_torrents": qbit["active"],
            "qbit_seeding_torrents": qbit["seeding"],
            "vpn_forwarded_port": forwarded_port if forwarded_port > 0 else 0,
            "vpn_port_status": port_status,
            "vpn_provider": provider,
            "vpn_port_forwarding_supported": _vpn_supports_port_forwarding(provider),
        })

    # ----- TMDB API key -----
    #
    # THE RESTART PROBLEM. The kiosk loads the TMDB key exactly once, during
    # startup: main.cpp reads $MDB_TMDB_API_KEY or the key file and passes the
    # string to `TmdbClient(tmdb_key)`. TmdbClient's only constructor takes the
    # key by value (tmdb_client.h) and exposes no setter, and the kiosk has no
    # SIGHUP handler or config-reload path — so writing the file does NOT reach
    # a process that is already running. It keeps whatever it had at boot,
    # which on a shipped box is nothing.
    #
    # We do NOT restart the kiosk automatically. This endpoint is reachable at
    # any time, not just during first-run setup, so an implicit restart could
    # kill a movie mid-playback with no warning. Instead:
    #   * the save response reports kiosk_restart_required
    #   * GET reports kiosk_has_current_key, derived from the kiosk service's
    #     start time vs. the key file's mtime — a fact, not a guess
    #   * POST /admin/media-browser/restart-kiosk performs the restart, but
    #     only when the operator explicitly asks for it
    def _tmdb_env_override_present() -> bool:
        """True if MDB_TMDB_API_KEY is set somewhere the kiosk will see it.

        main.cpp checks the environment variable BEFORE the file, and the
        kiosk unit has `EnvironmentFile=-/opt/magic_dingus_box/services/.env`.
        If that file ever defines MDB_TMDB_API_KEY, anything written here is
        dead on arrival — so the UI needs to be able to say so instead of
        reporting a successful save that changes nothing.
        """
        if os.getenv("MDB_TMDB_API_KEY", "").strip():
            return True
        try:
            return bool(_read_env_file(SERVICES_ENV).get("MDB_TMDB_API_KEY", "").strip())
        except Exception:
            return False

    def _tmdb_key_state() -> dict:
        """Everything the UI needs about the key — never the key itself."""
        path = _tmdb_key_file()
        configured = False
        key_length = 0
        updated_at = None
        try:
            if path.is_file():
                content = path.read_text(encoding="utf-8", errors="replace").strip()
                if content:
                    configured = True
                    key_length = len(content)
                    updated_at = path.stat().st_mtime
        except OSError:
            pass

        kiosk_active = check_service_status("magic-dingus-box-cpp") == "active"
        started_at = _kiosk_started_at()
        # Only meaningful when a key exists AND we could read both timestamps.
        if not configured or updated_at is None or started_at is None:
            kiosk_has_current_key = None
        else:
            kiosk_has_current_key = started_at >= updated_at

        return {
            "configured": configured,
            # Width for the masked readout. A v3 key is always 32 hex chars,
            # so this discloses nothing the format doesn't already imply, and
            # the real value never leaves the box.
            "key_length": key_length,
            "updated_at": updated_at,
            "kiosk_service_active": kiosk_active,
            "kiosk_has_current_key": kiosk_has_current_key,
            "env_override": _tmdb_env_override_present(),
            "signup_url": "https://www.themoviedb.org/settings/api",
        }

    @app.get("/admin/media-browser/tmdb")
    def media_browser_tmdb_status():  # type: ignore[no-redef]
        """Report whether a TMDB key is configured. Never returns the key.

        Layer 1 only (require_vpn=False): TMDB metadata calls exit via the
        host network, not through Gluetun, so the key is useful — and
        settable — regardless of VPN state.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        return success_response(data=_tmdb_key_state())

    @app.post("/admin/media-browser/tmdb")
    @require_csrf
    def media_browser_tmdb_save():  # type: ignore[no-redef]
        """Validate a TMDB v3 API key and write it where the kiosk reads it.

        Body: {"api_key": "<32 hex>", "allow_unverified": false}

        Order of operations matters: the key is checked against TMDB BEFORE
        anything is written, so a bad key cannot clobber a working one.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp

        body = request.get_json(silent=True) or {}
        kind, key = _tmdb_classify_key(body.get("api_key", ""))

        if kind == "empty":
            return error_response(
                "invalid_key_format", "Enter your TMDB API key.", status=400)
        if kind == "v4":
            return error_response(
                "unsupported_key_type",
                "That looks like a TMDB Read Access Token (v4). This box needs "
                "the API Key (v3) — the 32-character value on the same TMDB "
                "settings page, listed as \"API Key\".",
                status=400,
            )
        if kind != "v3":
            return error_response(
                "invalid_key_format",
                "A TMDB API key is exactly 32 characters, digits and letters "
                "a-f only. Check for a stray space or a truncated paste.",
                status=400,
            )

        result, detail = _tmdb_verify_key(key)
        if result == "invalid":
            return error_response(
                "key_rejected",
                detail or "TMDB rejected this key.",
                status=400,
            )
        if result == "unreachable" and not body.get("allow_unverified"):
            # Deliberately not saved. The format is right but we have no
            # evidence the key works, and saving an unverified key reproduces
            # the silent-empty-Browse failure. The client can retry with
            # allow_unverified once the operator accepts that trade.
            return error_response(
                "tmdb_unreachable",
                detail or "Could not reach TMDB to check this key.",
                status=503,
            )

        verified = (result == "valid")
        path = _tmdb_key_file()
        try:
            # Trailing newline: main.cpp strips \n, \r and spaces off the end,
            # so this is safe and keeps the file a well-formed text file.
            # 0600 because the kiosk and the web app both run as `magic` and
            # nobody else has any business reading it.
            _atomic_write_text(path, key + "\n", mode=0o600)
        except OSError as e:
            return error_response(
                "write_failed",
                _tmdb_redact(f"Could not write the key file: {e}", key),
                status=500,
            )

        state = _tmdb_key_state()
        state["verified"] = verified
        # After a fresh write the file is newer than any running kiosk, so
        # _tmdb_key_state already reports kiosk_has_current_key=False. Restate
        # it as an explicit instruction for the UI.
        state["kiosk_restart_required"] = (
            state["kiosk_service_active"] and not state["kiosk_has_current_key"]
        )
        if not verified:
            state["verify_warning"] = detail or "Key saved without verification."
        return success_response(
            data=state,
            message=("TMDB key saved and verified." if verified
                     else "TMDB key saved, but it could not be verified."),
        )

    @app.post("/admin/media-browser/restart-kiosk")
    @require_csrf
    def media_browser_restart_kiosk():  # type: ignore[no-redef]
        """Restart the kiosk app so it re-reads the TMDB key.

        Explicitly operator-triggered — see the restart-problem note above.
        This interrupts whatever is on the TV, which is why nothing calls it
        implicitly.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        try:
            result = subprocess.run(
                ["sudo", "-n", "systemctl", "restart", KIOSK_SERVICE],
                capture_output=True, text=True, timeout=120,
            )
            if result.returncode != 0:
                return error_response(
                    "restart_failed",
                    (result.stderr or result.stdout or "systemctl restart failed").strip(),
                    status=500,
                )
            return success_response(message="Kiosk restarted")
        except subprocess.TimeoutExpired:
            return error_response("timeout", "Kiosk restart timed out", status=504)
        except Exception as e:
            return error_response("restart_failed", str(e), status=500)

    @app.post("/admin/media-browser/restart")
    @require_csrf
    def media_browser_restart():  # type: ignore[no-redef]
        """Restart the magic-dingus-services systemd unit (~30 sec).

        Runs `sudo -n systemctl restart magic-dingus-services.service` —
        same path setup_services.sh uses, so it relies on the same NOPASSWD
        sudoers rule.
        """
        if (resp := _check_media_browser_gates()):
            return resp

        if (resp := _require_nopasswd_sudo()):
            return resp

        try:
            result = subprocess.run(
                ["sudo", "-n", "systemctl", "restart", "magic-dingus-services.service"],
                capture_output=True, text=True, timeout=120,
            )
            if result.returncode != 0:
                return error_response(
                    "restart_failed",
                    (result.stderr or result.stdout or "systemctl restart failed").strip(),
                    status=500,
                )
            return success_response(message="Services restarted")
        except subprocess.TimeoutExpired:
            return error_response("timeout", "Restart timed out after 120 seconds", status=504)
        except Exception as e:
            return error_response("restart_failed", str(e), status=500)

    @app.post("/admin/media-browser/reset")
    @require_csrf
    def media_browser_reset():  # type: ignore[no-redef]
        """Tear down the docker stack + wipe configuration. Movies on the SSD
        library are NOT touched; only Radarr's metadata + service config is.

        Requires confirmation token in JSON body to prevent accidental clicks
        in dev tools / curl. After this completes, /status returns
        configured=false → frontend transitions back to State A.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp

        body = request.get_json(silent=True) or {}
        if body.get("confirm") != "RESET":
            return error_response(
                "confirmation_required",
                'Reset requires {"confirm": "RESET"} in request body',
                status=400,
            )

        if (resp := _require_nopasswd_sudo()):
            return resp

        steps_completed = []

        # 1. docker compose down — stop + remove containers + networks
        try:
            result = subprocess.run(
                ["sudo", "-n", "docker", "compose", "down", "--remove-orphans"],
                cwd=str(SERVICES_DIR),
                capture_output=True, text=True, timeout=60,
            )
            if result.returncode != 0:
                # Non-fatal — proceed with file cleanup anyway so a stuck
                # docker daemon can't strand a half-reset state.
                steps_completed.append(
                    f"compose_down_failed:{(result.stderr or result.stdout or '').strip()[:200]}"
                )
            else:
                steps_completed.append("compose_down")
        except Exception as e:
            steps_completed.append(f"compose_down_exception:{e}")

        # 2. Remove .env (drops VPN credentials + API keys)
        try:
            if SERVICES_ENV.exists():
                SERVICES_ENV.unlink()
            steps_completed.append("env_removed")
        except Exception as e:
            return error_response(
                "reset_failed",
                f"Could not remove {SERVICES_ENV}: {e}",
                status=500,
                details={"steps": steps_completed},
            )

        # 3. Wipe service config dirs (radarr/prowlarr/qbit/gluetun/byparr)
        config_dirs_root = SERVICES_DIR / "config"
        targets = ["radarr", "prowlarr", "qbittorrent", "gluetun", "byparr"]
        for name in targets:
            target = config_dirs_root / name
            if not target.exists():
                continue
            try:
                # Use sudo for cleanup since service containers run as a
                # different uid and may have written root-owned state.
                rm_result = subprocess.run(
                    ["sudo", "-n", "find", str(target), "-mindepth", "1", "-delete"],
                    capture_output=True, text=True, timeout=30,
                )
                if rm_result.returncode != 0:
                    steps_completed.append(
                        f"wipe_{name}_failed:{(rm_result.stderr or '').strip()[:120]}"
                    )
                else:
                    steps_completed.append(f"wipe_{name}")
            except Exception as e:
                steps_completed.append(f"wipe_{name}_exception:{e}")

        return success_response(
            data={"steps": steps_completed},
            message="Media Browser reset",
        )
