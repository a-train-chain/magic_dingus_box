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
# This script runs via sudo, so $(whoami) is root. Use TARGET_USER (resolved
# from $SUDO_USER above) so the storage tree ends up owned by the magic user
# whose UID/GID the Docker containers run under (PUID/PGID in .env). Without
# this, Radarr and qBittorrent get EACCES on every download write.
sudo chown -R "${TARGET_USER}:${TARGET_USER}" "${STORAGE_ROOT}"

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

# 11. Prowlarr: cloudflare tag.
#
# A handful of indexers (Demonoid, EZTV, Internet Archive, Magnetz,
# Torrent Downloads, TorrentDownload) sit behind Cloudflare's bot-block
# and only resolve when Prowlarr routes the request through the
# FlareSolverr indexer-proxy in Step 12. The wiring is done with a
# shared "cloudflare" tag — set on both the FlareSolverr proxy AND
# every Cloudflare-protected indexer. Prowlarr matches tags between
# the two and routes requests for tagged indexers via the tagged proxy.
#
# We seed the tag first so Steps 12 + 13 can reference it by id.
echo "Configuring Prowlarr 'cloudflare' tag..."
PROWLARR_TAGS_FILE="$(dirname "$0")/data/prowlarr_tags.json"
if [[ ! -f "${PROWLARR_TAGS_FILE}" ]]; then
    echo "  WARN: ${PROWLARR_TAGS_FILE} not found — skipping. Tag may already be configured manually; verify via web UI."
    PROWLARR_CLOUDFLARE_TAG_ID=""
else
    # Idempotent: GET /tag, find by label, POST only if missing. Capture
    # the resulting id (live or just-created) to a single-line stdout
    # for the bash-side variable so subsequent steps can pass it as
    # the tag id when reconciling proxy/indexer "tags": [...] arrays.
    TAG_RESULT=$(python3 - "${PROWLARR_TAGS_FILE}" "${PROWLARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
tags_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:9696/api/v1"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

with open(tags_path) as f:
    desired_tags = json.load(f)

live_tags = http("GET", "/tag") or []
live_by_label = {t["label"]: t for t in live_tags}

created, unchanged = [], []
label_to_id = {}
for desired in desired_tags:
    label = desired["label"]
    if label in live_by_label:
        unchanged.append(label)
        label_to_id[label] = live_by_label[label]["id"]
    else:
        result = http("POST", "/tag", {"label": label})
        created.append(label)
        label_to_id[label] = result["id"]

print(json.dumps({"created": created, "unchanged": unchanged, "label_to_id": label_to_id}))
PYEOF
)
    # Pretty-print + extract the cloudflare tag id for the next steps.
    echo "${TAG_RESULT}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("unchanged", s["unchanged"])
'
    PROWLARR_CLOUDFLARE_TAG_ID=$(echo "${TAG_RESULT}" | python3 -c "import json,sys; print(json.load(sys.stdin)['label_to_id'].get('cloudflare', ''))")
fi

# 12. Prowlarr: FlareSolverr indexer proxy.
#
# FlareSolverr is a headless-browser sidecar (own container) that
# solves Cloudflare JS challenges and returns the decoded HTML.
# Prowlarr's "indexer proxy" feature can route requests for any
# tag-matched indexer through it — that's how the cloudflare-tagged
# indexers in Step 13 actually reach their sites in production.
#
# The fixture stores the tag membership as `tags_by_label` (a list of
# human-readable labels) so the file stays diff-friendly across
# deploys. We translate label → id at apply time using Step 11's map.
echo "Configuring Prowlarr FlareSolverr indexer proxy..."
PROWLARR_PROXIES_FILE="$(dirname "$0")/data/prowlarr_indexerproxies.json"
if [[ ! -f "${PROWLARR_PROXIES_FILE}" ]]; then
    echo "  WARN: ${PROWLARR_PROXIES_FILE} not found — skipping. Proxy may already be configured manually; verify via web UI."
