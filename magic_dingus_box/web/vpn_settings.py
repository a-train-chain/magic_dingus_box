"""VPN server country for the Media Browser (Content Manager > Media Browser
> Advanced).

The owner's ProtonVPN tunnel ran on Netherlands servers only — hardcoded at
setup time — and when that pool misbehaved (2026-10-03/04: ~170 drops in
bursts) there was no way to move it short of re-uploading a config by hand.
This lets the operator pick the country from a short list and applies it.

What it does, and nothing more:

* The country lives in services/.env as VPN_COUNTRIES, which compose passes
  to gluetun as SERVER_COUNTRIES (`${VPN_COUNTRIES-}` — see
  docker-compose.yml). A change rewrites ONLY that line (every other byte of
  the file is kept, comments included), atomically, with the file's mode
  preserved. Nothing else in the stack writes this key after setup:
  setup_services.sh only backfills keys it owns, and the setup route keeps
  the current country on a Reconfigure.
* Then ONLY gluetun is recreated (scripts/recreate_gluetun.sh, root, as a
  detached job — the same single-flight maintenance slot as an OTA or a
  Media Browser setup, so none of them can rewrite .env underneath another).
  That script takes the shared compose lock; the cascade watcher re-links
  the dependents on gluetun's start event.
* Country filtering only exists for gluetun's native `protonvpn` provider.
  With `custom` (any other WireGuard config) the uploaded config IS the
  server, and gluetun treats any country as fatal — so the setting is shown
  read-only there and the POST refuses.
* No passwords or logins (owner decision): the routes sit behind the same
  Layer 1 + 2 Media Browser gates, CSRF token and Sec-Fetch-Site check as
  every other state-changing Content Manager route.

The country list is deliberately short. Every entry is a gluetun `protonvpn`
country name (exact spelling — gluetun matches them literally) with at
least six port-forwarding (P2P) WireGuard servers, outside Secure Core and
Tor, in the server list embedded in gluetun v3.41.1/v3.41.3: Netherlands 7,
United States 79, Australia 20, Canada 14, France 13, United Kingdom 13,
Switzerland 9, Germany 8, Romania 7, Spain 6, Sweden 6. With
VPN_PORT_FORWARDING=on gluetun only picks from those servers, so a country
with one or two would leave no fallback when one misbehaves.

Wired into admin.py by a single register() call; all the logic is here.
"""
from __future__ import annotations

import os
import tempfile
import threading
from pathlib import Path
from typing import Callable, Optional

from flask import request

DEFAULT_COUNTRY = "Netherlands"
# Default first, then alphabetical. See the module docstring for the rule.
COUNTRIES = (
    "Netherlands",
    "Australia",
    "Canada",
    "France",
    "Germany",
    "Romania",
    "Spain",
    "Sweden",
    "Switzerland",
    "United Kingdom",
    "United States",
)
ENV_KEY = "VPN_COUNTRIES"
JOB_KIND = "vpn-country"
NATIVE_PROVIDER = "protonvpn"
LOG_TAIL_LINES = 8


def provider_of(env: dict) -> str:
    """The gluetun provider this box runs. compose defaults an absent key to
    protonvpn (`${VPN_SERVICE_PROVIDER:-protonvpn}`), so absent == protonvpn."""
    return (env.get("VPN_SERVICE_PROVIDER") or "").strip().lower() or NATIVE_PROVIDER


def country_state(env: dict) -> dict:
    """What the panel shows: current country, and whether it can change."""
    provider = provider_of(env)
    country = (env.get(ENV_KEY) or "").strip()
    editable = provider == NATIVE_PROVIDER
    if editable:
        reason = ""
    else:
        reason = ("This box uses your own WireGuard config, which already picks "
                  "the server. To change country, download a config for a server "
                  "in that country and use Reconfigure.")
    return {
        "provider": provider,
        # Unset on a protonvpn box means gluetun picks from every country;
        # report that honestly rather than pretending it is the default.
        "country": country,
        "default": DEFAULT_COUNTRY,
        "choices": list(COUNTRIES),
        "editable": editable,
        "reason": reason,
    }


def rewrite_env_text(text: str, line: str) -> str:
    """Replace the VPN_COUNTRIES line in .env text with `line`, keeping every
    other line byte-for-byte. Duplicates collapse into the first position
    (a later duplicate would otherwise silently win); an absent key is
    appended. `line` is a full, already-validated `KEY=VALUE` line."""
    out, placed = [], False
    for raw in text.splitlines(keepends=True):
        key = raw.strip().split("=", 1)[0].strip() if "=" in raw else ""
        if key == ENV_KEY and not raw.lstrip().startswith("#"):
            if not placed:
                out.append(line + ("\n" if raw.endswith("\n") else ""))
                placed = True
            continue
        out.append(raw)
    if not placed:
        if out and not out[-1].endswith("\n"):
            out[-1] += "\n"
        out.append(line + "\n")
    return "".join(out)


