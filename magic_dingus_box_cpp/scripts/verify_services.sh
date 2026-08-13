#!/usr/bin/env bash
# verify_services.sh — Hard-assertion smoke test for the Media Browser
# torrent-ecosystem stack.
#
# Run AFTER setup_services.sh. Asserts that every link in the chain is
# wired up correctly and producing live output. If any check fails this
# exits non-zero so a CI / first-boot harness can surface the problem
# immediately instead of waiting for a confused user to discover it
# weeks later.
#
# Why this exists: setup_services.sh is ~1800 lines and configures four
# services through three different APIs (Radarr, Prowlarr, qBittorrent)
# plus direct SQLite injection. Plenty of places to silently regress.
# A focused smoke test is the cheapest way to catch a broken setup
# before the operator opens the kiosk and finds Movies empty.
#
# Usage:
#   ./verify_services.sh                 # exits 0 on success, 1 on failure
#   ./verify_services.sh --quiet         # only print failures + summary
#
# Auto-discovers API keys from services/.env (same source as setup).

set -uo pipefail

QUIET=0
if [[ "${1:-}" == "--quiet" ]]; then QUIET=1; fi

# ── Configuration ───────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICES_DIR="$(cd "${SCRIPT_DIR}/../../services" 2>/dev/null && pwd || echo "/opt/magic_dingus_box/services")"
ENV_FILE="${SERVICES_DIR}/.env"

# Names we expect to see ENABLED in Radarr (sourced from
# scripts/data/prowlarr_indexers.json — keep in sync if that fixture
# changes). Listed here explicitly so this script catches drift in
# either direction (missing fixture entries OR sync-skipped indexers).
EXPECTED_ENABLED_INDEXERS=(
    "LimeTorrents"
    "The Pirate Bay"
    "TorrentDownload"
    "YTS"
    "Knaben"
)

# Quality-profile invariants from setup_services.sh Step 10
EXPECTED_PROFILE_NAME="Any"
EXPECTED_MIN_FORMAT_SCORE=-200
EXPECTED_CUTOFF_QUALITY="Bluray-720p"
# H.264 720p+1080p tiers we want allowed (sources of legitimate kiosk
# content). Anything outside this list — SD, 2160p, Remux — should be
# disallowed by the profile.
EXPECTED_ALLOWED_QUALITIES=(
    "HDTV-720p"
    "WEBDL-720p"
    "WEBRip-720p"
    "Bluray-720p"
    "HDTV-1080p"
    "WEBDL-1080p"
    "WEBRip-1080p"
    "Bluray-1080p"
)

EXPECTED_CUSTOM_FORMATS=(
    "AV1 codec (UNWATCHABLE on Pi 4)"
    "x265/HEVC 1080p+"
    "HDR / Dolby Vision"
    "Remux / Raw-HD"
    "x264 codec (BONUS)"
    # Split from the old single "Trusted small-release groups" (+30) on
    # 2026-07-26: that format lumped quality rips (RARBG/SURGE/EVO) in with
    # groups whose whole identity is making SMALL files (YIFY/GalaxyRG), so
    # a low-bitrate encode scored +80 and beat a better release at +50.
    # The legacy name is deliberately NOT listed here — fresh provisions
    # never create it (it is gone from radarr_custom_formats.json); it only
    # lingers, scored 0, on boxes provisioned before the split.
    "Quality release groups"
    "Low-bitrate size-optimized groups"
    # English-audio rule (owner decision 2026-08-13). Both layers are
    # asserted because either one alone leaves a hole: the language spec
    # trusts the release parser (which tagged a French MULTi season pack
    # as English), and the title regex only sees what the title admits.
    "Non-English title signals"
    "Release language is not the original"
)

# Catalog title used for live-search smoke test. "The Matrix" is
# universally well-seeded on every torrent indexer, has ~25 years of
# release variants, and is unambiguously movie-not-tv. If THIS doesn't
# return ≥10 releases through Prowlarr, the indexer chain is broken.
SEARCH_QUERY="The Matrix 1999"
SEARCH_MIN_RESULTS=10

# ── Helpers ─────────────────────────────────────────────────────────
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
FAILURES=()

log()    { [[ "${QUIET}" -eq 1 ]] || echo "$@"; }
header() { [[ "${QUIET}" -eq 1 ]] || echo -e "\n── $* ──"; }

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    log "  ✓ $*"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    FAILURES+=("$*")
    echo "  ✗ $*"
}

