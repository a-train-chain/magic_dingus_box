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
    log "       or move them off the SD card, then re-run. The clone captures"
    log "       every byte on the SD. NOTE: /mnt/ssd only counts as off-card"
    log "       when the movie drive is actually mounted there — verify with"
    log "       'mountpoint /mnt/ssd' first, or the files land on the SD and"
    log "       ship anyway."
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
log "Preflight: marking clone in progress at ${MARKER_PATH}..."

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

# The periodic units and the gluetun cascade watcher must stop too, and the
# watcher must stop BEFORE dockerd does. Every one of these writes to the SD
# on its own schedule — qbit-port-sync every 60 s, auto-blocklist every
# 15 min — so leaving them running repeats, at machine cadence, the exact
# "daemon rewrites data AFTER the fill" failure documented below for dockerd:
# journal writes and state files landing in free space the zerofill already
# passed over, then dd copying them verbatim. The cascade watcher is
# Restart=always; once dockerd stops, its `docker events` subscription dies
# and systemd respawns it every 10 s for the whole clone, each attempt
# logging a fresh failure. restore_after_cloning.sh starts all of these
# again on the way out.
CLONE_QUIESCE_UNITS=(
    gluetun-cascade-restart.service
    qbit-port-sync.timer
    magic-dingus-auto-blocklist.timer
    magic-dingus-missing-search.timer
    magic-dingus-smoke-test.timer
)
for _u in "${CLONE_QUIESCE_UNITS[@]}"; do
    systemctl stop "$_u" 2>/dev/null || true
done
unset _u
log "[1/5] Periodic timers + cascade watcher stopped for the duration of the clone"

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

# Now stop the DAEMONS themselves. Stopping the compose stack leaves dockerd
# and containerd running, and they keep rewriting their own metadata —
# /var/lib/docker/containers/*/config.v2.json and containerd's BoltDB at
# /var/lib/containerd/io.containerd.metadata.v1.bolt/meta.db. Both embed every
# container's ENVIRONMENT, which for gluetun means WIREGUARD_PRIVATE_KEY in
# plaintext.
#
# Two distinct problems, and this fixes the harder one. The live copies are
# handled by SECRET_PATHS below. But a daemon that is still running rewrites
# those files AFTER Step 4c has finished zeroing, freeing old blocks that the
# fill already passed over — so stale copies of the key land in free space
# where nothing will ever overwrite them, and dd copies them verbatim.
# Measured on the 2026-08-04 image: the key appeared 5 times while only 2 live
# on-SD files contained it.
#
# docker.socket must go too, or the next docker command activates dockerd
# again on demand.
systemctl stop docker.socket 2>/dev/null || true
systemctl stop docker.service 2>/dev/null || true
systemctl stop containerd.service 2>/dev/null || true
log "[1/5] Docker + containerd daemons stopped (they rewrite secret-bearing metadata)"

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
    # Globbed, not the bare literal: an operator `.env.bak` / `.env.old` made
    # during a maintenance session carries the same credentials as .env itself,
    # sat outside every pattern (the tripwire scans only /home/magic and
    # /root), and would have passed the leak check below, which also tested
    # `-name .env` exactly. The glob catches .env and every sibling.
    "/opt/magic_dingus_box/services/.env*"
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
        # GLOB, not three literal suffixes. migrate_hardlink_layout.sh:95
        # writes "${RADARR_DB}.pre-hardlink-<ts>.bak" and never deletes it —
        # a full copy of radarr.db carrying the qBittorrent password and the
        # Radarr API key — and setup_services.sh actively tells the operator
        # to run that migration. `radarr.db` as a literal matched the live DB
        # and nothing else, so the backup shipped in the artifact. It is a
        # live file, so Step 4c's free-space fill never had any bearing on it.
        # `.db*` also picks up -wal/-shm/-journal without naming each one.
        "${_cfg}/${_app}.db*"
        "${_cfg}/logs.db*"
        "${_cfg}/*.bak"
        "${_cfg}/Backups/**"
        "${_cfg}/logs/**"
        # Poster/fanart cache for every title in the operator's library — a
        # browsable picture-book of what they watch, same disclosure class as
        # BT_backup/** which this list already strips on those grounds.
        # first_boot.sh:306 deletes it on the CLONE, which is exactly why it
        # went unnoticed in the IMAGE.
        "${_cfg}/MediaCover/**"
        "${_cfg}/UpdateLogs/**"
    )
