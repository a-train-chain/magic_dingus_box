#!/usr/bin/env bash
#
# Magic Dingus Box - Docker container log rotation
#
# Merges json-file rotation (log-opts max-size / max-file) into Docker's
# daemon.json. Idempotent and NON-clobbering: every other key already in
# the file is kept, and the file is only rewritten when the result differs.
#
# Why: Docker's default json-file driver never rotates. The Radarr/Sonarr/
# Prowlarr/qBittorrent/Gluetun containers log continuously, so
# /var/lib/docker/containers/*/*-json.log grows without bound on a 32 GB SD
# card — and those logs are also the operator's activity record (search
# terms, titles, VPN session detail) that the golden-image scrub has to
# chase down.
#
# IMPORTANT: daemon.json log-opts apply only to containers CREATED after
# dockerd restarts with them. Existing containers keep the options they were
# created with until they are recreated (`docker compose up -d
# --force-recreate`, or `compose down` + `up -d`). The caller restarts dockerd
# when this script reports "changed".
#
# Usage: configure_docker_logging.sh [path-to-daemon.json]
#        (default /etc/docker/daemon.json)
# Stdout: "changed" or "unchanged".
# Exit:   0 ok, 1 error (existing file is not valid JSON — left untouched;
#         jq missing).

set -euo pipefail

DAEMON_JSON="${1:-/etc/docker/daemon.json}"
MAX_SIZE="${MDB_DOCKER_LOG_MAX_SIZE:-10m}"
MAX_FILE="${MDB_DOCKER_LOG_MAX_FILE:-3}"

if ! command -v jq >/dev/null 2>&1; then
    echo "configure_docker_logging: jq is required" >&2
    exit 1
fi

current='{}'
if [[ -s "$DAEMON_JSON" ]]; then
    # Never clobber a file we cannot parse — an operator's hand edit with a
    # trailing comma would otherwise be silently replaced, losing every
    # setting in it.
    if ! current="$(jq -e 'if type == "object" then . else error("not an object") end' \
                        "$DAEMON_JSON" 2>/dev/null)"; then
        echo "configure_docker_logging: ${DAEMON_JSON} is not a JSON object; not modifying it" >&2
        exit 1
    fi
fi

# Only the json-file driver (explicit, or implied by absence) gets the
# rotation opts. An operator who picked another driver (journald, local —
# which rotates on its own) keeps it untouched. Our two opts are CONVERGED,
# not just added-if-missing: a stale max-size from an earlier value is the
# "present but not correct" trap.
desired="$(printf '%s' "$current" | jq --arg ms "$MAX_SIZE" --arg mf "$MAX_FILE" '
    if ((.["log-driver"] // "json-file") == "json-file") then
        .["log-driver"] = "json-file"
        | .["log-opts"] = ((.["log-opts"] // {}) + {"max-size": $ms, "max-file": $mf})
    else . end')"

if [[ -s "$DAEMON_JSON" ]] \
   && [[ "$(printf '%s' "$current" | jq -S .)" == "$(printf '%s' "$desired" | jq -S .)" ]]; then
    echo unchanged
    exit 0
fi

mkdir -p "$(dirname "$DAEMON_JSON")"
tmp="$(mktemp "$(dirname "$DAEMON_JSON")/.daemon.json.XXXXXX")"
printf '%s\n' "$desired" > "$tmp"
chmod 0644 "$tmp"
mv -f "$tmp" "$DAEMON_JSON"
echo changed
