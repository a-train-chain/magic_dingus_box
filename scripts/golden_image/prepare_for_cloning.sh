#!/usr/bin/env bash
#
# Magic Dingus Box - Prepare for live SD clone
#
# Quiesces the running Pi just enough to take a consistent SD-card
# snapshot via `dd` over SSH WITHOUT permanently destroying any user
# data. The companion script restore_after_cloning.sh undoes everything
# this does, leaving the source Pi in the same state it was before.
#
# What this does (safe to re-run; idempotent):
#   1. Stop kiosk + Content Manager + Docker stack so the FS quiets down
#   2. Snapshot per-Pi identity that needs to differ on each clone
#      (device_info.json + hostname) into a backup dir, then remove
#      them from disk so the cloned image will trigger first_boot.sh
#      to regenerate fresh ones.
#   3. Re-enable magic-first-boot.service (it self-disabled on this
#      Pi long ago; we want it to fire on the cloned Pi's first boot)
#   4. sync; sync; sync (flush dirty pages to SD)
#
# After this script completes, the SD card is in a "ready to be cloned"
# state. The Mac-side orchestrator dd's it and then SSHes back to run
# restore_after_cloning.sh which puts everything back.
#
# CRITICAL: do not reboot the source Pi between this script and
# restore_after_cloning.sh. If you do, first_boot.sh will fire on the
# source and regenerate its identity (which is fine, but then the
# `restore` script's identity-restore will conflict).
#
# Must run as root (operator's Mac-side script runs this via sudo over
# SSH).
#

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
INSTALL_DIR="/opt/magic_dingus_box"
CPP_DIR="${INSTALL_DIR}/magic_dingus_box_cpp"
DATA_DIR="${CPP_DIR}/data"
SERVICES_DIR="${INSTALL_DIR}/services"
BACKUP_DIR="/var/lib/magic-dingus-box/cloning_backup"

DEVICE_INFO_PATH="${DATA_DIR}/device_info.json"
HOSTNAME_PATH="/etc/hostname"
HOSTS_PATH="/etc/hosts"
MARKER_PATH="${BACKUP_DIR}/in_progress"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
log() {
    echo "$1"
    logger -t "magic-prepare-clone" "$1" 2>/dev/null || true
}

log "=== prepare_for_cloning.sh starting ==="

# ---------------------------------------------------------------------------
# Preflight: must run as root
# ---------------------------------------------------------------------------
if [[ "$EUID" -ne 0 ]]; then
    log "ERROR: must be run as root (operator script invokes via sudo)"
    exit 1
fi

# ---------------------------------------------------------------------------
# Preflight: refuse to run twice without a restore in between
# ---------------------------------------------------------------------------
if [[ -f "$MARKER_PATH" ]]; then
    log "ERROR: prepare-for-cloning marker already present at $MARKER_PATH"
    log "       This means a previous clone attempt did not complete its restore."
    log "       Run /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh"
    log "       first to put the Pi back into normal state, then retry."
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 1: Stop services
# ---------------------------------------------------------------------------
log "[1/5] Stopping kiosk + Content Manager + Docker stack..."

# Order matters: stop the kiosk first (it holds DRM master / EGL context),
# then content manager, then the Docker stack last (it has the most state
# to flush). All `|| true` because each may already be stopped.
systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
systemctl stop magic-dingus-web.service 2>/dev/null || true

# Stop docker-compose stack via the systemd unit so its ExecStop runs
# (which calls `docker compose down` cleanly). reset-failed first so a
# unit currently in failed state (which can happen if the docker stack
# was previously restarted manually outside systemd) doesn't block the
# stop.
systemctl reset-failed magic-dingus-services.service 2>/dev/null || true
systemctl stop magic-dingus-services.service 2>/dev/null || true

