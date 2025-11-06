from pathlib import Path
from magic_dingus_box.library.loader import PlaylistLibrary


def test_load_empty(tmp_path: Path):
    lib = PlaylistLibrary(tmp_path)
    assert lib.load_playlists() == []


def test_parse_minimal(tmp_path: Path):
    p = tmp_path / "a.yaml"
    p.write_text("title: X\ncurator: Y\nitems: []\n", encoding="utf-8")
    lib = PlaylistLibrary(tmp_path)
    playlists = lib.load_playlists()
    assert len(playlists) == 1
    assert playlists[0].title == "X"

