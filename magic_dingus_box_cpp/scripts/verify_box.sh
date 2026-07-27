#!/usr/bin/env bash
#
# verify_box.sh — pre-ship acceptance test for a Magic Dingus Box.
#
# Answers one question: "is this box shippable?"
#
# Runs ON the Pi, read-only, ~30s. Complements scripts/verify_services.sh
# (which hard-asserts the Radarr/Prowlarr/qBit/Gluetun stack); this one
# covers everything else — content integrity, display configuration,
# platform detection, hardware, and kiosk health. Pass --with-services to
# run that suite too for a full sweep.
#
# WHY THIS EXISTS: every one of these checks was, at some point, a bug
# found by hand. Content that silently didn't resolve, a display mode that
# regressed, a status field that was never populated, a smoke test whose
# expectations drifted from the fixtures. Codifying them means the next
# unit — and the next change — gets checked in one command instead of a
# session of ad-hoc greps.
#
# Exit 0 = shippable. Exit 1 = at least one FAIL. WARNs never fail the run
# but always print, because several are "expected on a bench box, not on a
# customer unit" (no Dreamcast BIOS, drive absent, etc).
set -uo pipefail

APP="${MAGIC_APP_DIR:-/opt/magic_dingus_box/magic_dingus_box_cpp}"
BASE="${MAGIC_BASE_DIR:-/opt/magic_dingus_box}"
DATA="${APP}/data"
UNIT="magic-dingus-box-cpp.service"

PASS=0; FAIL=0; WARN=0
RUN_SERVICES=0
[[ "${1:-}" == "--with-services" ]] && RUN_SERVICES=1

c_g=$'\033[32m'; c_r=$'\033[31m'; c_y=$'\033[33m'; c_b=$'\033[1m'; c_0=$'\033[0m'
[[ -t 1 ]] || { c_g=""; c_r=""; c_y=""; c_b=""; c_0=""; }

header() { printf "\n%s== %s ==%s\n" "$c_b" "$1" "$c_0"; }
pass()   { printf "  %s[PASS]%s %s\n" "$c_g" "$c_0" "$1"; PASS=$((PASS+1)); }
fail()   { printf "  %s[FAIL]%s %s\n" "$c_r" "$c_0" "$1"; FAIL=$((FAIL+1)); }
warn()   { printf "  %s[WARN]%s %s\n" "$c_y" "$c_0" "$1"; WARN=$((WARN+1)); }

# ---------------------------------------------------------------------
header "Platform"
# ---------------------------------------------------------------------
MODEL=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null)
case "$MODEL" in
  "Raspberry Pi 5"*) pass "board: $MODEL" ;;
  "Raspberry Pi 4"*) pass "board: $MODEL" ;;
  *)                 warn "board: ${MODEL:-unknown} (untested target)" ;;
esac

# The kiosk logs what it detected; a mismatch here means platform
# detection regressed and per-board behaviour (rotary ratio, audio sink,
# gpiochip) will be wrong.
PLAT=$(journalctl -u "$UNIT" -b --no-pager 2>/dev/null | grep -oE "Platform: Raspberry Pi [0-9].*" | tail -1)
if [[ -n "$PLAT" ]]; then pass "detection: $PLAT"; else warn "no platform log line this boot"; fi

ARM_MHZ=$(( $(vcgencmd measure_clock arm 2>/dev/null | cut -d= -f2) / 1000000 ))
TEMP=$(vcgencmd measure_temp 2>/dev/null | cut -d= -f2)
THROT=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)
if [[ "$THROT" == "0x0" ]]; then pass "clock ${ARM_MHZ}MHz, ${TEMP}, no throttling"
else fail "THROTTLED (${THROT}) at ${TEMP} — check cooling/PSU"; fi

# ---------------------------------------------------------------------
header "Display"
# ---------------------------------------------------------------------
WANT=$(python3 - <<'PY' 2>/dev/null
import json
try:
    print(json.load(open("/opt/magic_dingus_box/config/settings.json"))["display"]["mode"])
except Exception:
    print("crt_native")
PY
)
ACTUAL=$(journalctl -u "$UNIT" -b --no-pager 2>/dev/null | grep -oE "Final Display Mode: [0-9]+x[0-9]+" | tail -1 | awk '{print $4}')
SELECTED=$(journalctl -u "$UNIT" -b --no-pager 2>/dev/null | grep -oE "Selected mode [0-9]+x[0-9]+@[0-9]+Hz.*" | tail -1)

case "${WANT}:${ACTUAL}" in
  modern_tv:1920x1080) pass "mode: modern_tv -> ${ACTUAL}" ;;
  modern_tv:1280x720)  warn "mode: modern_tv but running ${ACTUAL} (1080p unavailable on this display?)" ;;
  crt_native:1280x720) pass "mode: crt_native -> ${ACTUAL}" ;;
  crt_native:1920x1080)
      fail "crt_native is running 1080p — CRT rigs go through an HDMI->composite converter; CRT_MAX_HEIGHT must clamp this" ;;
  *) warn "mode: ${WANT} -> ${ACTUAL:-unknown}" ;;
