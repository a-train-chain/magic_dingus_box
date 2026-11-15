"""static/tube_rows.json (the Tube rows for the web page) against the C++ headers.

The file is generated from the C++ tables (app/tube_rows_json.h). This reads the
headers' TEXT, so it guards the file on a machine where test_app_unit is not built.
Regenerate: MDB_WRITE_TUBE_ROWS_JSON=1 ./build/test_app_unit "[tube_rows_json]"
"""
from __future__ import annotations

import importlib.util
import json
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
TUBE = REPO / "magic_dingus_box_cpp" / "src" / "tube"
DATA = json.loads((REPO / "magic_dingus_box" / "web" / "static" / "tube_rows.json").read_text("utf-8"))


def _load_parity():
    path = REPO / "magic_dingus_box_cpp" / "scripts" / "dev" / "check_tube_parity.py"
    spec = importlib.util.spec_from_file_location("check_tube_parity", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


PARITY = _load_parity()
ROWS_H = (TUBE / "tube_rows.h").read_text("utf-8")
LOOKS_H = (TUBE / "tube_looks.h").read_text("utf-8")
GROUPS_H = (TUBE / "tube_groups.h").read_text("utf-8")


def _block(pattern: str, text: str) -> str:
    m = re.search(pattern, text, re.S)
    assert m, f"header block not found: {pattern}"
    return m.group(1)


def test_keys_and_looks_match_the_headers():
    tables = PARITY.parse_tables(ROWS_H, LOOKS_H)
    keys = tables["keys"]
    assert len(keys) == 44
    assert [r["key"] for r in DATA["rows"]] == keys
    assert [lk["key"] for lk in DATA["looks"]] == tables["look_names"]
    assert tables["masks"] == [c["key"] for c in DATA["mask"]["choices"]]
    assert len(tables["looks"]) == 6
    for look in DATA["looks"]:
        values, mask = tables["looks"][look["key"]]
        assert list(look["values"].keys()) == keys
        assert list(look["values"].values()) == values
        # The header spells the mask by enum name; the config word is the same string.
        assert look["mask"] == mask


def test_labels_and_hints_match_kRowInfo():
    block = _block(r"kRowInfo\{\{(.*?)\}\};", GROUPS_H)
    triples = re.findall(r'\{"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\}', block)
    assert len(triples) == 44
    assert [(r["label"], *r["hint"]) for r in DATA["rows"]] == triples


def test_groups_match_kGroups():
    block = _block(r"kGroups\{\{(.*?)\}\};", GROUPS_H)
    entries = re.findall(
        r'\{"(\w+)",\s*"([^"]+)",\s*(true|false),\s*detail::rows_of\(([^)]*)\)\}', block, re.S)
    assert len(entries) == 10
    assert len(DATA["groups"]) == 10
    for (key, label, has_mask, rows), group in zip(entries, DATA["groups"]):
        want = (["mask"] if has_mask == "true" else []) + re.findall(r"Effect::(\w+)", rows)
        assert group["key"] == key
        assert group["label"] == label
        assert group["rows"] == want
        assert group["title"] == label[0] + label[1:].lower()


def test_steps():
    for row in DATA["rows"]:
        assert row["step"] == (10 if row["key"] in ("lineSize", "maskSize") else 5), row["key"]


def test_engines():
    assert [e["key"] for e in DATA["engines"]] == ["classic", "enhanced", "tube"]
    assert [e["word"] for e in DATA["engines"]] == ["Classic", "Enhanced", "Tube"]
