#!/usr/bin/env bash
#
# Magic Dingus Box — converge Radarr + Sonarr Custom Formats.
#
# One idempotent entry point for the *middle layer* of the 3-layer quality
# filter (see CLAUDE.md "Quality configuration"): the codified Custom
# Formats in scripts/data/{radarr,sonarr}_custom_formats.json plus the
# score maps below, applied to each service's "Any" quality profile.
#
# WHY THIS IS A SEPARATE SCRIPT (2026-08-13). These two blocks used to live
# inline in setup_services.sh, which runs at PROVISIONING time and from the
# Content Manager's Media Browser Configure/Reconfigure flow — and nowhere
# else. update.sh never ran it. So a release that added or retuned a Custom
# Format shipped the fixture JSON to every fielded box (the tarball carries
# scripts/data/*.json) and then never reconciled it into Radarr/Sonarr: the
# box kept downloading against the OLD rules with the NEW rules sitting on
# disk beside them. That is exactly how the English-audio enforcement of
# 2026-08-13 would have reached zero customers. The owner's standing
# requirement is that pushing an update means every box ends up running the
# same behaviour, so the reconciler now lives in a standalone converge
# script that update.sh invokes on every OTA — same pattern, gating and
# failure posture as setup_memory_tuning.sh / setup_network_hardening.sh.
#
# setup_services.sh CALLS this script rather than carrying its own copy.
# There is exactly one copy of each reconciler in the repo; provisioning and
# OTA can never drift apart because they run the same file.
#
# Skips cleanly (exit 0, never fails a caller) when:
#   * MAGIC_SKIP_SYSTEMCTL=true          — test mode / CI runner
#   * ${SERVICES_DIR}/.env is absent     — box has no Media Browser at all
#   * a service's API key cannot be read — that half of the stack was never
#                                          provisioned (pre-Sonarr boxes)
#   * a service does not answer /system/status within the probe window
#   * the fixture JSON is missing from this release
# An OTA must never fail because a box has no Media Browser.
#
# Test seams (mirroring update.sh / setup_memory_tuning.sh conventions):
#   MAGIC_SERVICES_DIR       - override /opt/magic_dingus_box/services
#   MAGIC_SKIP_SYSTEMCTL     - "true" skips the whole converge (test mode)
#   MAGIC_CF_PROBE_ATTEMPTS  - readiness probe attempts (2s apart, default 30)

set -euo pipefail

# Resolve our own absolute path NOW, before anything cds — the fixtures live
# next to us at scripts/data/*.json.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SERVICES_DIR="${MAGIC_SERVICES_DIR:-/opt/magic_dingus_box/services}"
ENV_FILE="${SERVICES_DIR}/.env"
SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"
PROBE_ATTEMPTS="${MAGIC_CF_PROBE_ATTEMPTS:-30}"

log() { echo "[cf-converge] $1"; }

if [[ "${SKIP_SYSTEMCTL}" == "true" ]]; then
    log "SKIP: Custom Format convergence (test mode)"
    exit 0
fi

# --- Unprovisioned box: nothing to converge -----------------------------------
# Same gate magic-dingus-services.service uses (ConditionPathExists on this
# file). A kiosk that was never given a WireGuard config has no Radarr and no
# Sonarr; converging is meaningless and an OTA must not fail over it.
if [[ ! -f "${ENV_FILE}" ]]; then
    log "no ${ENV_FILE} — box has no Media Browser; nothing to converge"
    exit 0
fi

# API keys: config.xml is authoritative (it is what setup_services.sh itself
# reads), services/.env is the fallback for a box whose config dir is not
# readable by this user. An empty key means that service was never
# provisioned — skip it, do not fail.
read_api_key() {
    local svc="$1" env_var="$2" key=""
    key="$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/${svc}/config.xml" 2>/dev/null || true)"
    if [[ -z "${key}" ]]; then
        key="$(grep -E "^${env_var}=" "${ENV_FILE}" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '[:space:]' || true)"
    fi
    printf '%s' "${key}"
}

RADARR_KEY="$(read_api_key radarr RADARR_API_KEY)"
SONARR_KEY="$(read_api_key sonarr SONARR_API_KEY)"

# Readiness probe. --max-time bounds it in WALL CLOCK, not just iterations: a
# container that is up with its port bound but the app wedged mid-init
# ACCEPTS the socket and never answers, and an untimed curl blocks there
# forever (observed live 2026-08-11, Sonarr). Same bound and cadence as the
# probes in setup_services.sh.
probe_ready() {
    local port="$1" key="$2" i
    for ((i = 0; i < PROBE_ATTEMPTS; i++)); do
        if curl -fsS --max-time 5 -o /dev/null -H "X-Api-Key: ${key}" \
            "http://localhost:${port}/api/v3/system/status" 2>/dev/null; then
            return 0
        fi
        sleep 2
    done
    return 1
}

