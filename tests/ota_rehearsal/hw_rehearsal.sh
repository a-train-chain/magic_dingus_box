#!/usr/bin/env bash
# Hardware OTA rehearsal, run FROM THE MAC against ONE real Pi:
#
#   PI_HOST=magic@<ip> tests/ota_rehearsal/hw_rehearsal.sh [--old v1.9.14] [--new HEAD] [--yes]
#   PI_HOST=magic@<ip> tests/ota_rehearsal/hw_rehearsal.sh --dry-run          # print every command, touch nothing
#   PI_HOST=magic@<ip> tests/ota_rehearsal/hw_rehearsal.sh --restore-only <logdir> --yes
#
# The box is taken DOWN to the OLD release (real GitHub, read-only),
# stripped of the system state the NEW line introduced (so it looks like an
# OLD field box), then updated to NEW through the OLD web admin's own HTTP
# API against a fake GitHub ON THE BOX, verified, rolled back through the NEW
# web admin, verified again, and finally restored to THIS checkout with
# deploy_cpp.sh + pisim.sh push and checked against the pre-rehearsal
# snapshot. See tests/ota_rehearsal/README.md for the step list, the
# safety guarantees, and how to recover from an aborted run.
#
# Options:
#   --old REF        OLD release tag (default v1.9.14); must be a published release
#   --new REF        NEW ref to build the release artifacts from (default HEAD)
#   --yes            required for a real run (after the host/model printout)
#   --dry-run        no ssh, no rsync, no builds: print every remote/local command
#   --restore-only D teardown + restore + verify + cleanup, using log dir D
#                    from an aborted run (what the exit trap tells you to run)
#
# Env: PI_HOST (required, never defaulted), OTA_WORK (run.sh artifact cache),
#      OTA_HW_LOGROOT (default ~/mdb-ota-hw-rehearsal), OTA_GH_REPO,
#      OTA_HW_JOB_TIMEOUT (seconds per on-box update job, default 1800).
#
# shellcheck source-path=SCRIPTDIR
# Remote scripts are written in single quotes ON PURPOSE (they expand on the
# box, against REMOTE_PRELUDE's variables): SC2016 / SC2029 are the point.
# shellcheck disable=SC2016,SC2029,SC2015
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
# shellcheck source=hw_rehearsal_lib.sh
. "${HERE}/hw_rehearsal_lib.sh"
HELPERS="${HERE}/hw_helpers.py"

# ---------------------------------------------------------------------------
OLD_REF="v1.9.14"
NEW_REF="HEAD"
YES=0
DRY=0
RESTORE_ONLY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --old) OLD_REF="${2:?--old needs a ref}"; shift 2 ;;
        --new) NEW_REF="${2:?--new needs a ref}"; shift 2 ;;
        --yes) YES=1; shift ;;
        --dry-run) DRY=1; shift ;;
        --restore-only) RESTORE_ONLY="${2:?--restore-only needs the log dir of the aborted run}"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "hw_rehearsal: unknown argument: $1" >&2; exit 2 ;;
    esac
done
if [[ -z "${PI_HOST:-}" ]]; then
    echo "hw_rehearsal: set PI_HOST explicitly (e.g. PI_HOST=magic@192.168.1.50) — never defaulted, two boxes are often reachable" >&2
    exit 2
fi

# --- on-box layout ----------------------------------------------------------
# (several are only used inside remote scripts, via REMOTE_PRELUDE)
# shellcheck disable=SC2034
INSTALL=/opt/magic_dingus_box
DATA="${INSTALL}/magic_dingus_box_cpp/data"
BOX_HOME=/home/magic
MARKER="${BOX_HOME}/.magic_dingus_box_backup.ota_in_progress"
SNAP="${BOX_HOME}/mdb_ota_hw_snapshot"          # contains secrets: deleted at the end
FAKEGH="${BOX_HOME}/fakegh"                       # fake GitHub server + NEW artifacts
FGRUN=/run/fake-github                            # CA/leaf/control (tmpfs)
# shellcheck disable=SC2034
CA_DEST=/usr/local/share/ca-certificates/mdb-ota-hw-rehearsal.crt
GH_REPO="${OTA_GH_REPO:-a-train-chain/magic_dingus_box}"
# shellcheck disable=SC2034
WEB=http://127.0.0.1:5000
# shellcheck disable=SC2034
PKILL_PATTERN="$(hw_pkill_pattern "${FAKEGH}")"
# shellcheck disable=SC2034
HOSTS_LINE="$(hw_hosts_line)"
# shellcheck disable=SC2034
HOSTS_SED="$(hw_hosts_sed)"
MIN_FREE_KIB=$((3 * 1024 * 1024))
JOB_TIMEOUT="${OTA_HW_JOB_TIMEOUT:-1800}"
CONTENT_DIRS=("${DATA}/playlists" "${DATA}/media" "${DATA}/roms" "${DATA}/saves"
              "${DATA}/states" "${DATA}/thumbnails" /mnt/ssd)
# shellcheck disable=SC2034
CLIENT_CONF_MARKER="# magic-dingus-audio: written by audio_service.sh"
export OTA_WORK="${OTA_WORK:-${TMPDIR:-/tmp}/mdb-ota-rehearsal}"
OTA_WORK="${OTA_WORK%/}"

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=8)

# --- Mac-side log dir ---------------------------------------------------------
if [[ -n "$RESTORE_ONLY" ]]; then
    LOGDIR="$(cd "$RESTORE_ONLY" && pwd)"
    [[ -f "${LOGDIR}/state.env" ]] || { echo "hw_rehearsal: ${LOGDIR}/state.env missing — not a rehearsal log dir" >&2; exit 2; }
else
    LOGROOT="${OTA_HW_LOGROOT:-${HOME}/mdb-ota-hw-rehearsal}"
    LOGDIR="${LOGROOT}/$(date +%Y%m%d-%H%M%S)_$(hw_safe_name "$PI_HOST")$([[ $DRY == 1 ]] && echo _dryrun || true)"
fi
mkdir -p "${LOGDIR}/state"
chmod 700 "${LOGDIR}"
exec > >(tee -a "${LOGDIR}/run.log") 2>&1

FAILS=0
RESULTS="${LOGDIR}/results$([[ -n "$RESTORE_ONLY" ]] && echo _restore || true).txt"
: > "$RESULTS"
say()   { printf '\n\033[1;34m[hw-rehearsal]\033[0m %s\n' "$*"; }
step()  { printf '\n\033[1;35m=== %s ===\033[0m\n' "$*"; printf '=== %s\n' "$*" >> "$RESULTS"; }
vpass() { printf '  PASS  %s\n' "$*"; printf 'PASS  %s\n' "$*" >> "$RESULTS"; }
vfail() { FAILS=$((FAILS + 1)); printf '  \033[1;31mFAIL\033[0m  %s\n' "$*"; printf 'FAIL  %s\n' "$*" >> "$RESULTS"; }
vnote() { printf '  NOTE  %s\n' "$*"; printf 'NOTE  %s\n' "$*" >> "$RESULTS"; }
die()   { printf '\n\033[1;31m[hw-rehearsal] FATAL:\033[0m %s\n' "$*"; exit 1; }
dry()   { [[ $DRY == 1 ]]; }
veq()   { if [[ "$2" == "$3" ]]; then vpass "$1 ($2)"; else vfail "$1: got '$2', want '$3'"; fi; }

# --- progress state (read by the exit trap and by --restore-only) -----------
S_SNAPSHOT_ON_BOX=0   # SNAP exists on the box (secrets!)
S_BOX_MODIFIED=0      # the box's release has been touched (downgrade started)
S_FAKEGH=0            # hosts line / CA / server may be installed
S_FAKEGH_DOWN=0       # ... and has been torn down + verified
S_RESTORED=0          # deploy + push of this checkout done
S_DONE=0
OLD_VER="" NEW_VER="" OLD_SHA="" NEW_SHA="" CUR_VER=""
save_state() {
    hw_assign S_SNAPSHOT_ON_BOX S_BOX_MODIFIED S_FAKEGH S_FAKEGH_DOWN S_RESTORED S_DONE \
        OLD_REF NEW_REF OLD_VER NEW_VER OLD_SHA NEW_SHA CUR_VER PI_HOST > "${LOGDIR}/state.env"
}

