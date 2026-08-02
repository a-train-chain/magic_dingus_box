"""Playlist reload pokes, filename derivation, and upload preconditions.

Three bugs meet in this file, all of them silent-success bugs — the ones that
matter most on an appliance whose only feedback channel is a TV across the
room:

  1. The kiosk loads playlists exactly ONCE, at boot. Every web write path
     returned a green toast while the TV kept showing the old set. The fix
     reuses the marker-file mechanism the settings restore already proved
     (see test_backup_restore.py::test_restore_pokes_the_kiosk_to_reload_settings):
     drop <data_dir>/playlists_reload_request, which the kiosk polls for,
     deletes, and then acts on. A kiosk binary that has never heard of the
     marker simply ignores the file, so the web half is safe to ship first.

  2. A playlist title made entirely of non-word characters (an emoji) slugged
     to the empty string and wrote the literal file '.yaml'. That file is a
     ghost twice over: std::filesystem gives it an EMPTY extension, so
     playlist_loader.cpp's `extension() == ".yaml"` never matches and the
     kiosk cannot see it — while the Content Manager's "*.y*ml" glob DOES
     list it, and every GET/POST/DELETE for it dies in _sanitize_filename
     because os.path.splitext('.yaml')[1] is ''. Visible, broken, and
     impossible to remove. The deletability is the half that made it a ghost,
     so it is tested here.

  3. Uploads had no free-space precondition. ENOSPC surfaced as a raw 500 —
     after the bytes were already on the card, which is the damage the check
     exists to prevent, because the running kiosk persists settings, save
     files and pairings to that same volume.

Conventions follow conftest.py: create_app(data_dir) driven through Flask's
test client, CSRF disabled by the `app` fixture, real files on a real temp
directory. Free space is the one thing that cannot be produced honestly on a
dev machine, so get_free_bytes is the only thing stubbed.
"""
from __future__ import annotations

import io
import sys
import zipfile
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).parent.parent))

import admin  # noqa: E402


MARKER_NAME = "playlists_reload_request"


# ===== package builders =====================================================

def _fake_video(name: str) -> bytes:
    """Deterministic stand-in bytes, unique per filename.

    Uniqueness is the point: the round-trip test proves each playlist item
    resolves to the file that shipped under THAT name, not merely to some
    file that happens to exist.
    """
    return b"FAKE-MP4:" + name.encode() + b"\x00" * 64


def _package_bytes(title: str, media_files=(), *, items=None,
                   playlist_name: str = "playlist.yaml") -> io.BytesIO:
    """Build a package ZIP shaped exactly like Retro Ripper's exporter.

    Layout comes from retro_ripper/src/export/mdb_exporter.py
    (_create_zip_from_temp): playlist.yaml at the archive ROOT, videos under
    media/, and item paths written as 'media/<filename>'. Keys match
    _generate_playlist_yaml's output so this exercises the real shape the box
    receives rather than a minimal one invented for the test.
    """
    if items is None:
        items = [
            {
                "title": Path(name).stem,
                "artist": "",
                "source_type": "local",
                "path": f"media/{name}",
                "start": 0,
            }
            for name in media_files
        ]
    doc = {
        "title": title,
        "curator": "",
        "playlist_type": "video",
        "loop": True,
        "items": items,
    }
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(playlist_name,
                    yaml.safe_dump(doc, sort_keys=False, allow_unicode=True))
        for name in media_files:
            zf.writestr(f"media/{name}", _fake_video(name))
    buf.seek(0)
    return buf


def _post_package(client, buf: io.BytesIO, *, overwrite: bool = False,
                  filename: str = "package.zip"):
    url = "/admin/playlists/import-package"
    if overwrite:
        url += "?overwrite=true"
    return client.post(url, data={"file": (buf, filename)},
                       content_type="multipart/form-data")


def _backup_zip(*playlist_names: str) -> io.BytesIO:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name in playlist_names:
            zf.writestr(f"playlists/{name}",
                        f"title: {Path(name).stem}\nitems: []\n")
    buf.seek(0)
    return buf


# ===== D1: every playlist-mutating path pokes the kiosk =====================

def _mutate_save(client, data_dir):
    return client.post("/admin/playlists/mix.yaml",
                       json={"title": "Mix", "items": []})


def _mutate_delete(client, data_dir):
    (data_dir / "playlists" / "mix.yaml").write_text("title: Mix\nitems: []\n")
    return client.delete("/admin/playlists/mix.yaml")


