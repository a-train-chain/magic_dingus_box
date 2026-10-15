"""Unit tests for the pure parsers in hw_validate_lib.py (stdlib only;
auto-discovered by test-ota.yml's `unittest discover`)."""
import json
import os
import struct
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import hw_validate_lib as hv  # noqa: E402


VERIFY_BOX_OUT = """\
\x1b[1m== Platform ==\x1b[0m
  \x1b[32m[PASS]\x1b[0m board: Raspberry Pi 4 Model B Rev 1.5
  [PASS] detection: Platform: Raspberry Pi 4 Model B
  [FAIL] THROTTLED (0x50005) at 81.0'C — check cooling/PSU

== First boot & boot config ==
  [PASS] [pi4] gpu_mem=76
  [WARN] config.txt not found (not a Pi?)

== Kiosk ==
  [FAIL] 2 failed unit(s)
         foo.service loaded failed failed Foo
         bar.service loaded failed failed Bar
  [PASS] service active

== RESULT ==
  3 passed, 2 failed, 1 warnings
  NOT SHIPPABLE — fix the FAILs above
"""


class VerifyBoxParserTests(unittest.TestCase):
    def test_items_sections_and_colors(self):
        items, summary = hv.parse_verify_box(VERIFY_BOX_OUT)
        self.assertEqual(len(items), 7)
        self.assertEqual(items[0], {"section": "Platform", "status": "PASS",
                                    "message": "board: Raspberry Pi 4 Model B Rev 1.5",
                                    "detail": []})
        self.assertEqual(items[3]["section"], "First boot & boot config")
        self.assertEqual(items[3]["message"], "[pi4] gpu_mem=76")
        self.assertEqual(items[4]["status"], "WARN")

    def test_continuation_lines_attach_to_previous_item(self):
        items, _ = hv.parse_verify_box(VERIFY_BOX_OUT)
        failed_units = [i for i in items if "failed unit" in i["message"]][0]
        self.assertEqual(len(failed_units["detail"]), 2)
        self.assertTrue(failed_units["detail"][0].startswith("foo.service"))

    def test_summary(self):
        _, summary = hv.parse_verify_box(VERIFY_BOX_OUT)
        self.assertEqual(summary, {"passed": 3, "failed": 2, "warnings": 1,
                                   "verdict": "NOT SHIPPABLE"})

    def test_shippable_verdict(self):
        _, summary = hv.parse_verify_box(
            "== RESULT ==\n  5 passed, 0 failed, 0 warnings\n  SHIPPABLE\n")
        self.assertEqual(summary["verdict"], "SHIPPABLE")

    def test_empty_output(self):
        self.assertEqual(hv.parse_verify_box(""), ([], {}))


ELD_TV = """\
monitor_present\t\t1
eld_valid\t\t1
monitor_name\t\tSONY TV  *00
connection_type\t\tHDMI
eld_version\t\t[0x2] CEA-861D or below
sad_count\t\t1
sad0_coding_type\t[0x1] LPCM
sad0_channels\t\t2
"""

ELD_NO_AUDIO = "monitor_present\t\t1\neld_valid\t\t1\nsad_count\t\t0\n"
ELD_EMPTY = "monitor_present\t\t0\neld_valid\t\t0\n"
ELD_NO_SAD_LINE = "monitor_present\t\t1\neld_valid\t\t1\n"


class EldTests(unittest.TestCase):
    def test_parse_fields(self):
        eld = hv.parse_eld(ELD_TV)
        self.assertEqual(eld["monitor_present"], 1)
        self.assertEqual(eld["sad_count"], 1)
        self.assertEqual(eld["monitor_name"], "SONY TV  *00")
        self.assertEqual(eld["sad0_coding_type"], "[0x1] LPCM")

    def test_takes_audio_mirrors_launch_contract(self):
        self.assertTrue(hv.eld_takes_audio(hv.parse_eld(ELD_TV)))
        # sad_count decides when present — even with a monitor attached.
        self.assertFalse(hv.eld_takes_audio(hv.parse_eld(ELD_NO_AUDIO)))
        self.assertFalse(hv.eld_takes_audio(hv.parse_eld(ELD_EMPTY)))
        # No sad_count line: a present, valid monitor counts.
        self.assertTrue(hv.eld_takes_audio(hv.parse_eld(ELD_NO_SAD_LINE)))

    def test_card_from_path(self):
        self.assertEqual(hv.card_from_eld_path("/proc/asound/vc4hdmi1/eld#0"),
                         "vc4hdmi1")


