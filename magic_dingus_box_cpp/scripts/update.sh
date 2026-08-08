#!/bin/bash
#
# Magic Dingus Box - OTA Update Script
#
# This script handles over-the-air updates for Magic Dingus Box devices.
# It runs on the Raspberry Pi and communicates with GitHub to check for
# and install updates.
#
# Usage:
#   ./update.sh check              # Check for updates (returns JSON)
#   ./update.sh install <ver> <url> # Install specific version
#   ./update.sh rollback           # Rollback to previous version
#
# ============================================================================
# OPERATOR-CONTENT PRESERVATION CONTRACT — READ BEFORE EDITING THE rsync CALLS
# ============================================================================
# The four `rsync --exclude` lists in this file (backup, install, internal
# rollback, user-initiated rollback) are the implementation of the contract
# documented at /OTA_UPDATE_GUARANTEES.md (repo root).
#
# That doc enumerates every path operators rely on surviving an OTA update:
# their videos, ROMs, saves, settings, Media Browser VPN credentials, etc.
# Pre-v1.4.3 a missing exclude wiped operator's services/.env (VPN creds)
# and services/config/* (Radarr library DB) on every update — a bug we
# only caught during physical update-flow verification.
#
# When you add a new category of operator content anywhere under
# INSTALL_DIR, you MUST:
#   1. Add the exclude entry to ALL FOUR rsync invocations in this script.
#      Inconsistency between them produces partial-update data loss.
#   2. Update OTA_UPDATE_GUARANTEES.md's "preserved" table in the same
#      commit so the contract stays in sync with the implementation.
# ============================================================================
#
set -euo pipefail

# Configuration
INSTALL_DIR="${MAGIC_BASE_PATH:-/opt/magic_dingus_box}"
BACKUP_DIR="${MAGIC_BACKUP_DIR:-${HOME}/.magic_dingus_box_backup}"
TEMP_DIR="${MAGIC_TEMP_DIR:-/tmp/magic_update}"
GITHUB_REPO="a-train-chain/magic_dingus_box"
GITHUB_API="${MAGIC_GITHUB_API:-https://api.github.com/repos/${GITHUB_REPO}/releases/latest}"
VERSION_FILE="${INSTALL_DIR}/VERSION"

# Testing mode overrides
# Set these environment variables to enable test mode:
#   MAGIC_SKIP_SYSTEMCTL=true  - Skip all systemctl calls
#   MAGIC_SKIP_BUILD=true      - Skip cmake/make build steps
#   MAGIC_DRY_RUN=true         - Skip all destructive operations
SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"
SKIP_BUILD="${MAGIC_SKIP_BUILD:-false}"
DRY_RUN="${MAGIC_DRY_RUN:-false}"

# Colors for terminal output (when not outputting JSON)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Log to stderr (so JSON output on stdout is clean)
log() {
    echo -e "${GREEN}[UPDATE]${NC} $1" >&2
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1" >&2
}

# Output JSON response
json_response() {
    local ok="$1"
    local message="$2"
    shift 2

    if [ "$ok" = "true" ]; then
        echo "{\"ok\": true, \"message\": \"$message\"$*}"
    else
        echo "{\"ok\": false, \"error\": {\"message\": \"$message\"}$*}"
    fi
}

# Output JSON progress (for streaming updates during install)
json_progress() {
    local stage="$1"
    local progress="$2"
    local message="${3:-}"
    echo "{\"ok\": true, \"stage\": \"$stage\", \"progress\": $progress, \"message\": \"$message\"}"
}

# Wrapper for systemctl calls (skipped in test mode)
run_systemctl() {
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: systemctl $*"
        return 0
    fi
    sudo systemctl "$@"
}

# Wrapper for build steps (skipped in test mode)
run_build() {
    if [ "$SKIP_BUILD" = "true" ]; then
        log "SKIP: Build step"
        return 0
    fi

    local build_dir="$INSTALL_DIR/magic_dingus_box_cpp/build"

    # ALWAYS build clean on an OTA. build/ is excluded from the install rsync
    # (see the --exclude below), so without this it is a long-lived directory
    # carrying objects compiled against the PREVIOUS release's headers. If a
    # release changes a struct layout, incremental make happily links old
    # objects against new ones: the build succeeds and the kiosk then segfaults
    # at startup inside an unrelated destructor. On a fielded box that is a
    # brick, unattended, with no one watching and no SSH.
    #
    # Observed for real on 2026-07-29 via deploy_cpp.sh (same defect, same
    # long-lived build dir) — 13 objects predated the headers they depended on.
    #
    # The cost is a full compile instead of a partial one. That is minutes on a
    # process that already downloads a tarball and restarts the kiosk, and it
    # is the correct trade against the failure it prevents. Rollback is
    # unaffected: create_backup() runs before this and deliberately includes
    # build/, so the previous working binary is still recoverable.
    log "Building clean (removing stale build directory)"
    rm -rf "$build_dir"
    mkdir -p "$build_dir"
    cd "$build_dir"

    # ENABLE_MEDIA_BROWSER defaults OFF in CMakeLists; production boxes
    # always build with it ON (runtime triple-gating hides it until
    # unlocked + VPN-provisioned). Before the clean-build fix above, the
    # long-lived build dir's CMake cache carried the ON from the last
    # deploy_cpp.sh run and masked this; with a fresh build dir, a plain
    # `cmake ..` would compile the movie kiosk OUT on every OTA.
    # BUILD_TESTS=OFF: the OTA rebuild used to compile the ENTIRE Catch2
    # test suite it never runs — real minutes on a Pi, plus a needless
    # GitHub fetch (Catch2) in the update path.
    if ! cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_MEDIA_BROWSER=ON -DBUILD_TESTS=OFF .. > /dev/null 2>&1; then
        return 1
    fi

    if ! make -j2 2>&1; then    # Reduced to prevent OOM on Pi 4B (1.5GB RAM)
        return 1
    fi

    return 0
}

# Retry download with exponential backoff
# Args: $1=output_file, $2=url, $3=max_time, $4=description
retry_download() {
    local output_file="$1"
    local url="$2"
    local max_time="${3:-600}"
    local description="${4:-file}"

    local max_attempts=3
    local wait_times=(5 15 45)

    for attempt in $(seq 1 $max_attempts); do
        log "Download attempt $attempt/$max_attempts for $description"

        local curl_exit=0
        curl -L -o "$output_file" \
            --connect-timeout 30 \
            --max-time "$max_time" \
            --progress-bar \
            "$url" 2>&2 || curl_exit=$?

        if [ "$curl_exit" -eq 0 ]; then
            return 0
        fi

        # Permanent errors (don't retry): malformed URL, protocol errors
        local permanent_errors="3 4 5 23 27"
        if echo " $permanent_errors " | grep -q " $curl_exit "; then
            log_error "Permanent download error (curl exit: $curl_exit)"
            return 1
        fi

        if [ "$attempt" -lt "$max_attempts" ]; then
            local wait_time="${wait_times[$((attempt-1))]}"
            log_warn "Download failed (curl exit: $curl_exit), retrying in ${wait_time}s..."
            json_progress "downloading" 15 "Retry in ${wait_time}s..."
            sleep "$wait_time"
        fi
    done

    log_error "Download failed after $max_attempts attempts"
    return 1
}

