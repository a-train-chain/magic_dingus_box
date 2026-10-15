#!/usr/bin/env python3
"""Helpers for hw_validate.sh — the pre-release hardware validation kit.

Two halves:

  * PURE PARSERS (no I/O, unit-tested on the Mac in
    tests/test_hw_validate.py): verify_box.sh output, HDMI ELD files, the
    emulator smoke-test report, the pairing-screen QR contrast check on a
    BMP screenshot, /proc/<pid>/stat CPU sampling, the redraw-gate report
    line, the poster-budget journal lines, the expected game ALSA device.

  * STAGES (subcommands hw_validate.sh calls ON the box): each one drives
    the kiosk through the emulator smoke-test harness (imported as a
    module, exactly like the lead's ad-hoc scripts do) and emits result
    lines. Stages never touch user content and never edit settings.json —
    every backup/restore lives in hw_validate.sh, in one place, behind its
    EXIT/INT/TERM/HUP trap.

Results are appended to $HWV_RESULTS as TSV:
    STATUS <TAB> GROUP <TAB> MESSAGE <TAB> DATA-JSON
with GROUP taken from $HWV_GROUP (hw_validate.sh exports it per section).
Statuses: PASS, FAIL, WARN, MANUAL. Stdlib only.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.environ.get(
    "MAGIC_DATA_DIR", "/opt/magic_dingus_box/magic_dingus_box_cpp/data")
STATUS_PATH = os.path.join(DATA_DIR, "kiosk_status.json")
KIOSK_UNIT = "magic-dingus-box-cpp.service"
HOME = os.path.expanduser("~")

STATUSES = ("PASS", "FAIL", "WARN", "MANUAL")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# Cores the Pi 4B profile gates off (platform_profile.cpp
# unsupported_emulator_cores, normalized without the _libretro suffix).
PI4_UNSUPPORTED_CORES = {"mupen64plus_next", "parallel_n64", "flycast"}
PI4_HIDDEN_PLAYLIST_RE = re.compile(r"(?i)\bn64\b|nintendo\s*64|dreamcast")
PS1_PLAYLIST_RE = re.compile(r"(?i)playstation|\bps1\b|\bpsx\b")

HEADPHONES_ALSA = "sysdefault:CARD=Headphones"

# Things only a human can check. Printed at the end of every run and
# written into the JSON report. Each maps to a release-review finding.
MANUAL_CHECKS = [
    "Listen: for every audio output exercised (headphone / auto / hdmi), "
    "game sound came out of the expected output — the script reads the "
    "ALSA device RetroArch was given but cannot hear the speakers.",
    "Two different pads (e.g. N64 adapter + DragonRise PS-style) each get "
    "their own mapping in a 2-player game (Mapping P1/P2 lines in "
    "~/retroarch_launcher.log, then actually play both ports).",
    "Pull power mid-OTA (during extract/build), power back on: the box "
    "rolls back or completes via magic-dingus-ota-recovery.service and "
    "boots to the menu.",
    "TV-off update behaviour: start an OTA with the TV off / on standby, "
    "turn the TV on afterwards — the kiosk shows the menu at the right "
    "mode with sound.",
    "Text edges look right (no fringing / clipped glyphs) on the main "
    "menu, Settings and Media Browser — eyeball the saved screenshots and "
    "the TV itself.",
    "Movie playback is smooth with lip sync for a full minute (1080p "
    "H.264 from the library), including after a seek.",
    "Frame pacing on the static CRT main menu: no visible judder; the "
    "per-minute 'Redraw gate: drew N / skipped M' report shows skips "
    "(crt30 > 0 in CRT mode).",
]


# ---------------------------------------------------------------------------
# Result emission
# ---------------------------------------------------------------------------
_COLORS = {"PASS": "\033[32m", "FAIL": "\033[31m", "WARN": "\033[33m",
           "MANUAL": "\033[36m"}


def emit(status: str, message: str, data=None, group: str | None = None):
    """Print one result line (verify_box.sh style) and append it to the
    TSV results file named by $HWV_RESULTS."""
    if status not in STATUSES:
        raise ValueError(f"bad status {status!r}")
    group = group if group is not None else os.environ.get("HWV_GROUP", "")
    message = " ".join(str(message).split())  # no tabs/newlines in TSV
    if sys.stdout.isatty():
        print(f"  {_COLORS[status]}[{status}]\033[0m {message}", flush=True)
    else:
        print(f"  [{status}] {message}", flush=True)
    path = os.environ.get("HWV_RESULTS")
    if path:
        payload = json.dumps(data, sort_keys=True) if data is not None else ""
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"{status}\t{group}\t{message}\t{payload}\n")


def note(msg: str):
    print(f"         {msg}", flush=True)


# ---------------------------------------------------------------------------
# Pure parsers
# ---------------------------------------------------------------------------
def parse_verify_box(text: str):
    """Parse verify_box.sh output into (items, summary).

    items: [{"section", "status", "message", "detail": [...]}]
    summary: {"passed", "failed", "warnings", "verdict"} (keys present only
    when found). Continuation lines (verify_box indents failed-unit lists
    and verify_services.sh tails by 9 spaces) attach to the previous item.
    """
    items = []
    summary = {}
    section = None
    for raw in text.splitlines():
        line = ANSI_RE.sub("", raw).rstrip()
        stripped = line.strip()
        m = re.match(r"^== (.+?) ==$", stripped)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"^\s*\[(PASS|FAIL|WARN)\]\s+(.*)$", line)
        if m:
            items.append({"section": section, "status": m.group(1),
                          "message": m.group(2), "detail": []})
            continue
        m = re.match(r"^(\d+) passed, (\d+) failed, (\d+) warnings", stripped)
        if m:
            summary.update(passed=int(m.group(1)), failed=int(m.group(2)),
                           warnings=int(m.group(3)))
            continue
        if stripped == "SHIPPABLE":
            summary["verdict"] = "SHIPPABLE"
            continue
        if stripped.startswith("NOT SHIPPABLE"):
            summary["verdict"] = "NOT SHIPPABLE"
            continue
        if line.startswith("        ") and stripped and items:
            items[-1]["detail"].append(stripped)
    return items, summary


def parse_eld(text: str) -> dict:
    """Parse /proc/asound/vc4hdmiN/eld#0 ("key<TABs>value" lines).

    Integer-looking values become ints; everything else stays a string
    (monitor_name, sad lines)."""
    out = {}
    for line in text.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        key, value = parts[0], parts[1].strip()
        try:
            out[key] = int(value, 0)
        except ValueError:
            out[key] = value
    return out


def eld_takes_audio(eld: dict) -> bool:
    """Mirror of retroarch::eld_reports_monitor (launch_contract.cpp): a
    sad_count line decides when present; otherwise a present, valid
    monitor counts."""
    sad = eld.get("sad_count")
    if isinstance(sad, int):
        return sad > 0
    return eld.get("monitor_present") == 1 and eld.get("eld_valid", 0) != 0


def card_from_eld_path(path: str) -> str:
    # /proc/asound/vc4hdmi0/eld#0 -> vc4hdmi0
    return os.path.basename(os.path.dirname(path))


_SMOKE_LINE_RE = re.compile(
    r"^\[(PASS|FAIL)\]\s+(\S+)\s+'(.*)'\s+launch=\s*(\S+)\s+return=\s*(\S+)\s*$")
_SMOKE_RESTART_RE = re.compile(
    r"Restart-button path: \[(PASS|FAIL)\] launched=(\w+) "
    r"intro_replayed=(\w+) recovered=(\w+)")


def _ms(token: str):
    m = re.match(r"^(\d+)ms$", token)
    return int(m.group(1)) if m else None


def parse_smoke_report(text: str) -> dict:
    """Parse the report block emulator_smoke_test.py prints at the end."""
    games = []
    restart = None
    summary = None
    in_report = False
    last = None
    for raw in text.splitlines():
        line = ANSI_RE.sub("", raw).rstrip()
        if "EMULATOR SMOKE TEST REPORT" in line:
            in_report = True
            continue
        if not in_report:
            continue
        m = _SMOKE_LINE_RE.match(line)
        if m:
            last = {"status": m.group(1), "core": m.group(2),
                    "rom": m.group(3).strip(), "launch_ms": _ms(m.group(4)),
                    "return_ms": _ms(m.group(5)), "errors": []}
            games.append(last)
            continue
        m = re.match(r"^\s+! (.*)$", line)
        if m and last is not None:
            last["errors"].append(m.group(1).strip())
            continue
        m = re.match(r"^Games: (\d+)/(\d+) clean pass", line)
        if m:
            summary = {"clean": int(m.group(1)), "total": int(m.group(2))}
            last = None
            continue
        m = _SMOKE_RESTART_RE.search(line)
        if m:
            restart = {"status": m.group(1),
                       "launched": m.group(2) == "True",
                       "intro_replayed": m.group(3) == "True",
                       "recovered": m.group(4) == "True", "errors": []}
            last = restart
    return {"found": in_report, "games": games, "summary": summary,
            "restart": restart}


def normalize_core(core: str) -> str:
    core = os.path.basename(core or "")
    if core.endswith(".so"):
        core = core[:-3]
    if core.endswith("_libretro"):
        core = core[:-len("_libretro")]
    return core


_CORE_RE = re.compile(r"^[\s-]*emulator_core:\s*['\"]?([\w.+-]+)", re.M)


def expected_cores(playlist_texts, board: str) -> set:
    """Distinct cores the box's game playlists reference, minus the ones
    the board's profile hides (Pi 4B: N64 + Dreamcast cores)."""
    cores = set()
    for text in playlist_texts:
        cores.update(normalize_core(c) for c in _CORE_RE.findall(text))
    if board == "pi4":
        cores -= PI4_UNSUPPORTED_CORES
    return cores


