#!/usr/bin/env bash
#
# Magic Dingus Box - Restore source Pi after live SD clone
#
# Undoes prepare_for_cloning.sh. Restores the per-Pi identity that was
# snapshotted away, disables magic-first-boot.service so this Pi
# doesn't run first-boot logic on its next reboot, and starts the
# kiosk + Docker stack back up.
#
# Idempotent: if the marker is missing and no secret stash is pending
# (i.e. nothing to restore), it's a no-op with a friendly message rather
# than an error. This matters because the Mac-side orchestrator runs this
# in a trap handler that may fire even on the happy path. Re-running after
# a partial or interrupted restore finishes the job; it never copies a
# stash back twice (see clone_stash_lib.sh, the `restored` flag).
#
# Reboot recovery: the secrets prepare removed are stashed on the movie
# drive (/mnt/ssd/.mdb-secret-stash), so a source Pi that rebooted or lost
# power mid-clone is recovered by booting it with the drive attached and
# running this script. It exits NON-ZERO, loudly, instead of quietly
# restoring nothing when:
#   - the stash is on a drive that is not mounted (attach it, re-run), or
#   - a stash was started and is gone (e.g. the --allow-ram-stash RAM stash
#     after a reboot). --accept-secret-loss acknowledges that and lets the
#     rest of the restore bring the box back up without those files.
#
# Must run as root.
#

set -euo pipefail

ACCEPT_SECRET_LOSS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --accept-secret-loss) ACCEPT_SECRET_LOSS=1; shift ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

STASH_LIB="$(dirname "${BASH_SOURCE[0]}")/clone_stash_lib.sh"
if [[ ! -f "$STASH_LIB" ]]; then
    echo "ERROR: ${STASH_LIB} is missing — cannot locate the secret stash." >&2
    echo "       Nothing has been restored. Re-deploy scripts/golden_image/ and re-run." >&2
    exit 1
fi
# shellcheck source=clone_stash_lib.sh
source "$STASH_LIB"

INSTALL_DIR="/opt/magic_dingus_box"
CPP_DIR="${INSTALL_DIR}/magic_dingus_box_cpp"
DATA_DIR="${CPP_DIR}/data"
BACKUP_DIR="/var/lib/magic-dingus-box/cloning_backup"

DEVICE_INFO_PATH="${DATA_DIR}/device_info.json"
HOSTNAME_PATH="/etc/hostname"
HOSTS_PATH="/etc/hosts"
MARKER_PATH="${BACKUP_DIR}/in_progress"

log() {
    echo "$1"
    logger -t "magic-restore-clone" "$1" 2>/dev/null || true
}

log "=== restore_after_cloning.sh starting ==="

if [[ "$EUID" -ne 0 ]]; then
    log "ERROR: must be run as root"
    exit 1
fi

# ---------------------------------------------------------------------------
# Idempotency guard
# ---------------------------------------------------------------------------
if [[ ! -f "$MARKER_PATH" ]]; then
    if mdb_secret_stash_pending "$BACKUP_DIR"; then
        # No marker, but a stash is waiting. first_boot.sh on a box whose root
        # has no SD CID cannot tell the source from a clone and wipes
        # cloning_backup/ (marker included) — while the stash on the movie
        # drive survives. Restoring it is the only way those secrets come back.
        log "WARNING: no clone-in-progress marker, but an unrestored secret stash"
        log "         exists — restoring it."
    else
        log "No clone-in-progress marker found at ${MARKER_PATH}"
        log "Nothing to restore. (This is fine — restore was a no-op.)"
        exit 0
    fi
fi

# Disarm first-boot BEFORE anything below can fail: this script runs under
# set -e, and an abort in step 1 used to leave the service enabled, so the
# source's next reboot ran first_boot.sh against it. Step 2 repeats this
# (idempotent) for the log line.
systemctl disable magic-first-boot.service &>/dev/null || true

# ---------------------------------------------------------------------------
# Step 0: Application secrets — FIRST, and fatal on failure
# ---------------------------------------------------------------------------
# Everything prepare_for_cloning.sh Step 2b/2c removed from the SD (the
# boot-partition cloud-init files, services/.env with the VPN key, the
# Radarr/Sonarr/Prowlarr databases, config.xml files and Backups/ zips,
# flask_secret.key, the TMDB key, the Wi-Fi profile, the operator's SSH/git
# identity, ...) is in ONE manifest-driven stash, normally on the movie drive.
# Each file goes back to its exact path with its recorded owner and mode, and
# is verified byte-for-byte; only then is the stash shredded.
#
# This runs before anything else so that a failure leaves the box untouched
# for a clean re-run: drive not mounted, stash gone, or a file that will not
# go back all exit NON-ZERO here, with the marker and the stash kept. The old
# behaviour — "No application-secret stash found (nothing to restore)", exit
# 0 — is exactly how a reboot mid-clone used to cost a box its libraries
# without a single error on screen.
set +e
mdb_restore_secrets "$BACKUP_DIR" "$ACCEPT_SECRET_LOSS"
_secrets_rc=$?
set -e
if [[ "$_secrets_rc" -ne 0 ]]; then
    log "ERROR: restore stopped at the secret stash (code ${_secrets_rc}). Identity,"
    log "       content and services have NOT been restored; the clone-in-progress"
    log "       marker is kept. Fix the problem above and re-run this script."
    exit 1
