"""
check_tube_parity.py - the box's CRT tube tables against Sound Factory's.

Contract under test:
  * parse_keys reads the effect-key array of either repo's header shape.
  * parse_looks reads each look's values and mask word (either shape).
  * compare ignores lineSize and maskSize (different by design) and names
    every other difference as "<look>.<key>: box N, Sound Factory M".
  * compare flags an empty table, a row whose value count differs from the key
    count, and rows out of order versus the look names - never a silent pass.
  * main returns 2 when there is no Sound Factory checkout.

Stdlib only (the CI job has no pip step).
"""
import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
    "check_tube_parity", SCRIPTS_DIR / "dev" / "check_tube_parity.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

KEYS = ["brightness", "lineSize", "maskSize", "hum"]

BOX_ROWS = """
inline constexpr std::array<std::string_view, kEffectCount> kEffectKeys{
    "brightness", "lineSize",
    "maskSize", "hum"};
inline constexpr std::array<std::string_view, 2> kMaskNames{"grille", "slot"};
inline constexpr std::array<std::string_view, 2> kLookNames{"pvm", "tv"};
static_assert(kEffectKeys.size() == kEffects.size(), "one key per effect");
"""

SF_ROWS = """
inline constexpr std::array<std::string_view, 2> kCrtMaskNames { "grille", "slot" };
inline constexpr std::array<std::string_view, 2> kCrtLookNames { "pvm",
                                                                 "tv" };
inline constexpr std::array<std::string_view, 4> kCrtEffectKeys {
    "brightness",   "lineSize",
    "maskSize", "hum"
};
"""

BOX_LOOKS = """
    /* pvm */  { { { 55, 10, 10, 0 } }, Mask::grille },
    /* tv */   { { { 58, 10, 10, 30 } }, Mask::slot },
"""

SF_LOOKS = """
    /* pvm */ { { { 55, 30, 40, 0 } }, CrtMask::grille },
    /* tv */  { { { 58, 30, 40, 30 } }, CrtMask::slot },
"""


def tables(rows, looks):
    return mod.parse_tables(rows, looks)


class ParseTests(unittest.TestCase):
    def test_keys_both_shapes(self):
        self.assertEqual(mod.parse_keys(BOX_ROWS), KEYS)
        self.assertEqual(mod.parse_keys(SF_ROWS), KEYS)

    def test_looks_both_shapes(self):
        self.assertEqual(mod.parse_looks(BOX_LOOKS),
                         {"pvm": ([55, 10, 10, 0], "grille"),
                          "tv": ([58, 10, 10, 30], "slot")})
        self.assertEqual(mod.parse_looks(SF_LOOKS)["tv"], ([58, 30, 40, 30], "slot"))


class CompareTests(unittest.TestCase):
    def test_equal_ignoring_sizes(self):
        self.assertEqual(
            mod.compare(tables(BOX_ROWS, BOX_LOOKS), tables(SF_ROWS, SF_LOOKS)), [])

    def test_changed_value_named(self):
        box = BOX_LOOKS.replace("58, 10, 10, 30", "58, 10, 10, 40")
        diffs = mod.compare(tables(BOX_ROWS, box), tables(SF_ROWS, SF_LOOKS))
        self.assertEqual(diffs, ["tv.hum: box 40, Sound Factory 30"])

    def test_changed_mask_named(self):
        box = BOX_LOOKS.replace("Mask::slot", "Mask::grille")
        diffs = mod.compare(tables(BOX_ROWS, box), tables(SF_ROWS, SF_LOOKS))
        self.assertEqual(diffs, ["tv.mask: box grille, Sound Factory slot"])

    def test_missing_key_reported(self):
        rows = BOX_ROWS.replace('"maskSize", "hum"}', '"maskSize"}')
        diffs = mod.compare(tables(rows, BOX_LOOKS), tables(SF_ROWS, SF_LOOKS))
        self.assertTrue(any("effect keys" in d for d in diffs), diffs)

    def test_reordered_key_reported(self):
        rows = BOX_ROWS.replace('"brightness", "lineSize"', '"lineSize", "brightness"')
        diffs = mod.compare(tables(rows, BOX_LOOKS), tables(SF_ROWS, SF_LOOKS))
        self.assertTrue(any("effect keys" in d for d in diffs), diffs)


class UnparsedTests(unittest.TestCase):
    def test_both_empty_is_not_ok(self):
        diffs = mod.compare(tables("", ""), tables("", ""))
        for label in ("box", "Sound Factory"):
            for what in ("effect keys", "mask names", "look names", "look table"):
                self.assertTrue(any(d.startswith(f"{label}: no {what}") for d in diffs),
                                (label, what, diffs))

    def test_one_side_empty_names_it(self):
        diffs = mod.compare(tables(BOX_ROWS, BOX_LOOKS), tables("", ""))
        self.assertTrue(diffs)
        self.assertTrue(any(d.startswith("Sound Factory: no look table") for d in diffs))
        self.assertFalse(any(d.startswith("box:") for d in diffs), diffs)

    def test_wrong_value_count(self):
        box = BOX_LOOKS.replace("58, 10, 10, 30", "58, 10, 10")
        diffs = mod.compare(tables(BOX_ROWS, box), tables(SF_ROWS, SF_LOOKS))
        self.assertTrue(any("box: look 'tv' has 3 values but there are 4" in d
                            for d in diffs), diffs)

    def test_rows_out_of_order(self):
        rows = BOX_LOOKS.strip().splitlines()
        box = "\n".join(reversed(rows))
        diffs = mod.compare(tables(BOX_ROWS, box), tables(SF_ROWS, SF_LOOKS))
        self.assertTrue(any(d.startswith("box: look table rows ['tv', 'pvm']")
                            for d in diffs), diffs)


class MainTests(unittest.TestCase):
    def test_missing_checkout_is_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(mod.main(["--sf", str(Path(tmp) / "nope")]), 2)


if __name__ == "__main__":
    unittest.main()