# Deliberately does NOT touch PASS_COUNT/FAIL_COUNT — a skip is neither.
# Keeping it out of both keeps TOTAL and the "All N checks PASSED" banner
# below honest: N reflects only checks that actually ran.
skip() {
    SKIP_COUNT=$((SKIP_COUNT + 1))
    log "  ⊘ $* (skipped)"
}

require_env() {
    if [[ ! -f "${ENV_FILE}" ]]; then
        echo "ERROR: ${ENV_FILE} not found. Has setup_services.sh been run?"
        exit 2
    fi
    # shellcheck disable=SC1090
    set -a; . "${ENV_FILE}"; set +a
    : "${RADARR_API_KEY:?RADARR_API_KEY missing from .env}"
    : "${PROWLARR_API_KEY:?PROWLARR_API_KEY missing from .env}"
    : "${QBITTORRENT_ADMIN_PASSWORD:?QBITTORRENT_ADMIN_PASSWORD missing from .env}"
    # SONARR_API_KEY is deliberately NOT a hard `:?` guard like the three
    # above. Under `set -uo pipefail`, a `:?` on a missing var exits the
    # WHOLE script immediately — before a single check runs, including
    # all four Radarr checks below. Release tarballs ship scripts/
    # wholesale, so any already-provisioned field box that hasn't re-run
    # setup_services.sh since Sonarr landed (and so has no
    # SONARR_API_KEY= line in .env) would fail the entire weekly
    # magic-dingus-smoke-test.timer run with zero Radarr coverage — the
    # exact opposite of what a smoke test is for. The five check_sonarr_*
    # checks below each individually WARN-and-skip when this is empty;
    # Radarr/Prowlarr/qBit coverage keeps running either way.
    SONARR_API_KEY="${SONARR_API_KEY:-}"
}

# ── Checks ──────────────────────────────────────────────────────────

check_radarr_indexers() {
    header "Radarr indexers"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${RADARR_API_KEY}" \
        "http://localhost:7878/api/v3/indexer" 2>/dev/null) || {
        fail "Radarr /api/v3/indexer unreachable"
        return
    }

    # Parse + assert in one python invocation — keeps shell logic simple
    # and gives us proper JSON handling for nested fields.
    local result
    result=$(python3 - "${response}" <<'PYEOF'
import json, re, sys
# Prowlarr's app-sync writes indexer names into Radarr as
# "Knaben (Prowlarr)" — the trailing " (Prowlarr)" tag is what Radarr's
# UI uses to identify Prowlarr-managed indexers vs hand-added ones.
# Strip it before comparing against our expected name list.
#
# Note: Radarr v3 has THREE enable flags per indexer (enableRss /
# enableAutomaticSearch / enableInteractiveSearch) and the legacy
# top-level `enable` field returns null. We treat an indexer as
# "enabled" when its automatic-search flag is on — that's the flag
# that actually gates Radarr's grab pipeline for the kiosk's flow.
SUFFIX_RE = re.compile(r"\s*\((?:Prowlarr|Jackett)\)\s*$", re.I)
expected = ["LimeTorrents", "The Pirate Bay", "TorrentDownload", "YTS", "Knaben"]
data = json.loads(sys.argv[1])
got_enabled = sorted(
    SUFFIX_RE.sub("", ix["name"])
    for ix in data
    if ix.get("enableAutomaticSearch")
)
missing = [n for n in expected if n not in got_enabled]
extra   = [n for n in got_enabled if n not in expected]
if not missing and not extra:
    print("OK")
else:
    parts = []
    if missing: parts.append("missing=" + ",".join(missing))
    if extra:   parts.append("extra=" + ",".join(extra))
    print("FAIL:" + " ".join(parts))
PYEOF
)
    if [[ "${result}" == "OK" ]]; then
        pass "All 5 expected indexers enabled in Radarr (LimeTorrents, TPB, TorrentDownload, YTS, Knaben)"
    else
        fail "Indexer set drift: ${result#FAIL:}"
    fi
}

