#!/usr/bin/env bash
#
# fleet_check.sh — run every box's own acceptance test, in parallel, from
# the Mac, and print one table.
#
#   scripts/fleet_check.sh magic@10.0.0.227 magic@magicpi-ab12.local
#   scripts/fleet_check.sh --hosts fleet.txt            # one host per line, # comments
#   scripts/fleet_check.sh --hosts fleet.txt --with-services -j 8
#
# For each host it runs, over ONE ssh session, the box's installed
#   /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_box.sh
# exactly the way the Content Manager's Box health card does
# (`sudo -n /usr/bin/timeout --kill-after=10 <secs> /bin/bash verify_box.sh
# [--with-services]`; without sudo when passwordless sudo is unavailable —
# verify_box.sh then reports what it can), and reads the board model,
# VERSION, update channel and uptime.
#
# STRICTLY READ-ONLY on the boxes: it only reads files and runs
# verify_box.sh, which is itself read-only (its only write is its own
# mktemp log for --with-services). Nothing is installed, restarted or
# changed. The only local state touched is ssh's known_hosts
# (StrictHostKeyChecking=accept-new, as the bats Pi tier does).
#
# Output: host (+ the box's hostname when addressed by IP) | board | version | channel | uptime | pass/warn/fail |
# result | first failing check. Result is SHIPPABLE, NOT-SHIPPABLE,
# UNREACHABLE, TIMEOUT (verify_box.sh hit the deadline), NO-VERIFY (no
# verify_box.sh installed — a pre-acceptance-test release) or ERROR.
#
# Exit 0 only when EVERY host is reachable and SHIPPABLE; 1 otherwise;
# 2 on a usage error.
#
# Options:
#   --hosts FILE         read hosts from FILE (repeatable; blank lines and
#                        # comments ignored). Positional hosts are added too.
#   --with-services      pass --with-services through (adds verify_services.sh;
#                        default deadline 600 s instead of 180 s)
#   -j, --jobs N         hosts checked at once (default 4)
#   --connect-timeout S  ssh ConnectTimeout (default 10)
#   --timeout S          verify_box.sh deadline on the box (default 180/600)
#   --log-dir DIR        keep each host's full output as DIR/<host>.log
#   -h, --help           this text
#
# Test seams: FLEET_SSH (ssh binary), FLEET_BOX_BASE (install dir on the box,
# default /opt/magic_dingus_box).

set -uo pipefail

usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; }

SSH_BIN="${FLEET_SSH:-ssh}"
BOX_BASE="${FLEET_BOX_BASE:-/opt/magic_dingus_box}"
JOBS=4
CONNECT_TIMEOUT=10
WITH_SERVICES=0
VERIFY_TIMEOUT=""
LOG_DIR=""
HOSTS=()

die_usage() { echo "fleet_check: $1" >&2; echo "Run with --help for usage." >&2; exit 2; }
is_uint() { [[ "$1" =~ ^[0-9]+$ ]] && (( $1 > 0 )); }

add_host() {
    local h="$1" existing
    [[ -n "$h" ]] || return 0
    # Hosts become ssh arguments; refuse anything that could be read as an
    # option or carries shell metacharacters.
    local ok_re='^[][A-Za-z0-9._@:%-]+$'   # "]" first: literal inside [...]
    [[ "$h" =~ $ok_re && "$h" != -* ]] \
        || die_usage "refusing suspicious host: $h"
    for existing in ${HOSTS[@]+"${HOSTS[@]}"}; do
        [[ "$existing" == "$h" ]] && return 0
    done
    HOSTS+=("$h")
}

