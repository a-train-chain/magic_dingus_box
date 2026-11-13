"""Record a just-published video in the per-video store (media_info.json).

Shared by the upload routes (admin_media) and the package import
(admin_playlist_import); not a route module, so both can import it.
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Callable, Optional

# Same dual import form as admin.py: flat when tests put web/ on sys.path,
# package-relative in production.
try:
    import media_info
    import media_probe
except ImportError:  # pragma: no cover - exercised by whichever form runs
    from . import media_info, media_probe


def record_media_info(data_dir: Path, published: Path,
                      fit_mode: Optional[str] = None,
                      crop: Optional[bool] = None,
                      poke: Optional[Callable[[], None]] = None) -> bool:
    """Probe `published` and write its length and displayed aspect to the
    store. Never raises: the video is already on disk, and a failed write of
    this best-effort store must not fail the upload/import (the kiosk then
    plays it cropped, the default). A file the probe cannot read gets nothing
    written. Returns True when the store was written; `poke` (when given) is
    called once after a successful write.

    `fit_mode` is an upload's Framing choice and sets the video's starting
    Crop: 'fit' writes crop: false; 'crop' clears it (absent = cropped —
    never written as true), so a stale entry left by a deleted video of the
    same name cannot hand its setting on. `crop=False` (an imported package's
    stored setting) writes crop: false. Both None leaves an existing setting
    alone."""
    try:
        key = media_info.media_key(published, data_dir)
        probe = media_probe.probe_source(published) if key else None
        if probe is None:
            print(f"[media] no media info recorded for {published.name}: "
                  + ("could not probe it" if key else
                     "outside the data directory"), file=sys.stderr)
            return False
        fields = {'duration_s': probe['duration_s'],
                  'display_aspect': probe['display_aspect']}
        if fit_mode is not None:
            fields['crop'] = False if fit_mode == 'fit' else None
        elif crop is False:
            fields['crop'] = False
        media_info.update(data_dir, key, **fields)
    except Exception as e:
        print(f"[media] could not record media info for "
              f"{published.name}: {e}", file=sys.stderr)
        return False
    if poke is not None:
        poke()
    return True
