#!/usr/bin/env bash
# Media Browser V2 — Companion services bootstrap.
#
# Run once on the Pi after the first deploy with --media-browser.
# Idempotent: safe to re-run.
#
# What it does:
#   1. Verifies Docker + docker-compose are installed (installs if missing)
#   2. Creates /mnt/ssd storage layout
#   3. Generates .env with random secrets (if not already present)
#   4. Starts the stack for the first time
#   5. Captures the auto-generated Radarr/Prowlarr API keys
#   6. Writes them back to .env for subsequent restarts
#   7. Seeds the "1080p Standard" quality profile in Radarr
#   8. Prints credentials to stdout ONCE for the operator

set -euo pipefail

SERVICES_DIR="/opt/magic_dingus_box/services"
STORAGE_ROOT="${STORAGE_ROOT:-/mnt/ssd}"
ENV_FILE="${SERVICES_DIR}/.env"

echo "=== Media Browser V2 service setup ==="

# 1. Docker install check + group membership
# The user under which systemd runs the services unit (User=magic) must be in
# the docker group, otherwise `docker compose up` fails with EACCES on the
# Docker socket on every boot. We handle install + group membership together.
#
# Note: this script runs via `sudo`, so $(whoami) is root. We target the real
# invoking user via $SUDO_USER (set when the script is invoked with sudo).
TARGET_USER="${SUDO_USER:-magic}"

if ! command -v docker &>/dev/null; then
    echo "Installing Docker..."
    curl -fsSL https://get.docker.com | sh
    echo "Docker installed."
fi

if ! docker compose version &>/dev/null; then
    echo "ERROR: docker compose plugin required. Install docker-compose-plugin."
    exit 1
fi

# Ensure target user is in the docker group (idempotent — usermod -aG
# silently succeeds if already a member).
echo "Ensuring ${TARGET_USER} is in docker group..."
usermod -aG docker "${TARGET_USER}"

# 2. Storage layout
echo "Creating storage layout at ${STORAGE_ROOT}..."
# Radarr writes movies directly to ${STORAGE_ROOT}/library/<Title (Year)>/, no
# Movies subdirectory. The earlier setup created library/Movies/ which then sat
# empty forever — drop it.
sudo mkdir -p "${STORAGE_ROOT}"/{downloads/incomplete,downloads/complete,library,backups}
sudo chown -R "$(whoami):$(whoami)" "${STORAGE_ROOT}"

# 3. Generate .env if missing
if [ ! -f "${ENV_FILE}" ]; then
    echo "Generating ${ENV_FILE} with random secrets..."
    QBIT_PW=$(openssl rand -base64 18 | tr -d '=+/')
    cat > "${ENV_FILE}" <<EOF
PUID=$(id -u)
PGID=$(id -g)
TZ=$(timedatectl show -p Timezone --value 2>/dev/null || echo "UTC")
STORAGE_ROOT=${STORAGE_ROOT}
RADARR_API_KEY=__WILL_BE_SET_AFTER_FIRST_START__
PROWLARR_API_KEY=__WILL_BE_SET_AFTER_FIRST_START__
QBITTORRENT_ADMIN_PASSWORD=${QBIT_PW}
EOF
    chmod 600 "${ENV_FILE}"
    echo "Generated .env with random qBittorrent password."
else
    echo ".env already exists — preserving existing secrets."
fi

# 4. Start stack
cd "${SERVICES_DIR}"
echo "Starting Docker stack..."
docker compose up -d

# 5. Wait for services to initialize their configs
echo "Waiting for services to finish first-time init (60s)..."
sleep 60

# 6. Extract auto-generated API keys from Radarr/Prowlarr configs
RADARR_KEY=$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/radarr/config.xml" 2>/dev/null || echo "")
PROWLARR_KEY=$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/prowlarr/config.xml" 2>/dev/null || echo "")

if [ -z "${RADARR_KEY}" ] || [ -z "${PROWLARR_KEY}" ]; then
    echo "WARNING: Could not extract API keys. Services may still be starting."
    echo "Re-run this script in a minute, or extract them manually from config.xml files."
    exit 1
fi

# 7. Write keys back to .env
sed -i "s|RADARR_API_KEY=.*|RADARR_API_KEY=${RADARR_KEY}|" "${ENV_FILE}"
sed -i "s|PROWLARR_API_KEY=.*|PROWLARR_API_KEY=${PROWLARR_KEY}|" "${ENV_FILE}"