# --- remote execution -------------------------------------------------------
REMOTE_PRELUDE="set -euo pipefail
$(hw_assign INSTALL DATA BOX_HOME MARKER SNAP FAKEGH FGRUN CA_DEST GH_REPO WEB PKILL_PATTERN HOSTS_LINE HOSTS_SED CLIENT_CONF_MARKER)
"
# rsh <desc> <script> — run <script> on the box. The script travels on ssh's
# STDIN into a temp file, then runs with stdin = /dev/null: the ssh command
# line is a fixed string (nothing on it can ever match a pkill pattern), and
# no subprocess (apt, an OLD-tree installer, tar) can swallow the rest of
# the script by reading stdin, as it would under plain `bash -s`.
RSH_RUNNER='f=$(mktemp /tmp/mdb-hw-rehearsal.XXXXXX) && cat > "$f" && { bash "$f" < /dev/null; rc=$?; rm -f "$f"; exit $rc; }'
rsh() {
    local desc="$1" body="$2"
    printf '\n### %s\n%s\n' "$desc" "$body" >> "${LOGDIR}/remote_commands.sh"
    if dry; then
        printf '  [dry-run] on %s — %s:\n' "$PI_HOST" "$desc" >&2
        printf '%s\n' "$body" | sed 's/^/      | /' >&2
        return 0
    fi
    ssh "${SSH_OPTS[@]}" "$PI_HOST" "$RSH_RUNNER" <<<"${REMOTE_PRELUDE}${body}"
}
# rcap <desc> <script> <dry-run placeholder> — rsh, capturing stdout
rcap() {
    if dry; then rsh "$1" "$2"; printf '%s' "$3"; return 0; fi
    rsh "$1" "$2"
}
# rpy [--sudo] <desc> <hw_helpers.py args...> — the helper, run ON THE BOX
rpy() {
    local pre=""
    [[ "$1" == --sudo ]] && { pre="sudo -n "; shift; }
    local desc="$1"; shift
    local cmd
    cmd="${pre}python3 - $(printf '%q ' "$@")"
    printf '\n### %s (hw_helpers.py on the box)\n%s\n' "$desc" "$cmd" >> "${LOGDIR}/remote_commands.sh"
    if dry; then printf '  [dry-run] on %s — %s: %s < hw_helpers.py\n' "$PI_HOST" "$desc" "$cmd" >&2; return 0; fi
    ssh "${SSH_OPTS[@]}" "$PI_HOST" "$cmd" < "$HELPERS"
}
# lrun [--log FILE] <desc> <cmd...> — a Mac-side command (printed in dry-run)
lrun() {
    local log=""
    [[ "$1" == --log ]] && { log="$2"; shift 2; }
    local desc="$1"; shift
    printf '\n### %s (Mac)\n%s%s\n' "$desc" "$(printf '%q ' "$@")" "${log:+> $log 2>&1}" >> "${LOGDIR}/remote_commands.sh"
    if dry; then printf '  [dry-run] Mac — %s: %s%s\n' "$desc" "$(printf '%q ' "$@")" "${log:+> $log}" >&2; return 0; fi
    if [[ -n "$log" ]]; then "$@" > "$log" 2>&1; else "$@"; fi
}
# fetch <remote path> <local path> — box -> Mac, as root. NOT rsync: macOS
# ships openrsync as /usr/bin/rsync, and as the RECEIVER of a pull it
# deadlocked mid-transfer (both ends asleep in poll for 23 min, 978 of 1416
# snapshot files copied, magicpi5 2026-10-06). A directory (trailing /)
# streams as a tar — GNU tar skips sockets on its own (a qBittorrent ipc
# socket in services/config broke plain rsync on 2026-10-04); a file is cat.
fetch() {
    if [[ "$1" == */ ]]; then
        lrun "fetch $1" fetch_dir "$1" "$2"
    else
        lrun "fetch $1" fetch_file "$1" "$2"
    fi
}
fetch_dir() {
    mkdir -p "$2"
    ssh "${SSH_OPTS[@]}" "$PI_HOST" \
        "sudo -n tar -C $(printf '%q' "$1") --warning=no-file-ignored -cf - ." < /dev/null \
        | tar -C "$2" -xf -
}
fetch_file() {
    mkdir -p "$(dirname "$2")"
    ssh "${SSH_OPTS[@]}" "$PI_HOST" "sudo -n cat $(printf '%q' "$1")" < /dev/null > "$2.part" \
        && mv -f "$2.part" "$2" || { rm -f "$2.part"; return 1; }
}
push() {  # push <remote dir/> <local paths...>
    local dest="$1"; shift
    lrun "push to $dest" rsync -a --no-specials --no-devices "$@" "${PI_HOST}:${dest}"
}
# json <file|-> <key...> — read a value with the helper
json() { python3 "$HELPERS" get "$@" || true; }

# box_state <label> — app-data manifest, content fingerprint, system listing,
# written on the box under SNAP and copied to ${LOGDIR}/state/
box_state() {
    local l="$1"
    rpy --sudo "app-data manifest ($l)" manifest "$INSTALL" "${SNAP}/appdata_${l}.json"
    rpy --sudo "content fingerprint ($l)" fingerprint "${SNAP}/content_${l}.json" "${CONTENT_DIRS[@]}"
    rpy --sudo "system-state listing ($l)" syslisting "${SNAP}/syslisting_${l}.json"
    local f
    for f in appdata content syslisting; do
        fetch "${SNAP}/${f}_${l}.json" "${LOGDIR}/state/${f}_${l}.json"
    done
}
# vcompare <what> <label-a> <label-b> [allow globs...]
vcompare() {
    local what="$1" a="$2" b="$3"; shift 3
    local fa="${LOGDIR}/state/${what}_${a}.json" fb="${LOGDIR}/state/${what}_${b}.json"
    local allow=() g out
    for g in "$@"; do allow+=(--allow "$g"); done
    if dry; then vnote "[dry-run] would compare ${what}: ${a} vs ${b}"; return 0; fi
    if out="$(python3 "$HELPERS" compare "$fa" "$fb" --label "${what} ${a}->${b}" ${allow[@]+"${allow[@]}"})"; then
        vpass "${what} identical (${a} -> ${b}): $(tail -1 <<<"$out")"
    else
        printf '%s\n' "$out" | sed 's/^/      /'
        vfail "${what} differs (${a} -> ${b}) — see above"
    fi
}

# remote_job <name> <command> — run <command> DETACHED on the box (setsid +
# nohup, as the ssh user, in INSTALL) so a dropped ssh link cannot kill an
# update mid-rsync; poll its rc file and stream its log.
remote_job() {
    local name="$1" cmd="$2" d="${SNAP}/jobs/$1"
    rsh "start job ${name}" "
mkdir -p $(printf '%q' "$d")
cd \"\$INSTALL\"
setsid nohup bash -c $(printf '%q' "${cmd} > ${d}/out 2>&1; echo \$? > ${d}/rc") </dev/null >/dev/null 2>&1 &
echo \"job ${name} started\""
    dry && return 0
    local t0=$SECONDS seen=0 n rc=""
    while (( SECONDS - t0 < JOB_TIMEOUT )); do
        sleep 5
        n="$(ssh "${SSH_OPTS[@]}" "$PI_HOST" "tail -n +$((seen + 1)) $(printf '%q' "$d/out") 2>/dev/null" || true)"
        if [[ -n "$n" ]]; then
            printf '%s\n' "$n" | sed "s/^/      [${name}] /"
            seen=$((seen + $(printf '%s\n' "$n" | wc -l)))
        fi
        rc="$(ssh "${SSH_OPTS[@]}" "$PI_HOST" "cat $(printf '%q' "$d/rc") 2>/dev/null" || true)"
        [[ -n "$rc" ]] && break
    done
    fetch "$d/out" "${LOGDIR}/job_${name}.log" || true
    [[ -n "$rc" ]] || die "job ${name} did not finish within ${JOB_TIMEOUT}s (still running on the box? log: $d/out)"
    return "$rc"
}

