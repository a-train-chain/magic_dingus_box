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

# Running under sudo resolves $HOME to /root, which silently breaks every
# per-user path below (libretro cores dir, PS1/DC BIOS) and reports a false
# "NOT SHIPPABLE" on a healthy box — observed live 2026-07-31. The kiosk
# owns those paths as the login user, so re-derive HOME from SUDO_USER
# rather than refusing to run (running via sudo is otherwise harmless).
if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
    HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
fi

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

# TEST-ONLY MDB_PLATFORM_POLICY_OVERRIDE (CLAUDE.md "Dual-board contract"):
# a Pi 5 rehearsing Pi 4B software policy must NEVER be cloned or shipped,
# so ANY non-empty value is a FAIL — even one the binaries ignore. Four
# independent sources, because an override can arrive by any of them: the
# kiosk and web units' Environment= (unit file + every drop-in, which is
# what `systemctl show` merges), the kiosk's EnvironmentFile (services/.env,
# which `systemctl show -p Environment` does NOT include), the RUNNING
# processes' environment, and the kiosk's own report in kiosk_status.json.
#
# Pure parser, pinned by tests/local/verify_box_policy_override.bats:
# prints the override's value from `systemctl show -p Environment --value`
# output (space-separated, optionally quoted KEY=VAL) or env-file / environ
# text (one per line, optional `export`). Prints nothing when unset/empty.
policy_override_in_env() {
  tr ' \t' '\n\n' <<<"${1:-}" \
    | sed -nE "s/^[\"']?MDB_PLATFORM_POLICY_OVERRIDE=[\"']?([^\"']*)[\"']?\$/\\1/p" \
    | tail -1
}
OVR_HITS=()
for _u in "$UNIT" magic-dingus-web.service; do
  _v=$(policy_override_in_env "$(systemctl show -p Environment --value "$_u" 2>/dev/null)")
  [[ -n "$_v" ]] && OVR_HITS+=("${_u} Environment=${_v}")
  _pid=$(systemctl show -p MainPID --value "$_u" 2>/dev/null)
  if [[ -n "$_pid" && "$_pid" != 0 && -r "/proc/${_pid}/environ" ]]; then
    _v=$(policy_override_in_env "$(tr '\0' '\n' < "/proc/${_pid}/environ" 2>/dev/null)")
    [[ -n "$_v" ]] && OVR_HITS+=("${_u} running process=${_v}")
  fi
done
if [[ -r "${BASE}/services/.env" ]]; then
  _v=$(policy_override_in_env "$(cat "${BASE}/services/.env" 2>/dev/null)")
  [[ -n "$_v" ]] && OVR_HITS+=("services/.env=${_v}")
