#!/usr/bin/env bash
# Several functions are reached only from traps (EXIT/INT/TERM/HUP),
# which static analysis cannot follow.
# shellcheck disable=SC2329
#
# hw_validate.sh — one-command hardware validation before a release tag.
#
# Runs ON the box (Pi 4B or Pi 5) as user `magic` (passwordless sudo).
#
#   bash hw_validate.sh            read-only checks only (safe any time)
#   bash hw_validate.sh --yes      + games, stop-mid-game, audio-output
#                                    switching, screenshots (changes state;
#                                    everything is backed up and restored)
#   bash hw_validate.sh --dry-run [--yes]
#                                  print the plan and every state-changing
#                                  command instead of running it
#
# Output: grouped [PASS]/[FAIL]/[WARN]/[MANUAL] lines (verify_box.sh
# style), a summary, ~/hw_validate_<ts>.json, and an artifacts dir
# ~/hw_validate_<ts>/ (screenshots, verify_box + smoke-test logs).
# Exit 0 only when nothing FAILed; 2 = refused to run.
#
# Every check maps to a finding from the v1.10.0 release reviews; see
# docs/HW_VALIDATION.md for the table.
#
# SAFETY CONTRACT (do not weaken):
#   * Never deletes or modifies user content. Before ANY game launches,
#     data/saves, data/states, data/screenshots, ~/.config/retroarch/saves
#     and ~/.config/retroarch/states are rsync'd to ~/hw_validate_backup_<ts>
#     and restored afterwards with `rsync -a --delete`, then verified with
#     `rsync -anc --delete --itemize-changes` (must print nothing). The
#     backup is removed ONLY after a verified restore; otherwise it is kept
#     and its path printed.
#   * settings.json is copied byte-for-byte before any change and restored
#     with the kiosk stopped; md5 must match, then the kiosk is restarted.
#   * Restores run from a trap on EXIT INT TERM HUP.
#   * Refuses to run while a game runs or a movie plays.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="${SCRIPT_DIR}/hw_validate_lib.py"
APP="${MAGIC_APP_DIR:-/opt/magic_dingus_box/magic_dingus_box_cpp}"
BASE="${MAGIC_BASE_DIR:-/opt/magic_dingus_box}"
DATA="${MAGIC_DATA_DIR:-${APP}/data}"
SETTINGS="${MAGIC_SETTINGS_FILE:-${BASE}/config/settings.json}"
KIOSK_LOG="${HWV_KIOSK_LOG:-${BASE}/config/magic_dingus_box.log}"
VERIFY_BOX="${HWV_VERIFY_BOX:-${SCRIPT_DIR}/verify_box.sh}"
SMOKE="${HWV_SMOKE:-${SCRIPT_DIR}/emulator_smoke_test.py}"
UNIT="magic-dingus-box-cpp.service"
AUDIO_UNIT="magic-dingus-audio.service"
WEB_UNIT="magic-dingus-web.service"
MODEL_FILE="${HWV_MODEL_FILE:-/proc/device-tree/model}"
export MAGIC_DATA_DIR="$DATA"

TS="$(date +%Y%m%d_%H%M%S)"
REPORT_BASE="${HWV_REPORT_DIR:-$HOME}"
REPORT_JSON="${REPORT_BASE}/hw_validate_${TS}.json"
ART_DIR="${REPORT_BASE}/hw_validate_${TS}"
BACKUP_DIR="${HOME}/hw_validate_backup_${TS}"
RESULTS="${ART_DIR}/results.tsv"
export HWV_RESULTS="$RESULTS"

YES=0; DRY_RUN=0; WITH_SERVICES=1
DO_GAMES=1; DO_AUDIO=1; DO_SHOTS=1
LISTEN=8

usage() {
    sed -n '6,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    cat <<'EOF'
Options:
  --yes               allow state-changing stages (games, audio, screenshots)
  --dry-run           print plan + would-be commands; change nothing
  --skip-games        no smoke test / stop-mid-game test
  --skip-audio        no audio-output switching
  --skip-screenshots  no screenshots
  --no-services       run verify_box.sh without --with-services
  --listen-seconds N  how long each audio-routing game plays (default 8)
  -h, --help          this text
EOF
}