# web <method> <path> [json body] — the box's own web admin, from the box
# itself, exactly as the browser would reach it (Host: localhost passes the
# Host allowlist). CSRF token + cookie jar as manager.js keeps them.
web() {
    local method="$1" path="$2" body="${3:-}" maxt="${4:-15}"
    [[ -n "$body" ]] || body='{}'
    local script="jar=\"\$FAKEGH/cookies.txt\"
curl -sS --max-time ${maxt} -H 'Host: localhost' -b \"\$jar\" -c \"\$jar\""
    if [[ "$method" == POST ]]; then
        script="tok=\$(curl -fsS --max-time 10 -H 'Host: localhost' -b \"\$FAKEGH/cookies.txt\" -c \"\$FAKEGH/cookies.txt\" \"\$WEB/admin/csrf-token\" | python3 -c 'import json,sys; print(json.load(sys.stdin)[\"data\"][\"token\"])')
${script} -X POST -H 'Content-Type: application/json' -H \"X-CSRF-Token: \$tok\" -d $(printf '%q' "$body")"
    fi
    rsh "web ${method} ${path}" "${script} \"\$WEB\"$(printf '%q' "$path")"
}

# --- exit trap ----------------------------------------------------------------
teardown_fakegh_script() {
    cat <<'EOF'
# 1. stop the server (anchored pattern: never matches this ssh session)
sudo -n pkill -f "$PKILL_PATTERN" 2>/dev/null || true
for _ in $(seq 1 20); do pgrep -f "$PKILL_PATTERN" >/dev/null || break; sleep 0.5; done
if pgrep -af "$PKILL_PATTERN"; then echo "TEARDOWN-ERROR: fake GitHub still running"; fi
# 2. /etc/hosts back byte-for-byte (fallback: delete only the tagged line)
if [ -f "$FAKEGH/hosts.orig" ]; then
    sudo -n cp "$FAKEGH/hosts.orig" /etc/hosts
    if cmp -s "$FAKEGH/hosts.orig" /etc/hosts; then echo "hosts: restored byte-for-byte"; else echo "TEARDOWN-ERROR: /etc/hosts differs from hosts.orig"; fi
elif grep -qF "$HOSTS_LINE" /etc/hosts; then
    sudo -n sed -i "$HOSTS_SED" /etc/hosts
    echo "hosts: no hosts.orig — removed the tagged line only"
fi
if grep -q 'mdb-ota-hw-rehearsal' /etc/hosts; then echo "TEARDOWN-ERROR: tagged line still in /etc/hosts"; fi
# 3. the temporary CA out of the trust store
if [ -e "$CA_DEST" ]; then sudo -n rm -f "$CA_DEST"; fi
sudo -n update-ca-certificates --fresh >/dev/null 2>&1 || echo "TEARDOWN-ERROR: update-ca-certificates --fresh failed"
if [ -e "$CA_DEST" ]; then echo "TEARDOWN-ERROR: CA still at $CA_DEST"; fi
# 4. the CA key + leaf
sudo -n rm -rf "$FGRUN"
echo "teardown complete"
EOF
}
teardown_fakegh() {
    local out
    out="$(rcap "tear down fake GitHub (server, /etc/hosts, CA, $FGRUN)" "$(teardown_fakegh_script)" "teardown complete")" || true
    printf '%s\n' "$out" | sed 's/^/      /'
    if grep -q 'TEARDOWN-ERROR' <<<"$out" || ! grep -q 'teardown complete' <<<"$out"; then
        return 1
    fi
    S_FAKEGH_DOWN=1; save_state
}

print_remaining() {
    local q_me q_repo q_log q_helpers
    q_me="PI_HOST=$(printf '%q' "$PI_HOST") $(printf '%q' "${HERE}/hw_rehearsal.sh")"
    q_repo="$(printf '%q' "$REPO")"; q_log="$(printf '%q' "$LOGDIR")"; q_helpers="$(printf '%q' "$HELPERS")"
    if [[ $S_SNAPSHOT_ON_BOX != 1 && $S_BOX_MODIFIED != 1 && $S_FAKEGH != 1 ]]; then
        say "nothing on the box was changed (logs: ${LOGDIR})"
        return 0
    fi
    say "REMAINING STEPS — nothing below was done automatically"
    if [[ $S_FAKEGH == 1 && $S_FAKEGH_DOWN != 1 ]]; then
        echo "  * The fake GitHub could NOT be confirmed removed. Run on the box:"
        echo "      ssh ${PI_HOST} 'bash -s' <<'EOF'"
        printf '%s\n' "${REMOTE_PRELUDE}" "$(teardown_fakegh_script)" | sed 's/^/      /'
        echo "      EOF"
    fi
    if [[ $S_BOX_MODIFIED == 1 && $S_RESTORED != 1 ]]; then
        echo "  * The box is NOT on this checkout (it is on ${OLD_VER:-OLD} or ${NEW_VER:-NEW}, possibly mid-update)."
        echo "    First check for an interrupted install:  ssh ${PI_HOST} ls -la ${MARKER}"
        echo "      (present = an install/rollback was cut off; boot recovery or"
        echo "       'cd ${INSTALL} && magic_dingus_box_cpp/scripts/update.sh recover' handles it)"
        echo "    Then restore + verify + clean up in one command:"
        echo "      ${q_me} --restore-only ${q_log} --yes"
        echo "    which runs, in order (do these by hand if you prefer):"
        echo "      ${q_repo}/magic_dingus_box_cpp/dev/pisim/pisim.sh build"
        echo "      python3 ${q_helpers} deploy-guard ${q_repo}/magic_dingus_box_cpp/scripts/deploy_cpp.sh --var CPP_DIR=${q_repo}/magic_dingus_box_cpp --var PI_HOST=${PI_HOST} --var PI_DIR=${INSTALL}"
        echo "        (aborts if a deploy --delete would remove anything under data/ or config/)"
        echo "      PI_HOST=${PI_HOST} ${q_repo}/magic_dingus_box_cpp/scripts/deploy_cpp.sh"
        echo "      PI_HOST=${PI_HOST} ${q_repo}/magic_dingus_box_cpp/dev/pisim/pisim.sh push"
        echo "      put back the strip-touched files (${q_log}/strip_journal.txt) from ${SNAP}/system_state.tar"
    fi
    if [[ $S_SNAPSHOT_ON_BOX == 1 ]]; then
        echo "  * The on-box snapshot holds SECRETS (services/.env, VPN key). Once the box is restored:"
        echo "      ssh ${PI_HOST} 'sudo rm -rf ${SNAP} ${FAKEGH}'"
    fi
    echo "  * Mac-side copy of the snapshot + logs: ${LOGDIR}"
}