def gating_violations(board: str, playlist_names, cores) -> list:
    """Pi 4B must never OFFER N64/Dreamcast (menu) nor LAUNCH their cores."""
    if board != "pi4":
        return []
    bad = [f"playlist offered: {n}" for n in playlist_names
           if PI4_HIDDEN_PLAYLIST_RE.search(n or "")]
    bad += [f"core launched: {c}" for c in sorted(
        {normalize_core(c) for c in cores} & PI4_UNSUPPORTED_CORES)]
    return bad


class Bmp:
    """Minimal uncompressed 24/32-bit BMP reader (what
    utils::encode_bmp24_from_rgba writes; bottom-up or top-down)."""

    def __init__(self, data: bytes):
        if len(data) < 54 or data[:2] != b"BM":
            raise ValueError("not a BMP")
        self.data = data
        self.offset = struct.unpack_from("<I", data, 10)[0]
        width, height = struct.unpack_from("<ii", data, 18)
        _planes, self.bpp = struct.unpack_from("<HH", data, 26)
        compression = struct.unpack_from("<I", data, 30)[0]
        if self.bpp not in (24, 32) or compression not in (0, 3):
            raise ValueError(f"unsupported BMP ({self.bpp} bpp, "
                             f"compression {compression})")
        if width <= 0 or height == 0:
            raise ValueError("bad BMP dimensions")
        self.width = width
        self.top_down = height < 0
        self.height = abs(height)
        self.stride = ((width * self.bpp + 31) // 32) * 4
        need = self.offset + self.stride * self.height
        if len(data) < need:
            raise ValueError("truncated BMP")

    def luma(self, x: int, y: int) -> int:
        """Luma 0-255 of pixel (x, y), y measured from the TOP."""
        row = y if self.top_down else self.height - 1 - y
        i = self.offset + row * self.stride + x * (self.bpp // 8)
        b, g, r = self.data[i], self.data[i + 1], self.data[i + 2]
        return (299 * r + 587 * g + 114 * b) // 1000


# The pairing screen draws the QR horizontally centred, 0.38*min(w,h)
# square, top at 108/720 of the height (pairing_screen_renderer.cpp) —
# so it spans ~39-61% across and ~15-53% down in either display mode
# (the 4:3 CRT viewport is pillarboxed, i.e. still centred).
QR_REGION = (0.35, 0.15, 0.65, 0.55)  # left, top, right, bottom fractions


def qr_contrast(bmp: Bmp, region=QR_REGION, light=170, dark=70,
                min_frac=0.10, step=2) -> dict:
    """A real QR has many near-white AND near-black pixels in its region;
    the 'black square' bug (texture missing/never uploaded) has almost no
    light ones. ok = both fractions >= min_frac."""
    x0 = int(bmp.width * region[0])
    x1 = max(x0 + 1, int(bmp.width * region[2]))
    y0 = int(bmp.height * region[1])
    y1 = max(y0 + 1, int(bmp.height * region[3]))
    n = n_light = n_dark = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            v = bmp.luma(x, y)
            n += 1
            if v >= light:
                n_light += 1
            elif v <= dark:
                n_dark += 1
    lf = n_light / n if n else 0.0
    df = n_dark / n if n else 0.0
    return {"ok": lf >= min_frac and df >= min_frac,
            "light_frac": round(lf, 4), "dark_frac": round(df, 4),
            "samples": n, "region_px": [x0, y0, x1, y1]}


def proc_stat_ticks(stat_text: str) -> int:
    """utime+stime (clock ticks) from /proc/<pid>/stat. The comm field may
    contain spaces/parens, so split after the LAST ')'."""
    rest = stat_text[stat_text.rindex(")") + 2:].split()
    # rest[0] is field 3 (state); utime/stime are fields 14/15.
    return int(rest[11]) + int(rest[12])


def cpu_percent(ticks_before: int, ticks_after: int, seconds: float,
                clk_tck: int = 100) -> float:
    """Percent of ONE core, same convention as top's %CPU."""
    if seconds <= 0:
        return 0.0
    return round((ticks_after - ticks_before) / clk_tck / seconds * 100.0, 1)


_REDRAW_RE = re.compile(
    r"Redraw gate: drew (\d+) / skipped (\d+) iterations in the last (\d+)s"
    r" \(crt30 (\d+)\)")


def parse_redraw_reports(text: str) -> list:
    return [{"drawn": int(a), "skipped": int(b), "window_s": int(c),
             "crt30": int(d)} for a, b, c, d in _REDRAW_RE.findall(text)]


_LOG_TS_RE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")


def _line_epoch(line: str):
    m = _LOG_TS_RE.match(line)
    if not m:
        return None
    try:
        return time.mktime(time.strptime(m.group(1), "%Y-%m-%d %H:%M:%S"))
    except ValueError:
        return None


def classify_artwork(lines, now: float, window_s: int = 900,
                     overshoot_warn: int = 3, evict_warn: int = 50):
    """Poster texture budget health from journal + kiosk file-log lines.

    WARN when the over-budget overshoot recurs (>= overshoot_warn episodes
    in the run) or LRU evictions keep happening (> evict_warn in the last
    window_s seconds of timestamped file-log lines) — on a static screen
    that means posters are being evicted and re-fetched in a loop."""
    art = [ln for ln in lines if "[artwork]" in ln]
    overshoot = sum("exceed the texture budget" in ln for ln in art)
    healed = sum("overshoot healed" in ln for ln in art)
    trims = sum("[artwork] trimmed" in ln for ln in art)
    recent_evicts = 0
    for ln in art:
        if "evicted LRU" in ln:
            t = _line_epoch(ln)
            if t is not None and now - t <= window_s:
                recent_evicts += 1
    data = {"artwork_lines": len(art), "overshoot_episodes": overshoot,
            "overshoot_healed": healed, "trims": trims,
            "recent_evictions": recent_evicts, "window_s": window_s}
    if not art:
        return "PASS", ("no [artwork] activity logged (Media Browser not "
                        "opened this run)"), data
    if overshoot >= overshoot_warn or recent_evicts > evict_warn:
        return "WARN", (f"poster budget churn: {overshoot} over-budget "
                        f"episode(s), {recent_evicts} LRU eviction(s) in the "
                        f"last {window_s // 60} min — check whether this "
                        f"recurs on a STATIC screen"), data
    return "PASS", (f"poster budget OK ({overshoot} over-budget episode(s), "
                    f"{recent_evicts} recent eviction(s), {trims} trim(s))"), data


_DECODER_RE = re.compile(r"Decoder: \S+ \(([\w-]+)\)")


def classify_decoders(text: str, board: str):
    """Which H.264 decoder playbin picked (kiosk DEBUG file log)."""
    factories = _DECODER_RE.findall(text)
    h264 = sorted({f for f in factories if "h264" in f})
    data = {"decoders_seen": sorted(set(factories)), "h264": h264}
    if not h264:
        return "MANUAL", ("no H.264 decoder logged yet — play an H.264 "
                          "video (playlist item or movie) and re-run"), data
    if board == "pi4":
        if "v4l2h264dec" in h264 and "avdec_h264" not in h264:
            return "PASS", "H.264 decoded in hardware (v4l2h264dec)", data
        if "v4l2h264dec" in h264:
            return "WARN", ("H.264 used v4l2h264dec AND avdec_h264 — some "
                            "file fell back to software decode"), data
        return "FAIL", ("Pi 4B decoded H.264 in SOFTWARE (" + ", ".join(h264)
                        + ") — v4l2h264dec rank promotion not effective"), data
    if board == "pi5":
        return "PASS", ("H.264 decoder: " + ", ".join(h264)
                        + " (Pi 5 has no hardware H.264 — software expected)"), data
    return "PASS", "H.264 decoder: " + ", ".join(h264), data


def check_game_alsa(output: str, device: str, eld_cards, monitor_cards,
                    headphones_listed: bool):
    """Is the ALSA device RetroArch was given the right one for the
    audio.output setting? Mirrors retroarch::pick_game_alsa_device.
    Returns (status, message, expected_human)."""
    device = (device or "").strip()
    if not device:
        return "FAIL", "no 'ALSA device:' line in the launcher log", "?"
    if device == "plughw:1,0":
        return ("FAIL", f"{output}: legacy fallback {device} — no PCM by "
                "name matched (aplay -L empty?)", "?")
    if output == "headphone":
        ok = device == HEADPHONES_ALSA
        return ("PASS" if ok else "FAIL",
                f"headphone -> {device}" + ("" if ok else
                                            f" (want {HEADPHONES_ALSA})"),
                "the 3.5 mm headphone jack")
    if (output == "auto" and eld_cards and not monitor_cards
            and headphones_listed):
        ok = device == HEADPHONES_ALSA
        return ("PASS" if ok else "FAIL",
                f"auto (no TV takes audio) -> {device}"
                + ("" if ok else f" (want {HEADPHONES_ALSA})"),
                "the 3.5 mm headphone jack (no HDMI sink takes audio)")
    want = [f"sysdefault:CARD={c}" for c in monitor_cards]
    if want:
        ok = device in want
        return ("PASS" if ok else "FAIL",
                f"{output} -> {device}" + ("" if ok else
                                           " (want " + " or ".join(want) + ")"),
                "the TV on " + "/".join(monitor_cards))
    ok = device.startswith("sysdefault:CARD=vc4hdmi")
    return ("PASS" if ok else "FAIL",
            f"{output} -> {device}" + ("" if ok else " (want a vc4hdmi card)"),
            "an HDMI port (no ELD evidence which one has the TV)")


def busy_reason(status: dict):
    """Why it is unsafe to run now (None = idle enough)."""
    if not status:
        return None
    if status.get("screen") == "retroarch" or status.get("retroarch"):
        return "a game is running (kiosk_status.json screen=retroarch)"
    kind = (status.get("now_playing") or {}).get("kind") or ""
    if kind in ("movie", "tv"):
        return f"a {kind} is playing in the Media Browser"
    return None


def apply_audio_output(settings_text: str, output: str) -> str:
    """settings.json with audio.output replaced (everything else kept)."""
    if output not in ("auto", "hdmi", "headphone"):
        raise ValueError(output)
    root = json.loads(settings_text)
    root.setdefault("audio", {})["output"] = output
    return json.dumps(root, indent=4) + "\n"


def parse_results_tsv(text: str) -> list:
    out = []
    for line in text.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        while len(parts) < 4:
            parts.append("")
        status, group, message, data = parts[:4]
        item = {"status": status, "group": group, "message": message}
        if data:
            try:
                item["data"] = json.loads(data)
            except ValueError:
                item["data"] = data
        out.append(item)
    return out


def build_report(results: list, meta: dict) -> dict:
    counts = {s.lower(): sum(r["status"] == s for r in results)
              for s in STATUSES}
    manual = [r["message"] for r in results if r["status"] == "MANUAL"]
    return {"tool": "hw_validate", "schema": 1, **meta,
            "summary": counts,
            "verdict": "FAIL" if counts["fail"] else "PASS",
            "results": results,
            "manual_from_run": manual,
            "manual_checklist": MANUAL_CHECKS}


# ---------------------------------------------------------------------------
# Box-side helpers (I/O)
# ---------------------------------------------------------------------------
def read_status() -> dict:
    for _ in range(5):
        try:
            with open(STATUS_PATH) as f:
                return json.load(f)
        except (OSError, ValueError):
            time.sleep(0.05)
    return {}


def journal_since(ts: float, unit: str = KIOSK_UNIT) -> str:
    since = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts))
    cmd = ["journalctl", "-u", unit, "--since", since, "--no-pager", "-o", "cat"]
    for prefix in (["sudo", "-n"], []):
        try:
            r = subprocess.run(prefix + cmd, capture_output=True, text=True,
                               timeout=30)
            if r.returncode == 0:
                return r.stdout
        except (OSError, subprocess.SubprocessError):
            continue
    return ""