done
unset _app _cfg

# Shell history. This is a verbatim record of everything the operator typed on
# the source box, and maintenance sessions put credentials on the command line
# routinely: `nmcli dev wifi connect <ssid> password <psk>`, curl calls with an
# API key in the URL, sqlite queries against radarr.db. It is exactly the class
# of file the whole scrub exists for.
#
# It is NOT a new discovery that this matters: the legacy destructive path in
# prepare_golden_image.sh has cleaned .bash_history for both accounts since it
# was written. The live-clone path — the one that actually produces the golden
# image today — simply never inherited it, so this has been shipping in every
# artifact this script has ever made.
#
# These go through the normal stash-and-restore machinery, so the operator does
# not lose their own history; it is removed for the duration of the dd and put
# back afterwards.
SECRET_PATHS+=(
    "/home/magic/.bash_history"
    "/root/.bash_history"
    "/home/magic/.zsh_history"
    "/root/.zsh_history"
    "/home/magic/.python_history"
    "/root/.python_history"
    "/home/magic/.sqlite_history"
    "/home/magic/.mysql_history"
    "/home/magic/.lesshst"
    "/root/.lesshst"
)

# Phone-remote per-Pi state. EVERY other layer already classifies these as
# per-Pi secrets — deploy_cpp.sh excludes them from rsync, update.sh excludes
# them on every OTA path, first_boot.sh wipes them from data/ AND build/data/,
# migrate_box.sh migrates them as sensitive — and SECRET_PATHS carried only
# flask_secret.key of the set. So the running unit was protected and the
# artifact was not, which is the precise asymmetry this script's own Step 2c
# header argues against.
#
# text_input_queue.jsonl is the sharp end: the phone remote appends one JSONL
# event PER CHARACTER typed, and one of the fields it is used to type into is
# the Wi-Fi password keyboard. Hence the *.jsonl glob rather than a name list
# — first_boot.sh globs for the same reason, after a hand-maintained list
# there missed remote_viewport_diag.jsonl.
for _d in data build/data; do
    SECRET_PATHS+=(
        "/opt/magic_dingus_box/magic_dingus_box_cpp/${_d}/paired_remotes.json"
        "/opt/magic_dingus_box/magic_dingus_box_cpp/${_d}/pairing_session.json"
        "/opt/magic_dingus_box/magic_dingus_box_cpp/${_d}/pairing_audit.log"
        "/opt/magic_dingus_box/magic_dingus_box_cpp/${_d}/pending_revocations.txt"
        "/opt/magic_dingus_box/magic_dingus_box_cpp/${_d}/seek_request.json"
        "/opt/magic_dingus_box/magic_dingus_box_cpp/${_d}/kiosk_status.json"
        "/opt/magic_dingus_box/magic_dingus_box_cpp/${_d}/"'*.jsonl'
    )
done
unset _d

# Dot-prefixed staging siblings. admin.py:545 stages atomic writes with
# `prefix=f".{path.name}."`, so an interrupted save leaves
# `.tmdb_api_key.<rand>.tmp` / `.flask_secret.key.<rand>.tmp` — and a leading
# dot defeats both the `tmdb_api_key*` glob and the two literal
# flask_secret.key entries. first_boot.sh justifies its own glob with "a
# half-written .tmp from an interrupted save"; the only .tmp this codebase
# produces is the one neither list matched.
SECRET_PATHS+=(
    "/home/magic/.config/magic_dingus_box/*tmdb_api_key*"
    "/opt/magic_dingus_box/magic_dingus_box_cpp/data/*flask_secret.key*"
    "/opt/magic_dingus_box/magic_dingus_box_cpp/build/data/*flask_secret.key*"
)

