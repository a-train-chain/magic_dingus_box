"""media_record.record_media_info: what a just-published video writes to the
per-video store (media_info.json), shared by the upload routes and the
package import.

A 4:3 video draws the same picture cropped or not, so neither the kiosk nor
the Content Manager offers it a Crop control: a stored crop:false on one
could never be undone (and inside a bezel it would shrink the picture). Only
a shape KNOWN to differ from 4:3 by more than 2 % (relative, the kiosk's
video::is_four_by_three) is stored uncropped.
"""
from __future__ import annotations

from pathlib import Path

import pytest

import media_info
import media_probe
from media_record import is_four_by_three, record_media_info


def _video(data: Path, name: str = "a.mp4") -> Path:
    (data / "media").mkdir(parents=True, exist_ok=True)
    f = data / "media" / name
    f.write_bytes(b"not really a video")
    return f


def _probe(monkeypatch, aspect, duration=10.0):
    result = None if aspect is None else {
        "width": 1, "height": 1, "sar": 1.0, "rotation": 0,
        "duration_s": duration, "display_aspect": aspect}
    monkeypatch.setattr(media_probe, "probe_source", lambda path: result)


@pytest.mark.parametrize("aspect", [1.3333, 1.3599, 1.3068])
def test_show_everything_on_a_4_3_video_stores_no_crop(tmp_path, monkeypatch,
                                                       aspect):
    f = _video(tmp_path)
    _probe(monkeypatch, aspect)
    assert record_media_info(tmp_path, f, fit_mode="fit") is True
    assert media_info.load(tmp_path) == {
        "media/a.mp4": {"duration_s": 10.0, "display_aspect": aspect}}


def test_show_everything_on_a_4_3_video_clears_a_stale_crop(tmp_path,
                                                            monkeypatch):
    f = _video(tmp_path)
    media_info.update(tmp_path, "media/a.mp4", crop=False)
    _probe(monkeypatch, 1.3333)
    assert record_media_info(tmp_path, f, fit_mode="fit") is True
    assert "crop" not in media_info.load(tmp_path)["media/a.mp4"]


@pytest.mark.parametrize("aspect", [1.7778, 1.3605, 1.3066, 0.5625, 2.39])
def test_show_everything_on_another_shape_stores_crop_false(tmp_path,
                                                            monkeypatch, aspect):
    f = _video(tmp_path)
    _probe(monkeypatch, aspect)
    assert record_media_info(tmp_path, f, fit_mode="fit") is True
    assert media_info.load(tmp_path)["media/a.mp4"]["crop"] is False


def test_a_packages_crop_false_on_a_4_3_video_is_not_stored(tmp_path,
                                                            monkeypatch):
    f = _video(tmp_path)
    _probe(monkeypatch, 1.3333)
    assert record_media_info(tmp_path, f, crop=False) is True
    assert "crop" not in media_info.load(tmp_path)["media/a.mp4"]


def test_a_packages_crop_false_on_a_wide_video_is_stored(tmp_path, monkeypatch):
    f = _video(tmp_path)
    _probe(monkeypatch, 1.7778)
    assert record_media_info(tmp_path, f, crop=False) is True
    assert media_info.load(tmp_path)["media/a.mp4"]["crop"] is False


@pytest.mark.parametrize("aspect, expected", [
    (4 / 3, True), (1.3599, True), (1.3068, True),
    (1.3605, False), (1.3066, False), (16 / 9, False),
    (None, False), (0, False), (-1.3333, False), (float("nan"), False),
    (float("inf"), False), ("1.3333", False), (True, False),
])
def test_is_four_by_three_matches_the_kiosk(aspect, expected):
    assert is_four_by_three(aspect) is expected


# ---------------------------------------------------------------------------
# A published file the probe cannot read. Its length and shape are unknown,
# but a "Show everything" choice is still the owner's: it is recorded, so the
# kiosk (which can learn the shape from the stream) and the Content Manager
# (which shows the checkbox for a crop: false whose shape is unknown) honour
# it and can undo it.
# ---------------------------------------------------------------------------

def test_unprobeable_show_everything_still_records_the_choice(tmp_path,
                                                              monkeypatch):
    f = _video(tmp_path)
    _probe(monkeypatch, None)
    pokes = []
    assert record_media_info(tmp_path, f, fit_mode="fit",
                             poke=lambda: pokes.append(1)) is True
    assert media_info.load(tmp_path) == {"media/a.mp4": {"crop": False}}
    assert pokes == [1]


def test_unprobeable_packaged_crop_false_is_recorded(tmp_path, monkeypatch):
    f = _video(tmp_path)
    _probe(monkeypatch, None)
    assert record_media_info(tmp_path, f, crop=False) is True
    assert media_info.load(tmp_path) == {"media/a.mp4": {"crop": False}}


@pytest.mark.parametrize("kwargs", [{"fit_mode": "crop"}, {}, {"crop": None}])
def test_unprobeable_without_a_fit_choice_writes_nothing(tmp_path, monkeypatch,
                                                         kwargs):
    f = _video(tmp_path)
    _probe(monkeypatch, None)
    pokes = []
    assert record_media_info(tmp_path, f, poke=lambda: pokes.append(1),
                             **kwargs) is False
    assert not (tmp_path / media_info.FILE_NAME).exists()
    assert pokes == []


def test_outside_the_data_dir_writes_nothing_even_for_fit(tmp_path, monkeypatch):
    data = tmp_path / "data"
    data.mkdir()
    f = _video(tmp_path / "elsewhere")
    _probe(monkeypatch, 1.7778)
    assert record_media_info(data, f, fit_mode="fit") is False
    assert not (data / media_info.FILE_NAME).exists()
