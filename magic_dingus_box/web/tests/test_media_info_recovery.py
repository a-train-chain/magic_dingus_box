"""media_info.json recovery from the update backup (media_info_recovery).

The situation it exists for: update.sh v1.11.0 and older back up the tree
(media_info.json included) and then install with an rsync --delete that has
no exclude for the store, so the store is gone and only the backup's copy
is left. These tests build that layout: <tmp>/data is the live data
directory, <tmp>/backup the updater's BACKUP_DIR.
"""
from __future__ import annotations

import fcntl
import json
import threading
import time
from pathlib import Path

import pytest

import admin
import media_info
import media_info_recovery as rec


@pytest.fixture
def box(tmp_path):
    data = tmp_path / "data"
    (data / "media" / "uploads").mkdir(parents=True)
    backup = tmp_path / "backup"
    (backup / "magic_dingus_box_cpp" / "data").mkdir(parents=True)
    return data, backup


def video(data: Path, key: str) -> None:
    p = data / key
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"\0" * 16)


def write_backup(backup: Path, videos: dict, **top) -> Path:
    p = rec.backup_store(backup)
    p.write_text(json.dumps({"version": 1, "videos": videos, **top}, indent=2))
    return p


def write_live(data: Path, videos: dict, **top) -> None:
    (data / "media_info.json").write_text(
        json.dumps({"version": 1, "videos": videos, **top}, indent=2))


def live(data: Path) -> dict:
    return json.loads((data / "media_info.json").read_text())


BACKUP_VIDEOS = {
    "media/uploads/wide.mp4": {"duration_s": 95.5, "display_aspect": 1.7778,
                               "crop": False},
    "media/uploads/classic.mp4": {"duration_s": 212.4, "display_aspect": 1.3333},
    "media/uploads/unprobeable.mp4": {"crop": False},
    "movie:603": {"crop": True},
}


# --- store missing, backup present ------------------------------------------

def test_missing_store_is_rebuilt_from_the_backup(box):
    data, backup = box
    for k in BACKUP_VIDEOS:
        if not k.startswith("movie:"):
            video(data, k)
    write_backup(backup, BACKUP_VIDEOS)

    assert rec.merge_from_backup(data, backup) == 4
    got = live(data)
    assert got["version"] == 1
    assert got["videos"] == BACKUP_VIDEOS
    # what the kiosk and the Content Manager will read
    assert media_info.load(data)["media/uploads/wide.mp4"]["crop"] is False
    assert media_info.load(data)["media/uploads/unprobeable.mp4"] == {"crop": False}
    assert got[rec.RECORD_KEY]["sha256"]


def test_run_recovery_pokes_the_kiosk_once_when_something_came_back(box, capsys):
    data, backup = box
    video(data, "media/uploads/wide.mp4")
    write_backup(backup, {"media/uploads/wide.mp4": {"crop": False}})
    pokes = []
    assert rec.run_recovery(data, backup, marker=backup.parent / "none",
                            poke=lambda: pokes.append(1)) == 1
    assert pokes == [1]
    assert "restored the Crop/length/shape of 1 video(s)" in capsys.readouterr().out
    # nothing restored -> no poke
    assert rec.run_recovery(data, backup, marker=backup.parent / "none",
                            poke=lambda: pokes.append(1)) == 0
    assert pokes == [1]


# --- a video deleted after the backup was taken ------------------------------

def test_entries_for_videos_deleted_since_the_backup_are_not_resurrected(box):
    data, backup = box
    video(data, "media/uploads/kept.mp4")
    # media/uploads/deleted.mp4 was in the backup, then deleted (by an old
    # web admin that does not know the store, or after the backup was taken)
    write_backup(backup, {
        "media/uploads/kept.mp4": {"crop": False},
        "media/uploads/deleted.mp4": {"crop": False, "duration_s": 10},
        "movie:603": {"crop": True},
    })
    assert rec.merge_from_backup(data, backup) == 2
    videos = live(data)["videos"]
    assert "media/uploads/deleted.mp4" not in videos
    assert videos["media/uploads/kept.mp4"] == {"crop": False}
    assert videos["movie:603"] == {"crop": True}  # no file to check: kept


@pytest.mark.parametrize("key", ["../outside.mp4", "media/../media/uploads/a.mp4",
                                 "/abs/a.mp4", "media/uploads/", "media"])
def test_keys_that_are_not_plain_paths_inside_the_data_dir_are_skipped(box, key):
    data, backup = box
    video(data, "media/uploads/a.mp4")
    (data.parent / "outside.mp4").write_bytes(b"x")
    write_backup(backup, {key: {"crop": False}})
    assert rec.merge_from_backup(data, backup) == 0
    assert key not in live(data)["videos"]


# --- store present, the backup older (or simply different) -------------------

