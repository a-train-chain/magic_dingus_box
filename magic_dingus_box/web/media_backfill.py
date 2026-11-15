"""Back-fill every video's length and shape into media_info.json.

The upload routes and the package import record a video's duration_s and
display_aspect when they publish it (media_record). Videos already on a box
before that existed — old 16:9 `modern` masters, package imports, raw
uploads — have neither, so the Content Manager offers them no Crop checkbox
and the Channel Guide cannot schedule them. Once per web-admin process,
create_app starts start_backfill_thread(): a low-priority daemon thread that
probes each video under <data>/media lacking either field, one at a time,
and fills only what is missing (a recorded Crop setting, or a recorded
shape, is never touched).

It yields to playback: while a game or movie has the services paused
(playback_services_pause.sh's marker) it waits instead of probing, and
ffprobe inherits the thread's nice 10 (os.nice is per-thread on Linux, so
the request threads keep their priority). The kiosk is poked once, at the
end, when anything was filled.

Module level and route-free (it imports nothing from admin).
"""
from __future__ import annotations

import math
import os
import sys
import threading
import time
from pathlib import Path
from typing import Any, Callable, Optional

# Same dual import form as admin.py: flat when tests put web/ on sys.path,
# package-relative in production.
try:
    import media_info
    import media_probe
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from . import media_info, media_probe

# Present while a game or movie has the media services paused
# (playback_services_pause.sh; the kiosk's game/movie quiet modes).
PAUSE_MARKER = Path("/tmp/mdb_playback_services_paused")
PAUSE_POLL_S = 5.0

# The Content Manager's video list (admin_media.list_media).
VIDEO_EXTENSIONS = ('.mp4', '.mkv', '.avi', '.mov', '.webm')

FIELDS = ('duration_s', 'display_aspect')


def _playback_paused() -> bool:
    return PAUSE_MARKER.exists()


def _positive(v: Any) -> bool:
    # What the kiosk and media_info.load accept: a positive finite number.
    return (isinstance(v, (int, float)) and not isinstance(v, bool)
            and math.isfinite(v) and v > 0)


def _videos(media_dir: Path) -> list[Path]:
    if not media_dir.is_dir():
        return []
    return sorted(p for p in media_dir.rglob("*")
                  if p.suffix.lower() in VIDEO_EXTENSIONS and p.is_file())


def run_backfill(data_dir: Path,
                 probe: Optional[Callable[[Path], Optional[dict]]] = None,
                 paused: Optional[Callable[[], bool]] = None,
                 wait: Callable[[float], Any] = time.sleep,
                 poke: Optional[Callable[[], None]] = None) -> int:
    """Probe every video under <data_dir>/media whose store entry lacks
    duration_s or display_aspect and write the missing fields. Returns how
    many entries it filled; calls `poke` once at the end when that is > 0.
    Never raises for one video: an unreadable file or a failed write is
    logged and skipped."""
    probe = probe or media_probe.probe_source
    paused = paused or _playback_paused
    data_dir = Path(data_dir)
    store = media_info.load(data_dir)
    filled = 0
    for path in _videos(data_dir / "media"):
        key = media_info.media_key(path, data_dir)
        if key is None:
            continue
        entry = store.get(key, {})
        missing = [f for f in FIELDS if f not in entry]
        if not missing:
            continue
        while paused():
            wait(PAUSE_POLL_S)
        try:
            geo = probe(path)
        except Exception as e:
            print(f"[media] back-fill could not probe {key}: {e}",
                  file=sys.stderr)
            continue
        if not isinstance(geo, dict):
            print(f"[media] back-fill could not probe {key}", file=sys.stderr)
            continue
        fields = {f: geo[f] for f in missing if _positive(geo.get(f))}
        if not fields or not path.is_file():  # nothing known, or deleted
            continue
        try:
            media_info.update(data_dir, key, **fields)
        except Exception as e:
            print(f"[media] back-fill could not record {key}: {e}",
                  file=sys.stderr)
            continue
        filled += 1
    if filled and poke is not None:
        poke()
    return filled


def start_backfill_thread(data_dir: Path,
                          poke: Optional[Callable[[], None]] = None, *,
                          run: Optional[Callable[..., int]] = None,
                          nice: Callable[[int], Any] = os.nice
                          ) -> threading.Thread:
    """Run run_backfill(data_dir, poke=poke) once, in a daemon thread at
    nice 10. `run` and `nice` are test seams."""
    data_dir = Path(data_dir)
    job = run or run_backfill

    def _body() -> None:
        try:
            nice(10)
        except OSError:
            pass  # stays at normal priority; still one probe at a time
        try:
            filled = job(data_dir, poke=poke)
        except Exception as e:
            print(f"[media] back-fill stopped: {e}", file=sys.stderr)
            return
        if filled:
            print(f"[media] back-filled the length and shape of {filled} "
                  "video(s)", flush=True)

    t = threading.Thread(target=_body, name="media-backfill", daemon=True)
    t.start()
    return t
