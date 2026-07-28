#!/usr/bin/env python3
"""Emulator smoke test for the Magic Dingus Box kiosk.

Drives the kiosk through Settings -> Browse Games, launches a game on each
RetroArch core, plays briefly, quits cleanly, and verifies the kiosk
recovers to the menu. Also exercises the restart-button path (which cold-
restarts the kiosk service and replays the intro).

HOW IT DRIVES THE KIOSK
  - Input: the existing "MagicDingus Phone Remote" virtual gamepad, driven
    through the web admin's POST /admin/remote/_debug/press endpoint. Auth
    is an HMAC cookie forged here from data/flask_secret.key + a paired
    device id (data/paired_remotes.json) — the same scheme the web service
    itself uses.
  - State: data/kiosk_status.json, which the kiosk writes at 5 Hz plus on
    RetroArch enter/exit. Fields used: screen, retroarch.{rom_name,core},
    and the settings.* game-browser cursor (added for closed-loop nav).
  - Health: the systemd journal for the kiosk service (launch/return log
    strings), /home/magic/retroarch_launcher.log (fatal video signatures),
    and the fresh /tmp/retroarch_mdb.ready KMS-takeover marker.

QUIT MECHANISM
  Games are quit by sending SIGTERM to the retroarch process. From the
  kiosk's perspective this is identical to a RetroArch-menu Quit: waitpid()
  returns and the normal DRM-reacquire / input-reinit / EGL-restore
  recovery path runs. (There is no one-button quit hotkey in the config,
  so signalling is the reliable programmatic exit.)

SAFETY
  Every launch has a timeout. If the kiosk doesn't return to the menu, the
  harness escalates: SIGKILL retroarch, then (last resort) restart the
  kiosk service, and records the launch as FAILED rather than hanging.

Run ON the Pi:
  python3 scripts/emulator_smoke_test.py            # full run
  python3 scripts/emulator_smoke_test.py --dry-run  # nav only, no games
  python3 scripts/emulator_smoke_test.py --games 1  # 1 game per core
  python3 scripts/emulator_smoke_test.py --no-restart-test
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request
import urllib.error

DATA_DIR = os.environ.get(
    "MAGIC_DATA_DIR", "/opt/magic_dingus_box/magic_dingus_box_cpp/data")
STATUS_PATH = os.path.join(DATA_DIR, "kiosk_status.json")
SECRET_PATH = os.path.join(DATA_DIR, "flask_secret.key")
PAIRED_PATH = os.path.join(DATA_DIR, "paired_remotes.json")
LAUNCHER_LOG = os.environ.get(
    "MAGIC_RETROARCH_LOG", "/home/magic/retroarch_launcher.log")
READY_MARKER = os.environ.get(
    "MAGIC_RETROARCH_READY", "/tmp/retroarch_mdb.ready")
BASE_URL = os.environ.get("MAGIC_WEB_URL", "http://127.0.0.1:5000")
KIOSK_UNIT = "magic-dingus-box-cpp.service"

# Button names understood by the phone-remote UinputWriter.
BTN_SETTINGS = "BLACK"   # -> InputAction::SETTINGS_MENU (open/close settings)
BTN_SELECT = "OK"        # -> InputAction::SELECT (confirm / launch)
BTN_DOWN = "DOWN"        # -> ROTATE_VERTICAL +1
BTN_UP = "UP"            # -> ROTATE_VERTICAL -1

# Timing (seconds)
PRESS_SETTLE = 0.35      # wait after a press for the status file to update
LAUNCH_TIMEOUT = 45      # SELECT -> fresh, live KMS-ready RetroArch PID
PLAY_SECONDS = 7         # let the core reach steady state (catch Vulkan thrash)
RETURN_TIMEOUT = 40      # quit -> screen==playlist
NAV_MAX_STEPS = 40       # bound blind-ish navigation loops


class Colors:
    G = "\033[92m"; R = "\033[91m"; Y = "\033[93m"; B = "\033[94m"; X = "\033[0m"


def log(msg, c=""):
    ts = time.strftime("%H:%M:%S")
    print(f"{c}[{ts}] {msg}{Colors.X}", flush=True)


# ---------------------------------------------------------------------------
# Auth + input injection
# ---------------------------------------------------------------------------
def forge_cookie() -> str:
    with open(SECRET_PATH) as f:
        secret = f.read().strip().encode()
    with open(PAIRED_PATH) as f:
        devices = json.load(f).get("devices", [])
    if not devices:
        raise RuntimeError(
            "No paired remote in paired_remotes.json — pair a phone first.")
    device_id = devices[0]["id"]
    issued_at = int(time.time())
    sig = hmac.new(secret, f"{device_id}|{issued_at}".encode(),
                   hashlib.sha256).hexdigest()
    return f"{device_id}.{issued_at}.{sig}"


class Kiosk:
    def __init__(self):
        self.cookie = forge_cookie()

    def press(self, btn, phase="tap"):
        url = f"{BASE_URL}/admin/remote/_debug/press?btn={btn}&phase={phase}"
        req = urllib.request.Request(url, method="POST",
                                     headers={"Cookie": f"mdb_remote={self.cookie}"})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                r.read()
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"press({btn}) HTTP {e.code}: {e.read()[:120]}")
        time.sleep(PRESS_SETTLE)

    def status(self) -> dict:
        # Atomic-rename writer means a plain read is always a complete doc;
        # retry briefly on the rare race.
        for _ in range(5):
            try:
                with open(STATUS_PATH) as f:
                    return json.load(f)
            except (json.JSONDecodeError, FileNotFoundError):
                time.sleep(0.05)
        return {}

    def wait_status(self, pred, timeout, desc=""):
        deadline = time.time() + timeout
        last = {}
        while time.time() < deadline:
            last = self.status()
            if pred(last):
                return last
            time.sleep(0.2)
        raise TimeoutError(f"timeout waiting for {desc}; last status: "
                           f"screen={last.get('screen')} "
                           f"settings={last.get('settings')} "
                           f"retroarch={last.get('retroarch')}")


# ---------------------------------------------------------------------------
# Journal + launcher-log health
# ---------------------------------------------------------------------------
def journal_since(cursor_ts: float) -> str:
    since = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(cursor_ts))
    try:
        out = subprocess.run(
            ["journalctl", "-u", KIOSK_UNIT, "--since", since, "--no-pager"],
            capture_output=True, text=True, timeout=15)
        return out.stdout
    except Exception as e:
        return f"(journal read failed: {e})"


def launcher_log_cursor() -> int:
    try:
        return os.path.getsize(LAUNCHER_LOG)
    except OSError:
        return 0


def launcher_log_since(cursor: int) -> str:
    try:
        with open(LAUNCHER_LOG, errors="replace") as launcher_log:
            size = os.fstat(launcher_log.fileno()).st_size
            launcher_log.seek(cursor if 0 <= cursor <= size else 0)
            return launcher_log.read()
    except OSError:
        return ""


def retroarch_pid_is_live(pid: int) -> bool:
    """Require both a live process and the actual RetroArch executable."""
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        with open(f"/proc/{pid}/comm") as comm:
            return comm.read().strip() == "retroarch"
    except (OSError, ValueError):
        return False


def read_ready_pid(marker_path: str, launch_started_at: float):
    """Return the live RetroArch PID from a marker created by this launch."""
    try:
        marker_stat = os.stat(marker_path)
        if marker_stat.st_mtime < launch_started_at:
            return None
        with open(marker_path) as marker:
            fields = marker.read().split()
        if len(fields) != 1:
            return None
        pid = int(fields[0])
    except (OSError, ValueError):
        return None
    return pid if retroarch_pid_is_live(pid) else None


def launch_log_failure(log_text: str):
    fatal_signatures = (
        ("QueuePresent failed", "QueuePresent failed in Vulkan KMS"),
        ("did not take over KMS within 15 seconds", "KMS takeover timed out"),
        ("exited before taking over KMS", "RetroArch exited before KMS takeover"),
    )
    for signature, message in fatal_signatures:
        if signature in log_text:
            return message
    # RetroArch may probe Wayland before falling through to a direct-display
    # context. That probe is harmless only when the same launch subsequently
    # confirms one. A lone Wayland failure remains fatal.
    #
    # There are TWO valid fallbacks, one per renderer, and this used to know
    # only about the Vulkan one — so every healthy N64/Dreamcast launch on
    # the GL path was reported as a fatal video error (observed on the Pi 5:
    # Banjo-Kazooie launched, hit KMS at 1920x1080, and still came back
    # FAIL). Vulkan cores land on "khr_display"; GL cores (mupen64plus_next,
    # parallel_n64) land on "kms".
    if ("Failed to connect to Wayland server" in log_text and
            'Found vulkan context: "khr_display"' not in log_text and
            'Found GL context: "kms"' not in log_text):
        return "Wayland display connection failed without KMS/KHR fallback"
    return None


PS1_DYNAREC_OK = re.compile(r"(?i)dynarec|dynamic recompiler")
PS1_DYNAREC_BAD = re.compile(r"(?i)lightrec|falling back to interpreter")


def check_ps1_dynarec(log_text: str):
    """A future core update that silently drops ari64 for Lightrec or the
    interpreter is a multi-x PS1 slowdown — fail the smoke run loudly.
    The healthy line on this build: 'Init new dynarec, ndrc size ...'."""
    if PS1_DYNAREC_BAD.search(log_text):
        return ("PS1 core is not using the ari64 dynarec "
                "(lightrec/interpreter found)")
    if not PS1_DYNAREC_OK.search(log_text):
        return "PS1 launch log has no dynarec initialization line"
    return None


def wait_for_kms_takeover(k: Kiosk, launch_started_at: float,
                          timeout: float, log_cursor: int) -> int:
    """Wait for a fresh marker, failing early if the kiosk cancels launch."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        pid = read_ready_pid(READY_MARKER, launch_started_at)
        if pid is not None:
            return pid

        status = k.status()
        if (time.time() - launch_started_at > 0.5 and
                status.get("screen") == "playlist"):
            failure = launch_log_failure(launcher_log_since(log_cursor))
            detail = failure or "launcher returned to the menu before KMS readiness"
            raise RuntimeError(detail)
        time.sleep(0.1)

    failure = launch_log_failure(launcher_log_since(log_cursor))
    detail = f": {failure}" if failure else ""
    raise TimeoutError(f"timeout waiting for fresh KMS readiness marker{detail}")