def _mutate_import_yaml(client, data_dir):
    return client.post(
        "/admin/playlists/import",
        data={"file": (io.BytesIO(b"title: Mix\nitems: []\n"), "mix.yaml")},
        content_type="multipart/form-data")


def _mutate_import_package(client, data_dir):
    return _post_package(client, _package_bytes("Mix", ["clip.mp4"]))


def _mutate_restore(client, data_dir):
    return client.post("/admin/restore",
                       data={"file": (_backup_zip("mix.yaml"), "backup.zip")},
                       content_type="multipart/form-data")


MUTATING_PATHS = [
    pytest.param(_mutate_save, id="POST /admin/playlists/<name>"),
    pytest.param(_mutate_delete, id="DELETE /admin/playlists/<name>"),
    pytest.param(_mutate_import_yaml, id="POST /admin/playlists/import"),
    pytest.param(_mutate_import_package,
                 id="POST /admin/playlists/import-package"),
    pytest.param(_mutate_restore, id="POST /admin/restore"),
]


@pytest.mark.parametrize("mutate", MUTATING_PATHS)
def test_every_playlist_mutating_path_pokes_the_kiosk(client, temp_data_dir,
                                                      mutate):
    marker = temp_data_dir / MARKER_NAME
    assert not marker.exists(), "fixture must start with no pending poke"

    resp = mutate(client, temp_data_dir)

    assert resp.status_code == 200, resp.get_data(as_text=True)
    assert resp.get_json()["ok"] is True
    assert marker.exists(), (
        "playlist changed on disk but the kiosk was never told — the TV keeps "
        "showing the old set until someone pulls the power")
    # Contract: the contents are str(time.time()). The kiosk only needs the
    # file's existence, but a parseable timestamp is what makes a stale marker
    # diagnosable by hand.
    float(marker.read_text())


def test_reading_playlists_does_not_poke(client, temp_data_dir):
    """A poke costs the kiosk a full playlist reload; only writes earn one."""
    (temp_data_dir / "playlists" / "mix.yaml").write_text(
        "title: Mix\nitems: []\n")

    assert client.get("/admin/playlists").status_code == 200
    assert client.get("/admin/playlists/mix.yaml").status_code == 200

    assert not (temp_data_dir / MARKER_NAME).exists()


def test_marker_write_failure_cannot_fail_the_request(client, temp_data_dir,
                                                      monkeypatch):
    """The poke is best-effort by design.

    Every caller has already completed the write the operator asked for.
    Turning a successful save into a 500 because a zero-byte marker could not
    be written would be strictly worse than the stale-UI bug the marker
    exists to fix — the content is on disk either way, and a reboot still
    picks it up, which is exactly the old behaviour.
    """
    real_write = admin._atomic_write_text
    exploded = []

    def exploding_write(path, content, *args, **kwargs):
        if Path(path).name == MARKER_NAME:
            exploded.append(path)
            raise OSError(28, "No space left on device")
        return real_write(path, content, *args, **kwargs)

    monkeypatch.setattr(admin, "_atomic_write_text", exploding_write)

    resp = client.post("/admin/playlists/mix.yaml",
                       json={"title": "Mix", "items": []})

    # Without this the test would pass vacuously against a build that never
    # attempts the poke at all — which is exactly the pre-fix behaviour.
    assert exploded, "the save path never tried to write the marker"
    assert resp.status_code == 200, resp.get_data(as_text=True)
    assert resp.get_json()["ok"] is True
    saved = temp_data_dir / "playlists" / "mix.yaml"
    assert saved.exists(), "the operator's playlist must still land on disk"
    assert not (temp_data_dir / MARKER_NAME).exists()


# ===== D2: an emoji-only title no longer writes the ghost '.yaml' ===========

def test_emoji_only_title_yields_a_real_filename_not_dot_yaml(client,
                                                              temp_data_dir):
    resp = _post_package(client, _package_bytes("📺", ["clip.mp4"]))

    assert resp.status_code == 200, resp.get_data(as_text=True)
    filename = resp.get_json()["data"]["playlist_filename"]

    assert Path(filename).stem, f"{filename!r} has no stem — this is the ghost"
    assert Path(filename).suffix == ".yaml"
    assert filename == "imported_playlist.yaml"

    playlists_dir = temp_data_dir / "playlists"
    assert (playlists_dir / filename).is_file()
    assert not (playlists_dir / ".yaml").exists(), (
        "the pre-fix file: invisible to the kiosk, un-deletable through the API")


