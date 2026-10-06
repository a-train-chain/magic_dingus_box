"""OTA update routes (/admin/update/*): version, check, channel, install,
status and rollback. Install/rollback run detached (detached_jobs.py)
through the shared maintenance-job launcher in admin.create_app(). Also the
OTA input validators (_OTA_VERSION_RE — keep in step with update.sh's
VERSION_RE and release.yml's tag check).
"""
from __future__ import annotations

import collections
import json
import os
import posixpath
import re
import socket
import subprocess
import threading
import time
from typing import Optional
from urllib.parse import urlsplit

from flask import jsonify, request

# Same dual import form as admin.py: flat when tests put web/ on
# sys.path, package-relative in production.
try:
    from admin_common import error_response, strip_ansi, success_response
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import error_response, strip_ansi, success_response


# OTA install inputs. See install_update() for why `version` is load-bearing.
# re.ASCII + explicit [0-9]: the pattern must not admit non-ASCII digits.
# X.Y.Z, or a beta-channel prerelease X.Y.Z-beta.N — the same grammar as
# update.sh's VERSION_RE and release.yml's tag check. Nothing here compares
# versions; update.sh's version_cmp is the only comparator.
_OTA_VERSION_RE = re.compile(
    r"[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}(?:-beta\.[0-9]{1,6})?", re.ASCII)
# The OTA update channels `update.sh channel` understands (stable = default).
_OTA_CHANNELS = ("stable", "beta")
# A release asset name: a plain filename. No "/", no "%" (so no encoded
# separators or dot-segments), and it may not be "." or "..".
_OTA_ASSET_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,199}", re.ASCII)


def _ota_download_url_ok(url: str, repo: str, version: str) -> bool:
    """True iff `url` is exactly one of the shapes a TAGGED v<version>
    release of `repo` is served from. Anything else — another repo, another
    tag, a branch archive, dot-segments, a query — is refused.

    Accepted (all https, exact host, path compared verbatim — a path that
    normalizes differently from how it is written is rejected outright):
      github.com/<repo>/releases/download/v<ver>/<asset>   (release asset;
                                     what `update.sh check` emits first)
      api.github.com/repos/<repo>/tarball/v<ver>           (tarball_url
                                     fallback of `update.sh check`)
      codeload.github.com/<repo>/tar.gz/[refs/tags/]v<ver> (tarball_url's
                                     redirect target)
      github.com/<repo>/archive/refs/tags/v<ver>.tar.gz    (UI "Source code")
    """
    try:
        parts = urlsplit(url)
        if (parts.scheme != "https" or parts.query or parts.fragment
                or parts.username is not None or parts.password is not None
                or parts.port is not None):  # .port raises on a bad port
            return False
    except ValueError:
        return False
    path = parts.path
    if not path or posixpath.normpath(path) != path:
        return False
    tag = f"v{version}"
    host = parts.hostname or ""
    if host == "github.com":
        prefix = f"/{repo}/releases/download/{tag}/"
        if path.startswith(prefix):
            return bool(_OTA_ASSET_RE.fullmatch(path[len(prefix):]))
        return path == f"/{repo}/archive/refs/tags/{tag}.tar.gz"
    if host == "api.github.com":
        return path == f"/repos/{repo}/tarball/{tag}"
    if host == "codeload.github.com":
        return path in (f"/{repo}/tar.gz/{tag}", f"/{repo}/tar.gz/refs/tags/{tag}")
    return False


def _has_internet(timeout: float = 3.0) -> bool:
    """Best-effort reachability probe, used ONLY to improve an error message.

    Never gates behaviour -- a false negative would just mean the operator
    gets the raw script output instead of the friendlier hint. Costs up to
    `timeout` seconds and only ever runs on a path that has already failed.
    """
    try:
        with socket.create_connection(("api.github.com", 443), timeout=timeout):
            return True
    except OSError:
        return False