while (( $# )); do
    case "$1" in
        --yes) YES=1 ;;
        --dry-run) DRY_RUN=1 ;;
        --skip-games) DO_GAMES=0 ;;
        --skip-audio) DO_AUDIO=0 ;;
        --skip-screenshots) DO_SHOTS=0 ;;
        --no-services) WITH_SERVICES=0 ;;
        --listen-seconds)
            shift
            [[ "${1:-}" =~ ^[0-9]+$ ]] || { echo "--listen-seconds needs a number" >&2; exit 2; }
            LISTEN="$1" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---------------------------------------------------------------------
# Output + result recording
# ---------------------------------------------------------------------
c_g=$'\033[32m'; c_r=$'\033[31m'; c_y=$'\033[33m'; c_c=$'\033[36m'; c_b=$'\033[1m'; c_0=$'\033[0m'
[[ -t 1 ]] || { c_g=""; c_r=""; c_y=""; c_c=""; c_b=""; c_0=""; }

mkdir -p "$ART_DIR" || { echo "cannot create ${ART_DIR}" >&2; exit 2; }
: > "$RESULTS"

header() {
    export HWV_GROUP="$1"
    printf "\n%s== %s ==%s\n" "$c_b" "$1" "$c_0"
}
record() {  # STATUS MESSAGE
    local st="$1" msg="$2" col
    msg="${msg//$'\t'/ }"; msg="${msg//$'\n'/ }"
    case "$st" in PASS) col=$c_g ;; FAIL) col=$c_r ;; WARN) col=$c_y ;; *) col=$c_c ;; esac
    printf "  %s[%s]%s %s\n" "$col" "$st" "$c_0" "$msg"
    printf '%s\t%s\t%s\t\n' "$st" "${HWV_GROUP:-}" "$msg" >> "$RESULTS"
}
pass()   { record PASS "$1"; }
fail()   { record FAIL "$1"; }
warn()   { record WARN "$1"; }
manual() { record MANUAL "$1"; }
info()   { printf "         %s\n" "$1"; }

lib() { python3 "$LIB" "$@"; }

# State-changing commands go through mut(): in --dry-run they are printed,
# never executed.
mut() {
    if (( DRY_RUN )); then
        printf "    %sDRY-RUN would run:%s %s\n" "$c_c" "$c_0" "$*"
        return 0
    fi
    "$@"
}

# Long-running python stages run in the BACKGROUND and are waited for, so
# a signal reaches our trap immediately instead of after the child ends
# (bash defers traps while a foreground child runs).
CHILD_PID=""
kill_tree() {  # TERM a process and all its descendants (children first)
    local c
    for c in $(pgrep -P "$1" 2>/dev/null); do kill_tree "$c"; done
    kill -TERM "$1" 2>/dev/null
}
run_child() {
    "$@" &
    CHILD_PID=$!
    wait "$CHILD_PID"
    local rc=$?
    CHILD_PID=""
    return "$rc"
}

# journalctl may need privileges for the system journal.
jctl() {
    if sudo -n true 2>/dev/null; then sudo -n journalctl "$@" 2>/dev/null
    else journalctl "$@" 2>/dev/null; fi
}
# Journal of the kiosk's CURRENT run (falls back to this boot).
kiosk_journal() {
    local inv
    inv="$(systemctl show -p InvocationID --value "$UNIT" 2>/dev/null)"
    if [[ -n "$inv" ]]; then
        jctl -u "$UNIT" "_SYSTEMD_INVOCATION_ID=${inv}" --no-pager -o cat
    else
        jctl -u "$UNIT" -b --no-pager -o cat
    fi
}

pa() {
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" pactl "$@" 2>/dev/null
}

# ---------------------------------------------------------------------
# Board
# ---------------------------------------------------------------------
MODEL="$(tr -d '\0' < "$MODEL_FILE" 2>/dev/null || true)"
case "$MODEL" in
    "Raspberry Pi 5"*) BOARD=pi5; OUTPUTS=(auto hdmi) ;;
    "Raspberry Pi 4"*) BOARD=pi4; OUTPUTS=(headphone auto hdmi) ;;
    *)                 BOARD=unknown; OUTPUTS=() ;;
esac

# ---------------------------------------------------------------------
# Restore machinery (trap)
# ---------------------------------------------------------------------
BACKUP_SET=(
    "data_saves|${DATA}/saves"
    "data_states|${DATA}/states"
    "data_screenshots|${DATA}/screenshots"
    "ra_saves|${HOME}/.config/retroarch/saves"
    "ra_states|${HOME}/.config/retroarch/states"
)
BACKUP_TAKEN=0; SETTINGS_SAVED=0; SETTINGS_RESTORE_OK=1; GAMES_TOUCHED=0
KIOSK_WAS_ACTIVE=0; ORIG_SINK=""; ORIG_MD5=""
CLEANED=0; ABORTED=0; FINISHED=0; BACKUP_PLANNED=0