SMOKE_OUT = """\
[12:00:00] preflight OK
========================================================================
EMULATOR SMOKE TEST REPORT
========================================================================
[\x1b[92mPASS\x1b[0m] pcsx_rearmed_libretro      'Crash Bandicoot             ' launch= 4210ms return= 1830ms
[\x1b[91mFAIL\x1b[0m] nestopia_libretro          'Super Mario Bros. 3         ' launch=      - return=      -
        \x1b[91m! launch failed: timeout waiting for fresh KMS readiness marker\x1b[0m
        \x1b[91m! missing journal marker: 'Game launched successfully'\x1b[0m
[\x1b[92mPASS\x1b[0m] snes9x2010_libretro        'It's a 'quoted' rom         ' launch= 3001ms return=  999ms
------------------------------------------------------------------------
Games: 2/3 clean pass
Restart-button path: [\x1b[92mPASS\x1b[0m] launched=True intro_replayed=True recovered=True
========================================================================
"""


class SmokeReportTests(unittest.TestCase):
    def test_games_parsed(self):
        rep = hv.parse_smoke_report(SMOKE_OUT)
        self.assertTrue(rep["found"])
        self.assertEqual(len(rep["games"]), 3)
        g0 = rep["games"][0]
        self.assertEqual((g0["status"], g0["core"], g0["rom"]),
                         ("PASS", "pcsx_rearmed_libretro", "Crash Bandicoot"))
        self.assertEqual((g0["launch_ms"], g0["return_ms"]), (4210, 1830))

    def test_failed_game_errors_and_dash_timings(self):
        g1 = hv.parse_smoke_report(SMOKE_OUT)["games"][1]
        self.assertEqual(g1["status"], "FAIL")
        self.assertIsNone(g1["launch_ms"])
        self.assertEqual(len(g1["errors"]), 2)
        self.assertIn("KMS readiness", g1["errors"][0])

    def test_rom_with_quotes(self):
        g2 = hv.parse_smoke_report(SMOKE_OUT)["games"][2]
        self.assertEqual(g2["rom"], "It's a 'quoted' rom")

    def test_summary_and_restart(self):
        rep = hv.parse_smoke_report(SMOKE_OUT)
        self.assertEqual(rep["summary"], {"clean": 2, "total": 3})
        self.assertEqual(rep["restart"]["status"], "PASS")
        self.assertTrue(rep["restart"]["recovered"])

    def test_no_report_block(self):
        rep = hv.parse_smoke_report("Traceback (most recent call last):\n")
        self.assertFalse(rep["found"])
        self.assertEqual(rep["games"], [])


class CoreGatingTests(unittest.TestCase):
    PLAYLISTS = [
        'items:\n  - emulator_core: "pcsx_rearmed_libretro"\n',
        "items:\n  - emulator_core: mupen64plus_next_libretro\n",
        "items:\n  - emulator_core: 'flycast_libretro'\n"
        "  - emulator_core: nestopia_libretro\n",
    ]

    def test_expected_cores_pi4_hides_n64_and_dc(self):
        self.assertEqual(hv.expected_cores(self.PLAYLISTS, "pi4"),
                         {"pcsx_rearmed", "nestopia"})

    def test_expected_cores_pi5_keeps_everything(self):
        self.assertEqual(hv.expected_cores(self.PLAYLISTS, "pi5"),
                         {"pcsx_rearmed", "nestopia", "mupen64plus_next",
                          "flycast"})

    def test_gating_violations(self):
        names = ["PlayStation Games", "Nintendo 64 Classics",
                 "Dreamcast Classics", "NES Games"]
        bad = hv.gating_violations("pi4", names, ["flycast_libretro",
                                                  "pcsx_rearmed_libretro"])
        self.assertEqual(len(bad), 3)
        self.assertEqual(hv.gating_violations("pi5", names, ["flycast"]), [])
        self.assertEqual(hv.gating_violations(
            "pi4", ["PlayStation Games", "SNES Games"], ["snes9x2010"]), [])

    def test_normalize_core(self):
        self.assertEqual(hv.normalize_core("pcsx_rearmed_libretro.so"),
                         "pcsx_rearmed")
        self.assertEqual(hv.normalize_core("/x/flycast_libretro"), "flycast")