def register(app, ctx) -> None:
    """Register this module's routes on `app`. Called exactly once, from
    admin.create_app(); `ctx` carries the create_app() state these routes
    share with the rest of the Content Manager (see admin.py)."""
    # create_app() state shared across route modules
    _launch_maintenance_job = ctx._launch_maintenance_job
    _prune_terminal_jobs = ctx._prune_terminal_jobs
    data_dir = ctx.data_dir
    detached = ctx.detached
    get_app_version = ctx.get_app_version
    get_device_info = ctx.get_device_info
    require_csrf = ctx.require_csrf

    # ===== OTA UPDATE MANAGEMENT =====

    # Path to update script
    # data_dir is /opt/magic_dingus_box/magic_dingus_box_cpp/data
    # update.sh is at /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/update.sh
    UPDATE_SCRIPT = data_dir.parent / "scripts" / "update.sh"

    # Parse-state cache for update jobs. NOT the source of truth: the job
    # itself runs detached (detached_jobs.py — its own systemd unit on the
    # Pi, so a magic-dingus-web restart no longer kills an OTA mid-rsync)
    # and its progress is re-derived from its on-disk log. A Flask that
    # restarted mid-job rebuilds this entry from offset 0 on the first poll.
    update_jobs: dict = {}
    _update_jobs_lock = threading.Lock()

    @app.get("/admin/update/version")
    def get_version():  # type: ignore[no-redef]
        """Get current installed version."""
        return success_response(data={
            "version": get_app_version(),
            "device_name": get_device_info().get("device_name", "Unknown")
        })

    # `update.sh check` is a GitHub API call (60/hour unauthenticated, per
    # public IP — shared by every box behind the same router) plus a process
    # spawn, and the endpoint is a plain GET: a few open tabs, the Settings
    # tab's post-update verify loop, or a hostile web page making the
    # browser hit http://<box-ip>:5000/admin/update/check in a loop each
    # spawned its own. One check at a time; concurrent callers wait for it
    # and share its answer; a SUCCESSFUL answer is reused briefly. Every
    # install/rollback start and finish drops the cache, so the verify loop
    # never sees a pre-update current_version (and both end with a web
    # restart, which empties it anyway).
    UPDATE_CHECK_CACHE_SECONDS = 30
    _update_check_lock = threading.Lock()
    _update_check_cache: dict = {"ts": 0.0, "body": None}

    def _invalidate_update_check_cache() -> None:
        _update_check_cache["body"] = None

    def _cached_update_check():
        body = _update_check_cache["body"]
        if body is not None and time.monotonic() - _update_check_cache["ts"] < UPDATE_CHECK_CACHE_SECONDS:
            return jsonify(body), 200
        return None

    @app.get("/admin/update/check")
    def check_for_update():  # type: ignore[no-redef]
        """Check if an update is available from GitHub."""
        if not UPDATE_SCRIPT.exists():
            return error_response(
                "UPDATE_NOT_AVAILABLE",
                "Update script not found. Run deploy with --build first.",
                status=500
            )
        if (cached := _cached_update_check()) is not None:
            return cached
        if not _update_check_lock.acquire(timeout=75):
            resp, status = error_response(
                "BUSY", "An update check is already running — try again in a moment.",
                status=429)
            resp.headers["Retry-After"] = "10"
            return resp, status
        try:
            if (cached := _cached_update_check()) is not None:
                return cached
            return _run_update_check()
        finally:
            _update_check_lock.release()

    def _run_update_check():
        try:
            result = subprocess.run(
                [str(UPDATE_SCRIPT), "check"],
                capture_output=True,
                text=True,
                timeout=60
            )

            if result.returncode == 0:
                # Parse JSON output from update script
                response_data = json.loads(result.stdout)
                _update_check_cache["body"] = response_data
                _update_check_cache["ts"] = time.monotonic()
                return jsonify(response_data), 200
            else:
                error_msg = strip_ansi(result.stderr) or strip_ansi(result.stdout) or "Unknown error"
                # The overwhelmingly common cause on a fresh unit is simply
                # that it has not joined Wi-Fi yet, so say so instead of
                # handing back the script's progress chatter.
                if not _has_internet():
                    error_msg = ("No internet connection. Join Wi-Fi from the "
                                 "kiosk's Settings screen, then check again.\n\n" + error_msg)
                return error_response("UPDATE_CHECK_FAILED", error_msg, status=500)

        except subprocess.TimeoutExpired:
            return error_response("TIMEOUT", "Update check timed out", status=504)
        except json.JSONDecodeError as e:
            return error_response("PARSE_ERROR", f"Failed to parse update info: {e}", status=500)
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)

    # Update channel (stable | beta). update.sh owns the file
    # (<install>/config/update_channel) and its default-to-stable rule, so
    # both endpoints go through `update.sh channel` — the same command an
    # owner runs over SSH — rather than a second reader/writer here. stdout
    # is exactly one word by contract.
    def _run_update_channel(args: list) -> str:
        result = subprocess.run(
            [str(UPDATE_SCRIPT), "channel", *args],
            capture_output=True, text=True, timeout=15)
        lines = (result.stdout or "").strip().splitlines()
        channel = lines[-1].strip() if lines else ""
        if result.returncode != 0 or channel not in _OTA_CHANNELS:
            raise RuntimeError(strip_ansi(result.stderr or "").strip()
                               or "update.sh channel failed")
        return channel

    @app.get("/admin/update/channel")
    def get_update_channel():  # type: ignore[no-redef]
        """The box's OTA update channel: "stable" (default) or "beta"."""
        if not UPDATE_SCRIPT.exists():
            return error_response("UPDATE_NOT_AVAILABLE", "Update script not found", status=500)
        try:
            channel = _run_update_channel([])
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as e:
            return error_response("INTERNAL_ERROR", f"Could not read the update channel: {e}",
                                  status=500)
        return success_response(data={"channel": channel})

    @app.post("/admin/update/channel")
    @require_csrf
    def set_update_channel():  # type: ignore[no-redef]
        """Switch the OTA update channel. Body: {"channel": "stable"|"beta"}.

        No password or PIN by owner decision (same trust model as Install
        Update itself, which is strictly more powerful). Beta only widens
        which RELEASES of this project's own repo are offered; the install
        path's repo/tag pinning is unchanged.
        """
        if not UPDATE_SCRIPT.exists():
            return error_response("UPDATE_NOT_AVAILABLE", "Update script not found", status=500)
        data = request.get_json(silent=True)
        channel = data.get("channel") if isinstance(data, dict) else None
        if not isinstance(channel, str) or channel not in _OTA_CHANNELS:
            return error_response("VALIDATION_ERROR", "channel must be \"stable\" or \"beta\"")
        try:
            channel = _run_update_channel([channel])
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as e:
            return error_response("INTERNAL_ERROR", f"Could not change the update channel: {e}",
                                  status=500)
        _invalidate_update_check_cache()  # the next check must use the new channel
        return success_response(data={"channel": channel},
                                message=f"Update channel set to {channel}")

    def _new_update_job_view(version: Optional[str]) -> dict:
        return {
            'status': 'running',
            'stage': 'preparing',
            'progress': 0,
            'message': 'Starting update...',
            'version': version,
            'new_version': None,
            '_cursor': 0,
            '_recent': collections.deque(maxlen=20),
            '_pruner_ts': time.time(),
        }

    def _refresh_update_job(job_id: str, job: dict) -> None:
        """Advance `job` with whatever the detached update.sh has written
        since the last poll (its stdout+stderr, merged, in the job log).

        Merged is fine: the parser ignores any non-JSON line, exactly as the
        old in-process pipe reader did. Driven by status polls rather than a
        reader thread, so there is nothing to lose on a Flask restart — a
        fresh process just re-parses from byte 0.
        """
        if job['status'] in ('complete', 'error') and job.get('_finished'):
            return
        result = detached.read(job_id, job['_cursor'])
        if result is None:
            return
        lines, job['_cursor'], state, rc = result
        for line in lines:
            line = line.strip()
            if not line:
                continue
            # Stripped at CAPTURE time: on failure this deque becomes
            # job['message'], which the Settings tab renders verbatim — the
            # same raw-ANSI-in-the-red-box leak the check endpoint already
            # fixed with strip_ansi. The JSON parse below is unaffected
            # (progress lines carry no colour).
            job['_recent'].append(strip_ansi(line))
            try:
                progress_data = json.loads(line)
            except json.JSONDecodeError:
                continue  # Non-JSON output, ignore
            if not isinstance(progress_data, dict):
                continue
            job['stage'] = progress_data.get('stage', job['stage'])
            job['progress'] = progress_data.get('progress', job['progress'])
            job['message'] = progress_data.get('message', job['message'])
            if progress_data.get('stage') == 'complete':
                job['status'] = 'complete'
                job['new_version'] = progress_data.get('new_version', job.get('version'))
            elif not progress_data.get('ok', True):
                job['status'] = 'error'
                err = progress_data.get('error')
                job['message'] = (err.get('message', 'Unknown error')
                                  if isinstance(err, dict) else 'Unknown error')

        if state == 'running':
            return
        job['_finished'] = True
        job['_pruner_ts'] = time.time()
        _invalidate_update_check_cache()  # VERSION may have just changed
        if job['status'] == 'complete':
            return
        tail = "\n".join(job['_recent'])
        if state == 'lost':
            job['status'] = 'error'
            job['message'] = ("The update process stopped unexpectedly (was the "
                              "box restarted?). Check the version shown here, "
                              "then retry the update or roll back."
                              + (f"\n\n{tail[-400:]}" if tail else ""))
        elif job['status'] != 'error':
            job['status'] = 'error'
            job['message'] = (tail[-500:] if tail and rc != 0
                              else 'Update failed' if rc != 0
                              else 'Update ended without confirming completion')

    @app.post("/admin/update/install")
    @require_csrf
    def install_update():  # type: ignore[no-redef]
        """Start an update installation."""
        if not UPDATE_SCRIPT.exists():
            return error_response(
                "UPDATE_NOT_AVAILABLE",
                "Update script not found",
                status=500
            )

        data = request.get_json()
        if not data:
            return error_response("VALIDATION_ERROR", "JSON body required")

        version = data.get('version')
        download_url = data.get('download_url')

        if not version or not download_url:
            return error_response("VALIDATION_ERROR", "version and download_url required")

        # `version` is NOT an inert label. update.sh interpolates it into
        # https://api.github.com/repos/<repo>/releases/tags/v${version} to
        # find the pre-compiled binary, and curl normalizes dot-segments —
        # so "1.0.8/../../../../attacker/evil/releases/tags/v1" fetched ANOTHER
        # repo's release metadata and installed ITS binary, while the
        # download_url below stayed a perfectly valid asset of our own repo.
        # It is also written verbatim into VERSION. X.Y.Z or X.Y.Z-beta.N only
        # (what `update.sh check` emits as latest_version). [0-9], not \d:
        # Python's \d matches every Unicode digit; fullmatch, not `$`, which
        # would accept a trailing newline. update.sh re-checks this
        # independently.
        if not isinstance(version, str) or not _OTA_VERSION_RE.fullmatch(version):
            return error_response(
                "VALIDATION_ERROR", "Invalid version (expected X.Y.Z or X.Y.Z-beta.N)")
        if not isinstance(download_url, str):
            return error_response("VALIDATION_ERROR", "Invalid download URL")

        # Validate the download URL. Pin it to THIS PROJECT's own repo, not
        # just "any github.com URL" — the old prefix check let any device on
        # the LAN POST a download_url pointing at an attacker-owned GitHub
        # repo and have the Pi fetch + install that tarball over itself
        # (update.sh runs with no signature check). Override via
        # MAGIC_GITHUB_REPO for forks.
        #
        # A plain string startswith() is NOT enough: curl (which update.sh
        # uses with -L) normalizes RFC-3986 dot-segments before the request,
        # so ".../a-train-chain/magic_dingus_box/../../attacker/repo/x.tar.gz"
        # would pass a prefix check yet fetch attacker/repo. And "anything
        # under the repo" was still too loose: it accepted a branch archive
        # or a DIFFERENT tag than `version`, so VERSION could claim one
        # release while another was installed. The URL must now be exactly
        # one of the shapes a tagged release of THIS version produces
        # (`update.sh check` emits the release-asset form, or the API
        # tarball_url as its fallback), with no dot-segments, query or
        # fragment, and an asset name drawn from a plain-filename alphabet so
        # percent-encoded separators (%2F, %2e) cannot ride along.
        gh_repo = os.getenv("MAGIC_GITHUB_REPO", "a-train-chain/magic_dingus_box")
        if not _ota_download_url_ok(download_url, gh_repo, version):
            return error_response(
                "VALIDATION_ERROR",
                f"Invalid download URL (must be the v{version} release of "
                f"the {gh_repo} GitHub repo)")

        # Launch detached (prune stale terminal-state entries first). The
        # job's id is the detached job's id, so a restarted Flask can find it.
        _prune_terminal_jobs(update_jobs)
        job_id, err = _launch_maintenance_job(
            "ota-install", [str(UPDATE_SCRIPT), "install", version, download_url])
        if err:
            return err
        update_jobs[job_id] = _new_update_job_view(version)
        _invalidate_update_check_cache()

        return success_response(data={'job_id': job_id}, message="Update started")

    @app.get("/admin/update/status/<job_id>")
    def update_status(job_id):  # type: ignore[no-redef]
        """Get status of an update job.

        Answered from the job's on-disk log, so it keeps working after the
        web service restarts mid-update (the job itself is unaffected — it
        runs in its own systemd unit). Still 404 for an id we have no record
        of; the Settings tab treats that as "restarted, verify the version".
        """
        job = update_jobs.get(job_id)
        if job is None:
            meta = detached.meta(job_id)
            if not meta or meta.get('kind') != 'ota-install':
                return error_response("NOT_FOUND", "Job not found", status=404)
            job = update_jobs.setdefault(job_id, _new_update_job_view(None))
        with _update_jobs_lock:  # concurrent pollers must not split the cursor
            _refresh_update_job(job_id, job)
        return success_response(data={
            'status': job['status'],
            'stage': job['stage'],
            'progress': job['progress'],
            'message': job['message'],
            'new_version': job.get('new_version')
        })

    @app.post("/admin/update/rollback")
    @require_csrf
    def rollback_update():  # type: ignore[no-redef]
        """Rollback to the previous version."""
        if not UPDATE_SCRIPT.exists():
            return error_response(
                "UPDATE_NOT_AVAILABLE",
                "Update script not found",
                status=500
            )

        # The rollback runs DETACHED (own systemd unit — see detached_jobs.py)
        # and this request waits on its log. The response contract is
        # unchanged (synchronous JSON); what changed is that the rollback no
        # longer dies with us: update.sh restarts magic-dingus-web as its
        # last act, and anything else restarting the unit mid-rsync used to
        # kill a half-restored install along with this request.
        job_id, err = _launch_maintenance_job("ota-rollback", [str(UPDATE_SCRIPT), "rollback"])
        if err:
            return err
        _invalidate_update_check_cache()

        deadline = time.monotonic() + 180  # 3 minute timeout for rollback
        cursor, lines, state, rc = 0, [], "running", None
        try:
            while True:
                result = detached.read(job_id, cursor)
                if result is None:
                    return error_response("INTERNAL_ERROR", "Rollback job vanished", status=500)
                new, cursor, state, rc = result
                lines += new
                if state != "running":
                    break
                if time.monotonic() > deadline:
                    # The job carries on in its own unit; only our wait ends.
                    return error_response("TIMEOUT", "Rollback timed out", status=504)
                time.sleep(0.25)
        except Exception as e:
            return error_response("INTERNAL_ERROR", str(e), status=500)
        finally:
            _invalidate_update_check_cache()

        json_lines = []
        for l in lines:
            l = l.strip()
            if l.startswith("{"):
                try:
                    parsed = json.loads(l)
                except json.JSONDecodeError:
                    continue
                if isinstance(parsed, dict):
                    json_lines.append(parsed)

        if state == "exited" and rc == 0:
            # The last JSON line of output is the completion message.
            if json_lines:
                return jsonify(json_lines[-1]), 200
            return success_response(message="Rollback completed")

        err_obj = json_lines[-1].get("error") if json_lines else None
        error_msg = (err_obj.get("message") if isinstance(err_obj, dict) else None) \
            or strip_ansi("\n".join(l for l in lines if not l.startswith("{"))[-500:]) \
            or ("Rollback process stopped unexpectedly" if state == "lost" else "Rollback failed")
        return error_response("ROLLBACK_FAILED", error_msg, status=500)
