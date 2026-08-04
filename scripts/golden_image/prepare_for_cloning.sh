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
# Preflight: secret tripwire — refuse to clone with stray secret-bearing
# backups on disk
# ---------------------------------------------------------------------------
# The SECRET_PATHS machinery below strips every secret this script KNOWS
# about. What it cannot know about is operator debris: ad-hoc backups made
# during maintenance sessions. This is not hypothetical — the 2026-08-03
# pre-golden-image audit found /home/magic/db-backup-*/radarr.db carrying the
# qBittorrent password in plaintext, plus .env backups, none of which any
# scrub list named. A dd would have shipped them in every unit.
#
# Rather than trying to enumerate-and-stash every possible backup name (the
# exact failure mode that bit the Backups/ zips), ABORT and make the operator
# delete or relocate them. Globs are intentionally broad; a false positive
# costs a minute of moving a file, a false negative ships a credential.
shopt -s nullglob
TRIPWIRE_HITS=(
    /home/magic/db-backup-*
    /home/magic/*.env.backup* /home/magic/.env.backup*
    /home/magic/*.env.bak* /home/magic/env-backup*
    /home/magic/state_backup*
    /home/magic/.magic_dingus_box_backup*
    /home/magic/*secret* /home/magic/*credential*
    /root/db-backup-* /root/*.env*
)
shopt -u nullglob
if [[ ${#TRIPWIRE_HITS[@]} -gt 0 ]]; then
    log "ERROR: secret tripwire — refusing to clone while these exist:"
    for hit in "${TRIPWIRE_HITS[@]}"; do
        log "         $hit"
    done
    log "       These look like operator backups that may carry credentials"
    log "       (API keys, the qBittorrent password, VPN keys). Delete them"
    log "       or move them off the SD card (e.g. to /mnt/ssd or another"
    log "       machine), then re-run. The clone captures every byte on the SD."
    exit 1
fi
log "Preflight: secret tripwire clean"

# ---------------------------------------------------------------------------
# Preflight: drop the in-progress marker BEFORE touching anything
# ---------------------------------------------------------------------------
# This used to live in Step 4, after Step 2 removed device_info.json, Step 2b
# emptied the boot partition and Step 2c shredded ~250 secret files. But
# restore_after_cloning.sh exits 0 the instant the marker is ABSENT ("nothing
# to restore"), so a failure anywhere in that window left the operator with a
# stripped box, an orphaned /dev/shm stash, and a restore script that
# cheerfully declined to do anything. The window covered every destructive
# step in the script.
#
# The marker is the record that destructive work has begun, so it has to be
# written before the first destructive step, not after the last one. Writing
# it early costs nothing: restore removes it, and the re-run guard above
# already refuses to start when it is present.
mkdir -p "$BACKUP_DIR"
chmod 700 "$BACKUP_DIR"
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
log "Preflight: clone-in-progress marker written (restore is now armed)"

# ---------------------------------------------------------------------------
# Step 1: Stop services
# ---------------------------------------------------------------------------
log "[1/5] Stopping kiosk + Content Manager + Docker stack..."

# Order matters: stop the kiosk first (it holds DRM master / EGL context),
# then content manager, then the Docker stack last (it has the most state
# to flush). All `|| true` because each may already be stopped.
# The standby watcher goes FIRST and must not be forgotten. It sits on
# GPIO 3 for the lifetime of the box, so with it running the front-panel
# toggle is still armed during the clone — one bump of the switch mid-dd
# starts the kiosk and the whole Docker stack back up underneath a copy
# that is supposed to be reading a frozen filesystem, and the resulting
# image is silently inconsistent. restore_after_cloning.sh starts it
# again on the way out.
systemctl stop kiosk-standby-watcher.service 2>/dev/null || true

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
#   services/config/<app>/config.xml        that app's API key
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
# These were missing from this list even though first_boot.sh Step 6 knows
# about them and deletes services/config/* on the clone. That safety net has the
# same limitation as everything else here: it protects the running unit, not the
# .img.gz. So the qBittorrent password survived in radarr.db inside the artifact
# even though the operator had just wiped the .env copy of the very same
# password one line above.
#
# The SQLite -wal and -shm sidecars are listed explicitly. Removing a .db while
# leaving its -wal behind would both leave the secret readable in the WAL and
# corrupt the database on restore, since the two must travel together.
#
#   services/config/<app>/Backups/**        the app's OWN scheduled-backup task
#                                           re-packages config.xml AND <app>.db
#                                           into a zip — every secret the
#                                           entries above strip, put back into
#                                           the artifact under a filename
#                                           nothing here was watching
#   services/config/<app>/logs/**           a verbatim record of the operator's
#   services/config/<app>/logs.db           own searches, grabs and imports
#   services/config/<app>/logs.db-wal
#   services/config/<app>/logs.db-shm
#
# Backups/ is the reason this list is now generated instead of typed. The
# entries above name radarr.db and config.xml explicitly; the app then writes
# both of them, on a timer, into
# Backups/scheduled/<app>_backup_<version>_<timestamp>.zip — a path outside
# every pattern this script knew. Measured on the live box before this commit:
#
#   radarr/Backups/scheduled/*.zip    Radarr API key, Prowlarr API key, and the
#                                     qBittorrent WebUI password in plaintext
#   prowlarr/Backups/scheduled/*.zip  Prowlarr API key, Radarr API key
#
# So the artifact shipped the qBittorrent password inside a zip sitting on the
# same disk where the .env copy of that password had just been shredded — the
# exact leak this list exists to prevent, routed around by the apps themselves.
# Sonarr had no Backups/ yet only because its scheduled task had not fired since
# install; it inherits the identical exposure within a week.
#
# logs/ and logs.db go in on a weaker but real basis, and the difference is
# worth stating rather than blurring: current *arr builds DO redact credentials.
# Every apikey occurrence in this box's 64 MB of logs reads `apikey=(removed)`,
# and no live key value appears anywhere in them. They are stripped anyway
# because (a) the artifact's safety must not rest on a third-party scrubber
# staying correct across app upgrades we do not control, (b) the logs are a
# verbatim record of the operator's personal activity — 158 Grabbed/Importing
# lines on this box — which has no business shipping to a customer, and (c) they
# are 64 MB of dead weight in every .img.gz.
#
# Generated per-app rather than enumerated, because the way this gap opened is a
# literal list that did not grow. Sonarr arrived in Phase 2a and every one of
# its paths had to be hand-copied in; the next *arr app is one word in ARR_APPS.
ARR_APPS=(radarr sonarr prowlarr)

SECRET_STASH="/dev/shm/mdb-secret-stash"
SECRET_PATHS=(
    "/opt/magic_dingus_box/services/.env"
    "/opt/magic_dingus_box/magic_dingus_box_cpp/data/flask_secret.key"
    "/opt/magic_dingus_box/magic_dingus_box_cpp/build/data/flask_secret.key"
    "/home/magic/.config/magic_dingus_box/tmdb_api_key*"
    "/opt/magic_dingus_box/services/config/qbittorrent/qBittorrent/qBittorrent.conf"
    # qBittorrent's own state, beyond the one conf file this list used to
    # name. BT_backup holds a .torrent and a .fastresume per download the
    # box has ever taken — file names, sizes and tracker URLs — so the
    # artifact shipped a readable inventory of the operator's entire
    # download history under a path nothing was watching. The rest is
    # session/UI state that no fresh unit should inherit.
    "/opt/magic_dingus_box/services/config/qbittorrent/qBittorrent/data/BT_backup/**"
    "/opt/magic_dingus_box/services/config/qbittorrent/qBittorrent/data/logs/**"
    "/opt/magic_dingus_box/services/config/qbittorrent/qBittorrent/data/GeoDB/**"
    "/opt/magic_dingus_box/services/config/qbittorrent/qBittorrent/watched_folders.json"
    # Watch history (Phase 3). media_browser.db's watch_state table is the
    # operator's complete viewing record — every episode/movie watched, resume
    # positions, timestamps. Personal data, not product content; the kiosk
    # recreates an empty schema-v3 db on first launch. Globbed for the SQLite
    # -wal/-shm sidecars (db and WAL must travel together or the restore is
    # corrupt), and BOTH copies — build/data/ is populated by on-Pi test runs,
    # the same dual-copy trap flask_secret.key already taught us.
    # first_boot.sh Step 6 wipes these on the clone as the on-unit safety net;
    # this entry keeps them out of the .img.gz artifact itself.
    "/opt/magic_dingus_box/magic_dingus_box_cpp/data/media_browser.db*"
    "/opt/magic_dingus_box/magic_dingus_box_cpp/build/data/media_browser.db*"
    # cloud-init's cache on the ROOT filesystem. This is the SAME secrets
    # Step 2b strips from the FAT boot partition, kept a second time on ext4
    # where Step 2b never looked — and /dev/mmcblk0p2 is inside the dd just
    # as much as p1 is. Measured on this box before the fix: 9 user-data /
    # network-config files across three instance directories, of which 2
    # network-config.json carry a Wi-Fi `password` field and 6 user-data
    # carry `ssh_authorized_keys` and the account `passwd` hash. 508 KB
    # total, and every byte of it ships.
    #
    # obj.pkl is in scope and easy to miss: it is the pickled DataSource
    # object, which holds the same user-data inside a binary blob that no
    # grep for "password" would necessarily surface.
    #
    # Wiping the whole per-instance cache is also the RIGHT state for a new
    # unit — it should inherit no instance identity, no completed-module
    # markers, and no seed. Safe for the source Pi: `magic`'s passwordless
    # sudo comes from /etc/sudoers.d/90-cloud-init-users and the account
    # itself from /etc/passwd, neither of which lives here (checked before
    # writing this), and restore_after_cloning.sh puts the cache back before
    # anything reboots.
    "/var/lib/cloud/instances/**"
    "/var/lib/cloud/seed/**"
    # The operator's Wi-Fi profile — carries the home network PSK in
    # plaintext (psk= line, root-readable but faithfully dd'd). first_boot.sh
    # Step 6c wipes it on the clone; this keeps it out of the artifact.
    # Safe to remove while the box is USING that Wi-Fi: NetworkManager does
    # not watch connection files (monitor-connection-files defaults to
    # false), so the active connection lives in memory until a reload or
    # reboot — and restore_after_cloning.sh puts the file back (and reloads
    # NM) before either can happen. The prepare-script header's "do not
    # reboot between prepare and restore" rule was already load-bearing for
    # the boot-partition stash; it now covers Wi-Fi continuity too.
    "/etc/NetworkManager/system-connections/*.nmconnection"
)

for _app in "${ARR_APPS[@]}"; do
    _cfg="/opt/magic_dingus_box/services/config/${_app}"
    SECRET_PATHS+=(
        "${_cfg}/config.xml"
        "${_cfg}/${_app}.db"
        "${_cfg}/${_app}.db-wal"
        "${_cfg}/${_app}.db-shm"
        "${_cfg}/logs.db"
        "${_cfg}/logs.db-wal"
        "${_cfg}/logs.db-shm"
        "${_cfg}/Backups/**"
        "${_cfg}/logs/**"
    )
done
unset _app _cfg

# Expand the glob entries. This list has to match the SHAPE of what
# first_boot.sh wipes on the clone: it globs tmdb_api_key*, so a stray
# tmdb_api_key.bak on the source Pi was cleaned off the running unit at first
# boot while remaining fully readable inside the .img.gz artifact — which is
# the one place these secrets must never survive, because the image is what
# gets copied around. Only entries containing '*' are re-split, so literal
# paths are untouched.
#
# globstar is on so the `Backups/**` and `logs/**` entries recurse. A single '*'
# is not enough: the apps file scheduled backups under Backups/scheduled/ and
# operator-triggered ones under Backups/manual/, so `Backups/*` would match the
# subdirectory and none of the zips inside it. '**' also yields the directories
# themselves, which the `-f` test in the loop below skips — leaving an empty
# Backups/ behind is correct, and restore rebuilds the tree on the way back in
# via its own `mkdir -p "$(dirname "$dest")"`.
_expanded=()
shopt -s nullglob globstar
for _entry in "${SECRET_PATHS[@]}"; do
    if [[ "$_entry" == *'*'* ]]; then
        for _match in $_entry; do _expanded+=("$_match"); done
    else
        _expanded+=("$_entry")
    fi
done
shopt -u nullglob globstar
SECRET_PATHS=("${_expanded[@]}")
unset _expanded _entry _match

log "[2c/5] Removing application secrets from the SD..."
mkdir -p "$SECRET_STASH"
chmod 700 "$SECRET_STASH"
: > "${SECRET_STASH}/manifest"
chmod 600 "${SECRET_STASH}/manifest"

# Adding Backups/ and logs/ took this pass from 15 files / ~7 MB to 86 files /
# ~72 MB on the production box, all of which is copied into $SECRET_STASH —
# tmpfs, i.e. RAM — before being overwritten in place. That is comfortable:
# /dev/shm is 992 MB there, it is empty at this point in the clone because every
# service has already been stopped in Step 1, and 72 MB is 7% of it. The cost is
# a shredding pass over ~72 MB plus one sync per file, seconds on the SSD-backed
# boxes and longer on a slow SD. The count logged below is what confirms the
# pass actually ran over the whole list.
stripped=0

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
    stripped=$((stripped + 1))
    log "[2c/5]   removed ${src}"
done
sync
log "[2c/5] ${stripped} application secret file(s) removed (stash: ${SECRET_STASH})"

# cloud-init's logs are a THIRD copy of the Wi-Fi PSK, after the boot
# partition and /var/lib/cloud. cloud-init renders the whole netplan dict at
# DEBUG on every run, password field included. Measured on this box:
#   /var/log/cloud-init.log        5 plaintext occurrences
#   cloud-init.log.{1,2,3}.gz     17 + 18 + 18
# 58 in total, 784 KB, and nothing removed them.
#
# Deleted outright rather than stashed: they are boot diagnostics for an
# instance the clone will never be, and the source Pi loses nothing it will
# miss. Done HERE, immediately before the leak check below, and not with the
# other log vacuuming in Step 4b — the check runs at this point in the
# script, so a removal after it would leave the guard asserting against files
# that still existed when it looked.
rm -f /var/log/cloud-init.log* /var/log/cloud-init-output.log* 2>/dev/null || true
log "[2c/5] cloud-init logs removed (they render the Wi-Fi PSK at DEBUG)"

# POST-CONDITION: prove the credential-bearing classes are actually gone
# before we hand the card to dd. Every entry in SECRET_PATHS above is a
# pattern someone wrote by hand, and the way this script has failed twice is
# a pattern that quietly matched nothing — the Backups/ zips, then the whole
# cloud-init cache on the root partition. A list that silently covers less
# than it claims is the failure mode, so assert the OUTCOME rather than
# trusting the list.
#
# ABORTS the clone rather than warning: shipping the operator's home Wi-Fi
# password and account hash on every unit is not something to discover from
# a log line nobody read.
leak_found=0
for pat in \
    '/var/lib/cloud -name user-data*' \
    '/var/lib/cloud -name network-config*' \
    '/var/lib/cloud -name obj.pkl' \
    '/etc/NetworkManager/system-connections -name *.nmconnection' \
    '/opt/magic_dingus_box/services -name .env' \
    '/var/log -name cloud-init.log*' \
    '/var/log -name cloud-init-output.log*'; do
    # shellcheck disable=SC2086  # deliberate: $pat carries find's own args
    n=$(find $pat -type f 2>/dev/null | wc -l)
    if [[ "$n" -gt 0 ]]; then
        log "ERROR: post-scrub leak check FAILED — ${n} file(s) still match: ${pat}"
        leak_found=$((leak_found + 1))
    fi
done
if [[ "$leak_found" -gt 0 ]]; then
    log "ERROR: refusing to continue. The image would ship operator credentials."
    log "       Run restore_after_cloning.sh to put the Pi back, then fix SECRET_PATHS."
    exit 1
fi
log "[2c/5] post-scrub leak check clean (no cloud-init, Wi-Fi or .env credentials remain)"

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
log "[4/5] Clone-in-progress marker was written during preflight (see above)"

# ---------------------------------------------------------------------------
# Step 4b: Vacuum system logs — the operator's activity record and dead
# image weight
# ---------------------------------------------------------------------------
# The persistent journal + auditd logs measured 112 MB on the source box at
# audit time. Two reasons they don't ship: (1) they are a verbatim record of
# the source operator's activity — every service start, download, playback
# session, and (in audit.log) every watched-path file deletion with full
# library filenames; (2) they are pure dead weight in every .img.gz copy.
# NOT stash-and-restored: logs are disposable by design, and keeping ~16 MB
# of recent journal preserves enough context for post-clone debugging on the
# source. auditd keeps running with a truncated log — the tripwire rules
# stay armed throughout.
log "[4b/5] Vacuuming journal + audit logs..."

# The journal is a FOURTH copy, by a different route: sudo records every
# command line verbatim, so an operator who joined Wi-Fi with
# `nmcli dev wifi connect <ssid> password <psk>` put the PSK straight into
# the journal. --vacuum-size keeps the NEWEST slices, which is exactly where
# a recent join sits. Vacuum by TIME instead so nothing from this session's
# work survives, then rotate again so the now-active file starts empty.
journalctl --rotate 2>/dev/null || true
journalctl --vacuum-time=1s 2>&1 | tail -1 | sed 's/^/    /' || true
journalctl --rotate 2>/dev/null || true
if [[ -d /var/log/audit ]]; then
    # Truncate in place rather than delete: auditd holds the fd open and
    # keeps writing; a deleted file would hold its blocks invisibly until
    # the daemon restarts. Rotated siblings (audit.log.1 ...) go entirely.
    rm -f /var/log/audit/audit.log.* 2>/dev/null || true
    truncate -s 0 /var/log/audit/audit.log 2>/dev/null || true
    log "[4b/5] audit logs truncated (rules stay armed)"
fi

# ---------------------------------------------------------------------------
# Step 4c: Zero the free space
# ---------------------------------------------------------------------------
# Deleting a file unlinks it; the blocks keep their contents until
# something reuses them, and `dd` copies blocks, not files. So every
# secret this script just shredded a NAMED copy of can still exist in
# free space — along with everything ever deleted on this card. On
# 2026-08-03 roughly 800 MB of operator debris, including an
# environment-file backup with service credentials, was moved off this
# SD to the SSD; a move across filesystems leaves the originals sitting
# unlinked but fully readable right where they were.
#
# The second reason is size. Free space full of old data is
# incompressible noise, so it lands in the .img.gz at close to full
# size; zeroed, it compresses to almost nothing. This step typically
# makes the artifact several GB smaller.
#
# Cost is real: it writes zeros until the filesystem is full, which on a
# slow SD can take 10-20 minutes. Set MDB_SKIP_ZEROFILL=1 to skip it for
# a throwaway image — never for one that leaves the building.
if [[ "${MDB_SKIP_ZEROFILL:-0}" == "1" ]]; then
    log "[4c/5] Free-space zeroing SKIPPED (MDB_SKIP_ZEROFILL=1) — do not ship this image"
else
    # ext4 RESERVES blocks for root — 5% by default, 2.6 GB on this card —
    # and `df` does not count them as available. Filling only to df's number
    # therefore leaves that whole reserve holding whatever was deleted from
    # it, and dd copies those blocks verbatim.
    #
    # This is not theoretical: the 2026-08-04 image shipped a deleted
    # cloud-init.log fragment containing
    #   'access-points': {'<SSID>': {'password': '<PSK>'}}
    # sitting in reserved space, found by grepping the finished artifact.
    # The ORIGINAL code ran dd to ENOSPC, which as root does consume the
    # reserve and would have caught it; a later "leave a 64 MB margin"
    # change replaced that with a bounded count and silently reintroduced
    # the leak. Hence: drop the reserve, fill, restore the reserve.
    ROOT_DEV=$(findmnt -no SOURCE /)
    # Restore by exact BLOCK COUNT, not a rounded percentage: this card's
    # reserve is 631957 of 15458816 blocks = 4.088%, which rounds to 4 and
    # would quietly shrink the reserve on every clone.
    RESERVE_BLOCKS=$(tune2fs -l "$ROOT_DEV" 2>/dev/null \
        | awk -F: '/^Reserved block count/{gsub(/ /,"",$2); print $2}')
    RESERVE_RESTORED=0
    if [[ -n "$RESERVE_BLOCKS" ]] && tune2fs -r 0 "$ROOT_DEV" >/dev/null 2>&1; then
        RESERVE_RESTORED=1
    else
        log "[4c/5] WARNING: could not drop the ext4 root reserve on ${ROOT_DEV}."
        log "         ~$(( ${RESERVE_BLOCKS:-0} * 4 / 1024 )) MB of free space will NOT be"
        log "         zeroed and may carry deleted-file remnants into the image."
    fi

    avail_mb=$(df -Pm / | awk 'NR==2 {print $4}')
    log "[4c/5] Zeroing ~${avail_mb} MB of free space incl. the ${RESERVE_PCT}% root reserve (10-20 min on SD)..."

    # Keep a small margin so the filesystem never actually hits zero free —
    # journald, systemd and the rest of this script still need to write while
    # the clone runs.
    ZERO_FILE=/var/tmp/mdb-zerofill.tmp
    zero_mb=$(( avail_mb > 64 ? avail_mb - 64 : 0 ))
    if [[ "$zero_mb" -gt 0 ]]; then
        dd if=/dev/zero of="$ZERO_FILE" bs=4M count=$(( zero_mb / 4 )) \
           status=none 2>/dev/null || true
    fi
    sync
    rm -f "$ZERO_FILE"
    sync
    if [[ "$RESERVE_RESTORED" == "1" ]]; then
        tune2fs -r "$RESERVE_BLOCKS" "$ROOT_DEV" >/dev/null 2>&1 \
            || log "[4c/5] WARNING: failed to restore the ${RESERVE_BLOCKS}-block root reserve"
    fi

    # The FAT boot partition has its own free space and was never zeroed at
    # all. Step 2b overwrites the NAMED cloud-init files there, but FAT
    # rewrites a file to fresh clusters rather than in place, so earlier
    # copies persist as unlinked remnants — three fragments of network-config
    # naming the operator's SSID were found in the shipped image at ~20 MB.
    BOOT_ZERO=/boot/firmware/.mdb-zerofill.tmp
    boot_avail=$(df -Pm /boot/firmware 2>/dev/null | awk 'NR==2 {print $4}')
    if [[ -n "$boot_avail" && "$boot_avail" -gt 8 ]]; then
        log "[4c/5] Zeroing ~$((boot_avail - 4)) MB of boot-partition free space..."
        dd if=/dev/zero of="$BOOT_ZERO" bs=1M count=$(( boot_avail - 4 )) \
           status=none 2>/dev/null || true
        sync
        rm -f "$BOOT_ZERO"
        sync
    fi
    # fstrim on top: on cards whose controller honors discard this also
    # clears the flash translation layer's copies, which dd never sees
    # but a chip-off reader would. Harmless no-op where unsupported.
    fstrim / 2>/dev/null || true
    log "[4c/5] Free space zeroed ($(df -Pm / | awk 'NR==2 {print $4}') MB free again)"
fi

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