md5_of() { md5sum "$1" 2>/dev/null | awk '{print $1}'; }

backup_user_content() {
    header "Safety: backup"
    local need avail e name src out kib
    need=0
    for e in "${BACKUP_SET[@]}"; do
        src="${e#*|}"
        [[ -d "$src" ]] || continue
        kib="$(du -sk "$src" 2>/dev/null | awk '{print $1}')"
        need=$(( need + ${kib:-0} ))
    done
    avail="$(df -Pk "$HOME" | awk 'NR==2{print $4}')"
    if (( need * 11 / 10 + 20480 > ${avail:-0} )); then
        fail "not enough space in ${HOME} for the save/state backup (${need} KiB needed, ${avail} KiB free) — game stages skipped"
        return 1
    fi
    if (( DRY_RUN )); then
        BACKUP_PLANNED=1
        for e in "${BACKUP_SET[@]}"; do
            mut rsync -a "${e#*|}/" "${BACKUP_DIR}/${e%%|*}/"
        done
        mut cp -p "$SETTINGS" "${BACKUP_DIR}/settings.json.orig"
        return 0
    fi
    mkdir -p "$BACKUP_DIR" || { fail "cannot create ${BACKUP_DIR}"; return 1; }
    for e in "${BACKUP_SET[@]}"; do
        name="${e%%|*}"; src="${e#*|}"
        mkdir -p "${BACKUP_DIR}/${name}"
        if [[ -d "$src" ]]; then
            if ! rsync -a "${src}/" "${BACKUP_DIR}/${name}/"; then
                fail "backup of ${src} failed — game stages skipped (partial backup kept at ${BACKUP_DIR})"
                return 1
            fi
            out="$(rsync -anc --delete --itemize-changes "${src}/" "${BACKUP_DIR}/${name}/")"
            if [[ -n "$out" ]]; then
                fail "backup of ${src} does not match its source — game stages skipped"
                return 1
            fi
        else
            echo "$name" >> "${BACKUP_DIR}/ABSENT"
        fi
    done
    BACKUP_TAKEN=1
    pass "user saves/states/screenshots backed up to ${BACKUP_DIR} ($(( need / 1024 )) MiB, verified)"
    ORIG_MD5="$(md5_of "$SETTINGS")"
    if [[ -n "$ORIG_MD5" ]] && cp -p "$SETTINGS" "${BACKUP_DIR}/settings.json.orig" \
        && [[ "$(md5_of "${BACKUP_DIR}/settings.json.orig")" == "$ORIG_MD5" ]]; then
        SETTINGS_SAVED=1
        cp -p "$SETTINGS" "${ART_DIR}/settings.json.orig" 2>/dev/null || true
        pass "settings.json saved (md5 ${ORIG_MD5})"
    else
        fail "could not save ${SETTINGS} — audio stage skipped"
        DO_AUDIO=0
    fi
    return 0
}

kill_leftover_games() {
    pgrep -x retroarch >/dev/null 2>&1 || return 0
    info "stopping a leftover retroarch (TERM, then KILL)"
    pkill -TERM -x retroarch 2>/dev/null
    local _
    for _ in $(seq 1 15); do pgrep -x retroarch >/dev/null 2>&1 || return 0; sleep 1; done
    pkill -KILL -x retroarch 2>/dev/null
    for _ in $(seq 1 5); do pgrep -x retroarch >/dev/null 2>&1 || return 0; sleep 1; done
    return 1
}

kiosk_restart_wait() {
    local t
    t="$(date +%s)"
    mut sudo -n systemctl start "$UNIT" || return 1
    (( DRY_RUN )) && return 0
    lib wait-menu --after "$t" --timeout 120
}