def test_emoji_only_playlist_is_listed_fetchable_and_deletable(client,
                                                               temp_data_dir):
    """The half that made '.yaml' a ghost rather than merely a bad name.

    The Content Manager's glob listed it, so the operator saw a row; but the
    row's GET and DELETE both ran the name through _sanitize_filename, where
    an empty extension fails the allowed-extensions check. Permanently
    visible, permanently un-actionable. A real stem has to survive the whole
    round trip, not just the write.
    """
    resp = _post_package(client, _package_bytes("📺", ["clip.mp4"]))
    assert resp.status_code == 200, resp.get_data(as_text=True)
    filename = resp.get_json()["data"]["playlist_filename"]

    listing = client.get("/admin/playlists")
    assert listing.status_code == 200
    rows = listing.get_json()["data"]
    assert [r for r in rows if r["filename"] == filename], rows

    fetched = client.get(f"/admin/playlists/{filename}")
    assert fetched.status_code == 200, fetched.get_data(as_text=True)
    assert fetched.get_json()["data"]["title"] == "📺", (
        "the emoji title itself must survive — only the FILENAME degrades")

    deleted = client.delete(f"/admin/playlists/{filename}")
    assert deleted.status_code == 200, deleted.get_data(as_text=True)
    assert not (temp_data_dir / "playlists" / filename).exists()


def test_emoji_prefix_still_yields_the_plain_stem(client, temp_data_dir):
    """Only a title with ZERO word characters degrades. A leading emoji was
    always fine and must stay fine — this is the common case, and turning it
    into 'imported_playlist.yaml' would be a much louder regression than the
    bug being fixed."""
    resp = _post_package(client, _package_bytes("📺 MDB", ["clip.mp4"]))

    assert resp.status_code == 200, resp.get_data(as_text=True)
    assert resp.get_json()["data"]["playlist_filename"] == "MDB.yaml"
    assert (temp_data_dir / "playlists" / "MDB.yaml").is_file()


def test_single_yaml_import_falls_back_to_the_uploaded_filename(client,
                                                                temp_data_dir):
    """Same derivation, better fallback.

    The single-YAML path knows the name the operator's file already had, so
    an unusable title degrades to that rather than to the generic name. It
    used to reach _sanitize_filename as the bare string '.yaml' and come back
    as an extension error — a baffling thing to tell someone who supplied a
    perfectly good title.
    """
    resp = client.post(
        "/admin/playlists/import",
        data={"file": (io.BytesIO("title: 📺\nitems: []\n".encode()),
                       "saturday_night.yaml")},
        content_type="multipart/form-data")

    assert resp.status_code == 200, resp.get_data(as_text=True)
    assert resp.get_json()["data"]["filename"] == "saturday_night.yaml"
    assert (temp_data_dir / "playlists" / "saturday_night.yaml").is_file()


# ===== D3: the collision 409 carries both titles ============================

def test_title_collision_409_names_both_playlists(client, temp_data_dir):
    """The slug is lossy in a way the operator cannot see.

    'Rock & Roll' and 'Rock, Roll' both save as Rock_Roll.yaml. The old
    message named only the FILENAME — which is not something the operator
    ever typed — so it gave them no way to tell an accidental re-import from
    a genuine collision with a different playlist they would be destroying.
    """
    first = _post_package(client, _package_bytes("Rock & Roll", ["a.mp4"]))
    assert first.status_code == 200, first.get_data(as_text=True)
    assert first.get_json()["data"]["playlist_filename"] == "Rock_Roll.yaml"

    # The kiosk consumes the marker; clear it so the next assertion is about
    # the collision alone.
    (temp_data_dir / MARKER_NAME).unlink()

    second = _post_package(client, _package_bytes("Rock, Roll", ["b.mp4"]))

    assert second.status_code == 409, second.get_data(as_text=True)
    body = second.get_json()
    assert body["ok"] is False
    assert body["error"]["code"] == "ALREADY_EXISTS"
    assert body["error"]["details"] == {
        "existing_title": "Rock & Roll",
        "existing_filename": "Rock_Roll.yaml",
        "incoming_title": "Rock, Roll",
    }

    # A refused import must change nothing: not the incumbent playlist, not
    # the media directory, and not the kiosk's reload state.
    on_disk = yaml.safe_load(
        (temp_data_dir / "playlists" / "Rock_Roll.yaml").read_text())
    assert on_disk["title"] == "Rock & Roll"
    assert not (temp_data_dir / "media" / "b.mp4").exists()
    assert not (temp_data_dir / MARKER_NAME).exists()