# 8. Verify Radarr's built-in HD-1080p profile is available.
# Radarr ships 6 built-in profiles on fresh install (Any, SD, HD-720p, HD-1080p,
# Ultra-HD, HD - 720p/1080p). We use HD-1080p (id 4) as the kiosk default rather
# than seeding a custom one — avoids schema-drift issues across Radarr versions
# (minUpgradeFormatScore requirement changed in 5.14+).
echo "Verifying Radarr built-in HD-1080p profile..."
PROFILE_JSON=$(curl -fsS -H "X-Api-Key: ${RADARR_KEY}" http://localhost:7878/api/v3/qualityprofile || echo "[]")
if echo "${PROFILE_JSON}" | grep -q '"name":"HD-1080p"'; then
    echo "  ✓ HD-1080p profile present"
else
    echo "  WARN: HD-1080p profile not found in Radarr response. Verify via web UI."
fi

# 9. Set quality profile "Any" language to "Original" (auto-adapts per
# movie) instead of the default English-only. The English-only setting
# rejects multilingual releases that include English audio because
# Radarr's title parser detects the FIRST language tag in the release
# title — for new theatrical releases that often comes back as Spanish
# / French / Italian. "Original" auto-adapts: each movie gets matched
# against its own original language. Multilingual releases satisfy
# both because they pass at least one language match.
#
# This is the live config we hit in production with Mario Galaxy 2026
# — every release from a 14-result search was rejected with "English
# is wanted, but found Spanish" until the profile was relaxed.
echo "Setting 'Any' quality profile language to 'Original' (auto-adapts per movie)..."
ANY_PROFILE=$(curl -fsS -H "X-Api-Key: ${RADARR_KEY}" \
    http://localhost:7878/api/v3/qualityprofile 2>/dev/null \
    | python3 -c "import sys,json
ps = json.load(sys.stdin)
p = next((p for p in ps if p['name']=='Any'), None)
if p:
    p['language'] = {'id':-2, 'name':'Original'}
    print(json.dumps(p))
else:
    print('')")
if [[ -n "${ANY_PROFILE}" ]]; then
    PROFILE_ID=$(echo "${ANY_PROFILE}" | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
    curl -fsS -X PUT -H "X-Api-Key: ${RADARR_KEY}" -H "Content-Type: application/json" \
        -d "${ANY_PROFILE}" \
        "http://localhost:7878/api/v3/qualityprofile/${PROFILE_ID}" >/dev/null \
        && echo "  ✓ 'Any' profile language set to Original" \
        || echo "  WARN: failed to update 'Any' profile language; verify via web UI"
fi

# 10. Codify Radarr Custom Formats + Any-profile score map.
#
# The kiosk's Pi 4 hardware can only smoothly decode H.264 in the
# 720p-1080p range. Anything else (AV1, HEVC at 1080p, HDR, Remux)
# either has no hardware decoder or blows the size budget. We enforce
# the codec/quality choice with a 3-layer filter and the middle layer
# is a Custom Format score map applied to the "Any" quality profile:
# every grab must net at least minFormatScore=-200 across the six
# formats below.
#
# These formats and scores were originally crafted by hand in the
# Radarr UI, which left them at risk of silent drift — we caught HEVC
# scoring slipping from -250 to -100 in production, which let HEVC
# files slip through the score floor and get downloaded. Codifying the
# spec in scripts/data/radarr_custom_formats.json + reapplying it on
# every setup run eliminates that whole class of drift.
#
# Match-by-name (NOT by id): the JSON file's "id" is just a hint for
# humans diffing changes. Radarr assigns ids server-side and they can
# vary across deploys, so the script always looks up the live id by
# matching the CF "name" field.
#
# Idempotency: a CF with matching name+specifications is left alone;
# a CF with the right name but stale specifications gets PUT'd; a
# missing CF gets POSTed. The Any profile's formatItems are only PUT
# back when at least one score (or minFormatScore) actually drifted.
echo "Configuring Radarr Custom Formats + 'Any' profile score map..."
CF_DATA_FILE="$(dirname "$0")/data/radarr_custom_formats.json"
if [[ ! -f "${CF_DATA_FILE}" ]]; then
    echo "  WARN: ${CF_DATA_FILE} not found — skipping. Custom Formats may already be configured manually; verify via web UI."
else
    # Run the full GET → match → POST/PUT for each CF, then PATCH the
    # Any profile's formatItems, all in one python heredoc so we can
    # share state (the live CF list, name→id map, drift counters)
    # without round-tripping through bash variables.
    CF_SUMMARY=$(python3 - "${CF_DATA_FILE}" "${RADARR_KEY}" <<'PYEOF'
import json, sys, urllib.request, urllib.error

cf_data_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:7878/api/v3"

# Desired Any-profile score map. Keep this in lockstep with the
# CLAUDE.md "Quality configuration" section — it's the source of truth
# the script enforces, mirrored on disk for human review.
SCORE_MAP = {
    "AV1 codec (UNWATCHABLE on Pi 4)": -1000,
    "x265/HEVC 1080p+":                -250,
    "HDR / Dolby Vision":              -200,
    "Remux / Raw-HD":                  -500,
    "x264 codec (BONUS)":               50,
    "Trusted small-release groups":     30,
}
MIN_FORMAT_SCORE = -200

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def cf_specs_match(live, desired):
    # We compare only the fields we care about (name, regex value,
    # negate, required, includeCustomFormatWhenRenaming). Radarr adds
    # server-side fields like "id" we must ignore.
    if live.get("name") != desired.get("name"): return False
    if live.get("includeCustomFormatWhenRenaming") != desired.get("includeCustomFormatWhenRenaming"): return False
    ls, ds = live.get("specifications", []), desired.get("specifications", [])
    if len(ls) != len(ds): return False
    for a, b in zip(ls, ds):
        if a.get("name") != b.get("name"): return False
        if a.get("implementation") != b.get("implementation"): return False
        if bool(a.get("negate")) != bool(b.get("negate")): return False
        if bool(a.get("required")) != bool(b.get("required")): return False
        # Compare regex value (the only field that ever drifts).
        av = next((f.get("value") for f in a.get("fields", []) if f.get("name") == "value"), None)
        bv = next((f.get("value") for f in b.get("fields", []) if f.get("name") == "value"), None)
        if av != bv: return False
    return True

with open(cf_data_path) as f:
    desired_cfs = json.load(f)

live_cfs = http("GET", "/customformat")
live_by_name = {c["name"]: c for c in live_cfs}

created, updated, unchanged = [], [], []
name_to_id = {}

for desired in desired_cfs:
    name = desired["name"]
    # Strip the spec-file's "id" so it doesn't conflict with whatever
    # Radarr assigns server-side.
    payload = {k: v for k, v in desired.items() if k != "id"}
    if name in live_by_name:
        live = live_by_name[name]
        if cf_specs_match(live, payload):
            unchanged.append(name)
            name_to_id[name] = live["id"]
        else:
            payload["id"] = live["id"]
            result = http("PUT", "/customformat/%d" % live["id"], payload)
            updated.append(name)
            name_to_id[name] = result["id"]
    else:
        result = http("POST", "/customformat", payload)
        created.append(name)
        name_to_id[name] = result["id"]

# Now reconcile the Any profile's formatItems. Radarr exposes one
# formatItems entry per CF (auto-synced); we just adjust scores.
profiles = http("GET", "/qualityprofile")
any_profile = next((p for p in profiles if p["name"] == "Any"), None)
profile_changed = False
score_changes = []

if any_profile is None:
    print("WARN: 'Any' profile missing; cannot apply score map", file=sys.stderr)
else:
    if any_profile.get("minFormatScore") != MIN_FORMAT_SCORE:
        any_profile["minFormatScore"] = MIN_FORMAT_SCORE
        profile_changed = True
        score_changes.append("minFormatScore=%d" % MIN_FORMAT_SCORE)
    for fi in any_profile.get("formatItems", []):
        fname = fi.get("name")
        if fname in SCORE_MAP:
            want = SCORE_MAP[fname]
            if fi.get("score") != want:
                fi["score"] = want
                profile_changed = True
                score_changes.append("%s=%+d" % (fname, want))
    if profile_changed:
        http("PUT", "/qualityprofile/%d" % any_profile["id"], any_profile)

print(json.dumps({
    "created": created,
    "updated": updated,
    "unchanged": unchanged,
    "profile_changed": profile_changed,
    "score_changes": score_changes,
}))
PYEOF
)
    # Pretty-print the summary for the operator. The python block emits
    # a single JSON line on stdout; we feed it back via stdin to avoid
    # any shell-quoting issues with embedded double quotes.
    printf '%s' "${CF_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
if s["profile_changed"]:
    print("  ✓ Any profile score map updated: " + ", ".join(s["score_changes"]))
else:
    print("  ✓ Any profile score map already matches desired state")
'
fi

# 9. Print credentials to operator
cat <<EOF

======================================================================
Services initialized. SAVE THESE CREDENTIALS in a password manager:

All service UIs are bound to 127.0.0.1 on this Pi (zero LAN attack
surface). Admin access requires an SSH tunnel from a trusted device:

  ssh -L 7878:localhost:7878 \\
      -L 9696:localhost:9696 \\
      -L 8080:localhost:8080 \\
      -L 8191:localhost:8191 \\
      magic@magicpi.local

Then from that device:

Radarr    → http://localhost:7878 (via SSH tunnel only — see operator guide)
            API key: ${RADARR_KEY}

Prowlarr  → http://localhost:9696 (via SSH tunnel only — see operator guide)
            API key: ${PROWLARR_KEY}

qBittorrent → http://localhost:8080 (via SSH tunnel only — see operator guide)
            Username: admin
            Password: $(grep QBITTORRENT_ADMIN_PASSWORD "${ENV_FILE}" | cut -d= -f2)
            (Log in and change the default via qBit web UI immediately)

NEXT STEPS:
  1. Open SSH tunnel (above), log into qBittorrent, change admin password
  2. In Prowlarr: add at least one indexer (legal content only)
  3. In Radarr: connect to Prowlarr (auto-discovered) and qBittorrent
  4. Kiosk Media Browser will now talk to Radarr once ENABLE_MEDIA_BROWSER is on
======================================================================
EOF
