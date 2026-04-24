#!/usr/bin/env bash
#
# configure_prowlarr_indexers.sh
#
# Idempotently configure the top public torrent indexers in Prowlarr
# for the Magic Dingus Box Media Browser feature.
#
# Indexers configured (all public, all built into Prowlarr's schema registry):
#   - 1337x
#   - The Pirate Bay
#   - TorrentGalaxy (TGx)
#   - YTS
#   - LimeTorrents
#   - BT4G
#
# Usage:
#   sudo ./configure_prowlarr_indexers.sh
#
# Environment:
#   PROWLARR_API_KEY   Overrides the key read from /opt/magic_dingus_box/services/.env
#   PROWLARR_URL       Defaults to http://localhost:9696
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
ENV_FILE="/opt/magic_dingus_box/services/.env"
PROWLARR_URL="${PROWLARR_URL:-http://localhost:9696}"

# Target indexers. Each entry is a pipe-separated pair:
#   <display-label>|<regex that must match the schema 'name' field, case-insensitive>
# The regex lets us tolerate slight differences in Prowlarr's schema naming
# (e.g. "1337x", "The Pirate Bay", "TorrentGalaxy").
TARGETS=(
    "1337x|^1337x$"
    "The Pirate Bay|^The Pirate Bay$"
    "TorrentGalaxy|^TorrentGalaxy(Clone)?$"
    "YTS|^YTS$"
    "LimeTorrents|^LimeTorrents$"
    "BT4G|^BT4G$"
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
warn() { printf '[%s] WARN: %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
err()  { printf '[%s] ERROR: %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

die() { err "$*"; exit 1; }

# Load API key.
if [[ -z "${PROWLARR_API_KEY:-}" ]]; then
    if [[ -f "$ENV_FILE" ]]; then
        # Extract PROWLARR_API_KEY=... from the env file without sourcing it
        # (the file may contain chars we don't want to eval).
        PROWLARR_API_KEY="$(grep -E '^PROWLARR_API_KEY=' "$ENV_FILE" | head -n1 | cut -d= -f2- | tr -d '"'\''[:space:]')"
    fi
fi
if [[ -z "${PROWLARR_API_KEY:-}" ]]; then
    die "PROWLARR_API_KEY not set and not found in $ENV_FILE"
fi

# Check Prowlarr reachability. /ping is unauthenticated.
log "Pinging Prowlarr at $PROWLARR_URL ..."
if ! curl -fsS --max-time 10 "$PROWLARR_URL/ping" >/dev/null; then
    die "Prowlarr is not reachable at $PROWLARR_URL/ping"
fi
log "Prowlarr is up."

# Wrapper: curl with the API key header.
api() {
    local method="$1"; shift
    local path="$1"; shift
    curl -sS -X "$method" \
        -H "X-Api-Key: $PROWLARR_API_KEY" \
        -H "Content-Type: application/json" \
        -H "Accept: application/json" \
        "$PROWLARR_URL$path" "$@"
}

# api_with_status: emits "HTTP_CODE<newline>BODY". Used where we need the status.
api_with_status() {
    local method="$1"; shift
    local path="$1"; shift
    curl -sS -o /tmp/prowlarr_resp.$$ -w '%{http_code}' -X "$method" \
        -H "X-Api-Key: $PROWLARR_API_KEY" \
        -H "Content-Type: application/json" \
        -H "Accept: application/json" \
        "$PROWLARR_URL$path" "$@"
    local code=$?
    local status
    status=$(tail -c 4 /tmp/prowlarr_resp.$$ 2>/dev/null || echo "000")
    cat /tmp/prowlarr_resp.$$ 2>/dev/null || true
    rm -f /tmp/prowlarr_resp.$$
    return $code
}

# ---------------------------------------------------------------------------
# Fetch existing indexers + full schema once (single source of truth).
# ---------------------------------------------------------------------------
EXISTING_FILE="$(mktemp)"
SCHEMA_FILE="$(mktemp)"
trap 'rm -f "$EXISTING_FILE" "$SCHEMA_FILE"' EXIT

log "Fetching current indexer list ..."
api GET /api/v1/indexer > "$EXISTING_FILE" || die "Failed to GET /api/v1/indexer"

log "Fetching indexer schema (this can take a moment, ~800 schemas) ..."
api GET /api/v1/indexer/schema > "$SCHEMA_FILE" || die "Failed to GET /api/v1/indexer/schema"

# Determine the default AppProfile id. Prowlarr requires indexers to be
# associated with at least one appProfileId > 0. We use the first available.
log "Fetching app profile id ..."
APP_PROFILE_ID="$(api GET /api/v1/appprofile | python3 -c '
import json, sys
data = json.load(sys.stdin)
if not data:
    print("")
    sys.exit(0)
print(data[0]["id"])
')"
if [[ -z "${APP_PROFILE_ID:-}" || "${APP_PROFILE_ID}" == "0" ]]; then
    die "No AppProfile found in Prowlarr (expected at least one, e.g. Standard)"
fi
log "Using AppProfileId=$APP_PROFILE_ID"

# ---------------------------------------------------------------------------
# Python helper: given the schema JSON, the existing JSON, and a target regex,
# produce either:
#   - "SKIP\t<name>"    if the indexer is already configured
#   - "MISSING\t<label>" if no schema entry matches the regex
#   - "ADD\t<payload-json>\t<name>" if we should POST the schema
# ---------------------------------------------------------------------------
plan_indexer() {
    local label="$1"
    local pattern="$2"
    EXISTING_FILE="$EXISTING_FILE" SCHEMA_FILE="$SCHEMA_FILE" \
    LABEL="$label" PATTERN="$pattern" APP_PROFILE_ID="$APP_PROFILE_ID" python3 <<'PY'
import json
import os
import re
import sys

with open(os.environ["EXISTING_FILE"]) as f:
    existing = json.load(f)
with open(os.environ["SCHEMA_FILE"]) as f:
    schema = json.load(f)
label    = os.environ["LABEL"]
pattern  = re.compile(os.environ["PATTERN"], re.IGNORECASE)

# Already configured?
for idx in existing:
    name = idx.get("name", "")
    defn = idx.get("definitionName", "")
    if pattern.fullmatch(name) or pattern.fullmatch(defn):
        print(f"SKIP\t{name}")
        sys.exit(0)

# Find matching schema entry. Match on 'name' first, then 'definitionName'.
match = None
for entry in schema:
    if pattern.fullmatch(entry.get("name", "")):
        match = entry
        break
if match is None:
    for entry in schema:
        if pattern.fullmatch(entry.get("definitionName", "") or ""):
            match = entry
            break

if match is None:
    print(f"MISSING\t{label}")
    sys.exit(0)

# Build the POST body. The schema entry is already a full indexer
# definition; we just need to tag it for persistence.
payload = dict(match)
payload["enable"]        = True
payload["priority"]      = 25
payload["tags"]          = []
payload["appProfileId"]  = int(os.environ["APP_PROFILE_ID"])
# Some Prowlarr versions expect these to be absent/null for creation.
payload.pop("id", None)

# Ensure fields defaults are preserved (Prowlarr does this, but be explicit).
if "fields" in payload and isinstance(payload["fields"], list):
    for f in payload["fields"]:
        if "value" not in f and "defaultValue" in f:
            f["value"] = f["defaultValue"]

print(f"ADD\t{json.dumps(payload)}\t{payload.get('name', label)}")
PY
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
CONFIGURED=0
TESTED_OK=0
FAILURES=0
SKIPPED=0

for target in "${TARGETS[@]}"; do
    label="${target%%|*}"
    pattern="${target#*|}"
    log "--- $label ---"

    plan="$(plan_indexer "$label" "$pattern" || true)"
    action="${plan%%$'\t'*}"
    rest="${plan#*$'\t'}"

    case "$action" in
        SKIP)
            log "  already configured as '$rest' - skipping"
            SKIPPED=$((SKIPPED + 1))
            CONFIGURED=$((CONFIGURED + 1))   # count toward 'present' tally
            ;;
        MISSING)
            warn "  no matching schema entry for '$label' - skipping"
            FAILURES=$((FAILURES + 1))
            ;;
        ADD)
            payload="${rest%%$'\t'*}"
            name="${rest#*$'\t'}"
            log "  POSTing schema for '$name' ..."
            resp_file="$(mktemp)"
            status="$(curl -sS -o "$resp_file" -w '%{http_code}' \
                -X POST \
                -H "X-Api-Key: $PROWLARR_API_KEY" \
                -H "Content-Type: application/json" \
                --data "$payload" \
                "$PROWLARR_URL/api/v1/indexer" || echo "000")"

            # Prowlarr runs an implicit connectivity test on creation. Public
            # trackers often fail a one-shot test (CloudFlare, redirects) but
            # work fine for real search traffic. If the create failed with a
            # connectivity-style error, retry with forceSave=true so the
            # indexer is persisted and the user can decide to keep it.
            if [[ ! "$status" =~ ^2 ]] && grep -qE 'CloudFlare|Redirected|Unable to (access|connect)' "$resp_file"; then
                log "  connectivity pre-check failed; retrying with forceSave=true"
                status="$(curl -sS -o "$resp_file" -w '%{http_code}' \
                    -X POST \
                    -H "X-Api-Key: $PROWLARR_API_KEY" \
                    -H "Content-Type: application/json" \
                    --data "$payload" \
                    "$PROWLARR_URL/api/v1/indexer?forceSave=true" || echo "000")"
            fi

            if [[ "$status" =~ ^2 ]]; then
                log "  added '$name' (HTTP $status)"
                CONFIGURED=$((CONFIGURED + 1))

                # Re-read body for the test payload (server-returned body includes the new id).
                created_body="$(cat "$resp_file")"
                log "  testing '$name' ..."
                test_status="$(curl -sS -o /tmp/prowlarr_test.$$ -w '%{http_code}' \
                    -X POST \
                    -H "X-Api-Key: $PROWLARR_API_KEY" \
                    -H "Content-Type: application/json" \
                    --data "$created_body" \
                    "$PROWLARR_URL/api/v1/indexer/test" || echo "000")"
                test_body="$(cat /tmp/prowlarr_test.$$ 2>/dev/null || echo '')"
                rm -f /tmp/prowlarr_test.$$

                if [[ "$test_status" =~ ^2 ]]; then
                    log "  test OK (HTTP $test_status)"
                    TESTED_OK=$((TESTED_OK + 1))
                else
                    warn "  test FAILED (HTTP $test_status): $(echo "$test_body" | head -c 300)"
                fi
            else
                err "  POST failed (HTTP $status): $(head -c 500 "$resp_file")"
                FAILURES=$((FAILURES + 1))
            fi
            rm -f "$resp_file"
            ;;
        *)
            warn "  unexpected planner output: $plan"
            FAILURES=$((FAILURES + 1))
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Post-pass: test indexers that already existed (skipped above).
# This gives us a meaningful TESTED_OK count for skip-path runs.
# ---------------------------------------------------------------------------
if (( SKIPPED > 0 )); then
    log "Running connectivity tests on pre-existing indexers ..."
    for target in "${TARGETS[@]}"; do
        label="${target%%|*}"
        pattern="${target#*|}"
        existing_match="$(EXISTING_FILE="$EXISTING_FILE" PATTERN="$pattern" python3 <<'PY'