on_exit() {
    local rc=$?
    trap - EXIT INT TERM HUP
    set +e
    if [[ $S_FAKEGH == 1 && $S_FAKEGH_DOWN != 1 ]]; then
        say "exit trap: removing the fake GitHub (server, /etc/hosts line, CA) — always safe to undo"
        teardown_fakegh || say "exit trap: fake GitHub teardown NOT confirmed"
    fi
    save_state
    if [[ $S_DONE != 1 ]]; then
        [[ $rc == 0 ]] && rc=1
        print_remaining
    fi
    if [[ $S_DONE == 1 ]]; then
        say "results: ${RESULTS} — ${FAILS} verification failure(s)"
    fi
    exit "$rc"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# ===========================================================================
# 1. preflight
# ===========================================================================
resolve_versions() {
    OLD_SHA="$(git -C "$REPO" rev-parse --verify "${OLD_REF}^{commit}")" || die "unknown --old ref ${OLD_REF}"
    NEW_SHA="$(git -C "$REPO" rev-parse --verify "${NEW_REF}^{commit}")" || die "unknown --new ref ${NEW_REF}"
    OLD_VER="$(git -C "$REPO" show "${OLD_SHA}:VERSION" | tr -d '[:space:]')"
    NEW_VER="$(git -C "$REPO" show "${NEW_SHA}:VERSION" | tr -d '[:space:]')"
    CUR_VER="$(tr -d '[:space:]' < "${REPO}/VERSION")"
    [[ "$OLD_VER" != "$NEW_VER" ]] || die "OLD and NEW are both ${OLD_VER}"
    save_state
}

step_preflight() {
    step "1. preflight"
    resolve_versions
    say "OLD ${OLD_REF} = ${OLD_SHA:0:7} (${OLD_VER})   NEW ${NEW_REF} = ${NEW_SHA:0:7} (${NEW_VER})   checkout = ${CUR_VER}"
    FIELD_FILE="$(hw_field_state_file "$HERE" "$OLD_VER")"
    [[ -f "$FIELD_FILE" ]] || die "no field-state list for ${OLD_VER}: create ${FIELD_FILE} (see README.md 'Field-state strip')"
    python3 "$HELPERS" field-plan --file "$FIELD_FILE" > /dev/null || die "${FIELD_FILE} does not parse"
    command -v rsync >/dev/null && command -v ssh >/dev/null && command -v python3 >/dev/null || die "need rsync, ssh, python3 on the Mac"
    dry || { docker info >/dev/null 2>&1 || die "Docker is not running (release artifacts + pisim build need it)"; }
    [[ -z "$(git -C "$REPO" status --porcelain)" ]] || vnote "checkout has uncommitted changes — the final restore deploys the working tree AS IS"

    local info
    info="$(rcap "preflight readings" '
echo "hostname=$(hostname)"
echo "model=$(tr -d "\0" </proc/device-tree/model 2>/dev/null || echo unknown)"
echo "version=$(tr -d "[:space:]" < "$INSTALL/VERSION" 2>/dev/null)"
echo "free_kib=$(df -Pk / | awk "NR==2{print \$4}")"
if [ -e "$MARKER" ]; then echo marker=present; else echo marker=absent; fi
if sudo -n true 2>/dev/null; then echo sudo=ok; else echo sudo=no; fi
for p in "$SNAP" "$FAKEGH" "$FGRUN" "$CA_DEST"; do [ -e "$p" ] && echo "leftover=$p"; done
grep -q "mdb-ota-hw-rehearsal" /etc/hosts && echo "leftover=/etc/hosts tagged line"
pgrep -f "$PKILL_PATTERN" >/dev/null && echo "leftover=fake GitHub server running"
ss -Hltn "sport = :443" 2>/dev/null | grep -q . && echo "port443=busy" || echo "port443=free"
true' "hostname=dry-run-host
model=Raspberry Pi 5 Model B Rev 1.0 (dry-run)
version=${CUR_VER}
free_kib=99999999
marker=absent
sudo=ok
port443=free")" || die "cannot reach ${PI_HOST} over ssh (BatchMode — keys must be set up)"
    printf '%s\n' "$info" > "${LOGDIR}/preflight.txt"
    local host model ver free
    host="$(hw_kv hostname "$info")"; model="$(hw_kv model "$info")"; ver="$(hw_kv version "$info")"; free="$(hw_kv free_kib "$info")"
    say "TARGET: ${PI_HOST}"
    echo "    hostname: ${host}"
    echo "    model:    ${model}"
    echo "    VERSION:  ${ver}"
    if grep -q '^leftover=' <<<"$info"; then
        grep '^leftover=' <<<"$info" | sed 's/^leftover=/    leftover from an earlier run: /'
        die "clean up the leftovers above first (README.md 'Recovering from an aborted run')"
    fi
    [[ "$(hw_kv sudo "$info")" == ok ]] || die "passwordless sudo (sudo -n) is required on the box"
    # The rehearsal ends by deploying THIS checkout and comparing against the
    # snapshot; a box on anything else could not come back to where it was.
    [[ "$ver" == "$CUR_VER" ]] || die "the box runs ${ver:-?} but this checkout is ${CUR_VER}: deploy this checkout first (deploy_cpp.sh + pisim.sh push) so the final restore puts the box back exactly"
    [[ "$(hw_kv marker "$info")" == absent ]] || die "${MARKER} exists: an OTA install/rollback is mid-flight or was interrupted — refusing"
    [[ "${free:-0}" =~ ^[0-9]+$ ]] && (( free >= MIN_FREE_KIB )) \
        || die "SD card free space ${free} KiB < 3 GiB (backup + two release trees + snapshot)"
    [[ "$(hw_kv port443 "$info")" == free ]] || die "something already listens on the box's port 443 — the fake GitHub needs it"
    rpy "kiosk idle check" kiosk-idle "${DATA}/kiosk_status.json" \
        || die "the kiosk is not idle on the playlist screen (or is in a game) — leave it on the main menu"

    lrun "OLD release exists on real GitHub (read-only HEAD request)" \
        curl -fsSIL -o /dev/null --max-time 30 \
        "https://github.com/${GH_REPO}/releases/download/v${OLD_VER}/magic-dingus-box-${OLD_VER}.tar.gz" \
        || die "v${OLD_VER} source asset is not downloadable from github.com/${GH_REPO}"

    if dry; then
        vnote "[dry-run] a real run requires --yes after this printout"
    elif [[ $YES != 1 ]]; then
        die "nothing has been changed. Re-run with --yes to rehearse on ${host} (${model})."
    fi
}

# ===========================================================================
# 2. build NEW release artifacts (+ the pisim binary used by the final
#    restore) BEFORE touching the box, so a build failure costs nothing
# ===========================================================================
step_artifacts() {
    step "2. NEW release artifacts (run.sh artifacts) + pisim kiosk binary"
    export OTA_OLD_REF="$OLD_REF" OTA_NEW_REF="$NEW_REF"
    lrun --log "${LOGDIR}/artifacts.log" "release artifacts (OTA_WORK=${OTA_WORK})" "${HERE}/run.sh" artifacts \
        || die "run.sh artifacts failed (log: ${LOGDIR}/artifacts.log)"
    REL_DIR="${OTA_WORK}/release/v${NEW_VER}"
    local f
    for f in "magic-dingus-box-${NEW_VER}.tar.gz" checksum.sha256 "magic_dingus_box_cpp-arm64-${NEW_VER}.tar.gz"; do
        if dry; then vnote "[dry-run] expects ${REL_DIR}/${f}"; continue; fi
        [[ -s "${REL_DIR}/${f}" ]] || die "missing artifact ${REL_DIR}/${f}"
    done
    if ! dry; then
        local built; built="$(cat "${OTA_WORK}/release/.built_from" 2>/dev/null || true)"
        [[ "$built" == "$NEW_SHA" ]] || die "artifacts in ${OTA_WORK} were built from ${built:-?}, not ${NEW_SHA}"
        for f in "magic-dingus-box-${NEW_VER}.tar.gz" "magic_dingus_box_cpp-arm64-${NEW_VER}.tar.gz"; do
            if python3 "$HELPERS" check-tarball "${REL_DIR}/${f}"; then
                vpass "${f}: no AppleDouble ._ files, no LFS pointers"
            else
                die "${f} carries AppleDouble or LFS-pointer files — it would land on the box"
            fi
        done
    fi
    lrun --log "${LOGDIR}/pisim_build.log" "pisim kiosk binary for the final restore" \
        "${REPO}/magic_dingus_box_cpp/dev/pisim/pisim.sh" build \
        || die "pisim build failed (log: ${LOGDIR}/pisim_build.log) — nothing on the box was touched"
}

# ===========================================================================
# 3. snapshot (on the box AND copied to the Mac)
# ===========================================================================
step_snapshot() {
    step "3. snapshot (box: ${SNAP}; Mac: ${LOGDIR}/snapshot)"
    S_SNAPSHOT_ON_BOX=1; save_state
    rsh "snapshot app data, secrets and system state" '
sudo -n install -d -m 0700 -o "$(id -un)" -g "$(id -gn)" "$SNAP"
umask 077
cp -p "$INSTALL/VERSION" "$SNAP/VERSION"
sudo -n rsync -a --no-specials --no-devices "$INSTALL/config/" "$SNAP/config/"
sudo -n install -d -m 0700 "$SNAP/services"
[ -e "$INSTALL/services/.env" ] && sudo -n cp -p "$INSTALL/services/.env" "$SNAP/services/.env"
[ -d "$INSTALL/services/config" ] && sudo -n rsync -a --no-specials --no-devices "$INSTALL/services/config/" "$SNAP/services/config/"
sudo -n rsync -a --no-specials --no-devices --exclude=/media --exclude=/roms "$DATA/" "$SNAP/data/"
shopt -s nullglob
paths=(/etc/systemd/system/magic-* /etc/systemd/system/gluetun-* /etc/systemd/system/qbit-* /etc/systemd/system/kiosk-*
       /etc/systemd/system/*.d /etc/systemd/system/*.wants /usr/local/bin /etc/udev/rules.d /etc/hosts
       "$BOX_HOME/.config/pulse")
sudo -n tar -cpf "$SNAP/system_state.tar" --ignore-failed-read "${paths[@]}" 2> "$SNAP/system_state.tar.err" \
  || { cat "$SNAP/system_state.tar.err"; exit 1; }
sudo -n tar -tf "$SNAP/system_state.tar" > "$SNAP/system_state.list"
systemctl list-units --state=active --plain --no-legend --no-pager "magic-*" "gluetun-*" "qbit-*" "kiosk-*" \
  | awk "{print \$1}" > "$SNAP/active_units.txt"
echo "snapshot: $(sudo -n du -sh "$SNAP" | cut -f1), $(wc -l < "$SNAP/system_state.list") system-state entries, $(wc -l < "$SNAP/active_units.txt") active units"' || die "snapshot failed on the box"
    box_state pre
    fetch "${SNAP}/" "${LOGDIR}/snapshot/"
    dry || chmod -R go-rwx "${LOGDIR}/snapshot"
    vnote "Mac copy of the snapshot (holds secrets): ${LOGDIR}/snapshot"
}

# ===========================================================================
# 4. downgrade to OLD with the CURRENT box's update.sh, real GitHub
# ===========================================================================
step_downgrade() {
    step "4. downgrade to ${OLD_VER} (this box's update.sh, real GitHub, read-only)"
    local url="https://github.com/${GH_REPO}/releases/download/v${OLD_VER}/magic-dingus-box-${OLD_VER}.tar.gz"
    S_BOX_MODIFIED=1; save_state
    remote_job downgrade "./magic_dingus_box_cpp/scripts/update.sh install ${OLD_VER} ${url}" \
        || die "downgrade to ${OLD_VER} failed (log: ${LOGDIR}/job_downgrade.log)"
    local v; v="$(rcap "VERSION" 'tr -d "[:space:]" < "$INSTALL/VERSION"' "$OLD_VER")"
    [[ "$v" == "$OLD_VER" ]] || die "after the downgrade VERSION is '${v}', want ${OLD_VER}"
    vpass "box downgraded to ${OLD_VER}"
}

# ===========================================================================
# 5. field-state strip
# ===========================================================================
derive_lists() {
    local d="${LOGDIR}/field"
    mkdir -p "$d/old_units"
    git -C "$REPO" ls-tree --name-only "$OLD_SHA" -- systemd/ magic_dingus_box_cpp/systemd/ > "$d/old_units.txt"
    git -C "$REPO" ls-tree --name-only "$NEW_SHA" -- systemd/ magic_dingus_box_cpp/systemd/ > "$d/new_units.txt"
    git -C "$REPO" diff --name-only --diff-filter=M "$OLD_SHA" "$NEW_SHA" -- systemd/ magic_dingus_box_cpp/systemd/ > "$d/changed_units.txt"
    local re='systemd/system/[A-Za-z0-9@._-]+\.d/[A-Za-z0-9._-]+\.conf'
    { git -C "$REPO" grep -ohE "$re" "$OLD_SHA" -- magic_dingus_box_cpp/scripts scripts || true; } | sed 's/^[^:]*://' | sort -u > "$d/old_dropins.txt"
    { git -C "$REPO" grep -ohE "$re" "$NEW_SHA" -- magic_dingus_box_cpp/scripts scripts || true; } | sed 's/^[^:]*://' | sort -u > "$d/new_dropins.txt"
    python3 "$HELPERS" field-plan --file "$FIELD_FILE" --old-units "$d/old_units.txt" --new-units "$d/new_units.txt" \
        --changed-units "$d/changed_units.txt" --old-dropins "$d/old_dropins.txt" --new-dropins "$d/new_dropins.txt" \
        > "$d/plan.txt"
    local u
    while IFS= read -r u; do
        [[ "$u" == old-unit\ * ]] || continue
        u="${u#old-unit }"
        git -C "$REPO" show "${OLD_SHA}:${u}" > "$d/old_units/$(basename "$u")"
    done < "$d/plan.txt"
}

strip_script() {
    cat <<'EOF'
plan="$SNAP/field/plan.txt"
while read -r directive arg; do
    case "$directive" in
        unit)
            if [ -e "/etc/systemd/system/$arg" ]; then
                sudo -n systemctl disable --now "$arg" >/dev/null 2>&1 || true
                sudo -n rm -f "/etc/systemd/system/$arg"
                echo "  strip: removed unit $arg"
            else echo "  strip: unit $arg not installed"; fi ;;
        old-unit)
            n="$(basename "$arg")"
            if [ -e "/etc/systemd/system/$n" ]; then
                sudo -n install -m 0644 "$SNAP/field/old_units/$n" "/etc/systemd/system/$n"
                echo "  strip: $n -> the OLD tree's copy (OTA never reinstalls unit files)"
            else echo "  strip: $n not installed"; fi ;;
        dropin|file)
            if [ -e "$arg" ]; then
                sudo -n rm -f "$arg"
                [ "$directive" = dropin ] && sudo -n rmdir --ignore-fail-on-non-empty "$(dirname "$arg")"
                echo "  strip: removed $arg"
            else echo "  strip: $arg absent"; fi ;;
        package)
            if dpkg-query -W -f='${Status}' "$arg" 2>/dev/null | grep -q 'install ok installed'; then
                sudo -n env DEBIAN_FRONTEND=noninteractive apt-get remove -y -q "$arg" >/dev/null
                echo "  strip: apt removed $arg"
                echo "$arg" >> "$SNAP/field/removed_packages.txt"
            else echo "  strip: package $arg not installed"; fi ;;
        pulse-client-conf)
            f="$BOX_HOME/.config/pulse/client.conf"
            if [ -f "$f" ] && grep -qF "$CLIENT_CONF_MARKER" "$f"; then
                rm -f "$f"; echo "  strip: removed marker-tagged $f"
            else echo "  strip: no marker-tagged $f"; fi ;;
        run)
            if [ -f "$INSTALL/$arg" ]; then
                echo "  strip: running the OLD tree's $arg"
                sudo -n bash "$INSTALL/$arg" 2>&1 | sed 's/^/    | /'
            else echo "  strip: WARNING $arg not in the OLD tree"; fi ;;
        *) echo "  strip: unknown directive $directive"; exit 1 ;;
    esac
done < "$plan"
sudo -n systemctl daemon-reload
sudo -n systemctl reset-failed >/dev/null 2>&1 || true
EOF
}

audio_report_script() {
    cat <<'EOF'
uid="$(id -u magic)"
echo "audio unit: $(systemctl is-active magic-dingus-audio.service 2>/dev/null || true) / file $([ -e /etc/systemd/system/magic-dingus-audio.service ] && echo present || echo absent)"
pids="$(pgrep -x pulseaudio || true)"
echo "pulseaudio_count=$(printf '%s' "$pids" | grep -c . || true)"
for p in $pids; do echo "pulseaudio pid $p cgroup: $(cut -d: -f3 /proc/$p/cgroup 2>/dev/null)"; done
sink="$(sudo -n -u magic env XDG_RUNTIME_DIR=/run/user/$uid pactl get-default-sink 2>/dev/null || true)"
echo "default_sink=${sink}"
sudo -n -u magic env XDG_RUNTIME_DIR=/run/user/$uid pactl list short sinks 2>/dev/null | sed 's/^/  sink: /' || true
if grep -lsE 'eld_valid[[:space:]]+1' /proc/asound/card*/eld* >/dev/null 2>&1; then echo "tv_audio=yes"; else echo "tv_audio=no"; fi
grep -HsE 'monitor_name|eld_valid' /proc/asound/card*/eld* | sed 's/^/  eld: /' || true
echo "hdmi_hide_rule=$([ -e /etc/udev/rules.d/91-pulse-ignore-unused-hdmi.rules ] && echo present || echo absent)"
EOF
}

# confirm_strip_script <plan> — a remote script printing "still-present=..."
# for every plan entry the strip should have removed, plus "version=...".
confirm_strip_script() {
    local d a
    while read -r d a; do
        case "$d" in
            unit) printf '[ -e /etc/systemd/system/%q ] && echo "still-present=unit %q"\n' "$a" "$a" ;;
            dropin|file) printf '[ -e %q ] && echo "still-present=%q"\n' "$a" "$a" ;;
            package)
                printf 'dpkg-query -W -f="\\${Status}" %q 2>/dev/null | grep -q "install ok installed" && echo "still-present=package %q"\n' "$a" "$a"
                if [[ "$a" == python3-gunicorn ]]; then
                    printf 'python3 -c "import gunicorn" 2>/dev/null && echo "still-present=gunicorn importable"\n'
                fi ;;
        esac
    done < "$1"
    printf '%s\n' 'echo "version=$(tr -d "[:space:]" < "$INSTALL/VERSION")"' 'true'
}

