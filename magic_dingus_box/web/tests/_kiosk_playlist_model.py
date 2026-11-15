"""What the kiosk's PlaylistLoader::load_playlist would read from a YAML text.

A Python model of magic_dingus_box_cpp/src/app/playlist_loader.cpp, used to
check that a Content Manager rewrite never changes what a playlist MEANS on
the TV. It models yaml-cpp, not PyYAML:

  * as<std::string>() returns a scalar's SOURCE text however it is spelled
    (`artist: Yes` is "Yes", `0x1F` is "0x1F", '12' and 12 are both "12"),
    except a plain null (`~`, `null`, `Null`, `NULL`, empty), which is a Null
    node and reads as "null".
  * as<bool>() accepts y/yes/true/on and n/no/false/off in yaml-cpp's
    spellings; anything else throws (the loader then uses false).
  * as<double>() parses the scalar text; a failure gives 0.0.

The filename-derived title (title_from_rom_path, C++) is not reimplemented:
an item with no `title:` is modelled as ("<derived from>", path), which is
equal for two items exactly when the C++ would derive the same title.
"""
from __future__ import annotations

import yaml

_NULL_TAG = "tag:yaml.org,2002:null"


class _KioskScalarLoader(yaml.SafeLoader):
    """Every plain scalar is a string except the plain nulls."""


_KioskScalarLoader.yaml_implicit_resolvers = {
    first: [(tag, rx) for tag, rx in resolvers if tag == _NULL_TAG]
    for first, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()
}

_TRUE = {"y", "Y", "yes", "Yes", "YES", "true", "True", "TRUE", "on", "On", "ON"}
_FALSE = {"n", "N", "no", "No", "NO", "false", "False", "FALSE", "off", "Off", "OFF"}


def _text(value) -> str:
    if value is None:
        return "null"
    if not isinstance(value, str):
        raise TypeError(f"as<std::string> on a collection: {value!r}")
    return value


def _bool(value) -> bool:
    if isinstance(value, str) and value in _TRUE:
        return True
    return False  # _FALSE, or a failed conversion the loader catches


def _double(value) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def kiosk_view(text: str) -> dict:
    node = yaml.load(text, Loader=_KioskScalarLoader)
    out = {
        "title": _text(node["title"]) if "title" in node else "",
        "curator": _text(node["curator"]) if "curator" in node else "",
        "loop": _bool(node["loop"]) if "loop" in node else False,
        "items": [],
    }
    for item in node.get("items") or []:
        if not isinstance(item, dict):
            path = _text(item)
            entry = {"path": path, "source_type": "local",
                     "title": ("<derived from>", path), "artist": "",
                     "start": 0.0, "end": 0.0,
                     "emulator_core": "", "emulator_system": ""}
        else:
            path = _text(item["path"]) if "path" in item else ""
            entry = {
                "path": path,
                "source_type": (_text(item["source_type"])
                                if "source_type" in item else "local"),
                "title": (_text(item["title"]) if "title" in item
                          else ("<derived from>", path)),
                "artist": _text(item["artist"]) if "artist" in item else "",
                "start": _double(item["start"]) if "start" in item else 0.0,
                "end": _double(item["end"]) if "end" in item else 0.0,
                "emulator_core": (_text(item["emulator_core"])
                                  if "emulator_core" in item else ""),
                "emulator_system": (_text(item["emulator_system"])
                                    if "emulator_system" in item else ""),
            }
            if 0.0 < entry["end"] <= entry["start"]:
                entry["end"] = 0.0
            if entry["start"] < 0.0:
                entry["start"] = 0.0
        if entry["source_type"] == "emulated_game" and (
                not entry["emulator_core"] or not entry["path"]):
            continue
        out["items"].append(entry)
    return out
