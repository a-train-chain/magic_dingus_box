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
# After the dd + restore, the finished .img.gz is scanned for the source
# box's live credentials (scan_image_for_secrets.sh). A LEAK verdict renames
# the artifact to *.LEAKED.img.gz and exits 1 — that image must not ship.
# --skip-leak-scan disables the gate (throwaway images only, NEVER for one
# that leaves the building).
#
# Usage:
#   ./clone_live_sd.sh --pi magic@magicpi-abcd.local   # --pi is REQUIRED
#   ./clone_live_sd.sh --pi magic@10.55.0.1            # over USB-Gadget (much faster than wifi)
#   ./clone_live_sd.sh --pi ... --output /Volumes/SSD/golden.img.gz
#   ./clone_live_sd.sh --pi ... --dry-run              # walk-through, no dd
#   ./clone_live_sd.sh --pi ... --no-compress          # skip gzip (faster, larger)
#   ./clone_live_sd.sh --pi ... --device /dev/mmcblk0  # if non-default
#   ./clone_live_sd.sh --pi ... --yes                  # skip the "Continue?" prompt
#   ./clone_live_sd.sh --pi ... --skip-leak-scan       # UNCHECKED image — do not ship
#

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
# --pi is REQUIRED, never defaulted, and the PI_HOST environment variable is
# deliberately IGNORED: deploy_cpp.sh documents PI_HOST as its own target
# variable with a different default box, so an operator who exported it for a
# deploy would silently image that box instead. Two Pis are usually reachable
# at once on this bench; sync_source_box.sh states the same rule.
PI_HOST=""
PI_DEVICE="${PI_DEVICE:-/dev/mmcblk0}"
OUTPUT_PATH="${HOME}/golden_image_$(date +%Y-%m-%d).img.gz"
DRY_RUN=0
COMPRESS=1
SKIP_CONFIRM=0
SKIP_LEAK_SCAN=0

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
        --skip-leak-scan) SKIP_LEAK_SCAN=1; shift ;;
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

if [[ -z "$PI_HOST" ]]; then
    echo "ERROR: --pi is required (never defaulted: the wrong box gets imaged)." >&2
    echo "       Example: $0 --pi magic@magicpi5.local --output /Volumes/SSD/golden.img.gz" >&2
    exit 2
fi

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
# Set the moment prepare_for_cloning.sh is INVOKED. Before that, the box has
# not been touched, so the cleanup path must not attempt a restore — and
# above all must not print the red "SOURCE PI IS STILL STRIPPED" emergency
# banner about a box that was never modified. A preflight failure (host
# unreachable, no disk space) used to do exactly that, and a gate that cries
# wolf trains the operator to ignore the one time it is real.
PREPARE_STARTED=0
# Set after the dd pipeline completes. If cleanup runs without it, whatever
# partial output exists is renamed *.partial so a truncated artifact can
# never sit on disk under a ship-looking name.
DD_DONE=0
# Filled in at preflight from the box's own hostname. The restore has to be
# reachable by MORE than the address the clone was started on: a clone begun
# over the USB gadget (10.55.0.1) dropped when the USB link went away, and
# the single-address restore then failed with "Operation timed out" even
# though the box was up and answering on Wi-Fi the whole time. It was left
# stripped, with 258 secrets parked in /dev/shm that a reboot would have
# destroyed permanently.
RESTORE_FALLBACK=""

restore_on() {
    # Deliberately does NOT reuse SSH_OPTS: the ControlMaster socket is
    # itself dead in exactly the situation this function exists for.
    ssh -o ConnectTimeout=10 -o BatchMode=yes -o StrictHostKeyChecking=no "$1" \
        "sudo /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh" 2>&1 \
        | sed 's/^/    /'
    return "${PIPESTATUS[0]}"
}

