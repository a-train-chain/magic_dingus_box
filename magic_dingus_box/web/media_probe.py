"""A video's displayed shape and length, and the size of its upload master.

Module level and stdlib-only (it imports nothing from admin), so both the
upload routes (admin_media) and the Channel Guide's backfill job use it.

Masters keep the source's own aspect. The kiosk crops or fits each video per
its media_info.json setting at PLAYBACK, so the file must never be cropped or
padded: master_size() picks the smallest size that still COVERS the preset's
4:3 box (Crop-on playback is then not upscaled, unless the source is
smaller than the box or the 1080-tall cap binds), never larger than the
source, never outside 1920x1080 — the Pi 4B's hardware H.264 envelope; a
portrait 1080x1920 store is worth revisiting only after a Pi 4B test shows it
decodes in hardware.
"""
from __future__ import annotations

import json
import math
import subprocess
from fractions import Fraction
from pathlib import Path
from typing import Optional

# Quality presets: the 4:3 box a master must cover. Values are the Quality
# picker's option values in static/index.html.
#   crt      640x480   legacy / smallest files
#   crt_hd   960x720   DEFAULT — 1:1 in the kiosk's 4:3 area at 720p output
#   crt_fhd  1440x1080 max detail, ~2.25x the files
#   modern   alias of crt_hd. It was a 16:9 1280x720 frame; with nothing
#            cropped or padded any more a 16:9 source lands at 1280x720
#            under crt_hd anyway, so the API keeps accepting the name.
TRANSCODE_RESOLUTIONS = {
    'crt':     {'width': 640,  'height': 480},
    'crt_hd':  {'width': 960,  'height': 720},
    'crt_fhd': {'width': 1440, 'height': 1080},
    'modern':  {'width': 960,  'height': 720},
}

# Changing this affects NEW uploads only; existing files are untouched and
# cannot regain detail they never had.
DEFAULT_TRANSCODE = 'crt_hd'

# Outer bound for any master, either orientation's long side included.
MAX_MASTER_W, MAX_MASTER_H = 1920, 1080

PROBE_TIMEOUT_S = 30


def run_ffprobe(path) -> Optional[dict]:
    """ffprobe's JSON for the first video stream and the container, or None
    when ffprobe is missing, times out, fails or prints something unusable."""
    try:
        r = subprocess.run(
            ['ffprobe', '-v', 'error', '-select_streams', 'v:0',
             '-show_streams', '-show_format', '-of', 'json', str(path)],
            capture_output=True, text=True, timeout=PROBE_TIMEOUT_S)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    try:
        data = json.loads(r.stdout)
    except ValueError:
        return None
    return data if isinstance(data, dict) else None


def _positive_int(v) -> Optional[int]:
    try:
        n = int(v)
    except (TypeError, ValueError):
        return None
    return n if n > 0 else None


def _sar(v) -> float:
    """"32:27" -> 1.185...; ffprobe's "0:1" / "N/A" / absent mean square."""
    try:
        num, den = str(v).split(':')
        sar = float(Fraction(int(num), int(den)))
    except (ValueError, ZeroDivisionError):
        return 1.0
    return sar if sar > 0 else 1.0


def _rotation_cw(stream: dict) -> int:
    """Clockwise degrees the picture must turn to be upright, 0..359.

    ffprobe >= 5 reports a display matrix (counter-clockwise, so a phone's
    portrait clip reads -90); 4.x reports the legacy clockwise rotate tag."""
    for sd in stream.get('side_data_list') or []:
        if isinstance(sd, dict) and 'rotation' in sd:
            try:
                return int(round(-float(sd['rotation']))) % 360
            except (TypeError, ValueError):
                break
    tags = stream.get('tags')
    try:
        return int(round(float((tags or {}).get('rotate', 0)))) % 360
    except (AttributeError, TypeError, ValueError):
        return 0


def _duration(*candidates) -> Optional[float]:
    for v in candidates:
        try:
            d = float(v)
        except (TypeError, ValueError):
            continue
        if math.isfinite(d) and d >= 0:
            return round(d, 3)
    return None


def geometry(data: dict) -> Optional[dict]:
    """The displayed shape of ffprobe -of json output (pure).

    Returns None without a first video stream of positive size, else
    {width, height, sar, rotation, duration_s, display_aspect} where width,
    height and sar describe the UPRIGHT picture (swapped, sar inverted, for a
    +-90 degree rotation — what ffmpeg's auto-rotate hands its filters), so
    display_aspect = width * sar / height. duration_s is None when neither the
    container nor the stream states one."""
    streams = data.get('streams') if isinstance(data, dict) else None
    if not isinstance(streams, list) or not streams:
        return None
    stream = streams[0]
    if not isinstance(stream, dict):
        return None
    w, h = _positive_int(stream.get('width')), _positive_int(stream.get('height'))
    if w is None or h is None:
        return None
    sar = _sar(stream.get('sample_aspect_ratio'))
    rotation = _rotation_cw(stream)
    if rotation % 180 == 90:
        w, h, sar = h, w, 1.0 / sar
    fmt = data.get('format') if isinstance(data.get('format'), dict) else {}
    return {
        'width': w,
        'height': h,
        'sar': sar,
        'rotation': rotation,
        'duration_s': _duration(fmt.get('duration'), stream.get('duration')),
        'display_aspect': round(w * sar / h, 4),
    }


def probe_source(path) -> Optional[dict]:
    """geometry() of the file at `path`, or None when it cannot be read."""
    data = run_ffprobe(path)
    return geometry(data) if data is not None else None


def _even(v: float) -> int:
    # Nearest whole pixel, then down to even for libx264's 4:2:0.
    return max(2, int(round(v)) & ~1)


def master_size(disp_w: float, disp_h: float, preset: str) -> tuple[int, int]:
    """Master size for a source displayed at disp_w x disp_h.

    Scaled to cover the preset's box, never up, never outside 1920x1080; each
    side rounded to the nearest pixel, then down to even. An unknown preset
    is the default one."""
    if not (disp_w > 0 and disp_h > 0):
        raise ValueError(f"bad display size {disp_w}x{disp_h}")
    box = TRANSCODE_RESOLUTIONS.get(preset, TRANSCODE_RESOLUTIONS[DEFAULT_TRANSCODE])
    cover = max(box['width'] / disp_w, box['height'] / disp_h)
    scale = min(cover, 1.0, MAX_MASTER_W / disp_w, MAX_MASTER_H / disp_h)
    return _even(disp_w * scale), _even(disp_h * scale)