def make_bmp(width, height, pixel, bpp=24, top_down=False):
    """Synthetic BMP; pixel(x, y_from_top) -> (r, g, b)."""
    stride = ((width * bpp + 31) // 32) * 4
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            r, g, b = pixel(x, y)
            row += bytes((b, g, r)) + (b"\xff" if bpp == 32 else b"")
        row += b"\0" * (stride - len(row))
        rows.append(bytes(row))
    if not top_down:
        rows.reverse()
    data = b"".join(rows)
    header = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width,
                       -height if top_down else height, 1, bpp, 0,
                       len(data), 2835, 2835, 0, 0)
    return header + info + data


W, H = 160, 120  # 4:3, like a CRT capture


def in_qr(x, y):
    # QR placement per pairing_screen_renderer.cpp: centred, 0.38*min side,
    # top at 108/720 of the height.
    size = int(min(W, H) * 0.38)
    qx, qy = (W - size) // 2, int(H * 108 / 720)
    return qx <= x < qx + size and qy <= y < qy + size, qx, qy


def qr_like(x, y):
    inside, qx, qy = in_qr(x, y)
    if not inside:
        return (20, 20, 30)  # dark pairing background
    module = ((x - qx) // 3 + (y - qy) // 3) % 2
    return (250, 250, 250) if module else (5, 5, 5)


def black_square(x, y):
    inside, _, _ = in_qr(x, y)
    return (0, 0, 0) if inside else (20, 20, 30)


class BmpQrTests(unittest.TestCase):
    def test_qr_passes_24bit_bottom_up(self):
        res = hv.qr_contrast(hv.Bmp(make_bmp(W, H, qr_like)), step=1)
        self.assertTrue(res["ok"], res)
        self.assertGreater(res["light_frac"], 0.10)
        self.assertGreater(res["dark_frac"], 0.10)

    def test_qr_passes_32bit_top_down(self):
        bmp = hv.Bmp(make_bmp(W, H, qr_like, bpp=32, top_down=True))
        self.assertTrue(hv.qr_contrast(bmp, step=1)["ok"])

    def test_black_square_fails(self):
        res = hv.qr_contrast(hv.Bmp(make_bmp(W, H, black_square)), step=1)
        self.assertFalse(res["ok"])
        self.assertLess(res["light_frac"], 0.01)

    def test_all_white_fails(self):
        res = hv.qr_contrast(hv.Bmp(make_bmp(W, H, lambda x, y: (255, 255, 255))))
        self.assertFalse(res["ok"])
        self.assertEqual(res["dark_frac"], 0.0)

    def test_orientation_is_respected(self):
        # Top half white, bottom half black: luma(0, 0) must read the TOP.
        def split(x, y):
            return (255, 255, 255) if y < H // 2 else (0, 0, 0)
        for top_down in (False, True):
            bmp = hv.Bmp(make_bmp(W, H, split, top_down=top_down))
            self.assertEqual(bmp.luma(0, 0), 255)
            self.assertEqual(bmp.luma(0, H - 1), 0)

    def test_odd_width_row_padding(self):
        bmp = hv.Bmp(make_bmp(7, 5, lambda x, y: (x * 30, 0, 0)))
        self.assertEqual(bmp.stride, 24)
        self.assertEqual(bmp.luma(6, 2), (299 * 180) // 1000)

    def test_rejects_garbage(self):
        with self.assertRaises(ValueError):
            hv.Bmp(b"PNG not a bmp" * 10)
        with self.assertRaises(ValueError):
            hv.Bmp(make_bmp(8, 8, qr_like)[:80])  # truncated


class MiscParserTests(unittest.TestCase):
    def test_proc_stat_ticks_handles_spaces_in_comm(self):
        stat = ("1234 (magic dingus) S 1 1234 1234 0 -1 4194560 100 0 0 0 "
                "250 50 0 0 20 0 8 0 100 1000 200")
        self.assertEqual(hv.proc_stat_ticks(stat), 300)

    def test_cpu_percent(self):
        self.assertEqual(hv.cpu_percent(1000, 1300, 15.0, 100), 20.0)
        self.assertEqual(hv.cpu_percent(0, 10, 0, 100), 0.0)

    def test_redraw_reports(self):
        text = ("[info] Redraw gate: drew 1800 / skipped 1800 iterations in the "
                "last 60s (crt30 3600)\nnoise\n"
                "[debug] Redraw gate: drew 60 / skipped 3540 iterations in the "
                "last 60s (crt30 0)\n")
        reps = hv.parse_redraw_reports(text)
        self.assertEqual(len(reps), 2)
        self.assertEqual(reps[-1], {"drawn": 60, "skipped": 3540,
                                    "window_s": 60, "crt30": 0})

    def test_classify_artwork(self):
        now = time.time()
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now - 60))
        old = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(now - 7200))
        quiet = [f"[{stamp}.000] [info] [artwork] uploaded poster: url='x'"]
        self.assertEqual(hv.classify_artwork(quiet, now)[0], "PASS")
        self.assertEqual(hv.classify_artwork([], now)[0], "PASS")
        churn = [f"[{stamp}.000] [debug] [artwork] evicted LRU entry url='u{i}'"
                 for i in range(60)]
        st, msg, data = hv.classify_artwork(churn, now)
        self.assertEqual(st, "WARN")
        self.assertEqual(data["recent_evictions"], 60)
        stale = [f"[{old}.000] [debug] [artwork] evicted LRU entry url='u{i}'"
                 for i in range(60)]
        self.assertEqual(hv.classify_artwork(stale, now)[0], "PASS")
        over = ["[artwork] on-screen posters exceed the texture budget"] * 3
        self.assertEqual(hv.classify_artwork(over, now)[0], "WARN")

    def test_classify_decoders(self):
        hw = "[debug]   Decoder: v4l2h264dec0 (v4l2h264dec)\n"
        sw = "[debug]   Decoder: avdec_h264-0 (avdec_h264)\n"
        self.assertEqual(hv.classify_decoders(hw, "pi4")[0], "PASS")
        self.assertEqual(hv.classify_decoders(sw, "pi4")[0], "FAIL")
        self.assertEqual(hv.classify_decoders(hw + sw, "pi4")[0], "WARN")
        self.assertEqual(hv.classify_decoders(sw, "pi5")[0], "PASS")
        self.assertEqual(hv.classify_decoders("", "pi4")[0], "MANUAL")
        self.assertEqual(hv.classify_decoders(
            "Decoder: x (avdec_aac)\n", "pi4")[0], "MANUAL")