check_radarr_download_client() {
    header "Radarr → qBittorrent download client"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${RADARR_API_KEY}" \
        "http://localhost:7878/api/v3/downloadclient" 2>/dev/null) || {
        fail "Radarr /api/v3/downloadclient unreachable"
        return
    }

    local result
    result=$(python3 - "${response}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
qbit = next((c for c in data if c.get("implementation") == "QBittorrent" and c.get("enable")), None)
if not qbit:
    print("FAIL:no enabled QBittorrent client")
    sys.exit()
fields = {f["name"]: f.get("value") for f in qbit.get("fields", [])}
checks = []
if not fields.get("host"):                  checks.append("host empty")
if not fields.get("port"):                  checks.append("port empty")
if not fields.get("movieCategory"):         checks.append("movieCategory empty")
# Password isn't returned by the API (Radarr redacts it) — we can only
# verify the field exists; the qBit-direct check below verifies auth.
if not qbit.get("name"):                    checks.append("name empty")
if checks:
    print("FAIL:" + ",".join(checks))
else:
    print(f"OK:host={fields.get('host')}:{fields.get('port')} category={fields.get('movieCategory')}")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "qBittorrent download client wired (${result#OK:})"
    else
        fail "Download client misconfigured: ${result#FAIL:}"
    fi
}

