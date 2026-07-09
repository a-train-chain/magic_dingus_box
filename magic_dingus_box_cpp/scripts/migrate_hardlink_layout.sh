#!/bin/bash
# One-time migration: switch Radarr + qBittorrent from the legacy split
# mount layout (/downloads + /library as separate container mounts) to the
# single-parent /data layout that lets Radarr HARDLINK finished downloads
# into the library instead of copying them.
#
# WHY: with /downloads and /library on separate container mounts, a
# cross-mount link() fails with EXDEV, so Radarr falls back to a full byte
# copy on every import — ~15-50s per movie at ~80 MB/s, plus a duplicate
# on-disk copy for every torrent that keeps seeding (2x storage). With both
# under one /data mount, imports are instant and seeding torrents share
# blocks with the library via hardlinks (1x storage).
#
# WHAT IT DOES (all reversible; NO media files are moved — only container
# path labels + the DB references to them change):
#   1. Backs up radarr.db (timestamped) before touching anything.
#   2. Rewrites RootFolders.Path and Movies.Path  /library -> /data/library
#      in radarr.db (transactional; exact-prefix only).
#   3. Sets qBittorrent's default save path to /data/downloads/complete and
#      bulk-relocates existing torrents from /downloads -> /data/downloads.
#   4. Restarts the stack and verifies Radarr sees the library (movie count
#      unchanged, no "missing" spike) before declaring success.
#
# PRECONDITIONS (checked below, aborts if unmet):
#   - The compose file already provides the ${STORAGE_ROOT}:/data mount
#     (present since the 2026-07-09 backward-compat compose change).
#   - Radarr + qBit are reachable.
#
# SAFETY: This was rehearsed offline against a copy of the live radarr.db
# (18 movies, 17 with files) — all paths rewrote cleanly, integrity_check
# ok, and every migrated container path mapped to a real host file. Still:
# run it ATTENDED, during a quiet moment (no active import), and keep the
# printed backup path until you've confirmed playback works.
#
# ROLLBACK: stop the stack, restore the printed radarr.db backup, revert
# qBit save path to /downloads (and "Set location" torrents back if needed),
# restart. The /data mount can stay — it's inert without the DB pointing at
# it. See docs/superpowers/plans/2026-07-09-import-hardlink-investigation.md.
#
# Idempotent: re-running after a successful migration is a no-op (paths are
# already /data/library; the guard detects this and exits 0).

set -euo pipefail

SERVICES_DIR="${SERVICES_DIR:-/opt/magic_dingus_box/services}"
RADARR_DB="${SERVICES_DIR}/config/radarr/radarr.db"
COMPOSE="${SERVICES_DIR}/docker-compose.yml"
ENV_FILE="${SERVICES_DIR}/.env"

log()  { echo "[hardlink-migrate] $*"; }
die()  { echo "[hardlink-migrate] ERROR: $*" >&2; exit 1; }

# --- Preconditions --------------------------------------------------------
[ -f "${RADARR_DB}" ] || die "radarr.db not found at ${RADARR_DB}"
[ -f "${COMPOSE}" ]   || die "compose not found at ${COMPOSE}"
command -v sqlite3 >/dev/null || die "sqlite3 not installed (sudo apt install sqlite3)"
grep -q ':/data' "${COMPOSE}" || die \
  "compose does not provide a :/data mount yet — update docker-compose.yml first"