import json, os, re, sys
with open(os.environ["EXISTING_FILE"]) as f:
    existing = json.load(f)
pat = re.compile(os.environ["PATTERN"], re.IGNORECASE)
for idx in existing:
    if pat.fullmatch(idx.get("name", "") or "") or pat.fullmatch(idx.get("definitionName", "") or ""):
        print(json.dumps(idx))
        sys.exit(0)
PY
)"
        if [[ -z "$existing_match" ]]; then
            continue
        fi
        log "  testing pre-existing '$label' ..."
        test_status="$(curl -sS -o /tmp/prowlarr_test.$$ -w '%{http_code}' \
            -X POST \
            -H "X-Api-Key: $PROWLARR_API_KEY" \
            -H "Content-Type: application/json" \
            --data "$existing_match" \
            "$PROWLARR_URL/api/v1/indexer/test" || echo "000")"
        test_body="$(cat /tmp/prowlarr_test.$$ 2>/dev/null || echo '')"
        rm -f /tmp/prowlarr_test.$$
        if [[ "$test_status" =~ ^2 ]]; then
            log "  test OK (HTTP $test_status)"
            TESTED_OK=$((TESTED_OK + 1))
        else
            warn "  test FAILED (HTTP $test_status): $(echo "$test_body" | head -c 300)"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
log "========================================"
log "Prowlarr indexer configuration summary"
log "  Indexers present : $CONFIGURED / ${#TARGETS[@]}"
log "  Tested OK        : $TESTED_OK"
log "  Failures         : $FAILURES"
log "========================================"

# Exit 0 even on partial failures (per spec: 'don't fail the whole script
# if one indexer's test fails'). Non-zero only if every indexer failed.
if (( CONFIGURED == 0 )); then
    exit 1
fi
exit 0
