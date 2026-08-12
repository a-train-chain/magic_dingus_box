#!/usr/bin/env bash
set -euo pipefail

#
# Magic Dingus Box - First Boot Setup
#
# Runs ONCE on each newly cloned Pi at first boot. Handles:
#   - SSH host key regeneration
#   - Root filesystem expansion to fill SD card
#   - Unique device identity generation
#   - Required directory creation
#   - Ownership/permission fixup
#   - Self-disabling so it never runs again
#
# This script is enabled by prepare_golden_image.sh and triggered by
# magic-first-boot.service (oneshot, before the kiosk app starts).
#
# Must run as root (systemd runs it as root by default).
#

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
INSTALL_DIR="/opt/magic_dingus_box"
CPP_DIR="${INSTALL_DIR}/magic_dingus_box_cpp"
DATA_DIR="${CPP_DIR}/data"
CONFIG_DIR="${INSTALL_DIR}/config"
MAGIC_USER="magic"
MAGIC_HOME="/home/${MAGIC_USER}"

# ---------------------------------------------------------------------------
# Logging helper - stdout + syslog + a FILE that survives
# ---------------------------------------------------------------------------
# The file matters. When this script died at Step 2 on the first real
# golden-image boot test, `journalctl -u magic-first-boot.service` and
# `journalctl -t magic-first-boot` were BOTH empty -- so the single most
# important script on a new unit failed with no diagnosable trace, and
# finding the cause meant re-running it by hand under `bash -x`.
#
# Step 6c-2 wipes the journal on purpose (the unit's own history starts at
# first boot), which makes journald the wrong place to keep the record of
# what first boot did. This log is written before that wipe and is not
# touched by it.
FIRST_BOOT_LOG="/var/log/magic-first-boot.log"
mkdir -p "$(dirname "$FIRST_BOOT_LOG")" 2>/dev/null || true

log() {
    echo "$1"
    logger -t "magic-first-boot" "$1" 2>/dev/null || true
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" >> "$FIRST_BOOT_LOG" 2>/dev/null || true
}

# Say WHERE it died, not just that it did. set -euo pipefail aborts on the
# first failing command, and without this the operator gets an exit code and
# nothing else.
first_boot_failed() {
    local rc=$? line=${1:-?}
    log "=== FAILED at line ${line} (exit ${rc}): ${BASH_COMMAND}"
    log "=== A partial first boot leaves SOURCE-BOX state on this unit:"
    log "===   identity, saves, pairing and credentials may NOT be wiped."
    log "=== Full log: ${FIRST_BOOT_LOG}"
    log "=== Re-run by hand once fixed:"
    log "===   sudo bash /opt/magic_dingus_box/scripts/golden_image/first_boot.sh"
}
trap 'first_boot_failed "$LINENO"' ERR

log "=== Magic Dingus Box first-boot setup starting ==="
log "=== $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')"

# ---------------------------------------------------------------------------
# Step 1: Regenerate SSH host keys
# ---------------------------------------------------------------------------
log "[1/7] Regenerating SSH host keys..."

# UNCONDITIONAL rotation. This block used to skip when keys already existed,
# which made it DEAD CODE: a raw `dd` clone always carries the source's
# /etc/ssh/ssh_host_* , and prepare_for_cloning.sh deliberately leaves them in
# place (the operator's live SSH session to the source box needs them valid
# until restore_after_cloning.sh runs). So the "else" branch never once
# executed on a real clone, and every unit shipped with byte-identical SSH
# host PRIVATE keys.
#
# Why that matters: the shared material is the private key, sitting on an SD
# card in the customer's hands. Anyone who buys one unit extracts it and can
# then impersonate or MITM the SSH service of every other unit ever shipped.
# Trust-on-first-use offers no protection, because the presented key genuinely
# matches. The old "trusted LAN" justification was a bench assumption and does
# not survive contact with a shipping product.
#
# Removing first, rather than relying on ssh-keygen -A (which only fills in
# MISSING types), guarantees rotation of the types that are already present.
rm -f /etc/ssh/ssh_host_*
ssh-keygen -A
log "[1/7] SSH host keys rotated (fresh per-unit identity)"

# Restart sshd so the running daemon serves the new keys immediately rather
# than at next boot. Debian names the unit ssh.service; some images use sshd.
if systemctl is-active sshd.service &>/dev/null; then
    systemctl restart sshd.service
    log "[1/7] sshd restarted with new host keys"
elif systemctl is-active ssh.service &>/dev/null; then
    systemctl restart ssh.service
    log "[1/7] ssh restarted with new host keys"
else
    log "[1/7] Warning: sshd not running, keys will be used on next start"
fi

# ---------------------------------------------------------------------------
# Step 1b: Regenerate /etc/machine-id
# ---------------------------------------------------------------------------
# Nothing in the repo referenced machine-id before this, so every cloned unit
# carried the source Pi's. systemd derives the DHCP DUID from it by default,
# so two Magic Dingus Boxes on the same LAN present the same DUID and fight
# over one lease — one box intermittently loses its address. To a customer
# that looks like the kiosk randomly dropping off the network and the phone
# remote losing pairing, with nothing in any log to explain it.
#
# Done here rather than in prepare_for_cloning.sh so the SOURCE box keeps its
# identity: truncating it there would change the source's DHCP identity too.
# Deliberately placed BEFORE the filesystem expand — the expand reboots on
# some paths, and a duplicate machine-id must not survive even one DHCP
# exchange.
log "[1b/7] Regenerating machine-id..."
rm -f /etc/machine-id /var/lib/dbus/machine-id
systemd-machine-id-setup >/dev/null 2>&1 || true
# Debian symlinks the dbus copy at /var/lib/dbus/machine-id; recreate it so
# dbus and systemd agree rather than drifting to two different ids.
# Guarded: an absent /var/lib/dbus would otherwise abort the whole script
# HERE, under set -e, before a single credential wipe has run.
mkdir -p /var/lib/dbus 2>/dev/null || true
ln -sf /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null \
    || log "[1b/7] WARNING: could not link /var/lib/dbus/machine-id — dbus may drift"
