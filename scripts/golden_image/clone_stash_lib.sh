#!/usr/bin/env bash
#
# Magic Dingus Box - reboot-safe secret stash for the live SD clone
#
# Sourced by prepare_for_cloning.sh and restore_after_cloning.sh. Sourcing
# defines paths and functions and RUNS NOTHING, so tests/local can exercise
# the logic off-Pi against a scratch directory.
#
# Why this exists. prepare_for_cloning.sh removes every secret-bearing file
# from the SD card before the dd (the .env with the VPN key, the *arr
# databases, every config.xml, the apps' own Backups/ zips, flask_secret.key,
# the TMDB key, the Wi-Fi profile, ...). The originals are zeroed in place and
# unlinked, so the stash is the ONLY copy until restore_after_cloning.sh puts
# them back.
#
# That stash used to live in /dev/shm. A power cut, a kernel panic or a
# reboot anywhere in the 30-90 minute dd erased RAM, restore then found no
# manifest and reported "nothing to restore" — and the source box had
# permanently lost its Radarr/Sonarr libraries, VPN key, API keys, phone
# pairings and the apps' own backups. Nothing in the output said so.
#
# So the stash now lives on the MOVIE DRIVE (/mnt/ssd): persistent across a
# reboot, and not part of the SD card the dd captures. It must NEVER live on
# the SD card itself — that would put every secret straight back into the
# image the scrub exists to protect — which is what the drive checks below
# guard against. RAM is available only behind an explicit opt-in
# (prepare_for_cloning.sh --allow-ram-stash) for a box with no drive.
#

# The movie drive. prepare already requires it for the curated-content stash
# (.mdb-content-stash); the secret stash reuses the same location and guards.
MDB_STASH_DRIVE="/mnt/ssd"
MDB_SECRET_STASH_DIR="${MDB_STASH_DRIVE}/.mdb-secret-stash"
# shellcheck disable=SC2034  # used by the sourcing scripts
MDB_CONTENT_STASH_DIR="${MDB_STASH_DRIVE}/.mdb-content-stash"

# RAM: the pre-fix location, still read by restore so a stash written by an
# older prepare is never stranded, and the opt-in fallback for a drive-less box.
MDB_RAM_SECRET_STASH_DIR="/dev/shm/mdb-secret-stash"
# shellcheck disable=SC2034  # used by restore_after_cloning.sh
MDB_LEGACY_BOOT_STASH_DIR="/dev/shm/mdb-boot-stash"

# In cloning_backup/ (on the SD): records WHERE the stash is. A path only,
# never a secret, so it is harmless that the dd captures it (and
# first_boot.sh wipes cloning_backup/ on every clone anyway).
MDB_STASH_POINTER_NAME="secret_stash_path"
# Inside the stash dir: written once every file is back and verified, before
# the stash is destroyed. A re-run that finds it only finishes the cleanup —
# it must never copy stale stash contents over files the box has since
# written to.
MDB_STASH_RESTORED_NAME="restored"

mdb_log() {
    if declare -F log >/dev/null 2>&1; then log "$1"; else echo "$1"; fi
}

# Parent block device of the filesystem holding PATH ("mmcblk0", "sda").
# Prints nothing when it cannot tell — callers treat that as a refusal.
mdb_disk_of() {
    local src pk
    src="$(findmnt -no SOURCE --target "$1" 2>/dev/null | head -1)" || true
    [[ -n "$src" ]] || return 0
    pk="$(lsblk -no PKNAME "$src" 2>/dev/null | head -1)" || true
    if [[ -n "$pk" ]]; then
        printf '%s\n' "$pk"
    else
        basename "$src"
    fi
}

# Pure decision: may a stash live on the drive?
#   $1 mounted (1/0)  $2 the drive's disk  $3 the root filesystem's disk
# Prints ok | not-mounted | disk-unknown | same-disk-as-root.
# "Not mounted" matters because /mnt/ssd is then a plain directory ON THE SD
# CARD and the dd captures whatever is written there. "Same disk" catches a
# drive partition that shares the root's disk, for the same reason.
mdb_stash_drive_verdict() {
    local mounted="$1" sdisk="$2" rdisk="$3"
    if [[ "$mounted" != "1" ]]; then echo "not-mounted"; return 0; fi
    if [[ -z "$sdisk" || -z "$rdisk" ]]; then echo "disk-unknown"; return 0; fi
    if [[ "$sdisk" == "$rdisk" ]]; then echo "same-disk-as-root"; return 0; fi
    echo "ok"
}

# The verdict for the real movie drive on this box.
mdb_check_stash_drive() {
    local mounted=0 sdisk=""
    if mountpoint -q "$MDB_STASH_DRIVE" 2>/dev/null; then
        mounted=1
        sdisk="$(mdb_disk_of "$MDB_STASH_DRIVE")"
    fi
    mdb_stash_drive_verdict "$mounted" "$sdisk" "$(mdb_disk_of /)"
}

