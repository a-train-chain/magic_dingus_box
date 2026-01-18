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
BACKUP_DIR="${HOME}/.magic_dingus_box_backup"
TEMP_DIR="/tmp/magic_update"
GITHUB_REPO="a-train-chain/magic_dingus_box"
GITHUB_API="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
VERSION_FILE="${INSTALL_DIR}/VERSION"

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

    if ! curl -L -o "$TEMP_DIR/update.tar.gz" \
        --connect-timeout 30 \
        --max-time 600 \
        --progress-bar \
        "$download_url" 2>&2; then
        json_response "false" "Download failed"
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

    # Find the actual content directory (GitHub tarballs have a top-level folder)
    local content_dir
    content_dir=$(find "$TEMP_DIR/extracted" -mindepth 1 -maxdepth 1 -type d | head -1)
    if [ -z "$content_dir" ]; then
        content_dir="$TEMP_DIR/extracted"
    fi

    json_progress "backing_up" 45 "Creating backup of current installation..."

    # Backup current installation
    log "Creating backup at $BACKUP_DIR"
    rm -rf "$BACKUP_DIR"

    # Create backup (exclude large user data to save space)
    mkdir -p "$BACKUP_DIR"
    rsync -a --delete \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/build' \
        "$INSTALL_DIR/" "$BACKUP_DIR/" 2>&2 || {
        json_response "false" "Failed to create backup"
        rm -rf "$TEMP_DIR"
        return 1
    }

    json_progress "stopping_services" 55 "Stopping services..."

    # Stop services
    log "Stopping services..."
    sudo systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
    sudo systemctl stop magic-dingus-web.service 2>/dev/null || true
    sleep 2  # Give services time to stop

    json_progress "installing" 60 "Installing new files..."

    # Install new files (preserve user data)
    log "Installing new files..."
    rsync -av --delete \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'config/*' \
        --exclude 'magic_dingus_box_cpp/build/*' \
        "$content_dir/" "$INSTALL_DIR/" 2>&2 || {
        log_error "Failed to install files, attempting rollback..."
        rollback_internal
        return 1
    }

    # Update VERSION file
    echo "$target_version" > "$INSTALL_DIR/VERSION"

    json_progress "building" 70 "Building application (this may take a few minutes)..."

    # Rebuild C++ application
    log "Building application..."
    cd "$INSTALL_DIR/magic_dingus_box_cpp"
    mkdir -p build
    cd build

    if ! cmake .. > /dev/null 2>&2; then
        log_error "CMake configuration failed, attempting rollback..."
        json_progress "error" 70 "CMake failed, rolling back..."
        rollback_internal
        return 1
    fi

    json_progress "building" 80 "Compiling..."

    if ! make -j4 2>&2; then
        log_error "Build failed, attempting rollback..."
        json_progress "error" 80 "Build failed, rolling back..."
        rollback_internal
        return 1
    fi

    json_progress "restarting_services" 90 "Restarting services..."

    # Reload systemd and restart services
    log "Restarting services..."
    sudo systemctl daemon-reload
    sudo systemctl start magic-dingus-web.service 2>/dev/null || true
    sleep 1
    sudo systemctl start magic-dingus-box-cpp.service 2>/dev/null || true

    # Cleanup temp files
    rm -rf "$TEMP_DIR"

    json_progress "complete" 100 "Update complete!"
    log "Update to version $target_version complete!"

    # Final success response
    cat << EOF
{
    "ok": true,
    "stage": "complete",
    "progress": 100,
    "message": "Update complete!",
    "new_version": "$target_version"
}
EOF
}

# Internal rollback function (used during failed updates)
rollback_internal() {
    if [ ! -d "$BACKUP_DIR" ]; then
        log_error "No backup available for rollback"
        return 1
    fi

    log "Rolling back to previous version..."

    # Stop services
    sudo systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
    sudo systemctl stop magic-dingus-web.service 2>/dev/null || true

    # Restore backup
    rsync -a --delete \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'config/*' \
        "$BACKUP_DIR/" "$INSTALL_DIR/"

    # Restart services
    sudo systemctl daemon-reload
    sudo systemctl start magic-dingus-web.service 2>/dev/null || true
    sudo systemctl start magic-dingus-box-cpp.service 2>/dev/null || true

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

    json_progress "stopping_services" 10 "Stopping services..."

    # Stop services
    log "Stopping services..."
    sudo systemctl stop magic-dingus-box-cpp.service 2>/dev/null || true
    sudo systemctl stop magic-dingus-web.service 2>/dev/null || true
    sleep 2

    json_progress "restoring" 30 "Restoring previous version..."

    # Restore backup (preserve user data)
    log "Restoring from backup..."
    rsync -av --delete \
        --exclude 'magic_dingus_box_cpp/data/media/*' \
        --exclude 'magic_dingus_box_cpp/data/roms/*' \
        --exclude 'magic_dingus_box_cpp/data/playlists/*' \
        --exclude 'magic_dingus_box_cpp/data/device_info.json' \
        --exclude 'config/*' \
        "$BACKUP_DIR/" "$INSTALL_DIR/" 2>&2 || {
        json_response "false" "Failed to restore backup"
        return 1
    }

    json_progress "restarting_services" 80 "Restarting services..."

    # Restart services
    log "Restarting services..."
    sudo systemctl daemon-reload
    sudo systemctl start magic-dingus-web.service 2>/dev/null || true
    sleep 1
    sudo systemctl start magic-dingus-box-cpp.service 2>/dev/null || true

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
