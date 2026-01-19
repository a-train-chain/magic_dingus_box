#!/bin/bash
#
# Manual OTA Reliability Test Script
#
# Run this on the Pi while testing from the web UI.
# It will watch for specific log patterns to verify the improvements.
#
# Usage: ./manual_ota_test.sh [test_name]
#   test_name: retry | version | all (default: all)
#

set -u

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m'

INSTALL_DIR="/opt/magic_dingus_box"
BACKUP_DIR="$HOME/.magic_dingus_box_backup"

header() {
    echo ""
    echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
    echo ""
}

info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

step() {
    echo -e "${YELLOW}[STEP]${NC} $1"
}

check() {
    echo -e "${GREEN}[CHECK]${NC} $1"
}

# Show current state
show_state() {
    header "Current System State"

    echo "VERSION file:"
    if [ -f "$INSTALL_DIR/VERSION" ]; then
        echo "  $INSTALL_DIR/VERSION: $(cat $INSTALL_DIR/VERSION)"
    else
        echo "  $INSTALL_DIR/VERSION: (not found)"
    fi

    echo ""
    echo "Backup VERSION:"
    if [ -f "$BACKUP_DIR/VERSION" ]; then
        echo "  $BACKUP_DIR/VERSION: $(cat $BACKUP_DIR/VERSION)"
    else
        echo "  $BACKUP_DIR/VERSION: (no backup exists)"
    fi

    echo ""
    echo "Services:"
    echo "  magic-dingus-web: $(systemctl is-active magic-dingus-web.service 2>/dev/null || echo 'unknown')"
    echo "  magic-dingus-box-cpp: $(systemctl is-active magic-dingus-box-cpp.service 2>/dev/null || echo 'unknown')"
}

# Test 1: Watch for retry download patterns
test_retry_download() {
    header "TEST 1: Network Retry Logic"

    info "This test verifies that downloads use exponential backoff retry."
    echo ""
    step "1. Open web manager in browser"
    step "2. Connect to this device"
    step "3. Check for updates"
    step "4. Start the update"
    echo ""
    info "Watch for these log patterns:"
    echo "  - 'Download attempt 1/3 for update package'"
    echo "  - 'Download attempt 1/3 for pre-compiled binary'"
    echo ""
    info "If network fails, you'll see:"
    echo "  - 'Download failed (curl exit: X), retrying in Xs...'"
    echo "  - 'Download attempt 2/3...'"
    echo ""

    read -p "Press ENTER to start watching logs (Ctrl+C to stop)..."
    echo ""
    check "Watching for retry patterns..."

    sudo journalctl -u magic-dingus-web -f --no-pager 2>&1 | grep --line-buffered -E "(Download attempt|Retry in|curl exit|retry_download|pre-compiled binary)"
}

# Test 2: Watch for VERSION atomicity
test_version_atomicity() {
    header "TEST 2: VERSION Atomicity"

    show_state

    info "This test verifies VERSION is only written after successful service start."
    echo ""
    step "1. Start an update from the web UI"
    step "2. Watch for these patterns in order:"
    echo "     a. 'Restarting services...'"
    echo "     b. 'Service start command...' or 'is-active' check"
    echo "     c. 'VERSION updated to X.X.X' (should come LAST)"
    echo ""
    info "If service fails to start, you should see:"
    echo "  - 'Service failed to start, rolling back...'"
    echo "  - 'VERSION restored from backup'"
    echo ""

    read -p "Press ENTER to start watching logs (Ctrl+C to stop)..."
    echo ""
    check "Watching for VERSION patterns..."

    sudo journalctl -u magic-dingus-web -f --no-pager 2>&1 | grep --line-buffered -E "(VERSION|Restarting services|Service start|is-active|rolling back|restored)"
}

# Test 3: Verify rollback restores VERSION
test_rollback_version() {
    header "TEST 3: Rollback VERSION Restore"

    show_state

    info "This test verifies rollback properly restores the VERSION file."
    echo ""
    step "1. After a successful update, click 'Rollback' in web UI"
    step "2. Watch for 'VERSION restored from backup'"
    step "3. Verify VERSION file matches backup after rollback"
    echo ""

    read -p "Press ENTER to start watching logs (Ctrl+C to stop)..."
    echo ""
    check "Watching for rollback patterns..."

    sudo journalctl -u magic-dingus-web -f --no-pager 2>&1 | grep --line-buffered -E "(VERSION|Rollback|rollback|restored|backup)"
}