class GameAlsaTests(unittest.TestCase):
    HP = hv.HEADPHONES_ALSA

    def test_headphone(self):
        self.assertEqual(hv.check_game_alsa("headphone", self.HP, [], [], True)[0],
                         "PASS")
        self.assertEqual(hv.check_game_alsa(
            "headphone", "sysdefault:CARD=vc4hdmi0", [], [], True)[0], "FAIL")

    def test_hdmi_goes_to_the_port_with_the_tv(self):
        cards = ["vc4hdmi0", "vc4hdmi1"]
        self.assertEqual(hv.check_game_alsa(
            "hdmi", "sysdefault:CARD=vc4hdmi1", cards, ["vc4hdmi1"], True)[0],
            "PASS")
        self.assertEqual(hv.check_game_alsa(
            "hdmi", "sysdefault:CARD=vc4hdmi0", cards, ["vc4hdmi1"], True)[0],
            "FAIL")

    def test_auto_without_tv_audio_uses_jack(self):
        cards = ["vc4hdmi0", "vc4hdmi1"]
        self.assertEqual(hv.check_game_alsa("auto", self.HP, cards, [], True)[0],
                         "PASS")
        self.assertEqual(hv.check_game_alsa(
            "auto", "sysdefault:CARD=vc4hdmi0", cards, [], True)[0], "FAIL")
        # Pi 5 (no jack listed): auto stays on HDMI.
        self.assertEqual(hv.check_game_alsa(
            "auto", "sysdefault:CARD=vc4hdmi0", cards, [], False)[0], "PASS")

    def test_legacy_fallback_and_missing_line_fail(self):
        self.assertEqual(hv.check_game_alsa("hdmi", "plughw:1,0", [], [], True)[0],
                         "FAIL")
        self.assertEqual(hv.check_game_alsa("hdmi", "", [], [], True)[0], "FAIL")