step_field_strip() {
    step "5. field-state strip (system state ${NEW_VER} introduced that a ${OLD_VER} field box lacks)"
    derive_lists
    say "strip plan (${FIELD_FILE##*/} + git-derived units/drop-ins):"
    sed 's/^/    /' "${LOGDIR}/field/plan.txt"
    rpy --sudo "system listing before the strip" syslisting "${SNAP}/syslisting_prestrip.json"
    rsh "make field dir" 'mkdir -p "$SNAP/field"'
    push "${SNAP}/field/" "${LOGDIR}/field/plan.txt" "${LOGDIR}/field/old_units"
    rsh "apply the strip, restart web + kiosk" "$(strip_script)
sudo -n systemctl restart magic-dingus-web.service
$(hw_kiosk_restart_snippet)
sleep 8" || die "the field-state strip failed part-way"
    rpy --sudo "system listing after the strip" syslisting "${SNAP}/syslisting_poststrip.json"
    fetch "${SNAP}/syslisting_prestrip.json" "${LOGDIR}/state/syslisting_prestrip.json"
    fetch "${SNAP}/syslisting_poststrip.json" "${LOGDIR}/state/syslisting_poststrip.json"
    fetch "${SNAP}/field/" "${LOGDIR}/field/box/" || true
    if ! dry; then
        python3 "$HELPERS" changed-keys "${LOGDIR}/state/syslisting_prestrip.json" "${LOGDIR}/state/syslisting_poststrip.json" \
            > "${LOGDIR}/strip_journal.txt"
        say "strip touched $(wc -l < "${LOGDIR}/strip_journal.txt" | tr -d ' ') system-state path(s) (${LOGDIR}/strip_journal.txt)"
        push "${SNAP}/field/" "${LOGDIR}/strip_journal.txt"
    fi

    # The strip must have taken, or the rehearsal does not test a field box.
    local chk
    chk="$(rcap "confirm the strip" "$(confirm_strip_script "${LOGDIR}/field/plan.txt")" "version=${OLD_VER}")"
    if grep -q '^still-present=' <<<"$chk"; then
        grep '^still-present=' <<<"$chk" | sed 's/^/    /'
        die "the field-state strip did not take — the rehearsal would not test a field box"
    fi
    veq "box is a ${OLD_VER} field box" "$(hw_kv version "$chk")" "$OLD_VER"
    local audio; audio="$(rcap "audio state on the stripped ${OLD_VER} box" "$(audio_report_script)" "default_sink=dry-run")"
    printf '%s\n' "$audio" | tee "${LOGDIR}/audio_old_field.txt" | sed 's/^/    /'
    vnote "${OLD_VER} field-box audio recorded (default sink '$(hw_kv default_sink "$audio")'; with the TV on HDMI1 ${OLD_VER} is silent — recorded, not failed)"
}