# Test 4: Watch all update activity
test_full_update() {
    header "FULL UPDATE TEST"

    show_state

    info "This will show ALL update-related log activity."
    echo ""
    step "1. Open web manager and start an update"
    step "2. Watch the full process"
    echo ""
    info "Key patterns to look for:"
    echo "  - Download attempts with retry logic"
    echo "  - 'Checking for pre-compiled binary'"
    echo "  - Build progress (if compiling from source)"
    echo "  - Service restart and verification"
    echo "  - VERSION update (should be last before 'complete')"
    echo ""

    read -p "Press ENTER to start watching logs (Ctrl+C to stop)..."
    echo ""
    check "Watching full update process..."

    sudo journalctl -u magic-dingus-web -f --no-pager 2>&1 | grep --line-buffered -E "(UPDATE|Download|VERSION|binary|Building|Compiling|Restarting|Service|complete|error|rollback)"
}

# Interactive verification after update
verify_after_update() {
    header "Post-Update Verification"

    show_state

    echo ""
    check "Checking update.sh has retry_download function:"
    if grep -q 'retry_download()' "$INSTALL_DIR/magic_dingus_box_cpp/scripts/update.sh"; then
        echo -e "  ${GREEN}✓ retry_download() found${NC}"
    else
        echo -e "  ${RED}✗ retry_download() NOT found${NC}"
    fi

    check "Checking VERSION write is after service start:"
    version_line=$(grep -n 'echo.*target_version.*VERSION' "$INSTALL_DIR/magic_dingus_box_cpp/scripts/update.sh" | grep -v '#' | grep -v 'NOTE' | head -1 | cut -d: -f1)
    service_line=$(grep -n 'run_systemctl start magic-dingus-box-cpp' "$INSTALL_DIR/magic_dingus_box_cpp/scripts/update.sh" | head -1 | cut -d: -f1)
    if [ -n "$version_line" ] && [ -n "$service_line" ] && [ "$version_line" -gt "$service_line" ]; then
        echo -e "  ${GREEN}✓ VERSION write (line $version_line) is after service start (line $service_line)${NC}"
    else
        echo -e "  ${RED}✗ VERSION write order incorrect${NC}"
    fi

    check "Checking rollback restores VERSION:"
    count=$(grep -c 'cp.*BACKUP_DIR/VERSION.*INSTALL_DIR/VERSION' "$INSTALL_DIR/magic_dingus_box_cpp/scripts/update.sh")
    if [ "$count" -ge 2 ]; then
        echo -e "  ${GREEN}✓ VERSION restore found in $count rollback functions${NC}"
    else
        echo -e "  ${RED}✗ VERSION restore missing (found $count)${NC}"
    fi

    check "Checking manager.js has polling timeout:"
    if grep -q 'MAX_POLL_TIME_MS' "$INSTALL_DIR/magic_dingus_box/web/static/manager.js"; then
        echo -e "  ${GREEN}✓ MAX_POLL_TIME_MS found${NC}"
    else
        echo -e "  ${RED}✗ MAX_POLL_TIME_MS NOT found${NC}"
    fi

    check "Checking manager.js has improved verification:"
    if grep -q 'backoffDelays' "$INSTALL_DIR/magic_dingus_box/web/static/manager.js"; then
        echo -e "  ${GREEN}✓ Exponential backoff verification found${NC}"
    else
        echo -e "  ${RED}✗ Exponential backoff verification NOT found${NC}"
    fi
}

# Main menu
main_menu() {
    header "OTA Reliability Manual Test Suite"

    echo "Available tests:"
    echo ""
    echo "  1) retry     - Watch for download retry patterns"
    echo "  2) version   - Watch for VERSION atomicity patterns"
    echo "  3) rollback  - Watch for rollback VERSION restore"
    echo "  4) full      - Watch full update process"
    echo "  5) verify    - Verify code changes are present"
    echo "  6) state     - Show current system state"
    echo "  q) quit"
    echo ""

    read -p "Select test [1-6, q]: " choice

    case "$choice" in
        1|retry)    test_retry_download ;;
        2|version)  test_version_atomicity ;;
        3|rollback) test_rollback_version ;;
        4|full)     test_full_update ;;
        5|verify)   verify_after_update ;;
        6|state)    show_state ;;
        q|Q|quit)   exit 0 ;;
        *)          warn "Unknown option: $choice" ;;
    esac

    echo ""
    read -p "Press ENTER to return to menu..."
    main_menu
}

# Handle command line argument
case "${1:-menu}" in
    retry)    test_retry_download ;;
    version)  test_version_atomicity ;;
    rollback) test_rollback_version ;;
    full)     test_full_update ;;
    verify)   verify_after_update ;;
    state)    show_state ;;
    all|menu) main_menu ;;
    *)
        echo "Usage: $0 [retry|version|rollback|full|verify|state|menu]"
        exit 1
        ;;
esac