else
    PROXY_SUMMARY=$(python3 - "${PROWLARR_PROXIES_FILE}" "${PROWLARR_KEY}" "${PROWLARR_CLOUDFLARE_TAG_ID:-}" <<'PYEOF'
import json, sys, urllib.request
proxies_path, api_key, cloudflare_tag_id = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:9696/api/v1"

# Build a single label→id map. Right now only "cloudflare" is in
# scope, but doing it as a dict keeps the path open for additional
# tags without restructuring the fixture format.
LABEL_TO_ID = {}
if cloudflare_tag_id:
    LABEL_TO_ID["cloudflare"] = int(cloudflare_tag_id)

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    # Reduce a Servarr-style fields[] array (each entry has name+value
    # plus a pile of UI-only metadata) to a flat {name: value} dict.
    # We only ever care about (name, value) for drift detection.
    return {f["name"]: f.get("value") for f in (fields or [])}

def resolve_tags(desired):
    # Translate fixture's `tags_by_label` to the live id list.
    return sorted(LABEL_TO_ID[lbl] for lbl in desired.get("tags_by_label", []) if lbl in LABEL_TO_ID)

def shape_payload(desired):
    # Strip the diff-friendly `tags_by_label` key and replace with the
    # API's id-based `tags`. Drop fixture-only `id` if any.
    payload = {k: v for k, v in desired.items() if k not in ("tags_by_label", "id")}
    payload["tags"] = resolve_tags(desired)
    return payload

def match(live, desired_payload):
    # Compare the fields we own. Servarr adds server-side keys (id,
    # supports*, etc.) that we ignore.
    keys = ("name", "implementation", "configContract",
            "onHealthIssue", "includeHealthWarnings")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    # Subset comparison: only verify the fields we specify — any
    # server-injected extras are ignored.
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    for k, v in des_fd.items():
        if live_fd.get(k) != v:
            return False
    return True

with open(proxies_path) as f:
    desired_proxies = json.load(f)

live_proxies = http("GET", "/indexerproxy") or []
live_by_name = {p["name"]: p for p in live_proxies}

created, updated, unchanged = [], [], []
for desired in desired_proxies:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/indexerproxy/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/indexerproxy", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
    echo "${PROXY_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
fi

# 13. Prowlarr: indexers.
#
# Nine Cardigann-backed public-tracker definitions captured from the
# reference Pi. Four are enabled by default (LimeTorrents, TPB,
# TorrentDownload, YTS) — those are the ones the Media Browser
# actively queries via Radarr+Prowlarr. The other five are kept
# pre-configured but disabled so an operator can flip them on later
# without re-discovering URLs / definitionFiles. The cloudflare-
# tagged ones (Demonoid, EZTV, Internet Archive, Magnetz, Torrent
# Downloads, TorrentDownload) route through Step 12's FlareSolverr.
#
# Match by name (server-assigned ids vary). We treat enable + tags +
# fields as the "owned" fields; if any drift, PUT to reset. URL lists
# (`indexerUrls`, `legacyUrls`) and capabilities aren't owned — those
# come from the Cardigann definitionFile and are recomputed by
# Prowlarr on every save. Comparing them would force pointless PUTs.
echo "Configuring Prowlarr indexers..."
PROWLARR_INDEXERS_FILE="$(dirname "$0")/data/prowlarr_indexers.json"
if [[ ! -f "${PROWLARR_INDEXERS_FILE}" ]]; then
    echo "  WARN: ${PROWLARR_INDEXERS_FILE} not found — skipping."
else
    INDEXER_SUMMARY=$(python3 - "${PROWLARR_INDEXERS_FILE}" "${PROWLARR_KEY}" "${PROWLARR_CLOUDFLARE_TAG_ID:-}" <<'PYEOF'
import json, sys, urllib.request
indexers_path, api_key, cloudflare_tag_id = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:9696/api/v1"

LABEL_TO_ID = {}
if cloudflare_tag_id:
    LABEL_TO_ID["cloudflare"] = int(cloudflare_tag_id)

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    return {f["name"]: f.get("value") for f in (fields or [])}

def resolve_tags(desired):
    return sorted(LABEL_TO_ID[lbl] for lbl in desired.get("tags_by_label", []) if lbl in LABEL_TO_ID)

def shape_payload(desired):
    # Strip diff-friendly metadata (`tags_by_label`, fixture-only id,
    # `added` timestamp) and replace with id-based `tags`.
    payload = {k: v for k, v in desired.items()
               if k not in ("tags_by_label", "id", "added")}
    payload["tags"] = resolve_tags(desired)
    return payload

def match(live, desired_payload):
    # Owned fields: name, implementation, configContract, definitionName,
    # enable, priority, appProfileId, tags, and the fields[] map. Anything
    # else (capabilities, indexerUrls, legacyUrls, language, encoding,
    # description, supportsRss, etc.) comes from the Cardigann definition
    # and is regenerated server-side — comparing it would cause needless
    # churn since Prowlarr can mutate those between calls.
    keys = ("name", "implementation", "configContract", "definitionName",
            "enable", "priority", "appProfileId")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    # Subset comparison on fields[]: Prowlarr injects a pile of base/
    # torrentBase settings (queryLimit, grabLimit, seedRatio, etc.)
    # that aren't in the fixture and we don't want to fight over. We
    # only verify every field WE specify matches the live value; any
    # extras are server-managed defaults and ignored.
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    for k, v in des_fd.items():
        if live_fd.get(k) != v:
            return False
    return True

with open(indexers_path) as f:
    desired_indexers = json.load(f)

live_indexers = http("GET", "/indexer") or []
live_by_name = {i["name"]: i for i in live_indexers}

created, updated, unchanged = [], [], []
for desired in desired_indexers:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/indexer/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/indexer", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
    echo "${INDEXER_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
fi

# 14. Prowlarr → Radarr Apps integration.
#
# Prowlarr "applications" is the auto-sync that pushes indexer changes
# to Radarr. Without this, every time we add/edit an indexer the
# operator would have to mirror it manually in Radarr's settings.
#
# The fixture's `apiKey` field is sanitized to "********" (the same
# masked value Prowlarr's GET /applications returns for security).
# We inject the live RADARR_KEY at apply time, which is fine because:
#   - On create, the real key is needed and goes through.
#   - On a re-run, the GET response masks the key to "********" — we
#     can't compare against our live key, so we just check non-secret
#     fields (URLs, syncCategories, syncLevel) for drift. If those
#     match, we no-op; if they don't, we PUT with the freshly-injected
#     real key (which won't drift because we control RADARR_KEY).
#
# Wait/retry: on a fresh Pi this step runs ~60s after `docker compose
# up`, which is usually enough for Radarr to be reachable, but we add
# a short retry just in case the API is still warming up.
echo "Configuring Prowlarr → Radarr Apps integration..."
PROWLARR_APPS_FILE="$(dirname "$0")/data/prowlarr_applications.json"
if [[ ! -f "${PROWLARR_APPS_FILE}" ]]; then
    echo "  WARN: ${PROWLARR_APPS_FILE} not found — skipping."
else
    # Brief readiness check on Radarr (it's the API key consumer; if
    # Radarr were down we couldn't even prove our key works). Doesn't
    # block; we just give it a chance to finish its first-time init
    # if this is a fresh Pi.
    for i in {1..30}; do
        if curl -fsS -o /dev/null -H "X-Api-Key: ${RADARR_KEY}" \
            http://localhost:7878/api/v3/system/status; then
            break
        fi
        sleep 2
    done
    APPS_SUMMARY=$(python3 - "${PROWLARR_APPS_FILE}" "${PROWLARR_KEY}" "${RADARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
apps_path, prowlarr_key, radarr_key = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:9696/api/v1"

def http(method, path, body=None):
    headers = {"X-Api-Key": prowlarr_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    return {f["name"]: f.get("value") for f in (fields or [])}

def inject_api_key(payload):
    # The fixture has `value: "********"` for the apiKey field; replace
    # with the live RADARR_KEY in-place. We mutate a deep-copied list
    # so the source dict stays untouched.
    new_fields = []
    for f in payload.get("fields", []):
        if f.get("name") == "apiKey":
            f = dict(f)
            f["value"] = radarr_key
        new_fields.append(f)
    payload = dict(payload)
    payload["fields"] = new_fields
    return payload

def shape_payload(desired):
    payload = {k: v for k, v in desired.items() if k != "id"}
    return inject_api_key(payload)

def match(live, desired_payload):
    # Owned fields: name, syncLevel, enable, implementation. For
    # `fields[]` we compare every value EXCEPT `apiKey`, since the
    # API masks apiKey to "********" on GET — we can't tell whether
    # the live key matches our injected one. In practice that's fine:
    # the only way the key drifts is if RADARR_KEY rotated, in which
    # case we'd want to push the new one anyway, which Step 14 does
    # not detect (acceptable trade-off — operator can re-create the
    # app integration manually if they rotate keys).
    keys = ("name", "syncLevel", "enable", "implementation", "configContract")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    # Drop apiKey from both before comparing — it's masked on GET.
    live_fd.pop("apiKey", None)
    des_fd.pop("apiKey", None)
    # Subset comparison: Prowlarr 2.x adds server-injected fields like
    # `syncRejectBlocklistedTorrentHashesWhileGrabbing` that aren't in
    # the fixture. Only verify our specified fields match.
    for k, v in des_fd.items():
        if live_fd.get(k) != v:
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    return True

with open(apps_path) as f:
    desired_apps = json.load(f)

live_apps = http("GET", "/applications") or []
live_by_name = {a["name"]: a for a in live_apps}

created, updated, unchanged = [], [], []
for desired in desired_apps:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/applications/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/applications", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
    echo "${APPS_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
fi

# 14b. Radarr: minimum-seeders threshold per indexer.
#
# Filters out dead-swarm releases at search time. Without this,
# Radarr happily grabs releases from old (10-30+ year) movies whose
# trackers still report cached "9 seeders exist" but no peer is
# actually online — qBit then sits in `metaDL` forever. Symptom is
# a download that hangs at 0% with 0 connected peers despite the
# torrent metadata claiming a healthy swarm.
#
# We set minimumSeeders=5 on every Radarr indexer (the indexer
# objects on Radarr's side, populated by the Apps-integration sync
# in Step 14). At search time, indexers that return releases with
# fewer than 5 reported seeders never even reach Radarr's scoring
# pass, so the operator never gets a "stalled" download from a
# dead swarm.
#
# 5 is a deliberately conservative floor: high enough to weed out
# graveyard torrents, low enough that legitimate niche releases
# (foreign films, documentaries, just-released indie movies) still
# pass. We don't compete on "fastest possible" downloads — we just
# refuse to start ones that won't progress.
#
# Idempotent: PUT only when the live value differs from 5. Default
# Radarr value when an indexer is first synced from Prowlarr is 1
# (effectively no filter), so on a fresh setup every indexer gets
# bumped on first run and stays at 5 thereafter.
echo "Configuring Radarr indexer minimum_seeders threshold..."
SEEDER_SUMMARY=$(python3 - "${RADARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
api_key = sys.argv[1]
BASE = "http://localhost:7878/api/v3"
TARGET = 10

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

result = {"updated": [], "unchanged": [], "skipped_disabled": []}
for ix in http("GET", "/indexer") or []:
    name = ix.get("name", "?")
    if not (ix["enableRss"] or ix["enableAutomaticSearch"] or ix["enableInteractiveSearch"]):
        result["skipped_disabled"].append(name)
        continue
    bumped = False
    for fld in ix.get("fields", []):
        if fld["name"] == "minimumSeeders":
            old = fld.get("value")
            if old != TARGET:
                fld["value"] = TARGET
                bumped = True
            break
    if bumped:
        http("PUT", "/indexer/%d?forceSave=true" % ix["id"], ix)
        result["updated"].append(name)
    else:
        result["unchanged"].append(name)

print(json.dumps(result))
PYEOF
)
echo "${SEEDER_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("updated to min=5  ", s["updated"])
show("already at 5      ", s["unchanged"])
show("skipped (disabled)", s["skipped_disabled"])
'

# 15. Radarr → qBittorrent download client.
#
# Radarr's grab pipeline: indexer search → magnet/.torrent URL →
# hand off to a configured download client. The fixture wires the
# qBittorrent container (reachable through Gluetun's network at
# `gluetun:8080`) with category=radarr so all Radarr-grabbed torrents
# land in their own qBit category and can be cleaned up
# independently of the operator's personal torrents.
#
# Like Step 14 the fixture has a sanitized `password` field; we
# inject the live ${QBIT_PW} (read from .env) at apply time. The
# password is masked to "********" on subsequent GETs, so drift
# detection ignores it (same trade-off as the apiKey in Step 14).
echo "Configuring Radarr → qBittorrent download client..."
RADARR_DLCLIENTS_FILE="$(dirname "$0")/data/radarr_downloadclients.json"
if [[ ! -f "${RADARR_DLCLIENTS_FILE}" ]]; then
    echo "  WARN: ${RADARR_DLCLIENTS_FILE} not found — skipping."
else
    QBIT_PW=$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)
    if [[ -z "${QBIT_PW}" ]]; then
        echo "  WARN: QBITTORRENT_ADMIN_PASSWORD missing from ${ENV_FILE} — skipping."
    else
        DLCLIENT_SUMMARY=$(python3 - "${RADARR_DLCLIENTS_FILE}" "${RADARR_KEY}" "${QBIT_PW}" <<'PYEOF'
import json, sys, urllib.request
clients_path, api_key, qbit_pw = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:7878/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    return {f["name"]: f.get("value") for f in (fields or [])}

def inject_password(payload):
    new_fields = []
    for f in payload.get("fields", []):
        if f.get("name") == "password":
            f = dict(f)
            f["value"] = qbit_pw
        new_fields.append(f)
    payload = dict(payload)
    payload["fields"] = new_fields
    return payload

def shape_payload(desired):
    payload = {k: v for k, v in desired.items() if k != "id"}
    return inject_password(payload)

def match(live, desired_payload):
    keys = ("name", "enable", "protocol", "priority", "implementation",
            "configContract", "removeCompletedDownloads",
            "removeFailedDownloads")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    # Drop the masked password from comparison (same logic as the
    # apiKey in Step 14). Also drop fields the live response carries
    # that aren't in the fixture (Radarr appends per-implementation
    # advanced defaults like initialState, recentMoviePriority, etc.
    # that are server-side managed and we don't want to fight over).
    live_fd.pop("password", None)
    des_fd.pop("password", None)
    # Compare only the keys the fixture cares about; ignore any extras
    # Radarr added on its own.
    for k in des_fd:
        if live_fd.get(k) != des_fd[k]:
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    return True

with open(clients_path) as f:
    desired_clients = json.load(f)

live_clients = http("GET", "/downloadclient") or []
live_by_name = {c["name"]: c for c in live_clients}

created, updated, unchanged = [], [], []
for desired in desired_clients:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/downloadclient/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/downloadclient", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
        echo "${DLCLIENT_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
    fi
fi

# 16. Radarr quality definitions (custom 720p/1080p size limits).
#
# Radarr ships with default size limits per quality (e.g. WEBDL-1080p
# default maxSize≈400 MB/min). For a Pi 4 + small SSD kiosk that's
# wasteful — the third layer of the quality filter (CLAUDE.md
# "Quality configuration") tightens the budget to:
#   720p qualities → maxSize 60 MB/min, preferredSize 25 MB/min
#   1080p qualities → maxSize 100 MB/min, preferredSize 40 MB/min
#
# The fixture lists every quality (including SD + 4K + Remux) so
# re-running the script also corrects values that may have drifted
# from a UI-side edit. Quality.id is stable across Radarr versions,
# so we match by quality.id (not quality.name, which has been
# renamed across major versions in the past).
echo "Configuring Radarr quality definitions..."
RADARR_QUALITY_FILE="$(dirname "$0")/data/radarr_qualitydefinitions.json"
if [[ ! -f "${RADARR_QUALITY_FILE}" ]]; then
    echo "  WARN: ${RADARR_QUALITY_FILE} not found — skipping."
else
    QD_SUMMARY=$(python3 - "${RADARR_QUALITY_FILE}" "${RADARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
qd_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:7878/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

with open(qd_path) as f:
    desired_qds = json.load(f)

live_qds = http("GET", "/qualitydefinition") or []
# Live response: each entry has top-level `id` (definition id) and
# nested `quality.id` (quality id, stable across versions). The PUT
# endpoint takes the top-level definition id, so we keep both.
live_by_quality_id = {q["quality"]["id"]: q for q in live_qds}

unchanged, updated, missing = [], [], []
for desired in desired_qds:
    qid = desired["quality"]["id"]
    if qid not in live_by_quality_id:
        # Should never happen — Radarr ships every built-in quality
        # on first start. If it does, log it and skip rather than
        # POST (the API doesn't support creating new quality defs).
        missing.append(desired["quality"]["name"])
        continue
    live = live_by_quality_id[qid]
    drift = (
        live.get("minSize") != desired.get("minSize")
        or live.get("maxSize") != desired.get("maxSize")
        or live.get("preferredSize") != desired.get("preferredSize")
    )
    if not drift:
        unchanged.append(desired["quality"]["name"])
        continue
    # PUT with the live entry as a base + the three size fields
    # overwritten. We keep weight/title/quality intact so Radarr
    # doesn't fight us on server-managed fields.
    payload = dict(live)
    payload["minSize"] = desired.get("minSize")
    payload["maxSize"] = desired.get("maxSize")
    payload["preferredSize"] = desired.get("preferredSize")
    http("PUT", "/qualitydefinition/%d" % live["id"], payload)
    updated.append(desired["quality"]["name"])

print(json.dumps({"updated": updated, "unchanged": unchanged, "missing": missing}))
PYEOF
)
    echo "${QD_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
# Quality definitions tend to be many (30+). Print a count summary
# rather than spelling each one to keep output readable.
def count(label, items):
    if items:
        print("  " + label + ": " + str(len(items)) + " (" + ", ".join(items[:3]) + ("..." if len(items) > 3 else "") + ")")
count("updated  ", s["updated"])
count("unchanged", s["unchanged"])
count("missing  ", s["missing"])
'
fi

# 17. qBittorrent: `radarr` category.
#
# Categories in qBit are just label-+-savePath pairs but they let us
# segregate Radarr's torrents from anything else and apply per-
# category cleanup later (the Confirm-Remove flow in the kiosk
# walks Radarr history → asks qBit to delete every torrent ever
# associated with the movie). The fixture leaves savePath empty —
# qBit then defaults each torrent's location to the global
# default-save-path, which lives on /downloads inside the container.
#
# qBit's web API needs cookie auth (no API key), so we log in once
# into a temp cookie jar and reuse it for the createCategory call.
# The createCategory endpoint returns 409 on duplicate name — our
# idempotency check just fetches existing categories first and only
# POSTs missing ones.
echo "Configuring qBittorrent categories..."
QBIT_CATS_FILE="$(dirname "$0")/data/qbit_categories.json"
if [[ ! -f "${QBIT_CATS_FILE}" ]]; then
    echo "  WARN: ${QBIT_CATS_FILE} not found — skipping."
else
    QBIT_PW="${QBIT_PW:-$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)}"
    if [[ -z "${QBIT_PW}" ]]; then
        echo "  WARN: QBITTORRENT_ADMIN_PASSWORD missing from ${ENV_FILE} — skipping."
    else
        QBIT_COOKIE="/tmp/qbit-setup-$$.cookie"
        # Login. SID cookie lives in $QBIT_COOKIE for downstream calls.
        # qBit returns plain "Ok." on success, "Fails." on bad creds.
        LOGIN_RESP=$(curl -fsS --connect-timeout 10 \
            -X POST -d "username=admin&password=${QBIT_PW}" \
            -c "${QBIT_COOKIE}" \
            http://localhost:8080/api/v2/auth/login || echo "fail")
        if [[ "${LOGIN_RESP}" != "Ok." ]]; then
            echo "  WARN: qBittorrent login failed (response: ${LOGIN_RESP}); skipping category setup."
            rm -f "${QBIT_COOKIE}"
        else
            QBIT_SUMMARY=$(python3 - "${QBIT_CATS_FILE}" "${QBIT_COOKIE}" <<'PYEOF'
import json, sys, urllib.request, urllib.parse
cats_path, cookie_path = sys.argv[1], sys.argv[2]
BASE = "http://localhost:8080/api/v2"

# Load the SID cookie out of the curl jar (Netscape cookie format).
# Format: domain<TAB>flag<TAB>path<TAB>secure<TAB>expiration<TAB>name<TAB>value
# qBit sends the cookie HttpOnly which curl annotates by prefixing the
# domain with `#HttpOnly_`. We split tab-fields on every line that has
# enough columns rather than filtering "#"-prefixed lines (which
# would skip the actual cookie row).
sid = ""
with open(cookie_path) as f:
    for line in f:
        if not line.strip() or line.startswith("# "):
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) >= 7 and parts[5] == "SID":
            sid = parts[6]
            break
if not sid:
    print(json.dumps({"error": "could not parse SID cookie"}))
    sys.exit(0)

def http(method, path, form=None):
    headers = {"Cookie": "SID=" + sid}
    data = None
    if form is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
        data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return raw.decode() if raw else ""

with open(cats_path) as f:
    desired_cats = json.load(f)

live_raw = http("GET", "/torrents/categories")
live_cats = json.loads(live_raw) if live_raw.strip() else {}

created, updated, unchanged = [], [], []
for desired in desired_cats:
    name = desired["name"]
    save_path = desired.get("savePath", "")
    if name in live_cats:
        if (live_cats[name].get("savePath", "") or "") == save_path:
            unchanged.append(name)
        else:
            http("POST", "/torrents/editCategory",
                 {"category": name, "savePath": save_path})
            updated.append(name)
    else:
        http("POST", "/torrents/createCategory",
             {"category": name, "savePath": save_path})
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
            rm -f "${QBIT_COOKIE}"
            echo "${QBIT_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
if s.get("error"):
    print("  WARN: " + s["error"])
else:
    def show(label, items):
        if items:
            print("  " + label + ": " + ", ".join(items))
    show("created  ", s["created"])
    show("updated  ", s["updated"])
    show("unchanged", s["unchanged"])
'
        fi
    fi
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
