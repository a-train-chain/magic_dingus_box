#!/usr/bin/env bash
#
# Magic Dingus Box - clone-source secret fingerprints + inherited-container purge
#
# Sourced by prepare_for_cloning.sh, restore_after_cloning.sh, first_boot.sh
# and magic_dingus_box_cpp/scripts/verify_box.sh. Sourcing defines paths and
# functions and RUNS NOTHING, so tests/local can exercise the logic off-Pi.
# Executed directly it offers one command, `record` (see the bottom).
#
# Why this exists. Two VPN clients holding the SAME WireGuard private key
# knock each other off the tunnel: ProtonVPN accepts one peer per key, so
# every handshake from one box drops the other, in bursts, for as long as
# both are powered on. Every unit cloned from the owner's source box starts
# life holding that box's key, three ways over:
#
#   - services/.env (WIREGUARD_PRIVATE_KEY), which first_boot.sh wipes —
#     except that on the first golden image (2026-08-04) first_boot.sh died
#     at Step 2, long before the wipe, on every unit flashed from it;
#   - Docker's own container metadata (/var/lib/docker/containers/*/
#     config.v2.json embeds every container's ENVIRONMENT, i.e. the key), and
#     Docker's restart policy (gluetun is `restart: unless-stopped`) starts
#     such a container whenever dockerd starts, whether or not .env exists;
#   - by hand, when an operator copies a working .env onto a unit.
#
# So this library does two jobs:
#
#   1. FINGERPRINTS. prepare_for_cloning.sh records salted SHA-256
#      fingerprints of the source box's per-box secrets (every secret-named
#      services/.env value — the WireGuard key above all — plus the phone
#      remote's Flask HMAC secret) into ${MDB_SOURCE_FP_FILE}, which rides
#      into the image. verify_box.sh on any unit then FAILs when the unit's
#      .env or any container's environment still holds one of them. The file
#      holds no secret: a fingerprint is sha256(salt ":" value) cut to 128
#      bits, the salt is random per image (so two images' files cannot even
#      be linked), and the values are 256-bit random keys — there is nothing
#      to brute-force a preimage from.
#
#      The SOURCE box must never flag itself. Two independent guards:
#      restore_after_cloning.sh deletes the file from the source the moment
#      the clone is done (only when the file names THIS board — a clone that
#      runs restore keeps its copy), and the file records a salted hash of
#      the source BOARD's hardware serial (/proc/device-tree/serial-number:
#      burned into the SoC, so unlike the hostname, machine-id or the SD
#      card's contents a dd can never copy it). verify_box.sh skips the
#      comparison on the board the fingerprints came from, so a leftover file
#      on the source (restore interrupted, file put back by hand) is
#      harmless.
#
#   2. PURGE. first_boot.sh removes every container a clone inherited from
#      the source, before Docker's restart policy can bring gluetun up on the
#      source's key — through the docker CLI when dockerd is up, or directly
#      from /var/lib/docker/containers when it is not (the same state every
#      current image already ships in: prepare stashes config.v2.json, and
#      dockerd skips a container directory without one).
#
# Values are never printed, logged or passed on a command line: they travel
# from file to function on stdin, and `printf` (a builtin) feeds the hash.
#

# Where the fingerprints live in the image. /etc, not the install tree: an
# OTA rsync --delete must never be able to remove it from a unit.
MDB_SOURCE_FP_FILE="${MDB_SOURCE_FP_FILE:-/etc/magic-dingus/source_secret_fingerprints}"

# The Pi SoC's hardware serial (device tree). Overridable for tests.
MDB_BOARD_SERIAL_FILE="${MDB_BOARD_SERIAL_FILE:-/proc/device-tree/serial-number}"

# Docker's per-container state directory. Overridable for tests.
MDB_DOCKER_CONTAINERS_DIR="${MDB_DOCKER_CONTAINERS_DIR:-/var/lib/docker/containers}"

# Which services/.env keys are per-box SECRETS. By name, not by list, so a
# new credential is covered without anyone remembering to add it — the way
# the clone scrub has failed before is a list that did not grow. Today this
# matches WIREGUARD_PRIVATE_KEY, RADARR/SONARR/PROWLARR_API_KEY,
# QBITTORRENT_ADMIN_PASSWORD, MDB_QBIT_PASS and VPN_PASSWORD. It must NOT
# match a value that is legitimately identical on every box (PUID, TZ,
# STORAGE_ROOT, WIREGUARD_PUBLIC_KEY — the PROVIDER's key, the same for every
# ProtonVPN customer on that server — VPN_COUNTRIES ...): a match there would
# fail every correctly provisioned unit.
MDB_FP_SECRET_KEY_RE='(PRIVATE_KEY|PRESHARED_KEY|API_KEY|APIKEY|PASSWORD|_PASS|SECRET|TOKEN)$'

