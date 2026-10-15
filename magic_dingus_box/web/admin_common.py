"""Shared helpers for the Content Manager's route modules.

Response envelopes (success_response / error_response), system monitoring
(CPU, memory, disk, free bytes, systemd unit state), and the file-safety
primitives every write path uses: atomic text writes, staged upload saves,
capped ZIP text reads and filename sanitizing. Split out of admin.py
(2026-10) without behavior changes; admin.py re-exports these names.
"""
from __future__ import annotations

import os
import re
import subprocess
import tempfile
import time
import zipfile
from pathlib import Path
from typing import Any, Optional

from flask import jsonify


# ===== SYSTEM MONITORING HELPERS =====

def get_cpu_temperature() -> Optional[float]:
    """Get CPU temperature (Raspberry Pi specific)."""
    try:
        # Try thermal zone (works on most Linux including Pi)
        temp_file = Path("/sys/class/thermal/thermal_zone0/temp")
        if temp_file.exists():
            return float(temp_file.read_text().strip()) / 1000.0
    except Exception:
        pass

    try:
        # Fallback: vcgencmd (Raspberry Pi specific)
        result = subprocess.run(
            ["vcgencmd", "measure_temp"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            # Output format: temp=45.0'C
            temp_str = result.stdout.strip()
            if "temp=" in temp_str:
                return float(temp_str.split("=")[1].replace("'C", ""))
    except Exception:
        pass

    return None


def get_memory_info() -> dict:
    """Get memory usage info."""
    try:
        with open("/proc/meminfo") as f:
            meminfo = {}
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    key = parts[0].rstrip(":")
                    value = int(parts[1])  # in kB
                    meminfo[key] = value

            total = meminfo.get("MemTotal", 0)
            available = meminfo.get("MemAvailable", meminfo.get("MemFree", 0))
            used = total - available

            return {
                "total_mb": round(total / 1024, 1),
                "used_mb": round(used / 1024, 1),
                "available_mb": round(available / 1024, 1),
                "percent": round((used / total) * 100, 1) if total > 0 else 0
            }
    except Exception:
        return {}


def get_disk_info(path: str = "/") -> dict:
    """Get disk usage info for a path."""
    try:
        stat = os.statvfs(path)
        total = stat.f_blocks * stat.f_frsize
        free = stat.f_bavail * stat.f_frsize
        used = total - free

        return {
            "total_gb": round(total / (1024**3), 2),
            "used_gb": round(used / (1024**3), 2),
            "free_gb": round(free / (1024**3), 2),
            "percent": round((used / total) * 100, 1) if total > 0 else 0
        }
    except Exception:
        return {}


# One gibibyte, spelled out once so every size the API reports uses the same
# unit the disk figures in /admin/health/detailed already use.
BYTES_PER_GB = 1024 ** 3


def get_free_bytes(path) -> Optional[int]:
    """Free bytes available to an unprivileged writer at `path`.

    get_disk_info() rounds to two decimal places of a gibibyte, which is fine
    for a dashboard and useless for a precondition — 0.00 GB free and 5 MB free
    round to the same number. Preconditions need the raw count.

    Returns None when the filesystem cannot be interrogated at all (statvfs is
    Linux/macOS only, and the path may not exist yet). Callers MUST treat None
    as "unknown", not as "full": refusing every upload on a machine where the
    check itself is unavailable would be worse than the ENOSPC it prevents.
    """
    try:
        stat = os.statvfs(str(path))
        return stat.f_bavail * stat.f_frsize
    except Exception:
        return None


def get_cpu_usage() -> Optional[float]:
    """Get CPU usage percentage."""
    try:
        # Read /proc/stat twice with a small delay
        def read_cpu_stats():
            with open("/proc/stat") as f:
                line = f.readline()
                parts = line.split()
                # cpu user nice system idle iowait irq softirq
                if parts[0] == "cpu":
                    return [int(x) for x in parts[1:8]]
            return None

        stats1 = read_cpu_stats()
        if not stats1:
            return None

        time.sleep(0.1)  # Small delay
        stats2 = read_cpu_stats()
        if not stats2:
            return None

        # Calculate difference
        diff = [s2 - s1 for s1, s2 in zip(stats1, stats2)]
        total = sum(diff)
        idle = diff[3]  # idle is 4th value

        if total > 0:
            return round(((total - idle) / total) * 100, 1)
    except Exception:
        pass

    return None


def get_uptime() -> Optional[int]:
    """Get system uptime in seconds."""
    try:
        with open("/proc/uptime") as f:
            uptime_seconds = float(f.read().split()[0])
            return int(uptime_seconds)
    except Exception:
        return None


def check_service_status(service_name: str) -> str:
    """Check if a systemd service is running."""
    try:
        result = subprocess.run(
            ["systemctl", "is-active", service_name],
            capture_output=True, text=True, timeout=5
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


# ===== STANDARDIZED API RESPONSE HELPERS =====

def success_response(data: Any = None, message: str = None) -> tuple:
    """Create a standardized success response.

    Args:
        data: Optional data payload
        message: Optional success message

    Returns:
        Tuple of (response_dict, status_code)
    """
    response = {"ok": True}
    if data is not None:
        response["data"] = data
    if message:
        response["message"] = message
    return jsonify(response), 200


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(text: str) -> str:
    """Remove terminal colour codes before showing script output in the UI.

    update.sh colours its progress lines for a human at a terminal. Passing
    that straight into a JSON error made the Content Manager render
    literal escape gibberish -- observed on a fresh unit with no network:
        \u001b[0;32m[UPDATE]\u001b[0m Checking GitHub for updates...
    which tells a customer nothing about what actually went wrong.
    """
    return _ANSI_RE.sub("", text or "").strip()


def error_response(code: str, message: str, status: int = 400, details: Any = None) -> tuple:
    """Create a standardized error response.

    Args:
        code: Error code (e.g., "NOT_FOUND", "VALIDATION_ERROR")
        message: Human-readable error message
        status: HTTP status code (default 400)
        details: Optional additional error details

    Returns:
        Tuple of (response_dict, status_code)
    """
    response = {
        "ok": False,
        "error": {
            "code": code,
            "message": message
        }
    }
    if details is not None:
        response["error"]["details"] = details
    return jsonify(response), status


# Hard cap for the small text entries (playlist YAML, settings/device JSON,
# manifest) read out of a backup or playlist-package ZIP. Real ones are a
# few KB; 4 MB is ~1000x headroom. Without it, zf.read() allocated whatever
# the archive declared — and deflate shrinks a run of spaces ~1000:1, so a
# sub-MB upload could make the web service allocate gigabytes on a 1.5 GB
# Pi 4B, where the OOM killer then picks the kiosk or the web service.
_ZIP_TEXT_ENTRY_MAX = 4 * 1024 * 1024


class _ZipEntryTooLarge(ValueError):
    """A ZIP text entry exceeds _ZIP_TEXT_ENTRY_MAX."""


def _read_zip_entry_capped(zf: "zipfile.ZipFile", name: str,
                           cap: int = _ZIP_TEXT_ENTRY_MAX) -> bytes:
    """Read one ZIP entry, refusing anything over `cap` bytes.

    Two checks, because the first is the archive's own claim: the declared
    (central-directory) size, which rejects an honest oversized entry before
    any decompression; then a bounded read(cap + 1), so an archive that
    under-declares can still never make us hold more than cap + 1 bytes.
    """
    info = zf.getinfo(name)
    if info.file_size > cap:
        raise _ZipEntryTooLarge(
            f"{name} is too large ({info.file_size} bytes; limit {cap})")
    with zf.open(info) as f:
        data = f.read(cap + 1)
    if len(data) > cap:
        raise _ZipEntryTooLarge(f"{name} is too large (limit {cap} bytes)")
    return data


class _ExtractTooLarge(Exception):
    """Internal signal: a ZIP entry would push extraction past the byte cap.

    Raised from inside the streaming loop so the staging temp file is cleaned up
    by one handler rather than at each bail-out point. Never escapes the import
    endpoint.
    """


def _staged_save_upload(file_storage, dest: Path, mode: int = 0o644) -> None:
    """Save a werkzeug upload to dest via same-directory staging + os.replace.

    A direct f.save(dest) writes the final name from byte 0: an interrupted
    transfer leaves a truncated file that lists as real content (a video the
    kiosk fails to play, a ROM that fails to launch), and re-uploading over
    an existing file destroys the ORIGINAL the moment the transfer starts
    rather than when it succeeds. Staging in the same directory keeps the
    os.replace atomic (one filesystem), and the dot-prefix + .tmp suffix
    keeps the staging file out of the extension globs the listings use.
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(
        dir=str(dest.parent), prefix=f".{dest.name}.", suffix=".tmp")
    os.close(fd)
    tmp_path = Path(tmp_name)
    try:
        file_storage.save(str(tmp_path))
        os.chmod(tmp_path, mode)
        os.replace(tmp_path, dest)
    except BaseException:
        try:
            tmp_path.unlink()
        except OSError:
            pass
        raise


def _atomic_write_text(path: Path, content: str, encoding: str = "utf-8",
                       mode: int = 0o644) -> None:
    """Write `content` to `path` so an interrupted write cannot corrupt it.

    This matters more here than in an ordinary web app. The Magic Dingus Box is
    a plug-it-in appliance with no shutdown ritual — it is powered off by pulling
    the cord, routinely, and that can land in the middle of a save. A bare
    Path.write_text() truncates the target and *then* streams into it, so losing
    power part-way leaves a half-written playlist on the SD card.

    The failure is silent, which is what makes it nasty: playlist_loader.cpp
    parses each file in a try/catch and only logs to stderr on a parse error
    (magic_dingus_box_cpp/src/app/playlist_loader.cpp), so a truncated playlist
    does not surface an error anywhere the user can see — the playlist simply
    stops appearing on the kiosk.

    The sequence:
      1. Write to a temp file in the SAME directory. os.replace is only atomic
         within one filesystem, so /tmp is not a valid staging area.
      2. fsync the file, so the bytes are actually on the card before anything
         points at them.
      3. os.replace onto the target — atomic, so a reader sees either the whole
         old file or the whole new one, never a mixture.
      4. fsync the *directory*, so the rename itself survives power loss. This
         is the step most implementations omit; without it the rename can still
         be lost even though the data was synced.

    Args:
        path: Destination file.
        content: Text to write.
        encoding: Text encoding.
        mode: Permission bits. mkstemp creates 0600, which would make files
            unreadable to other accounts; 0644 matches what write_text produced
            under the default umask, so behaviour is unchanged for readers.
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    fd, tmp_name = tempfile.mkstemp(
        dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp"
    )
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "w", encoding=encoding) as fh:
            fh.write(content)
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp_path, mode)
        os.replace(tmp_path, path)
    except BaseException:
        # Never leave the staging file behind on failure — the directory these
        # land in is scanned for playlists, and the dot-prefix plus .tmp suffix
        # keeps a stray one from being mistaken for content even if unlink fails.
        try:
            tmp_path.unlink()
        except OSError:
            pass
        raise

    # Durability of the rename. Best-effort: some filesystems refuse to open a
    # directory for fsync, and failing to sync is not a reason to fail a save
    # that has already landed.
    try:
        dir_fd = os.open(str(path.parent), os.O_RDONLY)
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    except OSError:
        pass


def _sanitize_filename(name: str, allowed_extensions: Optional[list[str]] = None) -> str:
    """Sanitize filename to prevent path traversal attacks.
    
    Args:
        name: Original filename
        allowed_extensions: Optional list of allowed extensions (e.g., ['.yaml', '.yml'])
        
    Returns:
        Sanitized filename (basename only, no path separators)
        
    Raises:
        ValueError: If filename contains path separators or invalid characters
    """
    # Get basename to remove any path components
    basename = os.path.basename(name)
    
    # Reject if still contains path separators (shouldn't happen after basename, but be safe)
    if '/' in basename or '\\' in basename or '..' in basename:
        raise ValueError("Filename contains invalid path characters")
    
    # Validate extension if required
    if allowed_extensions:
        ext = os.path.splitext(basename)[1].lower()
        # A basename that IS the extension ('.yaml') is a legacy ghost: an
        # emoji-only title used to slug to '' and write that literal file.
        # splitext puts the whole thing in the STEM, so ext is '' and the
        # check below rejects it — which is why those rows were listed by
        # Path.glob (pathlib does not hide dotfiles) yet could never be
        # fetched or deleted through the API. Creation is prevented upstream
        # now by _playlist_filename_for_title, so accepting it here only ever
        # lets an operator clean one up. No separator is involved, so this
        # widens nothing for path traversal.
        if not ext and basename.lower() in allowed_extensions:
            return basename
        if ext not in allowed_extensions:
            raise ValueError(f"Filename must have one of these extensions: {', '.join(allowed_extensions)}")

    return basename
