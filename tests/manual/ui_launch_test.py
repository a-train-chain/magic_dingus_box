#!/usr/bin/env python3
"""End-to-end UI launch test for Magic Dingus Box.

Drives the kiosk through the REAL main-menu UI using the phone-remote
uinput gamepad (the same virtual device production phones use), reading
kiosk_status.json for closed-loop feedback. For each console playlist,
launches the first N games from the Video Games browser, verifies
RetroArch actually takes over with the expected core, exits via the
QUIT_GAME chord, and verifies the kiosk recovers.

Run on the Pi as root:  sudo python3 ui_launch_test.py
"""
import json
import subprocess
import sys
import time

sys.path.insert(0, "/opt/magic_dingus_box")
from magic_dingus_box.web.remote.uinput_writer import UinputWriter, ButtonName

STATUS = "/opt/magic_dingus_box/magic_dingus_box_cpp/data/kiosk_status.json"
GAMES_PER_CONSOLE = 2
CONSOLES = [
    ("NES Games", "nestopia"),
    ("SNES Games", "snes9x2010"),
    ("Genesis Games", "genesis_plus_gx"),
    ("PlayStation Games", "pcsx_rearmed"),
    ("PC Engine Games", "mednafen_pce_fast"),
    ("Atari 7800 Games", "prosystem"),
    ("Arcade Games", "fbneo"),
]

w = UinputWriter()

def status():
    try:
        with open(STATUS) as f:
            return json.load(f)
    except Exception:
        return {}

def sm(field, default=None):
    s = status()
    node = s.get("settings") or {}
    return node.get(field, s.get(field, default))

def screen():
    return status().get("screen", "")

def retroarch_core():
    try:
        out = subprocess.run(["pgrep", "-af", "retroarch"], capture_output=True,
                             text=True, timeout=5).stdout
        for line in out.splitlines():
            if "-L" in line:
                return line
    except Exception:
        pass
    return ""

def tap(btn, settle=0.55):
    w.press(btn, "tap")
    time.sleep(settle)

def wait_for(pred, timeout, poll=0.5):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(poll)
    return False

def steer_until(field, target, max_presses=20, btn=ButtonName.DOWN):
    """Press btn until sm(field) == target. Returns True on match."""
    for _ in range(max_presses):
        if sm(field) == target:
            return True
        tap(btn)
    return sm(field) == target


def kiosk_restart():
    subprocess.run(["systemctl", "restart", "magic-dingus-box-cpp.service"],
                   capture_output=True, timeout=120)
    # Type=notify start + intro video before the playlist screen appears.
    return wait_for(lambda: screen() == "playlist", 90, poll=1.0)

def exit_game(console, game_idx):
    """Escalating exit: chord -> pkill -> pkill -9 -> service restart.
    Returns the method that worked, or None."""
    # Hold the chord ~400ms like a real finger — an instantaneous tap can
    # fall between RetroArch's input polls and be missed entirely.
    w.press(ButtonName.QUIT_GAME, "down")
    time.sleep(0.4)
    w.press(ButtonName.QUIT_GAME, "up")
    if wait_for(lambda: screen() != "retroarch" and not retroarch_core(), 12):
        return "chord"
    subprocess.run(["pkill", "retroarch"], capture_output=True)
    if wait_for(lambda: screen() != "retroarch" and not retroarch_core(), 25):
        return "pkill"
    subprocess.run(["pkill", "-9", "retroarch"], capture_output=True)
    if wait_for(lambda: screen() != "retroarch" and not retroarch_core(), 15):
        return "pkill9"
    if kiosk_restart():
        return "service_restart"
    return None


results = []

def fail(console, game_idx, reason):
    results.append({"console": console, "game": game_idx, "ok": False, "reason": reason})
    print(f"FAIL  {console} #{game_idx}: {reason}", flush=True)

def ensure_main_ui(tries=6):
    """Get back to the main playlist screen from wherever we are."""
    for _ in range(tries):
        s = screen()
        if s == "playlist":
            return True
        if s == "retroarch":
            w.press(ButtonName.QUIT_GAME, "tap")
            time.sleep(6)
            continue
        tap(ButtonName.BLACK, settle=1.2)  # toggle settings / back out
    if screen() == "playlist":
        return True
    return kiosk_restart()