# Get device architecture for binary matching
get_device_arch() {
    local arch=$(uname -m)
    case "$arch" in
        aarch64) echo "arm64" ;;
        armv7l) echo "arm32" ;;
        x86_64) echo "x64" ;;
        *) echo "unknown" ;;
    esac
}

# Check if pre-compiled binary exists for this release.
#
# Returns a binary asset download URL on stdout, or empty string if no
# matching pre-compiled binary is published for this version+arch (the
# common case — most v1.x.y releases ship source tarballs only and the
# install path falls through to compile-from-source).
#
# This function is in the install hot path. Two bugs in the v1.5.0
# version of this code caused a silent mid-install exit that left the
# kiosk non-running. Both fixed in v1.5.1:
#
#   1. Double-"v" URL bug. Callers may pass either "v1.5.0" or "1.5.0";
#      the function unconditionally prepended "v" via `v${version}`,
#      producing /releases/tags/vv1.5.0 → 404. Fix: strip a single
#      leading "v" from the input before building the URL.
#
#   2. grep-no-match-kills-script. The grep|head|sed pipeline returns
#      exit code 1 when grep finds no matches (which is the COMMON
#      case — only releases with custom-attached binary assets have a
#      browser_download_url ending in -arm64*.tar.gz; most don't).
#      Under the script-level `set -euo pipefail`, that nonzero
#      pipeline propagated up and silently terminated the entire
#      install — VERSION had already been rsync'd to the new value,
#      but the rebuild + service-restart steps never ran, leaving
#      the kiosk inactive with no error reported to the caller.
#      Fix: run the parse in a subshell with relaxed shell flags.
#
# The subshell pattern (rather than `set +e` directly inside the
# function) is deliberate — it scopes the flag relaxation to this
# function only, so the install path's strict mode remains in force
# for the rest of the script and a rollback still triggers cleanly
# on any real install failure that occurs later.
get_binary_url() {
    local version="${1#v}"   # Strip optional leading "v" (bug #1 fix)
    local arch=$(get_device_arch)

    (
        set +e +o pipefail   # Bug #2 fix: tolerate grep no-match
        local response
        response=$(curl -s "https://api.github.com/repos/${GITHUB_REPO}/releases/tags/v${version}" 2>/dev/null)
        [ -z "$response" ] && exit 0

        echo "$response" \
            | grep -o "\"browser_download_url\": *\"[^\"]*-${arch}[^\"]*\.tar\.gz\"" \
            | head -1 \
            | sed 's/.*"\(http[^"]*\)".*/\1/'
    ) || true
}

# Get current installed version
get_current_version() {
    if [ -f "$VERSION_FILE" ]; then
        cat "$VERSION_FILE" | tr -d '[:space:]'
    else
        echo "0.0.0"
    fi
}

# Compare semantic versions (returns 0 if v1 < v2, 1 if v1 >= v2)
version_lt() {
    local v1="$1"
    local v2="$2"

    # Sort versions and check if v1 comes first
    if [ "$(printf '%s\n' "$v1" "$v2" | sort -V | head -n1)" = "$v1" ] && [ "$v1" != "$v2" ]; then
        return 0  # v1 < v2
    fi
    return 1  # v1 >= v2
}