restore_settings() {
    (( SETTINGS_SAVED )) || return 0
    local cur
    cur="$(md5_of "$SETTINGS")"
    if [[ "$cur" == "$ORIG_MD5" ]]; then
        pass "settings.json unchanged (md5 ${cur})"
        return 0
    fi
    info "restoring settings.json with the kiosk stopped"
    sudo -n systemctl stop "$UNIT"
    if cp -p "${BACKUP_DIR}/settings.json.orig" "$SETTINGS" \
        && [[ "$(md5_of "$SETTINGS")" == "$ORIG_MD5" ]]; then
        pass "settings.json restored byte-for-byte (md5 ${ORIG_MD5})"
    else
        fail "settings.json restore FAILED — original kept at ${BACKUP_DIR}/settings.json.orig"
        SETTINGS_RESTORE_OK=0
        return 1
    fi
    if kiosk_restart_wait; then pass "kiosk restarted on the original settings"
    else fail "kiosk did not reach the menu after the settings restore"; fi
    cur="$(md5_of "$SETTINGS")"
    [[ "$cur" == "$ORIG_MD5" ]] \
        || warn "kiosk rewrote settings.json on start (md5 ${cur}) — content should be equivalent; diff against ${ART_DIR}/settings.json.orig"
    return 0
}

restore_user_content() {
    (( BACKUP_TAKEN )) || return 0
    local ok=1 e name src out
    for e in "${BACKUP_SET[@]}"; do
        name="${e%%|*}"; src="${e#*|}"
        if grep -qx "$name" "${BACKUP_DIR}/ABSENT" 2>/dev/null && [[ ! -d "$src" ]]; then
            continue  # absent before, still absent
        fi
        mkdir -p "$src"
        if ! rsync -a --delete "${BACKUP_DIR}/${name}/" "${src}/"; then
            fail "restore of ${src} failed"; ok=0; continue
        fi
        out="$(rsync -anc --delete --itemize-changes "${BACKUP_DIR}/${name}/" "${src}/")"
        if [[ -n "$out" ]]; then
            fail "restore of ${src} left differences: $(head -5 <<<"$out" | tr '\n' ' ')"
            ok=0
        elif grep -qx "$name" "${BACKUP_DIR}/ABSENT" 2>/dev/null; then
            rmdir "$src" 2>/dev/null || true  # created by the run, now empty
        fi
    done
    if (( ok && SETTINGS_RESTORE_OK )); then
        pass "saves/states/screenshots restored; rsync -anc --delete reports zero differences"
        case "$BACKUP_DIR" in
            "${HOME}"/hw_validate_backup_*) rm -rf -- "$BACKUP_DIR" && info "backup removed after verified restore" ;;
        esac
    else
        fail "restore NOT verified — backup KEPT at ${BACKUP_DIR} (restore by hand: rsync -a --delete <backup>/<name>/ <dir>/)"
    fi
}

cleanup_and_report() {  # $1 = the exit status that fired the EXIT trap
    local exit_rc="${1:-0}"
    (( CLEANED )) && return
    CLEANED=1
    trap '' INT TERM HUP
    if [[ -n "$CHILD_PID" ]]; then
        kill_tree "$CHILD_PID"
        wait "$CHILD_PID" 2>/dev/null
    fi
    if (( BACKUP_PLANNED )); then
        header "Safety: restore (dry-run)"
        local e
        mut sudo -n systemctl stop "$UNIT"
        mut cp -p "${BACKUP_DIR}/settings.json.orig" "$SETTINGS"
        for e in "${BACKUP_SET[@]}"; do
            mut rsync -a --delete "${BACKUP_DIR}/${e%%|*}/" "${e#*|}/"
            mut rsync -anc --delete --itemize-changes "${BACKUP_DIR}/${e%%|*}/" "${e#*|}/"
        done
        mut sudo -n systemctl start "$UNIT"
        mut rm -rf -- "$BACKUP_DIR"
    fi
    if (( BACKUP_TAKEN || SETTINGS_SAVED )); then
        header "Safety: restore"
        if (( GAMES_TOUCHED )); then
            kill_leftover_games || fail "retroarch would not die — restoring anyway"
        fi
        restore_settings
        restore_user_content
        if (( KIOSK_WAS_ACTIVE )) && ! systemctl is-active --quiet "$UNIT" 2>/dev/null; then
            if kiosk_restart_wait; then pass "kiosk running again"
            else fail "kiosk is NOT running — sudo systemctl start ${UNIT}"; fi
        fi
        if [[ -n "$ORIG_SINK" ]]; then
            local sink; sink="$(pa get-default-sink)"
            if [[ "$sink" == "$ORIG_SINK" ]]; then pass "default sink back to ${ORIG_SINK}"
            else warn "default sink is '${sink}', was '${ORIG_SINK}' before the run"; fi
        fi
    fi
    if (( ABORTED )); then
        HWV_GROUP="run"; fail "run aborted by signal — partial results"
    elif (( ! FINISHED )); then
        # set -u / an unexpected error ended the run early: never let a
        # truncated run report PASS.
        HWV_GROUP="run"; fail "hw_validate.sh stopped unexpectedly (exit ${exit_rc}) — partial results"
    fi
    lib report --results "$RESULTS" --out "$REPORT_JSON" \
        --meta "board=${BOARD}" --meta "model=${MODEL:-unknown}" \
        --meta "hostname=$(hostname 2>/dev/null)" --meta "timestamp=${TS}" \
        --meta "mode=$( ((YES)) && echo full || echo read-only)$( ((DRY_RUN)) && echo ' (dry-run)')" \
        --meta "artifacts=${ART_DIR}" \
        --meta "kiosk_version=$(cat "${BASE}/VERSION" 2>/dev/null || echo unknown)"
    REPORT_RC=$?
}

