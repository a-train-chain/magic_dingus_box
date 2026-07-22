#!/bin/bash
# Import existing movies from the attached MOVIES drive into Radarr.
#
# Product flow: MDB units store movies on an external drive labeled
# MOVIES, mounted at /mnt/ssd (fstab: LABEL=MOVIES ... x-systemd.automount).
# When a drive with a pre-built library (/mnt/ssd/library/<Title (Year)>/)
# is attached to a box whose Radarr doesn't know those movies yet — fresh
# provision, replacement SD, swapped drive — this script registers each
# unmapped folder with Radarr so the kiosk's Library screen shows them.
#
# Idempotent: movies Radarr already tracks are skipped (matched by path).
# Unmatchable folders are reported and skipped, never fatal.
#
# Runs on the Pi. Requires the services stack up (Radarr on :7878).
set -euo pipefail

ENV_FILE="/opt/magic_dingus_box/services/.env"
# shellcheck disable=SC1090
source "${ENV_FILE}"

: "${RADARR_API_KEY:?RADARR_API_KEY missing from ${ENV_FILE}}"

# Wait for Radarr (mount-triggered runs can race the stack at boot).
for _ in $(seq 1 30); do
    if curl -s -o /dev/null --max-time 3 "http://localhost:7878/ping"; then
        break
    fi
    sleep 5
done

python3 - "${RADARR_API_KEY}" <<'PYEOF'
import json, sys, time, urllib.parse, urllib.request

api_key = sys.argv[1]
BASE = "http://localhost:7878/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=60) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

roots = http("GET", "/rootfolder")
if not roots:
    print("no Radarr root folder configured — run setup_services.sh first")
    sys.exit(1)

profile_id = http("GET", "/qualityprofile")[0]["id"]

imported, skipped, unmatched = [], [], []
for root in roots:
    for folder in root.get("unmappedFolders", []):
        name, path = folder["name"], folder["path"]
        # "Title (Year)" -> lookup term. Radarr's lookup handles the
        # year suffix natively and ranks exact-year matches first.
        results = http("GET", "/movie/lookup?term=" + urllib.parse.quote(name))
        if not results:
            unmatched.append(name)
            continue
        movie = results[0]
        movie.update({
            "rootFolderPath": root["path"],
            "path": path,
            "monitored": True,
            "qualityProfileId": profile_id,
            "addOptions": {"searchForMovie": False, "monitor": "movieOnly"},
        })
        try:
            added = http("POST", "/movie", movie)
            imported.append(name)
            # Rescan so Radarr registers the existing file immediately.
            http("POST", "/command", {"name": "RescanMovie", "movieId": added["id"]})
            time.sleep(0.3)
        except urllib.error.HTTPError as e:
            # 400 with "already been added" == another path for skip.
            skipped.append(name)

existing = len(http("GET", "/movie"))
print(json.dumps({"imported": imported, "unmatched": unmatched,
                  "skipped": skipped, "total_in_radarr": existing}, indent=1))
PYEOF
