#!/usr/bin/env bash
set -euo pipefail

# migrate_box.sh — white-glove migration of a fielded Magic Dingus Box
# onto a fresh golden image, preserving everything the owner cares about.
#
# Why this exists: fielded Pi 4B units run Raspberry Pi OS Lite Bookworm,
# and v1.6.4 is the last release that OS can build or run (the libgpiod
# 2.x migration made Trixie the floor). OTA cannot cross a major OS
# boundary, so those boxes are reflashed once with the current dual-board
# golden image and then rejoin the normal OTA train forever.
#
# Usage (run from the dev machine, box reachable over SSH):
#
#   ./migrate_box.sh backup  magic@<box-host> <backup-dir>
#       Pull every preserved-content path off the box into <backup-dir>.
#       Run BEFORE reflashing the SD card. Safe to re-run; briefly stops
#       the Radarr/Prowlarr/qBittorrent containers (~30s) so their
#       SQLite databases copy in a consistent state.
#
#   ./migrate_box.sh restore magic@<box-host> <backup-dir>
#       Push the backup onto the freshly flashed box. Run AFTER the new
#       image has booted once (first_boot.sh must have completed — it
#       generates identity, prunes Pi 5-only content on a Pi 4, then
#       disables itself). Restores content, WiFi, hostname, Media
#       Browser state, and paired phones, then restarts services.
#
# What is preserved (mirrors the OTA contract in OTA_UPDATE_GUARANTEES.md):
#   - uploaded videos, ROMs, saves, save states, per-game thumbnails
#   - playlists (see the same-system dedupe note in restore)
#   - config/ (settings.json, controller_profiles.json)
#   - device identity (device_info.json + hostname)
#   - Media Browser state (services/.env, services/config/*)
#   - Phone Remote pairings (paired_remotes.json, flask_secret.key,
#     pairing_audit.log)
#   - WiFi profiles (/etc/NetworkManager/system-connections)

INSTALL_DIR="/opt/magic_dingus_box"
DATA_REL="magic_dingus_box_cpp/data"

usage() { sed -n '3,32p' "$0"; exit 1; }

