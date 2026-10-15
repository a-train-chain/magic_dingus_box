"""Media Browser provisioning routes: visibility, Prepare Drive, status,
setup (WireGuard .conf -> services/.env -> setup_services.sh, detached),
provider detection, setup-status and credentials, plus the VPN-country
routes registered from vpn_settings.py. Every route sits behind the
Layer 1 unlock gate (and most behind the VPN-configured gate).
"""
from __future__ import annotations

import json
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path

from flask import request

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    import vpn_settings
    from admin_common import error_response, success_response
    from admin_vpn_config import (
        _detect_vpn_brand, _format_env_line, _parse_wireguard_config,
        _read_env_file, _resolve_wireguard_endpoint_ip,
        _VPN_AUTO_NATIVE_PROVIDERS, _vpn_provider_choices,
        _VPN_PROVIDER_CUSTOM, _vpn_provider_env,
        _vpn_supports_port_forwarding, EnvFileReadError,
    )
    from storage_prepare import (
        eligible_devices, movies_drive_devices, protected_disk_names,
        PROTECTED_MOUNTPOINTS,
    )
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from . import vpn_settings
    from .admin_common import error_response, success_response
    from .admin_vpn_config import (
        _detect_vpn_brand, _format_env_line, _parse_wireguard_config,
        _read_env_file, _resolve_wireguard_endpoint_ip,
        _VPN_AUTO_NATIVE_PROVIDERS, _vpn_provider_choices,
        _VPN_PROVIDER_CUSTOM, _vpn_provider_env,
        _vpn_supports_port_forwarding, EnvFileReadError,
    )
    from .storage_prepare import (
        eligible_devices, movies_drive_devices, protected_disk_names,
        PROTECTED_MOUNTPOINTS,
    )


# Written by playback_services_pause.sh while the kiosk has intentionally
# stopped the RAM-heavy Media Browser containers for a game/movie. Lives in
# /tmp (tmpfs) so a marker orphaned by a power cut cannot survive into the
# next boot; neither this service's unit nor the kiosk's uses PrivateTmp, so
# both processes see the same file.
PLAYBACK_PAUSE_MARKER = Path("/tmp/mdb_playback_services_paused")

# The containers that script stops. Gluetun (VPN netns) and qBittorrent
# (active downloads) intentionally stay up during playback.
PLAYBACK_PAUSED_CONTAINERS = frozenset({"mdb_radarr", "mdb_sonarr", "mdb_prowlarr", "mdb_byparr"})


def _protected_disks() -> set:
    """Whole disks backing the running system — never offered for formatting.

    Derived live from findmnt rather than hardcoded, because the boot device
    differs across boards (mmcblk0 on SD, nvme0n1 on an NVMe hat, sda on USB
    boot). An empty return means we could not establish what the system runs
    from, and every caller treats that as fatal rather than proceeding.
    """
    sources = []
    for mountpoint in PROTECTED_MOUNTPOINTS:
        try:
            out = subprocess.run(["findmnt", "-no", "SOURCE", mountpoint],
                                 capture_output=True, text=True, timeout=10)
            if out.returncode == 0:
                sources.append(out.stdout.strip())
        except Exception:  # noqa: BLE001 - a missing mount is not an error
            continue
    return protected_disk_names(sources)


def _mountpoints_under(disk_path: str) -> list:
    """Every mountpoint currently served by this disk or its partitions.

    Used to clear a drive before formatting it. Anything still mounted makes
    wipefs fail with "device is busy", which is an unhelpful way for the
    Prepare Drive flow to end.
    """
    out = []
    try:
        result = subprocess.run(["lsblk", "-nro", "MOUNTPOINT", disk_path],
                                capture_output=True, text=True, timeout=15)
        if result.returncode != 0:
            return []
        for line in result.stdout.splitlines():
            mountpoint = line.strip()
            # Never hand a system path to umount, whatever lsblk reports. The
            # eligibility rule should already have excluded such a disk; this
            # is the same belt-and-braces reasoning applied one layer down.
            if mountpoint and mountpoint not in PROTECTED_MOUNTPOINTS:
                out.append(mountpoint)
    except Exception:  # noqa: BLE001
        return []
    return out


def _first_partition_of(disk_path: str):
    """The first partition node of a disk, once udev has created it.

    Naming differs by device class — sda -> sda1, but mmcblk0 -> mmcblk0p1 and
    nvme0n1 -> nvme0n1p1 — so this asks lsblk rather than concatenating.
    """
    try:
        out = subprocess.run(["lsblk", "-J", "-o", "NAME,TYPE", disk_path],
                             capture_output=True, text=True, timeout=15)
        if out.returncode != 0:
            return None
        for device in (json.loads(out.stdout).get("blockdevices") or []):
            for child in (device.get("children") or []):
                if child.get("type") == "part":
                    return "/dev/" + child["name"]
    except Exception:  # noqa: BLE001
        return None
    return None