check_radarr_quality_profile() {
    header "Radarr quality profile (Any → ${EXPECTED_CUTOFF_QUALITY} cutoff)"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${RADARR_API_KEY}" \
        "http://localhost:7878/api/v3/qualityprofile" 2>/dev/null) || {
        fail "Radarr /api/v3/qualityprofile unreachable"
        return
    }

    local result
    result=$(python3 - "${response}" "${EXPECTED_PROFILE_NAME}" \
        "${EXPECTED_MIN_FORMAT_SCORE}" "${EXPECTED_CUTOFF_QUALITY}" \
        "${EXPECTED_ALLOWED_QUALITIES[@]}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
profile_name = sys.argv[2]
expected_min = int(sys.argv[3])
expected_cutoff_name = sys.argv[4]
expected_allowed = set(sys.argv[5:])

prof = next((p for p in data if p.get("name") == profile_name), None)
if not prof:
    print(f"FAIL:profile '{profile_name}' not found"); sys.exit()

problems = []

# minFormatScore
got_min = prof.get("minFormatScore")
if got_min != expected_min:
    problems.append(f"minFormatScore={got_min} (want {expected_min})")

# Walk items[] flat: items can have nested `items` for grouped tiers
# (Radarr models "Bluray-720p" and "WEBDL-720p" inside a "720p" group
# in some profiles). Allowed quality is determined by the leaf.
def walk(items):
    for it in items:
        q = it.get("quality")
        if q is not None:
            yield it, q
        for sub_it, sub_q in walk(it.get("items", [])):
            yield sub_it, sub_q

leaves = list(walk(prof.get("items", [])))
allowed_names = {q["name"] for it, q in leaves if it.get("allowed")}

# Hard equality: kiosk profile must allow EXACTLY the H.264 720p+1080p
# tiers — not more (would invite SD/2160p), not less (would starve
# search results).
missing_allowed = expected_allowed - allowed_names
extra_allowed = allowed_names - expected_allowed
if missing_allowed: problems.append(f"missing-allowed={','.join(sorted(missing_allowed))}")
if extra_allowed:   problems.append(f"extra-allowed={','.join(sorted(extra_allowed))}")

# cutoff: int id pointing at a quality (or quality group). Look it up
# by id in the same walk and compare names.
cutoff_id = prof.get("cutoff")
def find_name_by_id(items, target):
    for it in items:
        q = it.get("quality")
        if q and q.get("id") == target: return q.get("name")
        if it.get("id") == target:      return it.get("name")
        sub = find_name_by_id(it.get("items", []), target)
        if sub: return sub
    return None
got_cutoff_name = find_name_by_id(prof.get("items", []), cutoff_id)
if got_cutoff_name != expected_cutoff_name:
    problems.append(f"cutoff={got_cutoff_name}(id={cutoff_id}) (want {expected_cutoff_name})")

if problems:
    print("FAIL:" + "; ".join(problems))
else:
    print(f"OK:cutoff={got_cutoff_name} minFmt={got_min} allowed={len(allowed_names)}")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Quality profile correct (${result#OK:})"
    else
        fail "Quality profile drift: ${result#FAIL:}"
    fi
}

check_radarr_custom_formats() {
    header "Radarr Custom Formats"
    local response
    response=$(curl -fsS -H "X-Api-Key: ${RADARR_API_KEY}" \
        "http://localhost:7878/api/v3/customformat" 2>/dev/null) || {
        fail "Radarr /api/v3/customformat unreachable"
        return
    }

    local result
    result=$(python3 - "${response}" "${EXPECTED_CUSTOM_FORMATS[@]}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
expected = set(sys.argv[2:])
got = {cf["name"] for cf in data}
missing = expected - got
if missing:
    print("FAIL:missing=" + ",".join(sorted(missing)))
else:
    print(f"OK:{len(got)} formats present")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Custom Formats applied (${result#OK:})"
    else
        fail "Custom Formats drift: ${result#FAIL:}"
    fi
}

check_radarr_root_folder() {
    header "Radarr root folder (/data/library)"
    # This check exists because its ABSENCE hid a shipping-blocking bug.
    # setup_services.sh created Sonarr's root folder and never Radarr's, so a
    # fresh unit had zero Radarr root folders and every movie download failed
    # with "no root folder configured in Radarr" -- while all 14 smoke checks
    # passed, because only Sonarr's was asserted. Found 2026-08-04 on the
    # first real provisioning. Asymmetric coverage between two near-identical
    # services is exactly where this class of bug hides.
    if [[ -z "${RADARR_API_KEY}" ]]; then
        skip "Radarr root folder — RADARR_API_KEY not in .env (re-run setup_services.sh)"
        return
    fi
    local response
    response=$(curl -fsS -H "X-Api-Key: ${RADARR_API_KEY}" \
        "http://localhost:7878/api/v3/rootfolder" 2>/dev/null) || {
        fail "Radarr /api/v3/rootfolder unreachable"
        return
    }
    # Presence alone is not health: the record survives the directory
    # vanishing and just flips accessible=false, after which every import
    # fails. Assert the record exists AND every root folder is accessible.
    if echo "${response}" | python3 -c "import sys,json; rf=json.load(sys.stdin); sys.exit(0 if any(r.get('path')=='/data/library' for r in rf) and all(r.get('accessible') is True for r in rf) else 1)" 2>/dev/null; then
        pass "Radarr root folder /data/library present and accessible"
    else
        fail "Radarr root folder missing or inaccessible — downloads will fail with 'no root folder configured'"
    fi
}

check_sonarr_root_folder() {
    header "Sonarr root folder (/data/library/tv)"
    if [[ -z "${SONARR_API_KEY}" ]]; then
        skip "Sonarr root folder — SONARR_API_KEY not in .env (re-run setup_services.sh)"
        return
    fi
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/rootfolder" 2>/dev/null) || {
        fail "Sonarr /api/v3/rootfolder unreachable"
        return
    }
    # Presence alone is not health: Sonarr keeps the root-folder RECORD even
    # when the directory vanished from disk (the twice-observed empty-dir
    # sweep, 2026-08-02) — it just flips accessible=false and every import
    # fails. Assert the record exists AND every root folder is accessible.
    if echo "${response}" | python3 -c "import sys,json; rf=json.load(sys.stdin); sys.exit(0 if any(r.get('path')=='/data/library/tv' for r in rf) and all(r.get('accessible') is True for r in rf) else 1)" 2>/dev/null; then
        pass "Sonarr root folder /data/library/tv present and accessible"
    else
        fail "Sonarr root folder missing or inaccessible — run storage_attach or check /mnt/ssd/library/tv"
    fi
}

check_sonarr_indexers() {
    header "Sonarr indexers (TV-capable subset)"
    if [[ -z "${SONARR_API_KEY}" ]]; then
        skip "Sonarr indexers — SONARR_API_KEY not in .env (re-run setup_services.sh)"
        return
    fi
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/indexer" 2>/dev/null) || {
        fail "Sonarr /api/v3/indexer unreachable"
        return
    }
    local result
    result=$(python3 - "${response}" <<'PYEOF'
import json, re, sys
# TV-capable indexers Prowlarr's 5000-category app-sync can push into
# Sonarr. YTS is movies-only and never syncs here. Knaben, unlike YTS,
# DOES advertise TV categories and shows up in Sonarr's live indexer
# set, but it's deliberately left out of the required POOL below — like
# EZTV, its automatic-search sync has been inconsistent (Byparr/
# Cloudflare churn), so we require a THRESHOLD (>=2 present) rather
# than an exact set — this catches a totally-broken sync without
# flaking on per-indexer quirks.
POOL = {"The Pirate Bay", "TorrentDownload", "LimeTorrents", "EZTV"}
SUFFIX_RE = re.compile(r"\s*\((?:Prowlarr|Jackett)\)\s*$", re.I)
data = json.loads(sys.argv[1])
got = sorted(
    SUFFIX_RE.sub("", ix["name"])
    for ix in data
    if ix.get("enableAutomaticSearch")
)
tv = [n for n in got if n in POOL]
if len(tv) >= 2:
    print("OK:" + ",".join(tv))
else:
    print("FAIL:only " + str(len(tv)) + " TV indexers synced (" + ",".join(tv) + "); want >=2 of " + ",".join(sorted(POOL)))
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Sonarr has TV indexers (${result#OK:})"
    else
        fail "Sonarr indexer sync weak: ${result#FAIL:}"
    fi
}

check_sonarr_download_client() {
    header "Sonarr → qBittorrent download client"
    if [[ -z "${SONARR_API_KEY}" ]]; then
        skip "Sonarr download client — SONARR_API_KEY not in .env (re-run setup_services.sh)"
        return
    fi
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/downloadclient" 2>/dev/null) || {
        fail "Sonarr /api/v3/downloadclient unreachable"
        return
    }
    local result
    result=$(python3 - "${response}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
qbit = next((c for c in data if c.get("implementation") == "QBittorrent" and c.get("enable")), None)
if not qbit:
    print("FAIL:no enabled QBittorrent client"); sys.exit()
fields = {f["name"]: f.get("value") for f in qbit.get("fields", [])}
checks = []
if not fields.get("host"):        checks.append("host empty")
if not fields.get("port"):        checks.append("port empty")
if not fields.get("tvCategory"):  checks.append("tvCategory empty")
if not qbit.get("name"):          checks.append("name empty")
if checks:
    print("FAIL:" + ",".join(checks))
else:
    print(f"OK:host={fields.get('host')}:{fields.get('port')} category={fields.get('tvCategory')}")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Sonarr qBittorrent download client wired (${result#OK:})"
    else
        fail "Sonarr download client misconfigured: ${result#FAIL:}"
    fi
}

check_sonarr_quality_profile() {
    header "Sonarr quality profile (Any → Bluray-720p cutoff)"
    if [[ -z "${SONARR_API_KEY}" ]]; then
        skip "Sonarr quality profile — SONARR_API_KEY not in .env (re-run setup_services.sh)"
        return
    fi
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/qualityprofile" 2>/dev/null) || {
        fail "Sonarr /api/v3/qualityprofile unreachable"
        return
    }
    local result
    result=$(python3 - "${response}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
expected_allowed = {
    "HDTV-720p", "WEBDL-720p", "WEBRip-720p", "Bluray-720p",
    "HDTV-1080p", "WEBDL-1080p", "WEBRip-1080p", "Bluray-1080p",
}
prof = next((p for p in data if p.get("name") == "Any"), None)
if not prof:
    print("FAIL:profile 'Any' not found"); sys.exit()
problems = []
if prof.get("minFormatScore") != -200:
    problems.append(f"minFormatScore={prof.get('minFormatScore')} (want -200)")
def walk(items):
    for it in items:
        q = it.get("quality")
        if q is not None: yield it, q
        for si, sq in walk(it.get("items", [])): yield si, sq
allowed = {q["name"] for it, q in walk(prof.get("items", [])) if it.get("allowed")}
missing = expected_allowed - allowed
extra = allowed - expected_allowed
if missing: problems.append("missing-allowed=" + ",".join(sorted(missing)))
if extra:   problems.append("extra-allowed=" + ",".join(sorted(extra)))
cutoff_id = prof.get("cutoff")
def name_by_id(items, target):
    for it in items:
        q = it.get("quality")
        if q and q.get("id") == target: return q.get("name")
        if it.get("id") == target: return it.get("name")
        sub = name_by_id(it.get("items", []), target)
        if sub: return sub
    return None
if name_by_id(prof.get("items", []), cutoff_id) != "Bluray-720p":
    problems.append(f"cutoff={name_by_id(prof.get('items', []), cutoff_id)} (want Bluray-720p)")
print("FAIL:" + "; ".join(problems) if problems else f"OK:allowed={len(allowed)}")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Sonarr quality profile correct (${result#OK:})"
    else
        fail "Sonarr quality profile drift: ${result#FAIL:}"
    fi
}

check_sonarr_custom_formats() {
    header "Sonarr Custom Formats"
    if [[ -z "${SONARR_API_KEY}" ]]; then
        skip "Sonarr Custom Formats — SONARR_API_KEY not in .env (re-run setup_services.sh)"
        return
    fi
    local response
    response=$(curl -fsS -H "X-Api-Key: ${SONARR_API_KEY}" \
        "http://localhost:8989/api/v3/customformat" 2>/dev/null) || {
        fail "Sonarr /api/v3/customformat unreachable"
        return
    }
    local expected=(
        "AV1 codec (UNWATCHABLE on Pi 4)"
        "x265/HEVC 1080p+"
        "HDR / Dolby Vision"
        "Remux / Raw-HD"
        "x264 codec (BONUS)"
        "Quality release groups"
        "Low-bitrate size-optimized groups"
        "Malware/scam executable in title"
        "Known scam aggregator branding"
        "Non-English title signals"
        "Release language is not the original"
    )
    local result
    result=$(python3 - "${response}" "${expected[@]}" <<'PYEOF'
import json, sys
data = json.loads(sys.argv[1])
expected = set(sys.argv[2:])
got = {cf["name"] for cf in data}
missing = expected - got
print("FAIL:missing=" + ",".join(sorted(missing)) if missing else f"OK:{len(got)} formats present")
PYEOF
)
    if [[ "${result}" == OK* ]]; then
        pass "Sonarr Custom Formats applied (${result#OK:})"
    else
        fail "Sonarr Custom Formats drift: ${result#FAIL:}"
    fi
}

check_qbit_auth() {
    header "qBittorrent auth (post-Step-7.5 hardening)"
    # Step 7.5 disables localhost-bypass and applies QBITTORRENT_ADMIN_PASSWORD
    # as the WebUI password. After that, login with no/wrong password should
    # FAIL and login with the .env password should SUCCEED. We test both
    # halves to guarantee the bypass really is off.
    local cookie_jar; cookie_jar=$(mktemp)
    trap 'rm -f "${cookie_jar}"' RETURN

    # Login with correct password — must succeed
    local login_ok=0
    if curl -fsS -c "${cookie_jar}" -X POST \
        "http://localhost:8080/api/v2/auth/login" \
        -d "username=admin" \
        --data-urlencode "password=${QBITTORRENT_ADMIN_PASSWORD}" 2>/dev/null \
        | grep -q "Ok."; then
        login_ok=1
    fi
    if [[ "${login_ok}" -eq 1 ]]; then
        pass "qBit login with .env password succeeds"
    else
        fail "qBit login with .env password FAILED — Step 7.5 may not have run"
        return
    fi

    # Localhost bypass should be OFF — login with WRONG password must fail.
    # We hit setPreferences with no auth cookie; if bypass were on, this
    # would return 200 even with no cookie.
    local bypass_disabled=0
    if ! curl -fsS -X POST \
        "http://localhost:8080/api/v2/app/setPreferences" \
        --data-urlencode 'json={"web_ui_username":"admin"}' \
        >/dev/null 2>&1; then
        bypass_disabled=1
    fi
    if [[ "${bypass_disabled}" -eq 1 ]]; then
        pass "Localhost auth bypass is OFF (setPreferences without cookie rejected)"
    else
        fail "Localhost auth bypass is STILL ON — Step 7.5 hardening did not stick"
    fi
}

check_kiosk_qbit_password() {
    header "Kiosk qBit password env var (MDB_QBIT_PASS)"
    if grep -q '^MDB_QBIT_PASS=' "${ENV_FILE}"; then
        local kiosk_pw
        kiosk_pw=$(grep '^MDB_QBIT_PASS=' "${ENV_FILE}" | cut -d= -f2-)
        local qbit_pw
        qbit_pw=$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)
        if [[ -n "${kiosk_pw}" && "${kiosk_pw}" == "${qbit_pw}" ]]; then
            pass "MDB_QBIT_PASS matches QBITTORRENT_ADMIN_PASSWORD"
        else
            fail "MDB_QBIT_PASS present but does not match QBITTORRENT_ADMIN_PASSWORD"
        fi
    else
        fail "MDB_QBIT_PASS missing from .env (Step 7.6 didn't run; kiosk will fail to auth)"
    fi
}

check_live_search() {
    header "Live indexer search ('${SEARCH_QUERY}' → Prowlarr)"
    # Prowlarr's /api/v1/search hits all enabled indexers and aggregates
    # the results. If our indexer chain is healthy, a popular query should
    # return MANY releases. <10 means most indexers are down or the
    # Newznab proxy URLs in the DB-injected indexers are wrong.
    local response
    response=$(curl -fsS -G -H "X-Api-Key: ${PROWLARR_API_KEY}" \
        --data-urlencode "query=${SEARCH_QUERY}" \
        --data-urlencode "type=search" \
        "http://localhost:9696/api/v1/search" 2>/dev/null) || {
        fail "Prowlarr /api/v1/search unreachable"
        return
    }

    local count
    count=$(echo "${response}" | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
    print(len(data) if isinstance(data, list) else 0)
except Exception:
    print(0)
")
    if [[ "${count}" -ge "${SEARCH_MIN_RESULTS}" ]]; then
        pass "Indexer chain returning live results (${count} releases for '${SEARCH_QUERY}')"
    elif [[ "${count}" -gt 0 ]]; then
        fail "Indexer chain weak: only ${count} results for '${SEARCH_QUERY}' (want ≥${SEARCH_MIN_RESULTS}). Some indexers may be unreachable."
    else
        fail "Indexer chain DEAD: 0 results for '${SEARCH_QUERY}'. Check Gluetun, indexer health, Cloudflare/Byparr."
    fi
}

check_no_active_cooldowns() {
    header "Radarr indexer cooldowns (none should be active)"
    # Radarr's IndexerFactory parks failing indexers for up to 24 hours
    # by persisting a DisabledTill timestamp to radarr.db. Even after the
    # network heals, those cooldowns silently keep the indexer out of
    # rotation until the timestamp expires. The boot-time cooldown-reset
    # oneshot (magic-dingus-clear-cooldowns.service) should keep this
    # table clean — if this check fires it means the oneshot didn't run
    # OR new failures piled up since boot (worth investigating).
    local db="/opt/magic_dingus_box/services/config/radarr/radarr.db"
    if [[ ! -f "${db}" ]]; then
        fail "Radarr DB not found at ${db}"
        return
    fi
    local active
    active=$(sudo python3 -c "
import sqlite3
con = sqlite3.connect('${db}')
cur = con.cursor()
cur.execute('SELECT COUNT(*) FROM IndexerStatus WHERE DisabledTill IS NOT NULL')
print(cur.fetchone()[0])
con.close()
" 2>/dev/null) || {
        fail "Could not query IndexerStatus table"
        return
    }
    if [[ "${active}" -eq 0 ]]; then
        pass "No indexer cooldowns active"
    else
        fail "${active} indexer(s) in cooldown — run /usr/local/bin/clear_radarr_cooldowns.py to clear"
    fi
}

# ── Main ────────────────────────────────────────────────────────────
require_env

log "Running Media Browser stack smoke test..."
log "  ENV_FILE=${ENV_FILE}"

check_radarr_indexers
check_radarr_download_client
check_radarr_quality_profile
check_radarr_custom_formats
check_radarr_root_folder
check_sonarr_root_folder
check_sonarr_indexers
check_sonarr_download_client
check_sonarr_quality_profile
check_sonarr_custom_formats
check_qbit_auth
check_kiosk_qbit_password
check_no_active_cooldowns
check_live_search

# Summary
TOTAL=$((PASS_COUNT + FAIL_COUNT))
SKIP_NOTE=""
if [[ "${SKIP_COUNT}" -gt 0 ]]; then
    SKIP_NOTE=" (${SKIP_COUNT} skipped — Sonarr not provisioned)"
fi
echo ""
echo "======================================================================"
if [[ "${FAIL_COUNT}" -eq 0 ]]; then
    echo "  ✓ All ${TOTAL} smoke-test checks PASSED${SKIP_NOTE}"
    echo "======================================================================"
    exit 0
else
    echo "  ✗ ${FAIL_COUNT} of ${TOTAL} smoke-test checks FAILED:"
    for msg in "${FAILURES[@]}"; do
        echo "    - ${msg}"
    done
    echo "======================================================================"
    exit 1
fi