log "[1b/7] machine-id regenerated: $(cut -c1-8 /etc/machine-id 2>/dev/null)..."

# ---------------------------------------------------------------------------
# Step 2: Expand root filesystem to fill SD card
# ---------------------------------------------------------------------------
log "[2/7] Expanding root filesystem..."

# Wait for udev to settle before querying block devices. Guarded: settle
# returns non-zero on its own timeout, and under set -e an unguarded call
# here aborts the script BEFORE every identity and credential wipe — the
# "defective product" outcome the rest of this step was rewritten to
# prevent. A settle timeout only means the device queries below might race;
# they have their own fallbacks.
udevadm settle 2>/dev/null || true

ROOT_PART=$(findmnt -n -o SOURCE /)
ROOT_DISK_NAME=$(lsblk -no pkname "$ROOT_PART" 2>/dev/null || true)

if [[ -z "$ROOT_DISK_NAME" ]]; then
    log "[2/7] WARNING: Could not determine parent disk for ${ROOT_PART}, skipping expansion"
else
    ROOT_DISK="/dev/${ROOT_DISK_NAME}"

    if [[ ! -b "$ROOT_DISK" ]]; then
        log "[2/7] WARNING: ${ROOT_DISK} is not a block device, skipping expansion"
    else
        PART_NUM=$(echo "$ROOT_PART" | grep -o '[0-9]*$')

        # Check if there is unallocated space after the root partition
        # Use parted to get the partition end and disk size in bytes.
        #
        # The `|| echo "0"` at the end of each pipeline is DEAD CODE kept only
        # for shape: a pipeline's status is awk's, and awk exits 0 even when
        # nothing matched — so a failed parse yields EMPTY, not "0". Empty
        # then flowed into $((DISK_END - PART_END)) as zero, GAP came out 0,
        # and the script logged "already fills the SD card" on a card that
        # was never expanded — the same class as the original parted -s bug,
        # and again only a larger-than-source card ever exercises it. The
        # explicit numeric validation below is what actually routes a failed
        # parse into the expand-anyway branch (growpart's NOCHANGE exit makes
        # running it on an already-full card safe).
        PART_END=$(parted -s "$ROOT_DISK" unit B print 2>/dev/null | awk "/^ ${PART_NUM} /{gsub(/B/,\"\"); print \$3}" || echo "0")
        DISK_END=$(parted -s "$ROOT_DISK" unit B print 2>/dev/null | grep "Disk ${ROOT_DISK}" | awk '{gsub(/B/,""); print $3}' || echo "0")
        [[ "$PART_END" =~ ^[0-9]+$ ]] || PART_END="0"
        [[ "$DISK_END" =~ ^[0-9]+$ ]] || DISK_END="0"

        # If we couldn't parse parted output, just run the expansion (safe to re-run)
        if [[ "$PART_END" == "0" || "$DISK_END" == "0" ]]; then
            NEEDS_EXPAND=true
        else
            # Skip if partition end is within 100MB of disk end
            THRESHOLD=$((100 * 1024 * 1024))
            GAP=$((DISK_END - PART_END))
            if [[ "$GAP" -lt "$THRESHOLD" ]]; then
                NEEDS_EXPAND=false
            else
                NEEDS_EXPAND=true
            fi
        fi

        if [[ "$NEEDS_EXPAND" == false ]]; then
            log "[2/7] Filesystem already fills the SD card, skipping expansion"
        else
            log "[2/7] Expanding partition ${PART_NUM} on ${ROOT_DISK}..."
            # `parted -s` PROMPTS when the partition is mounted --
            #   "Partition /dev/mmcblk0p2 is being used. Are you sure...?"
            # -- and in script mode answers NO and exits 1. Under set -e that
            # killed first_boot.sh right here, so NOTHING after Step 2 ever
            # ran: no device identity, no hostname, no saves wipe, no pairing
            # wipe, no Media Browser re-lock, no Wi-Fi wipe, and not the
            # Step 7 self-disable either.
            #
            # It hid because expansion only triggers when the card has more
            # than 100 MB of unused tail. The SOURCE card never does, so first
            # boot looked fine on every box that had already been through it.
            # The first customer-shaped test -- a fresh card 128 MB larger
            # than the source -- failed instantly (2026-08-04).
            #
            # growpart exists for exactly this: online growth of a mounted
            # root partition. parted with ---pretend-input-tty is the
            # fallback, which lets the "Yes" actually be answered.
            # growpart's exit codes matter: 0 = grown, 2 = NOCHANGE (already
            # fills the disk — expected whenever the gap measurement above
            # fell back to expand-anyway), anything else = real error. A real
            # growpart error falls through to the parted path rather than
            # giving up: the old `elif command -v growpart` shape meant a
            # growpart that existed-and-failed never tried parted at all.
            expand_ok=0
            already_full=0
            if command -v growpart >/dev/null 2>&1; then
                _grc=0
                growpart "$ROOT_DISK" "$PART_NUM" || _grc=$?
                if [[ "$_grc" -eq 0 ]]; then
                    expand_ok=1
                elif [[ "$_grc" -eq 2 ]]; then
                    already_full=1
                    log "[2/7] growpart: partition already fills the card (NOCHANGE)"
                else
                    log "[2/7] WARNING: growpart failed on ${ROOT_DISK} ${PART_NUM} (rc=${_grc}) — trying parted"
                fi
            fi
            if [[ "$expand_ok" -eq 0 && "$already_full" -eq 0 ]]; then
                if printf 'Yes\n' | parted ---pretend-input-tty "$ROOT_DISK" \
                         resizepart "$PART_NUM" 100%; then
                    expand_ok=1
                else
                    log "[2/7] WARNING: parted resizepart failed on ${ROOT_DISK}"
                fi
            fi

            if [[ "$expand_ok" -eq 1 || "$already_full" -eq 1 ]]; then
                # Notify kernel of the updated partition table
                partprobe "$ROOT_DISK" 2>/dev/null || true
                udevadm settle 2>/dev/null || true

                # resize2fs also runs on the NOCHANGE path: the partition may
                # already be full-size while the FILESYSTEM inside it is not
                # (a crashed earlier boot that grew one but not the other).
                # It is a fast no-op when nothing needs doing.
                log "[2/7] Resizing filesystem on ${ROOT_PART}..."
                if resize2fs "$ROOT_PART"; then
                    log "[2/7] Filesystem expanded successfully"
                else
                    log "[2/7] WARNING: resize2fs failed — unit keeps its current size"
                fi
            else
                # NOT fatal, deliberately. A unit that wastes the unused tail
                # of its card is a minor annoyance; a unit that skips the
                # identity and credential wipes because a partition tool
                # refused is a defective product. Boot continues.
                log "[2/7] WARNING: partition NOT expanded — continuing anyway."
                log "         The unit works but does not use the whole card."
                log "         Fix later: sudo growpart ${ROOT_DISK} ${PART_NUM} && sudo resize2fs ${ROOT_PART}"
            fi
        fi
    fi
