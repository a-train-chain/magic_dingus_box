#!/usr/bin/env python3
"""
check_tube_parity.py - do the box's CRT tube tables still match Sound Factory's?

The tube settings model (magic_dingus_box_cpp/src/tube/) is a manual port of
Sound Factory's engine/src/data/DisplayCrt.h and CrtLooks.h. This reads the two
repos' header TEXT and compares: the effect keys and their order, the mask and
look names, and every look's values and mask word. The lineSize and maskSize
columns are skipped: they differ by design (a TV is not a 2048 px panel).

  check_tube_parity.py [--sf PATH]    default: ../mdb-sound-factory (repo sibling)

Exit 0 "parity ok (Sound Factory <commit>)"; 1 listing each difference;
2 "no Sound Factory checkout at <path>". Developer tool, never run on a box.
Stdlib only.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SF = REPO_ROOT.parent / "mdb-sound-factory"

BOX_ROWS = Path("magic_dingus_box_cpp/src/tube/tube_rows.h")
BOX_LOOKS = Path("magic_dingus_box_cpp/src/tube/tube_looks.h")
SF_ROWS = Path("engine/src/data/DisplayCrt.h")
SF_LOOKS = Path("engine/src/data/CrtLooks.h")

# Different by design; not compared.
IGNORED_KEYS = {"lineSize", "maskSize"}


def _string_array(text, name):
    """The quoted strings of `... kName{ ... }` / `... kName { ... }`, or []."""
    m = re.search(r"\bk(?:Crt)?" + name + r"\s*\{(.*?)\}", text, re.S)
    return re.findall(r'"([^"]*)"', m.group(1)) if m else []


def parse_keys(text):
    """Effect keys in header order (either repo's spelling of the array)."""
    return _string_array(text, "EffectKeys")


_ROW = re.compile(
    r"/\*\s*(\w+)\s*\*/\s*\{\s*\{\s*\{([^}]*)\}\s*\}\s*,\s*(?:Crt)?Mask::(\w+)")


def parse_always_rich(text):
    """The Effect names in tube_budget.h's kAlwaysRich (the rows that always need a
    rich pass), read as text. Empty set when the array is not found."""
    m = re.search(r"\bkAlwaysRich\s*\{(.*?)\}", text, re.S)
    if not m:
        return set()
    body = re.sub(r"//[^\n]*", "", m.group(1))  # a commented-out entry is not an entry
    return set(re.findall(r"Effect::(\w+)", body))


def parse_looks(text):
    """{look name: ([values...], mask word)} from the look table's rows."""
    looks = {}
    for name, values, mask in _ROW.findall(text):
        looks[name] = ([int(v) for v in re.findall(r"-?\d+", values)], mask)
    return looks


def parse_tables(rows_text, looks_text):
    """Everything compared, from one repo's two headers."""
    return {
        "keys": parse_keys(rows_text),
        "masks": _string_array(rows_text, "MaskNames"),
        "look_names": _string_array(rows_text, "LookNames"),
        "looks": parse_looks(looks_text),
    }


def _unparsed(label, t):
    """Problems that mean a header was not parsed as expected (never a silent pass)."""
    problems = []
    for field, what in (("keys", "effect keys"), ("masks", "mask names"),
                        ("look_names", "look names"), ("looks", "look table")):
        if not t[field]:
            problems.append(f"{label}: no {what} parsed - the header could not be "
                            "parsed as expected")
    rows = list(t["looks"])
    if t["look_names"] and t["looks"] and rows != t["look_names"]:
        problems.append(f"{label}: look table rows {rows} do not match the look "
                        f"names {t['look_names']} (rows must be in the same order) "
                        "- the header could not be parsed as expected")
    if t["keys"]:
        for name, (values, _mask) in t["looks"].items():
            if len(values) != len(t["keys"]):
                problems.append(f"{label}: look '{name}' has {len(values)} values "
                                f"but there are {len(t['keys'])} effect keys - the "
                                "header could not be parsed as expected")
    return problems


def compare(box, sf):
    """Each difference as one line; [] when the tables agree."""
    diffs = _unparsed("box", box) + _unparsed("Sound Factory", sf)
    for field, label in (("keys", "effect keys"), ("masks", "mask names"),
                         ("look_names", "look names")):
        if box[field] != sf[field]:
            diffs.append(f"{label}: box {box[field]}, Sound Factory {sf[field]}")
    for look in sorted(set(box["looks"]) | set(sf["looks"])):
        if look not in box["looks"]:
            diffs.append(f"{look}: missing from the box's look table")
            continue
        if look not in sf["looks"]:
            diffs.append(f"{look}: missing from Sound Factory's look table")
            continue
        box_vals, box_mask = box["looks"][look]
        sf_vals, sf_mask = sf["looks"][look]
        if len(box_vals) != len(sf_vals):
            diffs.append(f"{look}: box has {len(box_vals)} values, "
                         f"Sound Factory {len(sf_vals)}")
        else:
            # Name columns by the box's keys; fall back to the index.
            for i, (b, s) in enumerate(zip(box_vals, sf_vals)):
                key = box["keys"][i] if i < len(box["keys"]) else f"[{i}]"
                if key in IGNORED_KEYS or b == s:
                    continue
                diffs.append(f"{look}.{key}: box {b}, Sound Factory {s}")
        if box_mask != sf_mask:
            diffs.append(f"{look}.mask: box {box_mask}, Sound Factory {sf_mask}")
    return diffs


def _commit(path):
    try:
        out = subprocess.run(["git", "-C", str(path), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        return out.stdout.strip() if out.returncode == 0 and out.stdout.strip() else "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sf", default=str(DEFAULT_SF),
                    help="Sound Factory checkout (default: ../mdb-sound-factory)")
    args = ap.parse_args(argv)
    sf_root = Path(args.sf)
    if not sf_root.is_dir():
        print(f"no Sound Factory checkout at {sf_root}")
        return 2
    try:
        box = parse_tables((REPO_ROOT / BOX_ROWS).read_text(),
                           (REPO_ROOT / BOX_LOOKS).read_text())
        sf = parse_tables((sf_root / SF_ROWS).read_text(),
                          (sf_root / SF_LOOKS).read_text())
    except OSError as e:
        print(f"cannot read a header: {e}")
        return 2
    diffs = compare(box, sf)
    if diffs:
        print("tube tables differ from Sound Factory:")
        for d in diffs:
            print("  " + d)
        return 1
    print(f"parity ok (Sound Factory {_commit(sf_root)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