# Belt-and-suspenders: if the systemd unit didn't catch the docker stack,
# do it directly. This handles the case where containers were started
# manually outside the systemd unit.
if [[ -f "$SERVICES_DIR/docker-compose.yml" ]] && command -v docker &>/dev/null; then
    (cd "$SERVICES_DIR" && docker compose down 2>&1 | sed 's/^/    /' || true)
fi

log "[1/5] Services stopped"

# ---------------------------------------------------------------------------
# Step 2: Backup per-Pi identity, then remove from disk
# ---------------------------------------------------------------------------
log "[2/5] Backing up per-Pi identity to ${BACKUP_DIR}..."

mkdir -p "$BACKUP_DIR"
chmod 700 "$BACKUP_DIR"

# Snapshot device_info.json (used by first_boot.sh to gate identity gen)
if [[ -f "$DEVICE_INFO_PATH" ]]; then
    cp -p "$DEVICE_INFO_PATH" "$BACKUP_DIR/device_info.json"
    rm -f "$DEVICE_INFO_PATH"
    log "[2/5] Snapshotted + removed device_info.json"
else
    log "[2/5] device_info.json not present — skipping (first_boot will create one)"
fi

# Snapshot /etc/hostname so we can restore it. The cloned Pi's
# first_boot.sh will set its own hostname from a fresh device_id.
if [[ -f "$HOSTNAME_PATH" ]]; then
    cp -p "$HOSTNAME_PATH" "$BACKUP_DIR/hostname"
    log "[2/5] Snapshotted /etc/hostname (current=$(cat "$HOSTNAME_PATH"))"
fi

# /etc/hosts also has the hostname embedded; snapshot it for restore.
if [[ -f "$HOSTS_PATH" ]]; then
    cp -p "$HOSTS_PATH" "$BACKUP_DIR/hosts"
fi

# We deliberately do NOT remove /etc/ssh/ssh_host_* here — the operator's
# active SSH session needs them valid until restore_after_cloning.sh runs.
# The clone no longer inherits them regardless: first_boot.sh Step 1 now
# rotates unconditionally (it used to skip whenever keys existed, which on a
# dd clone is always, making it dead code).

# ---------------------------------------------------------------------------
# Step 2b: Strip operator credentials from the FAT boot partition
# ---------------------------------------------------------------------------
# /boot/firmware is FAT32 and unencrypted. Anyone who puts a shipped SD card
# into any laptop reads these with no root, no login and no tooling — and the
# distributed .img.gz carries them too:
#
#   network-config  — the operator's home SSID and Wi-Fi PSK, in plaintext
#   user-data       — the `magic` account's password hash (identical on every
#                     unit, so one offline crack yields local login fleet-wide)
#                     and the operator's SSH public key
#
# first_boot.sh Step 6c only ever wiped /etc/NetworkManager, which is one of
# several places the PSK lives, and is on the ext4 partition rather than the
# one a customer can trivially read.
#
# Backups go to TMPFS, not $BACKUP_DIR: $BACKUP_DIR lives on the SD card and
# would therefore be captured by the dd, putting the secret straight back into
# the image it was just removed from. /dev/shm is RAM and never reaches the
# card. restore_after_cloning.sh reads them back from there.
#
# Each file is OVERWRITTEN IN PLACE before unlinking. `rm` only detaches the
# directory entry; the bytes stay in the free space that dd faithfully copies.
BOOT_FW="/boot/firmware"
BOOT_STASH="/dev/shm/mdb-boot-stash"