fi

# ---------------------------------------------------------------------------
# Step 3: Generate unique device identity
# ---------------------------------------------------------------------------
log "[3/7] Generating unique device identity..."

DEVICE_INFO_PATH="${DATA_DIR}/device_info.json"

if [[ -f "$DEVICE_INFO_PATH" ]]; then
    log "[3/7] device_info.json already exists, skipping"
else
    DEVICE_UUID=$(cat /proc/sys/kernel/random/uuid)
    CREATED_AT=$(date +%s)

    # ORDER IS THE CONTRACT HERE: the hostname work happens FIRST, and
    # device_info.json — the file this whole block gates on — is written
    # LAST. It used to be the other way around, which made the block
    # non-idempotent in the worst possible direction: hostnamectl failing
    # once (systemd-hostnamed/dbus not up yet, this early in boot) aborted
    # the run AFTER the gate file existed, so every retry boot skipped the
    # block, the unit permanently kept the source box's hostname, collided
    # with it on mDNS/DHCP, and phone pairing silently broke. Writing the
    # gate record last means a failed attempt reruns in full on the next
    # boot.
    #
    # Set unique hostname derived from device UUID (e.g., magicpi-a3f2).
    # /etc/hostname is written DIRECTLY — a plain file write that cannot
    # depend on dbus being ready; hostnamectl below is only the live-runtime
    # nicety and is best-effort.
    SHORT_ID=$(echo "$DEVICE_UUID" | cut -c1-4)
    NEW_HOSTNAME="magicpi-${SHORT_ID}"
    printf '%s\n' "$NEW_HOSTNAME" > /etc/hostname

    # /etc/hosts has a `127.0.1.1 <old hostname>` line that systemd's
    # nss-files lookup uses to map the local hostname back to a
    # loopback address. We just changed the hostname in /etc/hostname,
    # but if /etc/hosts still says `127.0.1.1 magicpi`, every `sudo`
    # invocation prints
    #   sudo: unable to resolve host magicpi-XXXX: Temporary failure
    # and gets a small but visible delay while the resolver gives up.
    # Cosmetic in isolation but accumulates across boot scripts and
    # operator commands. Update /etc/hosts to match the new hostname.
    #
    # Use `sed -i` with a permissive pattern (any token after 127.0.1.1)
    # so this works regardless of what hostname the source had baked
    # in (`magicpi`, `magicpi-XXXX` from a re-clone, etc.). Append a
    # fallback line if the file doesn't have a 127.0.1.1 entry at all.
    if grep -qE '^127\.0\.1\.1\s+' /etc/hosts; then
        sed -i -E "s/^(127\.0\.1\.1)\s+.*$/\1 ${NEW_HOSTNAME} ${NEW_HOSTNAME}/" \
            /etc/hosts
        log "[3/7] Updated /etc/hosts: 127.0.1.1 → ${NEW_HOSTNAME}"
    else
        echo "127.0.1.1 ${NEW_HOSTNAME} ${NEW_HOSTNAME}" >> /etc/hosts
        log "[3/7] Appended 127.0.1.1 → ${NEW_HOSTNAME} to /etc/hosts (no prior entry)"
    fi

    # Live-runtime hostname, best-effort: /etc/hostname above is what makes
    # the change durable; this makes it take effect without a reboot. Either
    # tool may be unavailable this early — that must not abort the run.
    hostnamectl set-hostname "$NEW_HOSTNAME" 2>/dev/null \
        || hostname "$NEW_HOSTNAME" 2>/dev/null \
        || log "[3/7] WARNING: runtime hostname not applied (takes effect on reboot)"

    # The gate record, LAST. DATA_DIR may not exist yet on a minimal image
    # (Step 4 normally creates it, but that runs after this): an unguarded
    # redirect into a missing directory would abort the whole script before
    # any wipe.
    mkdir -p "$DATA_DIR"
    cat > "$DEVICE_INFO_PATH" <<DEVEOF
{
    "device_id": "${DEVICE_UUID}",
    "device_name": "Magic Dingus Box",
    "created_at": ${CREATED_AT}
}
DEVEOF

    chown "${MAGIC_USER}:${MAGIC_USER}" "$DEVICE_INFO_PATH"

    log "[3/7] Device identity created: ${DEVICE_UUID} (hostname: ${NEW_HOSTNAME})"