# --- Radarr Custom Formats + "Any" profile score map --------------------------
#
# The kiosk's Pi 4 hardware can only smoothly decode H.264 in the
# 720p-1080p range. Anything else (AV1, HEVC at 1080p, HDR, Remux)
# either has no hardware decoder or blows the size budget. We enforce
# the codec/quality choice with a 3-layer filter and the middle layer
# is a Custom Format score map applied to the "Any" quality profile:
# every grab must net at least minFormatScore=-200 across the formats
# below.
#
# These formats and scores were originally crafted by hand in the
# Radarr UI, which left them at risk of silent drift — we caught HEVC
# scoring slipping from -250 to -100 in production, which let HEVC
# files slip through the score floor and get downloaded. Codifying the
# spec in scripts/data/radarr_custom_formats.json + reapplying it on
# every setup AND every OTA eliminates that whole class of drift.
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
# A second run is therefore a pure no-op — no writes, no ids churned.
echo "Configuring Radarr Custom Formats + 'Any' profile score map..."
CF_DATA_FILE="${SCRIPT_DIR}/data/radarr_custom_formats.json"
if [[ ! -f "${CF_DATA_FILE}" ]]; then
    echo "  WARN: ${CF_DATA_FILE} not found — skipping. Custom Formats may already be configured manually; verify via web UI."
elif [[ -z "${RADARR_KEY}" ]]; then
    echo "  SKIP: no Radarr API key on this box (Radarr never provisioned)."
elif ! probe_ready 7878 "${RADARR_KEY}"; then
    echo "  WARN: Radarr not reachable — skipping Custom Formats/profile reconciliation (later runs will apply it)."
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
    # Release-group policy, RETUNED FOR PI 5 (2026-07-26).
    #
    # These were ONE format ("Trusted small-release groups", +30) that
    # lumped quality rips (RARBG/SURGE/EVO) together with groups whose
    # entire identity is making SMALL files (YIFY/YTS/GalaxyRG/ION10).
    # That bonus stacked with x264's +50, so a low-bitrate YIFY encode
    # scored +80 and beat a genuinely better release at +50 — the rules
    # were optimizing for file size while claiming to optimize quality.
    # Correct on the Pi 4B (hardware H.264, tight storage); wrong now.
    #
    # Measured on Pi 5 (2026-07-26): 1080p software decode costs 36% of
    # 400% CPU for H.264 and 41% for HEVC — roughly 3.5 of 4 cores idle.
    # There is no decode headroom problem to protect against, and the
    # library SSD had 175GB free. So prefer the better encode.
    "Quality release groups":           30,
    "Low-bitrate size-optimized groups": -30,
    # LEGACY: the pre-split format. Boxes provisioned before 2026-07-26
    # still have it in Radarr, and the profile reconciler below only
    # touches formats named in this map — so it must stay here, scored
    # 0, to neutralize it. Removing this line would leave those boxes
    # silently running the old +30 small-file bias forever. It is
    # deliberately absent from radarr_custom_formats.json so fresh
    # provisions never create it.
    "Trusted small-release groups":      0,
    # Scam-rejection formats: well below the -200 minFormatScore floor
    # so a single match makes a release uneligible regardless of other
    # bonuses. Observed in production tonight: trash indexers (TPB-via-
    # Knaben aggregator, the "UIndex.org" prefix farm) post torrents
    # matching newly-released theatrical titles where the content is
    # actually a 0-byte .txt file or a .exe malware payload. Quality
    # profile alone doesn't catch them because the release name reads
    # as legit (proper year, 1080p tag, codec tag). These two CFs
    # detect the giveaways in the title itself.
    "Malware/scam executable in title": -10000,
    "Known scam aggregator branding":   -10000,
    # Title-level foreign-language defense. The quality profile's Language
    # setting (Original) is the primary defense, but Radarr's title parser
    # can be fooled into tagging a release as English when the torrent
    # title contains English text alongside non-Latin characters (e.g.
    # "Дьявол носит Prada 2 / The Devil Wears Prada 2" — parser saw the
    # English half and the cyrillic Russian audio slipped past). This
    # CF catches that via title regex: any cyrillic/CJK/korean char OR
    # an explicit foreign-dub / multi-audio keyword in the title scores
    # -10000, well below minFormatScore=-200 so the release is uneligible
    # regardless of other bonuses. Broadened 2026-08-13 with the bare
    # scene markers (MULTi, TRUEFRENCH, VOSTFR, VFF/VFQ, DUAL AUDIO,
    # GERMAN.DL, "iTA.ENG"-style language pairs) after
    # "Game Of Thrones S03 MULTi (1080p) BluRay x264 PopHD" — French
    # audio — matched NONE of the original alternatives and was grabbed.
    "Non-English title signals":        -10000,
    # LANGUAGE layer of the same rule (owner decision 2026-08-13: "no
    # foreign audio on English content"). LanguageSpecification with
    # value=-2 ("Original") + exceptLanguage=true matches when ANY
    # language other than the title's own original language is present,
    # so an English show rejects a French release while a Korean film
    # still accepts its Korean release — world cinema stays obtainable.
    # Both layers are needed: this one is precise but trusts the
    # parser, and the parser is exactly what failed on the MULTi grab
    # (it tagged the release English, i.e. as the Original, so this
    # spec would have passed it). The title regex above is the backstop.
    "Release language is not the original": -10000,
}
MIN_FORMAT_SCORE = -200

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def spec_field_values(spec):
    return {f.get("name"): f.get("value") for f in spec.get("fields", [])}

