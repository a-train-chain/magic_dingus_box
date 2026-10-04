"""Support diagnostics bundle: one .zip with what support asks for first.

What goes in (each file capped, the whole bundle kept well under ~20 MB):
VERSION, board model, uptime / memory / disk, `systemctl status` of the
kiosk, web, audio and Media Browser units, the last journal lines of each,
the kernel log tail, the last Box health result, kiosk_status.json, the
RetroArch launcher log (+ .1), the pairing audit log and `docker ps`.

What NEVER goes in: services/.env, flask_secret.key, paired_remotes.json,
the TMDB key file, NetworkManager connection files (Wi-Fi PSKs). None of
those paths is read into the bundle at all — .env and the key files are
read only to learn the exact values to scrub. Every text file then goes
through redact.Redactor (known values + secret shapes + whole-line drop).
kiosk_status.json gets field-level treatment: its text_input.buffer is
whatever is being typed on the TV keyboard RIGHT NOW, which includes the
Wi-Fi password screen.

Every command is a fixed argv; nothing from the request reaches one.
"""
from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
import tempfile
import time
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable, Optional

try:
    from redact import Redactor, env_secret_values
except ImportError:  # pragma: no cover - package form
    from .redact import Redactor, env_secret_values

JOURNAL_LINES = 2000
KERNEL_LINES = 500
FILE_CAP_BYTES = 2 * 1024 * 1024
TOTAL_CAP_BYTES = 18 * 1024 * 1024
CMD_TIMEOUT_S = 25

STATUS_UNITS = (
    "magic-dingus-box-cpp.service",
    "magic-dingus-web.service",
    "magic-dingus-audio.service",
    "magic-dingus-services.service",
    "magic-dingus-storage-attach.service",
    "gluetun-cascade-restart.service",
    "magic-dingus-auto-blocklist.timer",
    "magic-dingus-missing-search.timer",
    "magic-dingus-smoke-test.timer",
    "qbit-port-sync.timer",
)

JOURNAL_UNITS = (
    "magic-dingus-box-cpp.service",
    "magic-dingus-web.service",
    "magic-dingus-audio.service",
    "magic-dingus-services.service",
    "magic-dingus-auto-blocklist.service",
    "magic-dingus-missing-search.service",
    "magic-dingus-smoke-test.service",
    "qbit-port-sync.service",
    "gluetun-cascade-restart.service",
    "magic-dingus-storage-attach.service",
)

# Never read into the bundle, by name, wherever they turn up. Checked on
# every file path the builder copies, so a future edit that points a
# "log" entry at one of these fails closed.
FORBIDDEN_NAMES = frozenset({
    ".env", "flask_secret.key", "paired_remotes.json", "pairing_session.json",
    "tmdb_api_key", "tmdb.key", "text_input_queue.jsonl",
})


def _default_run(argv: list, timeout: int = CMD_TIMEOUT_S) -> tuple:
    """(returncode, combined output). Never raises."""
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                           stdin=subprocess.DEVNULL, errors="replace")
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, f"[timed out after {timeout}s]"
    except OSError as e:
        return 127, f"[could not run {argv[0]}: {e.strerror or e}]"


def _tail_bytes(data: bytes, cap: int) -> bytes:
    if len(data) <= cap:
        return data
    cut = data[-cap:]
    nl = cut.find(b"\n")
    if 0 <= nl < 4096:
        cut = cut[nl + 1:]
    return b"[... earlier lines dropped to keep the bundle small ...]\n" + cut


def scrub_kiosk_status(raw: str) -> str:
    try:
        doc = json.loads(raw)
    except ValueError:
        return "[kiosk_status.json was not valid JSON]\n"
    ti = doc.get("text_input") if isinstance(doc, dict) else None
    if isinstance(ti, dict) and "buffer" in ti:
        ti["buffer"] = "[REDACTED]" if ti.get("buffer") else ""
    return json.dumps(doc, indent=1, sort_keys=True) + "\n"