# ---------------------------------------------------------------------------
# Navigation
# ---------------------------------------------------------------------------
def open_settings(k: Kiosk):
    st = k.status()
    if not st.get("settings", {}).get("active"):
        k.press(BTN_SETTINGS)
        k.wait_status(lambda s: s.get("settings", {}).get("active"),
                      5, "settings menu to open")


def nav_to_label(k: Kiosk, target: str):
    """Press DOWN until the highlighted settings row matches `target`."""
    for _ in range(NAV_MAX_STEPS):
        label = k.status().get("settings", {}).get("highlighted_label", "")
        if label == target:
            return
        k.press(BTN_DOWN)
    raise RuntimeError(f"could not reach settings row '{target}'")


def enter_game_browser(k: Kiosk):
    # Main menu "Video Games" leads to the game browser. On this build a
    # single SELECT jumps straight in; on others it may open a submenu with
    # a "Browse Games" entry. Handle both.
    open_settings(k)
    if k.status().get("settings", {}).get("game_browser_active"):
        return
    nav_to_label(k, "Video Games")
    k.press(BTN_SELECT)
    st = k.status().get("settings", {})
    if not st.get("game_browser_active") and st.get("highlighted_label") == "Browse Games":
        k.press(BTN_SELECT)
    k.wait_status(lambda s: s.get("settings", {}).get("game_browser_active"),
                  5, "game browser to open")


