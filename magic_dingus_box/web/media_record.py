"""Record a just-published video in the per-video store (media_info.json).

Shared by the upload routes (admin_media) and the package import
(admin_playlist_import); not a route module, so both can import it.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path
from typing import Any, Callable, Optional

# Same dual import form as admin.py: flat when tests put web/ on sys.path,
# package-relative in production.
try:
    import media_info
    import media_probe
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from . import media_info, media_probe


def is_four_by_three(aspect: Any) -> bool:
    """True when a display aspect is within 2 % (relative) of 4:3 — the
    kiosk's video::is_four_by_three and the Content Manager's cropControl."""
    if (not isinstance(aspect, (int, float)) or isinstance(aspect, bool)
            or not math.isfinite(aspect) or aspect <= 0):
        return False
    return abs(aspect / (4 / 3) - 1) <= 0.02


def record_media_info(data_dir: Path, published: Path,
                      fit_mode: Optional[str] = None,
                      crop: Optional[bool] = None,
                      poke: Optional[Callable[[], None]] = None) -> bool:
    """Probe `published` and write its length and displayed aspect to the
    store. Never raises: the video is already on disk, and a failed write of
    this best-effort store must not fail the upload/import (the kiosk then
    plays it cropped, the default). A file the probe cannot read gets no
    length or shape; only an explicit "not cropped" choice (below) is still
    written for it — its shape is not known to be 4:3, the kiosk learns it
    from the stream, and the Content Manager shows the checkbox for any
    crop: false. Returns True when the store was written; `poke` (when
    given) is called once after a successful write.

    `fit_mode` is an upload's Framing choice and sets the video's starting
    Crop: 'fit' writes crop: false; 'crop' clears it (absent = cropped —
    never written as true), so a stale entry left by a deleted video of the
    same name cannot hand its setting on. `crop=False` (an imported package's
    stored setting) writes crop: false. Both None leaves an existing setting
    alone.

    Never crop: false for a video whose probed shape is 4:3 (within 2 %,
    is_four_by_three): there it changes nothing but the size inside a
    bezel, and no control is offered for a 4:3 video to undo it — so the
    choice is cleared instead, like 'crop'."""
    try:
        key = media_info.media_key(published, data_dir)
        if not key:
            print(f"[media] no media info recorded for {published.name}: "
                  "outside the data directory", file=sys.stderr)
            return False
        uncropped = (fit_mode == 'fit') if fit_mode is not None else (crop is False)
        probe = media_probe.probe_source(published)
        if probe is None:
            if not uncropped:
                print(f"[media] no media info recorded for {published.name}: "
                      "could not probe it", file=sys.stderr)
                return False
            print(f"[media] could not probe {published.name}: recording only "
                  "its Crop choice (off)", file=sys.stderr)
            fields = {'crop': False}
        else:
            aspect = probe['display_aspect']
            fields = {'duration_s': probe['duration_s'],
                      'display_aspect': aspect}
            if uncropped:
                fields['crop'] = None if is_four_by_three(aspect) else False
            elif fit_mode is not None:
                fields['crop'] = None
        media_info.update(data_dir, key, **fields)
    except Exception as e:
        print(f"[media] could not record media info for "
              f"{published.name}: {e}", file=sys.stderr)
        return False
    if poke is not None:
        poke()
    return True
