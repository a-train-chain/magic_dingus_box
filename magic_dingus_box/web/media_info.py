"""Per-video info shared with the kiosk: <data>/media_info.json.

Same file, lock and write order as the kiosk's app/media_info.{h,cpp}:

    {"version": 1, "videos": {"media/x.mp4": {"duration_s": 212.4,
                                               "display_aspect": 1.3333,
                                               "crop": false}}}

Keys are paths relative to the data directory with "/" separators, computed
lexically (no symlink resolution). `crop` is only ever written as false;
absent means cropped. Like the kiosk, a write merges into the JSON it read:
fields, entries and top-level keys this module does not know (an entry that
is not an object included) are kept, and so is the file's "version". A
missing, empty or corrupt file (root or "videos" not an object) is written
fresh.

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

# Same dual import form as admin.py: flat when tests put web/ on sys.path,
# package-relative in production.
try:
    from admin_common import _atomic_write_text
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from .admin_common import _atomic_write_text

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


def _read_root(data_dir: Path) -> dict:
    """The whole file as-is, with a "videos" object (added when absent); a
    fresh root for a missing, empty or corrupt file — the kiosk's
    parse_root."""
    try:
        text = (Path(data_dir) / FILE_NAME).read_text(encoding="utf-8")
        root = json.loads(text)
    except (OSError, ValueError):
        root = None
    if not isinstance(root, dict):
        return {"version": VERSION, "videos": {}}
    if "videos" not in root:
        root["videos"] = {}
    elif not isinstance(root["videos"], dict):
        return {"version": VERSION, "videos": {}}
    return root


def _read_raw(data_dir: Path) -> dict:
    """The file's object entries as-is; {} for missing/empty/corrupt."""
    return {k: v for k, v in _read_root(data_dir)["videos"].items()
            if isinstance(v, dict)}


def load(data_dir: Path) -> dict[str, dict]:
    """key -> {duration_s?, display_aspect?, crop?}; wrong-typed fields, and
    a length or aspect that is not a positive number, are treated as absent,
    like the kiosk's parser."""
    out: dict[str, dict] = {}
    for key, entry in _read_raw(data_dir).items():
        clean: dict = {}
        dur, aspect = entry.get("duration_s"), entry.get("display_aspect")
        if _is_number(dur) and dur > 0:
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


def _write(data_dir: Path, root: dict) -> None:
    root.setdefault("version", VERSION)
    _atomic_write_text(Path(data_dir) / FILE_NAME,
                       json.dumps(root, indent=2) + "\n")


def update(data_dir: Path, key: str, **fields) -> None:
    """Merge `fields` into `key`'s entry; a None value removes that field and
    an entry left empty is removed. Unknown fields are preserved. An entry
    that is not an object is replaced when there is a value to set, and left
    as it is otherwise (the kiosk's apply_edit)."""
    with _Lock(data_dir):
        root = _read_root(data_dir)
        videos = root["videos"]
        current = videos.get(key)
        entry = dict(current) if isinstance(current, dict) else {}
        for name, value in fields.items():
            if value is None:
                entry.pop(name, None)
            else:
                entry[name] = value
        if entry:
            videos[key] = entry
        elif isinstance(current, dict):
            del videos[key]
        _write(data_dir, root)


def remove(data_dir: Path, key: str) -> None:
    with _Lock(data_dir):
        root = _read_root(data_dir)
        if key in root["videos"]:
            del root["videos"][key]
            _write(data_dir, root)


def restore(data_dir: Path, text: str) -> None:
    """Merge a backup's per-video Crop settings onto the CURRENT store, under
    the same lock the kiosk's writers take. Only `crop` crosses over (as
    false; anything else is ignored): length and shape always come from this
    box's own probes, and keys the backup lacks are left untouched. Raises
    ValueError unless `text` is a JSON object."""
    root = json.loads(text)
    if not isinstance(root, dict):
        raise ValueError("media_info.json must be a JSON object")
    backup_videos = root.get("videos")
    crops = [k for k, v in (backup_videos.items()
                            if isinstance(backup_videos, dict) else [])
             if isinstance(k, str) and isinstance(v, dict)
             and v.get("crop") is False]
    with _Lock(data_dir):
        root = _read_root(data_dir)
        videos = root["videos"]
        for key in crops:
            current = videos.get(key)
            entry = dict(current) if isinstance(current, dict) else {}
            entry["crop"] = False
            videos[key] = entry
        _write(data_dir, root)