def test_live_values_always_win_and_only_missing_fields_are_added(box):
    data, backup = box
    for k in ("media/uploads/wide.mp4", "media/uploads/new.mp4",
              "media/uploads/classic.mp4"):
        video(data, k)
    write_backup(backup, {
        # the back-fill re-probed it since; the Crop choice is only in the backup
        "media/uploads/wide.mp4": {"duration_s": 95.0, "display_aspect": 1.7,
                                   "crop": False},
        "media/uploads/classic.mp4": {"duration_s": 212.4},
    })
    write_live(data, {
        "media/uploads/wide.mp4": {"duration_s": 95.5, "display_aspect": 1.7778},
        "media/uploads/new.mp4": {"duration_s": 3, "future": "x"},
        "media/uploads/classic.mp4": {"duration_s": 200.0},
    }, owner_note="keep me")

    assert rec.merge_from_backup(data, backup) == 1
    got = live(data)
    assert got["videos"]["media/uploads/wide.mp4"] == {
        "duration_s": 95.5, "display_aspect": 1.7778, "crop": False}
    assert got["videos"]["media/uploads/new.mp4"] == {"duration_s": 3, "future": "x"}
    assert got["videos"]["media/uploads/classic.mp4"] == {"duration_s": 200.0}
    assert got["owner_note"] == "keep me"


def test_a_live_entry_that_is_not_an_object_is_left_alone(box):
    data, backup = box
    video(data, "media/uploads/odd.mp4")
    write_backup(backup, {"media/uploads/odd.mp4": {"crop": False}})
    write_live(data, {"media/uploads/odd.mp4": 7})
    assert rec.merge_from_backup(data, backup) == 0
    assert live(data)["videos"]["media/uploads/odd.mp4"] == 7


# --- store present and NEWER than the backup ---------------------------------

def test_a_backup_copy_is_merged_once_and_never_over_later_choices(box):
    """After the recovery, the owner changes their mind. The current updater
    never refreshes the backup's copy (its backup rsync excludes the store),
    so the same stale copy is there at every later web-admin start: it must
    not bring the old choices back."""
    data, backup = box
    video(data, "media/uploads/wide.mp4")
    video(data, "media/uploads/other.mp4")
    write_backup(backup, {"media/uploads/wide.mp4": {"crop": False},
                          "media/uploads/other.mp4": {"crop": False}})
    assert rec.merge_from_backup(data, backup) == 2

    # owner sets both back to the default (cropped): the kiosk / web drop
    # the field and the now-empty entries
    media_info.update(data, "media/uploads/wide.mp4", crop=None)
    media_info.update(data, "media/uploads/other.mp4", crop=None)
    before = (data / "media_info.json").read_bytes()
    assert media_info.load(data) == {}

    assert rec.merge_from_backup(data, backup) == 0
    assert (data / "media_info.json").read_bytes() == before


def test_the_record_survives_the_kiosk_and_web_writers(box):
    data, backup = box
    video(data, "media/uploads/wide.mp4")
    write_backup(backup, {"media/uploads/wide.mp4": {"crop": False}})
    rec.merge_from_backup(data, backup)
    record = live(data)[rec.RECORD_KEY]
    media_info.update(data, "media/uploads/wide.mp4", duration_s=5.0)
    media_info.remove(data, "media/uploads/gone.mp4")
    assert live(data)[rec.RECORD_KEY] == record


def test_a_new_backup_from_another_old_updater_install_is_merged_again(box):
    """Rolled back to an old release AGAIN and updated forward again: the old
    updater deleted the store (record included) and backed up the latest
    copy first — that copy is merged."""
    data, backup = box
    video(data, "media/uploads/a.mp4")
    video(data, "media/uploads/b.mp4")
    write_backup(backup, {"media/uploads/a.mp4": {"crop": False}})
    assert rec.merge_from_backup(data, backup) == 1
    media_info.update(data, "media/uploads/b.mp4", crop=False)   # later choice
    # the old updater: back up the live store, then delete it
    rec.backup_store(backup).write_bytes((data / "media_info.json").read_bytes())
    (data / "media_info.json").unlink()

    assert rec.merge_from_backup(data, backup) == 2
    assert media_info.load(data) == {"media/uploads/a.mp4": {"crop": False},
                                     "media/uploads/b.mp4": {"crop": False}}


# --- nothing to do -----------------------------------------------------------

def test_no_backup_copy_is_a_no_op_that_creates_nothing(box):
    data, backup = box
    assert rec.merge_from_backup(data, backup) == 0
    assert not (data / "media_info.json").exists()


@pytest.mark.parametrize("text", ["", "{oops", "[]", '{"version":1,"videos":[]}',
                                  '{"version":1}', "\xff"])
def test_an_unusable_backup_copy_changes_nothing(box, text):
    data, backup = box
    rec.backup_store(backup).write_text(text, encoding="latin-1")
    write_live(data, {"media/a.mp4": {"crop": False}})
    before = (data / "media_info.json").read_bytes()
    assert rec.merge_from_backup(data, backup) == 0
    assert (data / "media_info.json").read_bytes() == before