def nav_cursor_to(k: Kiosk, field: str, target: int):
    """Move a game-browser cursor (game_browser_selected / selected_game_index)
    to `target` by pressing UP/DOWN and reading it back."""
    for _ in range(NAV_MAX_STEPS):
        cur = k.status().get("settings", {}).get(field, -1)
        if cur == target:
            return
        k.press(BTN_DOWN if cur < target else BTN_UP)
    raise RuntimeError(f"could not move {field} to {target}")


def open_game_playlist(k: Kiosk, playlist_idx: int) -> dict:
    enter_game_browser(k)
    nav_cursor_to(k, "game_browser_selected", playlist_idx)
    k.press(BTN_SELECT)
    st = k.wait_status(lambda s: s.get("settings", {}).get("viewing_games"),
                       5, f"game list {playlist_idx} to open")
    return st.get("settings", {})


# ---------------------------------------------------------------------------
# Launch / quit / verify
# ---------------------------------------------------------------------------
def sigterm_retroarch(sig="TERM"):
    subprocess.run(["pkill", f"-{sig}", "-x", "retroarch"],
                   capture_output=True)


def signal_retroarch_pid(pid: int, sig=signal.SIGTERM):
    try:
        os.kill(pid, sig)
    except ProcessLookupError:
        pass