fi
_v=$(python3 -c "
import json
s=json.load(open('${DATA}/kiosk_status.json'))
print(s.get('platform_policy_override') or '')" 2>/dev/null)
[[ -n "$_v" ]] && OVR_HITS+=("kiosk_status.json platform_policy_override=${_v}")
if (( ${#OVR_HITS[@]} )); then
  fail "TEST-ONLY platform policy override is ON ($(IFS=';'; echo "${OVR_HITS[*]}")) — remove the drop-ins (CLAUDE.md 'Platform policy override'); this box must not be cloned or shipped"
else
  pass "no platform policy override (MDB_PLATFORM_POLICY_OVERRIDE unset)"
fi

ARM_MHZ=$(( $(vcgencmd measure_clock arm 2>/dev/null | cut -d= -f2) / 1000000 ))
TEMP=$(vcgencmd measure_temp 2>/dev/null | cut -d= -f2)
THROT=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)
if [[ "$THROT" == "0x0" ]]; then pass "clock ${ARM_MHZ}MHz, ${TEMP}, no throttling"
else fail "THROTTLED (${THROT}) at ${TEMP} — check cooling/PSU"; fi

# ---------------------------------------------------------------------
header "First boot & boot config"
# ---------------------------------------------------------------------
# A clone that did not finish first_boot.sh still carries SOURCE-BOX state
# (hostname, saves, pairing, credentials) while looking perfectly healthy —
# the 2026-08-04 parted failure killed first boot at Step 2 on every unit
# and nothing else in this script could see it.
#
# The log is APPENDED across runs and a clone inherits the source's copy, so
# only the LAST run counts: from the final "starting" line to the end.
FB_LOG=/var/log/magic-first-boot.log
FB_START="=== Magic Dingus Box first-boot setup starting ==="
FB_DONE="=== Magic Dingus Box first-boot setup complete ==="
if [[ -r "$FB_LOG" ]]; then
  FB_LAST=$(awk -v s="$FB_START" 'index($0, s) { buf = "" } { buf = buf $0 "\n" } END { printf "%s", buf }' "$FB_LOG")
  if grep -qF "=== FAILED" <<<"$FB_LAST"; then
    fail "first boot FAILED on its last run — $(grep -F '=== FAILED' <<<"$FB_LAST" | head -1 | sed 's/^[^ ]* //') (see ${FB_LOG})"
  elif grep -qF "$FB_DONE" <<<"$FB_LAST"; then
    pass "first boot completed (${FB_LOG})"
  elif grep -qF "This is the SOURCE card" <<<"$FB_LAST"; then
    # The source-card guard: this box is the clone SOURCE and correctly
    # refused to run first boot on itself.
    pass "first boot skipped by the source-card guard (this is the clone source)"
  else
    fail "first boot started but never logged completion — interrupted? (see ${FB_LOG})"
  fi
else
  # The source box (or a hand-provisioned one) may never have run first boot.
  # A clone that never ran it is caught by the enabled-unit check below.
  pass "no first-boot log (source / hand-provisioned box)"
fi

# The unit self-disables as its final step; still enabled means first boot
# has not completed, and it WILL run (wiping saves/pairing) on next boot.
FB_STATE=$(systemctl is-enabled magic-first-boot.service 2>/dev/null)
case "$FB_STATE" in
  enabled|enabled-runtime)
    fail "magic-first-boot.service is still ENABLED — first boot has not completed (or a clone left it on)" ;;
  *) pass "magic-first-boot.service not enabled (${FB_STATE:-not installed})" ;;
esac

# config.txt model-specific settings must live under [pi4]/[pi5], never
# [all] — one image boots both boards. Filters stack in config.txt, so only
# the MODEL filters ([piN]/[cmN]/[all]/[none]) change the context tracked
# here; e.g. [HDMI:0] after [pi5] is still Pi 5-only.
CFG=/boot/firmware/config.txt
[[ -f "$CFG" ]] || CFG=/boot/config.txt
if [[ -r "$CFG" ]]; then
  CFG_KV=$(awk '
    { sub(/#.*/, ""); gsub(/^[ \t]+|[ \t]+$/, "") }
    /^\[.*\]$/ {
      f = tolower(substr($0, 2, length($0) - 2))
      if (f ~ /^(all|none|pi[0-9a-z]*|cm[0-9a-z]*)$/) ctx = f
      next
    }
    /^(kernel|v3d_freq|gpu_mem)=/ { print (ctx == "" ? "all" : ctx) ":" $0 }
  ' "$CFG")
  grep -qx "pi5:kernel=kernel8.img" <<<"$CFG_KV" \
    && pass "[pi5] kernel=kernel8.img (4 KB pages — flycast)" \
    || fail "[pi5] kernel=kernel8.img missing from ${CFG} — flycast dies on the 16 KB-page kernel"
  grep -q "^pi5:v3d_freq=" <<<"$CFG_KV" \
    && pass "[pi5] $(grep '^pi5:v3d_freq=' <<<"$CFG_KV" | tail -1 | cut -d: -f2)" \
    || fail "[pi5] v3d_freq missing from ${CFG}"
  grep -qx "pi4:gpu_mem=76" <<<"$CFG_KV" \
    && pass "[pi4] gpu_mem=76" \
    || fail "[pi4] gpu_mem=76 missing from ${CFG}"
  CFG_ALL=$(grep -E "^all:(kernel|v3d_freq|gpu_mem)=" <<<"$CFG_KV" | cut -d: -f2- | tr '\n' ' ')
  [[ -z "$CFG_ALL" ]] \
    && pass "no model-specific kernel/v3d_freq/gpu_mem under [all]" \
    || fail "model-specific setting(s) under [all] in ${CFG}: ${CFG_ALL}— move them to [pi4]/[pi5]"
else
  warn "config.txt not found (not a Pi?)"
fi

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
header "Audio"
# ---------------------------------------------------------------------
# A box with a picture and no sound used to pass every check here — observed
# live 2026-10-03: a udev rule hid HDMI1 from PulseAudio, the TV was on
# HDMI1, the only sink was auto_null, and this script said SHIPPABLE.
# A TV that takes audio lists short audio descriptors in its ELD
# (sad_count > 0); each such port must have a PulseAudio sink.
pa() {
    if [[ $EUID -eq 0 ]]; then
        local u; u="$(id -u magic 2>/dev/null)" || return 1
        runuser -u magic -- env XDG_RUNTIME_DIR="/run/user/${u}" pactl "$@"
    else
        pactl "$@"
    fi
}
TV_PORTS=0
for eld in /proc/asound/vc4hdmi*/eld#0; do
    [[ -r "$eld" ]] || continue
    n="$(awk '$1=="sad_count"{print $2}' "$eld")"
    [[ "${n:-0}" -gt 0 ]] && TV_PORTS=$((TV_PORTS+1))
done
SINKS="$(pa list short sinks 2>/dev/null || true)"
HDMI_SINKS="$(grep -ci hdmi <<<"$SINKS" || true)"
DEF_SINK="$(pa get-default-sink 2>/dev/null || true)"
if [[ -e /etc/udev/rules.d/91-pulse-ignore-unused-hdmi.rules ]]; then
    fail "udev rule hides an HDMI port from PulseAudio (91-pulse-ignore-unused-hdmi.rules) — restart magic-dingus-audio; its prepare step removes it"
fi
# PulseAudio must live in its OWN unit. Inside the kiosk's cgroup (the
# pre-2026-10 layout, or init_audio.sh's legacy fallback) every kiosk
# restart SIGKILLs it.
AUDIO_UNIT=magic-dingus-audio.service
PA_PID="$(pgrep -u magic -x pulseaudio 2>/dev/null | head -1 || true)"
PA_CGROUP=""
[[ -n "$PA_PID" ]] && PA_CGROUP="$(cat "/proc/${PA_PID}/cgroup" 2>/dev/null || true)"
if [[ ! -f "/etc/systemd/system/${AUDIO_UNIT}" ]]; then
    warn "${AUDIO_UNIT} not installed — PulseAudio runs inside the kiosk (legacy); run setup_memory_tuning.sh or OTA"
elif ! systemctl is-active --quiet "$AUDIO_UNIT"; then
    fail "${AUDIO_UNIT} is $(systemctl is-active "$AUDIO_UNIT" 2>/dev/null) — journalctl -u ${AUDIO_UNIT}"
elif [[ "$PA_CGROUP" == *"magic-dingus-box-cpp.service"* ]]; then
    fail "PulseAudio (pid ${PA_PID}) is in the kiosk's cgroup, not ${AUDIO_UNIT} — restart the kiosk"
elif [[ -n "$PA_PID" && "$PA_CGROUP" != *"${AUDIO_UNIT}"* ]]; then
    warn "PulseAudio (pid ${PA_PID}) is outside ${AUDIO_UNIT}: ${PA_CGROUP##*:}"
else
    pass "PulseAudio runs in ${AUDIO_UNIT}"
fi
if [[ -z "$SINKS" ]]; then
    fail "PulseAudio not answering — no audio for videos, menus or games"
elif [[ -z "$DEF_SINK" || "$DEF_SINK" == "auto_null" ]]; then
    fail "default sink is '${DEF_SINK:-none}' — the box is SILENT (TV ports with audio: ${TV_PORTS})"
elif (( TV_PORTS > HDMI_SINKS )); then
    fail "${TV_PORTS} HDMI port(s) have a TV that takes audio but only ${HDMI_SINKS} HDMI sink(s) exist"
elif (( TV_PORTS == 0 )); then
    warn "no TV reporting audio on HDMI (TV off, or a CRT converter without audio EDID?) — default sink ${DEF_SINK}"
else
    pass "audio: default sink ${DEF_SINK}"
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
#
# Check the LIVE core directory (~/.config/retroarch/cores), which is what
# RetroArch actually dlopens at launch — NOT $APP/libretro_cores, which is
# only the staging copy deploy_cpp.sh rsyncs FROM the repo. Those two
# diverge: the staging dir is inside the deploy tree, so `rsync --delete`
# prunes it back to whatever the repo carries, while the live dir is
# updated additively and keeps everything. Checking staging reported
# "7 present" on a box that was happily running 10 — including the N64 and
# Dreamcast cores the smoke test had just launched games on.
CORE_DIR="$HOME/.config/retroarch/cores"
[[ -d "$CORE_DIR" ]] || CORE_DIR="$APP/libretro_cores"
CORE_BAD=$(python3 - "$CORE_DIR" <<'PY'
import ctypes, glob, os, sys
bad = []
for so in sorted(glob.glob(os.path.join(sys.argv[1], "*.so"))):
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
CORE_N=$(ls "$CORE_DIR"/*.so 2>/dev/null | wc -l | tr -d ' ')
if [[ -z "$CORE_BAD" && "$CORE_N" -gt 0 ]]; then pass "libretro cores: ${CORE_N} present, all load (API v1)"
elif [[ "$CORE_N" == 0 ]]; then fail "no libretro cores installed"
else fail "cores failing to load: ${CORE_BAD}"; fi

[[ -f "$HOME/.config/retroarch/system/scph5501.bin" ]] \
  && pass "PS1 BIOS present" || fail "PS1 BIOS (scph5501.bin) missing — PS1 games will not boot"
if ls "$APP"/data/roms/dreamcast/* >/dev/null 2>&1; then
  # flycast searches the system dir AND its dc/ subdirectory; only checking
  # the top level reported "missing" on a box that had the BIOS installed
  # the way flycast's own docs describe.
  SYS="$HOME/.config/retroarch/system"
  if [[ -f "$SYS/dc_boot.bin" || -f "$SYS/dc/dc_boot.bin" ]]; then
    pass "Dreamcast BIOS present"
  else
    # Downgraded from FAIL 2026-07-28. It is not a blocker: flycast falls
    # back to its own HLE BIOS (REIOS) and the shipped library boots on it,
    # verified by emulator_smoke_test. Nor is it something an image can fix
    # — dc_boot.bin is Sega firmware and cannot be redistributed, so every
    # unit that wants the accurate path has to supply its own dump.
    warn "no Dreamcast BIOS (dc_boot.bin) — flycast will use its HLE BIOS (REIOS)"
  fi

  # The 16 KB-page kernel kills flycast outright: it hardcodes a 4096-byte
  # page for Linux/aarch64, so its mprotect() calls fail with EINVAL and the
  # core aborts ~4s into every launch. This is the single check that
  # separates "Dreamcast works" from "Dreamcast is dead on this box", and it
  # is invisible from anywhere else — the core loads fine, the ROMs verify
  # fine, and the failure only shows up once a game is actually started.
  PGSZ=$(getconf PAGESIZE 2>/dev/null)
  if [[ "$PGSZ" == "4096" ]]; then
    pass "kernel page size ${PGSZ} (flycast-compatible)"
  else
    fail "kernel page size ${PGSZ} — flycast crashes on anything but 4096; set kernel=kernel8.img in config.txt"
  fi
fi

# ---------------------------------------------------------------------
header "Kiosk"
# ---------------------------------------------------------------------
systemctl is-active --quiet "$UNIT" && pass "service active" || fail "service NOT active"
systemctl is-enabled --quiet "$UNIT" && pass "service enabled at boot" || fail "service not enabled"

FAILED_UNITS=$(systemctl --failed --no-legend | wc -l | tr -d ' ')
[[ "$FAILED_UNITS" == 0 ]] && pass "no failed systemd units" \
  || { fail "${FAILED_UNITS} failed unit(s)"; systemctl --failed --no-legend | sed 's/^/         /'; }

# Playback memory posture (2026-08-11 stutter fix, box-side half).
# Without the cgroup memory controller + kiosk memory.low, service
# memory pressure swaps the video pipeline mid-movie — the freeze-then-
# silent-fast-forward bug ships again with nothing else looking wrong.
# Every piece is installed by setup_memory_tuning.sh; the controller
# additionally needs the reboot after its cmdline append.
if grep -qw memory /sys/fs/cgroup/cgroup.controllers 2>/dev/null; then
  pass "cgroup memory controller enabled"
  KIOSK_MEMLOW=$(cat "/sys/fs/cgroup/system.slice/${UNIT}/memory.low" 2>/dev/null || echo 0)
  [[ "$KIOSK_MEMLOW" == "536870912" ]] && pass "kiosk memory.low protection active (512M)" \
    || fail "kiosk memory.low is '${KIOSK_MEMLOW}', want 536870912 (run setup_memory_tuning.sh; needs slice companion too)"
else
  fail "cgroup memory controller DISABLED (cmdline missing cgroup_enable=memory — run setup_memory_tuning.sh, then reboot)"
fi
# OOM bias (same drop-in): when memory runs out the kernel must kill a
# self-restarting container, not the kiosk. Reads the unit's configured
# value, so it passes as soon as setup_memory_tuning.sh + daemon-reload ran
# (the running process picks it up at its next restart).
KIOSK_OOM=$(systemctl show -p OOMScoreAdjust --value "$UNIT" 2>/dev/null)
[[ "$KIOSK_OOM" == "-500" ]] && pass "kiosk OOMScoreAdjust=-500 (containers die first under OOM)" \
  || fail "kiosk OOMScoreAdjust is '${KIOSK_OOM:-unset}', want -500 (run setup_memory_tuning.sh)"
[[ "$(cat /proc/sys/vm/page-cluster 2>/dev/null)" == "0" ]] \
  && pass "zram page-cluster tuned (0)" \
  || warn "vm.page-cluster != 0 (zram swap-ins decompress 8x more than needed)"

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
# remote showed a bare em-dash. Only meaningful while a PLAYLIST ITEM plays.
#
# item_index >= 0 is load-bearing, not belt-and-braces. The boot intro video
# is played straight from main.cpp, not out of a playlist, so it never sets
# now_playing_title (controller.cpp assigns it only when a playlist item
# starts). Gating on duration alone therefore fired during the intro and
# reported NOT SHIPPABLE for the first several seconds after every boot or
# kiosk restart — seen twice on the Pi 5 before it was understood.
NP=$(python3 -c "
import json
s=json.load(open('/opt/magic_dingus_box/magic_dingus_box_cpp/data/kiosk_status.json'))
playing = s.get('playback',{}).get('duration_sec',0)>0 and s.get('playlist',{}).get('item_index',-1)>=0
print(1 if playing else 0, s.get('now_playing',{}).get('title',''))" 2>/dev/null)
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
  (( UP >= 6 )) && pass "${UP} containers up" || fail "only ${UP} containers up (expect 6)"
else
  warn "services/.env absent — Media Browser unprovisioned (expected on a fresh box)"
fi

# Season-delete's AutoRedownloadGuard writes this marker to tmpfs
# immediately before disabling Sonarr's autoRedownloadFailed, and removes it
# on a confirmed restore. Its presence means a delete was interrupted before
# restore could confirm the flag went back — the disabling PUT may have
# landed on Sonarr while the client-side call timed out or died, or the
# process was SIGKILLed/lost power inside the held window. Neither
# verify_services.sh nor anything else checks this: both only touch
# /api/v3/downloadclient (the download CLIENT list), never
# /api/v3/config/downloadclient (this flag). Nothing on the box self-heals
# it — the next delete GETs the flag, sees it already false, and assumes the
# owner set it that way on purpose. Reboots clear the marker (tmpfs), which
# is intentional: after a reboot there is no way to tell an interrupted run
# from a deliberate owner setting either, so this check can only catch it
# within the same boot.
if [[ -f /tmp/mdb_sonarr_autoredownload_held ]]; then
  fail "found /tmp/mdb_sonarr_autoredownload_held — a season delete was interrupted before Sonarr's autoRedownloadFailed was confirmed restored; check Settings > Download Clients in Sonarr and turn autoRedownloadFailed back ON if it is off, then remove the marker file"
else
  pass "no interrupted auto-redownload-guard marker"
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