if [[ -d "$BOOT_FW" ]]; then
    log "[2b/5] Stripping operator credentials from ${BOOT_FW}..."
    mkdir -p "$BOOT_STASH"
    chmod 700 "$BOOT_STASH"

    for f in user-data network-config meta-data; do
        src="${BOOT_FW}/${f}"
        [[ -f "$src" ]] || continue
        cp -p "$src" "${BOOT_STASH}/${f}"
        # Shred the contents, then remove. On FAT there is no journal to
        # defeat, so an in-place overwrite genuinely clears the sectors.
        sz=$(stat -c %s "$src" 2>/dev/null || echo 0)
        if [[ "$sz" -gt 0 ]]; then
            dd if=/dev/zero of="$src" bs=1 count="$sz" conv=notrunc status=none 2>/dev/null || true
            sync
        fi
        rm -f "$src"
        # macOS AppleDouble sidecars written when the card is mounted on a Mac.
        # ._network-config holds a copy of the resource fork and has been seen
        # to contain indexed fragments of the same data.
        rm -f "${BOOT_FW}/._${f}"
        log "[2b/5]   removed ${f}"
    done

    # Spotlight index written by macOS when the boot partition was mounted —
    # it holds an indexed copy of the SSID string.
    rm -rf "${BOOT_FW}/.Spotlight-V100" "${BOOT_FW}/.fseventsd" 2>/dev/null || true
    sync
    log "[2b/5] Boot partition cleaned (stash: ${BOOT_STASH})"
else
    log "[2b/5] No ${BOOT_FW} directory — skipping boot-partition clean"
fi

# ---------------------------------------------------------------------------
# Step 2c: Remove application secrets from the SD before dd
# ---------------------------------------------------------------------------
# first_boot.sh Step 6 already wipes most of these on the CLONE's first boot,
# and that remains the safety net. It is not sufficient on its own, because it
# only protects the running unit — not the .img.gz artifact. The image file
# gets stored, copied to a laptop, kept on a shared drive, maybe emailed. Every
# copy of it contains, until first boot happens:
#
#   services/.env                 ProtonVPN WireGuard PRIVATE key, the
#                                 qBittorrent admin password, Radarr/Prowlarr
#                                 API keys
#   data/flask_secret.key         phone-remote HMAC secret — a phone paired to
#                                 ONE box would authenticate against every box
#                                 flashed from this image
#   build/data/flask_secret.key   byte-identical SECOND copy that first_boot.sh
#                                 does NOT wipe (it only knows about data/)
#   tmdb_api_key                  operator's personal TMDB key, wiped by nothing
#
# Anyone who reads a flashed card before its first boot, or whose first_boot.sh
# aborts partway (it runs under `set -euo pipefail`), keeps all of it.
#
# Same tmpfs discipline as Step 2b: stash in RAM, overwrite in place, then
# unlink. Restored by restore_after_cloning.sh.
#   services/config/radarr/config.xml       Radarr API key
#   services/config/prowlarr/config.xml     Prowlarr API key
#   services/config/sonarr/config.xml       Sonarr API key
#   services/config/radarr/radarr.db        the qBittorrent WebUI username and
#                                           password, in plaintext, inside
#                                           DownloadClients.Settings
#   services/config/sonarr/sonarr.db        same structure as radarr.db — the
#                                           qBittorrent WebUI username and
#                                           password again, in plaintext,
#                                           inside its own DownloadClients.Settings
#   services/config/prowlarr/prowlarr.db    the Radarr API key again, in the
#                                           Applications row used for app sync
#   services/config/qbittorrent/.../qBittorrent.conf
#                                           WebUI\Password_PBKDF2 hash
#
# These four were missing from this list even though first_boot.sh Step 6 knows
# about them and deletes services/config/* on the clone. That safety net has the
# same limitation as everything else here: it protects the running unit, not the
# .img.gz. So the qBittorrent password survived in radarr.db inside the artifact
# even though the operator had just wiped the .env copy of the very same
# password one line above.
#
# The SQLite -wal and -shm sidecars are listed explicitly. Removing a .db while
# leaving its -wal behind would both leave the secret readable in the WAL and
# corrupt the database on restore, since the two must travel together.
SECRET_STASH="/dev/shm/mdb-secret-stash"
SECRET_PATHS=(
    "/opt/magic_dingus_box/services/.env"
    "/opt/magic_dingus_box/magic_dingus_box_cpp/data/flask_secret.key"
    "/opt/magic_dingus_box/magic_dingus_box_cpp/build/data/flask_secret.key"
    "/home/magic/.config/magic_dingus_box/tmdb_api_key*"
    "/opt/magic_dingus_box/services/config/radarr/config.xml"
    "/opt/magic_dingus_box/services/config/radarr/radarr.db"
    "/opt/magic_dingus_box/services/config/radarr/radarr.db-wal"
    "/opt/magic_dingus_box/services/config/radarr/radarr.db-shm"
    "/opt/magic_dingus_box/services/config/prowlarr/config.xml"
    "/opt/magic_dingus_box/services/config/prowlarr/prowlarr.db"
    "/opt/magic_dingus_box/services/config/prowlarr/prowlarr.db-wal"
    "/opt/magic_dingus_box/services/config/prowlarr/prowlarr.db-shm"
    "/opt/magic_dingus_box/services/config/sonarr/config.xml"
    "/opt/magic_dingus_box/services/config/sonarr/sonarr.db"
    "/opt/magic_dingus_box/services/config/sonarr/sonarr.db-wal"
    "/opt/magic_dingus_box/services/config/sonarr/sonarr.db-shm"
    "/opt/magic_dingus_box/services/config/qbittorrent/qBittorrent/qBittorrent.conf"
)