on_signal() {
    ABORTED=1
    printf "\n%s!! caught SIG%s — stopping and restoring%s\n" "$c_r" "$1" "$c_0"
    exit 130
}
# shellcheck disable=SC2034  # read by the EXIT trap
REPORT_RC=1
trap 'on_signal INT' INT
trap 'on_signal TERM' TERM
trap 'on_signal HUP' HUP
finish() { FINISHED=1; exit "${1:-0}"; }
trap 'cleanup_and_report "$?"; exit $(( ABORTED ? 130 : REPORT_RC ))' EXIT

# ---------------------------------------------------------------------
# Plan + preflight
# ---------------------------------------------------------------------
printf "%sMagic Dingus Box hardware validation%s — %s (%s)\n" "$c_b" "$c_0" "${MODEL:-unknown board}" "$BOARD"
echo "Report: ${REPORT_JSON}"
echo "Artifacts: ${ART_DIR}/"
echo
echo "READ-ONLY checks (always):"
echo "  - verify_box.sh$( ((WITH_SERVICES)) && echo ' --with-services') (platform, config.txt, display, content, kiosk, storage)"
echo "  - audio: sinks, default sink, jack sink (Pi 4B), HDMI ELDs, udev rule, audio unit + cgroup, one pulseaudio"
echo "  - services: OTA recovery unit, gunicorn, kiosk OOMScoreAdjust, container oom_score_adj"
echo "  - rendering: redraw gate / UI batching / CRT field rate lines, redraw report, 15 s kiosk CPU sample"
echo "  - posters: [artwork] budget churn; video: H.264 decoder + frame-mapping errors; N64/DC gating"
echo
echo "STATE-CHANGING stages ($( ((YES)) && echo 'ENABLED by --yes' || echo 'need --yes')):"
(( DO_GAMES )) && echo "  - back up saves/states, run emulator_smoke_test.py --games 1, then 'systemctl stop' mid-PS1-game"
(( DO_AUDIO )) && echo "  - for output in ${OUTPUTS[*]:-(none on this board)}: edit settings.json audio.output, restart kiosk, launch a game for ${LISTEN}s, read the ALSA device; post-game 0 s input check; restore settings.json"
(( DO_SHOTS )) && echo "  - screenshots: main menu, Settings, pairing QR, Movies (if unlocked)"
echo "  - restore everything (rsync --delete + verify, settings md5), restart the kiosk"
(( DRY_RUN )) && echo && echo "DRY-RUN: no state-changing command will execute."

if (( EUID == 0 )) && (( ! DRY_RUN )); then
    echo "Refusing to run as root: run as the kiosk user (magic); sudo is used where needed." >&2
    trap - EXIT; rm -rf -- "$ART_DIR"; exit 2
fi

BUSY="$(lib busy 2>/dev/null)"; BUSY_RC=$?
if (( BUSY_RC == 3 )) || pgrep -x retroarch >/dev/null 2>&1; then
    echo >&2
    echo "Refusing to run: ${BUSY:-a retroarch process is running}. Quit the game / stop the movie first." >&2
    trap - EXIT; rm -rf -- "$ART_DIR"; exit 2
fi

# ---------------------------------------------------------------------
header "verify_box.sh"
# ---------------------------------------------------------------------
VB_ARGS=()
(( WITH_SERVICES )) && VB_ARGS=(--with-services)
if [[ -r "$VERIFY_BOX" ]]; then
    TO=(); command -v timeout >/dev/null 2>&1 && TO=(timeout 600)
    ${TO[@]+"${TO[@]}"} bash "$VERIFY_BOX" ${VB_ARGS[@]+"${VB_ARGS[@]}"} > "${ART_DIR}/verify_box.txt" 2>&1
    VB_RC=$?
    info "verify_box.sh exited ${VB_RC}; full output: ${ART_DIR}/verify_box.txt"
    lib verify-box "${ART_DIR}/verify_box.txt"