def cf_specs_match(live, desired):
    # We compare the fields we care about (name, EVERY spec field value,
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
        # Compare EVERY field the fixture declares, by name — not just
        # the regex. A ReleaseTitleSpecification has only "value", but a
        # LanguageSpecification also carries "exceptLanguage", and that
        # boolean is the entire meaning of the "Release language is not
        # the original" format: comparing "value" alone would call a
        # live CF with exceptLanguage=false a match for a fixture with
        # exceptLanguage=true and leave the box permanently unprotected
        # — the exact silent-drift class this reconciler exists to kill.
        # Extra server-side fields are ignored (we iterate the DESIRED
        # side), so a Radarr version that adds a field can't cause a
        # rewrite loop.
        av, bv = spec_field_values(a), spec_field_values(b)
        for fname, want in bv.items():
            if av.get(fname) != want: return False
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

    # Codify the quality TIER policy. Three pieces:
    #   1. cutoff = Bluray-720p (id 6) — once Radarr has Bluray-720p
    #      or anything ranked higher in the items[] list, stop
    #      searching for "upgrades."
    #   2. allowed quality tiers — only the 720p+1080p H.264 tiers.
    #      720p is preferred (lower in the items[] list = lower tier
    #      = preferred when both exist? no, items are ordered low→
    #      high quality, so 1080p tiers WIN ties. 720p preference
    #      isn't enforced via cutoff alone; it'd require items
    #      reordering or per-quality custom-format scoring. We allow
    #      both and accept that Radarr picks 1080p when available —
    #      that's the right behavior for old catalog titles where
    #      720p is scarce. The custom-format scoring elsewhere keeps
    #      AV1/HEVC/HDR/Remux out so we don't regress on hardware
    #      decode anyway.)
    #   3. disallow SD / 4K / Remux / raw-DVD tiers entirely so
    #      operator can't accidentally grab them via Pick a Source.
    #
    # The fixture is name-based (not id-based) so it survives Radarr
    # version bumps that re-number quality ids.
    ALLOWED_QUALITY_NAMES = {
        "HDTV-720p", "WEB 720p", "Bluray-720p",
        "HDTV-1080p", "WEB 1080p", "Bluray-1080p",
    }
    DESIRED_CUTOFF_QUALITY_NAME = "Bluray-720p"

    # cutoff is an int id that names a row in items[] (or a sub-row in
    # a quality-group). We resolve by walking items + their nested
    # qualities so the fixture stays name-based.
    def find_quality_id(items, target_name):
        for item in items:
            if item.get("name") == target_name and item.get("id"):
                return item["id"]
            for sub in (item.get("items") or []):
                q = sub.get("quality") or {}
                if q.get("name") == target_name:
                    return q.get("id")
            q = item.get("quality") or {}
            if q.get("name") == target_name:
                return q.get("id")
        return None

    desired_cutoff = find_quality_id(any_profile.get("items", []), DESIRED_CUTOFF_QUALITY_NAME)
    if desired_cutoff and any_profile.get("cutoff") != desired_cutoff:
        any_profile["cutoff"] = desired_cutoff
        profile_changed = True
        score_changes.append("cutoff=%s(id=%d)" % (DESIRED_CUTOFF_QUALITY_NAME, desired_cutoff))

    def item_name(item):
        return item.get("name") or (item.get("quality") or {}).get("name")

    def enforce_allowed(items, group_allowed=False):
        # MUST recurse. Radarr nests real qualities inside group rows:
        # "WEB 2160p" is a group whose children are WEBDL-2160p and
        # WEBRip-2160p. Walking only the top level flips the GROUP to
        # disallowed while its children stay allowed, so the
        # 720p/1080p-only policy was never actually enforced for any
        # grouped tier and 4K/SD releases stayed eligible. Caught by the
        # weekly smoke test, which correctly inspects leaves:
        #   "extra-allowed=WEBDL-2160p,WEBDL-480p,WEBRip-2160p,WEBRip-480p"
        #
        # NOTE ALLOWED_QUALITY_NAMES holds GROUP names ("WEB 720p"), not
        # leaf names ("WEBDL-720p") — a leaf is allowed when its own name
        # is listed OR its containing group is. Testing leaves against the
        # group-name set alone disallows every WEB tier, which starves
        # search results (WEB-DL is the most common source). Learned the
        # hard way on hardware: an earlier version of this fix left only
        # 4 qualities allowed.
        changed = False
        for item in items:
            nm = item_name(item)
            named = nm in ALLOWED_QUALITY_NAMES if nm else False
            want_allowed = named or group_allowed
            children = item.get("items") or []
            if children:
                if enforce_allowed(children, want_allowed):
                    changed = True
                # Group row reflects its leaves, so the Radarr UI checkbox
                # never contradicts what is actually eligible.
                want_allowed = any(c.get("allowed") for c in children)
            if item.get("allowed") != want_allowed:
                item["allowed"] = want_allowed
                changed = True
                score_changes.append("%s.allowed=%s" % (nm, want_allowed))
        return changed

    if enforce_allowed(any_profile.get("items", [])):
        profile_changed = True

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