def test_same_title_collision_reports_the_same_title_on_both_sides(client,
                                                                   temp_data_dir):
    """A re-import of the SAME playlist is a different question from a
    collision between two different ones, and the details must let the UI
    tell them apart without re-deriving the slug itself."""
    assert _post_package(
        client, _package_bytes("Movie Night", ["a.mp4"])).status_code == 200

    resp = _post_package(client, _package_bytes("Movie Night", ["b.mp4"]))

    assert resp.status_code == 409
    details = resp.get_json()["error"]["details"]
    assert details["existing_title"] == details["incoming_title"] == "Movie Night"
    assert details["existing_filename"] == "Movie_Night.yaml"


def test_same_title_overwrite_still_succeeds(client, temp_data_dir):
    assert _post_package(
        client, _package_bytes("Movie Night", ["a.mp4"])).status_code == 200

    resp = _post_package(client,
                         _package_bytes("Movie Night", ["a.mp4", "b.mp4"]),
                         overwrite=True)

    assert resp.status_code == 200, resp.get_data(as_text=True)
    data = resp.get_json()["data"]
    assert data["playlist_filename"] == "Movie_Night.yaml"
    assert data["playlist_title"] == "Movie Night"
    assert data["item_count"] == 2
    on_disk = yaml.safe_load(
        (temp_data_dir / "playlists" / "Movie_Night.yaml").read_text())
    assert len(on_disk["items"]) == 2
    assert (temp_data_dir / "media" / "b.mp4").is_file()
    assert (temp_data_dir / MARKER_NAME).exists()


# ===== D4: the precheck answers before the bytes move =======================

PRECHECK_URL = "/admin/playlists/import-package/precheck"


def test_precheck_answers_with_no_file_body(client, temp_data_dir):
    resp = client.post(PRECHECK_URL,
                       json={"title": "Saturday Mix", "size_bytes": 5 * 1024 * 1024})

    assert resp.status_code == 200, resp.get_data(as_text=True)
    data = resp.get_json()["data"]
    assert data["exists"] is False
    assert data["existing_filename"] is None
    assert data["existing_title"] is None
    assert data["existing_item_count"] is None
    assert isinstance(data["free_gb"], float)
    assert data["enough_space"] is True
    assert isinstance(data["max_upload_mb"], int)
    assert data["max_upload_mb"] > 0


def test_precheck_creates_nothing(client, temp_data_dir):
    """The whole point is to answer before anything moves — including before
    anything is written on the box's own side."""
    playlists_dir = temp_data_dir / "playlists"
    media_dir = temp_data_dir / "media"
    before = (sorted(p.name for p in playlists_dir.iterdir()),
              sorted(p.name for p in media_dir.iterdir()))

    resp = client.post(PRECHECK_URL,
                       json={"title": "Saturday Mix", "size_bytes": 1024})

    assert resp.status_code == 200
    assert (sorted(p.name for p in playlists_dir.iterdir()),
            sorted(p.name for p in media_dir.iterdir())) == before
    assert not (temp_data_dir / MARKER_NAME).exists()


def test_precheck_reports_the_incumbent_playlist(client, temp_data_dir):
    assert _post_package(
        client,
        _package_bytes("Saturday Mix", ["a.mp4", "b.mp4"])).status_code == 200

    resp = client.post(PRECHECK_URL,
                       json={"title": "Saturday Mix", "size_bytes": 1024})

    assert resp.status_code == 200, resp.get_data(as_text=True)
    data = resp.get_json()["data"]
    assert data["exists"] is True
    assert data["existing_filename"] == "Saturday_Mix.yaml"
    assert data["existing_title"] == "Saturday Mix"
    assert data["existing_item_count"] == 2


def test_precheck_sees_the_collision_the_slug_will_cause(client,
                                                         temp_data_dir):
    """The collision the operator cannot predict is the one worth prechecking:
    a DIFFERENT title that lands on the same file."""
    assert _post_package(
        client, _package_bytes("Rock & Roll", ["a.mp4"])).status_code == 200

    resp = client.post(PRECHECK_URL,
                       json={"title": "Rock, Roll", "size_bytes": 1024})

    data = resp.get_json()["data"]
    assert data["exists"] is True
    assert data["existing_title"] == "Rock & Roll"
    assert data["existing_filename"] == "Rock_Roll.yaml"