# The fingerprint label of the one secret whose reuse breaks the tunnel.
# shellcheck disable=SC2034  # used by verify_box.sh
MDB_FP_VPN_LABEL="env:WIREGUARD_PRIVATE_KEY"

# Non-.env secrets fingerprinted alongside it: LABEL=PATH. Both copies of the
# Flask secret (build/data/ is a byte-identical second copy, see
# prepare_for_cloning.sh). A missing file is skipped.
MDB_FP_DEFAULT_EXTRAS=(
    "flask-secret=/opt/magic_dingus_box/magic_dingus_box_cpp/data/flask_secret.key"
    "flask-secret=/opt/magic_dingus_box/magic_dingus_box_cpp/build/data/flask_secret.key"
)

mdb_fp_log() {
    if declare -F log >/dev/null 2>&1; then log "$1"; else echo "$1"; fi
}

# `timeout` where it exists (every Pi); plain execution where it does not
# (the Mac running the tests).
mdb_fp_timeout() {
    local secs="$1"; shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"
    else
        "$@"
    fi
}

# stdin -> 64 hex chars.
mdb_fp_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum
    else
        shasum -a 256
    fi | cut -c1-64
}

# SALT VALUE -> fingerprint (32 hex chars = 128 bits).
mdb_fp_hash() {
    printf '%s:%s' "$1" "$2" | mdb_fp_sha256 | cut -c1-32
}

# 16 random bytes as 32 hex chars.
mdb_fp_new_salt() {
    od -An -N16 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n'
}