read_hosts_file() {
    local line
    [[ -r "$1" ]] || die_usage "cannot read hosts file: $1"
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        add_host "$line"
    done < "$1"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hosts) [[ $# -ge 2 ]] || die_usage "--hosts needs a file"; read_hosts_file "$2"; shift 2 ;;
        --with-services) WITH_SERVICES=1; shift ;;
        -j|--jobs) [[ $# -ge 2 ]] && is_uint "$2" || die_usage "$1 needs a positive number"; JOBS="$2"; shift 2 ;;
        --connect-timeout) [[ $# -ge 2 ]] && is_uint "$2" || die_usage "--connect-timeout needs seconds"; CONNECT_TIMEOUT="$2"; shift 2 ;;
        --timeout) [[ $# -ge 2 ]] && is_uint "$2" || die_usage "--timeout needs seconds"; VERIFY_TIMEOUT="$2"; shift 2 ;;
        --log-dir) [[ $# -ge 2 ]] || die_usage "--log-dir needs a directory"; LOG_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; while [[ $# -gt 0 ]]; do add_host "$1"; shift; done ;;
        -*) die_usage "unknown option: $1" ;;
        *) add_host "$1"; shift ;;
    esac
done

[[ ${#HOSTS[@]} -gt 0 ]] || die_usage "no hosts given"
[[ "$BOX_BASE" =~ ^/[A-Za-z0-9._/-]+$ ]] || die_usage "bad FLEET_BOX_BASE: $BOX_BASE"
if [[ -z "$VERIFY_TIMEOUT" ]]; then
    # Same deadlines as the web admin's Box health card (box_health.py).
    if (( WITH_SERVICES )); then VERIFY_TIMEOUT=600; else VERIFY_TIMEOUT=180; fi
fi
if [[ -n "$LOG_DIR" ]]; then
    mkdir -p "$LOG_DIR" || die_usage "cannot create --log-dir $LOG_DIR"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/fleet_check.XXXXXX")" || { echo "fleet_check: mktemp failed" >&2; exit 2; }
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# The script each box runs (fed on stdin to `bash -s`). Read-only by design:
# file reads, hostname/uptime, and verify_box.sh. Every line it prints that
# fleet_check parses starts with "@@"; verify_box.sh's own output sits
# between @@verify_begin and @@verify_end.
cat > "$WORK/remote.sh" <<'REMOTE'
WS="${1:-0}"; T="${2:-180}"; BASE="${3:-/opt/magic_dingus_box}"
APP="$BASE/magic_dingus_box_cpp"
kv() { printf '@@%s=%s\n' "$1" "$2"; }
kv host "$(hostname 2>/dev/null)"
kv model "$(tr -d '\0' < /proc/device-tree/model 2>/dev/null)"
kv version "$(head -c 64 "$BASE/VERSION" 2>/dev/null | tr -d '[:space:]')"
# Same rule as update.sh read_update_channel: only the exact word "beta".
c="$(head -c 64 "$BASE/config/update_channel" 2>/dev/null | tr -d '[:space:]')"
if [ "$c" = "beta" ]; then kv channel beta; else kv channel stable; fi
kv uptime "$(uptime -p 2>/dev/null | sed 's/^up //')"
if [ ! -f "$APP/scripts/verify_box.sh" ]; then
    kv verify_exit missing
    exit 0
fi
set --
[ "$WS" = "1" ] && set -- --with-services
if sudo -n true 2>/dev/null; then
    kv verify_mode sudo
    echo "@@verify_begin"
    sudo -n /usr/bin/timeout --kill-after=10 "$T" /bin/bash "$APP/scripts/verify_box.sh" "$@"
    rc=$?
else
    kv verify_mode user
    echo "@@verify_begin"
    timeout --kill-after=10 "$T" /bin/bash "$APP/scripts/verify_box.sh" "$@"
    rc=$?
fi
echo "@@verify_end"
kv verify_exit "$rc"
exit 0
REMOTE

# Strip ANSI colour codes (verify_box.sh drops them without a tty, but a
# box's shell profile could still emit some).
strip_ansi() { sed $'s/\x1b\\[[0-9;]*[A-Za-z]//g'; }

# kv_of <key> <file> — value of the first "@@key=" line.
kv_of() { grep -m1 "^@@$1=" "$2" 2>/dev/null | cut -d= -f2-; }

short_board() {
    local m="$1"
    m="${m#Raspberry Pi }"; m="${m% Rev *}"; m="${m% Model B}"
    case "$1" in
        "Raspberry Pi "*) echo "Pi $m" ;;
        "") echo "?" ;;
        *) echo "$1" ;;
    esac
}

# check_host <index> <host> — writes $WORK/<index>.res, one TSV line:
# host board version channel uptime pass warn fail result first_fail
check_host() {
    local i="$1" host="$2" out="$WORK/$1.out" err="$WORK/$1.err"
    local rc=0
    "$SSH_BIN" -o BatchMode=yes -o ConnectTimeout="$CONNECT_TIMEOUT" \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 \
        -o StrictHostKeyChecking=accept-new \
        "$host" bash -s -- "$WITH_SERVICES" "$VERIFY_TIMEOUT" "$BOX_BASE" \
        < "$WORK/remote.sh" > "$out" 2> "$err" || rc=$?

    if [[ -n "$LOG_DIR" ]]; then
        { cat "$out"; echo "--- stderr (ssh exit $rc) ---"; cat "$err"; } \
            > "$LOG_DIR/$(printf '%s' "$host" | tr -c 'A-Za-z0-9._-' '_').log"
    fi

    local board="?" version="?" channel="?" up="?" p="-" w="-" f="-" result first=""
    if ! grep -q '^@@host=' "$out"; then
        first="$(strip_ansi < "$err" | grep -v '^[[:space:]]*$' | tail -1)"
        [[ -n "$first" ]] || first="ssh exit $rc"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$host" "$board" "$version" "$channel" "$up" "$p" "$w" "$f" "UNREACHABLE" "$first" \
            > "$WORK/$i.res"
        return 0
    fi

    # Name the box when it was addressed by IP (DHCP fleets).
    local boxname; boxname="$(kv_of host "$out")"
    [[ -n "$boxname" && "$host" != *"$boxname"* ]] && host="$host ($boxname)"
    board="$(short_board "$(kv_of model "$out")")"
    version="$(kv_of version "$out")"; [[ -n "$version" ]] || version="?"
    channel="$(kv_of channel "$out")"; [[ -n "$channel" ]] || channel="?"
    up="$(kv_of uptime "$out")"; [[ -n "$up" ]] || up="?"
    up="${up// days/d}"; up="${up// day/d}"; up="${up// hours/h}"; up="${up// hour/h}"
    up="${up// minutes/m}"; up="${up// minute/m}"; up="${up// weeks/w}"; up="${up// week/w}"
    up="${up//,/}"

    local vexit body totals
    vexit="$(kv_of verify_exit "$out")"
    body="$(sed -n '/^@@verify_begin$/,/^@@verify_end$/p' "$out" | strip_ansi)"
    totals="$(printf '%s\n' "$body" | grep -Eo '[0-9]+ passed, *[0-9]+ failed, *[0-9]+ warnings?' | tail -1)"
    if [[ -n "$totals" ]]; then
        p="$(echo "$totals" | sed -E 's/^([0-9]+) passed.*/\1/')"
        f="$(echo "$totals" | sed -E 's/.* ([0-9]+) failed.*/\1/')"
        w="$(echo "$totals" | sed -E 's/.* ([0-9]+) warnings?$/\1/')"
    elif [[ -n "$body" ]]; then
        # No RESULT line (killed mid-run): count what did print.
        p="$(printf '%s\n' "$body" | grep -c '\[PASS\]')"
        f="$(printf '%s\n' "$body" | grep -c '\[FAIL\]')"
        w="$(printf '%s\n' "$body" | grep -c '\[WARN\]')"
    fi
    first="$(printf '%s\n' "$body" | grep -m1 '\[FAIL\]' | sed -E 's/^[[:space:]]*\[FAIL\][[:space:]]*//')"

    case "$vexit" in
        0) result="SHIPPABLE" ;;
        1) result="NOT-SHIPPABLE" ;;
        124|137) result="TIMEOUT"; [[ -n "$first" ]] || first="verify_box.sh exceeded ${VERIFY_TIMEOUT}s" ;;
        missing) result="NO-VERIFY"; first="verify_box.sh not installed (old release?)" ;;
        "") result="ERROR"; first="${first:-connection dropped before verify_box.sh finished}" ;;
        *) result="ERROR"; first="${first:-verify_box.sh exit $vexit}" ;;
    esac
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$host" "$board" "$version" "$channel" "$up" "$p" "$w" "$f" "$result" "$first" \
        > "$WORK/$i.res"
}

