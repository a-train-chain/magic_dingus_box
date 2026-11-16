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
INPUTS_H = (TUBE / "tube_inputs.h").read_text("utf-8")
APP_H = (REPO / "magic_dingus_box_cpp" / "src" / "app" / "app_state.h").read_text("utf-8")
JSON_H = (REPO / "magic_dingus_box_cpp" / "src" / "app" / "tube_rows_json.h").read_text("utf-8")


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


def _triples(block_pattern: str, count: int, text: str = GROUPS_H):
    triples = re.findall(r'\{"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\}', _block(block_pattern, text))
    assert len(triples) == count
    return triples


def test_mask_words_and_hints_match_kMaskChoices():
    triples = _triples(r"kMaskChoices\{\{(.*?)\}\};", 5)
    assert [(c["word"], *c["hint"]) for c in DATA["mask"]["choices"]] == triples


def test_look_words_and_hints_match_kLookChoices():
    triples = _triples(r"kLookChoices\{\{(.*?)\}\};", 6)
    assert [(lk["word"], *lk["hint"]) for lk in DATA["looks"]] == triples


def test_mask_row_label_and_native_neutrality():
    m = re.search(r'kMaskRowInfo\{"([^"]*)",\s*"",\s*""\}', GROUPS_H)
    assert m, "kMaskRowInfo not found"
    assert DATA["mask"]["label"] == m.group(1)
    # In CRT Native the mask reads as none: tube_inputs.h says it in code (below) and
    # beside neutral_in_crt_native. The generator writes the flag as a literal.
    assert re.search(r"picture\.crt_native \? Mask::none : resolved\.mask", INPUTS_H), \
        "the CRT Native mask rule was not found in tube_inputs.h"
    assert re.search(r'"crt_native_neutral\\": true', JSON_H), "generator's mask flag not found"
    assert DATA["mask"]["crt_native_neutral"] is True


def test_row_crt_native_neutral_matches_the_owned_list():
    owned = re.findall(r"Effect::(\w+)",
                       _block(r"kOwnedByTheTube\{(.*?)\};", INPUTS_H))
    assert len(owned) == 8
    assert {r["key"] for r in DATA["rows"] if r["crt_native_neutral"]} == set(owned)
    assert len([r for r in DATA["rows"] if r["crt_native_neutral"]]) == 8


def test_header_constants():
    assert DATA["row_min"] == int(_block(r"kRowMin\s*=\s*(-?\d+);", ROWS_H))
    assert DATA["row_max"] == int(_block(r"kRowMax\s*=\s*(-?\d+);", ROWS_H))
    assert DATA["default_look"] == _block(r"kLookDefault\s*=\s*Look::(\w+);", ROWS_H)
    assert DATA["version"] == int(_block(r"kTubeRowsJsonVersion\s*=\s*(\d+);", JSON_H))
    assert set(DATA) == {"version", "row_min", "row_max", "default_look", "engines", "rows",
                         "mask", "groups", "looks"}


def test_steps():
    size_step = int(_block(r"kSizeRowStep\s*=\s*(\d+);", LOOKS_H))
    m = re.search(r"row_step\(Effect e\)[^{]*\{\s*return \(e == Effect::(\w+) \|\| e == Effect::(\w+)\)"
                  r" \? kSizeRowStep : (\d+);", GROUPS_H)
    assert m, "row_step not found in tube_groups.h"
    sized, other = {m.group(1), m.group(2)}, int(m.group(3))
    for row in DATA["rows"]:
        assert row["step"] == (size_step if row["key"] in sized else other), row["key"]


def test_engines():
    names = re.findall(r'"([^"]*)"', _block(r"kCrtEngineNames\{(.*?)\};", APP_H))
    words = re.findall(r'"([^"]*)"', _block(r"kCrtEngineWords\{(.*?)\};", APP_H))
    assert len(names) == len(words) == 3
    assert DATA["engines"] == [{"key": n, "word": w} for n, w in zip(names, words)]


def test_forces_30fps_includes_the_always_rich_rows():
    always = PARITY.parse_always_rich((TUBE / "tube_budget.h").read_text("utf-8"))
    assert always == {"glow", "afterglow", "sharpness", "convergence", "chromaBleed", "raster", "colorDrift"}
    assert always <= {r["key"] for r in DATA["rows"] if r["forces_30fps"]}


# Today's measured facts, pinned so a change to the lean set is a visible diff here.
# tube_budget.h (kLeanSet, kAlwaysRich) is what decides them.
def test_shipped_lean_set_forces_30fps_on_14_rows_and_every_look_is_30fps():
    forcing = {r["key"] for r in DATA["rows"] if r["forces_30fps"]}
    always = PARITY.parse_always_rich((TUBE / "tube_budget.h").read_text("utf-8"))
    assert forcing == always | {"noise", "flicker", "hum", "rollBand", "purity", "abl", "breathing"}
    assert len(forcing) == 14
    assert [l["fps"] for l in DATA["looks"]] == [30] * 6


def test_parse_always_rich_ignores_commented_out_entries():
    text = """inline constexpr std::array<Effect, 2> kAlwaysRich{
    Effect::glow,
    // Effect::afterglow,
    Effect::raster,  // Effect::colorDrift
};"""
    assert PARITY.parse_always_rich(text) == {"glow", "raster"}