cleanup() {
    local rc=$?
    # Quarantine a truncated artifact before anything else: a dd that died
    # mid-stream leaves a file that LOOKS like a golden image and is not.
    if [[ $DD_DONE -eq 0 && ${DRY_RUN:-0} -eq 0 && -f "${OUTPUT_PATH:-}" ]]; then
        mv "$OUTPUT_PATH" "${OUTPUT_PATH}.partial" 2>/dev/null || true
        echo -e "${YELLOW}    Incomplete output renamed to ${OUTPUT_PATH}.partial — delete it.${NC}"
    fi
    if [[ $PREPARE_STARTED -eq 0 ]]; then
        # The box was never touched; there is nothing to restore.
        ssh "${SSH_OPTS[@]}" -O exit "$PI_HOST" 2>/dev/null || true
        exit $rc
    fi
    if [[ $RESTORE_DONE -eq 0 ]]; then
        echo
        echo -e "${YELLOW}${BOLD}Running restore on source Pi (cleanup path)...${NC}"
        local host ok=0
        for host in "$PI_HOST" $RESTORE_FALLBACK; do
            [[ -z "$host" ]] && continue
            echo -e "    ${DIM}trying ${host}...${NC}"
            if restore_on "$host"; then ok=1; break; fi
            echo -e "    ${YELLOW}restore via ${host} failed${NC}"
        done
        if [[ $ok -eq 0 ]]; then
            echo
            echo -e "${RED}${BOLD}    RESTORE DID NOT RUN — THE SOURCE PI IS STILL STRIPPED.${NC}"
            echo -e "${RED}    Its secrets are stashed in /dev/shm, which is RAM: rebooting or${NC}"
            echo -e "${RED}    powering off the Pi DESTROYS THEM PERMANENTLY.${NC}"
            echo -e "${RED}    Do not reboot it. Reach it by any route and run:${NC}"
            echo -e "${RED}      sudo /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh${NC}"
        fi
        RESTORE_DONE=1
    fi
    # Close the SSH control master
    ssh "${SSH_OPTS[@]}" -O exit "$PI_HOST" 2>/dev/null || true
    exit $rc
}
# HUP is in the list on purpose: this script runs in the operator's terminal,
# and closing that terminal (or losing the connection driving it) delivers
# SIGHUP — which bash does NOT turn into an EXIT-trap run unless HUP is
# trapped explicitly. The Pi-side zerofill learned this from a real dropped
# link; the orchestrator gets the same protection so a closed laptop lid
# mid-dd still restores the source box.
trap cleanup EXIT INT TERM HUP

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

# Record a second way back in, for the cleanup path.
PI_USER="${PI_HOST%%@*}"
PI_SHORTNAME=$(ssh "${SSH_OPTS[@]}" "$PI_HOST" "hostname" 2>/dev/null | tr -d '[:space:]')
if [[ -n "$PI_SHORTNAME" && "$PI_HOST" != *"${PI_SHORTNAME}.local"* ]]; then
    RESTORE_FALLBACK="${PI_USER}@${PI_SHORTNAME}.local"
    echo -e "  ${GREEN}OK${NC} restore fallback address: ${RESTORE_FALLBACK}"
fi

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
# The directory must ALREADY exist — no mkdir -p. With the golden images kept
# on an external drive, `mkdir -p /Volumes/SSD/...` on a Mac where that drive
# is not mounted silently creates the path on the BOOT disk; df then measures
# the boot disk, the preflight passes, and 30+ GB lands on the internal SSD.
if [[ ! -d "$OUTPUT_DIR" ]]; then
    echo -e "${RED}    Output directory does not exist: ${OUTPUT_DIR}${NC}"
    echo -e "${RED}    If it lives on an external drive, is the drive mounted?${NC}"
    exit 1
fi
# Never silently overwrite an existing image: the file already there may be
# the last known-good golden master.
if [[ -e "$OUTPUT_PATH" && $DRY_RUN -eq 0 ]]; then
    echo -e "${RED}    Output file already exists: ${OUTPUT_PATH}${NC}"
    echo -e "${RED}    Delete or rename it first, or pick a different --output name.${NC}"
    exit 1
fi
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

# bc is used for the size summary; python3 runs the leak scanner — checked
# HERE so a missing interpreter surfaces in one second at preflight, not
# after a 30-90 minute dd when the ship gate tries to run.
REQUIRED_TOOLS=(ssh gzip bc)
[[ $SKIP_LEAK_SCAN -eq 0 ]] && REQUIRED_TOOLS+=(python3)
for tool in "${REQUIRED_TOOLS[@]}"; do
    if ! command -v "$tool" &>/dev/null; then
        echo -e "${RED}    Missing required tool: $tool${NC}"
        exit 1
    fi
done
echo -e "  ${GREEN}OK${NC} ${REQUIRED_TOOLS[*]}"

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

