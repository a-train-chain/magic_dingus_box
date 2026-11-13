"""Per-video info shared with the kiosk: <data>/media_info.json.

Same file, lock and write order as the kiosk's app/media_info.{h,cpp}:

    {"version": 1, "videos": {"media/x.mp4": {"duration_s": 212.4,
                                               "display_aspect": 1.3333,
                                               "crop": false}}}

Keys are paths relative to the data directory with "/" separators, computed
lexically (no symlink resolution). `crop` is only ever written as false;
absent means cropped. Fields this module does not know are kept untouched.

Writers (this module and the kiosk, in different processes) serialize on
flock(LOCK_EX) of the SIDECAR media_info.json.lock, held across read ->
merge -> write. The data file itself is never locked: it is replaced by
rename, so a lock on it would guard an inode about to be orphaned. Readers
take no lock because every write is an atomic rename.
"""
from __future__ import annotations

import fcntl
import json
import math
import os
from pathlib import Path
from typing import Any, Optional

from admin_common import _atomic_write_text

FILE_NAME = "media_info.json"
LOCK_NAME = "media_info.json.lock"
VERSION = 1


def media_key(path: Path, data_dir: Path) -> Optional[str]:
    """Key for `path` under `data_dir`, or None when it is outside it."""
    file = os.path.normpath(str(path))
    base = os.path.normpath(str(data_dir))
    if not file or not base or os.path.isabs(file) != os.path.isabs(base):
        return None
    rel = os.path.relpath(file, base)
    if rel == "." or rel == ".." or rel.startswith(".." + os.sep):
        return None
    return rel.replace(os.sep, "/")


def _is_number(v: Any) -> bool:
    return (isinstance(v, (int, float)) and not isinstance(v, bool)
            and math.isfinite(v))


def _read_raw(data_dir: Path) -> dict:
    """The file's videos object as-is; {} for missing/empty/corrupt."""
    try:
        text = (Path(data_dir) / FILE_NAME).read_text(encoding="utf-8")
        root = json.loads(text)
    except (OSError, ValueError):
        return {}
    if not isinstance(root, dict):
        return {}
    videos = root.get("videos")
    if not isinstance(videos, dict):
        return {}
    return {k: v for k, v in videos.items() if isinstance(v, dict)}


def load(data_dir: Path) -> dict[str, dict]:
    """key -> {duration_s?, display_aspect?, crop?}; wrong-typed fields are
    treated as absent, like the kiosk's parser."""
    out: dict[str, dict] = {}
    for key, entry in _read_raw(data_dir).items():
        clean: dict = {}
        dur, aspect = entry.get("duration_s"), entry.get("display_aspect")
        if _is_number(dur) and dur >= 0:
            clean["duration_s"] = dur
        if _is_number(aspect) and aspect > 0:
            clean["display_aspect"] = aspect
        if entry.get("crop") is False:
            clean["crop"] = False
        out[key] = clean
    return out


class _Lock:
    def __init__(self, data_dir: Path):
        Path(data_dir).mkdir(parents=True, exist_ok=True)
        self._fd = os.open(str(Path(data_dir) / LOCK_NAME),
                           os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o666)

    def __enter__(self):
        fcntl.flock(self._fd, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        try:
            fcntl.flock(self._fd, fcntl.LOCK_UN)
        finally:
            os.close(self._fd)


def _write(data_dir: Path, videos: dict) -> None:
    doc = {"version": VERSION, "videos": videos}
    _atomic_write_text(Path(data_dir) / FILE_NAME,
                       json.dumps(doc, indent=2) + "\n")


def update(data_dir: Path, key: str, **fields) -> None:
    """Merge `fields` into `key`'s entry; a None value removes that field and
    an entry left empty is removed. Unknown fields are preserved."""
    with _Lock(data_dir):
        videos = _read_raw(data_dir)
        entry = dict(videos.get(key, {}))
        for name, value in fields.items():
            if value is None:
                entry.pop(name, None)
            else:
                entry[name] = value
        if entry:
            videos[key] = entry
        else:
            videos.pop(key, None)
        _write(data_dir, videos)


def remove(data_dir: Path, key: str) -> None:
    with _Lock(data_dir):
        videos = _read_raw(data_dir)
        if key in videos:
            del videos[key]
            _write(data_dir, videos)