# ===========================================================================
# 6. fake GitHub on the box
# ===========================================================================
step_fakegh_up() {
    step "6. fake GitHub on the box (temporary CA, tagged /etc/hosts line, setsid server)"
    S_FAKEGH=1; S_FAKEGH_DOWN=0; save_state
    rsh "fakegh dirs" 'mkdir -p "$FAKEGH/release"'
    push "${FAKEGH}/release/" "${REL_DIR}"
    push "${FAKEGH}/release/" "${OTA_WORK}/release/body_template.md"
    push "${FAKEGH}/" "${HERE}/box/fake_github.py"
    rsh "CA + leaf, trust store, /etc/hosts, start the server" "
cp -p /etc/hosts \"\$FAKEGH/hosts.orig\"
sudo -n install -d -m 0755 \"\$FGRUN\"
sudo -n bash -c '
set -e
cd \"$FGRUN\"
openssl req -x509 -newkey rsa:2048 -nodes -days 3 -subj \"/CN=MDB OTA hardware rehearsal CA (temporary)\" \\
  -addext \"basicConstraints=critical,CA:TRUE\" -addext \"keyUsage=critical,keyCertSign,cRLSign\" \\
  -keyout ca.key -out ca.crt 2>/dev/null
openssl req -newkey rsa:2048 -nodes -subj \"/CN=github.com\" -keyout leaf.key -out leaf.csr 2>/dev/null
printf \"%s\\n\" \"subjectAltName=DNS:github.com,DNS:api.github.com,DNS:codeload.github.com,DNS:uploads.github.com,DNS:objects.githubusercontent.com,DNS:release-assets.githubusercontent.com\" \\
  \"basicConstraints=CA:FALSE\" \"extendedKeyUsage=serverAuth\" > ext.cnf
openssl x509 -req -in leaf.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3 -extfile ext.cnf -out leaf.crt 2>/dev/null
chmod 0600 ca.key leaf.key'
sudo -n install -m 0644 \"\$FGRUN/ca.crt\" \"\$CA_DEST\"
sudo -n update-ca-certificates >/dev/null
printf '%s\n' \"\$HOSTS_LINE\" | sudo -n tee -a /etc/hosts >/dev/null
printf '{\"latest\": \"%s\", \"asset_order\": \"release\", \"minify\": false}\n' $(printf '%q' "$NEW_VER") | sudo -n tee \"\$FGRUN/control.json\" >/dev/null
: > \"\$FAKEGH/requests.jsonl\"
sudo -n setsid env FAKE_GH_RELEASES=\"\$FAKEGH/release\" FAKE_GH_CONTROL=\"\$FGRUN/control.json\" \\
  FAKE_GH_LOG=\"\$FAKEGH/requests.jsonl\" FAKE_GH_REPO=\"\$GH_REPO\" FAKE_GH_TEMPLATE_VERSION=$(printf '%q' "$NEW_VER") \\
  python3 \"\$FAKEGH/fake_github.py\" \"\$FGRUN/leaf.crt\" \"\$FGRUN/leaf.key\" > \"\$FAKEGH/server.log\" 2>&1 < /dev/null &
for _ in \$(seq 1 50); do
  curl -fsS -o /dev/null --max-time 3 \"https://api.github.com/repos/\$GH_REPO/releases/latest\" 2>/dev/null && break; sleep 0.3
done
echo \"server: \$(pgrep -af \"\$PKILL_PATTERN\" || echo NOT RUNNING)\"" || die "could not bring up the fake GitHub"
    local tag n
    tag="$(rcap "fake api.github.com latest" 'curl -fsS --max-time 10 "https://api.github.com/repos/$GH_REPO/releases/latest" | python3 -c "import json,sys; print(json.load(sys.stdin)[\"tag_name\"])"' "v${NEW_VER}")" || true
    n="$(rcap "fake GitHub request log" 'wc -l < "$FAKEGH/requests.jsonl"' 1)" || true
    [[ "$tag" == "v${NEW_VER}" && "${n// /}" -gt 0 ]] \
        || die "the box does not reach the FAKE GitHub (latest tag '${tag}', ${n:-0} logged requests)"
    vpass "box resolves api.github.com to the fake (latest v${NEW_VER}, served from ${FAKEGH})"
    fetch "${FAKEGH}/hosts.orig" "${LOGDIR}/hosts.orig"   # a Mac-side copy for manual recovery
}

# ===========================================================================
# 7. install NEW through the OLD web admin, as its UI does
# ===========================================================================
wait_version() {  # wait_version <want> <secs> — VERSION file AND web admin agree
    local want="$1" secs="$2" t0=$SECONDS v w
    while (( SECONDS - t0 < secs )); do
        v="$(rcap "VERSION" 'tr -d "[:space:]" < "$INSTALL/VERSION" 2>/dev/null || true' "$want")" || true
        w="$(web GET /admin/update/version "" 5 2>/dev/null | json - data version)" || true
        dry && w="$want"
        [[ "$v" == "$want" && "$w" == "$want" ]] && return 0
        sleep 3
    done
    echo "VERSION=${v:-?} web=${w:-?}"
    return 1
}