# ---------------------------------------------------------------------------
# Bounded parallelism. Polls `jobs` rather than `wait -n` so it runs under
# the Mac's stock bash 3.2.
echo "fleet_check: ${#HOSTS[@]} host(s), ${JOBS} at a time, verify deadline ${VERIFY_TIMEOUT}s$( ((WITH_SERVICES)) && echo ', with services')" >&2
idx=0
for host in "${HOSTS[@]}"; do
    while [[ "$(jobs -rp | wc -l | tr -d ' ')" -ge "$JOBS" ]]; do sleep 0.2; done
    check_host "$idx" "$host" &
    idx=$((idx + 1))
done
wait

# ---------------------------------------------------------------------------
# Table.
MAX_FIRST=72
rows=()
for ((i = 0; i < idx; i++)); do
    if [[ -s "$WORK/$i.res" ]]; then
        rows+=("$(cat "$WORK/$i.res")")
    else
        rows+=("$(printf '%s\t?\t?\t?\t?\t-\t-\t-\tERROR\tinternal: no result' "${HOSTS[$i]}")")
    fi
done

headers=(HOST BOARD VERSION CHANNEL UPTIME "P/W/F" RESULT "FIRST FAILING CHECK")
widths=(4 5 7 7 6 5 6)
cells_of() {  # splits one TSV row into the global array CELLS (8 columns)
    local t
    # read -a, not t=($1): no globbing of the cell text (a "?" cell would
    # otherwise match one-character file names in the cwd).
    IFS=$'\t' read -r -a t <<< "$1"
    CELLS=("${t[0]}" "${t[1]}" "${t[2]}" "${t[3]}" "${t[4]}" "${t[5]}/${t[6]}/${t[7]}" "${t[8]}" "${t[9]:-}")
}
for row in "${rows[@]}"; do
    cells_of "$row"
    for c in 0 1 2 3 4 5 6; do
        (( ${#CELLS[$c]} > widths[c] )) && widths[c]=${#CELLS[$c]}
    done
done
print_row() {
    local c
    for c in 0 1 2 3 4 5 6; do printf '%-*s  ' "${widths[$c]}" "$1"; shift; done
    printf '%s\n' "$1"
}
print_row "${headers[@]}"
n_ok=0; n_bad=0; n_unreach=0
for row in "${rows[@]}"; do
    cells_of "$row"
    first="${CELLS[7]}"
    (( ${#first} > MAX_FIRST )) && first="${first:0:$((MAX_FIRST - 3))}..."
    print_row "${CELLS[0]}" "${CELLS[1]}" "${CELLS[2]}" "${CELLS[3]}" "${CELLS[4]}" "${CELLS[5]}" "${CELLS[6]}" "$first"
    case "${CELLS[6]}" in
        SHIPPABLE) n_ok=$((n_ok + 1)) ;;
        UNREACHABLE) n_unreach=$((n_unreach + 1)) ;;
        *) n_bad=$((n_bad + 1)) ;;
    esac
done
echo
echo "${idx} host(s): ${n_ok} shippable, ${n_bad} not shippable/error, ${n_unreach} unreachable"
[[ -n "$LOG_DIR" ]] && echo "full per-host output: $LOG_DIR"

(( n_ok == idx )) && exit 0
exit 1