def kiosk_alive_and_in_menu(st: dict) -> bool:
    # Predicate for wait_status: receives a status dict. Back at the menu
    # means the RetroArch mode has been torn down and we're on the playlist.
    return st.get("screen") == "playlist" and st.get("retroarch") is None


def recover(k: Kiosk):
    """Escalating recovery if a launch got stuck."""
    log("  recovery: SIGKILL retroarch", Colors.Y)
    sigterm_retroarch("KILL")
    try:
        k.wait_status(kiosk_alive_and_in_menu, 20, "kiosk to return after kill")
        return True
    except TimeoutError:
        log("  recovery: restarting kiosk service (last resort)", Colors.R)
        subprocess.run(["sudo", "systemctl", "restart", KIOSK_UNIT])
        time.sleep(20)
        return False


def test_one_game(k: Kiosk, playlist_idx: int, game_idx: int) -> dict:
    """Launch one game, verify launch, play, quit, verify recovery."""
    result = {"playlist_idx": playlist_idx, "game_idx": game_idx,
              "core": "?", "rom": "?", "launched": False, "launch_ms": None,
              "played_clean": None, "returned": False, "return_ms": None,
              "errors": []}
    cursor_ts = time.time() - 1
    log_cursor = launcher_log_cursor()

    # Navigate to the game and launch.
    sm = open_game_playlist(k, playlist_idx)
    result["playlist_name"] = sm.get("game_playlist_name", "?")
    ngames = sm.get("games_in_playlist", 0)
    if game_idx >= ngames:
        result["errors"].append(f"game_idx {game_idx} >= {ngames}")
        # leave the game list cleanly
        k.press(BTN_SETTINGS)
        return result
    nav_cursor_to(k, "selected_game_index", game_idx)

    t0 = time.time()
    k.press(BTN_SELECT)
    try:
        st = k.wait_status(
            lambda s: s.get("screen") == "retroarch" and s.get("retroarch"),
            5, "kiosk to enter RetroArch launch mode")
        retroarch_pid = wait_for_kms_takeover(
            k, t0, LAUNCH_TIMEOUT, log_cursor)
        result["launched"] = True
        result["launch_ms"] = int((time.time() - t0) * 1000)
        result["core"] = (st.get("retroarch") or {}).get("core", "?")
        result["rom"] = (st.get("retroarch") or {}).get("rom_name", "?")
        log(f"  KMS ready pid={retroarch_pid} core={result['core']} "
            f"rom='{result['rom']}' in {result['launch_ms']}ms", Colors.G)
    except (TimeoutError, RuntimeError) as e:
        result["errors"].append(f"launch failed: {e}")
        log(f"  LAUNCH FAILED: {e}", Colors.R)
        recover(k)
        return result

    # Play — let the core reach steady state so a Vulkan swapchain thrash
    # would surface in the launcher log.
    time.sleep(PLAY_SECONDS)
    launch_log = launcher_log_since(log_cursor)
    log_failure = launch_log_failure(launch_log)
    if not log_failure and result["core"].startswith("pcsx_rearmed"):
        log_failure = check_ps1_dynarec(launch_log)
    if log_failure:
        result["played_clean"] = False
        result["errors"].append(log_failure)
        log(f"  FATAL VIDEO ERROR: {log_failure}", Colors.R)
    elif not retroarch_pid_is_live(retroarch_pid):
        result["played_clean"] = False
        result["errors"].append("RetroArch died during the sustained-play check")
        log("  RetroArch died before the sustained-play check completed", Colors.R)
    else:
        result["played_clean"] = True

    # Quit via SIGTERM (== RetroArch-menu Quit from the kiosk's POV).
    t1 = time.time()
    signal_retroarch_pid(retroarch_pid)
    try:
        k.wait_status(kiosk_alive_and_in_menu, RETURN_TIMEOUT,
                      "kiosk to return to menu")
        result["returned"] = True
        result["return_ms"] = int((time.time() - t1) * 1000)
        log(f"  returned to menu in {result['return_ms']}ms", Colors.G)
    except TimeoutError as e:
        result["errors"].append(f"return timeout: {e}")
        log(f"  RETURN FAILED: {e}", Colors.R)
        recover(k)

    # Cross-check the journal for a clean recovery sequence.
    jtail = journal_since(cursor_ts)
    for marker in ["RetroArch exited with status", "Game launched successfully"]:
        if marker not in jtail:
            result["errors"].append(f"missing journal marker: '{marker}'")
    if "CRITICAL: Failed to acquire DRM master" in jtail:
        result["errors"].append("DRM master re-acquire hit CRITICAL retries")

    # Leave the settings/game menu so the next iteration starts clean.
    # Wait for the close to land in the status file — a blind settle is
    # not enough right after a game returns (quiet-mode container resume
    # loads the box), and a stale "settings.active" makes the next
    # enter_game_browser skip reopening and navigate a closed menu.
    st = k.status()
    if st.get("settings", {}).get("active"):
        k.press(BTN_SETTINGS)
        try:
            k.wait_status(lambda s: not s.get("settings", {}).get("active"),
                          5, "settings menu to close")
        except TimeoutError:
            # The close press itself may have been dropped under load —
            # retry once before giving up on this iteration's cleanup.
            k.press(BTN_SETTINGS)
            k.wait_status(lambda s: not s.get("settings", {}).get("active"),
                          5, "settings menu to close")
    return result