fi

# ---------------------------------------------------------------------------
# Step 4: Create required directories
# ---------------------------------------------------------------------------
log "[4/7] Creating required directories..."

mkdir -p "${DATA_DIR}/saves"
mkdir -p "${DATA_DIR}/states"
mkdir -p "${DATA_DIR}/media"
mkdir -p "${CONFIG_DIR}"

log "[4/7] Directories verified: saves, states, media, config"

# ---------------------------------------------------------------------------
# Step 5: Fix permissions
# ---------------------------------------------------------------------------
log "[5/7] Fixing file ownership..."

# Only fix ownership on directories/files that first-boot created or modified.
# The golden image already has correct ownership on ROMs, cores, binary, etc.
# A full recursive chown would take 30-120s on a ROM-laden 2GB Pi.
chown "${MAGIC_USER}:${MAGIC_USER}" "${DATA_DIR}/saves" "${DATA_DIR}/states" "${DATA_DIR}/media"
chown "${MAGIC_USER}:${MAGIC_USER}" "${CONFIG_DIR}"
if [[ -f "${DATA_DIR}/device_info.json" ]]; then
    chown "${MAGIC_USER}:${MAGIC_USER}" "${DATA_DIR}/device_info.json"
fi
log "[5/7] Set ownership on first-boot created directories and files"

# ---------------------------------------------------------------------------
# Step 6: Wipe Media Browser per-Pi state (cloned-image only)
# ---------------------------------------------------------------------------
# When a cloned SD is booted on a new Pi for the first time, the
# image still carries the source Pi's Media Browser state:
#   - services/.env has source's WG private key + auto-generated API
#     keys + qBit admin password
#   - services/config/radarr/* has source's library DB + API key
#   - services/config/prowlarr/* has source's API key + indexer
#     sync history
#   - services/config/qbittorrent/* has fastresume + cookies
#   - services/config/gluetun/* has VPN runtime state
#
# Sharing any of those across Pis is a problem (multi-device WG
# collision with ProtonVPN, identical API keys leaking auth, library
# state confusion). Wipe them here. The operator restores fresh per-Pi
# state via the Content Manager UI on the cloned Pi (drops in a new
# WireGuard config; setup_services.sh rebuilds everything else from
# the codified fixtures in scripts/data/).
#
# Note: first_boot.sh self-disables in Step 7, so this only ever runs
# once per cloned Pi. The source Pi's restore_after_cloning.sh disabled
# the unit before we ever got here, so this code path doesn't fire on
# the source Pi.
log "[6/7] Cleaning Media Browser per-Pi state..."

SERVICES_DIR="${INSTALL_DIR}/services"
if [[ -d "$SERVICES_DIR" ]]; then
    # Stop the Docker stack if it auto-started. On a fresh-cloned Pi,
    # gluetun would have failed to come up (no WG creds in .env that
    # we're about to delete anyway), but other containers may have
    # entered a healthy-ish state. Compose down cleanly.
    if [[ -f "$SERVICES_DIR/docker-compose.yml" ]] && command -v docker &>/dev/null; then
        (cd "$SERVICES_DIR" && docker compose down 2>&1 | sed 's/^/    /' || true)
        log "[6/7] Stopped Docker stack (was inherited from source image)"
    fi

    # Remove the .env (WG creds + API keys + qBit password)
    if [[ -f "$SERVICES_DIR/.env" ]]; then
        rm -f "$SERVICES_DIR/.env"
        log "[6/7] Wiped services/.env"
    fi

    # Wipe per-service config dirs. Each service will regenerate
    # fresh state on next start. Setup_services.sh re-creates the
    # full configuration from fixtures.
    #
    # `find -mindepth 1 -delete` is fully recursive (-delete implies -depth, so
    # non-empty subdirectories go too). That is deliberate and load-bearing: it
    # means this wipe needs no per-file list and already covers the *arr apps'
    # Backups/ zips, their logs/ trees and logs.db, none of which are named
    # anywhere in this script. Verified against the live box — 207 entries for
    # radarr, 573 for prowlarr, 17 for sonarr, Backups and logs included.
    #
    # What it does NOT cover is the .img.gz. This runs on the CLONE, at its
    # first boot; the artifact sits on a laptop or a shared drive from the
    # moment dd finishes until someone flashes and boots it, and anyone reading
    # a flashed card before that first boot keeps everything. That is why
    # prepare_for_cloning.sh Step 2c carries its own SECRET_PATHS list covering
    # the same Backups/ and logs/ paths: this loop protects the running unit,
    # Step 2c protects the image. Neither one makes the other redundant.
    wipe_count=0
    for svc in radarr prowlarr qbittorrent gluetun flaresolverr sonarr; do
        if [[ -d "$SERVICES_DIR/config/$svc" ]]; then
            file_count=$(find "$SERVICES_DIR/config/$svc" -mindepth 1 2>/dev/null | wc -l)
            if [[ $file_count -gt 0 ]]; then
                find "$SERVICES_DIR/config/$svc" -mindepth 1 -delete 2>/dev/null || true
                log "[6/7] Wiped services/config/$svc/ (${file_count} entries)"
                ((wipe_count++)) || true
            fi
        fi
    done
    log "[6/7] Media Browser per-Pi state cleaned (${wipe_count} dirs)"