fi
unset _secrets_rc

# The stash includes the Wi-Fi profile (*.nmconnection). The active connection
# survived in NM's memory while the file was gone (NM does not watch
# connection files); reload so NM's file view matches again rather than
# waiting for the next reboot — or, after a reboot mid-clone, so the restored
# profile is picked up at all. Best-effort; harmless no-op when nothing changed.
nmcli connection reload 2>/dev/null || true

# ---------------------------------------------------------------------------
# Step 1: Restore per-Pi identity files from backup
# ---------------------------------------------------------------------------
log "[1/4] Restoring per-Pi identity from ${BACKUP_DIR}..."

if [[ -f "$BACKUP_DIR/device_info.json" ]]; then
    mkdir -p "$(dirname "$DEVICE_INFO_PATH")"
    cp -p "$BACKUP_DIR/device_info.json" "$DEVICE_INFO_PATH"
    chown magic:magic "$DEVICE_INFO_PATH" 2>/dev/null || true
    log "[1/4] Restored device_info.json"
fi

if [[ -f "$BACKUP_DIR/hostname" ]]; then
    cp -p "$BACKUP_DIR/hostname" "$HOSTNAME_PATH"
    # hostnamectl picks it up live (without reboot)
    hostnamectl set-hostname "$(cat "$HOSTNAME_PATH")" 2>/dev/null || true
    log "[1/4] Restored /etc/hostname ($(cat "$HOSTNAME_PATH"))"
fi

if [[ -f "$BACKUP_DIR/hosts" ]]; then
    cp -p "$BACKUP_DIR/hosts" "$HOSTS_PATH"
    log "[1/4] Restored /etc/hosts"
fi

# LEGACY boot-partition stash. Current prepare puts the boot-partition
# cloud-init files (Wi-Fi PSK, `magic` password hash, operator SSH key) into
# the shared secret stash restored in Step 0. An older prepare kept them in
# their own /dev/shm directory; restore it if one is still around so an
# in-flight clone started by the old script is never stranded.
BOOT_FW="/boot/firmware"
BOOT_STASH="$MDB_LEGACY_BOOT_STASH_DIR"

if [[ -d "$BOOT_STASH" ]]; then
    restored=0
    for f in user-data network-config meta-data; do
        if [[ -f "${BOOT_STASH}/${f}" ]]; then
            cp -p "${BOOT_STASH}/${f}" "${BOOT_FW}/${f}" 2>/dev/null && restored=$((restored + 1))
        fi
    done
    sync
    # Wipe the RAM stash so the credentials do not linger in /dev/shm, which
    # is world-readable by default.
    rm -rf "$BOOT_STASH"
    log "[1/4] Restored ${restored} legacy boot-partition file(s) and cleared that stash"
fi

# (Application secrets were restored and verified in Step 0 above.)

# Curated content (the operator's own playlists/videos) that prepare moved to
# the disk-backed stash on the movie drive so the artifact would not carry
# them. mv'd back exactly where they came from. This stash survives a reboot
# (it is on the SSD, not tmpfs), so a crashed clone loses nothing.
CONTENT_STASH="$MDB_CONTENT_STASH_DIR"

if [[ -f "${CONTENT_STASH}/manifest" ]]; then
    restored=0
    while IFS=$'\t' read -r key dest; do
        [[ -n "$key" && -n "$dest" ]] || continue
        [[ -f "${CONTENT_STASH}/${key}" ]] || continue
        mkdir -p "$(dirname "$dest")"
        mv "${CONTENT_STASH}/${key}" "$dest"
        restored=$((restored + 1))
    done < "${CONTENT_STASH}/manifest"
    sync
    rm -rf "$CONTENT_STASH"
    log "[1/4] Restored ${restored} curated content file(s) and cleared the content stash"
else
    log "[1/4] No curated-content stash found (nothing to restore)"
fi

# ---------------------------------------------------------------------------
# Step 1b: Repair zerofill aftermath the trap could not reach
# ---------------------------------------------------------------------------
# prepare's own trap handles the normal failure modes (including SIGHUP from a
# dropped ssh link), but SIGKILL, an OOM kill, or a power cut bypass traps
# entirely. That leaves multi-GB junk fill files on disk and — worse — the
# ext4 root reserve at 0, with the correct count existing nowhere in shell
# memory. prepare persists the count to reserve_blocks for exactly this case.
rm -f /var/tmp/mdb-zerofill.tmp /var/tmp/mdb-zerofill-tail.tmp \
      /boot/firmware/.mdb-zerofill.tmp 2>/dev/null || true