# A raw .env / container-env value -> the value the stack actually uses.
# Trims whitespace (CR included: a CRLF .env must not hash differently) and
# ONE layer of matching quotes — docker compose strips them when it reads
# .env, so the container environment carries the bare value and both sides
# must hash the same bytes. An unquoted value loses a trailing ` # comment`
# the same way compose drops it.
mdb_fp_normalize() {
    local v="$1" q
    v="${v#"${v%%[![:space:]]*}"}"
    v="${v%"${v##*[![:space:]]}"}"
    if [[ ${#v} -ge 2 ]]; then
        q="${v:0:1}"
        if [[ ( "$q" == '"' || "$q" == "'" ) && "${v:${#v}-1:1}" == "$q" ]]; then
            printf '%s' "${v:1:${#v}-2}"
            return 0
        fi
    fi
    case "$v" in
        \"*|\'*) ;;
        *) v="${v%%[[:space:]]#*}" ;;
    esac
    printf '%s' "$v"
}

mdb_fp_is_secret_key() {
    [[ "$1" =~ $MDB_FP_SECRET_KEY_RE ]]
}

# Template / empty values are not secrets: .env.example ships __REPLACE_ME__
# and setup_services.sh writes __WILL_BE_SET_AFTER_FIRST_START__ on every box
# mid-setup; fingerprinting those would make every half-provisioned box
# "match" the source.
mdb_fp_is_placeholder() {
    [[ -z "$1" || "$1" =~ ^__[A-Z0-9_]+__$ ]] && return 0
    case "$1" in
        changeme|CHANGEME|CHANGE_ME) return 0 ;;
    esac
    return 1
}

# Reads KEY=VALUE text on stdin (a .env file, `docker inspect` env lines or
# config.v2.json's Config.Env, one entry per line; `export ` prefixes and
# comments tolerated) and, for every SECRET key with a real value, calls
#   "$@" KEY VALUE
# The value only ever exists inside this process.
mdb_fp_each_secret() {
    local line key val
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        line="${line#"${line%%[![:space:]]*}"}"
        [[ -z "$line" || "$line" == \#* ]] && continue
        [[ "$line" == "export "* ]] && line="${line#export }"
        [[ "$line" == *=* ]] || continue
        key="${line%%=*}"
        key="${key%"${key##*[![:space:]]}"}"
        [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
        mdb_fp_is_secret_key "$key" || continue
        val="$(mdb_fp_normalize "${line#*=}")"
        mdb_fp_is_placeholder "$val" && continue
        "$@" "$key" "$val"
    done
}

# --- recording ---------------------------------------------------------------

_mdb_fp_emit_env() {   # SALT KEY VALUE
    printf 'fp env:%s %s\n' "$2" "$(mdb_fp_hash "$1" "$3")"
}

# This board's hardware serial, or nothing (not a Pi). The device tree first;
# /proc/cpuinfo's "Serial" line (same value on Pi 4B and Pi 5) only when the
# default device-tree path is missing — a test's override never falls back.
mdb_fp_board_serial() {
    if [[ -r "$MDB_BOARD_SERIAL_FILE" ]]; then
        tr -d '\0' < "$MDB_BOARD_SERIAL_FILE" 2>/dev/null | tr -d '[:space:]'
    elif [[ "$MDB_BOARD_SERIAL_FILE" == /proc/device-tree/serial-number && -r /proc/cpuinfo ]]; then
        awk -F': *' '/^Serial/ { gsub(/[[:space:]]/, "", $2); print $2; exit }' /proc/cpuinfo 2>/dev/null
    fi
    return 0
}

# mdb_fp_record OUT ENV_FILE [LABEL=PATH ...]
# Writes the fingerprint file (OUT "-" = stdout). Fingerprints only — never a
# value. An absent ENV_FILE (an unprovisioned source box) still writes a valid
# file with no `fp` lines. Returns non-zero only when nothing could be written.
mdb_fp_record() {
    local out="$1" env_file="$2"; shift 2
    local salt board spec label path val body tmp
    salt="$(mdb_fp_new_salt)"
    [[ "$salt" =~ ^[0-9a-f]{32}$ ]] || { mdb_fp_log "ERROR: could not generate a fingerprint salt"; return 1; }
    board="$(mdb_fp_board_serial)"

    body="$(
        if [[ -r "$env_file" ]]; then
            mdb_fp_each_secret _mdb_fp_emit_env "$salt" < "$env_file"
        fi
        for spec in "$@"; do
            label="${spec%%=*}"; path="${spec#*=}"
            [[ -r "$path" ]] || continue
            val="$(mdb_fp_normalize "$(cat "$path" 2>/dev/null)")"
            mdb_fp_is_placeholder "$val" && continue
            printf 'fp %s %s\n' "$label" "$(mdb_fp_hash "$salt" "$val")"
        done
    )"

    if [[ "$out" == "-" ]]; then
        _mdb_fp_render "$salt" "$board" "$body"
        return 0
    fi
    mkdir -p "$(dirname "$out")" 2>/dev/null || return 1
    tmp="${out}.tmp.$$"
    if ! _mdb_fp_render "$salt" "$board" "$body" > "$tmp" 2>/dev/null; then
        rm -f "$tmp"
        return 1
    fi
    chmod 0644 "$tmp" 2>/dev/null || true
    mv -f "$tmp" "$out"
}

_mdb_fp_render() {   # SALT BOARD BODY
    echo "# Magic Dingus Box — fingerprints of the CLONE SOURCE box's per-box secrets."
    echo "# Written by prepare_for_cloning.sh; read by verify_box.sh, which FAILs a"
    echo "# unit still using one of them (above all the source's WireGuard key: two"
    echo "# boxes on one key knock each other off the VPN). No secret is stored here:"
    echo "# each line is sha256(salt:value) cut to 128 bits. Do not delete on a unit."
    echo "version=1"
    echo "created=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "salt=$1"
    if [[ -n "$2" ]]; then
        echo "source_board=$(mdb_fp_hash "$1" "board:$2")"
    fi
    if [[ -n "$3" ]]; then
        printf '%s\n' "$3" | sort -u
    fi
}

# --- reading -----------------------------------------------------------------

# mdb_fp_field FILE NAME -> value of the NAME= header line.
mdb_fp_field() {
    [[ -r "$1" ]] || return 0
    sed -n "s/^$2=//p" "$1" 2>/dev/null | head -1
}

# Number of recorded fingerprints.
mdb_fp_count() {
    [[ -r "$1" ]] || { echo 0; return 0; }
    grep -c '^fp ' "$1" 2>/dev/null || true
}

# True when FILE's fingerprints were taken on THIS board — i.e. this is the
# clone source, and its own secrets are not "inherited" by anyone.
mdb_fp_is_source_board() {
    local file="$1" salt rec board
    salt="$(mdb_fp_field "$file" salt)"
    rec="$(mdb_fp_field "$file" source_board)"
    board="$(mdb_fp_board_serial)"
    [[ -n "$salt" && -n "$rec" && -n "$board" ]] || return 1
    [[ "$(mdb_fp_hash "$salt" "board:${board}")" == "$rec" ]]
}

# mdb_fp_match_value FILE VALUE -> prints the label(s) recorded for VALUE.
mdb_fp_match_value() {
    local file="$1" salt h
    salt="$(mdb_fp_field "$file" salt)"
    [[ -n "$salt" ]] || return 0
    mdb_fp_is_placeholder "$2" && return 0
    h="$(mdb_fp_hash "$salt" "$(mdb_fp_normalize "$2")")"
    awk -v h="$h" '$1 == "fp" && $3 == h { print $2 }' "$file" 2>/dev/null
}

_mdb_fp_match_env() {   # FILE KEY VALUE
    mdb_fp_match_value "$1" "$3"
}

# mdb_fp_scan_env FILE  (KEY=VALUE text on stdin)
# Prints, once each, the labels of recorded source secrets present in the
# text. Prints nothing when FILE is unreadable or holds no salt.
mdb_fp_scan_env() {
    local file="$1"
    [[ -n "$(mdb_fp_field "$file" salt)" ]] || { cat >/dev/null; return 0; }
    mdb_fp_each_secret _mdb_fp_match_env "$file" | sort -u
}

# --- restore ------------------------------------------------------------------

# On the SOURCE box after a clone: delete the fingerprint file, so the box
# that IS the source carries no record of being one. Only when the file names
# this board (or names no board at all — then it cannot be a clone's file
# either way we can tell): a CLONE that runs restore_after_cloning.sh (its
# image carries the clone marker until first boot wipes it) must keep it.
mdb_fp_remove_on_source() {
    local file="${1:-$MDB_SOURCE_FP_FILE}"
    [[ -e "$file" ]] || return 0
    if [[ -z "$(mdb_fp_field "$file" source_board)" ]] || mdb_fp_is_source_board "$file"; then
        rm -f "$file" && mdb_fp_log "Removed ${file} (clone-source fingerprints belong in the image, not on the source)"
    else
        mdb_fp_log "Kept ${file}: it records a different board, so this box is a CLONE of that source"
    fi
}

# --- inherited containers (first boot) ---------------------------------------

# mdb_fp_container_inherited NAME [FILE]   (container env lines on stdin)
# True when a container on a FIRST-BOOTING clone came from the source box.
# At first boot nothing has been provisioned yet, so any of these is
# inherited by definition:
#   - one of the Media Browser stack's containers (compose names them mdb_*);
#   - any container holding a WireGuard private key at all;
#   - any container holding a fingerprinted source secret.
mdb_fp_container_inherited() {
    local name="${1#/}" file="${2:-$MDB_SOURCE_FP_FILE}" env
    env="$(cat)"
    [[ "$name" == mdb_* ]] && return 0
    if printf '%s\n' "$env" | grep -qE '^[[:space:]]*WIREGUARD_PRIVATE_KEY=[^[:space:]]'; then
        return 0
    fi
    [[ -n "$(printf '%s\n' "$env" | mdb_fp_scan_env "$file")" ]]
}

# mdb_fp_docker_cfg_dump CONFIG_V2_JSON -> line 1: container name, then one
# Config.Env entry per line. Reads Docker's on-disk state without dockerd.
mdb_fp_docker_cfg_dump() {
    local cfg="$1"
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$cfg" <<'PY' 2>/dev/null && return 0
import json, sys
c = json.load(open(sys.argv[1]))
print(c.get("Name") or "")
for e in ((c.get("Config") or {}).get("Env") or []):
    print(str(e).replace("\n", " "))
PY
    fi
    # No python3: enough of a parse for the decision above (Go's JSON encoder
    # escapes none of the base64/hex characters these values use).
    grep -oE '"Name":"[^"]*"' "$cfg" 2>/dev/null | head -1 | sed -E 's/^"Name":"(.*)"$/\1/'
    grep -oE '"[A-Za-z_][A-Za-z0-9_]*=[^"]*"' "$cfg" 2>/dev/null | tr -d '"'
}

# mdb_purge_inherited_containers [FILE]
# Removes every inherited container (see mdb_fp_container_inherited) so that
# Docker's restart policy can never start gluetun on the source's key. Never
# touches images, volumes or the bind-mounted services/config tree.
#   dockerd up        -> `docker rm -f` (also clears containerd's record)
#   dockerd starting  -> wait (bounded) for it, then the same
#   dockerd down      -> delete the container's state directory directly
# Returns non-zero when something inherited could not be removed; callers
# log it loudly but carry on.
mdb_purge_inherited_containers() {
    local file="${1:-$MDB_SOURCE_FP_FILE}"
    local ids id name dir cfg dump waited=0 removed=0 failed=0
    local wait_max="${MDB_DOCKERD_WAIT_S:-60}"

    if ! command -v docker >/dev/null 2>&1 && [[ ! -d "$MDB_DOCKER_CONTAINERS_DIR" ]]; then
        mdb_fp_log "    Docker not installed — no inherited containers to remove"
        return 0
    fi

    # dockerd mid-start: never edit its state underneath it. Bounded wait.
    if ! systemctl is-active --quiet docker.service 2>/dev/null \
       && pgrep -x dockerd >/dev/null 2>&1; then
        while (( waited < wait_max )) && ! systemctl is-active --quiet docker.service 2>/dev/null; do
            sleep 2
            waited=$((waited + 2))
        done
    fi

    if systemctl is-active --quiet docker.service 2>/dev/null && command -v docker >/dev/null 2>&1; then
        if ! ids="$(mdb_fp_timeout 30 docker ps -aq --no-trunc 2>/dev/null)"; then
            mdb_fp_log "    ERROR: dockerd is up but did not list its containers"
            return 1
        fi
        for id in $ids; do
            name="$(mdb_fp_timeout 15 docker inspect -f '{{.Name}}' "$id" 2>/dev/null)"
            if mdb_fp_timeout 15 docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$id" 2>/dev/null \
                | mdb_fp_container_inherited "$name" "$file"; then
                if mdb_fp_timeout 60 docker rm -f "$id" >/dev/null 2>&1; then
                    mdb_fp_log "    removed inherited container ${name#/} (docker rm -f)"
                    removed=$((removed + 1))
                else
                    mdb_fp_log "    ERROR: could not remove inherited container ${name#/}"
                    failed=$((failed + 1))
                fi
            fi
        done
    elif pgrep -x dockerd >/dev/null 2>&1; then
        mdb_fp_log "    ERROR: dockerd is running but not active after ${waited}s —"
        mdb_fp_log "           cannot remove inherited containers safely"
        return 1
    else
        [[ -d "$MDB_DOCKER_CONTAINERS_DIR" ]] || { mdb_fp_log "    no Docker container state on disk"; return 0; }
        for dir in "$MDB_DOCKER_CONTAINERS_DIR"/*/; do
            [[ -d "$dir" ]] || continue
            cfg="${dir}config.v2.json"
            [[ -f "$cfg" ]] || continue
            # dockerd may have started since the check above (a unit with
            # Ethernet reaches network-online quickly). Stop editing at once.
            if pgrep -x dockerd >/dev/null 2>&1; then
                mdb_fp_log "    ERROR: dockerd started during the offline purge — stopped early"
                return 1
            fi
            dump="$(mdb_fp_docker_cfg_dump "$cfg")"
            name="$(printf '%s\n' "$dump" | head -1)"
            if printf '%s\n' "$dump" | tail -n +2 | mdb_fp_container_inherited "$name" "$file"; then
                if rm -rf -- "${dir%/}"; then
                    mdb_fp_log "    removed inherited container ${name#/} (offline: dockerd not running)"
                    removed=$((removed + 1))
                else
                    mdb_fp_log "    ERROR: could not remove ${dir%/}"
                    failed=$((failed + 1))
                fi
            fi
        done
    fi

    mdb_fp_log "    inherited containers removed: ${removed}, failed: ${failed}"
    (( failed == 0 ))
}

# --- CLI -----------------------------------------------------------------------
# sudo bash source_secrets_lib.sh record [OUT]
#   Print (OUT omitted or "-") or write a fingerprint file for THIS box. Used
#   to audit a unit cloned before fingerprints existed: pipe the source's
#   output into the unit's ${MDB_SOURCE_FP_FILE}, then run verify_box.sh there
#   (scripts/golden_image/CLONING.md "Is a unit using the source's VPN key?").
#   Read-only on the box it runs on when OUT is "-".
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    case "${1:-}" in
        record)
            mdb_fp_record "${2:--}" "${MDB_SERVICES_ENV:-/opt/magic_dingus_box/services/.env}" \
                "${MDB_FP_DEFAULT_EXTRAS[@]}"
            ;;
        *)
            echo "usage: $0 record [OUT|-]" >&2
            exit 2
            ;;
    esac
fi