# The kiosk's own log. config.cpp resolves it to
# /opt/magic_dingus_box/config/magic_dingus_box.log, the unit sets no
# MAGIC_LOG_FILE override, and the sink is DEBUG-level and rotating (5 MB x 3),
# so up to four files of the operator's verbatim activity: Prowlarr query
# strings, movie and series titles, TMDB ids, poster URLs. The legacy
# destructive path has cleaned this since it was written; the live-clone path
# never inherited it, and first_boot.sh does not remove it either — so it ships
# in the image AND then persists on the customer's unit indefinitely.
#
# Stashed rather than deleted: it is the first thing worth reading when the
# source box misbehaves, and /dev/shm has ample room for 20 MB.
SECRET_PATHS+=(
    "/opt/magic_dingus_box/config/magic_dingus_box.log*"
    "/home/magic/retroarch_launcher.log*"
)

# Container-runtime metadata. Docker and containerd each keep their own copy
# of every container's environment, so the WireGuard private key, the *arr API
# keys and the qBittorrent password all exist a second time outside .env —
# and nothing in this script has ever looked at /var/lib/docker.
#
# Found by scanning the finished artifact, not by reading code: the gate
# reported `env:WIREGUARD_PRIVATE_KEY appears 5 time(s)` on an image whose
# .env had been correctly stashed. The scrub strips the credential and the
# infrastructure ships its own copy alongside.
#
# The daemons are stopped above, so these are quiescent and safe to move.
SECRET_PATHS+=(
    "/var/lib/docker/containers/*/config.v2.json"
    "/var/lib/containerd/io.containerd.metadata.v1.bolt/meta.db"
)

# ---------------------------------------------------------------------------
# Content curation: what the golden image SHIPS
# ---------------------------------------------------------------------------
# Not secrets — product curation. The source box is a working development
# machine and accumulates the operator's own playlists and uploaded videos;
# a customer's unit should carry the game playlists plus ONE video playlist
# as a worked example of how to build one, and nothing personal.
#
# Measured on the source box before this existed: 13 playlists and 1.9 GB of
# the operator's own music videos (31 files) shipped in every image.
#
# These are stashed-and-restored like the secrets, so the source box keeps
# every playlist and every video; they are simply absent for the duration of
# the dd. That is deliberately different from pruning in first_boot.sh: doing
# it there would still leave the operator's personal content readable inside
# the .img.gz artifact and on any flashed-but-unbooted card, and would leave
# the image needlessly large.
#
# They do NOT go through SECRET_PATHS. That stash is tmpfs (/dev/shm, ~990 MB
# on the 2 GB board) and the curated media measured 921 MB across 20 files on
# the source box (2026-08-04) — with the ~72 MB secret pass sharing the same
# tmpfs that overflows it, ENOSPC aborts the whole prepare under set -e, and
# long before that the copy is eating RAM the board needs. Bulk content gets
# its own DISK-backed stash on /mnt/ssd below
# (the movie drive is attached on any box being used as a clone source, and
# the SD-only dd never captures it). Content is also not zeroed in place the
# way secrets are: these are music videos, not credentials — the two-stage
# free-space fill in Step 4c overwrites their freed blocks, and even a
# surviving fragment is harmless, so mv (no copy, no shred) is correct.
SHIP_PLAYLISTS=(
    games_arcade.yaml
    games_atari7800.yaml
    games_dreamcast.yaml
    games_genesis.yaml
    games_n64.yaml
    games_nes.yaml
    games_pcengine.yaml
    games_ps1.yaml
    games_snes.yaml
    The_Nostalgia_Channel.yaml
)