else
    fail "verify_box.sh not found at ${VERIFY_BOX}"
fi

# ---------------------------------------------------------------------
header "Audio"
# ---------------------------------------------------------------------
SINKS="$(pa list short sinks || true)"
DEF_SINK="$(pa get-default-sink || true)"
ORIG_SINK="$DEF_SINK"
if [[ -n "$SINKS" ]]; then
    pass "pactl sinks: $(awk -F'\t' '{printf "%s%s", (NR>1?", ":""), $2}' <<<"$SINKS")"
else
    fail "pactl list short sinks returned nothing — PulseAudio not answering"
fi
if [[ -n "$DEF_SINK" && "$DEF_SINK" != "auto_null" ]]; then pass "default sink: ${DEF_SINK}"
else fail "default sink is '${DEF_SINK:-none}' — the box is silent"; fi

# Pi 4B 3.5 mm jack: the Headphones ALSA card must surface as a PulseAudio
# sink that resolve_audio_sink.sh's pattern (mailbox|analog, never hdmi)
# can find — otherwise "Headphones" output silently resolves to HDMI.
if [[ "$BOARD" == "pi4" ]]; then
    if grep -qi 'headphones' /proc/asound/cards 2>/dev/null; then
        JACK="$(awk -F'\t' 'tolower($2) !~ /hdmi/ && tolower($2) ~ /mailbox|analog/ {print $2; exit}' <<<"$SINKS")"
        if [[ -n "$JACK" ]]; then pass "jack sink: ${JACK}"
        else fail "Headphones ALSA card exists but no PulseAudio sink matches mailbox|analog — headphone output will fall back to HDMI"; fi
    else
        warn "no Headphones ALSA card (dtparam=audio=off?) — 3.5 mm output untestable"
    fi
else
    pass "3.5 mm jack check n/a on ${BOARD}"
fi

lib eld

if [[ -e /etc/udev/rules.d/91-pulse-ignore-unused-hdmi.rules ]]; then
    fail "udev rule 91-pulse-ignore-unused-hdmi.rules present — hides an HDMI port from PulseAudio"
else
    pass "no PulseAudio-ignore udev rule"
fi

if systemctl is-active --quiet "$AUDIO_UNIT" 2>/dev/null; then
    pass "${AUDIO_UNIT} active"
else
    fail "${AUDIO_UNIT} is $(systemctl is-active "$AUDIO_UNIT" 2>/dev/null || echo unknown)"
fi
PA_PIDS="$(pgrep -x pulseaudio 2>/dev/null | tr '\n' ' ')"
PA_N="$(wc -w <<<"$PA_PIDS" | tr -d ' ')"
case "$PA_N" in
    1) pass "exactly one pulseaudio (pid ${PA_PIDS% })" ;;
    0) fail "no pulseaudio process" ;;
    *) fail "${PA_N} pulseaudio processes (${PA_PIDS% }) — two daemons fight over the sinks" ;;
esac
for pid in $PA_PIDS; do
    CG="$(cat "/proc/${pid}/cgroup" 2>/dev/null)"
    if [[ "$CG" == *"$AUDIO_UNIT"* ]]; then pass "pulseaudio ${pid} lives in ${AUDIO_UNIT}"
    else fail "pulseaudio ${pid} is outside ${AUDIO_UNIT}: ${CG##*:} — a kiosk restart will kill it"; fi
done

