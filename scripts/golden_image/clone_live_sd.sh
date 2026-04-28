#!/usr/bin/env bash
#
# Magic Dingus Box - Live SD card clone over SSH
#
# Runs on the OPERATOR'S MAC. Clones a running Pi's SD card into a
# .img.gz file on this machine without anyone ever needing to remove
# the SD card from the Pi. Uses ssh ControlMaster so prepare/dd/restore
# all run inside one persistent SSH session, which means they survive
# even if the source Pi's identity files get manipulated mid-flight.
#
# Workflow (see scripts/golden_image/CLONING.md for details):
#   1. Open ssh ControlMaster to source Pi
#   2. Run prepare_for_cloning.sh on Pi (stops services, snapshots
#      identity, re-enables first-boot service)
#   3. dd /dev/mmcblk0 over SSH, gzip-compressed, save to local .img.gz
#   4. Run restore_after_cloning.sh on Pi (puts services back, restores
#      identity, disables first-boot service)
#   5. Close ControlMaster
#
# Trap handler ensures Step 4 fires even if the user Ctrl-C's during
# the dd or the network drops mid-stream — the Pi never gets stuck in
# the "in-progress" state.
#
# Usage:
#   ./clone_live_sd.sh                                 # uses defaults
#   ./clone_live_sd.sh --pi magic@magicpi-abcd.local
#   ./clone_live_sd.sh --pi magic@10.55.0.1            # over USB-Gadget (much faster than wifi)
#   ./clone_live_sd.sh --output ~/Desktop/golden.img.gz
#   ./clone_live_sd.sh --dry-run                       # walk-through, no dd
#   ./clone_live_sd.sh --no-compress                   # skip gzip (faster, larger)
#   ./clone_live_sd.sh --device /dev/mmcblk0           # if non-default
#   ./clone_live_sd.sh --yes                           # skip the "Continue?" prompt
#

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
PI_HOST="${PI_HOST:-magic@magicpi.local}"
PI_DEVICE="${PI_DEVICE:-/dev/mmcblk0}"
OUTPUT_PATH="${HOME}/golden_image_$(date +%Y-%m-%d).img.gz"
DRY_RUN=0
COMPRESS=1
SKIP_CONFIRM=0

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' CYAN='\033[0;36m'
    BOLD='\033[1m' DIM='\033[2m' NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' CYAN='' BOLD='' DIM='' NC=''
fi

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pi)            PI_HOST="$2"; shift 2 ;;
        --output|-o)     OUTPUT_PATH="$2"; shift 2 ;;
        --device)        PI_DEVICE="$2"; shift 2 ;;
        --dry-run)       DRY_RUN=1; shift ;;
        --no-compress)   COMPRESS=0; shift ;;
        --yes|-y)        SKIP_CONFIRM=1; shift ;;
        -h|--help)
            sed -n 's/^# //;s/^#$//;1,/^$/p' "$0" | head -50
            exit 0
            ;;
        *)
            echo "Unknown arg: $1" >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# ControlMaster setup — single persistent SSH session for prepare+dd+restore
# ---------------------------------------------------------------------------
SSH_CONTROL_PATH="/tmp/magic-clone-ssh-${USER}-$$"
SSH_OPTS=(-o ControlMaster=auto
          -o "ControlPath=${SSH_CONTROL_PATH}"
          -o ControlPersist=300
          -o ServerAliveInterval=15
          -o ServerAliveCountMax=4)