else
    log "[6/7] services/ not present; nothing to clean (Pi without Media Browser)"
fi

# Reset media_browser_unlocked in settings.json. The kiosk has a built-in
# "secret sequence" that gates the entire Media Browser feature visibility
# (BTN1+BTN3 chord → BTN2 × 3 → rotary click). When the source Pi was
# being used to build the golden image, that flag was likely flipped to
# true and persisted. Cloned Pis would inherit "unlocked" state and show
# the Media Browser chip — defeating the "users won't know the feature
# exists" UX intent. Reset the flag here so cloned Pis start locked.
SETTINGS_PATH="${INSTALL_DIR}/config/settings.json"
if [[ -f "$SETTINGS_PATH" ]] && command -v python3 &>/dev/null; then
    python3 -c "
import json, sys
try:
    with open('$SETTINGS_PATH') as f:
        s = json.load(f)
    pb = s.get('playback', {})
    if pb.get('media_browser_unlocked', False):
        pb['media_browser_unlocked'] = False
        s['playback'] = pb
        with open('$SETTINGS_PATH', 'w') as f:
            json.dump(s, f, indent=2)
        print('reset media_browser_unlocked → false')
    else:
        print('media_browser_unlocked already false (or absent)')
except Exception as e:
    print(f'could not reset: {e}', file=sys.stderr)
" 2>&1 | sed 's/^/    /' || true
    log "[6/7] Reset media_browser_unlocked flag (cloned Pi starts locked)"
fi

# Reset the source operator's personal Media Browser browse-filter state to
# factory defaults. These keys record what the OPERATOR was browsing —
# Movies-vs-TV mode, library sort/filter, and the per-mode discover filters
# (decade / genre / language / rating / runtime / sort for popular and
# top-rated, movie and TV variants). A customer's fresh box should not open
# to someone else's 2020s-decade filter. The settings loader is
# absent-tolerant by contract (settings_persistence.cpp: "Every key is
# absent-tolerant"), so DELETING the keys is the reset — the kiosk fills in
# defaults on next load and rewrites the file.
#
# Deliberately NOT touched: display.mode (Modern TV is the shipped standard),
# every *_intensity key and mb_playback_show_frame (the tuned CRT overlay
# look IS the product standard), and audio settings.
if [[ -f "$SETTINGS_PATH" ]] && command -v python3 &>/dev/null; then
    python3 -c "
import json, sys
try:
    with open('$SETTINGS_PATH') as f:
        s = json.load(f)
    d = s.get('display', {})
    doomed = [k for k in d
              if k in ('mb_mode', 'mb_library_sort', 'mb_library_filter')
              or k.startswith(('mb_popular_', 'mb_toprated_',
                               'mb_tv_popular_', 'mb_tv_toprated_'))]
    for k in doomed:
        del d[k]
    if doomed:
        s['display'] = d
        with open('$SETTINGS_PATH', 'w') as f:
            json.dump(s, f, indent=2)
    print(f'reset {len(doomed)} personal browse-filter key(s) to defaults')
except Exception as e:
    print(f'could not reset filters: {e}', file=sys.stderr)
" 2>&1 | sed 's/^/    /' || true
    log "[6/7] Reset personal browse filters (customer starts at factory defaults)"
fi

# Phone Remote: a cloned Pi must NOT inherit the source's paired phones
# or its Flask HMAC secret. Wiping flask_secret.key forces create_app() to
# generate a fresh one on first boot, which invalidates any cookies issued
# by the source Pi (defense-in-depth even after paired_remotes.json is
# already wiped). The other files are transient state files that the kiosk
# / Flask recreate on demand — safe to delete.
log "[6/7] Wiping Phone Remote per-Pi state (paired phones, sessions, secrets)..."
for f in paired_remotes.json pairing_session.json pairing_audit.log pending_revocations.txt seek_request.json kiosk_status.json flask_secret.key; do
    if [[ -f "${DATA_DIR}/${f}" ]]; then
        rm -f "${DATA_DIR}/${f}"
        log "    wiped: ${f}"
    fi
done

# Any *.jsonl left in the data dir. This is a GLOB, deliberately, not more
# filenames added to the list above — that list is hand-maintained, and the
# way this gap was found is that something not on it shipped:
# remote_viewport_diag.jsonl accumulated 46 records of device user-agent
# strings and screen geometry from the temporary viewport instrumentation,
# and neither this script nor prepare_for_cloning.sh knew the name, so it
# would have been captured into the image and shipped on every unit.
#
# Every .jsonl under data/ is per-unit append-only debris by construction —
# a queue or a log that the kiosk or Flask recreates on demand. There is no
# .jsonl here that a fresh box needs to inherit, so a glob is safe and it
# catches the next one nobody thought to name. text_input_queue.jsonl is the
# other current example: transient phone-to-kiosk keystrokes, and a
# first-booting clone has nothing in flight.
for d in "$DATA_DIR" "${CPP_DIR}/build/data"; do
    [[ -d "$d" ]] || continue
    while IFS= read -r -d '' f; do
        rm -f "$f"
        log "    wiped: ${f#${INSTALL_DIR}/}"
    done < <(find "$d" -maxdepth 1 -name '*.jsonl' -print0 2>/dev/null)
done