def test_restart_path(k: Kiosk) -> dict:
    """Launch a game, trigger the restart action (what the GPIO24 button
    does), and verify the kiosk cold-restarts, replays the intro, and
    recovers to the menu."""
    result = {"test": "restart_button", "launched": False,
              "intro_replayed": False, "recovered": False, "errors": []}
    log("RESTART-BUTTON PATH: launch a game, then trigger restart", Colors.B)
    # Launch playlist 0 / game 0.
    log_cursor = launcher_log_cursor()
    try:
        sm = open_game_playlist(k, 0)
        nav_cursor_to(k, "selected_game_index", 0)
        launch_started_at = time.time()
        k.press(BTN_SELECT)
        k.wait_status(lambda s: s.get("screen") == "retroarch" and s.get("retroarch"),
                      5, "kiosk to enter RetroArch launch mode")
        wait_for_kms_takeover(k, launch_started_at, LAUNCH_TIMEOUT, log_cursor)
        result["launched"] = True
        log("  game running; triggering restart (systemctl restart)", Colors.Y)
    except (TimeoutError, RuntimeError) as e:
        result["errors"].append(f"pre-restart launch failed: {e}")
        recover(k)
        return result

    cursor_ts = time.time() - 1
    # This is exactly what gpio_manager's restart-button handler runs.
    subprocess.run(["sudo", "systemctl", "restart", KIOSK_UNIT])

    # Wait for the service to come back and replay the intro.
    time.sleep(8)
    deadline = time.time() + 60
    while time.time() < deadline:
        j = journal_since(cursor_ts)
        if ("Intro video is now ready" in j) or ("Intro video loaded" in j):
            result["intro_replayed"] = True
            break
        time.sleep(2)
    if not result["intro_replayed"]:
        result["errors"].append("intro did not replay after restart")

    # Then confirm it settles into the menu.
    try:
        k.wait_status(kiosk_alive_and_in_menu, 40, "menu after restart")
        result["recovered"] = True
        log("  restart -> intro replayed -> menu recovered", Colors.G)
    except TimeoutError as e:
        result["errors"].append(f"did not reach menu after restart: {e}")
        log(f"  RESTART RECOVERY FAILED: {e}", Colors.R)
    return result


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def preflight(k: Kiosk):
    st = k.status()
    if not st:
        raise SystemExit("kiosk_status.json unreadable — is the kiosk running?")
    if "settings" not in st:
        raise SystemExit(
            "status file has no 'settings' block — deploy the kiosk build "
            "that exposes the game-browser cursor first.")
    # Injection round-trip: open + close settings.
    log("preflight: testing input injection + status readback...")
    k.press(BTN_SETTINGS)
    k.wait_status(lambda s: s.get("settings", {}).get("active"), 5,
                  "settings to open (injection check)")
    k.press(BTN_SETTINGS)
    k.wait_status(lambda s: not s.get("settings", {}).get("active"), 5,
                  "settings to close")
    log("preflight OK: injection + status + closed-loop nav working", Colors.G)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", type=int, default=2,
                    help="games per core (default 2)")
    ap.add_argument("--dry-run", action="store_true",
                    help="preflight + nav only, launch no games")
    ap.add_argument("--no-restart-test", action="store_true")
    args = ap.parse_args()

    k = Kiosk()
    preflight(k)
    if args.dry_run:
        log("dry-run complete", Colors.G)
        return 0

    # Discover how many game playlists exist.
    enter_game_browser(k)
    n_playlists = k.status().get("settings", {}).get("game_playlist_count", 0)
    k.press(BTN_SETTINGS)  # close
    time.sleep(PRESS_SETTLE)
    log(f"discovered {n_playlists} game playlists", Colors.B)

    results = []
    for pidx in range(n_playlists):
        log(f"=== game playlist {pidx} ===", Colors.B)
        for gidx in range(args.games):
            r = test_one_game(k, pidx, gidx)
            results.append(r)

    restart_result = None
    if not args.no_restart_test:
        restart_result = test_restart_path(k)

    # ---- Report ----
    print("\n" + "=" * 72)
    print("EMULATOR SMOKE TEST REPORT")
    print("=" * 72)
    by_core = {}
    for r in results:
        by_core.setdefault(r["core"], []).append(r)
    total_ok = 0
    for core, rs in sorted(by_core.items()):
        for r in rs:
            ok = r["launched"] and r["returned"] and not r["errors"]
            total_ok += 1 if ok else 0
            tag = f"{Colors.G}PASS{Colors.X}" if ok else f"{Colors.R}FAIL{Colors.X}"
            lm = f"{r['launch_ms']}ms" if r['launch_ms'] else "-"
            rm = f"{r['return_ms']}ms" if r['return_ms'] else "-"
            print(f"[{tag}] {core:26s} '{r['rom'][:28]:28s}' "
                  f"launch={lm:>7s} return={rm:>7s}")
            for e in r["errors"]:
                print(f"        {Colors.R}! {e}{Colors.X}")
    print("-" * 72)
    print(f"Games: {total_ok}/{len(results)} clean pass")
    if restart_result is not None:
        rr = restart_result
        ok = rr["launched"] and rr["intro_replayed"] and rr["recovered"] and not rr["errors"]
        tag = f"{Colors.G}PASS{Colors.X}" if ok else f"{Colors.R}FAIL{Colors.X}"
        print(f"Restart-button path: [{tag}] launched={rr['launched']} "
              f"intro_replayed={rr['intro_replayed']} recovered={rr['recovered']}")
        for e in rr["errors"]:
            print(f"        {Colors.R}! {e}{Colors.X}")
    print("=" * 72)

    all_ok = total_ok == len(results) and (
        restart_result is None or
        (restart_result["launched"] and restart_result["recovered"]
         and not restart_result["errors"]))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