# --- Sonarr Custom Formats + "Any" profile score map --------------------------
#
# Sonarr twin of the Radarr block above. Same codec/quality policy: the
# kiosk decodes H.264 720p/1080p; AV1/HEVC-1080p+/HDR/Remux and non-English
# / scam releases are pushed below the -200 minFormatScore floor. All CF
# specs port verbatim from the identical Radarr fixture. Two Sonarr-specific
# choices:
#   * SCORE_MAP omits the legacy "Trusted small-release groups" (0) entry —
#     that entry only exists to neutralize pre-2026-07-26 RADARR boxes;
#     Sonarr is greenfield and never had it.
#   * ALLOWED_QUALITY_NAMES lists LEAF quality names (HDTV-720p, WEBDL-720p,
#     …) rather than group names. Sonarr's default profile grouping isn't
#     assumed; a leaf is allowed when its own name matches, and group rows
#     reflect their leaves. No profile-language mutation (Sonarr v4 handles
#     language separately from the quality profile).
echo "Configuring Sonarr Custom Formats + 'Any' profile score map..."
SONARR_CF_DATA_FILE="${SCRIPT_DIR}/data/sonarr_custom_formats.json"
if [[ ! -f "${SONARR_CF_DATA_FILE}" ]]; then
    echo "  WARN: ${SONARR_CF_DATA_FILE} not found — skipping. Verify Sonarr Custom Formats via web UI."
elif [[ -z "${SONARR_KEY}" ]]; then
    echo "  SKIP: no Sonarr API key on this box (pre-Sonarr provisioning)."
elif ! probe_ready 8989 "${SONARR_KEY}"; then
    # The probe GATES the python below — it must not merely delay it. The
    # python makes ~10 unguarded urllib calls, so a still-initializing Sonarr
    # either answers within the probe window or every call was doomed anyway.
    echo "  WARN: Sonarr not reachable — skipping Custom Formats/profile reconciliation (later runs will apply it)."