# The build tree carries a SECOND, byte-identical copy of the same per-Pi
# state — build/data/ is populated when the C++ tests run and was never in
# the loop above, so a clone kept a working copy of the source box's Flask
# HMAC secret and its paired-phone records. A phone paired to the source
# would have authenticated against the clone.
#
# kiosk_status.json and seek_request.json are here for the same reason they
# are in the data/ list above: transient kiosk↔Flask state a fresh box
# regenerates on demand, and the build copy shipped when only data/ was wiped.
BUILD_DATA_DIR="${CPP_DIR}/build/data"
if [[ -d "$BUILD_DATA_DIR" ]]; then
    for f in paired_remotes.json pairing_session.json pairing_audit.log \
             pending_revocations.txt seek_request.json kiosk_status.json \
             flask_secret.key; do
        if [[ -f "${BUILD_DATA_DIR}/${f}" ]]; then
            rm -f "${BUILD_DATA_DIR}/${f}"
            log "    wiped: build/data/${f}"
        fi
    done
fi

# Media Browser watch history. media_browser.db holds the source operator's
# entire viewing record — every episode and movie watched, resume positions,
# timestamps (watch_state table, schema v3) — plus their library cache. It is
# personal data and must not ship on any unit. The glob covers the SQLite
# -wal/-shm sidecars, which travel with the db or corrupt it. Both copies:
# data/ (production) and build/data/ (populated by on-Pi test runs). The
# kiosk recreates an empty db with the full schema on first launch, so
# deleting it is always safe. prepare_for_cloning.sh also strips these from
# the image itself; this is the on-unit safety net for a card read between
# flash and first boot.
for d in "$DATA_DIR" "$BUILD_DATA_DIR"; do
    [[ -d "$d" ]] || continue
    shopt -s nullglob
    WATCH_DB_FILES=("$d"/media_browser.db*)
    shopt -u nullglob
    for f in "${WATCH_DB_FILES[@]}"; do
        rm -f "$f"
        log "    wiped: ${f#${INSTALL_DIR}/} (watch history)"
    done
done