[[ $# -eq 3 ]] || usage
MODE="$1"; BOX="$2"; BACKUP_DIR="$3"

log() { echo "[migrate] $1"; }

ssh_box() { ssh -o ConnectTimeout=10 "$BOX" "$@"; }

case "$MODE" in
# ---------------------------------------------------------------------------
backup)
    mkdir -p "$BACKUP_DIR"
    log "Backing up $BOX -> $BACKUP_DIR"

    # Owner content + per-box state, all owned by the magic user.
    # --relative keeps the tree shape so restore is a single rsync back.
    rsync -az --relative --info=progress2 \
        "$BOX:$INSTALL_DIR/./$DATA_REL/media" \
        "$BOX:$INSTALL_DIR/./$DATA_REL/roms" \
        "$BOX:$INSTALL_DIR/./$DATA_REL/saves" \
        "$BOX:$INSTALL_DIR/./$DATA_REL/states" \
        "$BOX:$INSTALL_DIR/./$DATA_REL/playlists" \
        "$BOX:$INSTALL_DIR/./$DATA_REL/thumbnails" \
        "$BOX:$INSTALL_DIR/./$DATA_REL/device_info.json" \
        "$BOX:$INSTALL_DIR/./config" \
        "$BACKUP_DIR/content/"

    # Optional per-box files — absent on boxes that never used the
    # feature (old releases predate the Phone Remote entirely).
    for f in paired_remotes.json flask_secret.key pairing_audit.log; do
        rsync -az "$BOX:$INSTALL_DIR/$DATA_REL/$f" \
            "$BACKUP_DIR/content/$DATA_REL/$f" 2>/dev/null \
            || log "  (no $f on this box — fine)"
    done

    # Media Browser per-box state. Root-owned bits inside services/config
    # need sudo on the box side.
    if ssh_box "test -f $INSTALL_DIR/services/.env" 2>/dev/null; then
        # Quiesce the database writers before copying services/config.
        # Radarr, Prowlarr and qBittorrent write their SQLite databases
        # (WAL mode) continuously; rsync reads a .db and its -wal/-shm
        # sidecars at different instants, and a checkpoint landing between
        # those reads yields a copy whose parts are mutually inconsistent.
        # SQLite reports "database disk image is malformed" only at
        # RESTORE time — onto the freshly flashed box, after the owner's
        # original SD has been wiped. prepare_for_cloning.sh stops the
        # same stack before touching the same files for the same reason.
        # Gluetun and byparr keep running: nothing in services/config of
        # theirs mutates.
        STACK_QUIESCED=0
        resume_stack() {
            if [[ "$STACK_QUIESCED" -eq 1 ]]; then
                STACK_QUIESCED=0
                log "  Restarting media stack containers..."
                ssh_box "cd $INSTALL_DIR/services && sudo docker compose start radarr prowlarr qbittorrent" \
                    || log "  WARNING: could not restart the media stack — run 'sudo docker compose start radarr prowlarr qbittorrent' in $INSTALL_DIR/services on the box"
            fi
        }
        log "  Pausing media stack containers for a consistent DB copy..."
        if ssh_box "cd $INSTALL_DIR/services && sudo docker compose stop radarr prowlarr qbittorrent" 2>/dev/null; then
            STACK_QUIESCED=1
            # set -e aborts this script on a failed rsync below; the trap
            # guarantees the owner's box gets its media stack back even
            # then. Cleared again right after the normal-path resume.
            trap resume_stack EXIT
        else
            log "  (could not stop containers — stack not running? copying as-is)"
        fi

        # rsync creates only the LAST path component of its destination —
        # without this mkdir the .env copy dies on the missing services/
        # parent (found the first time this block ran against real
        # hardware; the script had never been exercised before).
        mkdir -p "$BACKUP_DIR/services"
        rsync -az --rsync-path="sudo rsync" \
            "$BOX:$INSTALL_DIR/services/.env" \
            "$BACKUP_DIR/services/.env"
        rsync -az --rsync-path="sudo rsync" \
            "$BOX:$INSTALL_DIR/services/config/" \
            "$BACKUP_DIR/services/config/"
        log "  Media Browser state captured"

        resume_stack
        trap - EXIT
    else
        log "  (no services/.env — Media Browser never provisioned)"
    fi

    # WiFi profiles are root-only. The owner's box must come back up on
    # their network without anyone re-typing a password.
    rsync -az --rsync-path="sudo rsync" \
        "$BOX:/etc/NetworkManager/system-connections/" \
        "$BACKUP_DIR/wifi/" 2>/dev/null \
        || log "  (no WiFi profiles readable — box may be on Ethernet)"

    # Manifest for the restore phase + a human reading the folder later.
    {
        echo "source_host: $BOX"
        echo "backed_up_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "box_version: $(ssh_box "cat $INSTALL_DIR/VERSION 2>/dev/null" || echo unknown)"
        echo "box_os: $(ssh_box 'cat /etc/debian_version 2>/dev/null' || echo unknown)"
        echo "box_hostname: $(ssh_box hostname)"
    } > "$BACKUP_DIR/manifest.txt"

    log "Backup complete:"
    cat "$BACKUP_DIR/manifest.txt" | sed 's/^/    /'
    du -sh "$BACKUP_DIR" | awk '{print "[migrate] total size: " $1}'
    log "Next: reflash the SD with the golden image, boot it once, then:"
    log "  $0 restore $BOX $BACKUP_DIR"
    ;;

# ---------------------------------------------------------------------------
restore)
    [[ -d "$BACKUP_DIR/content" ]] || { echo "ERROR: $BACKUP_DIR/content missing — is this a backup dir?"; exit 1; }
    log "Restoring $BACKUP_DIR -> $BOX"

    # Preflight: the target must be a freshly provisioned golden image —
    # first_boot.sh disables itself as its last step. Restoring onto a
    # box where it is still enabled means it hasn't run (its wipes would
    # destroy what we're about to restore on the next boot).
    if ssh_box "systemctl is-enabled magic-first-boot.service 2>/dev/null" | grep -q "^enabled"; then
        echo "ERROR: magic-first-boot.service is still enabled on $BOX."
        echo "Boot the new image once and let first-boot finish, then re-run."
        exit 1
    fi

    log "Stopping services on the box..."
    ssh_box "sudo systemctl stop magic-dingus-box-cpp.service magic-dingus-web.service 2>/dev/null; sudo systemctl stop magic-dingus-services.service 2>/dev/null; true"

    # Owner content back. NO --delete: this is a merge — the golden
    # image's curated content stays, the owner's content wins on
    # filename collisions.
    log "Restoring owner content..."
    rsync -az --relative --info=progress2 \
        "$BACKUP_DIR/content/./$DATA_REL/media" \
        "$BACKUP_DIR/content/./$DATA_REL/roms" \
        "$BACKUP_DIR/content/./$DATA_REL/saves" \
        "$BACKUP_DIR/content/./$DATA_REL/states" \
        "$BACKUP_DIR/content/./$DATA_REL/thumbnails" \
        "$BACKUP_DIR/content/./$DATA_REL/device_info.json" \
        "$BACKUP_DIR/content/./config" \
        "$BOX:$INSTALL_DIR/"

    for f in paired_remotes.json flask_secret.key pairing_audit.log; do
        [[ -f "$BACKUP_DIR/content/$DATA_REL/$f" ]] && \
            rsync -az "$BACKUP_DIR/content/$DATA_REL/$f" "$BOX:$INSTALL_DIR/$DATA_REL/$f"
    done

    # Playlists need the same same-system dedupe the OTA sync uses: the
    # old box's game playlists predate the games_* naming, and the fresh
    # image already carries a playlist per system. Restoring arcade.yaml
    # next to games_arcade.yaml would put two Arcade rows on the menu.
    # Policy: video/custom playlists restore unconditionally; a game
    # playlist restores ONLY if no playlist on the box already covers the
    # same emulator_system. An owner's EDIT to a default game playlist is
    # the one thing this loses — logged loudly when it happens.
    log "Restoring playlists (same-system dedupe)..."
    rsync -az "$BACKUP_DIR/content/$DATA_REL/playlists/" "$BOX:/tmp/migrate_playlists/"
    ssh_box "bash -s" <<'REMOTE'