# ---------------------------------------------------------------------
header "Services"
# ---------------------------------------------------------------------
OTA_EN="$(systemctl is-enabled magic-dingus-ota-recovery.service 2>/dev/null)"
if [[ "$OTA_EN" == "enabled" ]]; then pass "magic-dingus-ota-recovery.service enabled"
else fail "magic-dingus-ota-recovery.service is '${OTA_EN:-not installed}' — a power cut mid-OTA cannot self-recover"; fi
if python3 -c 'import gunicorn' 2>/dev/null; then pass "python3 can import gunicorn"
else fail "gunicorn not importable (python3-gunicorn missing)"; fi
WEB_LINE="$(jctl -u "$WEB_UNIT" -b --no-pager -o cat | grep -E 'serving with|gunicorn not installed|gunicorn unusable' | tail -1)"
if [[ "$WEB_LINE" == *"serving with gunicorn"* ]]; then pass "web: ${WEB_LINE}"
elif [[ -n "$WEB_LINE" ]]; then fail "web not on gunicorn: ${WEB_LINE}"
else warn "no web-server choice logged this boot by ${WEB_UNIT}"; fi
KOOM="$(systemctl show -p OOMScoreAdjust --value "$UNIT" 2>/dev/null)"
if [[ "$KOOM" == "-500" ]]; then pass "kiosk OOMScoreAdjust=-500"
else fail "kiosk OOMScoreAdjust='${KOOM:-unset}', want -500"; fi
if [[ -f "${BASE}/services/.env" ]]; then
    for pair in mdb_byparr:800 mdb_qbittorrent:300; do
        c="${pair%%:*}"; want="${pair#*:}"
        got="$(sudo -n docker inspect -f '{{.HostConfig.OomScoreAdj}}' "$c" 2>/dev/null)"
        if [[ "$got" == "$want" ]]; then pass "${c} oom_score_adj=${got}"
        elif [[ -z "$got" ]]; then warn "${c}: docker inspect failed (container absent?)"
        else warn "${c} oom_score_adj=${got}, want ${want} — container not yet recreated (docker compose up -d)"; fi
    done
else
    pass "container oom_score_adj n/a (Media Browser unprovisioned)"
fi

# ---------------------------------------------------------------------
header "Rendering"
# ---------------------------------------------------------------------
KJ="$(kiosk_journal)"
printf '%s\n' "$KJ" > "${ART_DIR}/kiosk_journal_current_run.txt"
for want in "Redraw gate: ON" "UI batching: ON"; do
    if grep -qF "$want" <<<"$KJ"; then pass "journal: ${want}"
    elif grep -qF "${want% *}: OFF" <<<"$KJ"; then fail "journal: ${want% *}: OFF (env override left on?)"
    else fail "journal lacks '${want}' for the current kiosk run (old binary?)"; fi
done
DMODE="$(lib settings-value "$SETTINGS" display.mode --default crt_native)"
if [[ "$DMODE" == "crt_native" ]]; then
    if grep -qF "CRT field rate active" <<<"$KJ"; then pass "journal: CRT field rate active"
    else warn "CRT mode but no 'CRT field rate active' line yet (static main menu not shown since start?)"; fi
else
    pass "CRT field rate n/a (display mode ${DMODE})"
fi
lib redraw "${ART_DIR}/kiosk_journal_current_run.txt" "$KIOSK_LOG"
KPID="$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)"
if [[ -n "$KPID" && "$KPID" != "0" ]]; then
    info "sampling kiosk CPU for 15 s (pid ${KPID})..."
    lib cpu "$KPID" --seconds 15
else
    warn "kiosk not running — CPU sample skipped"
fi

# ---------------------------------------------------------------------
header "Media Browser posters"
# ---------------------------------------------------------------------
lib artwork "${ART_DIR}/kiosk_journal_current_run.txt" "$KIOSK_LOG"

# ---------------------------------------------------------------------
header "Video decode"
# ---------------------------------------------------------------------
if [[ "$BOARD" == "pi4" ]]; then
    if gst-inspect-1.0 v4l2h264dec >/dev/null 2>&1; then pass "v4l2h264dec available"
    else fail "v4l2h264dec missing — Pi 4B would software-decode H.264"; fi
else
    pass "hardware H.264 check n/a on ${BOARD} (Pi 5 has no H.264 block)"
fi
KIOSK_LOGS=("$KIOSK_LOG")
for f in "${KIOSK_LOG%.log}".[0-9].log; do [[ -f "$f" ]] && KIOSK_LOGS+=("$f"); done
lib decoders --board "$BOARD" "${KIOSK_LOGS[@]}"
BOOTJ="$(jctl -u "$UNIT" -b --no-pager -o cat)"
BAD="$(grep -cE 'unusable plane layout|frame_map failed|skipping frame' <<<"$BOOTJ")"
if (( BAD == 0 )); then pass "no 'unusable plane layout' / 'frame_map failed' / 'skipping frame' this boot"
else fail "${BAD} frame-mapping error line(s) this boot: $(grep -E 'unusable plane layout|frame_map failed|skipping frame' <<<"$BOOTJ" | tail -1)"; fi

# ---------------------------------------------------------------------
header "Game system gating"
# ---------------------------------------------------------------------
lib gating --board "$BOARD"

# =====================================================================
# STATE-CHANGING STAGES
# =====================================================================
if (( ! YES )); then
    header "State-changing stages"
    info "skipped (read-only run). Re-run with --yes to launch games, switch audio outputs and take screenshots."
    finish 0  # EXIT trap writes the report