# Flattened stash filename for a source path (the manifest's key column).
mdb_stash_key() {
    printf '%s' "$1" | tr '/' '_'
}

# Create the stash directory: owned by the caller (root in production), mode
# 0700, with an empty manifest, flushed to disk. Fails when a manifest is
# already there — that stash may be the ONLY copy of a previous clone's
# secrets, and only restore_after_cloning.sh may consume it.
mdb_init_secret_stash() {
    local dir="$1" perms owner
    if [[ -e "${dir}/manifest" ]]; then
        mdb_log "ERROR: a secret stash already exists at ${dir}"
        mdb_log "       It may hold the only copy of a previous clone's secrets."
        mdb_log "       Run restore_after_cloning.sh first, then retry."
        return 1
    fi
    # Leftovers with no manifest cannot be mapped back to any path (the
    # manifest is created before the first copy), so they are debris — and a
    # stale `restored` flag carried into a NEW stash would make restore skip
    # it, so the directory goes entirely.
    if [[ -d "$dir" ]]; then
        mdb_destroy_stash_dir "$dir"
        rm -rf "$dir"
    fi
    mkdir -p "$dir" || return 1
    chmod 700 "$dir" || return 1
    # A drive that ignores Unix permissions (exFAT, NTFS) would leave every
    # secret world-readable. Prove the mode and owner stuck.
    perms="$(stat -c %a "$dir" 2>/dev/null || stat -f %Lp "$dir" 2>/dev/null || echo '')"
    owner="$(stat -c %u "$dir" 2>/dev/null || stat -f %u "$dir" 2>/dev/null || echo '')"
    if [[ "$perms" != "700" || "$owner" != "$EUID" ]]; then
        mdb_log "ERROR: ${dir} did not keep mode 0700 / owner ${EUID} (got ${perms:-?} / ${owner:-?})"
        mdb_log "       — the drive's filesystem does not honour Unix permissions."
        rm -rf "$dir" 2>/dev/null || true
        return 1
    fi
    ( umask 077 && : > "${dir}/manifest" ) || return 1
    sync
    return 0
}

# Copy ONE file into the stash and record it in the manifest. Never touches
# the original. A path already in the manifest is skipped (the SECRET_PATHS
# globs overlap). Columns: key, original path, mode, uid, gid.
mdb_stash_copy() {
    local src="$1" dir="$2" key mode uid gid
    key="$(mdb_stash_key "$src")"
    if cut -f1 "${dir}/manifest" 2>/dev/null | grep -qxF -- "$key"; then
        return 0
    fi
    cp -p "$src" "${dir}/${key}" 2>/dev/null || cp "$src" "${dir}/${key}" || return 1
    chmod 600 "${dir}/${key}" 2>/dev/null || true
    mode="$(stat -c %a "$src" 2>/dev/null || echo '')"
    uid="$(stat -c %u "$src" 2>/dev/null || echo '')"
    gid="$(stat -c %g "$src" 2>/dev/null || echo '')"
    printf '%s\t%s\t%s\t%s\t%s\n' "$key" "$src" "$mode" "$uid" "$gid" >> "${dir}/manifest"
}

# Does the stash copy of SRC match SRC byte for byte?
mdb_stash_matches() {
    local src="$1" dir="$2"
    cmp -s "$src" "${dir}/$(mdb_stash_key "$src")"
}

# Overwrite a file in place with zeros, then unlink it. `rm` alone only
# detaches the name; the bytes stay in free space. Rounded up to whole 64K
# blocks (overshooting is harmless — the file is unlinked right after).
mdb_shred_file() {
    local f="$1" sz blocks
    [[ -f "$f" ]] || return 0
    sz="$(stat -c %s "$f" 2>/dev/null || stat -f %z "$f" 2>/dev/null || echo 0)"
    if [[ "$sz" -gt 0 ]]; then
        blocks=$(( (sz + 65535) / 65536 ))
        dd if=/dev/zero of="$f" bs=65536 count="$blocks" conv=notrunc 2>/dev/null || true
        sync
    fi
    rm -f "$f"
}

# Shred every file in a stash directory, then remove it. The `restored` flag
# goes last of all, so an interrupted destroy is still recognisable as an
# already-restored stash.
mdb_destroy_stash_dir() {
    local dir="$1" f
    [[ -d "$dir" ]] || return 0
    while IFS= read -r -d '' f; do
        [[ "$(basename "$f")" == "$MDB_STASH_RESTORED_NAME" ]] && continue
        mdb_shred_file "$f"
    done < <(find "$dir" -type f -print0 2>/dev/null)
    sync
}