set -u
BOX_PL="/opt/magic_dingus_box/magic_dingus_box_cpp/data/playlists"
norm_systems() {  # all normalized emulator_system tokens in a file
    grep -hiE '^[[:space:]]*emulator_system:' "$1" 2>/dev/null \
        | awk -F: '{gsub(/[ "\047]/,"",$2); print tolower($2)}' | sort -u
}
existing=$(cat $BOX_PL/*.yaml 2>/dev/null | grep -iE '^[[:space:]]*emulator_system:' \
        | awk -F: '{gsub(/[ "\047]/,"",$2); print tolower($2)}' | sort -u)
for f in /tmp/migrate_playlists/*.yaml; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    if [ -e "$BOX_PL/$base" ]; then
        echo "    kept image copy (same name exists): $base"
        continue
    fi
    sys=$(norm_systems "$f" | head -1)
    if [ -n "$sys" ] && echo "$existing" | grep -qx "$sys"; then
        echo "    SKIPPED $base — box already has a playlist for system '$sys'"
        echo "    (if the owner had edits in it, port them by hand)"
        continue
    fi
    cp "$f" "$BOX_PL/$base"
    echo "    restored: $base"
done
rm -rf /tmp/migrate_playlists
REMOTE

    # Media Browser state.
    if [[ -f "$BACKUP_DIR/services/.env" ]]; then
        log "Restoring Media Browser state..."
        rsync -az "$BACKUP_DIR/services/.env" "$BOX:$INSTALL_DIR/services/.env"
        rsync -az --rsync-path="sudo rsync" \
            "$BACKUP_DIR/services/config/" "$BOX:$INSTALL_DIR/services/config/"
    fi

    # WiFi profiles — root-owned, mode 600, then ask NM to reload.
    if [[ -d "$BACKUP_DIR/wifi" ]] && compgen -G "$BACKUP_DIR/wifi/*" > /dev/null; then
        log "Restoring WiFi profiles..."
        rsync -az --rsync-path="sudo rsync" \
            "$BACKUP_DIR/wifi/" "$BOX:/etc/NetworkManager/system-connections/"
        ssh_box "sudo chown root:root /etc/NetworkManager/system-connections/* && sudo chmod 600 /etc/NetworkManager/system-connections/* && sudo nmcli connection reload"
    fi

    # Hostname: the fresh image generated a new magicpi-XXXX identity in
    # first boot; put the owner's back so bookmarks, the Content Manager,
    # and mDNS all keep working. device_info.json was restored above;
    # the OS-level hostname has to match it.
    OLD_HOSTNAME=$(python3 -c "import json;print(json.load(open('$BACKUP_DIR/content/$DATA_REL/device_info.json')).get('hostname',''))" 2>/dev/null || true)
    if [[ -z "$OLD_HOSTNAME" ]]; then
        OLD_HOSTNAME=$(grep "^box_hostname:" "$BACKUP_DIR/manifest.txt" | cut -d' ' -f2 || true)
    fi
    if [[ -n "$OLD_HOSTNAME" ]]; then
        log "Restoring hostname: $OLD_HOSTNAME"
        ssh_box "sudo hostnamectl set-hostname '$OLD_HOSTNAME' && sudo sed -i 's/127\.0\.1\.1.*/127.0.1.1\t$OLD_HOSTNAME/' /etc/hosts"
    fi

    # Ownership: rsync over ssh-as-magic wrote magic-owned files already;
    # belt and braces for anything that landed root-owned via sudo rsync.
    ssh_box "sudo chown -R magic:magic $INSTALL_DIR/$DATA_REL $INSTALL_DIR/config; sudo chown magic:magic $INSTALL_DIR/services/.env 2>/dev/null; true"

    log "Restarting services..."
    ssh_box "sudo systemctl daemon-reload && sudo systemctl start magic-dingus-services.service 2>/dev/null; sudo systemctl start magic-dingus-web.service 2>/dev/null; sudo systemctl start magic-dingus-box-cpp.service"

    log "Running acceptance check (verify_box.sh)..."
    ssh_box "$INSTALL_DIR/magic_dingus_box_cpp/scripts/verify_box.sh" \
        || log "verify_box.sh reported issues — review before handing the box back"

    log "Restore complete. The box may take ~90s for the Media Browser stack to settle."
    ;;

*)
    usage
    ;;
esac