def test_precheck_refuses_a_file_body(client):
    """An endpoint that accepted the upload in order to tell you not to send
    the upload would answer the question too late to be worth asking."""
    resp = client.post(
        PRECHECK_URL,
        data={"file": (_package_bytes("Mix", ["a.mp4"]), "package.zip"),
              "title": "Mix"},
        content_type="multipart/form-data")

    assert resp.status_code == 400
    assert resp.get_json()["error"]["code"] == "VALIDATION_ERROR"


def test_precheck_reports_not_enough_space(client, monkeypatch):
    """Same arithmetic the import itself applies, so a green precheck and a
    507 from the real POST cannot disagree."""
    monkeypatch.setattr(admin, "get_free_bytes", lambda _p: 900 * 1024 * 1024)

    resp = client.post(PRECHECK_URL,
                       json={"title": "Huge", "size_bytes": 2 * 1024 ** 3})

    assert resp.status_code == 200, resp.get_data(as_text=True)
    data = resp.get_json()["data"]
    assert data["enough_space"] is False
    assert data["free_gb"] == pytest.approx(0.88, abs=0.01)


def test_precheck_is_optimistic_when_free_space_is_unknowable(client,
                                                              monkeypatch):
    """statvfs is not universal and the path may not exist yet. Refusing every
    upload on a box where the CHECK is unavailable would be worse than the
    ENOSPC it prevents."""
    monkeypatch.setattr(admin, "get_free_bytes", lambda _p: None)

    resp = client.post(PRECHECK_URL,
                       json={"title": "Huge", "size_bytes": 2 * 1024 ** 3})

    assert resp.status_code == 200
    assert resp.get_json()["data"]["enough_space"] is True


# ===== D5: 507 when it will not fit, JSON when it is too big ================

@pytest.mark.parametrize("post", [
    pytest.param(
        lambda c: _post_package(c, _package_bytes("Mix", ["a.mp4"])),
        id="POST /admin/playlists/import-package"),
    pytest.param(
        lambda c: c.post("/admin/upload",
                         data={"file": (io.BytesIO(b"x" * 4096), "clip.mp4")},
                         content_type="multipart/form-data"),
        id="POST /admin/upload"),
])
def test_upload_returns_507_when_the_box_cannot_fit_it(client, temp_data_dir,
                                                       monkeypatch, post):
    # 8 MB free is below the 512 MB reserve the box keeps for its own
    # persistence (settings.json, RetroArch saves, paired remotes), so any
    # upload at all must be refused.
    monkeypatch.setattr(admin, "get_free_bytes", lambda _p: 8 * 1024 * 1024)

    resp = post(client)

    assert resp.status_code == 507, resp.get_data(as_text=True)
    body = resp.get_json()
    assert body["ok"] is False
    assert body["error"]["code"] == "INSUFFICIENT_STORAGE"
    details = body["error"]["details"]
    assert isinstance(details["needed_gb"], float)
    assert isinstance(details["free_gb"], float)
    assert details["free_gb"] == pytest.approx(0.01, abs=0.01)


def test_507_is_returned_before_anything_touches_the_card(client,
                                                          temp_data_dir,
                                                          monkeypatch):
    """The check sits ahead of request.files on purpose: touching it makes
    werkzeug spool the whole multipart body onto the SD card, which is
    exactly the space being protected."""
    monkeypatch.setattr(admin, "get_free_bytes", lambda _p: 8 * 1024 * 1024)

    resp = _post_package(client, _package_bytes("Mix", ["a.mp4"]))

    assert resp.status_code == 507
    assert list((temp_data_dir / "playlists").iterdir()) == []
    assert list((temp_data_dir / "media").iterdir()) == []
    assert not (temp_data_dir / MARKER_NAME).exists()


def test_unknown_free_space_does_not_block_the_upload(client, temp_data_dir,
                                                      monkeypatch):
    monkeypatch.setattr(admin, "get_free_bytes", lambda _p: None)

    resp = _post_package(client, _package_bytes("Mix", ["a.mp4"]))

    assert resp.status_code == 200, resp.get_data(as_text=True)