step_install_new() {
    step "7. install ${NEW_VER} through the ${OLD_VER} web admin (GET check -> POST install -> poll status)"
    rsh "restart the web admin (drops any cached real-GitHub check)" "sudo -n systemctl restart magic-dingus-web.service"
    wait_version "$OLD_VER" 60 || die "the ${OLD_VER} web admin did not come back"
    local cj latest url want
    cj="$(web GET /admin/update/check "" 60)" || true
    printf '%s\n' "$cj" > "${LOGDIR}/web_check_old.json"
    latest="$(json - data latest_version <<<"$cj")"; url="$(json - data download_url <<<"$cj")"
    want="https://github.com/${GH_REPO}/releases/download/v${NEW_VER}/magic-dingus-box-${NEW_VER}.tar.gz"
    if dry; then latest="$NEW_VER"; url="$want"; fi
    veq "OLD admin check: latest_version" "$latest" "$NEW_VER"
    veq "OLD admin check: download_url is the source asset" "$url" "$want"
    [[ "$latest" == "$NEW_VER" && -n "$url" ]] || die "the ${OLD_VER} admin does not offer ${NEW_VER}"

    local body st jid
    body="$(printf '{"version": "%s", "download_url": "%s"}' "$NEW_VER" "$url")"
    st="$(web POST /admin/update/install "$body" 30)" || true
    printf '%s\n' "$st" > "${LOGDIR}/web_install_start.json"
    jid="$(json - data job_id <<<"$st")"
    dry && jid="dry-run-job"
    [[ -n "$jid" ]] || die "install job did not start: ${st}"
    vpass "install job ${jid} started"
    local t0=$SECONDS last="" line s
    while ! dry && (( SECONDS - t0 < JOB_TIMEOUT )); do
        if ! s="$(web GET "/admin/update/status/${jid}" "" 5 2>/dev/null)" || [[ -z "$(json - data status <<<"$s")" ]]; then
            vnote "status poll failed (web admin restarting onto ${NEW_VER}) — falling back to /admin/update/check, as manager.js does"
            break
        fi
        line="$(json - data status <<<"$s")/$(json - data stage <<<"$s")/$(json - data progress <<<"$s")"
        [[ "$line" != "$last" ]] && { echo "      [web] ${line}  $(json - data message <<<"$s")"; last="$line"; }
        printf '%s\n' "$s" >> "${LOGDIR}/web_install_status.jsonl"
        case "$(json - data status <<<"$s")" in
            complete) break ;;
            error) vfail "install job reported error: $(json - data message <<<"$s")"; break ;;
        esac
        sleep 2
    done
    if wait_version "$NEW_VER" 900; then vpass "box on ${NEW_VER} (VERSION + web admin agree)"
    else die "box did not reach ${NEW_VER} through the web admin"; fi
}

# ===========================================================================
# 8. / 9. verification helpers
# ===========================================================================
verify_units() {  # verify_units <phase> <expect-audio-unit: yes|no>
    local phase="$1" audio="$2" out
    out="$(rcap "unit states (${phase})" '
while read -r u; do [ -n "$u" ] && echo "unit $u $(systemctl is-active "$u" 2>/dev/null || true)"; done < "$SNAP/active_units.txt"
for u in magic-dingus-box-cpp.service magic-dingus-web.service magic-dingus-audio.service; do
  echo "nrestarts $u $(systemctl show -p NRestarts --value "$u" 2>/dev/null)"
done
systemctl list-units --failed --plain --no-legend --no-pager "magic-*" "gluetun-*" "qbit-*" "kiosk-*" | awk "{print \"failed \" \$1}"
true' "unit magic-dingus-box-cpp.service active")"
    printf '%s\n' "$out" > "${LOGDIR}/units_${phase}.txt"
    local bad
    bad="$(awk -v audio="$audio" '$1=="unit" && $3!="active" && !($2=="magic-dingus-audio.service" && audio=="no") {print $2" "$3}' <<<"$out")"
    [[ -z "$bad" ]] && vpass "${phase}: every unit active before the rehearsal is active" \
                    || vfail "${phase}: not active: $(tr '\n' ' ' <<<"$bad")"
    bad="$(awk '$1=="failed" {print $2}' <<<"$out")"
    [[ -z "$bad" ]] && vpass "${phase}: no failed magic-*/gluetun-*/qbit-*/kiosk-* units" || vfail "${phase}: failed units: $(tr '\n' ' ' <<<"$bad")"
    local u n
    for u in magic-dingus-box-cpp.service magic-dingus-web.service magic-dingus-audio.service; do
        [[ "$u" == magic-dingus-audio.service && "$audio" == no ]] && continue
        n="$(awk -v u="$u" '$1=="nrestarts" && $2==u {print $3}' <<<"$out")"
        dry && n=0
        veq "${phase}: ${u} NRestarts" "${n:-?}" 0
    done
}

verify_audio() {  # verify_audio <phase> <mode: unit|legacy>
    local phase="$1" mode="$2" a
    a="$(rcap "audio state (${phase})" "$(audio_report_script)" "pulseaudio_count=1
default_sink=dry-run
tv_audio=no")"
    printf '%s\n' "$a" > "${LOGDIR}/audio_${phase}.txt"
    printf '%s\n' "$a" | sed 's/^/    /'
    veq "${phase}: exactly one pulseaudio" "$(hw_kv pulseaudio_count "$a")" 1
    if [[ "$mode" == unit ]]; then
        if dry || grep -q 'cgroup: .*magic-dingus-audio.service' <<<"$a"; then vpass "${phase}: pulseaudio runs in magic-dingus-audio.service's cgroup"
        else vfail "${phase}: pulseaudio is not in magic-dingus-audio.service's cgroup"; fi
        if [[ "$(hw_kv tv_audio "$a")" == yes ]]; then
            local s; s="$(hw_kv default_sink "$a")"
            [[ -n "$s" && "$s" != auto_null ]] && vpass "${phase}: TV reports audio and the default sink is ${s}" \
                                              || vfail "${phase}: TV reports audio (ELD valid) but the default sink is '${s:-none}'"
        else
            vnote "${phase}: no TV reports audio (no valid ELD) — default sink '$(hw_kv default_sink "$a")' not judged"
        fi
    else
        if grep -q '^audio unit: active' <<<"$a"; then vfail "${phase}: a stale magic-dingus-audio.service is still active on ${OLD_VER}"
        else vpass "${phase}: no magic-dingus-audio.service running (legacy init_audio.sh path)"; fi
        grep -q 'file present' <<<"$a" && vnote "${phase}: magic-dingus-audio.service unit FILE still installed (inert: ConditionPathExists)"
        vnote "${phase}: default sink '$(hw_kv default_sink "$a")' (recorded only on ${OLD_VER})"
    fi
}

verify_web() {  # verify_web <phase> <version>
    local code
    code="$(rcap "web admin root" 'curl -s -o /dev/null -w "%{http_code}" --max-time 10 -H "Host: localhost" "$WEB/"' 200)" || true
    veq "$1: Content Manager serves /" "$code" 200
    veq "$1: /admin/update/version" "$(dry && echo "$2" || web GET /admin/update/version "" 10 | json - data version)" "$2"
}

verify_box_sh() {  # verify_box_sh <phase>
    local rc=0
    rsh "verify_box.sh --with-services (${1})" 'sudo -n bash "$INSTALL/magic_dingus_box_cpp/scripts/verify_box.sh" --with-services' \
        > "${LOGDIR}/verify_box_${1}.txt" 2>&1 || rc=$?
    tail -4 "${LOGDIR}/verify_box_${1}.txt" | sed 's/^/    /'
    [[ $rc == 0 ]] && vpass "$1: verify_box.sh --with-services: SHIPPABLE" \
                   || vfail "$1: verify_box.sh --with-services exited ${rc} (${LOGDIR}/verify_box_${1}.txt)"
}

verify_data() {  # verify_data <label>
    box_state "$1"
    vcompare appdata pre "$1"
    vcompare content pre "$1"
}

step_verify_new() {
    step "8. verify ${NEW_VER}"
    veq "VERSION" "$(rcap VERSION 'tr -d "[:space:]" < "$INSTALL/VERSION"' "$NEW_VER")" "$NEW_VER"
    local has_audio=no
    git -C "$REPO" cat-file -e "${NEW_SHA}:magic_dingus_box_cpp/systemd/magic-dingus-audio.service" 2>/dev/null && has_audio=yes
    verify_units new "$has_audio"
    if [[ $has_audio == yes ]]; then verify_audio new unit; else verify_audio new legacy; fi
    verify_web new "$NEW_VER"
    verify_box_sh new
    verify_data new
    local m; m="$(rcap "OTA marker" '[ -e "$MARKER" ] && echo present || echo absent' absent)"
    veq "new: OTA in-progress marker cleared" "$m" absent
}