fi

header "Preflight (state-changing)"
PRE_OK=1
if sudo -n true 2>/dev/null; then pass "passwordless sudo"
elif (( DRY_RUN )); then warn "passwordless sudo unavailable (dry-run continues)"
else fail "passwordless sudo required for kiosk stop/start"; PRE_OK=0; fi
if systemctl is-active --quiet "$UNIT" 2>/dev/null; then
    KIOSK_WAS_ACTIVE=1; pass "kiosk active"
elif (( DRY_RUN )); then warn "kiosk not active (dry-run continues)"
else fail "kiosk not active — start it first"; PRE_OK=0; fi
if (( BUSY_RC == 0 )); then pass "kiosk idle: ${BUSY}"
elif (( DRY_RUN )); then warn "kiosk status not fresh/idle: ${BUSY:-unreadable} (dry-run continues)"
else fail "kiosk status not fresh/idle: ${BUSY:-unreadable}"; PRE_OK=0; fi
HARNESS="$(lib harness-ready 2>&1)"; HARNESS_RC=$?
if (( HARNESS_RC == 0 )); then pass "input-injection harness ready (paired remote found)"
elif (( DRY_RUN )); then warn "${HARNESS} (dry-run continues)"
else fail "${HARNESS} — pair a phone once (Settings > Connect a Device), then re-run"; PRE_OK=0; fi
if (( ! PRE_OK )); then
    fail "state-changing stages NOT run"
    finish 1
fi

if ! backup_user_content; then
    finish 1
fi

if (( DO_GAMES )); then
    header "Games: emulator smoke test (1 per core)"
    GAMES_TOUCHED=1
    if (( DRY_RUN )); then
        mut python3 "$SMOKE" --games 1
    else
        # shellcheck disable=SC2016  # expanded by the inner bash
        run_child bash -c 'python3 "$1" --games 1 2>&1 | tee "$2"; exit "${PIPESTATUS[0]}"' _ \
            "$SMOKE" "${ART_DIR}/emulator_smoke.txt"
        SMOKE_RC=$?
        lib smoke-report "${ART_DIR}/emulator_smoke.txt" --board "$BOARD" --rc "$SMOKE_RC"
    fi
    lib gating --board "$BOARD"

    header "Games: systemctl stop mid-game"
    if (( DRY_RUN )); then
        mut python3 "$LIB" stop-test   # launches PS1, sudo systemctl stop/start
    else
        run_child python3 "$LIB" stop-test
    fi
fi

if (( DO_AUDIO )); then
    header "Game audio routing"
    if (( ${#OUTPUTS[@]} == 0 )); then
        warn "unknown board — audio-output switching skipped"
    elif (( ! SETTINGS_SAVED )) && (( ! DRY_RUN )); then
        fail "settings.json not backed up — audio stage refused"
    else
        GAMES_TOUCHED=1
        for out in ${OUTPUTS[@]+"${OUTPUTS[@]}"}; do
            info "audio.output=${out}: stop kiosk, edit settings.json, start kiosk"
            mut sudo -n systemctl stop "$UNIT"
            mut python3 "$LIB" set-audio-output "$SETTINGS" "$out"
            if ! kiosk_restart_wait; then
                fail "audio.output=${out}: kiosk did not reach the menu"
                continue
            fi
            if (( DRY_RUN )); then
                mut python3 "$LIB" audio-probe --output "$out" --listen "$LISTEN"
            else
                run_child python3 "$LIB" audio-probe --output "$out" --listen "$LISTEN"
            fi
        done
        (( DRY_RUN )) || restore_settings
    fi
fi

if (( DO_SHOTS )); then
    header "Screenshots"
    MB_FLAG=()
    [[ "$(lib settings-value "$SETTINGS" playback.media_browser_unlocked --default false)" == "true" ]] && MB_FLAG=(--mb)
    if (( DRY_RUN )); then
        mut python3 "$LIB" screenshots --outdir "${ART_DIR}/screenshots" ${MB_FLAG[@]+"${MB_FLAG[@]}"}
    else
        run_child python3 "$LIB" screenshots --outdir "${ART_DIR}/screenshots" ${MB_FLAG[@]+"${MB_FLAG[@]}"}
        info "copy them off the box: scp -r ${USER:-magic}@$(hostname).local:${ART_DIR}/screenshots ."
    fi
fi

finish 0  # EXIT trap restores, verifies and writes the report