def _require_nopasswd_sudo():
    """Fail-fast NOPASSWD-sudo precheck for routes that shell out via `sudo -n`.

    Returns an error_response() tuple when the magic user lacks NOPASSWD sudo
    (so the caller can `if (resp := _require_nopasswd_sudo()): return resp`),
    or None when sudo is available. Extracted from three byte-identical copies
    (WireGuard setup, MB restart, MB reset) — the point is to fail with a clear
    error rather than spawn a process that hangs on a password prompt.
    """
    try:
        sudo_check = subprocess.run(
            ["sudo", "-n", "true"], capture_output=True, text=True, timeout=5
        )
        if sudo_check.returncode != 0:
            return error_response(
                "sudo_required",
                "magic user must have NOPASSWD sudo configured",
                status=500,
            )
    except Exception:
        return error_response(
            "sudo_required",
            "magic user must have NOPASSWD sudo configured",
            status=500,
        )
    return None


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    _launch_maintenance_job = ctx._launch_maintenance_job
    _maintenance_precheck = ctx._maintenance_precheck
    _prune_terminal_jobs = ctx._prune_terminal_jobs
    data_dir = ctx.data_dir
    detached = ctx.detached
    require_csrf = ctx.require_csrf
    # Defined/re-exported in admin.py; tests monkeypatch admin.<name>, so
    # these are shims that look the name up in admin at call time.
    _atomic_write_text = ctx._atomic_write_text
    _media_browser_locked_response = ctx._media_browser_locked_response
    _media_browser_unlocked = ctx._media_browser_unlocked
    _require_nopasswd_sudo = ctx._require_nopasswd_sudo

    # ===== MEDIA BROWSER (RADARR/PROWLARR/QBIT/GLUETUN) SETUP =====
    #
    # Provisions the Media Browser docker stack on a fresh Pi. Operator drops a
    # WireGuard .conf from the ProtonVPN dashboard into the Content Manager UI;
    # we parse out the 4 vars Gluetun needs, write them into
    # /opt/magic_dingus_box/services/.env (preserving any non-WG vars), then
    # invoke setup_services.sh as a background job and stream its stdout for
    # the frontend to tail.

    SERVICES_DIR = data_dir.parent.parent / "services"
    SERVICES_ENV = SERVICES_DIR / ".env"
    SETUP_SERVICES_SCRIPT = data_dir.parent / "scripts" / "setup_services.sh"

    EXPECTED_CONTAINERS = [
        "mdb_gluetun",
        "mdb_radarr",
        "mdb_sonarr",
        "mdb_prowlarr",
        "mdb_qbittorrent",
        "mdb_byparr",
    ]

    # Track media-browser setup jobs (in-memory, cleared on restart)
    media_browser_jobs: dict = {}
    _MB_LOG_BUFFER_LIMIT = 500  # keep at most this many lines per job
    _MB_LOG_TAIL_LINES = 30     # return this many lines on each status poll

    def _write_env_file(path: Path, env: dict) -> None:
        """Write a dict back to a .env file with chmod 600. Creates parent dir."""
        path.parent.mkdir(parents=True, exist_ok=True)
        # Write to a tempfile in the same dir, then atomic rename, so a crash
        # mid-write can't leave a partial .env.
        lines = [_format_env_line(k, v) for k, v in env.items()]
        # Was already tmp+rename, but with no fsync: the rename could survive a
        # power cut while the contents behind it did not. This file holds the
        # WireGuard private key and the qBittorrent password.
        _atomic_write_text(path, "\n".join(lines) + "\n", mode=0o600)

    def _detect_timezone() -> str:
        """Best-effort host timezone detection; falls back to UTC."""
        try:
            result = subprocess.run(
                ["timedatectl", "show", "-p", "Timezone", "--value"],
                capture_output=True, text=True, timeout=5,
            )
            tz = result.stdout.strip()
            if tz:
                return tz
        except Exception:
            pass
        return "UTC"

    def _docker_ps_table() -> list[dict]:
        """Return [{name, status, paused_for_playback}] for the expected
        media-browser containers.

        paused_for_playback marks the state where playback_services_pause.sh
        stopped the container to free RAM for a game/movie. Radarr and
        Prowlarr never survive the script's 2 s SIGTERM grace, so their raw
        docker status during every playback session is "Exited (137)" —
        indistinguishable from a crash/OOM without this flag. Requires all
        three signals: the script's marker file, membership in the pause
        set, and an Exited status (an Up container beats a stale marker,
        and "not found"/"unknown" is not the pause signature).
        """
        try:
            result = subprocess.run(
                ["docker", "ps", "-a", "--format", "{{.Names}}\t{{.Status}}"],
                capture_output=True, text=True, timeout=5,
            )
            if result.returncode != 0:
                return [{"name": n, "status": "unknown",
                         "paused_for_playback": False}
                        for n in EXPECTED_CONTAINERS]
            running = {}
            for line in result.stdout.splitlines():
                parts = line.split("\t", 1)
                if len(parts) == 2:
                    running[parts[0].strip()] = parts[1].strip()
            playback_paused = PLAYBACK_PAUSE_MARKER.exists()
            return [
                {
                    "name": n,
                    "status": running.get(n, "not found"),
                    "paused_for_playback": (
                        playback_paused
                        and n in PLAYBACK_PAUSED_CONTAINERS
                        and running.get(n, "").lower().startswith("exited")
                    ),
                }
                for n in EXPECTED_CONTAINERS
            ]
        except Exception:
            return [{"name": n, "status": "unknown",
                     "paused_for_playback": False}
                    for n in EXPECTED_CONTAINERS]

    def _vpn_exit_info() -> dict:
        """Hit gluetun's local control server for current exit IP + country.

        Gluetun's control server listens on port 8000 INSIDE the container and
        is not (by default) exposed on the host, so we have to shell into the
        container with `docker exec`. Returns empty strings when gluetun isn't
        running or anything goes wrong — never raises.
        """
        try:
            result = subprocess.run(
                ["docker", "exec", "mdb_gluetun", "wget", "-qO-",
                 "http://localhost:8000/v1/publicip/ip"],
                capture_output=True, text=True, timeout=5,
            )
            if result.returncode != 0 or not result.stdout.strip():
                return {"vpn_exit_ip": "", "vpn_country": ""}
            payload = json.loads(result.stdout)
            return {
                "vpn_exit_ip": payload.get("public_ip", "") or "",
                "vpn_country": payload.get("country", "") or "",
            }
        except Exception:
            return {"vpn_exit_ip": "", "vpn_country": ""}

    def _env_has_wireguard_key(path: Path) -> bool:
        """True iff .env exists AND has a non-empty WIREGUARD_PRIVATE_KEY=."""
        try:
            env = _read_env_file(path)
        except EnvFileReadError:
            return False  # fail closed — an unreadable .env allows nothing
        return bool(env.get("WIREGUARD_PRIVATE_KEY", "").strip())

    def _vpn_configured() -> bool:
        """True iff services/.env exists AND has a non-empty WIREGUARD_PRIVATE_KEY.

        Layer 2 of the three-layer Media Browser gate. Failure-closed:
        any error reading the .env returns False so a malformed file
        can't accidentally allow access.

        Delegates to _env_has_wireguard_key with the canonical SERVICES_ENV
        path. Use _env_has_wireguard_key directly if you need to check a
        non-canonical path (e.g., during setup-job preview).
        """
        return _env_has_wireguard_key(SERVICES_ENV)

    def _vpn_required_response():
        """Standard 403 used when Layer 2 (VPN configured) fails."""
        return error_response(
            "vpn_not_configured",
            "VPN must be configured in the Media Browser tab before using this feature",
            status=403,
        )

    def _check_media_browser_gates(*, require_vpn: bool = True):
        """Run the Layer 1 + (optionally) Layer 2 gates.

        Returns None on pass, or a 403 Response on fail. Endpoints that
        are part of the VPN-setup flow itself (status, setup,
        setup-status, reset) pass require_vpn=False so the operator can
        reach them before configuring VPN.
        """
        if not _media_browser_unlocked():
            return _media_browser_locked_response()
        if require_vpn and not _vpn_configured():
            return _vpn_required_response()
        return None

    @app.get("/admin/media-browser/visibility")
    def media_browser_visibility():  # type: ignore[no-redef]
        """Public — return whether the Media Browser tab should be rendered.

        Always 200, never errors. Returns two flags:
          - visible: Layer 1 (unlock). Whether to render the tab DOM
            at all.
          - vpn_configured: Layer 2 (WireGuard config dropped). When
            visible=true and vpn_configured=false, the frontend shows
            a "Configure VPN" form instead of the dashboard.

        Other /admin/media-browser/* routes enforce the same gates
        server-side via _check_media_browser_gates and return 403
        (`media_browser_locked` or `vpn_not_configured`) on failure.
        """
        return success_response(data={
            "visible": _media_browser_unlocked(),
            "vpn_configured": _vpn_configured(),
        })

    # VPN server country (Media Browser > Advanced). All logic lives in
    # vpn_settings.py; this hands it the gates and helpers it must share.
    vpn_settings.register(
        app,
        require_csrf=require_csrf,
        check_gates=_check_media_browser_gates,
        env_path=SERVICES_ENV,
        script_path=data_dir.parent / "scripts" / "recreate_gluetun.sh",
        read_env=_read_env_file,
        env_read_error=EnvFileReadError,
        format_env_line=_format_env_line,
        require_sudo=lambda: _require_nopasswd_sudo(),
        maintenance_precheck=_maintenance_precheck,
        launch_job=_launch_maintenance_job,
        detached=detached,
        success_response=success_response,
        error_response=error_response,
    )

    # ===== PREPARE DRIVE =====
    #
    # A customer's new drive is exFAT or NTFS with some arbitrary label, so it
    # does not match the `LABEL=MOVIES ... ext4` line in /etc/fstab and never
    # mounts. Worse, STORAGE_ROOT=/mnt/ssd is a plain directory on the SD card
    # when nothing is mounted there, so the stack comes up bound to the SD and
    # downloads land on the OS partition.
    #
    # ext4 is not something a customer can produce from Windows or macOS, so
    # the preparation has to happen on the box.
    #
    # Gated behind the SAME Layer 1 unlock as the rest of the Media Browser —
    # when the secret sequence has not been entered, these 403 exactly like
    # every other /admin/media-browser/* route and the UI renders nothing.
    # require_vpn=False because preparing storage is setup work that precedes
    # dropping in a WireGuard config, same as the other setup endpoints.

    @app.get("/admin/media-browser/storage/devices")
    def storage_devices():  # type: ignore[no-redef]
        gate = _check_media_browser_gates(require_vpn=False)
        if gate is not None:
            return gate
        try:
            listing = subprocess.run(
                ["lsblk", "-J", "-o", "NAME,TYPE,SIZE,RM,MOUNTPOINT,LABEL,FSTYPE"],
                capture_output=True, text=True, timeout=15, check=True).stdout
            protected = _protected_disks()
        except Exception as e:  # noqa: BLE001 - surfaced to the operator
            return error_response("STORAGE_ERROR", f"Could not enumerate disks: {e}")
        if not protected:
            # We could not establish what the system is running from. Refusing
            # is the only safe answer — an empty protected set would make the
            # boot disk eligible.
            return error_response(
                "STORAGE_ERROR",
                "Could not determine the system disk; refusing to list targets")
        return success_response(data={
            "devices": eligible_devices(json.loads(listing), protected),
            "mountpoint": "/mnt/ssd",
            "label": "MOVIES",
        })

    @app.post("/admin/media-browser/storage/prepare")
    @require_csrf
    def storage_prepare():  # type: ignore[no-redef]
        """ERASE a drive and set it up to hold the movie library.

        Destructive and irreversible. Three independent checks stand between a
        request and mkfs: the device must be in the eligible list computed
        fresh here (never trusted from the client), the caller must echo the
        device name back in `confirm`, and the eligibility rule itself refuses
        anything serving the running system.
        """
        gate = _check_media_browser_gates(require_vpn=False)
        if gate is not None:
            return gate

        body = request.get_json(silent=True) or {}
        device = str(body.get("device") or "").strip()
        confirm = str(body.get("confirm") or "").strip()
        if not device:
            return error_response("VALIDATION_ERROR", "device required")

        try:
            listing = subprocess.run(
                ["lsblk", "-J", "-o", "NAME,TYPE,SIZE,RM,MOUNTPOINT,LABEL,FSTYPE"],
                capture_output=True, text=True, timeout=15, check=True).stdout
            protected = _protected_disks()
        except Exception as e:  # noqa: BLE001
            return error_response("STORAGE_ERROR", f"Could not enumerate disks: {e}")
        if not protected:
            return error_response(
                "STORAGE_ERROR",
                "Could not determine the system disk; refusing to format anything")

        # Re-derive eligibility server-side. The client's idea of what is safe
        # is never trusted.
        allowed = {d["name"]: d for d in eligible_devices(json.loads(listing), protected)}
        target = allowed.get(device)
        if target is None:
            return error_response(
                "VALIDATION_ERROR",
                f"{device} is not a device this box may format", status=400)

        # Explicit, typed confirmation naming the exact device. Guards against
        # a mis-click reaching a destructive endpoint.
        if confirm != device:
            return error_response(
                "VALIDATION_ERROR",
                "confirm must exactly match the device name", status=400)

        # Refuse to mint a SECOND MOVIES drive. mkfs stamps LABEL=MOVIES and
        # fstab mounts by label, so formatting a new disk while another
        # MOVIES-labeled disk is attached makes which one mounts at boot
        # arbitrary — and the old code here made it worse by lazy-unmounting
        # /mnt/ssd unconditionally first, detaching the LIVE library out from
        # under the running containers even when the target was an unrelated
        # disk (writes continued into the detached filesystem; imports failed
        # with nothing in any log). Formatting the current MOVIES drive
        # itself remains allowed — that's the wipe-and-rebuild flow.
        movies_disks = movies_drive_devices(json.loads(listing))
        movies_elsewhere = [n for n in movies_disks if n != device]
        if movies_elsewhere:
            return error_response(
                "VALIDATION_ERROR",
                f"{movies_elsewhere[0]} is currently the MOVIES drive. "
                f"Formatting {device} too would leave two MOVIES-labeled "
                "disks and which one holds the library after a reboot would "
                "be arbitrary. Unplug the old drive first (or format that "
                "drive instead).", status=409)

        path = target["path"]
        try:
            # Unmount everything on the target before touching it, or wipefs
            # fails with "device is busy" — a confusing way for this to end.
            #
            # /mnt/ssd is released ONLY when the target itself is the MOVIES
            # drive (the guard above means any other MOVIES disk already
            # blocked the request, so this is scoped by construction).
            #
            # Mountpoints are queried live rather than read from the listing
            # above: the eligible-device records deliberately carry no
            # partition detail, and state can change between listing and
            # formatting anyway.
            if device in movies_disks:
                subprocess.run(["sudo", "umount", "-l", "/mnt/ssd"],
                               capture_output=True, timeout=30)
            for mp in _mountpoints_under(path):
                subprocess.run(["sudo", "umount", "-l", mp],
                               capture_output=True, timeout=30)

            steps = [
                # Clear any existing signatures so a stale label cannot linger.
                ["sudo", "wipefs", "-a", path],
                ["sudo", "parted", "-s", path, "mklabel", "gpt"],
                ["sudo", "parted", "-s", path, "mkpart", "primary", "ext4",
                 "0%", "100%"],
            ]
            for step in steps:
                subprocess.run(step, capture_output=True, text=True,
                               timeout=120, check=True)
            subprocess.run(["sudo", "partprobe", path], capture_output=True,
                           timeout=60)
            time.sleep(2)  # let udev create the partition node

            partition = _first_partition_of(path)
            if not partition:
                return error_response(
                    "STORAGE_ERROR",
                    "Partition was created but did not appear; try again")

            # The label is the whole contract with /etc/fstab.
            subprocess.run(["sudo", "mkfs.ext4", "-F", "-L", "MOVIES", partition],
                           capture_output=True, text=True, timeout=600, check=True)
            subprocess.run(["sudo", "systemctl", "daemon-reload"],
                           capture_output=True, timeout=30)
            subprocess.run(["sudo", "mount", "/mnt/ssd"], capture_output=True,
                           text=True, timeout=60, check=True)

            # Radarr binds ${STORAGE_ROOT}/library and qBit ${STORAGE_ROOT}/
            # downloads. Docker resolves a bind source once, at container
            # start, so these must exist before the stack is re-linked.
            for sub in ("library", "downloads"):
                subprocess.run(["sudo", "mkdir", "-p", f"/mnt/ssd/{sub}"],
                               capture_output=True, timeout=30, check=True)
            subprocess.run(["sudo", "chown", "-R", "magic:magic", "/mnt/ssd"],
                           capture_output=True, timeout=120)
        except subprocess.CalledProcessError as e:
            detail = (e.stderr or e.stdout or "").strip()[:300]
            return error_response("STORAGE_ERROR",
                                  f"Preparation failed: {detail or e}")
        except Exception as e:  # noqa: BLE001
            return error_response("STORAGE_ERROR", f"Preparation failed: {e}")

        # Re-link the containers onto the freshly mounted drive. Without this
        # they keep the bind they resolved at start — the empty placeholder
        # dirs on the SD card — and Radarr reports an empty library with
        # nothing in any log to explain it.
        try:
            subprocess.run(
                ["sudo", "systemctl", "start", "magic-dingus-storage-attach.service"],
                capture_output=True, timeout=300)
        except Exception:  # noqa: BLE001 - drive is prepared either way
            pass

        return success_response(
            data={"device": device, "mountpoint": "/mnt/ssd", "label": "MOVIES"},
            message=f"{device} is ready for movies")

    @app.get("/admin/media-browser/status")
    def media_browser_status():  # type: ignore[no-redef]
        """Return current Media Browser configuration + service health.

        Drives the 3-state UI: Not configured / Configuring / Configured.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        env_present = _env_has_wireguard_key(SERVICES_ENV)
        containers = _docker_ps_table()
        services_running = any(
            c["status"].lower().startswith("up")
            for c in containers
        )
        vpn = _vpn_exit_info() if services_running else {"vpn_exit_ip": "", "vpn_country": ""}

        return success_response(data={
            "configured": env_present,
            "env_present": env_present,
            "services_running": services_running,
            "containers": containers,
            "vpn_country": vpn["vpn_country"],
        })

    _mb_jobs_lock = threading.Lock()

    def _new_mb_job_view(started_ts: float) -> dict:
        return {
            "status": "running",
            "exit_code": None,
            "log": [],
            "started_at": datetime.fromtimestamp(started_ts).isoformat(),
            "started_ts": started_ts,
            "_cursor": 0,
            "_pruner_ts": time.time(),
        }

    def _refresh_media_browser_job(job_id: str, job: dict) -> None:
        """Pull setup_services.sh's new output from the detached job's log.

        setup_services.sh runs as its own systemd unit (detached_jobs.py):
        it restarts magic-dingus-web itself (Step: uinput group), which under
        KillMode=control-group used to kill the script that issued the
        restart, mid-provisioning. Status is re-derived from the log, so a
        restarted Flask resumes reporting where the old one stopped.
        """
        if job["status"] != "running":
            return
        result = detached.read(job_id, job["_cursor"])
        if result is None:
            return
        lines, job["_cursor"], state, rc = result
        buf = job["log"]
        buf.extend(lines)
        if len(buf) > _MB_LOG_BUFFER_LIMIT:
            del buf[: len(buf) - _MB_LOG_BUFFER_LIMIT]
        if state == "exited":
            job["exit_code"] = rc
            job["status"] = "success" if rc == 0 else "failed"
        elif state == "lost":
            buf.append("[admin.py] setup process stopped unexpectedly — "
                       "safe to run setup again")
            job["exit_code"] = -1
            job["status"] = "failed"
        if job["status"] != "running":
            job["_pruner_ts"] = time.time()

    @app.post("/admin/media-browser/setup")
    @require_csrf
    def media_browser_setup():  # type: ignore[no-redef]
        """Configure VPN credentials + run setup_services.sh.

        Accepts the WireGuard config either as a multipart .conf file upload
        ('file' field) or as pasted text in the 'config_text' form field.
        Returns a job_id; clients poll /admin/media-browser/setup-status/<id>.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        if not SETUP_SERVICES_SCRIPT.exists():
            return error_response(
                "setup_script_missing",
                f"setup_services.sh not found at {SETUP_SERVICES_SCRIPT}",
                status=500,
            )

        # Pull config text from upload or form field
        config_text = ""
        if "file" in request.files and request.files["file"].filename:
            try:
                config_text = request.files["file"].read().decode("utf-8", errors="replace")
            except Exception as e:
                return error_response("invalid_wireguard_config",
                                      f"Could not read uploaded file: {e}", status=400)
        else:
            config_text = (request.form.get("config_text") or "").strip()

        if not config_text:
            return error_response(
                "invalid_wireguard_config",
                "No WireGuard config provided (expected .conf file upload or 'config_text' form field)",
                status=400,
            )

        try:
            wg = _parse_wireguard_config(config_text)
        except ValueError as e:
            return error_response("invalid_wireguard_config",
                                  f"Could not parse WireGuard config: {e}", status=400)

        # Which gluetun provider runs this tunnel.
        #
        # The operator's dropdown wins when they set it; otherwise we detect.
        # Detection only promotes to NATIVE mode for the providers in
        # _VPN_AUTO_NATIVE_PROVIDERS (ProtonVPN alone, because it is the only
        # WireGuard provider gluetun can port-forward for). Every other
        # recognised brand is still a label — it runs as `custom`, which
        # works for any standard WireGuard config. That keeps a wrong guess
        # cosmetic instead of turning it into a failed tunnel.
        detected = _detect_vpn_brand(config_text, wg)
        requested = (request.form.get("provider") or "").strip().lower()
        if requested and requested != "auto":
            provider = requested
        elif detected in _VPN_AUTO_NATIVE_PROVIDERS:
            provider = detected
        else:
            provider = _VPN_PROVIDER_CUSTOM

        # `custom` has no server list, so gluetun connects to the endpoint in
        # the .conf verbatim — and it will only accept a literal IP there.
        # Several providers ship a hostname, so resolve it now and fail with
        # something the operator can act on rather than letting gluetun exit
        # at startup with a ParseAddr error nobody will ever read.
        if provider == _VPN_PROVIDER_CUSTOM:
            try:
                wg["WIREGUARD_ENDPOINT_IP"] = _resolve_wireguard_endpoint_ip(
                    wg["WIREGUARD_ENDPOINT_IP"])
            except ValueError as e:
                return error_response("invalid_wireguard_config", str(e),
                                      status=400)

        # Fail fast if the magic user lacks NOPASSWD sudo (rather than hang on
        # a password prompt) — this route shells out via `sudo -n` below.
        if (resp := _require_nopasswd_sudo()):
            return resp

        # Refuse BEFORE touching .env: a setup (or OTA) already running is
        # reading it, and rewriting it underneath is the half of the race
        # the launch-time check alone cannot prevent.
        if (resp := _maintenance_precheck()):
            return resp

        # Merge WG vars + sensible defaults into existing .env. An existing
        # .env we cannot READ must abort the request: treating it as empty
        # would rewrite it with only the WireGuard keys and destroy the qBit
        # password + API keys (see _read_env_file).
        try:
            env = _read_env_file(SERVICES_ENV)
        except EnvFileReadError as e:
            return error_response(
                "env_read_failed",
                f"{e}. Nothing was changed. The file's owner or permissions "
                "need fixing before the VPN can be reconfigured.",
                status=500,
            )
        env.update(wg)

        # Host-level defaults: fill only when absent, so a box that has been
        # tuned by hand keeps its settings.
        for key, value in {
            "STORAGE_ROOT": "/mnt/ssd",
            "PUID": "1000",
            "PGID": "1000",
            "TZ": _detect_timezone(),
        }.items():
            if not env.get(key):
                env[key] = value

        # VPN keys, by contrast, are applied AUTHORITATIVELY. They have to
        # stay mutually consistent with the config that was just uploaded:
        # a VPN_PORT_FORWARDING=on or a WIREGUARD_ENDPOINT_IP left over from
        # a previous provider is precisely what stops gluetun from starting.
        #
        # Country: the form's value, else the one this box already uses (the
        # Content Manager's VPN-country setting writes it — a Reconfigure
        # with a fresh .conf must not silently move the box back to the
        # Netherlands), else the historical default.
        country = ((request.form.get("country") or "").strip()
                   or (env.get("VPN_COUNTRIES") or "").strip()
                   or vpn_settings.DEFAULT_COUNTRY)
        env.update(_vpn_provider_env(provider, wg, country=country))

        try:
            _write_env_file(SERVICES_ENV, env)
        except Exception as e:
            return error_response(
                "env_write_failed",
                f"Could not write {SERVICES_ENV}: {e}",
                status=500,
            )

        # Start the long-running setup script DETACHED, as root (it was
        # `sudo -n setup_services.sh` before — same privilege, now in its own
        # cgroup). Prune stale terminal-state entries first.
        _prune_terminal_jobs(media_browser_jobs)
        job_id, err = _launch_maintenance_job(
            "mb-setup", [str(SETUP_SERVICES_SCRIPT)], as_root=True)
        if err:
            return err
        media_browser_jobs[job_id] = _new_mb_job_view(time.time())

        return success_response(
            data={
                "job_id": job_id,
                "detected_provider": detected,
                "provider": provider,
                "port_forwarding": _vpn_supports_port_forwarding(provider),
            },
            message="Media Browser setup started",
        )

    @app.post("/admin/media-browser/detect-provider")
    @require_csrf
    def media_browser_detect_provider():  # type: ignore[no-redef]
        """Preview which VPN provider a config looks like, without applying it.

        Lets the setup panel default its provider dropdown to the detected
        value the moment a .conf is dropped, so the operator sees — and can
        correct — the choice BEFORE anything is written to services/.env.

        Parses only; writes nothing and starts no job.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp

        config_text = ""
        if "file" in request.files and request.files["file"].filename:
            try:
                config_text = request.files["file"].read().decode(
                    "utf-8", errors="replace")
            except Exception as e:
                return error_response("invalid_wireguard_config",
                                      f"Could not read uploaded file: {e}",
                                      status=400)
        else:
            config_text = (request.form.get("config_text") or "").strip()

        if not config_text:
            return error_response(
                "invalid_wireguard_config", "No WireGuard config provided",
                status=400)

        try:
            wg = _parse_wireguard_config(config_text)
        except ValueError as e:
            return error_response(
                "invalid_wireguard_config",
                f"Could not parse WireGuard config: {e}", status=400)

        detected = _detect_vpn_brand(config_text, wg)
        provider = (detected if detected in _VPN_AUTO_NATIVE_PROVIDERS
                    else _VPN_PROVIDER_CUSTOM)
        return success_response(data={
            "detected_provider": detected,
            "provider": provider,
            "port_forwarding": _vpn_supports_port_forwarding(provider),
            "choices": _vpn_provider_choices(),
        })

    @app.get("/admin/media-browser/setup-status/<job_id>")
    def media_browser_setup_status(job_id):  # type: ignore[no-redef]
        """Return last N log lines + status for a setup job.

        On unknown job_id (e.g. server restart cleared in-memory state), returns
        success with status='unknown' rather than 404 so the frontend can fall
        back to the generic /admin/media-browser/status endpoint.
        """
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
        job = media_browser_jobs.get(job_id)
        if not job:
            # Not in memory — e.g. this Flask restarted mid-setup (which
            # setup_services.sh itself causes). The job's own record on disk
            # still knows; rebuild from it.
            meta = detached.meta(job_id)
            if meta and meta.get("kind") == "mb-setup":
                started = meta.get("started_ts")
                job = media_browser_jobs.setdefault(
                    job_id, _new_mb_job_view(
                        started if isinstance(started, (int, float)) else time.time()))
        if not job:
            return success_response(data={
                "status": "unknown",
                "log_lines": [],
                "exit_code": None,
                "started_at": None,
                "elapsed_sec": 0,
            })

        with _mb_jobs_lock:  # concurrent pollers must not split the cursor
            _refresh_media_browser_job(job_id, job)

        log = job["log"]
        tail = log[-_MB_LOG_TAIL_LINES:] if len(log) > _MB_LOG_TAIL_LINES else list(log)

        return success_response(data={
            "status": job["status"],
            "log_lines": tail,
            "exit_code": job["exit_code"],
            "started_at": job["started_at"],
            "elapsed_sec": int(time.time() - job["started_ts"]),
        })

    @app.get("/admin/media-browser/credentials")
    def media_browser_credentials():  # type: ignore[no-redef]
        """Return Radarr / Prowlarr API keys + qBit admin password.

        Operators use these to SSH-tunnel into the Pi and admin the services
        directly (Radarr at :7878, Prowlarr at :9696, qBit at :8080).

        Lazy-loaded by the frontend — only fetched when the operator opens
        the "Show credentials" expander, never on routine status polls, to
        avoid leaking secrets into background traffic.
        """
        if (resp := _check_media_browser_gates()):
            return resp

        try:
            env = _read_env_file(SERVICES_ENV)
        except EnvFileReadError as e:
            return error_response("env_read_failed", str(e), status=500)
        radarr_key = env.get("RADARR_API_KEY", "").strip()
        prowlarr_key = env.get("PROWLARR_API_KEY", "").strip()
        qbit_password = env.get("QBITTORRENT_ADMIN_PASSWORD", "").strip()

        # Treat unset / placeholder values as "not ready". setup_services.sh
        # writes __WILL_BE_SET_AFTER_FIRST_START__ initially and then patches
        # in the real keys once Radarr+Prowlarr have generated them.
        def _is_placeholder(value: str) -> bool:
            return (
                not value
                or value.startswith("__WILL_BE_SET_AFTER_FIRST_START__")
                or value.startswith("__")
            )

        if _is_placeholder(radarr_key) or _is_placeholder(prowlarr_key) or _is_placeholder(qbit_password):
            return error_response(
                "credentials_not_ready",
                "Setup not yet complete",
                status=400,
            )

        # DELIBERATELY does not return the key/password VALUES.
        #
        # The Content Manager has no authentication — it is reachable by any
        # device on the customer's LAN, and GET /admin/csrf-token hands a token
        # to anyone who asks, so the CSRF token authenticates nobody. This
        # endpoint was therefore disclosing the qBittorrent admin password and
        # both API keys to any LAN client: a guest phone, a compromised IoT
        # device, or a malicious page via DNS rebinding.
        #
        # Nothing is lost by withholding them. The docstring's own use case is
        # "operators use these to SSH-tunnel into the Pi" — and anyone with the
        # shell access that presupposes can simply read services/.env directly.
        # So the values only ever helped someone who already had them, while
        # exposing them to everyone who did not.
        #
        # NOTE this is disclosure control, not authentication. The admin
        # surface still needs a real credential; that is a separate designed
        # change (a setup PIN shown on the kiosk screen, exchanged for a
        # session cookie). It deliberately must NOT reuse the phone-remote
        # pairing cookie: the Content Manager is opened from a laptop, which
        # never pairs, and first_boot.sh wipes paired_remotes.json — so a
        # pairing gate would 401 every fresh unit and break the documented
        # no-SSH setup workflow.
        return success_response(data={
            "radarr_url": "http://localhost:7878",
            "prowlarr_url": "http://localhost:9696",
            "qbittorrent_url": "http://localhost:8080",
            "qbittorrent_admin_username": env.get("QBITTORRENT_ADMIN_USERNAME", "admin"),
            "credentials_hint": (
                "Read secrets on the box: "
                "sudo cat /opt/magic_dingus_box/services/.env"
            ),
        })

    # Published for the route modules registered after this one
    ctx.SERVICES_DIR = SERVICES_DIR
    ctx.SERVICES_ENV = SERVICES_ENV
    ctx._check_media_browser_gates = _check_media_browser_gates