esac

# THE CANARY. The UI draws into a logical canvas that is deliberately
# decoupled from the framebuffer, so 1080p is a pure resolution change and
# every layout constant keeps its proportions. In Modern TV the 4:3 main
# menu content rect must be 960x720 in LOGICAL space at ANY output
# resolution. If this reads 1440x1080, the logical canvas broke and every
# menu element is rendering at 2/3 its intended size.
CANARY=$(journalctl -u "$UNIT" -b --no-pager 2>/dev/null | grep -oE "set_content_viewport\([0-9]+, [0-9]+\)" | tail -1)
if [[ "$WANT" == "modern_tv" ]]; then
    if [[ "$CANARY" == "set_content_viewport(960, 720)" ]]; then
        pass "logical canvas canary: ${CANARY}"
    elif [[ -z "$CANARY" ]]; then
        warn "logical canvas canary: not logged this boot (menu not yet drawn?)"
    else
        fail "logical canvas canary: ${CANARY} — expected (960, 720); menus are mis-scaled"
    fi
fi

# Refresh rate: the main loop blocks on the DRM page-flip, so a 24/30Hz
# timing clamps the WHOLE kiosk to that frame rate.
if [[ -n "$SELECTED" ]]; then
    HZ=$(sed -E 's/.*@([0-9]+)Hz.*/\1/' <<<"$SELECTED")
    if (( HZ >= 50 )); then pass "refresh: ${SELECTED}"
    else fail "refresh ${HZ}Hz — kiosk frame rate is clamped to this"; fi
fi

# ---------------------------------------------------------------------
header "Content"
# ---------------------------------------------------------------------
# Playlist paths resolve against the APP root or the data dir depending on
# the item type — resolving only one way produced a phantom "all ROMs
# missing" once, so try both.
while read -r kind a b; do
  case "$kind" in
    VID)  if [[ "$a" == "$b" && "$b" != 0 ]]; then pass "video playlists: ${a}/${b} files present"
          elif [[ "$b" == 0 ]]; then warn "no video playlist items found"
          else fail "video playlists: ${a}/${b} present"; fi ;;
    GAME) if [[ "$a" == "$b" && "$b" != 0 ]]; then pass "game playlists: ${a}/${b} ROMs present"
          elif [[ "$b" == 0 ]]; then warn "no game playlist items found"
          else fail "game playlists: ${a}/${b} present"; fi ;;
    BAD)  fail "unresolved: ${a} ${b}" ;;
  esac
done < <(python3 - "$APP" <<'PY'
import glob, os, re, sys
app = sys.argv[1]; data = os.path.join(app, "data")
def resolve(p):
    return (os.path.exists(p) if p.startswith("/")
            else os.path.exists(os.path.join(app, p)) or os.path.exists(os.path.join(data, p)))
vid_ok = vid_tot = game_ok = game_tot = 0; bad = []
for f in sorted(glob.glob(os.path.join(data, "playlists", "*.yaml"))):
    txt = open(f, encoding="utf-8", errors="replace").read()
    paths = re.findall(r"^\s*path:\s*['\"]?(.+?)['\"]?\s*$", txt, re.M)
    ok = sum(1 for p in paths if resolve(p)); miss = len(paths) - ok
    if "source_type: emulated_game" in txt: game_ok += ok; game_tot += len(paths)
    else: vid_ok += ok; vid_tot += len(paths)
    if miss: bad.append("BAD %s %dmissing" % (os.path.basename(f), miss))
print("VID %d %d" % (vid_ok, vid_tot)); print("GAME %d %d" % (game_ok, game_tot))
for b in bad: print(b)
PY
)