class BundleBuilder:
    def __init__(self, *, data_dir: Path, install_dir: Path,
                 known_secrets: Iterable[str] = (),
                 run: Optional[Callable] = None,
                 which: Optional[Callable[[str], Optional[str]]] = None,
                 home: Optional[Path] = None,
                 extra_files: Optional[dict] = None,
                 proc_root: Path = Path("/proc")):
        self.data_dir = Path(data_dir)
        self.install_dir = Path(install_dir)
        self.redactor = Redactor(known_secrets)
        self.run = run or _default_run
        self.which = which or shutil.which
        self.home = Path(home) if home else Path.home()
        self.extra_files = extra_files or {}
        self.proc_root = Path(proc_root)
        self.total = 0
        self.manifest = []

    # ------------------------------------------------------------ helpers
    def _add(self, zf: zipfile.ZipFile, arcname: str, text: str) -> None:
        data = self.redactor.text(text, strict=True).encode("utf-8", "replace")
        data = _tail_bytes(data, FILE_CAP_BYTES)
        if self.total + len(data) > TOTAL_CAP_BYTES:
            self.manifest.append(f"{arcname}: SKIPPED (bundle size limit reached)")
            return
        self.total += len(data)
        zf.writestr(arcname, data)
        self.manifest.append(f"{arcname}: {len(data)} bytes")

    def _cmd(self, argv: list, sudo_first: bool = False) -> str:
        if sudo_first:
            rc, out = self.run(["sudo", "-n", *argv])
            if rc == 0 or (out.strip() and "password is required" not in out
                           and "a terminal is required" not in out):
                return f"$ {' '.join(argv)}\n{out}"
        rc, out = self.run(argv)
        return f"$ {' '.join(argv)}\n{out}"

    def _read_file(self, path: Path) -> Optional[str]:
        if path.name in FORBIDDEN_NAMES:
            raise ValueError(f"refusing to bundle {path.name}")
        try:
            with open(path, "rb") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - FILE_CAP_BYTES))
                return f.read().decode("utf-8", "replace")
        except OSError:
            return None

    # -------------------------------------------------------------- build
    def build(self, out_path: Path) -> Path:
        started = time.time()
        with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
            self._system(zf)
            self._units(zf)
            self._journals(zf)
            self._files(zf)
            self._docker(zf)
            readme = (
                "Magic Dingus Box diagnostics bundle\n"
                f"Created: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%SZ')}\n"
                f"Build time: {time.time() - started:.1f}s\n\n"
                "Passwords, keys, tokens, Wi-Fi passwords and paired-phone\n"
                "records are NOT included; anything resembling one in a log\n"
                "line was replaced with [REDACTED].\n\n"
                "Contents:\n  " + "\n  ".join(self.manifest) + "\n")
            zf.writestr("README.txt", readme)
        return out_path

    def _system(self, zf) -> None:
        version = self._read_file(self.install_dir / "VERSION") or "unknown\n"
        model = self._read_file(self.proc_root / "device-tree" / "model")
        model = (model or "unknown (not a Raspberry Pi?)").replace("\x00", "").strip()
        parts = [
            f"version: {version.strip()}",
            f"board:   {model}",
            f"python:  {platform.python_version()}",
            "",
            self._cmd(["uname", "-a"]),
            self._cmd(["uptime"]),
            self._cmd(["free", "-m"]),
            self._cmd(["df", "-h"]),
            self._cmd(["vcgencmd", "measure_temp"]),
            self._cmd(["vcgencmd", "get_throttled"]),
            self._cmd(["systemctl", "--failed", "--no-pager"]),
            self._cmd(["systemctl", "list-timers", "--all", "--no-pager"]),
        ]
        self._add(zf, "system.txt", "\n".join(parts))

    def _units(self, zf) -> None:
        out = [self._cmd(["systemctl", "status", "--no-pager", "-l", "-n", "20", u])
               for u in STATUS_UNITS]
        self._add(zf, "systemctl_status.txt", "\n".join(out))

    def _journals(self, zf) -> None:
        for unit in JOURNAL_UNITS:
            text = self._cmd(["journalctl", "-u", unit, "-n", str(JOURNAL_LINES),
                              "--no-pager", "-o", "short-iso"], sudo_first=True)
            self._add(zf, f"journal/{unit.replace('.service', '')}.txt", text)
        self._add(zf, "journal/kernel.txt",
                  self._cmd(["journalctl", "-k", "-n", str(KERNEL_LINES),
                             "--no-pager", "-o", "short-iso"], sudo_first=True))

    def _files(self, zf) -> None:
        health = self._read_file(self.data_dir / "box_health_last.json")
        if health is not None:
            self._add(zf, "box_health_last.json", health)
        status = self._read_file(self.data_dir / "kiosk_status.json")
        if status is not None:
            self._add(zf, "kiosk_status.json", scrub_kiosk_status(status))
        for name in ("retroarch_launcher.log", "retroarch_launcher.log.1"):
            text = self._read_file(self.home / name)
            if text is not None:
                self._add(zf, f"logs/{name}", text)
        audit = self._read_file(self.data_dir / "pairing_audit.log")
        if audit is not None:
            self._add(zf, "logs/pairing_audit.log", audit)
        for arcname, path in self.extra_files.items():
            text = self._read_file(Path(path))
            if text is not None:
                self._add(zf, arcname, text)

    def _docker(self, zf) -> None:
        if not self.which("docker"):
            return
        self._add(zf, "docker_ps.txt", self._cmd(
            ["docker", "ps", "-a", "--format",
             "table {{.Names}}\t{{.Image}}\t{{.Status}}\t{{.RunningFor}}"],
            sudo_first=True))


def known_secret_values(install_dir: Path, data_dir: Path,
                        extra_paths: Iterable[Path] = ()) -> list:
    """Exact secret values on this box, for scrubbing. Read-only; a file we
    cannot read simply contributes nothing (the shape rules still apply)."""
    values = []
    try:
        values += env_secret_values(
            (Path(install_dir) / "services" / ".env").read_text(encoding="utf-8", errors="replace"))
    except OSError:
        pass
    for p in (Path(data_dir) / "flask_secret.key", *extra_paths):
        try:
            v = Path(p).read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            continue
        if v:
            values.append(v)
    env_secret = os.environ.get("FLASK_SECRET_KEY")
    if env_secret:
        values.append(env_secret)
    for name in ("MDB_QBIT_PASS", "MAGIC_ADMIN_TOKEN", "TMDB_API_KEY", "MDB_TMDB_API_KEY"):
        if os.environ.get(name):
            values.append(os.environ[name])
    return values


def bundle_filename(device_name: str) -> str:
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in (device_name or "box"))
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return f"{safe[:40] or 'box'}_diagnostics_{stamp}.zip"


def build_to_tempfile(builder: BundleBuilder, tmp_dir: Optional[str] = None):
    """Build into a temp file and return an open binary handle positioned at
    0. The file is unlinked BEFORE returning: the handle keeps the data
    alive while it is streamed, and nothing is left on the card even if the
    download is abandoned or the service restarts mid-transfer."""
    fd, path = tempfile.mkstemp(prefix="mdb_diag_", suffix=".zip", dir=tmp_dir)
    os.close(fd)
    try:
        builder.build(Path(path))
        handle = open(path, "rb")
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    return handle