# Operator's personal TMDB API key. Nothing wiped this before, so every unit
# made Media Browser metadata calls against one person's key — rate limits are
# per key, so the whole fleet shared one quota and one attribution.
# Globbed, not an exact path. The exact-path version wiped tmdb_api_key and
# nothing else, so any sibling copy — a hand-made .bak, an editor's ~ or .swp
# file, a half-written .tmp from an interrupted save — survived the wipe and
# shipped the operator's key on every cloned unit. The customer now supplies
# their own key through the Content Manager, so on a fresh box NO file in this
# family should exist; deleting the whole set is the correct, safe behaviour.
shopt -s nullglob
# Leading-star glob, matching prepare_for_cloning.sh: admin.py stages atomic
# writes as `.tmdb_api_key.<rand>.tmp`, and the leading dot defeats a glob
# that starts at the literal name — the one .tmp form this codebase actually
# produces was the one the old pattern missed.
TMDB_KEY_FILES=(/home/magic/.config/magic_dingus_box/*tmdb_api_key*)
shopt -u nullglob
for f in "${TMDB_KEY_FILES[@]}"; do
    rm -f "$f"
    log "    wiped: $(basename "$f")"
done

# Step 6c and 6d run UNCONDITIONALLY — they are the most important
# anti-leak steps for cloned Pis (inherited WiFi credentials and the
# stale cloning-backup marker that blocks future re-clones). Previously
# they were nested inside the python3-availability guard for Step 6b,
# which meant a Pi missing python3 (or with the settings file already
# absent) silently inherited the source operator's WiFi password and
# kept the stale clone marker. Both are now top-level so they always
# run regardless of python3 / settings.json state.

# Step 6c: Wipe inherited WiFi credentials.
#
# The source Pi was provisioned with the operator's home WiFi
# password (stored at /etc/NetworkManager/system-connections/
# *.nmconnection — readable only by root, but it IS persisted to
# the SD card and would be inherited by every clone). Whoever
# flashes this image onto a fresh Pi shouldn't auto-connect to
# the source operator's home network — both for privacy and
# because the credential won't even work in their location.
#
# The kiosk has a built-in Wi-Fi setup UI (Settings → Wi-Fi →
# Scan Networks → on-screen keyboard for passwords), so the
# cloned Pi's first-time operator can join their own network in
# ~30 seconds without ever needing SSH or HDMI keyboard.
#
# We delete the connection profile files; NetworkManager picks
# up the change automatically on its next reload. No need to
# restart NM here — Pi will boot, NM will start, find no saved
# profiles, sit idle until the operator sets one up.
NM_DIR="/etc/NetworkManager/system-connections"
if [[ -d "$NM_DIR" ]]; then
    # Count what we're about to wipe (for the log).
    nm_count=$(find "$NM_DIR" -maxdepth 1 -name "*.nmconnection" \
                   -type f 2>/dev/null | wc -l)
    if [[ "$nm_count" -gt 0 ]]; then
        find "$NM_DIR" -maxdepth 1 -name "*.nmconnection" -type f \
             -delete 2>/dev/null || true
        log "[6/7] Wiped $nm_count inherited WiFi profile(s) from $NM_DIR"
    else
        log "[6/7] No WiFi profiles to wipe (already clean)"
    fi
fi

# Step 6c-2: Wipe inherited system logs.
#
# The on-unit safety net for the operator's logs, mirroring the way every
# other secret here is handled in BOTH places: prepare_for_cloning.sh keeps
# them out of the .img.gz, this keeps them off a unit whose card was read
# before its first boot, or whose prepare aborted partway (that script runs
# under `set -euo pipefail`).
#
# Two distinct leaks, both measured on the source box:
#   - cloud-init renders the whole netplan dict at DEBUG, Wi-Fi password
#     field included: 58 plaintext occurrences of the PSK across
#     cloud-init.log and its three rotated siblings.
#   - the journal records every sudo command line verbatim, so joining Wi-Fi
#     with `nmcli ... password <psk>` writes the PSK straight into it, and it
#     holds a complete record of the operator's pre-ship activity besides.
#
# Vacuum by TIME, not size: --vacuum-size keeps the NEWEST slices, which is
# exactly where a recent Wi-Fi join sits.
log "[6/7] Wiping inherited system logs (cloud-init + journal)..."
rm -f /var/log/cloud-init.log* /var/log/cloud-init-output.log* 2>/dev/null || true
journalctl --rotate 2>/dev/null || true
journalctl --vacuum-time=1s >/dev/null 2>&1 || true
if [[ -d /var/log/audit ]]; then
    rm -f /var/log/audit/audit.log.* 2>/dev/null || true
    truncate -s 0 /var/log/audit/audit.log 2>/dev/null || true
fi
log "[6/7] Inherited logs cleared (this unit's own history starts now)"

# Step 6d: Wipe inherited cloning-backup state.
#
# /var/lib/magic-dingus-box/cloning_backup/ is created by
# prepare_for_cloning.sh on the source Pi just before the dd snapshot.
# It contains:
#   - in_progress         (marker file — gates prepare_for_cloning's
#                          "is a clone already in flight?" check)
#   - hostname, hosts     (per-Pi identity to restore on the source
#                          after dd completes)
#   - device_info.json    (per-Pi identity restored on source)
#
# restore_after_cloning.sh deletes the MARKER on the source Pi but
# not the rest of the backup files, by design — the script's
# primary job is to put the source Pi back into normal state, and
# leaving the (now-stale) backup files around is harmless on the
# source.
#
# However the dd snapshot captures whatever is on disk DURING dd,
# which is mid-prepare — meaning the marker AND backup files are
# all baked into the cloned SD image. When the image flashes onto
# a fresh Pi, those leftover files travel with it. The marker in
# particular is poisonous: any future attempt to clone the new Pi
# (e.g., golden-image v2 from a customer's already-running box)
# bails out with "prepare-for-cloning marker already present" and
# the operator has to SSH in to manually clean it up.
#
# Wiping the entire cloning_backup/ here is safe because:
#   - On a fresh-flashed Pi, those files are leftovers from the
#     SOURCE's prepare/dd cycle — not anything the NEW Pi cares
#     about. The source's restore_after_cloning.sh has already
#     run by the time the operator flashes our image.
#   - The new Pi has its own fresh device identity (Step 3 above),
#     fresh hostname, fresh hosts entry. The backed-up "magicpi"
#     values aren't relevant.
BACKUP_DIR="/var/lib/magic-dingus-box/cloning_backup"
if [[ -d "$BACKUP_DIR" ]]; then
    backup_count=$(find "$BACKUP_DIR" -mindepth 1 2>/dev/null | wc -l)
    if [[ "$backup_count" -gt 0 ]]; then
        find "$BACKUP_DIR" -mindepth 1 -delete 2>/dev/null || true
        log "[6/7] Wiped $backup_count inherited cloning-backup file(s) from $BACKUP_DIR (prevents stale marker from blocking future re-clones)"
    else
        log "[6/7] cloning_backup/ already empty"
    fi
fi

# Step 6e: Prune Pi 5-only game content on Pi 4 boards.
#
# ONE golden image serves both boards. The image is cloned from the
# Pi 5 production box, so it carries N64 + Dreamcast ROMs, per-game
# thumbnails, and playlists — systems the Pi 4B cannot run acceptably
# (mupen64plus needs more CPU than the 4B has; flycast's fill rate
# needs the Pi 5's V3D headroom). The kiosk ALSO hides these systems
# at runtime (PlatformProfile::unsupported_game_systems — that gate
# is the source of truth and protects the menu no matter what is on
# disk), so this pruning is purely about disk space: the Dreamcast
# CHDs alone are multiple GB of SD card a Pi 4 unit can never use,
# reclaimed here for operator videos instead.
#
# Board detection mirrors the kiosk's own parse_pi_model(): prefix
# match on /proc/device-tree/model (which carries a trailing NUL).
PI_MODEL="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
if [[ "$PI_MODEL" == "Raspberry Pi 4 "* ]]; then
    log "[6/7] Pi 4B detected — pruning Pi 5-only game content..."
    pruned_kb=0
    for path in \
        "${DATA_DIR}/roms/n64" \
        "${DATA_DIR}/roms/dreamcast" \
        "${DATA_DIR}/thumbnails/n64" \
        "${DATA_DIR}/thumbnails/dreamcast" \
        "${DATA_DIR}/playlists/games_n64.yaml" \
        "${DATA_DIR}/playlists/games_dreamcast.yaml"; do
        if [[ -e "$path" ]]; then
            sz=$(du -sk "$path" 2>/dev/null | cut -f1) || sz=0
            rm -rf "$path"
            pruned_kb=$((pruned_kb + sz))
            log "    pruned: ${path#${INSTALL_DIR}/} (${sz} KB)"
        fi
    done
    log "[6/7] Pi 5-only content pruned (~$((pruned_kb / 1024)) MB reclaimed for operator use)"
else
    log "[6/7] Board is '${PI_MODEL:-unknown}' — no platform pruning needed"
fi

# Step 6f: Wipe RetroArch saves and save states — units ship PRISTINE.
#
# History of this decision, because it has flipped twice: v1.5.2 wiped
# saves here; v1.5.3 reverted to ship the operator's "showcase" progress;
# 2026-08-03 the operator decided units ship pristine — every customer
# starts every game fresh. data/media (uploaded videos / playlist content)
# remains PRESERVED: that is the curated product content, not personal
# state. Only game progress is wiped.
#
# Directory skeletons are kept (rm contents, not the dirs) — Step 4 created
# them and RetroArch's sort_savefiles_by_content_enable recreates per-core
# subdirs on demand anyway.
#
# BOTH data/ AND build/data/ — the build tree carries a full SECOND copy of
# game progress, populated when the C++ tests run on the box. Measured on the
# source box 2026-08-03: data/ held 57 saves + 63 states, and build/data/ held
# another 40 saves + 63 states, 133 MB of them. Wiping only data/ (as this
# step first did) shipped the operator's entire save-state library on every
# unit under a path nobody was looking at — the exact thing the pristine-unit
# decision exists to prevent. Same dual-copy trap as flask_secret.key and
# media_browser.db above; this tree has now caught three separate leaks, so
# assume any new per-unit state has a build/data twin until proven otherwise.
log "[6/7] Wiping RetroArch saves + save states (pristine-unit policy)..."
for d in "${DATA_DIR}/saves" "${DATA_DIR}/states" \
         "${BUILD_DATA_DIR}/saves" "${BUILD_DATA_DIR}/states"; do
    if [[ -d "$d" ]]; then
        entry_count=$(find "$d" -mindepth 1 2>/dev/null | wc -l)
        if [[ "$entry_count" -gt 0 ]]; then
            find "$d" -mindepth 1 -delete 2>/dev/null || true
            log "    wiped: ${d#${INSTALL_DIR}/} (${entry_count} entries)"
        fi
    fi
done

# RetroArch keeps a SECOND set of personal state in the user's own config
# directory, which the data/ wipe above never touched. Found on the source
# box 2026-08-03: a Tomorrow Never Dies save, four content-history lists
# naming the games the operator had played, and three Dreamcast memory
# cards. All of it would have shipped on every unit.
#
# Surgical by necessity — system/ holds the PS1 BIOS (scph5501.bin) and the
# N64 core data, which MUST survive, right beside the Dreamcast VMU files
# which must not. Delete by exact pattern, never by directory.
#
# dc_nvmem.bin is deliberately KEPT: it is the Dreamcast's own NVRAM
# (console date and language), not game progress, and removing it makes
# flycast open its date-setting screen on a customer's first launch.
RA_DIR="${MAGIC_HOME}/.config/retroarch"
if [[ -d "$RA_DIR" ]]; then
    log "[6/7] Wiping RetroArch personal state (history, saves, memory cards)..."

    # Recently-played lists — these name titles, nothing else.
    rm -f "$RA_DIR"/content_history.lpl \
          "$RA_DIR"/content_image_history.lpl \
          "$RA_DIR"/content_music_history.lpl \
          "$RA_DIR"/content_video_history.lpl 2>/dev/null || true

    # Battery saves + save states living under the config dir (the other
    # copy, under data/, was wiped earlier in this step).
    for d in "$RA_DIR/saves" "$RA_DIR/states" "$RA_DIR/screenshots" "$RA_DIR/records"; do
        [[ -d "$d" ]] || continue
        find "$d" -mindepth 1 -delete 2>/dev/null || true
    done

    # Dreamcast memory cards. A cleared card makes flycast offer to format
    # on first use, which is correct for a new unit.
    rm -f "$RA_DIR"/system/dc/vmu_save_*.bin 2>/dev/null || true

    # RetroArch's own logs — a record of what the operator launched.
    [[ -d "$RA_DIR/logs" ]] && { find "$RA_DIR/logs" -mindepth 1 -delete 2>/dev/null || true; }

    log "[6/7] RetroArch personal state cleared (BIOS + core configs kept)"
fi

# NOTE on data/media: deliberately PRESERVED — uploaded videos and playlist
# content are the shipped product, same as ROMs, cores, and thumbnails.

# ---------------------------------------------------------------------------
# Step 6g: Converge the playback memory posture
# ---------------------------------------------------------------------------
# Clones cut from a current donor image inherit all four artifacts (the
# drop-ins and cmdline flags live on the image), making this a no-op.
# It exists for clones cut from OLDER donor images: they get the
# MemoryLow drop-ins, the zram tune, and the cmdline append here. A
# cmdline change on that path logs REBOOT_REQUIRED and simply arms on
# the box's next natural power cycle — first_boot must not add another
# reboot to the sequence (Step 2's expand may already have done one),
# and the posture is inert-but-harmless until then. Best-effort like
# every other converge step: a failure must not stop first boot.
MEMTUNE="/opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_memory_tuning.sh"
if [[ -f "$MEMTUNE" ]]; then
    log "[6g/7] Converging playback memory posture..."
    bash "$MEMTUNE" 2>&1 | while IFS= read -r line; do log "[6g/7] $line"; done \
        || log "[6g/7] WARNING: memory tuning failed (next OTA will retry)"
else
    log "[6g/7] setup_memory_tuning.sh not on this image; skipping"
fi

# ---------------------------------------------------------------------------
# Step 7: Disable this service (run once only)
# ---------------------------------------------------------------------------
log "[7/7] Disabling first-boot service..."

systemctl disable magic-first-boot.service
log "[7/7] magic-first-boot.service disabled (will not run again)"

log "=== Magic Dingus Box first-boot setup complete ==="