else
    SONARR_CF_SUMMARY=$(python3 - "${SONARR_CF_DATA_FILE}" "${SONARR_KEY}" <<'PYEOF'
import json, sys, urllib.request, urllib.error

cf_data_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:8989/api/v3"

SCORE_MAP = {
    "AV1 codec (UNWATCHABLE on Pi 4)": -1000,
    "x265/HEVC 1080p+":                -250,
    "HDR / Dolby Vision":              -200,
    "Remux / Raw-HD":                  -500,
    "x264 codec (BONUS)":               50,
    "Quality release groups":           30,
    "Low-bitrate size-optimized groups": -30,
    "Malware/scam executable in title": -10000,
    "Known scam aggregator branding":   -10000,
    # English-audio rule, both layers — see the Radarr block above for the
    # full rationale. Sonarr needs them at least as much: it has NO
    # profile-level language gate at all, so before 2026-08-13 a French
    # MULTi season pack had nothing standing in its way.
    "Non-English title signals":        -10000,
    "Release language is not the original": -10000,
}
MIN_FORMAT_SCORE = -200

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def spec_field_values(spec):
    return {f.get("name"): f.get("value") for f in spec.get("fields", [])}

def cf_specs_match(live, desired):
    if live.get("name") != desired.get("name"): return False
    if live.get("includeCustomFormatWhenRenaming") != desired.get("includeCustomFormatWhenRenaming"): return False
    ls, ds = live.get("specifications", []), desired.get("specifications", [])
    if len(ls) != len(ds): return False
    for a, b in zip(ls, ds):
        if a.get("name") != b.get("name"): return False
        if a.get("implementation") != b.get("implementation"): return False
        if bool(a.get("negate")) != bool(b.get("negate")): return False
        if bool(a.get("required")) != bool(b.get("required")): return False
        # Every declared field, by name — "exceptLanguage" carries the
        # whole meaning of the language format. See the Radarr twin.
        av, bv = spec_field_values(a), spec_field_values(b)
        for fname, want in bv.items():
            if av.get(fname) != want: return False
    return True

with open(cf_data_path) as f:
    desired_cfs = json.load(f)

live_cfs = http("GET", "/customformat")
live_by_name = {c["name"]: c for c in live_cfs}

created, updated, unchanged = [], [], []
name_to_id = {}

for desired in desired_cfs:
    name = desired["name"]
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

profiles = http("GET", "/qualityprofile")
any_profile = next((p for p in profiles if p["name"] == "Any"), None)
profile_changed = False
score_changes = []

if any_profile is None:
    print("WARN: 'Any' profile missing; cannot apply score map", file=sys.stderr)
    print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged,
                      "profile_changed": False, "score_changes": []}))
    sys.exit(0)

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

# Leaf-name allowed set: only the eight H.264 720p/1080p tiers.
ALLOWED_QUALITY_NAMES = {
    "HDTV-720p", "WEBDL-720p", "WEBRip-720p", "Bluray-720p",
    "HDTV-1080p", "WEBDL-1080p", "WEBRip-1080p", "Bluray-1080p",
}
DESIRED_CUTOFF_QUALITY_NAME = "Bluray-720p"

def find_quality_id(items, target_name):
    for item in items:
        if item.get("name") == target_name and item.get("id"):
            return item["id"]
        for sub in (item.get("items") or []):
            q = sub.get("quality") or {}
            if q.get("name") == target_name:
                return q.get("id")
        q = item.get("quality") or {}
        if q.get("name") == target_name:
            return q.get("id")
    return None

desired_cutoff = find_quality_id(any_profile.get("items", []), DESIRED_CUTOFF_QUALITY_NAME)
if desired_cutoff:
    if any_profile.get("cutoff") != desired_cutoff:
        any_profile["cutoff"] = desired_cutoff
        profile_changed = True
        score_changes.append("cutoff=%s(id=%d)" % (DESIRED_CUTOFF_QUALITY_NAME, desired_cutoff))
else:
    print("WARN: could not resolve quality id for cutoff '%s' (Sonarr may have renamed it); cutoff left unchanged" % DESIRED_CUTOFF_QUALITY_NAME, file=sys.stderr)

def item_name(item):
    return item.get("name") or (item.get("quality") or {}).get("name")

def enforce_allowed(items, group_allowed=False):
    changed = False
    for item in items:
        nm = item_name(item)
        named = nm in ALLOWED_QUALITY_NAMES if nm else False
        want_allowed = named or group_allowed
        children = item.get("items") or []
        if children:
            if enforce_allowed(children, want_allowed):
                changed = True
            want_allowed = any(c.get("allowed") for c in children)
        if item.get("allowed") != want_allowed:
            item["allowed"] = want_allowed
            changed = True
            score_changes.append("%s.allowed=%s" % (nm, want_allowed))
    return changed

if enforce_allowed(any_profile.get("items", [])):
    profile_changed = True

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
    printf '%s' "${SONARR_CF_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
if s["profile_changed"]:
    print("  ✓ Sonarr Any profile score map updated: " + ", ".join(s["score_changes"]))
else:
    print("  ✓ Sonarr Any profile score map already matches desired state")
'
fi

log "Custom Formats converged"
