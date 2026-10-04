"""Long-running maintenance jobs that must outlive the web service.

WHY this exists: OTA install/rollback and Media Browser setup used to run as
`subprocess.Popen(..., start_new_session=True)` children of the Flask process,
on the theory that a new session "survives web restart". It does not.
magic-dingus-web.service uses systemd's default KillMode=control-group, so a
restart of that unit SIGTERMs/SIGKILLs EVERY process in its cgroup — session
leaders included. And the unit is restarted from inside these very jobs:
setup_services.sh restarts magic-dingus-web to pick up the input group, and
the unit is Restart=always, so any Flask crash mid-job took the job with it —
an OTA killed between its rsync and its rebuild leaves a half-installed box.

So on a systemd host the job runs as its OWN transient unit
(`sudo -n systemd-run --unit=mdb-<kind>-<id> --collect ...`), in its own
cgroup, and writes stdout+stderr to a log file under the data dir. The job's
progress is DERIVED from that file, never held only in memory, so a Flask
that restarted mid-job can still answer status polls for it: the caller keeps
a byte cursor and re-parses from 0 when it has no cursor (fresh process).

A wrapper appends a final `__MDB_JOB_EXIT__ <rc>` line, which is how the exit
status crosses a Flask restart. A job whose process is gone without that line
was killed — reported as "lost", never as running-forever.

Off systemd (dev Mac, CI, tests) the same wrapper runs under a plain Popen
with the same log file, so every code path above the launcher is identical.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
import uuid
from pathlib import Path
from typing import Iterable, Optional

EXIT_MARKER = "__MDB_JOB_EXIT__"
_EXIT_RE = re.compile(r"__MDB_JOB_EXIT__ (-?[0-9]+)")
_JOB_ID_RE = re.compile(r"[0-9a-f]{32}")
_KIND_RE = re.compile(r"[a-z][a-z0-9-]{0,30}")

# The wrapper: run the job with stderr merged into stdout, then record the
# exit code as the final line. `exec 2>&1` matters in Popen mode only (the
# unit already routes both streams to the log), and is harmless in both.
_WRAPPER = ('exec 2>&1; "$@"; rc=$?; '
            'printf "\\n' + EXIT_MARKER + ' %d\\n" "$rc"; exit "$rc"')

# Environment a transient unit needs from us. systemd-run starts the unit
# from PID 1's environment, NOT ours — so anything the scripts read (every
# MAGIC_* override, HOME, which update.sh builds BACKUP_DIR from, TMPDIR,
# which the web unit points at the SD card) must be passed explicitly.
_PASSTHROUGH_ENV = ("HOME", "PATH", "LANG", "LC_ALL", "TMPDIR", "PYTHONPATH",
                    "USER", "LOGNAME")

RETENTION_SECONDS = 7 * 24 * 3600
MAX_KEPT_JOBS = 40


def default_state_dir(data_dir) -> Path:
    """Where job records live: /tmp, deliberately NOT under the install tree.

    The OTA's own `rsync --delete` (tarball -> /opt/magic_dingus_box, and the
    rollback's backup -> install) would delete anything under the data dir or
    the web unit's TMPDIR (/opt/magic_dingus_box/tmp) that is not on its
    exclude lists — including the very log the running OTA is writing to.
    /tmp is outside INSTALL_DIR, shared with transient units (no PrivateTmp),
    and a RAM tmpfs on Pi OS: records only need to survive a web-service
    restart, and setup output never lands on the SD card (or in a golden
    image). Keyed by uid + data dir so test apps / dev checkouts never share
    a single-flight namespace. MAGIC_JOB_STATE_DIR overrides.
    """
    override = os.getenv("MAGIC_JOB_STATE_DIR")
    if override:
        return Path(override)
    import hashlib
    key = hashlib.sha1(str(Path(data_dir).resolve()).encode()).hexdigest()[:10]
    base = "/tmp" if os.path.isdir("/tmp") else tempfile.gettempdir()
    return Path(base) / f"mdb_jobs_{os.getuid()}_{key}"


class DetachedJobs:
    """Launches jobs and reports their state from on-disk records.

    State dir layout: <id>.json (immutable launch metadata, written before the
    process starts) and <id>.log (the job's merged output + exit marker).
    """

    def __init__(self, state_dir: Path, mode_resolver=None):
        self.state_dir = Path(os.path.abspath(state_dir))
        # Called at LAUNCH time (not construction) so a test app flipping
        # app.testing after create_app() is honoured. Returns "systemd",
        # "popen" or "auto".
        self._mode_resolver = mode_resolver or (lambda: "auto")
        self._procs: dict = {}   # job_id -> Popen (popen mode; reaps zombies)
        self._lock = threading.Lock()

    # ---------------------------------------------------------------- paths
    def _meta_path(self, job_id: str) -> Path:
        return self.state_dir / f"{job_id}.json"

    def log_path(self, job_id: str) -> Path:
        return self.state_dir / f"{job_id}.log"

    @staticmethod
    def valid_id(job_id) -> bool:
        return isinstance(job_id, str) and bool(_JOB_ID_RE.fullmatch(job_id))

    # --------------------------------------------------------------- launch
    def _use_systemd(self) -> bool:
        mode = (os.getenv("MAGIC_JOB_LAUNCHER") or self._mode_resolver() or "auto").lower()
        if mode == "popen":
            return False
        available = (shutil.which("systemd-run") is not None
                     and os.path.isdir("/run/systemd/system"))
        return available if mode in ("auto", "systemd") else False

    def launch(self, kind: str, argv: list, *, as_root: bool = False,
               cwd: Optional[str] = None, extra_env: Optional[dict] = None) -> str:
        """Start `argv` detached; return its job id. Raises OSError if the
        job could not be started by either launcher."""
        if not _KIND_RE.fullmatch(kind):
            raise ValueError(f"bad job kind {kind!r}")
        self.state_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        # It lives in /tmp: refuse a directory someone else pre-created.
        if self.state_dir.stat().st_uid != os.getuid():
            raise OSError(f"job state dir {self.state_dir} is not owned by us")
        self._prune()
        job_id = uuid.uuid4().hex
        log = self.log_path(job_id)
        # Created by US (the web user) before launch: a root-run unit then
        # APPENDS to a file we own and can read, instead of creating a
        # root-owned one. 0600: setup output can name credentials.
        fd = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        meta = {"kind": kind, "started_ts": time.time(), "argv0": Path(argv[0]).name}
        wrapped = ["/bin/bash", "-c", _WRAPPER, "mdb-job", *map(str, argv)]

        if as_root:
            # Mirror what `sudo -n <script>` (the pre-existing launch) handed
            # the script: sudo's env_reset environment plus SUDO_USER, which
            # setup_services.sh uses to chown the storage tree back to the
            # real user. Our MAGIC_* overrides never reached it through sudo
            # and must not start to now.
            env = {"HOME": "/root", "USER": "root", "LOGNAME": "root",
                   "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                   "SUDO_UID": str(os.getuid()), "SUDO_GID": str(os.getgid())}
            user = os.environ.get("USER") or os.environ.get("LOGNAME")
            if user:
                env["SUDO_USER"] = user
            if "LANG" in os.environ:
                env["LANG"] = os.environ["LANG"]
        else:
            env = {k: os.environ[k] for k in _PASSTHROUGH_ENV if k in os.environ}
            env.update({k: v for k, v in os.environ.items()
                        if k.startswith(("MAGIC_", "MDB_"))})
        if extra_env:
            env.update(extra_env)

        if self._use_systemd():
            unit = f"mdb-{kind}-{job_id[:12]}"
            cmd = ["sudo", "-n", "systemd-run", f"--unit={unit}", "--collect",
                   "--quiet", "--service-type=exec",
                   f"--property=StandardOutput=append:{log}",
                   f"--property=StandardError=append:{log}"]
            if not as_root:
                # The OTA must run as the web user, exactly as before: the
                # install tree is magic-owned and update.sh's backup lives in
                # that user's $HOME.
                cmd += [f"--uid={os.getuid()}", f"--gid={os.getgid()}"]
            if cwd:
                cmd.append(f"--working-directory={cwd}")
            cmd += [f"--setenv={k}={v}" for k, v in env.items()]
            cmd += wrapped
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
                if r.returncode == 0:
                    meta["unit"] = unit
                    self._write_meta(job_id, meta)
                    return job_id
                fallback_reason = (r.stderr or r.stdout or "").strip()[:300]
            except (OSError, subprocess.TimeoutExpired) as e:
                fallback_reason = str(e)
            # systemd-run refused (no sudo rule, polkit, ...). Degrade to the
            # pre-existing behaviour rather than refusing the operation —
            # and say so in the job's own log.
            with open(log, "a", encoding="utf-8") as f:
                f.write(f"[admin] systemd-run unavailable ({fallback_reason}); "
                        "running in-process — a web-service restart will "
                        "interrupt this job\n")

        # Pre-existing launch, unchanged: inherit our environment (sudo
        # applies its own env_reset for the root case).
        popen_argv = ["sudo", "-n", *wrapped] if as_root else wrapped
        with open(log, "ab") as out:
            proc = subprocess.Popen(
                popen_argv, stdout=out, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL, cwd=cwd,
                env={**os.environ, **(extra_env or {})},
                start_new_session=True)
        meta["pid"] = proc.pid
        with self._lock:
            self._procs[job_id] = proc
        self._write_meta(job_id, meta)
        return job_id

    def _write_meta(self, job_id: str, meta: dict) -> None:
        path = self._meta_path(job_id)
        tmp = path.with_suffix(".json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(meta, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)

    # ---------------------------------------------------------------- state
    def meta(self, job_id: str) -> Optional[dict]:
        if not self.valid_id(job_id):
            return None
        try:
            data = json.loads(self._meta_path(job_id).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return data if isinstance(data, dict) else None

    def _alive(self, job_id: str, meta: dict) -> bool:
        unit = meta.get("unit")
        if unit:
            try:
                r = subprocess.run(["systemctl", "is-active", "--quiet", unit],
                                   capture_output=True, timeout=10)
                return r.returncode == 0
            except (OSError, subprocess.TimeoutExpired):
                return True  # can't tell — never declare a live OTA dead
        with self._lock:
            proc = self._procs.get(job_id)
        if proc is not None:
            return proc.poll() is None
        pid = meta.get("pid")
        if not isinstance(pid, int) or pid <= 0:
            return False
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True

    def read(self, job_id: str, offset: int = 0):
        """Return (new_lines, new_offset, state, exit_code).

        state: "running" | "exited" | "lost". Only COMPLETE lines are
        consumed, so a poll racing a half-written line never splits it.
        Returns None for an unknown job id.
        """
        meta = self.meta(job_id)
        if meta is None:
            return None
        lines, offset, rc = self._read_from(job_id, offset)
        if rc is not None:
            return lines, offset, "exited", rc
        if self._alive(job_id, meta):
            return lines, offset, "running", None
        # Gone. Re-read once: the marker may have landed between the read
        # above and the liveness check.
        more, offset, rc = self._read_from(job_id, offset, final=True)
        lines += more
        if rc is not None:
            return lines, offset, "exited", rc
        return lines, offset, "lost", None

    def _read_from(self, job_id: str, offset: int, final: bool = False):
        try:
            with open(self.log_path(job_id), "rb") as f:
                f.seek(max(0, int(offset)))
                data = f.read()
        except OSError:
            return [], offset, None
        end = len(data) if final else data.rfind(b"\n") + 1
        chunk = data[:end]
        offset = max(0, int(offset)) + len(chunk)
        rc = None
        out = []
        for raw in chunk.decode("utf-8", errors="replace").splitlines():
            m = _EXIT_RE.fullmatch(raw.strip())
            if m:
                rc = int(m.group(1))
            else:
                out.append(raw)
        return out, offset, rc

    def finished(self, job_id: str) -> bool:
        """Cheap terminal check: the exit marker is the log's last line, so
        only the tail is read; then liveness."""
        meta = self.meta(job_id)
        if meta is None:
            return True
        try:
            with open(self.log_path(job_id), "rb") as f:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - 256))
                if EXIT_MARKER.encode() in f.read():
                    return True
        except OSError:
            return True
        return not self._alive(job_id, meta)

    def active(self, kinds: Iterable[str]) -> Optional[tuple]:
        """(job_id, kind) of a still-running job of one of `kinds`, or None.
        Survives a Flask restart: it is answered from disk + liveness."""
        kinds = set(kinds)
        try:
            metas = sorted(self.state_dir.glob("*.json"),
                           key=lambda p: p.stat().st_mtime, reverse=True)
        except OSError:
            return None
        for path in metas[:MAX_KEPT_JOBS]:
            job_id = path.stem
            meta = self.meta(job_id)
            if not meta or meta.get("kind") not in kinds:
                continue
            if not self.finished(job_id):
                return job_id, meta["kind"]
        return None

    def _prune(self) -> None:
        """Drop finished job records past retention / beyond the cap.
        A running job is never pruned."""
        try:
            metas = sorted(self.state_dir.glob("*.json"),
                           key=lambda p: p.stat().st_mtime, reverse=True)
        except OSError:
            return
        cutoff = time.time() - RETENTION_SECONDS
        for i, path in enumerate(metas):
            try:
                old = path.stat().st_mtime < cutoff
            except OSError:
                continue
            if (i >= MAX_KEPT_JOBS or old) and self.finished(path.stem):
                for p in (path, self.log_path(path.stem)):
                    try:
                        p.unlink()
                    except OSError:
                        pass
                with self._lock:
                    self._procs.pop(path.stem, None)