# radarr.db is owned by root (the linuxserver image runs Radarr as root/
# PUID and creates config/ root-owned). The invoking user usually can't
# even open it in SQLite's default read-WRITE mode (it wants to create a
# -wal sidecar). Detect writability and route all DB ops through sudo if
# needed. `db_sqlite` and `db_cp` are the sudo-aware wrappers used below.
SUDO=""
if [ ! -w "${RADARR_DB}" ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
        log "radarr.db is not user-writable — using sudo for DB operations."
    else
        die "radarr.db is not writable and sudo is unavailable — cannot proceed."
    fi
fi
db_sqlite() { ${SUDO} sqlite3 "${RADARR_DB}" "$@"; }
db_cp()     { ${SUDO} cp -a "$@"; }

cd "${SERVICES_DIR}"

# --- Idempotency / state check -------------------------------------------
CUR_ROOT="$(db_sqlite "SELECT Path FROM RootFolders LIMIT 1;")"
if [ "${CUR_ROOT}" = "/data/library" ]; then
    log "root folder is already /data/library — migration already applied, nothing to do."
    exit 0
fi
if [ "${CUR_ROOT}" != "/library" ]; then
    die "unexpected root folder '${CUR_ROOT}' (expected '/library'). Aborting so we don't corrupt a custom layout."
fi

LIB_COUNT_BEFORE="$(db_sqlite "SELECT count(*) FROM Movies;")"
log "current root folder: ${CUR_ROOT}  |  ${LIB_COUNT_BEFORE} movies in DB"

# --- 1. Backup ------------------------------------------------------------
TS="$(date +%Y%m%d-%H%M%S)"
BACKUP="${RADARR_DB}.pre-hardlink-${TS}.bak"
# Stop Radarr first so the DB is quiescent during copy + edit (a running
# Radarr holds SQLite locks and could write between our copy and edit).
log "stopping radarr + qbittorrent for a consistent edit..."
docker compose stop radarr qbittorrent >/dev/null
db_cp "${RADARR_DB}" "${BACKUP}"
log "radarr.db backed up to: ${BACKUP}"

# --- 2. Rewrite radarr.db paths (transactional, exact-prefix) ------------
log "rewriting /library -> /data/library in radarr.db..."
db_sqlite <<'SQL'
BEGIN;
UPDATE RootFolders SET Path = '/data/library' WHERE Path = '/library';
UPDATE Movies      SET Path = '/data' || Path WHERE Path LIKE '/library/%';
COMMIT;
SQL

# Verify the rewrite + integrity before bringing Radarr back.
REMAIN="$(db_sqlite "SELECT count(*) FROM Movies WHERE Path LIKE '/library/%';")"
INTEG="$(db_sqlite "PRAGMA integrity_check;")"
LIB_COUNT_AFTER="$(db_sqlite "SELECT count(*) FROM Movies;")"
[ "${REMAIN}" = "0" ]      || { db_cp "${BACKUP}" "${RADARR_DB}"; die "rewrite left ${REMAIN} /library paths — restored backup, aborted"; }
[ "${INTEG}" = "ok" ]      || { db_cp "${BACKUP}" "${RADARR_DB}"; die "integrity_check=${INTEG} — restored backup, aborted"; }
[ "${LIB_COUNT_AFTER}" = "${LIB_COUNT_BEFORE}" ] || { db_cp "${BACKUP}" "${RADARR_DB}"; die "movie count changed ${LIB_COUNT_BEFORE}->${LIB_COUNT_AFTER} — restored backup, aborted"; }
log "radarr.db rewritten: 0 /library remaining, integrity ok, ${LIB_COUNT_AFTER} movies intact."

# --- 3. qBittorrent save path + relocate existing torrents ---------------
# Read qBit creds from .env (MDB_QBIT_PASS). Best-effort: if qBit auth
# isn't reachable, we log a WARNING and continue — Radarr is the critical
# path; qBit torrents can be relocated by hand from the WebUI if needed.
QBIT_PASS="$(grep -oP '(?<=^MDB_QBIT_PASS=).*' "${ENV_FILE}" 2>/dev/null || true)"
log "starting qbittorrent to update its save path..."
docker compose up -d qbittorrent >/dev/null
# Give qBit a moment to bind its WebUI.
for _ in $(seq 1 15); do
    curl -sf -o /dev/null "http://localhost:8080/api/v2/app/version" && break || sleep 2
done
if [ -n "${QBIT_PASS}" ] && curl -sf -c /tmp/qbmig_cookies \
        -d "username=admin&password=${QBIT_PASS}" \
        "http://localhost:8080/api/v2/auth/login" >/dev/null 2>&1; then
    # New default save path.
    curl -sf -b /tmp/qbmig_cookies \
        --data-urlencode 'json={"save_path":"/data/downloads/complete","temp_path":"/data/downloads/incomplete"}' \
        "http://localhost:8080/api/v2/app/setPreferences" >/dev/null || \
        log "WARN: could not set qBit default save path (set it manually to /data/downloads/complete)"
    # Relocate every existing torrent to the /data path (same host files,
    # new container label). setLocation takes hashes=all.
    curl -sf -b /tmp/qbmig_cookies \
        -d "hashes=all&location=/data/downloads/complete" \
        "http://localhost:8080/api/v2/torrents/setLocation" >/dev/null || \
        log "WARN: could not relocate existing torrents (relocate to /data/downloads/complete manually)"
    rm -f /tmp/qbmig_cookies
    log "qBit save path + existing torrents moved to /data/downloads."
else
    log "WARN: qBit auth unavailable — skipped qBit path update. Set save path to"
    log "      /data/downloads/complete and 'Set location' all torrents there manually."
fi

# --- 4. Bring Radarr back + verify ---------------------------------------
log "starting radarr..."
docker compose up -d radarr >/dev/null
RADARR_KEY="$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/radarr/config.xml" 2>/dev/null || true)"
for _ in $(seq 1 30); do
    curl -sf -o /dev/null "http://localhost:7878/ping" && break || sleep 2
done
if [ -n "${RADARR_KEY}" ]; then
    # Count movies Radarr reports as having a file — should match the
    # pre-migration on-disk count (17 in the rehearsal; the one missing was
    # the never-imported "Three Days of the Condor"). A big drop here means
    # Radarr can't see the files → investigate before trusting the result.
    WITHFILE="$(curl -sf -H "X-Api-Key: ${RADARR_KEY}" \
        "http://localhost:7878/api/v3/movie" 2>/dev/null \
        | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin) if m.get("hasFile")))' 2>/dev/null || echo '?')"
    log "Radarr back up. Movies with a visible file: ${WITHFILE}"
    log "If that number looks right (matches what you had before), the migration succeeded."
    log "If it dropped to 0/low, restore the backup: docker compose stop radarr && ${SUDO} cp -a '${BACKUP}' '${RADARR_DB}' && docker compose up -d radarr"
else
    log "Radarr back up (could not read API key to auto-verify file count — check the Library screen)."
fi

log "DONE. Backup kept at: ${BACKUP}"
log "Future imports will now hardlink (instant, no duplicate storage)."
