"""Bring media_info.json back after an updater older than the store deleted it.

The per-video store (media_info.json: each video's Crop choice, length and
shape) arrived in 2026-11. Every updater since then excludes it from its
rsyncs, but update.sh v1.11.0 and older do not: their install rsync's
--delete removes it, because the release tarball has no such file. A box
meets one of those updaters again after a rollback to an older release
(the restored tree carries that release's update.sh): the next update
forward deletes the store, and with it every Crop choice, which nothing
can re-derive (media_backfill re-probes lengths and shapes only).

Those same old updaters back up the whole tree INCLUDING the store right
before their install rsync (their backup rsync has no exclude for it
either), into <backup dir>/magic_dingus_box_cpp/data/media_info.json. So
the copy the install deleted is in the backup, current to the moment of
the install. Once per web-admin process, create_app starts
start_recovery_thread(), which merges that copy back:

  * only entries and fields the live store LACKS are added; a value the
    live store has is never changed (anything written since the backup —
    by the kiosk, an upload, the back-fill — wins);
  * a playlist-path entry is restored only when its video file still
    exists (no entry for a video deleted after the backup); a film's
    movie:<tmdb_id> entry has no file to check and is restored as-is;
  * each backup copy is merged at most ONCE: its sha256 is recorded in the
    live store under RECORD_KEY (both writers keep unknown top-level keys).
    The current updater never refreshes the backup's copy (its backup rsync
    excludes the store, so an old copy just lingers there), so the record
    keeps a stale copy from ever being merged over newer choices. The
    record lives and dies with the live store: when an old updater deletes
    the store again it has just taken a new backup, whose copy is merged.
  * it waits while an install is in progress (the OTA marker beside the
    backup): the backup is the restore source then, and the tree is about
    to change.

Writes go through media_info's lock and atomic write, the protocol the
kiosk shares; the kiosk is poked to re-read when anything was restored.
Module level and route-free (it imports nothing from admin).
"""
from __future__ import annotations

import datetime
import hashlib
import json
import os
import sys
import threading
import time
from pathlib import Path
from typing import Any, Callable, Mapping, Optional

# Same dual import form as admin.py: flat when tests put web/ on sys.path,
# package-relative in production.
try:
    import media_info
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from . import media_info

# Top-level key in media_info.json naming the backup copy last merged.
RECORD_KEY = "ota_backup_merged"
MOVIE_PREFIX = "movie:"
MARKER_POLL_S = 10.0


def backup_dir(env: Mapping[str, str] = os.environ,
               home: Optional[Path] = None) -> Path:
    """update.sh's BACKUP_DIR: MAGIC_BACKUP_DIR, else ~/.magic_dingus_box_backup
    (the web admin and the updater it starts share HOME)."""
    override = env.get("MAGIC_BACKUP_DIR")
    if override:
        return Path(override)
    return (home if home is not None else Path.home()) / ".magic_dingus_box_backup"


def ota_marker(backup: Path, env: Mapping[str, str] = os.environ) -> Path:
    """update.sh's OTA_MARKER: MAGIC_OTA_MARKER, else <backup dir>.ota_in_progress."""
    override = env.get("MAGIC_OTA_MARKER")
    if override:
        return Path(override)
    return Path(str(backup).rstrip("/") + ".ota_in_progress")


def backup_store(backup: Path) -> Path:
    return Path(backup) / "magic_dingus_box_cpp" / "data" / media_info.FILE_NAME


def _video_still_there(data_dir: Path, key: str) -> bool:
    if key.startswith(MOVIE_PREFIX):
        return True
    # Only a plain relative path inside the data directory, as media_key
    # writes them ("media/x.mp4"): no "..", no absolute or unnormalized key.
    if media_info.media_key(Path(data_dir) / key, data_dir) != key:
        return False
    return (Path(data_dir) / key).is_file()


def merge_from_backup(data_dir: Path, backup: Path,
                      now: Callable[[], datetime.datetime] = (
                          lambda: datetime.datetime.now(datetime.timezone.utc))
                      ) -> int:
    """Merge the backup's copy of the store into the live one (rules in the
    module docstring). Returns how many entries gained something; 0 when
    there is no usable copy, it was merged before, or nothing was missing.
    Raises OSError when the live store cannot be written."""
    data_dir = Path(data_dir)
    try:
        raw = backup_store(backup).read_bytes()
    except OSError:
        return 0  # no backup, or one taken by an updater that excludes it
    digest = hashlib.sha256(raw).hexdigest()
    try:
        copy = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return 0
    if not isinstance(copy, dict) or not isinstance(copy.get("videos"), dict):
        return 0
    with media_info._Lock(data_dir):
        root = media_info._read_root(data_dir)
        record = root.get(RECORD_KEY)
        if isinstance(record, dict) and record.get("sha256") == digest:
            return 0
        videos = root["videos"]
        restored = 0
        for key, saved in copy["videos"].items():
            if not isinstance(key, str) or not isinstance(saved, dict) or not saved:
                continue
            current = videos.get(key)
            if current is not None and not isinstance(current, dict):
                continue  # not ours to interpret; the kiosk leaves it too
            if not _video_still_there(data_dir, key):
                continue
            entry = dict(current or {})
            added = {f: v for f, v in saved.items() if f not in entry}
            if not added:
                continue
            entry.update(added)
            videos[key] = entry
            restored += 1
        root[RECORD_KEY] = {
            "sha256": digest,
            "merged_at": now().strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        media_info._write(data_dir, root)
    return restored


def run_recovery(data_dir: Path, backup: Optional[Path] = None,
                 marker: Optional[Path] = None,
                 wait: Callable[[float], Any] = time.sleep,
                 poke: Optional[Callable[[], None]] = None) -> int:
    """Wait out an install in progress, then merge_from_backup. Calls `poke`
    when anything was restored. Never raises."""
    backup = backup if backup is not None else backup_dir()
    marker = marker if marker is not None else ota_marker(backup)
    while marker.exists():
        wait(MARKER_POLL_S)
    try:
        restored = merge_from_backup(data_dir, backup)
    except Exception as e:
        print(f"[media] could not restore media_info.json from the update "
              f"backup: {e}", file=sys.stderr)
        return 0
    if restored:
        print(f"[media] restored the Crop/length/shape of {restored} video(s) "
              f"from the update backup ({backup_store(backup)})", flush=True)
        if poke is not None:
            poke()
    return restored


def start_recovery_thread(data_dir: Path,
                          poke: Optional[Callable[[], None]] = None, *,
                          run: Optional[Callable[..., int]] = None
                          ) -> threading.Thread:
    """run_recovery(data_dir, poke=poke) once, in a daemon thread (it may
    wait on the store lock, and on an install in progress). `run` is a test
    seam."""
    job = run or run_recovery
    t = threading.Thread(target=lambda: job(Path(data_dir), poke=poke),
                         name="media-info-recovery", daemon=True)
    t.start()
    return t