if [[ -f "${BACKUP_DIR}/reserve_blocks" ]]; then
    read -r _blocks _dev < "${BACKUP_DIR}/reserve_blocks" || true
    if [[ -n "${_blocks:-}" && -n "${_dev:-}" ]]; then
        _current=$(tune2fs -l "$_dev" 2>/dev/null \
            | awk -F: '/^Reserved block count/{gsub(/ /,"",$2); print $2}' || true)
        if [[ "${_current:-}" != "0" ]]; then
            # Reserve is intact (prepare's own unwind got there first).
            rm -f "${BACKUP_DIR}/reserve_blocks"
        elif tune2fs -r "$_blocks" "$_dev" >/dev/null 2>&1; then
            log "[1/4] Restored the ${_blocks}-block root reserve on ${_dev} (a crashed zerofill had left it at 0)"
            rm -f "${BACKUP_DIR}/reserve_blocks"
        else
            # Keep the file: it is the only surviving record of the correct
            # count, and a re-run can retry once the underlying problem is
            # fixed.
            log "[1/4] WARNING: root reserve on ${_dev} is 0 and could not be restored — run: sudo tune2fs -r ${_blocks} ${_dev}"
        fi
    else
        rm -f "${BACKUP_DIR}/reserve_blocks"
    fi
    unset _blocks _dev _current
fi

# ---------------------------------------------------------------------------
# Step 2: Disable magic-first-boot.service
# ---------------------------------------------------------------------------
# We re-enabled it during prepare so the cloned image would run it.
# On the source Pi we DO NOT want it firing (it would wipe all the
# state we just restored). Disable now.
log "[2/4] Disabling magic-first-boot.service on source Pi..."

if systemctl is-enabled magic-first-boot.service &>/dev/null; then
    systemctl disable magic-first-boot.service 2>&1 | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
# Step 3: Restart services
# ---------------------------------------------------------------------------
log "[3/4] Starting services back up..."

# Order matters: Docker stack first (kiosk's Media Browser depends on
# Radarr being reachable), then content manager, then the kiosk last.
# reset-failed first so a previously-failed unit can be restarted.
# The container-runtime daemons first. prepare_for_cloning.sh stops dockerd and
# containerd outright (not just the compose stack) so neither can rewrite its
# secret-bearing metadata after the free-space zeroing has run. Nothing below
# can start without them: magic-dingus-services shells out to `docker compose`.
systemctl start containerd.service 2>/dev/null || true
systemctl start docker.socket 2>/dev/null || true
systemctl start docker.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: docker.service failed to start — the stack cannot come up without it"

systemctl reset-failed magic-dingus-services.service 2>/dev/null || true
systemctl start magic-dingus-services.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-services.service failed to start (check docker logs)"

systemctl start magic-dingus-web.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-web.service failed to start"

systemctl start magic-dingus-box-cpp.service 2>&1 | sed 's/^/    /' || \
    log "[3/4] WARN: magic-dingus-box-cpp.service failed to start"

# Re-arm the front-panel standby switch, which prepare stopped so a bump
# of the toggle could not restart services underneath the dd.
if systemctl is-enabled kiosk-standby-watcher.service &>/dev/null; then
    systemctl start kiosk-standby-watcher.service 2>&1 | sed 's/^/    /' || \
        log "[3/4] WARN: kiosk-standby-watcher.service failed to start"
fi

# Re-arm the periodic units and the gluetun cascade watcher, which prepare
# stopped so nothing could write to the SD behind the zerofill and the dd.
# Same enabled-guard pattern as the standby watcher: an unprovisioned box
# (no VPN yet) never enabled these, and starting them there would just fail.
for _u in gluetun-cascade-restart.service \
          qbit-port-sync.timer \
          magic-dingus-auto-blocklist.timer \
          magic-dingus-missing-search.timer \
          magic-dingus-smoke-test.timer; do
    if systemctl is-enabled "$_u" &>/dev/null; then
        systemctl start "$_u" 2>/dev/null || \
            log "[3/4] WARN: ${_u} failed to start"
    fi
done
unset _u

# ---------------------------------------------------------------------------
# Step 4: Clear the in-progress marker + cleanup
# ---------------------------------------------------------------------------
log "[4/4] Removing clone-in-progress marker..."

# The stash pointer goes BEFORE the marker (Step 0 normally removed it
# already): a pointer that outlived its marker would read as a lost stash.
rm -f "${BACKUP_DIR}/${MDB_STASH_POINTER_NAME}"
rm -f "$MARKER_PATH"

# Backup files no longer needed; remove them so a future prepare run
# starts from a clean slate. fstab.before-clone is included: the corrected
# MOVIES line is deliberately permanent (better on the source box too), so
# the pre-fix snapshot is only clutter that kept the rmdir failing forever.
# reserve_blocks is NOT force-removed here — Step 1b keeps it when a zero
# reserve could not be repaired, and that record must survive.
rm -f "$BACKUP_DIR/device_info.json" "$BACKUP_DIR/hostname" "$BACKUP_DIR/hosts" \
      "$BACKUP_DIR/fstab.before-clone"
rmdir "$BACKUP_DIR" 2>/dev/null || true

log "=== restore_after_cloning.sh complete ==="
log ""
log "Source Pi is fully restored. Verify services with:"
log "  systemctl is-active magic-dingus-box-cpp.service"
log "  systemctl is-active magic-dingus-services.service"
log "  docker ps"