# Routes whose oversize body actually reaches the 413 handler.
#
# NOT exhaustive on purpose: /admin/playlists/import and
# /admin/playlists/import-package wrap their whole body in `except Exception`
# and return the werkzeug RequestEntityTooLarge as a 500 INTERNAL_ERROR, so
# the handler never runs for the two routes an oversize upload is most likely
# to hit. That is an admin.py fix (re-raise HTTPException before the blanket
# handler), reported separately; add those routes here once it lands.
OVERSIZE_ROUTES = [
    pytest.param("/admin/upload", "clip.mp4", id="POST /admin/upload"),
    pytest.param("/admin/upload/rom/nes", "game.nes",
                 id="POST /admin/upload/rom/<system>"),
    pytest.param("/admin/restore", "backup.zip", id="POST /admin/restore"),
    pytest.param("/admin/smart-upload", "clip.mp4",
                 id="POST /admin/smart-upload"),
]


@pytest.mark.parametrize("url,filename", OVERSIZE_ROUTES)
def test_413_returns_the_standard_json_envelope(app, client, url, filename):
    """Werkzeug's own 413 body is HTML, so the Content Manager's fetch wrapper
    found no JSON, fell back to response.statusText, and showed the operator a
    bare 'REQUEST ENTITY TOO LARGE' with no mention of a size limit or what
    it is."""
    app.config["MAX_CONTENT_LENGTH"] = 1024 * 1024  # 1 MB

    resp = client.post(
        url,
        data={"file": (io.BytesIO(b"x" * (2 * 1024 * 1024)), filename)},
        content_type="multipart/form-data")

    assert resp.status_code == 413, resp.get_data(as_text=True)[:200]
    assert resp.is_json, resp.get_data(as_text=True)[:200]
    body = resp.get_json()
    assert body["ok"] is False
    assert body["error"]["code"] == "PAYLOAD_TOO_LARGE"
    assert body["error"]["details"]["max_upload_mb"] == 1
    assert "1 MB" in body["error"]["message"], (
        "the message must name the limit — the whole reason the HTML body was "
        "useless is that it never did")


# ===== D6: the round trip this branch exists for ============================

def test_package_round_trip_lands_playlist_media_and_poke(client,
                                                          temp_data_dir):
    """The whole point of the branch: an exported package reaches the TV.

    Package shape is Retro Ripper's (playlist.yaml at the root, media/ beside
    it, item paths written as 'media/<file>'). Success means three things at
    once, and any one of them missing is a silent failure the operator cannot
    diagnose from the Content Manager: the playlist parses on disk, every item
    points at a file that actually got extracted, and the kiosk was told to
    reload.
    """
    media_files = ["opening_theme.mp4", "episode_one.mp4"]

    resp = _post_package(
        client, _package_bytes("Living Room Mix", media_files))

    assert resp.status_code == 200, resp.get_data(as_text=True)
    data = resp.get_json()["data"]
    assert data["playlist_filename"] == "Living_Room_Mix.yaml"
    assert data["item_count"] == 2
    assert data["videos_imported"] == 2
    # Both skip counters are always present so the client can render zero;
    # roms_skipped used to be computed and never reported.
    assert data["videos_skipped"] == 0
    assert data["roms_skipped"] == 0

    playlist_path = temp_data_dir / "playlists" / data["playlist_filename"]
    assert playlist_path.is_file()
    on_disk = yaml.safe_load(playlist_path.read_text())
    assert on_disk["title"] == "Living Room Mix"
    assert len(on_disk["items"]) == 2

    for item, name in zip(on_disk["items"], media_files):
        assert item["path"] == f"media/{name}"
        landed = temp_data_dir / item["path"]
        assert landed.is_file(), f"{item['path']} points at nothing on the box"
        assert landed.read_bytes() == _fake_video(name), (
            "item resolves to the wrong extracted file")

    assert (temp_data_dir / MARKER_NAME).exists(), (
        "content landed but the kiosk was never told — the import is "
        "invisible until the next reboot, which is the bug this branch fixes")


def test_package_round_trip_survives_a_source_type_less_playlist(client,
                                                                 temp_data_dir):
    """playlist_loader.cpp defaults an absent source_type to 'local', so a
    third-party package that omits the key is legal input. Its videos have to
    be repointed at where they actually landed, not left on the packager's
    paths."""
    items = [{"title": "Clip", "path": "/home/packager/videos/clip.mp4"}]
    buf = _package_bytes("No Source Type", ["clip.mp4"], items=items)

    resp = _post_package(client, buf)

    assert resp.status_code == 200, resp.get_data(as_text=True)
    on_disk = yaml.safe_load(
        (temp_data_dir / "playlists" / "No_Source_Type.yaml").read_text())
    assert on_disk["items"][0]["path"] == "media/clip.mp4"
    assert (temp_data_dir / "media" / "clip.mp4").is_file()