# Check for available updates from GitHub
check_update() {
    # Temporarily relax shell flags inside this function. The script's
    # top-level `set -euo pipefail` is great for the install path
    # (catches mid-extract failures so we rollback cleanly) but it's
    # too aggressive for the metadata-parsing happy-path here. The
    # function does a lot of `var=$(echo "$response" | grep -o ... |
    # head -1 | sed ...)` chains; in some shell/GitHub-payload
    # combinations a non-fatal pipe element exits non-zero (a SIGPIPE
    # from `head` killing upstream `grep` after 1 line is the usual
    # culprit) and the script silently dies before the `cat << EOF`
    # JSON output, leaving the web admin's "Check for Updates" with
    # no JSON body to parse → user sees "UPDATE_CHECK_FAILED" with
    # only the [UPDATE] log lines as the error message.
    #
    # All the parsing below already handles empty results via
    # `[ -z "$var" ]` checks and emits proper json_response error
    # output, so disabling fast-fail here doesn't hide real problems.
    set +e
    set +o pipefail

    local current_version
    current_version=$(get_current_version)

    log "Current version: $current_version"
    log "Checking GitHub for updates..."

    # Fetch latest release info from GitHub API
    local response
    response=$(curl -s -H "Accept: application/vnd.github.v3+json" \
        --connect-timeout 10 \
        --max-time 30 \
        "$GITHUB_API" 2>/dev/null) || {
        json_response "false" "Failed to connect to GitHub"
        return 1
    }

    if [ -z "$response" ]; then
        json_response "false" "Empty response from GitHub"
        return 1
    fi

    # Check for API rate limiting or errors
    if echo "$response" | grep -q '"message".*API rate limit'; then
        json_response "false" "GitHub API rate limit exceeded. Try again later."
        return 1
    fi

    # Parse version (remove 'v' prefix)
    local latest_version
    latest_version=$(echo "$response" | grep -o '"tag_name": *"v[^"]*"' | head -1 | sed 's/.*"v\([^"]*\)".*/\1/')

    if [ -z "$latest_version" ]; then
        json_response "false" "Could not parse latest version from GitHub"
        return 1
    fi

    # Parse download URL for the SOURCE tarball specifically. Releases carry
    # two .tar.gz assets: the source tarball (magic-dingus-box-*.tar.gz) and
    # the pre-compiled binary (magic_dingus_box_cpp-arm64-*.tar.gz). A
    # generic first-match grep here depended on asset upload ORDER to pick
    # the right one — if the binary asset ever sorted first, install would
    # rsync --delete a binary-only tree over the whole install dir.
    local download_url
    download_url=$(echo "$response" | grep -o '"browser_download_url": *"[^"]*/magic-dingus-box-[^"/]*\.tar\.gz"' | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')

    # Fallback to GitHub's auto-generated source tarball if no release asset
    if [ -z "$download_url" ]; then
        download_url=$(echo "$response" | grep -o '"tarball_url": *"[^"]*"' | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')
    fi

    # Parse release notes (truncate to 500 chars)
    local release_notes
    release_notes=$(echo "$response" | grep -o '"body": *"[^"]*"' | head -1 | sed 's/"body": *"\(.*\)"/\1/' | head -c 500 | sed 's/"/\\"/g; s/\\n/ /g; s/\\r//g')

    # Parse published date
    local published_at
    published_at=$(echo "$response" | grep -o '"published_at": *"[^"]*"' | head -1 | sed 's/.*"\([^"]*\)".*/\1/')

    # Determine if update is available
    local update_available="false"
    if version_lt "$current_version" "$latest_version"; then
        update_available="true"
        log "Update available: $current_version -> $latest_version"
    else
        log "Already up to date"
    fi

    # Check if backup exists (for rollback capability)
    local has_backup="false"
    if [ -d "$BACKUP_DIR" ] && [ -f "$BACKUP_DIR/VERSION" ]; then
        has_backup="true"
    fi

    # Output JSON response
    cat << EOF
{
    "ok": true,
    "data": {
        "current_version": "$current_version",
        "latest_version": "$latest_version",
        "update_available": $update_available,
        "download_url": "$download_url",
        "release_notes": "$release_notes",
        "published_at": "$published_at",
        "has_backup": $has_backup
    }
}
EOF
}

# Compose-file delivery guard.
#
# /opt/magic_dingus_box/services/docker-compose.yml is the flattened
# copy every consumer reads (magic-dingus-services.service's
# WorkingDirectory, gluetun_cascade_restart.sh, setup_services.sh).
# Releases before v1.9.7 shipped NO top-level services/ in the tarball, so
# the install rsync's --delete DELETED this file on the first OTA of every
# fielded box — while the services/.env and services/config/* excludes kept
# the directory alive, so the damage stayed invisible until something ran
# `docker compose` and got "no configuration file provided: not found"
# (exit 14).
#
# release.yml now stages services/ into the tarball so the rsync delivers
# it. This is the belt to that suspenders: the canonical copy is ALWAYS in
# the tarball at magic_dingus_box_cpp/services/, so a future packaging
# regression can never again leave a box without a compose file.
# Deliberately NOT implemented as an rsync exclude — an exclude would
# freeze a stale compose file on the box forever and break the
# OTA_UPDATE_GUARANTEES.md promise that compose fixes flow through
# automatically.
#
# Called from install AND from both rollback paths: the rollback rsyncs
# restore from a BACKUP_DIR that was captured before this repair existed,
# so on every fielded box the first rollback after the repairing OTA would
# otherwise re-delete the compose file it had just been given.
ensure_compose_file() {
    if [ -f "$INSTALL_DIR/services/docker-compose.yml" ]; then
        return 0
    fi
    if [ -f "$INSTALL_DIR/magic_dingus_box_cpp/services/docker-compose.yml" ]; then
        log_warn "services/docker-compose.yml absent — restoring from in-tree copy"
        mkdir -p "$INSTALL_DIR/services"
        cp "$INSTALL_DIR/magic_dingus_box_cpp/services/docker-compose.yml" \
           "$INSTALL_DIR/services/docker-compose.yml"
        if [ -f "$INSTALL_DIR/magic_dingus_box_cpp/services/.env.example" ]; then
            cp "$INSTALL_DIR/magic_dingus_box_cpp/services/.env.example" \
               "$INSTALL_DIR/services/.env.example"
        fi
    else
        log_error "services/docker-compose.yml is missing and no in-tree copy exists at $INSTALL_DIR/magic_dingus_box_cpp/services/docker-compose.yml — Media Browser stack cannot start"
    fi
}

# Refresh the copies of shipped files that live OUTSIDE $INSTALL_DIR.
#
# The install rsync only ever writes inside /opt/magic_dingus_box. The six
# helper scripts under /usr/local/bin and the usb0 dnsmasq conf are copied
# out of the tree by setup_services.sh / install_deps.sh at PROVISIONING
# time — and both of those only re-run from an OTA when the Phone Remote
# markers are missing, which is never on a box cut from the golden image.
# So every fix to those files was frozen at image-cut time on every fielded
# unit: most importantly the drive-absent guard in qbit_port_sync.sh, which
# is the only thing stopping torrents from writing to the OS partition when
# the movie drive is unplugged mid-seed.
#
# Bounded and REFRESH-ONLY by design:
#   - each target is copied only if it ALREADY exists, so an OTA can never
#     provision a box with a helper it was not set up with;
#   - gated on SKIP_SYSTEMCTL exactly like the network-hardening call
#     below, because CI runners have passwordless sudo and the BATS suite
#     must never write into a runner's /usr/local/bin or /etc;
#   - `sudo -n` because this runs as the unprivileged magic-dingus-web
#     user, and -n fails fast instead of hanging on a dev machine.
# systemd unit files are deliberately NOT refreshed here — see
# OTA_UPDATE_GUARANTEES.md.
refresh_out_of_tree_files() {
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: out-of-tree helper refresh (test mode)"
        return 0
    fi

    local script_dir="${INSTALL_DIR}/magic_dingus_box_cpp/scripts"
    local src dest

    # Same name in both places.
    for helper in playback_services_pause.sh \
                  gluetun_cascade_restart.sh \
                  clear_radarr_cooldowns.py \
                  sync_qbit_password.sh \
                  auto_blocklist_stuck_warnings.py; do
        src="${script_dir}/${helper}"
        dest="/usr/local/bin/${helper}"
        if [ -f "$dest" ] && [ -f "$src" ]; then
            sudo -n install -m 0755 "$src" "$dest" \
                || log_warn "could not refresh $dest"
        fi
    done

    # The one rename: qbit_port_sync.sh -> qbit-port-sync.sh.
    if [ -f /usr/local/bin/qbit-port-sync.sh ] && [ -f "${script_dir}/qbit_port_sync.sh" ]; then
        sudo -n install -m 0755 "${script_dir}/qbit_port_sync.sh" \
            /usr/local/bin/qbit-port-sync.sh \
            || log_warn "could not refresh /usr/local/bin/qbit-port-sync.sh"
    fi

    # usb0 gadget DNS. install_deps.sh is the only writer of this file and
    # it is invoked from exactly one place (the Phone Remote bootstrap
    # above), so this is the ONLY delivery path it has. Reload dnsmasq only
    # when the content actually changed.
    src="${script_dir}/data/dnsmasq-usb0.conf"
    dest="/etc/dnsmasq.d/usb0.conf"
    if [ -f "$dest" ] && [ -f "$src" ] && ! cmp -s "$src" "$dest"; then
        if sudo -n install -m 0644 "$src" "$dest"; then
            log "usb0 dnsmasq config updated; reloading dnsmasq"
            run_systemctl reload-or-restart dnsmasq.service 2>/dev/null \
                || log_warn "dnsmasq reload failed (usb0 DNS applies on next restart)"
        else
            log_warn "could not refresh $dest"
        fi
    fi
}

# Install an update
install_update() {
    local target_version="$1"
    local download_url="$2"

    log "Starting update to version $target_version"

    # Validate inputs
    if [ -z "$target_version" ] || [ -z "$download_url" ]; then
        json_response "false" "Version and download URL are required"
        return 1
    fi

    # Validate URL (must be from GitHub)
    if [[ ! "$download_url" =~ ^https://github\.com/ ]] && [[ ! "$download_url" =~ ^https://api\.github\.com/ ]]; then
        json_response "false" "Invalid download URL (must be from GitHub)"
        return 1
    fi

    # Pre-flight checks
    json_progress "preparing" 5 "Running pre-flight checks..."

    # Check disk space (need at least 500MB free)
    local free_space
    free_space=$(df -m "$INSTALL_DIR" | awk 'NR==2 {print $4}')
    if [ "$free_space" -lt 500 ]; then
        json_response "false" "Insufficient disk space (need 500MB, have ${free_space}MB)"
        return 1
    fi

    # Create temp directory
    rm -rf "$TEMP_DIR"
    mkdir -p "$TEMP_DIR"

    # Download update
    json_progress "downloading" 10 "Downloading update package..."
    log "Downloading from: $download_url"

    if ! retry_download "$TEMP_DIR/update.tar.gz" "$download_url" 600 "update package"; then
        json_response "false" "Download failed after multiple attempts"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    json_progress "downloading" 30 "Download complete, verifying..."

    # Verify download (check file size and type)
    local file_size
    file_size=$(stat -c%s "$TEMP_DIR/update.tar.gz" 2>/dev/null || stat -f%z "$TEMP_DIR/update.tar.gz")
    if [ "$file_size" -lt 10000 ]; then
        json_response "false" "Downloaded file too small (possibly corrupted or API error)"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    # Verify it's a valid gzip file
    if ! gzip -t "$TEMP_DIR/update.tar.gz" 2>/dev/null; then
        json_response "false" "Downloaded file is not a valid gzip archive"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    json_progress "extracting" 35 "Extracting update package..."

    # Extract to temp location
    mkdir -p "$TEMP_DIR/extracted"
    if ! tar -xzf "$TEMP_DIR/update.tar.gz" -C "$TEMP_DIR/extracted" 2>&2; then
        json_response "false" "Failed to extract update package"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    # Find the actual content directory
    # Our release tarballs extract directly (files at root), but GitHub source
    # tarballs have a wrapper directory (repo-name-version/).
    # Check for VERSION file to determine correct root.
    local content_dir
    if [ -f "$TEMP_DIR/extracted/VERSION" ]; then
        # Our release tarball - content is at extraction root
        content_dir="$TEMP_DIR/extracted"
    else
        # GitHub source tarball - look for wrapper directory
        content_dir=$(find "$TEMP_DIR/extracted" -mindepth 1 -maxdepth 1 -type d | head -1)
        if [ -z "$content_dir" ]; then
            content_dir="$TEMP_DIR/extracted"
        fi
    fi

    log "Using content directory: $content_dir"

    # Sanity-check that this is actually a source tree BEFORE the backup /
    # rsync steps run. If the wrong asset was downloaded (e.g. the ARM64
    # binary tarball, which contains a single executable), the install
    # rsync's --delete would wipe every non-excluded file in INSTALL_DIR.
    if [ ! -d "$content_dir/magic_dingus_box_cpp/src" ] || [ ! -f "$content_dir/magic_dingus_box_cpp/CMakeLists.txt" ]; then
        json_response "false" "Update package is not a source tree (wrong asset downloaded?)"
        rm -rf "$TEMP_DIR"
        return 1
    fi

    json_progress "backing_up" 45 "Creating backup of current installation..."

    # Backup current installation
    log "Creating backup at $BACKUP_DIR"
    rm -rf "$BACKUP_DIR"

    # Create backup (exclude large user data to save space, but keep build for rollback)
    mkdir -p "$BACKUP_DIR"
    # Use --no-group --no-owner to avoid permission errors on group/owner changes
    # NOTE: We include build/ so rollback has a working binary
    # NOTE: thumbnails/, services/.env, services/config/* are also
    # excluded from backup because they're operator content that
    # didn't change between install + rollback (preserved by the
    # install rsync below); no point round-tripping them through
    # backup. Saves significant disk space on a populated kiosk.
    #
    # `--exclude 'VERSION'` is a DELIBERATE divergence from the other three
    # lists (the contract header above says they normally move together —
    # this is the documented exception, pinned by
    # tests/local/update_rsync_excludes.bats). VERSION is copied in
    # explicitly AFTER rsync returns 0, which turns $BACKUP_DIR/VERSION
    # into a completion marker: `has_backup` in check_update and both
    # rollback entry points gate on that one file. Before this, a backup
    # that died mid-transfer (near-full SD card, power cut) still wrote
    # VERSION — it is transfer entry #9 of ~4,800 — so the UI offered a
    # Rollback whose `--delete` then wiped the real installation down to
    # whatever fragment the partial backup held, and reported success.
    rsync -a --delete --no-group --no-owner \
        --include 'magic_dingus_box_cpp/data/thumbnails/systems/***' \
        --exclude 'VERSION' \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/saves/*' \
        --exclude 'magic_dingus_box_cpp/data/states/*' \
        --exclude 'magic_dingus_box_cpp/data/thumbnails/*' \
        --exclude 'magic_dingus_box_cpp/data/media_browser.db*' \
        --exclude 'services/.env' \
        --exclude 'services/config/*' \
        "$INSTALL_DIR/" "$BACKUP_DIR/" 2>&2 || {
        json_response "false" "Failed to create backup"
        # Reclaim the space and leave nothing that looks like a backup.
        # Without this the box is left with a full card AND a partial
        # BACKUP_DIR; the update is then not cleanly retryable.
        rm -rf "$BACKUP_DIR"
        rm -rf "$TEMP_DIR"
        return 1
    }

    # Completion marker (see the VERSION exclude above). Only written once
    # the backup rsync has fully succeeded.
    if [ -f "$INSTALL_DIR/VERSION" ]; then
        cp "$INSTALL_DIR/VERSION" "$BACKUP_DIR/VERSION"
    fi

    json_progress "stopping_services" 55 "Stopping C++ service..."

    # Only stop C++ service - web service stays running until the end
    # (stopping web service would kill our parent process and abort the update)
    log "Stopping C++ service..."
    run_systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
    sleep 1

    json_progress "installing" 60 "Installing new files..."

    # Install new files while preserving user content
    #
    # PRESERVED (user / per-Pi content - never overwritten):
    #   - data/media/*      - User-uploaded video files
    #   - data/roms/*       - User-uploaded ROM files
    #   - data/saves/*      - Game save files (SRAM per core)
    #   - data/states/*     - Save states (per core)
    #   - data/playlists/*  - User-created playlist YAML files
    #   - data/thumbnails/* - Game cover art populated externally.
    #                          EXCEPTION: data/thumbnails/systems/ IS
    #                          updated (see --include above the
    #                          excludes) — the system tiles are tracked
    #                          in git and new consoles need their tile
    #                          to reach existing boxes via OTA.
    #                          (deploy_cpp.sh syncs these from the
    #                          operator's local thumbnails dir, NOT
    #                          tracked in git, so the GitHub release
    #                          tarball doesn't contain them; without
    #                          this exclude the rsync --delete would
    #                          wipe every thumbnail when the operator
    #                          OTA-updates)
    #   - data/device_info.json - Device identity (UUID, hostname)
    #   - data/paired_remotes.json, data/flask_secret.key,
    #     data/pairing_session.json, data/pairing_audit.log
    #                       - Phone Remote pairing state. None of these
    #                          are in the release tarball, so without
    #                          excludes the rsync --delete below would
    #                          DELETE them → every paired phone silently
    #                          unpaired on every OTA.
    #   - data/kiosk_status.json, data/text_input_queue.jsonl,
    #     data/seek_request.json
    #                       - Transient kiosk<->web runtime files;
    #                          excluded so an OTA can't yank them out
    #                          from under the running web admin.
    #   - config/*          - User settings (settings.json, WiFi)
    #   - services/.env     - Per-Pi Media Browser config (WireGuard
    #                          private key, ProtonVPN credentials,
    #                          auto-generated Radarr/Prowlarr API
    #                          keys, qBit admin password). Wiping
    #                          this on update would force operators
    #                          to re-do the entire Media Browser
    #                          setup flow from scratch (drop in WG
    #                          config, wait 90 sec for setup_services,
    #                          etc.) every time they updated.
    #   - services/config/* - Per-Pi Media Browser stack state
    #                          (Radarr library DB, Prowlarr indexer
    #                          sync history, qBit fastresume + cookies,
    #                          Gluetun VPN runtime state, FlareSolverr
    #                          state). All of this is auto-generated
    #                          by the Docker stack; not in git, but
    #                          critical for the operator's working
    #                          setup.
    #   - build/*           - Local build artifacts (rebuilt fresh
    #                          from updated sources below).
    #   - data/media_browser.db* - Media Browser watch state (resume
    #                          positions, watched flags, NEW badges) for
    #                          movies AND TV, plus its -wal/-shm
    #                          sidecars. Per-box operator data, never in
    #                          the release tarball, so without this
    #                          exclude the --delete below wiped every
    #                          household's watch history on EVERY OTA —
    #                          silently, because WatchStore just
    #                          re-creates an empty schema and nothing
    #                          errors. (Added v1.9.8; WatchStore landed
    #                          in v1.9.0, after the 2026-07-30 exclude
    #                          audit that caught the same class of bug
    #                          for the pairing files.)
    #   - VERSION           - NOT stamped by the rsync. See below: it is
    #                          written only after a verified service
    #                          start, so an interrupted OTA can never
    #                          leave a kiosk-less box self-reporting
    #                          "up to date".
    #
    # UPDATED (system files - replaced with new version):
    #   - Source code (src/*)
    #   - Scripts (scripts/*) — incl. setup_services.sh, update.sh
    #   - Bezel assets (assets/bezels/*)
    #   - System assets (assets/*, data/intro/*)
    #   - Documentation (docs/*)
    #   - Build configuration (CMakeLists.txt, etc.)
    #   - services/docker-compose.yml — operator gets new VPN
    #     firewall rules, port-sync timer changes, etc.
    #     NOTE: only true as of v1.9.7. The repo has no top-level
    #     services/ dir, so until release.yml started staging one at
    #     package time this line described something that never
    #     happened — the rsync --delete below DELETED the box's
    #     compose file on its first OTA instead. See the delivery
    #     guard after the rsync.
    #
    # Use --no-group --no-owner to avoid permission errors
    # Exit code 23 means "some files could not transfer attributes" which is OK
    # `--exclude 'VERSION'` is deliberate and applies to THIS rsync only
    # (the two rollback rsyncs restore VERSION explicitly with `cp`, see
    # the comments there). The tarball contains exactly one file named
    # VERSION (./VERSION), and rsync --delete never deletes an excluded
    # file, so the box simply keeps its current value until :919 stamps
    # the new one after a verified service start — which is what the
    # NOTE below has always promised. Before this, VERSION was transferred
    # in the first handful of files, so an OTA killed anywhere between
    # here and the service start (power cut, or magic-dingus-web being
    # restarted out from under its own child) left a box with no kiosk
    # binary that answered "up to date" and hid the Install button.
    log "Installing new files..."
    local rsync_exit=0
    rsync -av --delete --no-group --no-owner \
        --include 'magic_dingus_box_cpp/data/thumbnails/systems/***' \
        --exclude 'VERSION' \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/saves/*' \
        --exclude 'magic_dingus_box_cpp/data/states/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/thumbnails/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'magic_dingus_box_cpp/data/paired_remotes.json' \
        --exclude 'magic_dingus_box_cpp/data/flask_secret.key' \
        --exclude 'magic_dingus_box_cpp/data/pairing_session.json' \
        --exclude 'magic_dingus_box_cpp/data/pairing_audit.log' \
        --exclude 'magic_dingus_box_cpp/data/kiosk_status.json' \
        --exclude 'magic_dingus_box_cpp/data/text_input_queue.jsonl' \
        --exclude 'magic_dingus_box_cpp/data/seek_request.json' \
        --exclude 'magic_dingus_box_cpp/data/media_browser.db*' \
        --exclude 'config/*' \
        --exclude 'magic_dingus_box_cpp/build/*' \
        --exclude 'services/.env' \
        --exclude 'services/config/*' \
        "$content_dir/" "$INSTALL_DIR/" 2>&2 || rsync_exit=$?

    # Add-only playlist sync: ship NEW default playlists (e.g. a new
    # console's playlist) without ever touching a playlist file that
    # already exists on the box — operator edits to existing playlists
    # stay untouched. Two gates keep this from polluting fielded boxes:
    #
    #   1. Same-system dedupe: if ANY existing playlist on the box already
    #      covers the same emulator_system, skip. Older boxes have
    #      pre-`games_*`-naming playlists (arcade.yaml vs games_arcade.yaml)
    #      — without this gate every rename in the repo would duplicate a
    #      menu row on every fielded box.
    #   2. Content existence: only add a playlist if at least one item is
    #      actually playable on this box (a youtube item, or a local path
    #      that exists). ROMs/videos are not in the release tarball, so a
    #      new console's playlist must wait until the box has its content
    #      — the next OTA after content arrives will add it.
    #
    # Side effect (deliberate): a default playlist the operator deleted
    # comes back on the next OTA if its content is still present.
    local box_pl_dir="$INSTALL_DIR/magic_dingus_box_cpp/data/playlists"
    if [ -d "$content_dir/magic_dingus_box_cpp/data/playlists" ]; then
        mkdir -p "$box_pl_dir"
        for new_pl in "$content_dir"/magic_dingus_box_cpp/data/playlists/*.yaml; do
            [ -e "$new_pl" ] || continue
            local pl_base
            pl_base="$(basename "$new_pl")"
            local pl_dest="$box_pl_dir/$pl_base"
            [ -e "$pl_dest" ] && continue

            # Gate 1: same-system dedupe (game playlists only)
            local new_sys existing_systems
            new_sys=$(grep -m1 -iE '^[[:space:]]*emulator_system:' "$new_pl" 2>/dev/null \
                        | awk -F: '{gsub(/[ "\047]/,"",$2); print tolower($2)}') || true
            if [ -n "$new_sys" ]; then
                existing_systems=$(grep -rhiE '^[[:space:]]*emulator_system:' "$box_pl_dir"/*.yaml 2>/dev/null \
                        | awk -F: '{gsub(/[ "\047]/,"",$2); print tolower($2)}' | sort -u) || true
                if echo "$existing_systems" | grep -qx "$new_sys"; then
                    log "Skipping $pl_base (box already has a playlist for system '$new_sys')"
                    continue
                fi
            fi

            # Gate 2: at least one item must be playable on this box
            local pl_playable=0
            if grep -qiE '^[[:space:]]*source_type:[[:space:]]*["'\'']?youtube' "$new_pl" 2>/dev/null; then
                pl_playable=1
            else
                while IFS= read -r item_path; do
                    [ -n "$item_path" ] || continue
                    if [ -e "$INSTALL_DIR/magic_dingus_box_cpp/$item_path" ] || [ -e "$INSTALL_DIR/$item_path" ]; then
                        pl_playable=1
                        break
                    fi
                done < <(grep -E '^[[:space:]]*path:' "$new_pl" 2>/dev/null \
                        | sed -E 's/^[[:space:]]*path:[[:space:]]*//; s/^["'\'']//; s/["'\'']$//')
            fi
            if [ "$pl_playable" -eq 0 ]; then
                log "Skipping $pl_base (no referenced content present on this box yet)"
                continue
            fi

            cp "$new_pl" "$pl_dest"
            log "Added new default playlist: $pl_base"
        done
    fi

    # Exit code 23 = some files couldn't transfer attrs (OK), 24 = vanished files (OK)
    if [ "$rsync_exit" -ne 0 ] && [ "$rsync_exit" -ne 23 ] && [ "$rsync_exit" -ne 24 ]; then
        log_error "Failed to install files (rsync exit code: $rsync_exit), attempting rollback..."
        rollback_internal
        return 1
    fi

    # Compose-file delivery guard (see ensure_compose_file above).
    ensure_compose_file

    # Refresh the shipped copies that live outside $INSTALL_DIR
    # (see refresh_out_of_tree_files above).
    refresh_out_of_tree_files

    # NOTE: VERSION file is written AFTER successful service start (see below)
    # This ensures version consistency if build fails

    # Check for pre-compiled binary (faster than compiling)
    local device_arch=$(get_device_arch)
    local binary_url=""
    local use_binary=false

    if [ "$device_arch" = "arm64" ]; then
        json_progress "checking_binary" 35 "Checking for pre-compiled binary..."
        binary_url=$(get_binary_url "$target_version")

        if [ -n "$binary_url" ]; then
            json_progress "downloading_binary" 40 "Downloading pre-compiled binary..."
            log "Found pre-compiled ARM64 binary: $binary_url"

            if retry_download "$TEMP_DIR/binary.tar.gz" "$binary_url" 300 "pre-compiled binary"; then
                if gzip -t "$TEMP_DIR/binary.tar.gz" 2>/dev/null; then
                    json_progress "installing_binary" 50 "Installing pre-compiled binary..."

                    mkdir -p "$TEMP_DIR/binary_extracted"
                    tar -xzf "$TEMP_DIR/binary.tar.gz" -C "$TEMP_DIR/binary_extracted"

                    # Verify binary architecture AND loadability. The CI
                    # binary is built on Debian Trixie; on a box running an
                    # older Debian (older glibc, libgpiod 1.x) it passes the
                    # arch check but cannot load — it would install, fail
                    # the service-start check, and roll back the ENTIRE
                    # update, leaving the box permanently unable to update
                    # via the binary path. ldd surfaces both missing
                    # libraries ("not found") and glibc version gaps
                    # ("version GLIBC_x.yz not found"); either means this
                    # box must compile from source instead.
                    if ! file "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" 2>/dev/null | grep -q "aarch64"; then
                        log_warn "Binary architecture mismatch, will compile from source"
                    elif ldd "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" 2>&1 | grep -q "not found"; then
                        log_warn "Pre-compiled binary needs newer system libraries than this OS provides; will compile from source"
                    else
                        mkdir -p "$INSTALL_DIR/magic_dingus_box_cpp/build"
                        cp "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" "$INSTALL_DIR/magic_dingus_box_cpp/build/"
                        chmod +x "$INSTALL_DIR/magic_dingus_box_cpp/build/magic_dingus_box_cpp"
                        use_binary=true
                        log "Using pre-compiled ARM64 binary"
                    fi
                else
                    log_warn "Binary download corrupt, will compile from source"
                fi
            else
                log_warn "Binary download failed, will compile from source"
            fi
        else
            log "No pre-compiled binary found for this version"
        fi
    fi

    # Only build from source if no binary available
    if [ "$use_binary" = false ]; then
        json_progress "building" 60 "Compiling from source (this may take 8-10 minutes)..."

        # Rebuild C++ application
        log "Building application from source..."

        json_progress "building" 70 "Compiling..."

        if ! run_build; then
            log_error "Build failed, attempting rollback..."
            json_progress "error" 70 "Build failed, rolling back..."
            rollback_internal
            return 1
        fi
    fi

    # Phone Remote bootstrap (idempotent). The Phone Remote feature added
    # new system deps (python3-pip, python3-evdev, flask-sock) and a udev
    # rule for /dev/uinput. An existing Pi OTA-updating to a release
    # introducing these would otherwise get the new binary but no deps,
    # silently breaking the WS path. Check two markers; if either is
    # missing, re-run install_deps.sh + setup_services.sh (both are
    # idempotent — cheap on already-provisioned Pis).
    json_progress "phone_remote_bootstrap" 85 "Checking Phone Remote dependencies..."
    PHONE_REMOTE_DEPS_OK=1
    if ! python3 -c "import flask_sock" 2>/dev/null; then
        log "Phone Remote: flask-sock not installed; bootstrap needed"
        PHONE_REMOTE_DEPS_OK=0
    fi
    if [[ ! -f /etc/udev/rules.d/90-magicdingus-uinput.rules ]]; then
        log "Phone Remote: uinput udev rule missing; bootstrap needed"
        PHONE_REMOTE_DEPS_OK=0
    fi
    if [[ "$PHONE_REMOTE_DEPS_OK" -eq 0 ]]; then
        if [[ -x "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/install_deps.sh" ]]; then
            bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/install_deps.sh" \
                || log_warn "install_deps.sh failed (Phone Remote will be degraded)"
        fi
        if [[ -x "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_services.sh" ]]; then
            bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_services.sh" \
                || log_warn "setup_services.sh failed (Phone Remote will be degraded)"
        fi
    else
        log "Phone Remote: deps already provisioned; skipping bootstrap"
    fi

    # RetroArch core bootstrap (idempotent). New releases can reference new
    # emulator cores (v1.7.x added N64 + Dreamcast); the cores are binary
    # .so files that are NOT in the release tarball (gitignored). Scan the
    # box's live playlists for every referenced core and run
    # install_cores.sh (apt + aarch64 core repo) only if one is missing.
    # Scanning the BOX's playlists (not the release's) is deliberate: the
    # add-only playlist sync above only adds a game playlist when its
    # content is present, so a box only fetches cores it can actually use.
    json_progress "cores_bootstrap" 87 "Checking emulator cores..."
    local cores_user="${SUDO_USER:-$(id -un)}"
    local cores_home
    cores_home="$(getent passwd "$cores_user" | cut -d: -f6)"
    [ -n "$cores_home" ] || cores_home="$HOME"
    local cores_dir="${cores_home}/.config/retroarch/cores"
    local missing_core=0
    while IFS= read -r core_name; do
        [ -n "$core_name" ] || continue
        if [ ! -f "${cores_dir}/${core_name}.so" ]; then
            log "Emulator core missing: ${core_name}.so"
            missing_core=1
        fi
    done < <(grep -rhE '^[[:space:]]*emulator_core:' \
                "$INSTALL_DIR/magic_dingus_box_cpp/data/playlists/"*.yaml 2>/dev/null \
                | awk -F: '{gsub(/[ "\047]/,"",$2); print $2}' | sort -u)
    if [ "$missing_core" -eq 1 ]; then
        if [ -x "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/install_cores.sh" ]; then
            log "Installing missing RetroArch cores..."
            json_progress "cores_bootstrap" 88 "Installing emulator cores..."
            bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/install_cores.sh" \
                || log_warn "install_cores.sh failed (new-system games may not launch)"
        else
            log_warn "install_cores.sh not found; skipping core bootstrap"
        fi
    else
        log "All referenced emulator cores present"
    fi

    # Activate the any-Wi-Fi network posture delivered by this update.
    # The rsync above only lands FILES; without this call an OTA-updated
    # field unit would carry the installer forever and never run it —
    # the golden image gets the posture baked into /etc, but fielded
    # boxes only ever update. Idempotent, never drops the link, and a
    # failure must not fail the update (the dispatcher will also be
    # installed by any future provisioning run).
    #
    # `sudo -n` is load-bearing: this script runs as the web-service user
    # (magic-dingus-web is User=magic — verified live), and the installer's
    # first act is an EUID root check, so the original unprivileged `bash`
    # call failed with "must run as root" on EVERY OTA — the one delivery
    # path this hook exists for. The magic user has passwordless sudo (the
    # run_systemctl wrapper above depends on the same fact); -n makes a
    # sudo that would prompt fail instantly instead of hanging a
    # dev-machine run. Gated on SKIP_SYSTEMCTL like every other
    # system-touching call: CI runners have passwordless sudo too, and the
    # BATS suite must never rewrite a runner's NetworkManager config.
    #
    # The test is `-f`, not `-x`, and that is load-bearing: the installer
    # shipped mode 0644 in the v1.9.6/v1.9.7 tarballs, so the old `-x` gate
    # silently skipped it on EVERY OTA — the entire any-Wi-Fi hardening was
    # inert on every fielded box and the elif chain had no else, so nothing
    # was logged. The file mode is fixed in git as of v1.9.8, but line 894
    # invokes via `sudo -n bash` anyway, so the bit was never needed here.
    if [ "$SKIP_SYSTEMCTL" = "true" ]; then
        log "SKIP: network hardening (test mode)"
    elif [ -f "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_network_hardening.sh" ]; then
        log "Applying network hardening (IPv6 off + public-DNS-first)..."
        sudo -n bash "${INSTALL_DIR}/magic_dingus_box_cpp/scripts/setup_network_hardening.sh" \
            || log_warn "network hardening install failed (will retry on next provisioning run)"
    else
        log_warn "setup_network_hardening.sh not found in this release; skipping network hardening"
    fi

    json_progress "restarting_services" 90 "Restarting services..."

    # Reload systemd and start C++ app
    log "Restarting services..."
    run_systemctl daemon-reload
    if ! run_systemctl start magic-dingus-box-cpp.service 2>/dev/null; then
        log_warn "Service start command failed, checking if active..."
    fi
    sleep 2

    # Verify service actually started
    if [ "$SKIP_SYSTEMCTL" != "true" ]; then
        if ! run_systemctl is-active magic-dingus-box-cpp.service >/dev/null 2>&1; then
            log_error "Service failed to start, rolling back..."
            json_progress "error" 90 "Service failed to start, rolling back..."
            rollback_internal
            return 1
        fi
    fi

    # Only commit VERSION after successful service start
    echo "$target_version" > "$INSTALL_DIR/VERSION"
    log "VERSION updated to $target_version"

    # Cleanup temp files
    rm -rf "$TEMP_DIR"

    json_progress "complete" 100 "Update complete!"
    log "Update to version $target_version complete!"

    # Final success response - output BEFORE restarting web service
    cat << EOF
{
    "ok": true,
    "stage": "complete",
    "progress": 100,
    "message": "Update complete!",
    "new_version": "$target_version"
}
EOF

    # Restart web service AFTER outputting final JSON
    # This will cause our parent process to be killed, but that's OK
    # since we've already output the completion message
    run_systemctl restart magic-dingus-web.service 2>/dev/null || true
}

# Internal rollback function (used during failed updates)
rollback_internal() {
    # Gate on the completion marker, not merely on the directory: a backup
    # that died mid-transfer has a tree but no VERSION, and restoring from
    # it with --delete would take the real installation down with it. Same
    # predicate `has_backup` uses, so the UI and the script agree.
    if [ ! -f "$BACKUP_DIR/VERSION" ]; then
        log_error "No complete backup available for rollback"
        return 1
    fi

    log "Rolling back to previous version..."

    # Only stop C++ service - don't stop web service during rollback
    run_systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true

    # Restore backup. Same exclude list as the install rsync — the
    # rollback should leave operator content alone, NOT roll it back
    # to whatever was in the pre-update backup. Specifically:
    #   - Anything under data/* that the install path preserved is
    #     ALREADY current; restoring from backup would do nothing or
    #     wipe newly-added content (e.g., a video the operator
    #     uploaded between the failed install and this rollback).
    #   - services/.env + services/config/* must be preserved through
    #     rollback. Otherwise: install fails partway through →
    #     rollback wipes VPN credentials → operator's Media Browser
    #     dies even though the kiosk binary rolled back successfully.
    #
    # `VERSION` is deliberately NOT excluded here (unlike the install and
    # backup rsyncs): a rollback MUST restore the old version number, and
    # the explicit `cp` below does it regardless. Do not "harmonise" the
    # lists by adding it.
    rsync -a --delete --no-group --no-owner \
        --include 'magic_dingus_box_cpp/data/thumbnails/systems/***' \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/saves/*' \
        --exclude 'magic_dingus_box_cpp/data/states/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/thumbnails/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'magic_dingus_box_cpp/data/paired_remotes.json' \
        --exclude 'magic_dingus_box_cpp/data/flask_secret.key' \
        --exclude 'magic_dingus_box_cpp/data/pairing_session.json' \
        --exclude 'magic_dingus_box_cpp/data/pairing_audit.log' \
        --exclude 'magic_dingus_box_cpp/data/kiosk_status.json' \
        --exclude 'magic_dingus_box_cpp/data/text_input_queue.jsonl' \
        --exclude 'magic_dingus_box_cpp/data/seek_request.json' \
        --exclude 'magic_dingus_box_cpp/data/media_browser.db*' \
        --exclude 'config/*' \
        --exclude 'services/.env' \
        --exclude 'services/config/*' \
        "$BACKUP_DIR/" "$INSTALL_DIR/" || true

    # Explicitly restore VERSION file from backup
    if [ -f "$BACKUP_DIR/VERSION" ]; then
        cp "$BACKUP_DIR/VERSION" "$INSTALL_DIR/VERSION"
        log "VERSION restored from backup"
    fi

    # The backup predates the v1.9.7 compose repair on every fielded box,
    # so the rsync above can re-delete the compose file this update just
    # delivered. Re-run the guard against the (already restored) in-tree
    # copy.
    ensure_compose_file

    # Restart C++ service (web service will be restarted at end of main function)
    run_systemctl daemon-reload
    run_systemctl start magic-dingus-box-cpp.service 2>/dev/null || true

    local restored_version
    restored_version=$(get_current_version)
    log "Rolled back to version $restored_version"
}

# User-initiated rollback
rollback() {
    # Completion-marker gate — see rollback_internal.
    if [ ! -f "$BACKUP_DIR/VERSION" ]; then
        json_response "false" "No complete backup available for rollback"
        return 1
    fi

    local backup_version
    if [ -f "$BACKUP_DIR/VERSION" ]; then
        backup_version=$(cat "$BACKUP_DIR/VERSION" | tr -d '[:space:]')
    else
        backup_version="unknown"
    fi

    json_progress "stopping_services" 10 "Stopping C++ service..."

    # Only stop C++ service - don't stop web service during rollback
    log "Stopping C++ service..."
    run_systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
    sleep 1

    json_progress "restoring" 30 "Restoring previous version..."

    # Restore backup (preserve user data — same exclude list as
    # install + internal rollback so user/per-Pi content is left
    # alone in either direction).
    #
    # As in rollback_internal: `VERSION` is deliberately NOT excluded —
    # a rollback must restore the old version number.
    log "Restoring from backup..."
    local rsync_exit=0
    rsync -av --delete --no-group --no-owner \
        --include 'magic_dingus_box_cpp/data/thumbnails/systems/***' \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/saves/*' \
        --exclude 'magic_dingus_box_cpp/data/states/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/thumbnails/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'magic_dingus_box_cpp/data/paired_remotes.json' \
        --exclude 'magic_dingus_box_cpp/data/flask_secret.key' \
        --exclude 'magic_dingus_box_cpp/data/pairing_session.json' \
        --exclude 'magic_dingus_box_cpp/data/pairing_audit.log' \
        --exclude 'magic_dingus_box_cpp/data/kiosk_status.json' \
        --exclude 'magic_dingus_box_cpp/data/text_input_queue.jsonl' \
        --exclude 'magic_dingus_box_cpp/data/seek_request.json' \
        --exclude 'magic_dingus_box_cpp/data/media_browser.db*' \
        --exclude 'config/*' \
        --exclude 'services/.env' \
        --exclude 'services/config/*' \
        "$BACKUP_DIR/" "$INSTALL_DIR/" 2>&2 || rsync_exit=$?

    if [ "$rsync_exit" -ne 0 ] && [ "$rsync_exit" -ne 23 ] && [ "$rsync_exit" -ne 24 ]; then
        json_response "false" "Failed to restore backup"
        return 1
    fi

    # Explicitly restore VERSION file from backup
    if [ -f "$BACKUP_DIR/VERSION" ]; then
        cp "$BACKUP_DIR/VERSION" "$INSTALL_DIR/VERSION"
        log "VERSION restored from backup"
    fi

    # See rollback_internal: the backup can predate the compose repair.
    ensure_compose_file

    json_progress "restarting_services" 80 "Restarting services..."

    # Restart C++ service
    log "Restarting C++ service..."
    run_systemctl daemon-reload
    run_systemctl start magic-dingus-box-cpp.service 2>/dev/null || true

    local restored_version
    restored_version=$(get_current_version)

    json_progress "complete" 100 "Rollback complete!"
    log "Rolled back to version $restored_version"

    cat << EOF
{
    "ok": true,
    "stage": "complete",
    "progress": 100,
    "message": "Rollback complete!",
    "version": "$restored_version"
}
EOF

    # Restart web service AFTER outputting final JSON
    run_systemctl restart magic-dingus-web.service 2>/dev/null || true
}

# Print usage
usage() {
    echo "Magic Dingus Box Update Script"
    echo ""
    echo "Usage: $0 <command> [args]"
    echo ""
    echo "Commands:"
    echo "  check                    Check for available updates"
    echo "  install <version> <url>  Install a specific version"
    echo "  rollback                 Rollback to previous version"
    echo "  version                  Show current version"
    echo ""
    echo "Examples:"
    echo "  $0 check"
    echo "  $0 install 1.0.1 https://github.com/.../release.tar.gz"
    echo "  $0 rollback"
    echo ""
}

# Main command dispatcher
case "${1:-}" in
    check)
        check_update
        ;;
    install)
        if [ -z "${2:-}" ] || [ -z "${3:-}" ]; then
            json_response "false" "Usage: $0 install <version> <download_url>"
            exit 1
        fi
        install_update "$2" "$3"
        ;;
    rollback)
        rollback
        ;;
    version)
        echo "$(get_current_version)"
        ;;
    --help|-h|help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