class BusyAndSettingsTests(unittest.TestCase):
    def test_busy_reason(self):
        self.assertIsNone(hv.busy_reason({"screen": "playlist",
                                          "retroarch": None}))
        self.assertIsNone(hv.busy_reason({}))
        self.assertIn("game", hv.busy_reason({"screen": "retroarch"}))
        self.assertIn("game", hv.busy_reason(
            {"screen": "playlist", "retroarch": {"core": "x"}}))
        self.assertIn("movie", hv.busy_reason(
            {"screen": "media_browser", "now_playing": {"kind": "movie"}}))
        self.assertIn("tv", hv.busy_reason({"now_playing": {"kind": "tv"}}))
        self.assertIsNone(hv.busy_reason({"now_playing": {"kind": "video"}}))

    def test_apply_audio_output_keeps_other_keys(self):
        src = json.dumps({"audio": {"output": "auto",
                                    "retroarch_volume_offset_db": -3.0},
                          "display": {"mode": "crt_native"}})
        out = json.loads(hv.apply_audio_output(src, "headphone"))
        self.assertEqual(out["audio"], {"output": "headphone",
                                        "retroarch_volume_offset_db": -3.0})
        self.assertEqual(out["display"], {"mode": "crt_native"})
        with self.assertRaises(ValueError):
            hv.apply_audio_output(src, "spdif")


class ReportTests(unittest.TestCase):
    def test_tsv_roundtrip_and_verdict(self):
        tsv = ("PASS\tAudio\tdefault sink: x\t\n"
               "FAIL\tServices\tno gunicorn\t{\"a\": 1}\n"
               "MANUAL\tGame audio routing\tlisten to the jack\t\n"
               "WARN\tVideo\tmeh\t\n")
        results = hv.parse_results_tsv(tsv)
        self.assertEqual(results[1]["data"], {"a": 1})
        rep = hv.build_report(results, {"board": "pi4"})
        self.assertEqual(rep["summary"], {"pass": 1, "fail": 1, "warn": 1,
                                          "manual": 1})
        self.assertEqual(rep["verdict"], "FAIL")
        self.assertEqual(rep["board"], "pi4")
        self.assertEqual(rep["manual_from_run"], ["listen to the jack"])
        self.assertTrue(rep["manual_checklist"])

    def test_emit_writes_tsv_and_report_cmd_exit_code(self):
        with tempfile.TemporaryDirectory() as d:
            res = os.path.join(d, "r.tsv")
            out = os.path.join(d, "r.json")
            with mock.patch.dict(os.environ, {"HWV_RESULTS": res,
                                              "HWV_GROUP": "G"}):
                with mock.patch("sys.stdout"):
                    hv.emit("PASS", "a\tb\nc", data={"k": 1})
                    rc = hv.main(["report", "--results", res, "--out", out,
                                  "--meta", "board=pi5"])
                    self.assertEqual(rc, 0)
                    hv.emit("FAIL", "boom")
                    rc = hv.main(["report", "--results", res, "--out", out])
            self.assertEqual(rc, 1)
            with open(res) as f:
                first = f.readline().rstrip("\n").split("\t")
            self.assertEqual(first[:3], ["PASS", "G", "a b c"])
            with open(out) as f:
                rep = json.load(f)
            self.assertEqual(rep["verdict"], "FAIL")
            self.assertEqual(rep["results"][0]["data"], {"k": 1})


if __name__ == "__main__":
    unittest.main()