# Cores must be loadable, not merely present — a wrong-arch or
# missing-dependency .so passes a file test and fails at launch.
CORE_BAD=$(python3 - "$APP" <<'PY'
import ctypes, glob, os, sys
bad = []
for so in sorted(glob.glob(os.path.join(sys.argv[1], "libretro_cores", "*.so"))):
    try:
        lib = ctypes.CDLL(so)
        lib.retro_api_version.restype = ctypes.c_uint
        if lib.retro_api_version() != 1:
            bad.append(os.path.basename(so) + ":api")
    except Exception:
        bad.append(os.path.basename(so) + ":load")
print(",".join(bad))
PY
)
CORE_N=$(ls "$APP"/libretro_cores/*.so 2>/dev/null | wc -l | tr -d ' ')
if [[ -z "$CORE_BAD" && "$CORE_N" -gt 0 ]]; then pass "libretro cores: ${CORE_N} present, all load (API v1)"
elif [[ "$CORE_N" == 0 ]]; then fail "no libretro cores installed"
else fail "cores failing to load: ${CORE_BAD}"; fi

[[ -f "$HOME/.config/retroarch/system/scph5501.bin" ]] \
  && pass "PS1 BIOS present" || fail "PS1 BIOS (scph5501.bin) missing — PS1 games will not boot"
if ls "$APP"/data/roms/dreamcast/* >/dev/null 2>&1; then
  [[ -f "$HOME/.config/retroarch/system/dc_boot.bin" ]] \
    && pass "Dreamcast BIOS present" || fail "Dreamcast ROMs present but dc_boot.bin missing"
fi

# ---------------------------------------------------------------------
header "Kiosk"
# ---------------------------------------------------------------------
systemctl is-active --quiet "$UNIT" && pass "service active" || fail "service NOT active"
systemctl is-enabled --quiet "$UNIT" && pass "service enabled at boot" || fail "service not enabled"

FAILED_UNITS=$(systemctl --failed --no-legend | wc -l | tr -d ' ')
[[ "$FAILED_UNITS" == 0 ]] && pass "no failed systemd units" \
  || { fail "${FAILED_UNITS} failed unit(s)"; systemctl --failed --no-legend | sed 's/^/         /'; }

# Status file must be FRESH: it survives a restart, so a stale read looks
# healthy when the kiosk is actually down.
python3 - <<'PY' && pass "status file fresh + on a real screen" || fail "kiosk status stale or missing"
import json, sys, time
try:
    s = json.load(open("/opt/magic_dingus_box/magic_dingus_box_cpp/data/kiosk_status.json"))
    sys.exit(0 if time.time() - s.get("ts", 0) < 5 and s.get("screen") else 1)
except Exception:
    sys.exit(1)
PY

# now_playing was published-but-never-assigned for a long time; the phone
# remote showed a bare em-dash. Only meaningful while something plays.
NP=$(python3 -c "
import json
s=json.load(open('/opt/magic_dingus_box/magic_dingus_box_cpp/data/kiosk_status.json'))
print(1 if s.get('playback',{}).get('duration_sec',0)>0 else 0, s.get('now_playing',{}).get('title',''))" 2>/dev/null)
if [[ "${NP%% *}" == "1" ]]; then
  [[ -n "${NP#* }" ]] && pass "now_playing populated: ${NP#* }" \
                      || fail "media playing but now_playing.title empty (phone remote shows '-')"
fi

ERRS=$(journalctl -u "$UNIT" -b --no-pager -p err 2>/dev/null \
        | grep -viE "alsa|pulseaudio|module-stream-restore" | wc -l | tr -d ' ')
[[ "$ERRS" == 0 ]] && pass "no unexpected errors this boot" \
  || warn "${ERRS} non-ALSA error line(s) — review: journalctl -u ${UNIT} -b -p err"

# ---------------------------------------------------------------------
header "Storage & Media Browser"
# ---------------------------------------------------------------------
if grep -q " /mnt/ssd " /proc/mounts; then
  MOV=$(ls /mnt/ssd/library 2>/dev/null | wc -l | tr -d ' ')
  pass "movie drive mounted (${MOV} titles)"
else
  warn "movie drive not mounted — Movies shows 'drive not connected' (expected if unplugged)"
fi
ROOT_FREE=$(df -h / | tail -1 | awk '{print $4}')
ROOT_PCT=$(df / | tail -1 | awk '{print $5}' | tr -d '%')
(( ROOT_PCT < 85 )) && pass "SD card ${ROOT_PCT}% used (${ROOT_FREE} free)" \
                    || fail "SD card ${ROOT_PCT}% full — ${ROOT_FREE} free"

if [[ -f "${BASE}/services/.env" ]]; then
  if curl -fsS --max-time 8 http://localhost:7878/ping >/dev/null 2>&1; then
    pass "Radarr reachable (Media Browser functional)"
  else
    fail "services/.env present but Radarr unreachable — stack down?"
  fi
  UP=$(sudo docker ps -q 2>/dev/null | wc -l | tr -d ' ')
  (( UP >= 5 )) && pass "${UP} containers up" || fail "only ${UP} containers up (expect 5)"
else
  warn "services/.env absent — Media Browser unprovisioned (expected on a fresh box)"
fi

# ---------------------------------------------------------------------
if (( RUN_SERVICES )); then
  header "Service stack (verify_services.sh)"
  if bash "${APP}/scripts/verify_services.sh" >/tmp/vs.log 2>&1; then
    pass "verify_services.sh passed"
  else
    fail "verify_services.sh failed — see /tmp/vs.log"
    tail -12 /tmp/vs.log | sed 's/^/         /'
  fi
fi

# ---------------------------------------------------------------------
printf "\n%s== RESULT ==%s\n" "$c_b" "$c_0"
printf "  %s%d passed%s, %s%d failed%s, %s%d warnings%s\n" \
  "$c_g" "$PASS" "$c_0" "$c_r" "$FAIL" "$c_0" "$c_y" "$WARN" "$c_0"
if (( FAIL == 0 )); then
  printf "  %sSHIPPABLE%s\n" "$c_g" "$c_0"; exit 0
else
  printf "  %sNOT SHIPPABLE%s — fix the FAILs above\n" "$c_r" "$c_0"; exit 1
fi