# Expand the glob entries. This list has to match the SHAPE of what
# first_boot.sh wipes on the clone: it globs tmdb_api_key*, so a stray
# tmdb_api_key.bak on the source Pi was cleaned off the running unit at first
# boot while remaining fully readable inside the .img.gz artifact — which is
# the one place these secrets must never survive, because the image is what
# gets copied around. Only entries containing '*' are re-split, so literal
# paths are untouched.
_expanded=()
shopt -s nullglob
for _entry in "${SECRET_PATHS[@]}"; do
    if [[ "$_entry" == *'*'* ]]; then
        for _match in $_entry; do _expanded+=("$_match"); done
    else
        _expanded+=("$_entry")
    fi
done
shopt -u nullglob
SECRET_PATHS=("${_expanded[@]}")
unset _expanded _entry _match

log "[2c/5] Removing application secrets from the SD..."
mkdir -p "$SECRET_STASH"
chmod 700 "$SECRET_STASH"
: > "${SECRET_STASH}/manifest"
chmod 600 "${SECRET_STASH}/manifest"

for src in "${SECRET_PATHS[@]}"; do
    [[ -f "$src" ]] || continue
    # Flatten the path into a stash filename, recording the original so restore
    # can put each file back exactly where it came from — along with its mode
    # and ownership. Restore used to hardcode `chown magic` + `chmod 600`, which
    # silently changed these service files from their real 1000:1000 / 644 and
    # is wrong for anything a container owns. Recording the real values keeps
    # restore an exact inverse. Older stashes carry only two fields; restore
    # falls back to its previous behaviour when mode/uid/gid are absent.
    key="$(printf '%s' "$src" | tr '/' '_')"
    cp -p "$src" "${SECRET_STASH}/${key}"
    mode="$(stat -c %a "$src" 2>/dev/null || echo '')"
    uid="$(stat -c %u "$src" 2>/dev/null || echo '')"
    gid="$(stat -c %g "$src" 2>/dev/null || echo '')"
    printf '%s\t%s\t%s\t%s\t%s\n' "$key" "$src" "$mode" "$uid" "$gid" \
        >> "${SECRET_STASH}/manifest"
    sz=$(stat -c %s "$src" 2>/dev/null || echo 0)
    if [[ "$sz" -gt 0 ]]; then
        # Block size matters now that radarr.db (~2.6MB) and its WAL (~2.9MB)
        # are on the list: bs=1 issues one write syscall per byte, which took
        # milliseconds for a 500-byte key but would grind for minutes on a
        # multi-megabyte database. Round up to whole 64K blocks — overshooting
        # the end is harmless because the file is unlinked immediately after.
        blocks=$(( (sz + 65535) / 65536 ))
        dd if=/dev/zero of="$src" bs=64K count="$blocks" conv=notrunc status=none 2>/dev/null || true
        sync
    fi
    rm -f "$src"
    log "[2c/5]   removed ${src}"