def atomic_replace(path: Path, text: str) -> None:
    """tmp + fsync + rename + dir fsync, keeping the file's permission bits
    (services/.env is 0600: it holds the WireGuard key and qBit password)."""
    path = Path(path)
    try:
        mode = path.stat().st_mode & 0o777
    except OSError:
        mode = 0o600
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    try:
        dfd = os.open(str(path.parent), os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    except OSError:
        pass


def register(app, *, require_csrf: Callable, check_gates: Callable,
             env_path: Path, script_path: Path, read_env: Callable,
             env_read_error: type, format_env_line: Callable,
             require_sudo: Callable, maintenance_precheck: Callable,
             launch_job: Callable, detached, success_response: Callable,
             error_response: Callable) -> None:
    """Add GET/POST /admin/media-browser/vpn-country to `app`.

    Everything box-specific is injected by admin.py's create_app (its CSRF
    decorator, Media Browser gates, .env reader/formatter, detached-job
    launcher), so this module carries no second copy of any of them.
    """
    lock = threading.Lock()
    last_job: dict = {"id": None}

    def _job_view() -> Optional[dict]:
        job_id = last_job["id"]
        if job_id is None:
            # A Flask that restarted mid-change: the running job is on disk.
            running = detached.active([JOB_KIND])
            if not running:
                return None
            job_id = running[0]
        result = detached.read(job_id, 0)
        if result is None:
            return None
        lines, _, state, rc = result
        status = {"running": "running", "lost": "failed"}.get(
            state, "success" if rc == 0 else "failed")
        return {"id": job_id, "status": status, "exit_code": rc,
                "log": [l for l in lines if l.strip()][-LOG_TAIL_LINES:]}

    @app.get("/admin/media-browser/vpn-country")
    def vpn_country_get():  # type: ignore[no-redef]
        if (resp := check_gates()):
            return resp
        try:
            env = read_env(env_path)
        except env_read_error as e:
            return error_response("env_read_failed", str(e), status=500)
        data = country_state(env)
        data["job"] = _job_view()
        return success_response(data=data)

    @app.post("/admin/media-browser/vpn-country")
    @require_csrf
    def vpn_country_set():  # type: ignore[no-redef]
        if (resp := check_gates()):
            return resp
        body = request.get_json(silent=True)
        if not isinstance(body, dict):
            body = {}
        country = str(body.get("country") or "").strip()
        if country not in COUNTRIES:
            return error_response(
                "invalid_country",
                "Pick one of: " + ", ".join(COUNTRIES), status=400)
        if (resp := require_sudo()):
            return resp
        if not lock.acquire(blocking=False):
            return error_response("busy", "A VPN country change is already being "
                                  "applied — wait for it to finish.", status=409)
        try:
            if (resp := maintenance_precheck()):
                return resp
            try:
                env = read_env(env_path)
                original = Path(env_path).read_text(encoding="utf-8")
            except env_read_error as e:
                return error_response(
                    "env_read_failed",
                    f"{e}. Nothing was changed.", status=500)
            except (OSError, UnicodeDecodeError) as e:
                return error_response(
                    "env_read_failed",
                    f"Could not read {env_path}: {e}. Nothing was changed.", status=500)
            state = country_state(env)
            if not state["editable"]:
                return error_response("not_supported", state["reason"], status=409)
            if state["country"] == country:
                return success_response(
                    data={"country": country, "changed": False, "job_id": None},
                    message=f"The VPN already uses {country}")

            try:
                new_text = rewrite_env_text(original, format_env_line(ENV_KEY, country))
                atomic_replace(Path(env_path), new_text)
            except (OSError, ValueError) as e:
                return error_response(
                    "env_write_failed",
                    f"Could not update {env_path}: {e}. Nothing was changed.",
                    status=500)

            job_id, err = launch_job(JOB_KIND, ["/bin/bash", str(script_path)],
                                     as_root=True)
            if err:
                # Nothing applied it: put the old line back so the file keeps
                # describing the tunnel that is actually running.
                try:
                    atomic_replace(Path(env_path), original)
                except OSError:
                    pass
                return err
            last_job["id"] = job_id
            return success_response(
                data={"country": country, "changed": True, "job_id": job_id},
                message=f"Switching the VPN to {country}")
        finally:
            lock.release()