step_rollback() {
    step "9. rollback through the ${NEW_VER} web admin (POST /admin/update/rollback)"
    local r rc=0
    r="$(web POST /admin/update/rollback '{}' 240)" || rc=$?
    printf '%s\n' "$r" > "${LOGDIR}/web_rollback_response.txt"
    if [[ $rc == 0 ]]; then vnote "rollback POST answered: $(tr '\n' ' ' <<<"$r" | cut -c1-200)"
    else vnote "rollback POST got no answer (rc=${rc}: the job restarts the web service under its own request) — polling VERSION"; fi
    wait_version "$OLD_VER" 400 || die "box did not roll back to ${OLD_VER}"
    vpass "rolled back to ${OLD_VER} (VERSION + web admin agree)"
    sleep 10
    step "9b. verify the rolled-back ${OLD_VER}"
    verify_units old no
    verify_audio old legacy
    verify_web old "$OLD_VER"
    verify_data old
    local m; m="$(rcap "OTA marker" '[ -e "$MARKER" ] && echo present || echo absent' absent)"
    veq "old: OTA in-progress marker cleared" "$m" absent
}

# ===========================================================================
# 10. teardown + restore this checkout
# ===========================================================================
step_teardown() {
    step "10a. teardown: fake GitHub server, /etc/hosts, CA, ${FGRUN}"
    teardown_fakegh || die "fake GitHub teardown not confirmed"
    vpass "fake GitHub removed; /etc/hosts restored byte-for-byte; CA removed (update-ca-certificates --fresh)"
}

restore_stripped_script() {
    cat <<'EOF'
# Files the field-state strip touched that deploy_cpp.sh does NOT rewrite
# (e.g. setup_services.sh-installed units put back to their OLD copy) go
# back to their pre-rehearsal bytes from the snapshot tar. Paths deploy
# DID rewrite already match and are left alone.
j="$SNAP/field/strip_journal.txt"
[ -f "$j" ] || { echo "restore: no strip journal"; exit 0; }
python3 - "$SNAP/syslisting_pre.json" "$j" > "$SNAP/field/restore_candidates.txt" <<'PY'
import hashlib, json, os, sys
pre = json.load(open(sys.argv[1]))
for p in (l.strip() for l in open(sys.argv[2]) if l.strip()):
    want = pre.get(p)
    if want is None:
        continue
    try:
        cur = ("symlink:" + os.readlink(p)) if os.path.islink(p) else hashlib.md5(open(p, "rb").read()).hexdigest()
    except OSError:
        cur = None
    if cur != want:
        print(p)
PY
n=0
while IFS= read -r p; do
    sudo -n tar -xpf "$SNAP/system_state.tar" -C / "${p#/}" && { echo "restore: $p <- snapshot"; n=$((n + 1)); }
done < "$SNAP/field/restore_candidates.txt"
if [ -f "$SNAP/field/removed_packages.txt" ]; then
    while IFS= read -r pkg; do
        if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed'; then
            sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends "$pkg" >/dev/null \
                && echo "restore: apt reinstalled $pkg" || echo "restore: WARNING could not reinstall $pkg"
        fi
    done < "$SNAP/field/removed_packages.txt"
fi
[ "$n" -gt 0 ] && sudo -n systemctl daemon-reload
echo "restore: ${n} strip-touched path(s) put back"
EOF
}

step_restore() {
    step "10b. restore this checkout (${CUR_VER}): deploy guard, deploy_cpp.sh, pisim push"
    local dargs=(--var "CPP_DIR=${REPO}/magic_dingus_box_cpp" --var "PI_HOST=${PI_HOST}" --var "PI_DIR=${INSTALL}")
    if dry; then
        python3 "$HELPERS" deploy-guard "${REPO}/magic_dingus_box_cpp/scripts/deploy_cpp.sh" "${dargs[@]}" --print-only
    else
        python3 "$HELPERS" deploy-guard "${REPO}/magic_dingus_box_cpp/scripts/deploy_cpp.sh" "${dargs[@]}" \
            --log "${LOGDIR}/deploy_guard.log" \
            || die "deploy guard failed (a --delete rsync would remove something under data/ or config/, or its dry run could not complete) — NOT deploying (${LOGDIR}/deploy_guard.log)"
        vpass "deploy guard: none of the three --delete rsyncs would delete under data/ or config/"
    fi
    if ! dry && [[ ! -x "${REPO}/magic_dingus_box_cpp/build-pisim/magic_dingus_box_cpp" ]]; then
        lrun --log "${LOGDIR}/pisim_build.log" "pisim build" "${REPO}/magic_dingus_box_cpp/dev/pisim/pisim.sh" build \
            || die "pisim build failed (${LOGDIR}/pisim_build.log)"
    fi
    lrun --log "${LOGDIR}/deploy_cpp.log" "deploy_cpp.sh (no --build)" \
        env PI_HOST="$PI_HOST" "${REPO}/magic_dingus_box_cpp/scripts/deploy_cpp.sh" \
        || die "deploy_cpp.sh failed (${LOGDIR}/deploy_cpp.log)"
    lrun --log "${LOGDIR}/pisim_push.log" "pisim.sh push" \
        env PI_HOST="$PI_HOST" "${REPO}/magic_dingus_box_cpp/dev/pisim/pisim.sh" push \
        || die "pisim.sh push failed (${LOGDIR}/pisim_push.log)"
    S_RESTORED=1; save_state
    if [[ -f "${LOGDIR}/strip_journal.txt" ]] && ! dry; then push "${SNAP}/field/" "${LOGDIR}/strip_journal.txt"; fi
    rsh "put strip-touched files back" "$(restore_stripped_script)" | sed 's/^/    /'

    step "10c. verify the restored box against the pre-rehearsal snapshot"
    sleep 10
    veq "VERSION is this checkout's" "$(rcap VERSION 'tr -d "[:space:]" < "$INSTALL/VERSION"' "$CUR_VER")" "$CUR_VER"
    box_state post
    vcompare syslisting pre post
    vcompare appdata pre post
    vcompare content pre post
    local has_audio=no
    [[ -f "${REPO}/magic_dingus_box_cpp/systemd/magic-dingus-audio.service" ]] && has_audio=yes
    verify_units post "$has_audio"
    verify_box_sh post
}

step_cleanup() {
    step "10d. delete the on-box snapshot (secrets) and ${FAKEGH}"
    local out
    out="$(rcap "delete snapshot + fakegh" 'sudo -n rm -rf "$SNAP" "$FAKEGH"; for p in "$SNAP" "$FAKEGH"; do [ -e "$p" ] && echo "left=$p"; done; true' "")"
    if grep -q '^left=' <<<"$out"; then vfail "could not delete: $(tr '\n' ' ' <<<"$out")"; return; fi
    S_SNAPSHOT_ON_BOX=0; save_state
    vpass "on-box snapshot and ${FAKEGH} deleted"
    say "Mac copy of the snapshot (contains secrets — delete when done): ${LOGDIR}/snapshot"
}

# ===========================================================================
main() {
    say "log dir: ${LOGDIR}$(dry && echo '   (DRY RUN — nothing is executed)')"
    if [[ -n "$RESTORE_ONLY" ]]; then
        local want_host="$PI_HOST"
        # shellcheck disable=SC1091
        . "${LOGDIR}/state.env"
        [[ "$PI_HOST" == "$want_host" ]] || die "${LOGDIR} is a rehearsal of ${PI_HOST}, not ${want_host}"
        CUR_VER="$(tr -d '[:space:]' < "${REPO}/VERSION")"
        [[ $YES == 1 || $DRY == 1 ]] || die "--restore-only deploys to ${PI_HOST}: add --yes"
        S_RESTORED=0; S_DONE=0
        S_FAKEGH=1; S_FAKEGH_DOWN=0   # teardown is idempotent: always run it
        step_teardown
        step_restore
        step_cleanup
    else
        step_preflight
        save_state
        step_artifacts
        step_snapshot
        step_downgrade
        step_field_strip
        step_fakegh_up
        step_install_new
        step_verify_new
        step_rollback
        step_teardown
        step_restore
        step_cleanup
    fi
    S_DONE=1; save_state
    if (( FAILS > 0 )); then
        say "FINISHED WITH ${FAILS} VERIFICATION FAILURE(S) — see ${RESULTS}"
        exit 1
    fi
    if dry; then
        say "DRY RUN complete — nothing was executed; every command is in ${LOGDIR}/remote_commands.sh"
    else
        say "REHEARSAL PASSED — box restored to ${CUR_VER}; logs + snapshot: ${LOGDIR}"
    fi
}
main