print("initial status keys:", json.dumps({k: status().get(k) for k in ("screen",)}), flush=True)
if not ensure_main_ui():
    print("ABORT: could not reach main UI", flush=True)
    sys.exit(2)


CORE_BY_TITLE = {
    "NES Games": "nestopia", "SNES Games": "snes9x2010",
    "Genesis Games": "genesis_plus_gx", "PlayStation Games": "pcsx_rearmed",
    "PC Engine Games": "mednafen_pce_fast", "Atari 7800 Games": "prosystem",
    "Arcade Games": "fbneo",
}

def steer_int(field, target, max_presses=25):
    """Directionally steer an integer cursor field to target."""
    for _ in range(max_presses):
        cur = sm(field)
        if cur == target:
            return True
        if not isinstance(cur, int) or cur < 0:
            time.sleep(0.3)
            cur = sm(field)
            if cur == target:
                return True
            if not isinstance(cur, int) or cur < 0:
                return False
        tap(ButtonName.DOWN if cur < target else ButtonName.UP)
    return sm(field) == target

def open_game_browser():
    """From main UI: settings -> Video Games -> browser active."""
    tap(ButtonName.BLACK, settle=1.2)
    if not wait_for(lambda: sm("active"), 4):
        # One retry — right after a service start the intro video may
        # still be swallowing input.
        time.sleep(5)
        tap(ButtonName.BLACK, settle=1.2)
        if not wait_for(lambda: sm("active"), 4):
            return False
    if not steer_until("highlighted_label", "Video Games"):
        # try steering upward in case we started below it
        if not steer_until("highlighted_label", "Video Games", btn=ButtonName.UP):
            return False
    tap(ButtonName.OK, settle=1.0)
    return wait_for(lambda: sm("game_browser_active"), 5)

count = None
for _ in range(10):
    st = status()
    count = (st.get("settings") or {}).get("game_playlist_count")
    if count:
        break
    time.sleep(0.5)
if not count:
    print("ABORT: no game_playlist_count in status", flush=True)
    sys.exit(2)
print(f"consoles reported: {count}", flush=True)

for idx in range(count):
    for game_idx in range(GAMES_PER_CONSOLE):
        if not ensure_main_ui():
            fail(f"console[{idx}]", game_idx, "could not reach main UI")
            continue
        if not open_game_browser():
            fail(f"console[{idx}]", game_idx, "could not open game browser")
            continue
        if not steer_int("game_browser_selected", idx):
            fail(f"console[{idx}]", game_idx,
                 f"cursor stuck at {sm('game_browser_selected')!r}")
            continue
        tap(ButtonName.OK, settle=1.0)
        if not wait_for(lambda: sm("viewing_games"), 5):
            fail(f"console[{idx}]", game_idx, "games list did not open")
            continue
        console = sm("game_playlist_name") or f"console[{idx}]"
        n_games = sm("games_in_playlist") or 0
        if game_idx >= n_games:
            fail(console, game_idx, f"only {n_games} games in playlist")
            continue
        if not steer_int("selected_game_index", game_idx):
            fail(console, game_idx, "could not steer to game")
            continue

        tap(ButtonName.OK, settle=0.5)  # LAUNCH
        if not wait_for(lambda: screen() == "retroarch", 30):
            fail(console, game_idx, f"screen never became retroarch (at {screen()!r})")
            ensure_main_ui()
            continue

        time.sleep(8)
        proc = retroarch_core()
        core_frag = CORE_BY_TITLE.get(console, "")
        core_ok = bool(core_frag) and core_frag in proc
        if not proc:
            fail(console, game_idx, "no retroarch process while screen=retroarch")
            ensure_main_ui()
            continue

        method = exit_game(console, game_idx)
        recovered = method is not None

        ok = core_ok and recovered
        results.append({"console": console, "game": game_idx, "ok": ok,
                        "core_ok": core_ok, "exit_method": method, "proc": proc[-110:]})
        print(f"{'PASS' if ok else 'FAIL'}  {console} #{game_idx} core_ok={core_ok} exit={method}", flush=True)
        time.sleep(4)

ensure_main_ui()
passed = sum(1 for r in results if r["ok"])
print(f"\n=== UI LAUNCH TEST: {passed}/{len(results)} passed ===", flush=True)
print(json.dumps(results, indent=1), flush=True)
sys.exit(0 if passed == len(results) else 1)