PREPARE_STARTED=1
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

    DD_DONE=1
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
# Step 6.5: Scan the FINISHED ARTIFACT for the operator's credentials
# ---------------------------------------------------------------------------
# The ship gate. Everything before this validated the live filesystem; this
# validates the bytes that actually go onto every customer's SD card. Those
# are not the same thing -- a deleted file is absent from the filesystem and
# fully present in the image, which is exactly how the v1.9.3 leak passed its
# own audit.
#
# Delegated to scan_image_for_secrets.sh so there is ONE scanner to keep
# correct, and so the same check can be re-run by hand on any older image.
# It runs AFTER the restore, when the box's secrets are back on disk to be
# harvested as needles.
if [[ $DRY_RUN -eq 0 && $SKIP_LEAK_SCAN -eq 0 ]]; then
    echo
    echo -e "${CYAN}[6.5/6] Scanning the image for credential remnants (~5-10 min)...${NC}"

    SCANNER="$(dirname "${BASH_SOURCE[0]}")/scan_image_for_secrets.sh"
    if [[ ! -x "$SCANNER" ]]; then
        echo -e "${RED}  Scanner not found or not executable: ${SCANNER}${NC}"
        echo -e "${RED}  The image has NOT been checked. Do not ship it until it is.${NC}"
        exit 1
    fi

    SCAN_HOST="$PI_HOST"
    if ! ssh -o ConnectTimeout=8 -o BatchMode=yes "$SCAN_HOST" true 2>/dev/null; then
        SCAN_HOST="$RESTORE_FALLBACK"
    fi
    if [[ -z "$SCAN_HOST" ]]; then
        echo -e "${RED}  Cannot reach the Pi to harvest scan needles.${NC}"
        echo -e "${RED}  The image has NOT been checked. Re-run by hand:${NC}"
        echo -e "${RED}    ${SCANNER} --image ${OUTPUT_PATH} --pi <host>${NC}"
        exit 1
    fi
    # The needles must come from the box that was IMAGED. The fallback is an
    # mDNS name, and an un-first-booted clone that kept the source hostname is
    # exactly the collision scenario this project has already lived through —
    # harvesting from the wrong box would produce a CLEAN verdict about the
    # wrong secrets, the one direction this gate must never fail in.
    if [[ -n "$PI_SHORTNAME" ]]; then
        SCAN_ID=$(ssh -o ConnectTimeout=8 -o BatchMode=yes "$SCAN_HOST" "hostname" 2>/dev/null | tr -d '[:space:]')
        if [[ "$SCAN_ID" != "$PI_SHORTNAME" ]]; then
            echo -e "${RED}  Scan host answered as '${SCAN_ID}', expected '${PI_SHORTNAME}' —${NC}"
            echo -e "${RED}  refusing to harvest needles from a different box.${NC}"
            echo -e "${RED}  The image has NOT been checked. Re-run by hand once the source${NC}"
            echo -e "${RED}  box is reachable:${NC}"
            echo -e "${RED}    ${SCANNER} --image ${OUTPUT_PATH} --pi <host>${NC}"
            exit 1
        fi
    fi

    set +e
    "$SCANNER" --image "$OUTPUT_PATH" --pi "$SCAN_HOST" --expect-bytes "$SD_SIZE_BYTES"
    SCAN_RC=$?
    set -e

    if [[ "$SCAN_RC" -ne 0 ]]; then
        echo
        echo -e "${RED}${BOLD}════════════════════════════════════════════════════════════════${NC}"
        if [[ "$SCAN_RC" -eq 1 ]]; then
            echo -e "${RED}${BOLD}  DO NOT SHIP THIS IMAGE${NC}"
            echo -e "${RED}${BOLD}════════════════════════════════════════════════════════════════${NC}"
            echo -e "  The artifact contains at least one of this Pi's live secrets."
            echo -e "  Every card flashed from it would carry that credential."
            echo
            echo -e "  Most likely cause: free-space zeroing did not cover the whole"
            echo -e "  filesystem. Check the ${BOLD}[4c/5]${NC} lines above for a WARNING about"
            echo -e "  the ext4 root reserve, and confirm ${BOLD}tune2fs${NC} exists on the Pi."
            # Quarantine: a known-contaminated artifact must not sit on disk
            # under a normal, ship-looking name for a tired operator (or a
            # later session) to flash by reflex.
            if mv "$OUTPUT_PATH" "${OUTPUT_PATH}.LEAKED" 2>/dev/null; then
                echo
                echo -e "  Artifact renamed to ${BOLD}${OUTPUT_PATH}.LEAKED${NC} — delete it."
            fi
        else
            echo -e "${YELLOW}${BOLD}  IMAGE NOT CHECKED${NC}"
            echo -e "${YELLOW}${BOLD}════════════════════════════════════════════════════════════════${NC}"
            echo -e "  The scan did not complete (exit ${SCAN_RC}). This is NOT a pass and"
            echo -e "  NOT a leak — the image simply has not been verified."
        fi
        echo
        echo -e "  The source Pi has been fully restored — it is safe and unchanged."
        echo -e "  Remove the artifact (see above for its current name) and re-run"
        echo -e "  the clone."
        echo
        exit 1
    fi
fi

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
    echo -e "  4. ${BOLD}Do NOT apply OS customisation${NC} (the gear icon). If Imager"
    echo -e "     offers to apply saved settings, choose ${BOLD}NO${NC}."
    echo -e "     Setting Wi-Fi there writes YOUR SSID and password onto the card"
    echo -e "     — the exact credentials prepare_for_cloning.sh just scrubbed out"
    echo -e "     of this image. The customer joins Wi-Fi from the kiosk's own"
    echo -e "     Settings screen; the card must ship with no network saved."
    echo -e "  5. Storage → choose your SD card → ${BOLD}Write${NC}"
    echo -e "  6. Insert SD into new Pi, power on. ~90 sec for first_boot.sh"
    echo -e "     to run (regenerate SSH host keys + device identity, expand FS,"
    echo -e "     wipe Media Browser per-Pi state)."
    echo -e "  7. Open Content Manager from your laptop, set up WireGuard config"
    echo -e "     to activate Media Browser on the new Pi."
fi
echo
