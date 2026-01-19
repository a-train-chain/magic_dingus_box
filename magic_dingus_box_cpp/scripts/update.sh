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
        echo "{\"ok\": true, \"message\": \"$message\"$@}"
    else
        echo "{\"ok\": false, \"error\": {\"message\": \"$message\"}$@}"
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
    mkdir -p "$build_dir"
    cd "$build_dir"

    if ! cmake .. > /dev/null 2>&2; then
        return 1
    fi

    if ! make -j2 2>&2; then    # Reduced to prevent OOM on Pi 4B (1.5GB RAM)
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

# Check if pre-compiled binary exists for this release
get_binary_url() {
    local version="$1"
    local arch=$(get_device_arch)

    local response
    response=$(curl -s "https://api.github.com/repos/${GITHUB_REPO}/releases/tags/v${version}" 2>/dev/null)

    [ -z "$response" ] && echo "" && return

    # Find binary asset URL for this architecture
    echo "$response" | grep -o "\"browser_download_url\": *\"[^\"]*-${arch}[^\"]*\.tar\.gz\"" | \
        head -1 | sed 's/.*"\(http[^"]*\)".*/\1/'
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

    # Parse download URL for the tarball
    local download_url
    download_url=$(echo "$response" | grep -o '"browser_download_url": *"[^"]*\.tar\.gz"' | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')

    # Fallback to tarball URL if no release asset
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

    json_progress "backing_up" 45 "Creating backup of current installation..."

    # Backup current installation
    log "Creating backup at $BACKUP_DIR"
    rm -rf "$BACKUP_DIR"

    # Create backup (exclude large user data to save space, but keep build for rollback)
    mkdir -p "$BACKUP_DIR"
    # Use --no-group --no-owner to avoid permission errors on group/owner changes
    # NOTE: We include build/ so rollback has a working binary
    rsync -a --delete --no-group --no-owner \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        "$INSTALL_DIR/" "$BACKUP_DIR/" 2>&2 || {
        json_response "false" "Failed to create backup"
        rm -rf "$TEMP_DIR"
        return 1
    }

    json_progress "stopping_services" 55 "Stopping C++ service..."

    # Only stop C++ service - web service stays running until the end
    # (stopping web service would kill our parent process and abort the update)
    log "Stopping C++ service..."
    run_systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
    sleep 1

    json_progress "installing" 60 "Installing new files..."

    # Install new files while preserving user content
    #
    # PRESERVED (user content - never overwritten):
    #   - data/media/*      - User-uploaded video files
    #   - data/roms/*       - User-uploaded ROM files
    #   - data/playlists/*  - User-created playlist YAML files
    #   - data/device_info.json - Device configuration
    #   - config/*          - User settings (settings.json, WiFi, etc.)
    #   - build/*           - Local build artifacts
    #
    # UPDATED (system files - replaced with new version):
    #   - Source code (src/*)
    #   - Scripts (scripts/*)
    #   - System assets (assets/*, data/intro/*)
    #   - Documentation (docs/*)
    #   - Build configuration (CMakeLists.txt, etc.)
    #
    # Use --no-group --no-owner to avoid permission errors
    # Exit code 23 means "some files could not transfer attributes" which is OK
    log "Installing new files..."
    local rsync_exit=0
    rsync -av --delete --no-group --no-owner \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'config/*' \
        --exclude 'magic_dingus_box_cpp/build/*' \
        "$content_dir/" "$INSTALL_DIR/" 2>&2 || rsync_exit=$?

    # Exit code 23 = some files couldn't transfer attrs (OK), 24 = vanished files (OK)
    if [ "$rsync_exit" -ne 0 ] && [ "$rsync_exit" -ne 23 ] && [ "$rsync_exit" -ne 24 ]; then
        log_error "Failed to install files (rsync exit code: $rsync_exit), attempting rollback..."
        rollback_internal
        return 1
    fi

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

                    # Verify binary architecture
                    if file "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" 2>/dev/null | grep -q "aarch64"; then
                        mkdir -p "$INSTALL_DIR/magic_dingus_box_cpp/build"
                        cp "$TEMP_DIR/binary_extracted/magic_dingus_box_cpp" "$INSTALL_DIR/magic_dingus_box_cpp/build/"
                        chmod +x "$INSTALL_DIR/magic_dingus_box_cpp/build/magic_dingus_box_cpp"
                        use_binary=true
                        log "Using pre-compiled ARM64 binary"
                    else
                        log_warn "Binary architecture mismatch, will compile from source"
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
    if [ ! -d "$BACKUP_DIR" ]; then
        log_error "No backup available for rollback"
        return 1
    fi

    log "Rolling back to previous version..."

    # Only stop C++ service - don't stop web service during rollback
    run_systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true

    # Restore backup
    rsync -a --delete --no-group --no-owner \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'config/*' \
        "$BACKUP_DIR/" "$INSTALL_DIR/" || true

    # Explicitly restore VERSION file from backup
    if [ -f "$BACKUP_DIR/VERSION" ]; then
        cp "$BACKUP_DIR/VERSION" "$INSTALL_DIR/VERSION"
        log "VERSION restored from backup"
    fi

    # Restart C++ service (web service will be restarted at end of main function)
    run_systemctl daemon-reload
    run_systemctl start magic-dingus-box-cpp.service 2>/dev/null || true

    local restored_version
    restored_version=$(get_current_version)
    log "Rolled back to version $restored_version"
}

# User-initiated rollback
rollback() {
    if [ ! -d "$BACKUP_DIR" ]; then
        json_response "false" "No backup available for rollback"
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

    # Restore backup (preserve user data)
    log "Restoring from backup..."
    local rsync_exit=0
    rsync -av --delete --no-group --no-owner \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'config/*' \
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