def state_dirs() -> list:
    return [os.path.join(DATA_DIR, "states"),
            os.path.join(HOME, ".config", "retroarch", "states")]


def newest_state(since: float = 0.0):
    """(mtime, path) of the newest save-state file (.state, .state.auto,
    .stateN) under the state dirs, optionally only those newer than
    `since`."""
    best = None
    for root in state_dirs():
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                if ".state" not in name:
                    continue
                p = os.path.join(dirpath, name)
                try:
                    mt = os.path.getmtime(p)
                except OSError:
                    continue
                if mt > since and (best is None or mt > best[0]):
                    best = (mt, p)
    return best


def read_eld_cards():
    readable, monitors, info = [], [], {}
    for path in sorted(glob.glob("/proc/asound/vc4hdmi*/eld#0")):
        try:
            with open(path) as f:
                eld = parse_eld(f.read())
        except OSError:
            continue
        card = card_from_eld_path(path)
        readable.append(card)
        info[card] = eld
        if eld_takes_audio(eld):
            monitors.append(card)
    return readable, monitors, info


def headphones_listed() -> bool:
    try:
        out = subprocess.run(["aplay", "-L"], capture_output=True, text=True,
                             timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return any(line.rstrip() == HEADPHONES_ALSA for line in out.splitlines())


def _harness():
    sys.path.insert(0, SCRIPTS_DIR)
    import emulator_smoke_test as h  # noqa: E402 — on-box only
    return h


def _menu_ready(after: float):
    def pred(s):
        return (s.get("ts", 0) > after and s.get("screen") == "playlist"
                and s.get("retroarch") is None)
    return pred


def wait_menu(after: float, timeout: float) -> bool:
    pred = _menu_ready(after)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred(read_status()):
            return True
        time.sleep(0.5)
    return False


def take_screenshot(name: str, outdir: str, timeout: float = 15.0):
    """touch data/screenshot_request, wait for the new BMP, copy it into
    the report dir. Returns the copied path or None."""
    shots = os.path.join(DATA_DIR, "screenshots")
    req = os.path.join(DATA_DIR, "screenshot_request")
    before = {p: os.path.getmtime(p)
              for p in glob.glob(os.path.join(shots, "*.bmp"))}
    t0 = time.time()
    with open(req, "a"):
        pass
    deadline = t0 + timeout
    while time.time() < deadline:
        time.sleep(0.25)
        if os.path.exists(req):
            continue  # the worker removes it once the BMP is written
        fresh = []
        for p in glob.glob(os.path.join(shots, "*.bmp")):
            try:
                mt = os.path.getmtime(p)
            except OSError:
                continue
            if p not in before or mt > before[p]:
                fresh.append((mt, p))
        if fresh:
            src = max(fresh)[1]
            os.makedirs(outdir, exist_ok=True)
            dst = os.path.join(outdir, f"{name}.bmp")
            shutil.copy2(src, dst)
            return dst
    # Our own request file — never leave it behind to fire later.
    try:
        os.remove(req)
    except OSError:
        pass
    return None


def _launch_first_game(h, k, playlist_idx: int):
    """Open game list `playlist_idx`, launch game 0, wait for KMS ready.
    Returns (pid, status, launch_t0, log_cursor)."""
    log_cursor = h.launcher_log_cursor()
    h.open_game_playlist(k, playlist_idx)
    h.nav_cursor_to(k, "selected_game_index", 0)
    t0 = time.time()
    k.press(h.BTN_SELECT)
    st = k.wait_status(
        lambda s: s.get("screen") == "retroarch" and s.get("retroarch"),
        10, "kiosk to enter RetroArch launch mode")
    pid = h.wait_for_kms_takeover(k, t0, h.LAUNCH_TIMEOUT, log_cursor)
    return pid, st, t0, log_cursor


def _pick_playlist(names, prefer_re=PS1_PLAYLIST_RE):
    for i, n in enumerate(names):
        if prefer_re.search(n or ""):
            return i
    return 0 if names else None


# ---------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------
def cmd_busy(_args) -> int:
    st = read_status()
    if not st:
        print("kiosk_status.json unreadable")
        return 4
    reason = busy_reason(st)
    if reason:
        print(reason)
        return 3
    fresh = time.time() - st.get("ts", 0) < 10
    print(f"idle (screen={st.get('screen')}, status "
          f"{'fresh' if fresh else 'STALE'})")
    return 0 if fresh else 5


def cmd_verify_box(args) -> int:
    with open(args.file, errors="replace") as f:
        items, summary = parse_verify_box(f.read())
    if not items:
        emit("FAIL", "verify_box.sh produced no results — see " + args.file)
        return 0
    section = object()
    for it in items:
        if it["section"] != section:
            section = it["section"]
            print(f"  -- {section} --", flush=True)
        msg = it["message"]
        if it["detail"]:
            msg += " | " + " ; ".join(it["detail"][:6])
        emit(it["status"], msg, group=f"verify_box: {it['section']}")
    verdict = summary.get("verdict", "?")
    emit("PASS" if verdict == "SHIPPABLE" else "FAIL",
         f"verify_box verdict: {verdict} ({summary.get('passed', '?')} passed,"
         f" {summary.get('failed', '?')} failed, "
         f"{summary.get('warnings', '?')} warnings)", data=summary,
         group="verify_box")
    return 0


def cmd_eld(_args) -> int:
    readable, monitors, info = read_eld_cards()
    if not readable:
        emit("WARN", "no /proc/asound/vc4hdmi*/eld#0 readable (vc4 HDMI "
                     "audio driver not loaded?)")
        return 0
    for card in readable:
        eld = info[card]
        takes = card in monitors
        msg = (f"{card}: monitor_present={eld.get('monitor_present', '?')} "
               f"eld_valid={eld.get('eld_valid', '?')} "
               f"sad_count={eld.get('sad_count', '?')} "
               f"monitor='{eld.get('monitor_name', '')}' -> "
               + ("TV takes audio" if takes else "no audio sink"))
        status = "PASS"
        if eld.get("monitor_present") == 1 and not takes:
            status = "WARN"
            msg += " (a display is attached but advertises no audio)"
        emit(status, msg, data={k: eld.get(k) for k in (
            "monitor_present", "eld_valid", "sad_count", "monitor_name")})
    if not monitors:
        emit("WARN", "no HDMI port reports a TV that takes audio — HDMI "
                     "game audio cannot be validated by ear on this setup")
    return 0


def cmd_gating(args) -> int:
    st = read_status()
    names = (st.get("settings") or {}).get("game_playlist_names") or []
    if not names:
        emit("WARN", "kiosk_status.json has no settings.game_playlist_names "
                     "— cannot check which systems the menu offers")
        return 0
    bad = gating_violations(args.board, names, [])
    if bad:
        emit("FAIL", "Pi 4B offers Pi 5-only systems: " + "; ".join(bad),
             data={"playlists": names})
    elif args.board == "pi4":
        emit("PASS", f"Pi 4B menu hides N64/Dreamcast ({len(names)} game "
                     "playlists offered)", data={"playlists": names})
    else:
        emit("PASS", f"{len(names)} game playlists offered: "
                     + ", ".join(names), data={"playlists": names})
    return 0


def cmd_smoke_report(args) -> int:
    with open(args.file, errors="replace") as f:
        rep = parse_smoke_report(f.read())
    if not rep["found"]:
        emit("FAIL", f"emulator_smoke_test.py printed no report (exit "
                     f"{args.rc}) — see {args.file}")
        return 0
    for g in rep["games"]:
        msg = (f"{g['core']} '{g['rom']}' launch={g['launch_ms']}ms "
               f"return={g['return_ms']}ms")
        if g["errors"]:
            msg += " | " + " ; ".join(g["errors"])
        emit(g["status"], msg, data=g)
    s = rep["summary"] or {"clean": 0, "total": 0}
    emit("PASS" if s["total"] and s["clean"] == s["total"] else "FAIL",
         f"smoke test: {s['clean']}/{s['total']} games clean", data=s)
    if rep["restart"]:
        r = rep["restart"]
        emit(r["status"], f"restart-button path: launched={r['launched']} "
                          f"intro_replayed={r['intro_replayed']} "
                          f"recovered={r['recovered']}", data=r)
    tested = {normalize_core(g["core"]) for g in rep["games"]} - {"?"}
    texts = []
    for p in glob.glob(os.path.join(DATA_DIR, "playlists", "*.yaml")):
        with open(p, errors="replace") as f:
            texts.append(f.read())
    want = expected_cores(texts, args.board)
    missing = sorted(want - tested)
    if missing:
        emit("WARN", "cores referenced by playlists but not exercised: "
                     + ", ".join(missing))
    elif want:
        emit("PASS", f"every core available on this board was exercised "
                     f"({len(want)})")
    bad = gating_violations(args.board, [], tested)
    if bad:
        emit("FAIL", "Pi 4B launched a gated core: " + "; ".join(bad))
    return 0


def cmd_stop_test(args) -> int:
    """systemctl stop mid-game: RetroArch must get SIGTERM, save its auto
    state, and the kiosk must exit inside TimeoutStopSec."""
    h = _harness()
    k = h.Kiosk()
    names = (k.status().get("settings") or {}).get("game_playlist_names") or []
    idx = None
    for i, n in enumerate(names):
        if PS1_PLAYLIST_RE.search(n or ""):
            idx = i
            break
    if idx is None:
        emit("WARN", "no PlayStation playlist — stop-mid-game test skipped")
        return 0
    try:
        pid, st, _t0, _cur = _launch_first_game(h, k, idx)
    except Exception as e:  # noqa: BLE001 — report, never traceback
        emit("FAIL", f"PS1 launch for the stop test failed: {e}")
        h.recover(k)
        return 0
    ra = st.get("retroarch") or {}
    emit("PASS", f"PS1 game running: '{ra.get('rom_name', '?')}' "
                 f"({ra.get('core', '?')}, pid {pid})")
    time.sleep(args.play_seconds)
    before = newest_state()
    note("newest save state before stop: "
         + (f"{before[1]} @ {time.ctime(before[0])}" if before else "none"))

    t_stop = time.time()
    r = subprocess.run(["sudo", "-n", "systemctl", "stop", KIOSK_UNIT],
                       capture_output=True, text=True, timeout=90)
    dur = time.time() - t_stop
    emit("PASS" if r.returncode == 0 and dur < 20 else "FAIL",
         f"systemctl stop mid-game took {dur:.1f}s (limit 20s, "
         f"TimeoutStopSec)", data={"stop_seconds": round(dur, 2)})
    time.sleep(1.0)
    j = journal_since(t_stop - 1)
    want = "stopped on kiosk shutdown request (exited after SIGTERM)"
    emit("PASS" if want in j else "FAIL",
         ("journal: '" + want + "'") if want in j else
         ("journal lacks '" + want + "'"))
    if "SIGKILLed" in j or "ignored SIGTERM" in j:
        emit("FAIL", "RetroArch ignored SIGTERM and was SIGKILLed — the "
                     "auto save-state may be lost")
    if re.search(r"(?i)timed out\. killing|failed with result 'timeout'", j):
        emit("FAIL", "systemd hit TimeoutStopSec and killed the kiosk")
    after = newest_state(since=t_stop - 0.5)
    if after:
        emit("PASS", f"fresh save state written at stop: "
                     f"{os.path.relpath(after[1], '/')} "
                     f"(+{after[0] - t_stop:.1f}s)")
    else:
        emit("FAIL", "no save state newer than the stop — RetroArch did not "
                     "auto-save on the kiosk's SIGTERM")
    leftover = subprocess.run(["pgrep", "-x", "retroarch"],
                              capture_output=True, text=True).stdout.strip()
    if leftover:
        emit("FAIL", f"retroarch still running after kiosk stop: {leftover}")
        subprocess.run(["pkill", "-TERM", "-x", "retroarch"])
        time.sleep(5)
        subprocess.run(["pkill", "-KILL", "-x", "retroarch"])
    t_start = time.time()
    subprocess.run(["sudo", "-n", "systemctl", "start", KIOSK_UNIT])
    if wait_menu(t_start, 120):
        emit("PASS", "kiosk restarted to the menu after the stop test")
    else:
        emit("FAIL", "kiosk did not reach the menu within 120s after restart")
    return 0


def cmd_audio_probe(args) -> int:
    """Launch one game with the current audio.output, record the ALSA
    device RetroArch got, give a human time to listen, then quit and do
    the 0-second post-game input check."""
    h = _harness()
    k = h.Kiosk()
    names = (k.status().get("settings") or {}).get("game_playlist_names") or []
    idx = _pick_playlist(names)
    if idx is None:
        emit("FAIL", f"{args.output}: no game playlists to launch")
        return 0
    try:
        pid, st, t0, cursor = _launch_first_game(h, k, idx)
    except Exception as e:  # noqa: BLE001
        emit("FAIL", f"{args.output}: game launch failed: {e}")
        h.recover(k)
        return 0
    log = h.launcher_log_since(cursor)
    m = re.findall(r"^ALSA device: (.*)$", log, re.M)
    device = m[-1].strip() if m else ""
    eld_cards, monitor_cards, _info = read_eld_cards()
    status, msg, human = check_game_alsa(args.output, device, eld_cards,
                                         monitor_cards, headphones_listed())
    ra = st.get("retroarch") or {}
    emit(status, f"audio.output={msg}",
         data={"output": args.output, "alsa_device": device,
               "monitor_cards": monitor_cards, "rom": ra.get("rom_name")})
    emit("MANUAL", f"audio.output={args.output}: confirm game sound "
                   f"('{ra.get('rom_name', '?')}') came from {human} "
                   f"[ALSA {device or '?'}]")
    print(f"\n  >>> LISTEN NOW for {args.listen}s: sound should come from "
          f"{human}\n", flush=True)
    time.sleep(args.listen)

    h.signal_retroarch_pid(pid)
    try:
        k.wait_status(h.kiosk_alive_and_in_menu, h.RETURN_TIMEOUT,
                      "kiosk to return to menu")
    except TimeoutError as e:
        emit("FAIL", f"{args.output}: kiosk did not return to the menu: {e}")
        h.recover(k)
        return 0
    # Post-game input check, ZERO settle: the 2026-10-03 bug published
    # "menu" before the post-game reset finished, and the first press
    # opened Settings then landed a SELECT on Master Shuffle.
    k.press(h.BTN_SETTINGS)
    try:
        k.wait_status(lambda s: (s.get("settings") or {}).get("active"), 5,
                      "settings to open right after the game")
        opened = True
    except TimeoutError:
        opened = False
    time.sleep(1.0)
    j = journal_since(t0 - 1)
    shuffle = "Master Shuffle selected" in j
    emit("PASS" if opened and not shuffle else "FAIL",
         "post-game input (0 s settle): "
         + ("Settings opened" if opened else "Settings did NOT open")
         + ("; Master Shuffle FIRED" if shuffle else "; no Master Shuffle"))
    if (k.status().get("settings") or {}).get("active"):
        k.press(h.BTN_SETTINGS)
        try:
            k.wait_status(lambda s: not (s.get("settings") or {}).get("active"),
                          5, "settings to close")
        except TimeoutError:
            k.press(h.BTN_SETTINGS)
    return 0


def _close_settings(h, k):
    for _ in range(3):
        if not (k.status().get("settings") or {}).get("active"):
            return True
        k.press(h.BTN_SETTINGS)
        time.sleep(0.6)
    return not (k.status().get("settings") or {}).get("active")


def cmd_screenshots(args) -> int:
    h = _harness()
    k = h.Kiosk()
    out = args.outdir

    def shot(name, settle=1.5):
        time.sleep(settle)
        p = take_screenshot(name, out)
        if p:
            emit("PASS", f"screenshot {name}: {p}", data={"path": p})
        else:
            emit("FAIL", f"screenshot {name}: no BMP appeared in "
                         f"{DATA_DIR}/screenshots within 15s")
        return p

    try:
        _close_settings(h, k)
        shot("main_menu", settle=2.0)
        h.open_settings(k)
        shot("settings")
        h.nav_to_label(k, "Connect a Device")
        k.press(h.BTN_SELECT)
        p = shot("pairing", settle=3.0)
        if p:
            with open(p, "rb") as f:
                res = qr_contrast(Bmp(f.read()))
            emit("PASS" if res["ok"] else "FAIL",
                 "pairing QR region has light+dark modules "
                 f"(light {res['light_frac']:.0%}, dark {res['dark_frac']:.0%})"
                 if res["ok"] else
                 "pairing QR looks like a solid square (light "
                 f"{res['light_frac']:.0%}, dark {res['dark_frac']:.0%}) — "
                 "QR not rendered", data=res)
        k.press(h.BTN_SETTINGS)  # pairing screen -> settings
        time.sleep(0.8)
        _close_settings(h, k)
    except Exception as e:  # noqa: BLE001
        emit("FAIL", f"screenshot navigation failed: {e}")
        _close_settings(h, k)
        return 0

    if not args.mb:
        emit("PASS", "Media Browser locked — Movies screenshots skipped")
        return 0
    try:
        h.open_settings(k)
        h.nav_to_label(k, "Movies")
        k.press(h.BTN_SELECT)
        k.wait_status(lambda s: s.get("screen") == "media_browser", 15,
                      "Media Browser to open")
        shot("movies_browse", settle=6.0)  # let posters stream in
        emit("MANUAL", "Movies Library screenshot: navigate to Library, then "
                       f"`touch {DATA_DIR}/screenshot_request` and eyeball it")
    except Exception as e:  # noqa: BLE001
        emit("WARN", f"Movies screenshots skipped: {e}")
    finally:
        if read_status().get("screen") == "media_browser":
            # BTN4 long-press (>= 500 ms) exits the Media Browser.
            k.press(h.BTN_SETTINGS, phase="down")
            time.sleep(0.9)
            k.press(h.BTN_SETTINGS, phase="up")
            try:
                k.wait_status(lambda s: s.get("screen") != "media_browser",
                              10, "Media Browser to exit")
            except TimeoutError:
                emit("WARN", "could not leave the Media Browser — exit it by "
                             "hand (long-press the black button)")
        _close_settings(h, k)
    return 0


def cmd_harness_ready(_args) -> int:
    """Can the smoke harness drive the kiosk? (paired remote + secret)."""
    try:
        h = _harness()
        h.forge_cookie()
    except Exception as e:  # noqa: BLE001
        print(f"harness not usable: {e}")
        return 1
    print("harness ready")
    return 0


def cmd_wait_menu(args) -> int:
    ok = wait_menu(args.after, args.timeout)
    if ok:
        time.sleep(2.0)
    return 0 if ok else 1


def cmd_set_audio_output(args) -> int:
    with open(args.settings, encoding="utf-8") as f:
        text = f.read()
    new = apply_audio_output(text, args.output)
    tmp = args.settings + ".hwv.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(new)
    shutil.copymode(args.settings, tmp)
    os.replace(tmp, args.settings)
    return 0


def cmd_settings_value(args) -> int:
    try:
        with open(args.settings, encoding="utf-8") as f:
            node = json.load(f)
        for key in args.path.split("."):
            node = node[key]
    except (OSError, ValueError, KeyError, TypeError):
        print(args.default)
        return 0
    print(json.dumps(node) if isinstance(node, (dict, list)) else
          (str(node).lower() if isinstance(node, bool) else node))
    return 0


def cmd_cpu(args) -> int:
    path = f"/proc/{args.pid}/stat"
    clk = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100
    try:
        with open(path) as f:
            a = proc_stat_ticks(f.read())
        t0 = time.time()
        st0 = read_status()
        time.sleep(args.seconds)
        with open(path) as f:
            b = proc_stat_ticks(f.read())
        dt = time.time() - t0
    except (OSError, ValueError) as e:
        emit("WARN", f"kiosk CPU sample failed: {e}")
        return 0
    pct = cpu_percent(a, b, dt, clk)
    st1 = read_status()
    static_menu = all(s.get("screen") == "playlist"
                      and not (s.get("settings") or {}).get("active")
                      and not (s.get("playback") or {}).get("duration_sec")
                      for s in (st0, st1))
    ctx = "static main menu" if static_menu else f"screen={st1.get('screen')}"
    status = "PASS"
    if static_menu and pct > args.warn_pct:
        status = "WARN"
    emit(status, f"kiosk CPU {pct}% of one core over {dt:.0f}s ({ctx})"
         + (f" — above {args.warn_pct}% on an idle menu; the redraw gate "
            "should idle it" if status == "WARN" else ""),
         data={"cpu_pct": pct, "seconds": round(dt, 1), "context": ctx})
    return 0


def cmd_redraw(args) -> int:
    text = ""
    for p in args.files:
        try:
            with open(p, errors="replace") as f:
                text += f.read()
        except OSError:
            continue
    reps = parse_redraw_reports(text)
    if not reps:
        emit("MANUAL", "no per-minute 'Redraw gate: drew/skipped' report "
                       "found yet — leave the box on the static main menu "
                       "for 60 s and re-run")
        return 0
    r = reps[-1]
    emit("PASS", f"latest redraw report: drew {r['drawn']} / skipped "
                 f"{r['skipped']} in {r['window_s']}s (crt30 {r['crt30']})",
         data=r)
    return 0


def _read_lines(paths):
    lines = []
    for p in paths:
        try:
            with open(p, errors="replace") as f:
                lines.extend(f.read().splitlines())
        except OSError:
            continue
    return lines


def cmd_artwork(args) -> int:
    status, msg, data = classify_artwork(_read_lines(args.files), time.time())
    emit(status, msg, data=data)
    return 0


def cmd_decoders(args) -> int:
    text = "\n".join(_read_lines(args.files))
    status, msg, data = classify_decoders(text, args.board)
    emit(status, msg, data=data)
    return 0


def cmd_report(args) -> int:
    try:
        with open(args.results, encoding="utf-8") as f:
            results = parse_results_tsv(f.read())
    except OSError:
        results = []
    meta = {}
    for kv in args.meta:
        k, _, v = kv.partition("=")
        meta[k] = v
    rep = build_report(results, meta)
    tmp = args.out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=2)
        f.write("\n")
    os.replace(tmp, args.out)

    c = rep["summary"]
    print("\n== MANUAL checks (a human must do these) ==")
    for m in rep["manual_from_run"]:
        print(f"  [MANUAL] {m}")
    for m in MANUAL_CHECKS:
        print(f"  [MANUAL] {m}")
    print("\n== RESULT ==")
    print(f"  {c['pass']} passed, {c['fail']} failed, {c['warn']} warnings, "
          f"{c['manual']} manual")
    if c["fail"]:
        print("  FAILED:")
        for r in results:
            if r["status"] == "FAIL":
                print(f"    - [{r['group']}] {r['message']}")
    print(f"  report: {args.out}")
    print("  VALIDATION " + ("PASSED (automated part)" if not c["fail"]
                             else "FAILED"))
    return 1 if c["fail"] else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("busy")
    sub.add_parser("harness-ready")
    sub.add_parser("eld")
    p = sub.add_parser("verify-box"); p.add_argument("file")
    p = sub.add_parser("gating"); p.add_argument("--board", default="unknown")
    p = sub.add_parser("smoke-report")
    p.add_argument("file"); p.add_argument("--board", default="unknown")
    p.add_argument("--rc", default="?")
    p = sub.add_parser("stop-test")
    p.add_argument("--play-seconds", type=float, default=15.0)
    p = sub.add_parser("audio-probe")
    p.add_argument("--output", required=True,
                   choices=["auto", "hdmi", "headphone"])
    p.add_argument("--listen", type=float, default=8.0)
    p = sub.add_parser("screenshots")
    p.add_argument("--outdir", required=True)
    p.add_argument("--mb", action="store_true")
    p = sub.add_parser("wait-menu")
    p.add_argument("--after", type=float, required=True)
    p.add_argument("--timeout", type=float, default=120.0)
    p = sub.add_parser("set-audio-output")
    p.add_argument("settings"); p.add_argument("output")
    p = sub.add_parser("settings-value")
    p.add_argument("settings"); p.add_argument("path")
    p.add_argument("--default", default="")
    p = sub.add_parser("cpu")
    p.add_argument("pid"); p.add_argument("--seconds", type=float, default=15)
    p.add_argument("--warn-pct", type=float, default=80.0)
    p = sub.add_parser("redraw"); p.add_argument("files", nargs="*")
    p = sub.add_parser("artwork"); p.add_argument("files", nargs="*")
    p = sub.add_parser("decoders")
    p.add_argument("--board", default="unknown")
    p.add_argument("files", nargs="*")
    p = sub.add_parser("report")
    p.add_argument("--results", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--meta", action="append", default=[])

    args = ap.parse_args(argv)
    handler = {
        "busy": cmd_busy, "harness-ready": cmd_harness_ready,
        "eld": cmd_eld, "verify-box": cmd_verify_box, "gating": cmd_gating,
        "smoke-report": cmd_smoke_report, "stop-test": cmd_stop_test,
        "audio-probe": cmd_audio_probe, "screenshots": cmd_screenshots,
        "wait-menu": cmd_wait_menu, "set-audio-output": cmd_set_audio_output,
        "settings-value": cmd_settings_value, "cpu": cmd_cpu,
        "redraw": cmd_redraw, "artwork": cmd_artwork,
        "decoders": cmd_decoders, "report": cmd_report,
    }[args.cmd]
    return handler(args)


if __name__ == "__main__":
    sys.exit(main())