done
sync
log "[2c/5] Application secrets removed (stash: ${SECRET_STASH})"

# ---------------------------------------------------------------------------
# Step 3: Re-enable magic-first-boot.service
# ---------------------------------------------------------------------------
# This Pi already ran first_boot.sh long ago, which self-disabled the
# unit. We want it ENABLED in the cloned image so it fires on the
# cloned Pi's first boot. restore_after_cloning.sh disables it again
# afterwards so the source Pi doesn't run first-boot logic if it
# reboots later.
log "[3/5] Re-enabling magic-first-boot.service (so cloned Pi runs it)..."

# Self-heal: the unit file may never have been installed on this Pi
# (only prepare_golden_image.sh historically installed it; deploy_cpp.sh
# Step 1.65 now does too, but older deployments predate that). Without
# the unit file, `systemctl enable` fails and set -euo pipefail aborts
# the clone. Install from the repo copy synced to ${INSTALL_DIR}/systemd/.
UNIT_INSTALLED="/etc/systemd/system/magic-first-boot.service"
UNIT_SRC="${INSTALL_DIR}/systemd/magic-first-boot.service"
if [[ ! -f "$UNIT_INSTALLED" ]]; then
    if [[ -f "$UNIT_SRC" ]]; then
        cp "$UNIT_SRC" "$UNIT_INSTALLED"
        systemctl daemon-reload
        log "[3/5] Installed missing magic-first-boot.service unit from ${UNIT_SRC}"
    else
        log "ERROR: magic-first-boot.service unit not installed and no copy at ${UNIT_SRC}"
        log "       Run deploy_cpp.sh from the dev machine first (it syncs systemd/ units),"
        log "       then retry the clone. Without this unit the cloned image would never"
        log "       run first_boot.sh (no identity reset, no WiFi/VPN credential wipe)."
        exit 1
    fi
fi

if systemctl is-enabled magic-first-boot.service &>/dev/null; then
    log "[3/5] magic-first-boot.service was already enabled (unusual but harmless)"
else
    systemctl enable magic-first-boot.service 2>&1 | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
# Step 4: Drop the in-progress marker so a Pi reboot mid-clone leaves
# evidence for restore_after_cloning.sh to find.
# ---------------------------------------------------------------------------
log "[4/5] Marking clone in progress at ${MARKER_PATH}..."

cat > "$MARKER_PATH" <<EOF
# Magic Dingus Box - clone in progress
# Created: $(date -u +%Y-%m-%dT%H:%M:%SZ)
# Hostname at prep time: $(hostname)
#
# This file exists ONLY between prepare_for_cloning.sh and
# restore_after_cloning.sh. If you see it after a reboot, the previous
# clone attempt did not complete — run restore_after_cloning.sh to
# put the Pi back into normal state.
EOF
chmod 600 "$MARKER_PATH"

# ---------------------------------------------------------------------------
# Step 5: Flush dirty pages to SD so dd reads consistent state
# ---------------------------------------------------------------------------
log "[5/5] Flushing dirty pages to SD card..."

sync
sync
# Drop pagecache to force dd to read directly from the SD (vs cached pages)
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
sync

log "=== prepare_for_cloning.sh complete ==="
log ""
log "The Pi is now ready to be dd'd. The Mac-side script will:"
log "  1. Stream /dev/mmcblk0 over this SSH session"
log "  2. SSH back to run restore_after_cloning.sh when done"
log ""
log "DO NOT reboot the source Pi until restore completes."
