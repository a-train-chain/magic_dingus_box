"""Upload masters keep the source's shape (web/media_probe.py).

An upload used to be transcoded into a fixed 4:3 frame, cropped or padded,
and the original deleted, so the framing was permanent. Now the master keeps
the source's own aspect: big enough to COVER the preset's 4:3 box (so Crop-on
playback is never upscaled), never larger than the source, never outside
1920x1080 (the Pi 4B's hardware H.264 envelope), each side even for libx264.
The crop choice lives per video in media_info.json instead.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

import media_probe
from media_probe import TRANSCODE_RESOLUTIONS, geometry, master_size

REPO = Path(__file__).resolve().parents[3]


@pytest.mark.parametrize("disp,preset,expected", [
    ((1920, 1080), "crt_hd", (1280, 720)),    # 16:9 covers 960x720 at 720 tall
    ((1080, 1920), "crt_hd", (608, 1080)),    # 9:16 phone clip: capped at 1080 tall
    ((640, 480), "crt_hd", (640, 480)),       # never upscaled
    ((3840, 2160), "crt_fhd", (1920, 1080)),  # 4K covers 1440x1080 at 1080 tall
    ((853.33, 480), "crt_hd", (852, 480)),    # anamorphic DVD: whole pixels, even
    ((960, 720), "crt_hd", (960, 720)),
    ((1920, 1080), "crt", (852, 480)),
    ((2560, 1440), "crt_fhd", (1920, 1080)),
    ((1440, 2560), "crt_fhd", (608, 1080)),
])
def test_master_covers_the_preset_box_without_upscaling(disp, preset, expected):
    assert master_size(*disp, preset) == expected


def test_modern_is_an_alias_of_crt_hd():
    assert TRANSCODE_RESOLUTIONS["modern"] == TRANSCODE_RESOLUTIONS["crt_hd"]
    assert master_size(1920, 1080, "modern") == master_size(1920, 1080, "crt_hd")


@pytest.mark.parametrize("disp", [(1920, 1080), (1080, 1920), (7680, 4320),
                                  (4000, 100), (100, 4000), (1001, 667)])
@pytest.mark.parametrize("preset", ["crt", "crt_hd", "crt_fhd"])
def test_master_is_even_inside_1080p_and_never_larger_than_the_source(disp, preset):
    w, h = master_size(*disp, preset)
    assert w % 2 == 0 and h % 2 == 0
    assert 2 <= w <= 1920 and 2 <= h <= 1080
    assert w <= disp[0] and h <= disp[1]


def _stream(**over):
    s = {"codec_type": "video", "width": 1920, "height": 1080,
         "sample_aspect_ratio": "1:1"}
    s.update(over)
    return {"streams": [s], "format": {"duration": "212.400000"}}


def test_geometry_of_a_plain_stream():
    assert geometry(_stream()) == {
        "width": 1920, "height": 1080, "sar": 1.0, "rotation": 0,
        "duration_s": 212.4, "display_aspect": 1.7778}


@pytest.mark.parametrize("data", [
    # ffprobe >= 5 reports a display matrix, counter-clockwise; a phone's
    # portrait clip carries -90 (= the legacy rotate tag's 90 clockwise).
    _stream(side_data_list=[{"side_data_type": "Display Matrix",
                             "rotation": -90}]),
    _stream(tags={"rotate": "90"}),     # ffprobe 4.x
])
def test_rotated_stream_is_reported_upright(data):
    g = geometry(data)
    assert (g["width"], g["height"], g["rotation"]) == (1080, 1920, 90)
    assert g["display_aspect"] == pytest.approx(0.5625)


def test_anamorphic_stream_reports_its_display_aspect():
    g = geometry(_stream(width=720, height=480, sample_aspect_ratio="32:27"))
    assert g["sar"] == pytest.approx(32 / 27)
    assert g["display_aspect"] == pytest.approx(1.7778)


@pytest.mark.parametrize("sar", ["0:1", "N/A", None])
def test_unknown_sar_is_square(sar):
    data = _stream()
    if sar is None:
        del data["streams"][0]["sample_aspect_ratio"]
    else:
        data["streams"][0]["sample_aspect_ratio"] = sar
    assert geometry(data)["sar"] == 1.0


def test_duration_falls_back_to_the_stream_and_may_be_unknown():
    data = _stream(duration="9.5")
    data["format"] = {}
    assert geometry(data)["duration_s"] == 9.5
    del data["streams"][0]["duration"]
    assert geometry(data)["duration_s"] is None


@pytest.mark.parametrize("data", [
    {}, {"streams": []}, {"streams": [{"width": 0, "height": 1080}]},
    {"streams": [{"width": "x", "height": 1080}]}, {"streams": "nope"},
])
def test_unreadable_probe_output_is_none(data):
    assert geometry(data) is None


def test_probe_source_of_a_missing_file_is_none(tmp_path):
    assert media_probe.probe_source(tmp_path / "missing.mp4") is None


@pytest.mark.parametrize("module", ["media_probe", "admin_media"])
def test_imports_as_a_package_module_without_web_on_sys_path(module):
    # Production runs `python3 -m magic_dingus_box.web.wsgi`; web/ is not on
    # sys.path there (conftest adds it for the tests), so a flat-only import
    # would fail on first import.
    r = subprocess.run(
        [sys.executable, "-c", f"import magic_dingus_box.web.{module}"],
        cwd=str(REPO), capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


# ---------------------------------------------------------------------------
# The probe-failure fallback filter, run through the REAL ffmpeg: without a
# probe there is no master_size, so the filter alone must keep the display
# shape. force_original_aspect_ratio works from the STORED size, so an
# anamorphic source (720x480 at SAR 32:27, shown 16:9) must be brought to
# square pixels at its display width first, or it is stored 720x480 square
# (3:2) — squeezed.
# ---------------------------------------------------------------------------

def _ffmpeg_or_skip():
    import shutil
    if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
        pytest.skip("ffmpeg/ffprobe not installed")


def _encode(src_filter: str, out: Path, vf: str = None, src: Path = None):
    argv = ["ffmpeg", "-v", "error", "-y"]
    if src is None:
        argv += ["-f", "lavfi", "-i", src_filter, "-t", "0.4"]
    else:
        argv += ["-i", str(src)]
    if vf:
        argv += ["-vf", vf]
    argv += ["-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
             str(out)]
    r = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("size, sar, aspect", [
    ("720x480", "32/27", 16 / 9),      # anamorphic NTSC widescreen DVD
    ("720x576", "16/11", 20 / 11),     # anamorphic PAL (display ~1.82)
    ("720x480", "8/9", 4 / 3),         # anamorphic NTSC 4:3
    ("1280x720", "1/1", 16 / 9),       # square pixels: unchanged
    ("3840x2160", "1/1", 16 / 9),      # bounded to 1920x1080
])
def test_probe_failed_filter_keeps_the_display_shape_with_square_pixels(
        tmp_path, size, sar, aspect):
    _ffmpeg_or_skip()
    src = tmp_path / "src.mp4"
    out = tmp_path / "out.mp4"
    _encode(f"testsrc=size={size}:rate=10,setsar={sar}", src)
    _encode("", out, vf=media_probe.PROBE_FAILED_VF, src=src)
    geo = media_probe.probe_source(out)
    assert geo is not None
    assert geo["sar"] == pytest.approx(1.0), geo
    assert geo["width"] / geo["height"] == pytest.approx(aspect, rel=0.01), geo
    assert geo["width"] <= 1920 and geo["height"] <= 1080
    assert geo["width"] % 2 == 0 and geo["height"] % 2 == 0
    # Never enlarged: the stored height is at most the source's.
    assert geo["height"] <= int(size.split("x")[1])


def test_quality_labels_are_true_for_every_shape():
    # A master keeps its source's shape, so "720 tall" was false for a
    # portrait clip (608x1080 under HD 720). The values are the API's and
    # stay as they are.
    import re
    html = (Path(__file__).resolve().parents[1] / "static" / "index.html").read_text()
    select = re.search(r'<select id="targetResolution".*?</select>', html, re.S)
    assert select, "Quality picker not found"
    options = re.findall(r'<option value="([^"]+)"[^>]*>(?:<svg.*?</svg>)?\s*([^<]+)</option>',
                         select.group(0), re.S)
    assert [(v, label.strip()) for v, label in options] == [
        ("crt_hd", "HD 720 (recommended)"),
        ("crt", "Classic 480 (smallest files)")]
    assert all(v in TRANSCODE_RESOLUTIONS for v, _ in options)
    assert "tall" not in select.group(0)