# --- concurrency and timing --------------------------------------------------

def test_it_takes_the_writers_lock(box):
    data, backup = box
    video(data, "media/uploads/wide.mp4")
    write_backup(backup, {"media/uploads/wide.mp4": {"crop": False}})
    fd = (data / media_info.LOCK_NAME).open("w")
    fcntl.flock(fd, fcntl.LOCK_EX)          # the kiosk, mid-write
    t = threading.Thread(target=rec.merge_from_backup, args=(data, backup))
    t.start()
    time.sleep(0.3)
    assert t.is_alive()
    assert not (data / "media_info.json").exists()
    # the kiosk's write lands; the merge must read it, not clobber it
    write_live(data, {"movie:9": {"crop": True}})
    fcntl.flock(fd, fcntl.LOCK_UN)
    fd.close()
    t.join(5)
    assert not t.is_alive()
    assert media_info.load(data) == {"movie:9": {},
                                     "media/uploads/wide.mp4": {"crop": False}}
    assert live(data)["videos"]["movie:9"] == {"crop": True}


def test_it_waits_while_an_install_is_in_progress(box):
    data, backup = box
    video(data, "media/uploads/wide.mp4")
    write_backup(backup, {"media/uploads/wide.mp4": {"crop": False}})
    marker = rec.ota_marker(backup, env={})
    marker.write_text("1.12.0 1.11.0\n")
    waits = []

    def wait(s):
        waits.append(s)
        assert not (data / "media_info.json").exists()
        if len(waits) == 3:
            marker.unlink()                  # the install finished

    assert rec.run_recovery(data, backup, marker=marker, wait=wait) == 1
    assert waits == [rec.MARKER_POLL_S] * 3


def test_a_failure_is_logged_never_raised(box, monkeypatch, capsys):
    data, backup = box
    def boom(*_a, **_k):
        raise OSError("read-only file system")
    monkeypatch.setattr(rec, "merge_from_backup", boom)
    assert rec.run_recovery(data, backup, marker=backup.parent / "none") == 0
    assert "read-only file system" in capsys.readouterr().err


# --- paths match update.sh ---------------------------------------------------

def test_paths_match_update_sh(tmp_path):
    assert rec.backup_dir(env={}, home=Path("/home/magic")) == \
        Path("/home/magic/.magic_dingus_box_backup")
    assert rec.backup_dir(env={"MAGIC_BACKUP_DIR": "/x/bk"}) == Path("/x/bk")
    assert rec.ota_marker(Path("/home/magic/.magic_dingus_box_backup"), env={}) == \
        Path("/home/magic/.magic_dingus_box_backup.ota_in_progress")
    assert rec.ota_marker(Path("/x/bk/"), env={}) == Path("/x/bk.ota_in_progress")
    assert rec.ota_marker(Path("/x/bk"), env={"MAGIC_OTA_MARKER": "/m"}) == Path("/m")
    assert rec.backup_store(Path("/b")) == \
        Path("/b/magic_dingus_box_cpp/data/media_info.json")


def test_update_sh_derives_the_same_paths():
    sh = (Path(__file__).resolve().parents[3] / "magic_dingus_box_cpp" / "scripts"
          / "update.sh").read_text()
    assert 'BACKUP_DIR="${MAGIC_BACKUP_DIR:-${HOME}/.magic_dingus_box_backup}"' in sh
    assert 'OTA_MARKER="${MAGIC_OTA_MARKER:-${BACKUP_DIR%/}.ota_in_progress}"' in sh


# --- wiring ------------------------------------------------------------------

def test_start_recovery_thread_runs_the_job_once_in_a_daemon_thread(tmp_path):
    calls = []
    t = rec.start_recovery_thread(tmp_path, poke="p",
                                  run=lambda d, poke=None: calls.append((d, poke)))
    t.join(5)
    assert t.daemon
    assert calls == [(tmp_path, "p")]


@pytest.mark.parametrize("env, starts", [("1", True), (None, True), ("0", False)])
def test_create_app_starts_the_recovery_once(temp_data_dir, monkeypatch, env, starts):
    calls = []
    monkeypatch.setattr(rec, "start_recovery_thread",
                        lambda data_dir, poke=None, **_k: calls.append((data_dir, poke)))
    if env is None:
        monkeypatch.delenv("MAGIC_MEDIA_INFO_RECOVERY", raising=False)
    else:
        monkeypatch.setenv("MAGIC_MEDIA_INFO_RECOVERY", env)
    admin.create_app(temp_data_dir, config={"TESTING": True})
    assert len(calls) == (1 if starts else 0)
    if starts:
        data_dir, poke = calls[0]
        assert data_dir == temp_data_dir
        poke()
        assert (temp_data_dir / "media_info_reload_request").exists()