CONTENT_STASH="/mnt/ssd/.mdb-content-stash"
CONTENT_PATHS=()
_curated=0
for _d in "${DATA_DIR}" "${CPP_DIR}/build/data"; do
    [[ -d "${_d}/playlists" ]] || continue

    # 1. Playlists that are not on the ship list.
    shopt -s nullglob
    for _pl in "${_d}"/playlists/*.yaml "${_d}"/playlists/*.yml; do
        _base=$(basename "$_pl")
        _keep=0
        for _want in "${SHIP_PLAYLISTS[@]}"; do
            [[ "$_base" == "$_want" ]] && { _keep=1; break; }
        done
        if [[ "$_keep" -eq 0 ]]; then
            CONTENT_PATHS+=("$_pl")
            _curated=$(( _curated + 1 ))
        fi
    done

    # 2. Media files no SHIPPED playlist references.
    #
    # Derived from the kept playlists rather than from a second hand-written
    # list, so removing a playlist from SHIP_PLAYLISTS automatically drops the
    # videos only it used. Entries look like:  path: 'media/Some File.mp4'
    #
    # Basename extraction is sed, NOT `xargs -I{} basename`: xargs treats
    # quotes in its INPUT as shell quoting even with -I, so one media title
    # containing an apostrophe ("Don't Stop Believin'.mp4") aborts xargs with
    # "unmatched single quote", silently drops every remaining reference from
    # the file, and the loop below then curates AWAY media the kept playlist
    # actually uses. Current filenames happen to be apostrophe-free; uploads
    # through the web admin carry no such guarantee.
    [[ -d "${_d}/media" ]] || { shopt -u nullglob; continue; }
    _refs_file=$(mktemp)
    for _want in "${SHIP_PLAYLISTS[@]}"; do
        [[ -f "${_d}/playlists/${_want}" ]] || continue
        # The (-[[:space:]]*)? alternative matters: YAML accepts both the
        # indented form (`    path: ...`) and the first-key-of-item form
        # (`  - path: ...`). Current playlists use the former, but a
        # playlist authored the other way would silently contribute ZERO
        # refs here, and every media file it references would be curated
        # away from under it.
        grep -hE "^[[:space:]]*(-[[:space:]]*)?path:" "${_d}/playlists/${_want}" 2>/dev/null \
            | sed -e "s/^[[:space:]]*-*[[:space:]]*path:[[:space:]]*//" \
                  -e "s/^['\"]//" -e "s/['\"][[:space:]]*$//" \
                  -e 's#.*/##' >> "$_refs_file" || true
    done
    # If the reference list came out EMPTY (unreadable playlists, a schema
    # change in the path: lines), curating would sweep away every media file
    # the shipped example playlist needs. Ship a slightly-larger image instead
    # of a broken one.
    if [[ ! -s "$_refs_file" ]]; then
        log "[2c/5] WARNING: no media references parsed from kept playlists in ${_d} —"
        log "         skipping media curation there (image may carry extra videos)"
        rm -f "$_refs_file"
        shopt -u nullglob
        continue
    fi
    for _m in "${_d}"/media/*; do
        [[ -f "$_m" ]] || continue
        if ! grep -qxF "$(basename "$_m")" "$_refs_file" 2>/dev/null; then
            CONTENT_PATHS+=("$_m")
            _curated=$(( _curated + 1 ))
        fi
    done
    rm -f "$_refs_file"
    shopt -u nullglob
done
unset _d _pl _base _keep _want _m _refs_file
log "[2c/5] Content curation: ${_curated} non-shipping playlist/media file(s) held back from the image"

# Move the curated content to its disk-backed stash NOW, before the secret
# pass. mv preserves mode/ownership (we are root) and the manifest records
# where each file goes back. Aborts rather than degrades if the movie drive
# is not actually mounted: falling back to /dev/shm re-creates the ENOSPC
# failure, and quietly shipping the operator's personal videos is exactly
# what the curation exists to prevent. restore_after_cloning.sh reverses
# this whether or not the rest of prepare completed.
if [[ ${#CONTENT_PATHS[@]} -gt 0 ]]; then
    if ! mountpoint -q /mnt/ssd; then
        log "ERROR: ${#CONTENT_PATHS[@]} curated file(s) need the content stash, but /mnt/ssd"
        log "       is not a mounted drive — stashing there would write to the SD card,"
        log "       which the dd captures. Attach/mount the movie drive and re-run"
        log "       (restore_after_cloning.sh first, to clear the marker)."
        exit 1
    fi
    mkdir -p "$CONTENT_STASH"
    chmod 700 "$CONTENT_STASH"
    : > "${CONTENT_STASH}/manifest"
    chmod 600 "${CONTENT_STASH}/manifest"
    _moved_mb=0
    for _src in "${CONTENT_PATHS[@]}"; do
        [[ -f "$_src" ]] || continue
        _key="$(printf '%s' "$_src" | tr '/' '_')"
        _sz=$(stat -c %s "$_src" 2>/dev/null || echo 0)
        mv "$_src" "${CONTENT_STASH}/${_key}"
        printf '%s\t%s\n' "$_key" "$_src" >> "${CONTENT_STASH}/manifest"
        _moved_mb=$(( _moved_mb + _sz / 1048576 ))
    done
    sync
    log "[2c/5] Curated content moved to ${CONTENT_STASH} (~${_moved_mb} MB, restored after the dd)"
    unset _src _key _sz _moved_mb
fi

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
#
# INVARIANT: only small secret files belong on this list. Bulk content (the
# curated playlists/media above) has its own disk-backed stash on /mnt/ssd —
# 1.9 GB of curated video routed through this loop filled tmpfs mid-copy and
# aborted the whole prepare with ENOSPC. If a future entry can plausibly
# exceed a few hundred MB, it goes in CONTENT_PATHS, not here.
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

# The Wi-Fi profile is named after the network: the file is literally
# "<SSID>.nmconnection". Stashing it removes the CONTENTS, but a deleted
# filename lives on in its parent directory's data block, and that block stays
# ALLOCATED to the directory — so Step 4c's free-space fill can never reach it
# and the SSID rides into the image inside a dead directory entry.
#
# Confirmed on the 2026-08-04 artifact: the PSK was gone (all ten contextual
# framings scored zero) while the SSID still scored one hit, and the only file
# on the box containing it was that filename.
#
# Deleting the now-empty directory frees the block, so the fill DOES cover it,
# and recreating it immediately keeps NetworkManager happy. rmdir refuses on a
# non-empty directory, which is the safety property we want: if anything was
# left behind, this quietly does nothing rather than destroying a live profile.
NM_CONN_DIR=/etc/NetworkManager/system-connections
if [[ -d "$NM_CONN_DIR" ]] && rmdir "$NM_CONN_DIR" 2>/dev/null; then
    mkdir -p "$NM_CONN_DIR"
    chmod 700 "$NM_CONN_DIR"
    chown root:root "$NM_CONN_DIR" 2>/dev/null || true
    log "[2c/5] Wi-Fi profile directory recreated (its old block held the SSID as a filename)"
elif [[ -d "$NM_CONN_DIR" ]]; then
    # ABORT, not a note. If rmdir refused, something the stash didn't match is
    # still in there (a .bak, an editor swapfile) — and a leftover file means
    # the directory block that holds the deleted <SSID>.nmconnection FILENAME
    # stays allocated, so the SSID rides into the image exactly the way the
    # 2026-08-04 artifact proved it does. The leak check below only matches
    # *.nmconnection, so it cannot catch whatever blocked the rmdir here.
    # Filenames are NOT listed — the SSID may be in the name itself.
    _nm_left=$(find "$NM_CONN_DIR" -mindepth 1 2>/dev/null | wc -l)
    log "ERROR: ${NM_CONN_DIR} still holds ${_nm_left} entr(ies) after stashing —"
    log "       the deleted Wi-Fi profile's filename would persist in the image."
    log "       Inspect the directory by hand (names withheld here on purpose),"
    log "       move the stragglers off the SD, then run restore_after_cloning.sh"
    log "       and retry the clone."
    exit 1
fi

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

# ---------------------------------------------------------------------------
# Step 2d: the MOVIES-drive fstab entry must not block boot on a unit that
# has no movie drive — which is EVERY unit, the first time a customer
# powers it on.
# ---------------------------------------------------------------------------
# The source box has the drive attached, so a blocking entry is invisible
# here and fatal there. Observed on the first golden-image boot test
# (2026-08-04): systemd waited on /dev/disk/by-label/MOVIES, timed out after
# 90 s, failed mnt-ssd.mount plus magic-dingus-library-import.service and
# magic-dingus-storage-attach.service, and never reached the kiosk. To the
# operator it looked like the Pi rebooting in a loop.
#
# setup_services.sh writes the correct line, but its guard only checked
# whether ANY "LABEL=MOVIES" line existed, so a box provisioned before
# nofail/automount were added kept the blocking one forever and the image
# inherited it. That guard is fixed too; this is the golden-image net, so a
# stale entry on any future source box cannot reach a customer.
#
# Not stashed-and-restored: the corrected line is strictly better on the
# source box as well (it mounts on access instead of at boot, and a drive
# that is unplugged stops being a boot-time failure), so the fix stays.
# noauto and NO x-systemd.automount. The automount was the actual cause of the
# boot hang -- see udev/99-magic-movies-mount.rules. nofail was already present
# on the source box and did not help, because it governs the mount, not the
# automount. fsck pass 0: never fsck a drive that may not exist.
MOVIES_FSTAB_LINE="LABEL=MOVIES /mnt/ssd ext4 noauto,nofail 0 0"
if [[ -f /etc/fstab ]]; then
    if grep -qxF "$MOVIES_FSTAB_LINE" /etc/fstab; then
        log "[2c/5] MOVIES fstab entry already non-blocking"
    elif grep -q "LABEL=MOVIES" /etc/fstab; then
        cp -p /etc/fstab "${BACKUP_DIR}/fstab.before-clone"
        sed -i '/LABEL=MOVIES/d' /etc/fstab
        printf '%s\n' "$MOVIES_FSTAB_LINE" >> /etc/fstab
        systemctl daemon-reload 2>/dev/null || true
        log "[2c/5] REPLACED a blocking MOVIES fstab entry — it would have hung"
        log "         first boot on every unit with no movie drive attached"
    else
        printf '%s\n' "$MOVIES_FSTAB_LINE" >> /etc/fstab
        systemctl daemon-reload 2>/dev/null || true
        log "[2c/5] Added the MOVIES fstab mount entry (none was present)"
    fi

    # The fstab line is inert without this rule -- it is what actually mounts
    # the drive, at boot or on hotplug. Ship it in the image regardless of
    # whether the source box happens to have it.
    _udev_src="${CPP_DIR}/udev/99-magic-movies-mount.rules"
    if [[ -f "$_udev_src" ]]; then
        install -m 0644 "$_udev_src" /etc/udev/rules.d/99-magic-movies-mount.rules
        udevadm control --reload-rules 2>/dev/null || true
        log "[2c/5] MOVIES-drive udev mount rule installed into the image"
    else
        log "[2c/5] WARNING: ${_udev_src} missing — cloned units will NOT auto-mount a movie drive"
    fi
    unset _udev_src
fi

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
# Each entry is "directory|name-glob". The glob reaches find as a QUOTED
# argument, never as a bare word: the old form (`find $pat` unquoted) let the
# pattern words themselves be subject to pathname expansion against the CWD —
# harmless only while nullglob happened to be off at this point in the script.
# With nullglob on, an unmatched `*.nmconnection` word would simply vanish,
# find would error on the dangling -name, stderr is discarded, and the guard
# would report CLEAN over an image carrying the password. The one check whose
# job is to abort the ship must not depend on shell-option ambience.
leak_found=0
leak_checks=(
    '/var/lib/cloud|user-data*'
    '/var/lib/cloud|network-config*'
    '/var/lib/cloud|obj.pkl'
    '/etc/NetworkManager/system-connections|*.nmconnection'
    '/opt/magic_dingus_box/services|.env*'
    '/var/log|cloud-init.log*'
    '/var/log|cloud-init-output.log*'
    # Outcome assertions for stash classes that have silently matched nothing
    # in past iterations: the *arr databases (API keys + qBit password) and
    # the watch-history DB, both trees.
    '/opt/magic_dingus_box/services/config/radarr|radarr.db*'
    '/opt/magic_dingus_box/services/config/sonarr|sonarr.db*'
    '/opt/magic_dingus_box/services/config/prowlarr|prowlarr.db*'
    '/opt/magic_dingus_box/magic_dingus_box_cpp|media_browser.db*'
)
for chk in "${leak_checks[@]}"; do
    dir="${chk%%|*}"
    name="${chk#*|}"
    [[ -d "$dir" ]] || continue
    n=$(find "$dir" -name "$name" -type f 2>/dev/null | wc -l)
    if [[ "$n" -gt 0 ]]; then
        log "ERROR: post-scrub leak check FAILED — ${n} file(s) still match: ${dir} ${name}"
        leak_found=$((leak_found + 1))
    fi
done
unset chk dir name
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
    # Both captures are guarded: under set -euo pipefail an absent tune2fs or
    # a non-ext4 root makes the bare pipeline abort the script AT THE CAPTURE
    # — before the trap below is armed — leaving a stripped box with the
    # marker set. Empty values fall through to the existing "could not drop
    # the reserve" warning path instead.
    ROOT_DEV=$(findmnt -no SOURCE / 2>/dev/null || true)
    # Restore by exact BLOCK COUNT, not a rounded percentage: this card's
    # reserve is 631957 of 15458816 blocks = 4.088%, which rounds to 4 and
    # would quietly shrink the reserve on every clone.
    RESERVE_BLOCKS=$(tune2fs -l "$ROOT_DEV" 2>/dev/null \
        | awk -F: '/^Reserved block count/{gsub(/ /,"",$2); print $2}' || true)
    RESERVE_RESTORED=0
    # Persist the count where restore_after_cloning.sh can find it. The trap
    # below covers every signal bash can see — but SIGKILL, an OOM kill, or a
    # power cut bypass traps entirely, and the correct count then exists
    # nowhere. restore reads this file and repairs a zero reserve; the unwind
    # removes it once the reserve is back.
    if [[ -n "$RESERVE_BLOCKS" && "$RESERVE_BLOCKS" != "0" ]]; then
        printf '%s %s\n' "$RESERVE_BLOCKS" "$ROOT_DEV" > "${BACKUP_DIR}/reserve_blocks"
        chmod 600 "${BACKUP_DIR}/reserve_blocks"
    fi
    # Anything between the drop and the restore that exits the script would
    # otherwise leave the root filesystem permanently at zero reserve. That
    # is not hypothetical: an unbound-variable abort three lines below this
    # did exactly that on the source box, and nothing in the script would
    # ever have put it back. The trap makes the restore unconditional.
    ZERO_FILE=/var/tmp/mdb-zerofill.tmp
    ZERO_TAIL=/var/tmp/mdb-zerofill-tail.tmp
    BOOT_ZERO=/boot/firmware/.mdb-zerofill.tmp
    unwind_zerofill() {
        # Order matters: free the space FIRST, so that whatever comes next
        # (including a human logging in to see what happened) has room.
        rm -f "$ZERO_FILE" "$ZERO_TAIL" "$BOOT_ZERO" 2>/dev/null || true
        [[ "${RESERVE_RESTORED:-0}" == "1" ]] || return 0
        RESERVE_RESTORED=0
        if tune2fs -r "$RESERVE_BLOCKS" "$ROOT_DEV" >/dev/null 2>&1; then
            rm -f "${BACKUP_DIR}/reserve_blocks" 2>/dev/null || true
        else
            log "[4c/5] WARNING: could not restore the ${RESERVE_BLOCKS}-block root reserve on ${ROOT_DEV} — run: sudo tune2fs -r ${RESERVE_BLOCKS} ${ROOT_DEV}"
        fi
    }
    # HUP is the one that actually happened. This script runs under ssh, and
    # when the link drops the remote shell gets SIGHUP — which bash does NOT
    # convert into an EXIT-trap run unless HUP is trapped explicitly. A USB
    # gadget link dropped mid-fill and left the source box with a zero root
    # reserve and an 18 GB junk file, because the trap listed only EXIT INT
    # TERM. Losing the link is the NORMAL failure here, not an exotic one.
    trap unwind_zerofill EXIT INT TERM HUP

    if [[ -n "$RESERVE_BLOCKS" && "$RESERVE_BLOCKS" != "0" ]] && tune2fs -r 0 "$ROOT_DEV" >/dev/null 2>&1; then
        RESERVE_RESTORED=1
    elif [[ "$RESERVE_BLOCKS" == "0" ]]; then
        log "[4c/5] Root reserve is already 0 — nothing to drop, the whole"
        log "         filesystem is reachable by the fill."
    else
        log "[4c/5] WARNING: could not drop the ext4 root reserve on ${ROOT_DEV}."
        log "         ~$(( ${RESERVE_BLOCKS:-0} * 4 / 1024 )) MB of free space will NOT be"
        log "         zeroed and may carry deleted-file remnants into the image."
    fi

    avail_mb=$(df -Pm / | awk 'NR==2 {print $4}')
    log "[4c/5] Zeroing ~${avail_mb} MB of free space, including the ${RESERVE_BLOCKS:-0}-block root reserve (10-20 min on SD)..."

    # Margin. With the root reserve dropped there is no longer 2.4 GB of
    # hidden headroom underneath this number, so the old 64 MB left the box
    # at genuinely zero free: auditd logged "no space left on logging
    # partition" and systemd units failed to start during the window.
    # 256 MB costs ~1.3% of coverage and keeps daemons alive; the artifact
    # scan in clone_live_sd.sh is the authority on whether anything actually
    # survived, so trading a sliver of coverage for stability is safe.
    zero_mb=$(( avail_mb > 256 ? avail_mb - 256 : 0 ))
    if [[ "$zero_mb" -gt 0 ]]; then
        dd if=/dev/zero of="$ZERO_FILE" bs=4M count=$(( zero_mb / 4 )) \
           status=none 2>/dev/null || true
    fi
    sync

    # STAGE 2 — the tail. A bounded fill leaves its margin's worth of free
    # extents NEVER WRITTEN, and the allocator decides which ones those are.
    # Step 2c deletes ~30 MB of secret-bearing files immediately before this
    # (the *arr databases and their WALs, prowlarr's logs, the container
    # metadata), and any of those freed blocks that land in the unwritten
    # remainder ride into the image intact.
    #
    # Not hypothetical: with a 256 MB margin the 2026-08-04 image still
    # carried PROWLARR_API_KEY twice and WIREGUARD_PRIVATE_KEY once, while
    # every live file containing them had been correctly stashed. The
    # remnants were in the margin.
    #
    # So finish the job with a second, unbounded fill. It runs to ENOSPC and
    # therefore reaches every remaining free extent. The genuinely-zero-free
    # window lasts only as long as writing <=256 MB — seconds — instead of
    # spanning the whole multi-GB fill, which is what made a zero margin
    # unsafe in the first place. Both files are removed by the trap on any
    # exit path, and stage 2 is freed first so the box is never left tight.
    dd if=/dev/zero of="$ZERO_TAIL" bs=1M status=none 2>/dev/null || true
    sync
    rm -f "$ZERO_TAIL"
    sync
    rm -f "$ZERO_FILE"
    sync

    # The FAT boot partition has its own free space and was never zeroed at
    # all. Step 2b overwrites the NAMED cloud-init files there, but FAT
    # rewrites a file to fresh clusters rather than in place, so earlier
    # copies persist as unlinked remnants — three fragments of network-config
    # naming the operator's SSID were found in the shipped image at ~20 MB.
    #
    # This runs with the trap still ARMED. It used to sit after the disarm, so
    # a dropped ssh link during this fill — the normal failure mode the trap
    # exists for — left /boot/firmware 100% full of junk that neither prepare
    # nor restore would ever remove (unwind_zerofill already knows about
    # $BOOT_ZERO; it just wasn't listening any more).
    boot_avail=$(df -Pm /boot/firmware 2>/dev/null | awk 'NR==2 {print $4}')
    if [[ -n "$boot_avail" && "$boot_avail" -gt 8 ]]; then
        log "[4c/5] Zeroing ~$((boot_avail - 4)) MB of boot-partition free space..."
        # Unbounded: the FAT boot partition has no daemon writing to it during
        # a clone, so there is no reason to leave a margin here at all.
        dd if=/dev/zero of="$BOOT_ZERO" bs=1M status=none 2>/dev/null || true
        sync
        rm -f "$BOOT_ZERO"
        sync
    fi

    unwind_zerofill
    trap - EXIT INT TERM HUP
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