# ---------------------------------------------------------------------------
# Trap handler: always run restore + close master, even on Ctrl-C / errors
# ---------------------------------------------------------------------------
RESTORE_DONE=0
cleanup() {
    local rc=$?
    if [[ $RESTORE_DONE -eq 0 ]]; then
        echo
        echo -e "${YELLOW}${BOLD}Running restore on source Pi (cleanup path)...${NC}"
        # Best-effort restore. If this fails too, operator must SSH in
        # manually and run restore_after_cloning.sh. But that scenario
        # only triggers when SSH is genuinely broken.
        ssh "${SSH_OPTS[@]}" "$PI_HOST" \
            "sudo /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh" \
            2>&1 | sed 's/^/    /' || \
            echo -e "${RED}    Restore failed. SSH manually and run restore_after_cloning.sh.${NC}"
        RESTORE_DONE=1
    fi
    # Close the SSH control master
    ssh "${SSH_OPTS[@]}" -O exit "$PI_HOST" 2>/dev/null || true
    exit $rc
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Banner + plan
# ---------------------------------------------------------------------------
echo
echo -e "${BOLD}${CYAN}════════════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}${CYAN}  Magic Dingus Box - Live SD Card Clone${NC}"
echo -e "${BOLD}${CYAN}════════════════════════════════════════════════════════════════${NC}"
echo
echo -e "  Source Pi:  ${BOLD}${PI_HOST}${NC}"
echo -e "  SD device:  ${BOLD}${PI_DEVICE}${NC}"
echo -e "  Output:     ${BOLD}${OUTPUT_PATH}${NC}"
echo -e "  Compress:   $( [[ $COMPRESS -eq 1 ]] && echo "${GREEN}gzip -1 (fast, ~50% size)${NC}" || echo "${YELLOW}none${NC}" )"
[[ $DRY_RUN -eq 1 ]] && echo -e "  Mode:       ${YELLOW}${BOLD}DRY RUN (no dd, no gzip)${NC}"
echo

# ---------------------------------------------------------------------------
# Preflight 1: SSH connectivity + sudo nopasswd check
# ---------------------------------------------------------------------------
echo -e "${CYAN}[1/6] Verifying SSH connectivity...${NC}"

if ! ssh "${SSH_OPTS[@]}" -o ConnectTimeout=5 "$PI_HOST" "echo connected" >/dev/null 2>&1; then
    echo -e "${RED}    Cannot SSH to ${PI_HOST}.${NC}"
    echo -e "${RED}    Check the host is online and you have key-based auth set up.${NC}"
    exit 1
fi
echo -e "  ${GREEN}OK${NC} ssh ${PI_HOST}"

# Sudo NOPASSWD check (required because we'll need root for dd, systemctl, etc.
# and the Mac script can't interactively type a sudo password during dd).
if ! ssh "${SSH_OPTS[@]}" "$PI_HOST" "sudo -n true" 2>/dev/null; then
    echo -e "${RED}    sudo on the Pi requires a password.${NC}"
    echo -e "${RED}    Add NOPASSWD for the magic user, or run sudo manually first to cache${NC}"
    echo -e "${RED}    credentials, then re-run this script within ~5 minutes.${NC}"
    exit 1
fi
echo -e "  ${GREEN}OK${NC} sudo NOPASSWD"

# ---------------------------------------------------------------------------
# Preflight 2: SD device size + Mac-side disk space
# ---------------------------------------------------------------------------
echo -e "${CYAN}[2/6] Checking SD card size + local disk space...${NC}"

SD_SIZE_BYTES=$(ssh "${SSH_OPTS[@]}" "$PI_HOST" "sudo blockdev --getsize64 ${PI_DEVICE}")
SD_SIZE_GB=$(( SD_SIZE_BYTES / 1024 / 1024 / 1024 ))
echo -e "  ${GREEN}OK${NC} SD card: ${BOLD}${SD_SIZE_GB} GB${NC} (${SD_SIZE_BYTES} bytes)"

# Mac-side: target directory must have enough free space.
# Estimated max output size: full SD if --no-compress, ~50% if compressed.
EST_OUTPUT_GB=$( [[ $COMPRESS -eq 1 ]] && echo $(( SD_SIZE_GB / 2 + 1 )) || echo $(( SD_SIZE_GB + 1 )) )
OUTPUT_DIR=$(dirname "$OUTPUT_PATH")
mkdir -p "$OUTPUT_DIR"
FREE_KB=$(df -k "$OUTPUT_DIR" | awk 'NR==2 {print $4}')
FREE_GB=$(( FREE_KB / 1024 / 1024 ))
if [[ $FREE_GB -lt $EST_OUTPUT_GB ]]; then
    echo -e "${RED}    Free space on $OUTPUT_DIR: ${FREE_GB} GB < estimated ${EST_OUTPUT_GB} GB needed${NC}"
    exit 1
fi
echo -e "  ${GREEN}OK${NC} ${FREE_GB} GB free on $OUTPUT_DIR (need ~${EST_OUTPUT_GB} GB)"

# ---------------------------------------------------------------------------
# Preflight 3: required local tools
# ---------------------------------------------------------------------------
echo -e "${CYAN}[3/6] Checking local tools...${NC}"

for tool in ssh dd gzip; do
    if ! command -v "$tool" &>/dev/null; then
        echo -e "${RED}    Missing required tool: $tool${NC}"
        exit 1
    fi
done
echo -e "  ${GREEN}OK${NC} ssh, dd, gzip"

if command -v pv &>/dev/null; then
    HAS_PV=1
    echo -e "  ${GREEN}OK${NC} pv (will show progress bar)"
else
    HAS_PV=0
    echo -e "  ${DIM}pv not found — progress will be reported by dd's status=progress${NC}"
fi

# ---------------------------------------------------------------------------
# Confirmation
# ---------------------------------------------------------------------------
echo
echo -e "${YELLOW}${BOLD}This will:${NC}"
echo -e "  ${YELLOW}1.${NC} Stop kiosk + Content Manager + Docker stack on the source Pi (~30s)"
echo -e "  ${YELLOW}2.${NC} Stream ${SD_SIZE_GB} GB SD card over SSH to your Mac (~30-90 min)"
echo -e "  ${YELLOW}3.${NC} Restart services on source Pi (~30s)"
echo
echo -e "${DIM}Source Pi loses NO permanent data — services come back as if nothing${NC}"
echo -e "${DIM}happened. Library, downloads, settings, saves all intact.${NC}"
echo

if [[ $DRY_RUN -eq 0 && $SKIP_CONFIRM -eq 0 ]]; then
    read -r -p "$(echo -e "${BOLD}Continue with live clone? [y/N] ${NC}")" confirm
    if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
        echo "Aborted."
        # Trap will run restore (no-op since we never ran prepare)
        exit 1
    fi
elif [[ $DRY_RUN -eq 1 ]]; then
    echo -e "${YELLOW}DRY RUN: skipping confirmation${NC}"
else
    echo -e "${YELLOW}--yes: skipping confirmation${NC}"
fi

# ---------------------------------------------------------------------------
# Step 4: Run prepare_for_cloning.sh on Pi
# ---------------------------------------------------------------------------
echo
echo -e "${CYAN}[4/6] Running prepare_for_cloning.sh on source Pi...${NC}"

ssh "${SSH_OPTS[@]}" "$PI_HOST" \
    "sudo /opt/magic_dingus_box/scripts/golden_image/prepare_for_cloning.sh" \
    2>&1 | sed 's/^/    /'

if [[ $DRY_RUN -eq 1 ]]; then
    echo
    echo -e "${YELLOW}${BOLD}DRY RUN: skipping dd. Going straight to restore...${NC}"
else
    # ---------------------------------------------------------------------
    # Step 5: dd over SSH (the slow part)
    # ---------------------------------------------------------------------
    echo
    echo -e "${CYAN}[5/6] Streaming SD card to ${OUTPUT_PATH}...${NC}"
    echo -e "${DIM}    This is the long step. Monitor with progress bar below.${NC}"
    echo

    START_TS=$(date +%s)

    # The pipeline: dd on Pi → ssh stdout → optional pv (progress) → optional gzip → file
    if [[ $HAS_PV -eq 1 ]]; then
        if [[ $COMPRESS -eq 1 ]]; then
            ssh "${SSH_OPTS[@]}" "$PI_HOST" \
                "sudo dd if=${PI_DEVICE} bs=4M iflag=direct status=none" \
                | pv -s "$SD_SIZE_BYTES" -ptea \
                | gzip -1 \
                > "$OUTPUT_PATH"
        else
            ssh "${SSH_OPTS[@]}" "$PI_HOST" \
                "sudo dd if=${PI_DEVICE} bs=4M iflag=direct status=none" \
                | pv -s "$SD_SIZE_BYTES" -ptea \
                > "$OUTPUT_PATH"
        fi
    else
        if [[ $COMPRESS -eq 1 ]]; then
            ssh "${SSH_OPTS[@]}" "$PI_HOST" \
                "sudo dd if=${PI_DEVICE} bs=4M iflag=direct status=progress" \
                | gzip -1 \
                > "$OUTPUT_PATH"
        else
            ssh "${SSH_OPTS[@]}" "$PI_HOST" \
                "sudo dd if=${PI_DEVICE} bs=4M iflag=direct status=progress" \
                > "$OUTPUT_PATH"
        fi
    fi

    END_TS=$(date +%s)
    ELAPSED=$(( END_TS - START_TS ))
    OUTPUT_SIZE=$(stat -f %z "$OUTPUT_PATH" 2>/dev/null || stat -c %s "$OUTPUT_PATH")
    OUTPUT_GB=$(echo "scale=2; $OUTPUT_SIZE / 1024 / 1024 / 1024" | bc)

    echo
    echo -e "  ${GREEN}OK${NC} Wrote ${OUTPUT_GB} GB in $((ELAPSED / 60)) min $((ELAPSED % 60)) sec"
fi

# ---------------------------------------------------------------------------
# Step 6: Run restore_after_cloning.sh on Pi
# ---------------------------------------------------------------------------
echo
echo -e "${CYAN}[6/6] Restoring source Pi...${NC}"

ssh "${SSH_OPTS[@]}" "$PI_HOST" \
    "sudo /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh" \
    2>&1 | sed 's/^/    /'

RESTORE_DONE=1

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo -e "${BOLD}${GREEN}════════════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}${GREEN}  Clone complete${NC}"
echo -e "${BOLD}${GREEN}════════════════════════════════════════════════════════════════${NC}"
echo
if [[ $DRY_RUN -eq 0 ]]; then
    echo -e "  Image:  ${BOLD}${OUTPUT_PATH}${NC}"
    echo -e "  Size:   ${BOLD}$(du -h "$OUTPUT_PATH" | awk '{print $1}')${NC}"
    echo
    echo -e "${BOLD}Next steps:${NC}"
    echo -e "  1. Insert a fresh SD card into your Mac"
    echo -e "  2. Open ${BOLD}Raspberry Pi Imager${NC}"
    echo -e "  3. Operating System → ${BOLD}Use custom${NC} → select ${OUTPUT_PATH}"
    echo -e "  4. (Optional) Click gear icon → set WiFi SSID/password"
    echo -e "  5. Storage → choose your SD card → ${BOLD}Write${NC}"
    echo -e "  6. Insert SD into new Pi, power on. ~90 sec for first_boot.sh"
    echo -e "     to run (regenerate SSH host keys + device identity, expand FS,"
    echo -e "     wipe Media Browser per-Pi state)."
    echo -e "  7. Open Content Manager from your laptop, set up WireGuard config"
    echo -e "     to activate Media Browser on the new Pi."
fi
echo