# Copy every manifest entry back to its original path, with its recorded
# owner and mode, and verify each one. Sets MDB_RESTORED / MDB_RESTORE_FAILED.
mdb_copy_back_stash() {
    local dir="$1" key dest mode uid gid s
    MDB_RESTORED=0
    MDB_RESTORE_FAILED=0
    while IFS=$'\t' read -r key dest mode uid gid; do
        [[ -n "$key" && -n "$dest" ]] || continue
        s="${dir}/${key}"
        if [[ ! -f "$s" ]]; then
            mdb_log "  MISSING from stash: ${dest}"
            MDB_RESTORE_FAILED=$((MDB_RESTORE_FAILED + 1))
            continue
        fi
        mkdir -p "$(dirname "$dest")" 2>/dev/null || true
        # cp -p can refuse to preserve ownership on FAT (/boot/firmware);
        # the content is what matters there, so fall back to a plain copy.
        cp -p "$s" "$dest" 2>/dev/null || cp "$s" "$dest" 2>/dev/null || true
        if [[ -n "$mode" && -n "$uid" && -n "$gid" ]]; then
            # Exact inverse of prepare: the container-owned files are
            # 1000:1000 / 644, and blanket-chowning them changed service state.
            chown "${uid}:${gid}" "$dest" 2>/dev/null || true
            chmod "$mode" "$dest" 2>/dev/null || true
        else
            # Stash written by an older prepare that recorded only key + dest.
            case "$dest" in
                /home/magic/*|/opt/magic_dingus_box/*) chown magic:magic "$dest" 2>/dev/null || true ;;
            esac
            chmod 600 "$dest" 2>/dev/null || true
        fi
        if cmp -s "$s" "$dest"; then
            MDB_RESTORED=$((MDB_RESTORED + 1))
        else
            mdb_log "  FAILED to restore: ${dest}"
            MDB_RESTORE_FAILED=$((MDB_RESTORE_FAILED + 1))
        fi
    done < "${dir}/manifest"
    sync
}

# Pure decision for restore.
#   $1 pointer (the recorded stash path, or "")   $2 stash reachable (1/0)
#   $3 manifest present (1/0)                     $4 restored flag present (1/0)
# Prints:
#   none        no stash was ever started — nothing to restore
#   unreachable the stash is on a drive that is not mounted right now
#   cleanup     already restored; only the stash's destruction is unfinished
#   restore     copy everything back
#   lost        a stash was started and is GONE — the secrets are unrecoverable
mdb_secret_restore_plan() {
    local pointer="$1" reachable="$2" manifest="$3" restored="$4"
    if [[ -n "$pointer" && "$reachable" != "1" ]]; then echo "unreachable"; return 0; fi
    if [[ "$restored" == "1" ]]; then echo "cleanup"; return 0; fi
    if [[ "$manifest" == "1" ]]; then echo "restore"; return 0; fi
    if [[ -z "$pointer" ]]; then echo "none"; return 0; fi
    echo "lost"
}

# Where is the stash this box is waiting on? The pointer when prepare wrote
# one; otherwise any stash found at the known locations (first_boot.sh on a
# box with no SD CID wipes cloning_backup/, pointer included, while the stash
# on the drive survives; an older prepare used RAM and wrote no pointer).
mdb_find_stash_dir() {
    local backup_dir="$1" pointer="" d
    if [[ -s "${backup_dir}/${MDB_STASH_POINTER_NAME}" ]]; then
        read -r pointer < "${backup_dir}/${MDB_STASH_POINTER_NAME}" || true
    fi
    if [[ -n "$pointer" ]]; then printf '%s\n' "$pointer"; return 0; fi
    for d in "$MDB_SECRET_STASH_DIR" "$MDB_RAM_SECRET_STASH_DIR"; do
        if [[ -e "${d}/manifest" || -e "${d}/${MDB_STASH_RESTORED_NAME}" ]]; then
            printf '%s\n' "$d"; return 0
        fi
    done
    return 0
}

# Is there a secret stash restore still has to deal with?
mdb_secret_stash_pending() {
    local backup_dir="$1"
    [[ -s "${backup_dir}/${MDB_STASH_POINTER_NAME}" ]] && return 0
    [[ -n "$(mdb_find_stash_dir "$backup_dir")" ]]
}

# The whole secret half of restore_after_cloning.sh.
#   $1 cloning_backup dir   $2 accept loss (1/0, from --accept-secret-loss)
# Returns 0 restored / nothing to do / loss accepted, 1 secrets LOST,
# 2 stash unreachable (drive not mounted), 3 a file failed to restore.
# On any non-zero return NOTHING is destroyed, so a re-run can finish the job.
mdb_restore_secrets() {
    local backup_dir="$1" accept_loss="${2:-0}"
    local pointer_file="${backup_dir}/${MDB_STASH_POINTER_NAME}"
    local pointer="" dir reachable=1 manifest=0 restored=0 plan

    if [[ -s "$pointer_file" ]]; then
        read -r pointer < "$pointer_file" || true
    fi
    dir="$(mdb_find_stash_dir "$backup_dir")"

    # A stash on the movie drive needs the drive mounted. After a reboot the
    # udev rule normally mounts it; try the fstab entry once if not.
    if [[ -n "$dir" && "$dir" == "${MDB_STASH_DRIVE}/"* ]] \
            && ! mountpoint -q "$MDB_STASH_DRIVE" 2>/dev/null; then
        if [[ "$EUID" -eq 0 ]]; then
            mount "$MDB_STASH_DRIVE" >/dev/null 2>&1 || true
        fi
        mountpoint -q "$MDB_STASH_DRIVE" 2>/dev/null || reachable=0
    fi
    if [[ -n "$dir" && "$reachable" == "1" ]]; then
        [[ -f "${dir}/manifest" ]] && manifest=1
        [[ -f "${dir}/${MDB_STASH_RESTORED_NAME}" ]] && restored=1
    fi

    plan="$(mdb_secret_restore_plan "$pointer" "$reachable" "$manifest" "$restored")"
    case "$plan" in
        none)
            mdb_log "[1/4] No application-secret stash was started (nothing to restore)"
            return 0
            ;;
        unreachable)
            mdb_log "ERROR: ============================================================"
            mdb_log "ERROR: the secret stash is on the movie drive, and ${MDB_STASH_DRIVE}"
            mdb_log "ERROR: is NOT mounted. Nothing has been restored or deleted."
            mdb_log "ERROR: Attach the movie drive (check: mountpoint ${MDB_STASH_DRIVE}),"
            mdb_log "ERROR: then re-run:"
            mdb_log "ERROR:   sudo /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh"
            mdb_log "ERROR: ============================================================"
            return 2
            ;;
        lost)
            mdb_log "ERROR: ============================================================"
            mdb_log "ERROR: SECRET STASH IS MISSING: ${pointer}"
            mdb_log "ERROR: prepare_for_cloning.sh removed this box's secrets (services/.env,"
            mdb_log "ERROR: the Radarr/Sonarr/Prowlarr databases and config.xml files, their"
            mdb_log "ERROR: Backups/, flask_secret.key, the TMDB key, the Wi-Fi profile) into"
            mdb_log "ERROR: that stash, and it no longer exists — most likely it was the RAM"
            mdb_log "ERROR: fallback (--allow-ram-stash) and the box rebooted mid-clone."
            mdb_log "ERROR: Those files CANNOT be restored from here."
            mdb_log "ERROR: If the movie drive holding the stash is simply not attached,"
            mdb_log "ERROR: attach it and re-run. To accept the loss and bring the box"
            mdb_log "ERROR: back up without them (Media Browser will need re-provisioning):"
            mdb_log "ERROR:   sudo .../restore_after_cloning.sh --accept-secret-loss"
            mdb_log "ERROR: ============================================================"
            if [[ "$accept_loss" == "1" ]]; then
                mdb_log "[1/4] --accept-secret-loss given: continuing WITHOUT the stashed secrets"
                rm -f "$pointer_file"
                sync
                return 0
            fi
            return 1
            ;;
        restore)
            if [[ -z "$pointer" ]]; then
                mdb_log "[1/4] WARNING: found an unrestored secret stash at ${dir} with no"
                mdb_log "         record of it in ${backup_dir} — restoring it now"
            fi
            mdb_log "[1/4] Restoring application secrets from ${dir}..."
            mdb_copy_back_stash "$dir"
            if [[ "$MDB_RESTORE_FAILED" -gt 0 ]]; then
                mdb_log "ERROR: ${MDB_RESTORE_FAILED} secret file(s) failed to restore (${MDB_RESTORED} ok)."
                mdb_log "ERROR: The stash at ${dir} has been KEPT. Fix the cause and re-run"
                mdb_log "ERROR: restore_after_cloning.sh."
                return 3
            fi
            # Record success INSIDE the stash before destroying anything, so
            # an interrupted destroy is never mistaken for an unrestored stash.
            : > "${dir}/${MDB_STASH_RESTORED_NAME}"
            sync
            mdb_log "[1/4] Restored ${MDB_RESTORED} application secret(s), all verified"
            ;;
        cleanup)
            mdb_log "[1/4] Secrets were already restored by an earlier run — finishing stash cleanup"
            ;;
    esac

    # Destroy: shred the payload, drop the pointer, THEN remove the directory
    # (with its `restored` flag). Every interruption point leaves a state
    # that the plan above maps to cleanup or none — never to lost.
    mdb_destroy_stash_dir "$dir"
    rm -f "$pointer_file"
    sync
    rm -rf "$dir"
    sync
    mdb_log "[1/4] Secret stash at ${dir} securely removed"
    return 0
}
