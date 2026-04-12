#!/usr/bin/env bash
set -euo pipefail

#
# Magic Dingus Box - Prepare Golden Image
#
# Cleans a working Raspberry Pi for SD card cloning. Preserves all game
# content (ROMs, cores, BIOS, playlists, thumbnails) while removing user
# media, device identity, saves, settings, and ephemeral system state.
#
# Usage: sudo ./scripts/golden_image/prepare_golden_image.sh
#
# Must be run ON the Pi as root.
#

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
INSTALL_DIR="/opt/magic_dingus_box"
CPP_DIR="${INSTALL_DIR}/magic_dingus_box_cpp"
DATA_DIR="${CPP_DIR}/data"
CONFIG_DIR="${INSTALL_DIR}/config"
MAGIC_HOME="/home/magic"
RETROARCH_DIR="${MAGIC_HOME}/.config/retroarch"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The first-boot service file lives alongside other systemd units in the repo
SYSTEMD_SRC="${SCRIPT_DIR}/../../systemd/magic-first-boot.service"

# Game playlists to KEEP (anything not on this list gets removed)
GAME_PLAYLISTS=(
    arcade.yaml
    atari_7800.yaml
    pc_engine.yaml
    ps1_collection.yaml
    retro_games.yaml
    sega_genesis.yaml
    snes.yaml
)

# Video/music playlists to REMOVE explicitly
VIDEO_PLAYLISTS=(
    danny_gatton.yaml
    wes_montgomery.yaml
    paul_franklin.yaml
)

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
if [[ "$EUID" -ne 0 ]]; then
    echo -e "${RED}Error: This script must be run as root (sudo).${NC}"
    exit 1
fi

if [[ ! -d "$INSTALL_DIR" ]]; then
    echo -e "${RED}Error: Install directory not found: ${INSTALL_DIR}${NC}"
    exit 1
fi

if [[ ! -f "${CPP_DIR}/build/magic_dingus_box_cpp" ]]; then
    echo -e "${RED}Error: Compiled binary not found at ${CPP_DIR}/build/magic_dingus_box_cpp${NC}"
    echo -e "${RED}Build the project before preparing the golden image.${NC}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Show plan
# ---------------------------------------------------------------------------
echo ""
echo -e "${BOLD}${CYAN}============================================${NC}"
echo -e "${BOLD}${CYAN}  Magic Dingus Box - Golden Image Prep${NC}"
echo -e "${BOLD}${CYAN}============================================${NC}"
echo ""

echo -e "${GREEN}${BOLD}KEEPING:${NC}"
echo -e "  ${GREEN}+${NC} Game ROMs              ${DIM}${DATA_DIR}/roms/${NC}"
echo -e "  ${GREEN}+${NC} Game playlists         ${DIM}${DATA_DIR}/playlists/ (${#GAME_PLAYLISTS[@]} game playlists)${NC}"
echo -e "  ${GREEN}+${NC} Game thumbnails        ${DIM}${DATA_DIR}/thumbnails/${NC}"
echo -e "  ${GREEN}+${NC} Intro video            ${DIM}${DATA_DIR}/intro/${NC}"
echo -e "  ${GREEN}+${NC} Compiled binary        ${DIM}${CPP_DIR}/build/magic_dingus_box_cpp${NC}"
echo -e "  ${GREEN}+${NC} RetroArch cores        ${DIM}${RETROARCH_DIR}/cores/${NC}"
echo -e "  ${GREEN}+${NC} BIOS files             ${DIM}${RETROARCH_DIR}/system/${NC}"
echo -e "  ${GREEN}+${NC} Installed packages     ${DIM}(all system packages and config)${NC}"
echo ""

echo -e "${RED}${BOLD}REMOVING:${NC}"
echo -e "  ${RED}-${NC} Video/music playlists  ${DIM}(${VIDEO_PLAYLISTS[*]})${NC}"
echo -e "  ${RED}-${NC} User media             ${DIM}${DATA_DIR}/media/*, ${INSTALL_DIR}/dev_data/${NC}"
echo -e "  ${RED}-${NC} Device identity        ${DIM}device_info.json (anywhere under ${INSTALL_DIR})${NC}"
echo -e "  ${RED}-${NC} Game saves             ${DIM}${DATA_DIR}/saves/*, ${DATA_DIR}/states/*${NC}"
echo -e "  ${RED}-${NC} Settings               ${DIM}${CONFIG_DIR}/settings.json${NC}"
echo -e "  ${RED}-${NC} SSH host keys          ${DIM}/etc/ssh/ssh_host_*${NC}"
echo -e "  ${RED}-${NC} Logs                   ${DIM}journal, app log, retroarch launcher log${NC}"
echo -e "  ${RED}-${NC} Bash history           ${DIM}magic + root users${NC}"
echo ""

echo -e "${YELLOW}${BOLD}INSTALLING:${NC}"
echo -e "  ${YELLOW}*${NC} First-boot service     ${DIM}magic-first-boot.service (regenerates SSH keys + device identity)${NC}"
echo ""

echo -e "${YELLOW}${BOLD}WARNING: This is destructive and cannot be undone.${NC}"
read -r -p "$(echo -e "${BOLD}Proceed with golden image preparation? [y/N] ${NC}")" confirm
if [[ "${confirm}" != "y" && "${confirm}" != "Y" ]]; then
    echo -e "${RED}Aborted.${NC}"
    exit 1
fi

echo ""

# ---------------------------------------------------------------------------
# Step 1: Stop services
# ---------------------------------------------------------------------------
echo -e "${CYAN}[1/9] Stopping services...${NC}"

systemctl stop magic-dingus-box-cpp.service 2>/dev/null && \
    echo -e "  ${GREEN}Stopped${NC} magic-dingus-box-cpp.service" || \
    echo -e "  ${DIM}magic-dingus-box-cpp.service was not running${NC}"

systemctl stop magic-dingus-web.service 2>/dev/null && \
    echo -e "  ${GREEN}Stopped${NC} magic-dingus-web.service" || \
    echo -e "  ${DIM}magic-dingus-web.service was not running${NC}"

# Give processes time to fully release resources
sleep 1

# ---------------------------------------------------------------------------
# Step 2: Remove video/music playlists
# ---------------------------------------------------------------------------
echo -e "${CYAN}[2/9] Removing video/music playlists...${NC}"

playlist_dir="${DATA_DIR}/playlists"
removed_count=0
for pl in "${VIDEO_PLAYLISTS[@]}"; do
    if [[ -f "${playlist_dir}/${pl}" ]]; then
        rm -f "${playlist_dir}/${pl}"
        echo -e "  ${RED}Removed${NC} ${pl}"
        ((removed_count++)) || true
    else
        echo -e "  ${DIM}Not found: ${pl}${NC}"
    fi
done

# Safety: warn about any remaining non-game playlists
for f in "${playlist_dir}"/*.yaml; do
    [[ -f "$f" ]] || continue
    basename_f="$(basename "$f")"
    is_game=false
    for gp in "${GAME_PLAYLISTS[@]}"; do
        if [[ "$basename_f" == "$gp" ]]; then
            is_game=true
            break
        fi
    done
    if [[ "$is_game" == false ]]; then
        echo -e "  ${YELLOW}Warning: Unknown playlist remains: ${basename_f}${NC}"
    fi
done
echo -e "  ${GREEN}Done${NC} (removed ${removed_count} playlist(s))"

# ---------------------------------------------------------------------------
# Step 3: Remove user media and dev data
# ---------------------------------------------------------------------------
echo -e "${CYAN}[3/9] Removing user media files...${NC}"

media_dir="${DATA_DIR}/media"
if [[ -d "$media_dir" ]]; then
    file_count=$(find "$media_dir" -type f 2>/dev/null | wc -l)
    # Remove contents but keep the directory
    find "$media_dir" -mindepth 1 -delete 2>/dev/null || true
    echo -e "  ${RED}Cleared${NC} ${media_dir}/ (${file_count} files)"
else
    echo -e "  ${DIM}No media directory found${NC}"
fi

dev_data_dir="${INSTALL_DIR}/dev_data"
if [[ -d "$dev_data_dir" ]]; then
    rm -rf "$dev_data_dir"
    echo -e "  ${RED}Removed${NC} ${dev_data_dir}/"
else
    echo -e "  ${DIM}No dev_data directory found${NC}"
fi

# ---------------------------------------------------------------------------
# Step 4: Remove device identity
# ---------------------------------------------------------------------------
echo -e "${CYAN}[4/9] Removing device identity...${NC}"

di_count=0
while IFS= read -r -d '' di_file; do
    rm -f "$di_file"
    echo -e "  ${RED}Removed${NC} ${di_file}"
    ((di_count++)) || true
done < <(find "$INSTALL_DIR" -name "device_info.json" -print0 2>/dev/null)

if [[ "$di_count" -eq 0 ]]; then
    echo -e "  ${DIM}No device_info.json found${NC}"
else
    echo -e "  ${GREEN}Done${NC} (removed ${di_count} file(s))"
fi

# ---------------------------------------------------------------------------
# Step 5: Remove game saves and settings
# ---------------------------------------------------------------------------
echo -e "${CYAN}[5/9] Removing saves, states, and settings...${NC}"

saves_dir="${DATA_DIR}/saves"
if [[ -d "$saves_dir" ]]; then
    save_count=$(find "$saves_dir" -type f 2>/dev/null | wc -l)
    find "$saves_dir" -mindepth 1 -delete 2>/dev/null || true
    echo -e "  ${RED}Cleared${NC} ${saves_dir}/ (${save_count} save files)"
else
    echo -e "  ${DIM}No saves directory${NC}"
fi

states_dir="${DATA_DIR}/states"
if [[ -d "$states_dir" ]]; then
    state_count=$(find "$states_dir" -type f 2>/dev/null | wc -l)
    find "$states_dir" -mindepth 1 -delete 2>/dev/null || true
    echo -e "  ${RED}Cleared${NC} ${states_dir}/ (${state_count} state files)"
else
    echo -e "  ${DIM}No states directory${NC}"
fi

settings_file="${CONFIG_DIR}/settings.json"
if [[ -f "$settings_file" ]]; then
    rm -f "$settings_file"
    echo -e "  ${RED}Removed${NC} ${settings_file} (will regenerate with factory defaults)"
else
    echo -e "  ${DIM}No settings.json found${NC}"
fi

# ---------------------------------------------------------------------------
# Step 6: Remove SSH host keys
# ---------------------------------------------------------------------------
echo -e "${CYAN}[6/9] Removing SSH host keys...${NC}"

key_count=$(ls /etc/ssh/ssh_host_* 2>/dev/null | wc -l)
if [[ "$key_count" -gt 0 ]]; then
    rm -f /etc/ssh/ssh_host_*
    echo -e "  ${RED}Removed${NC} ${key_count} SSH host key files (will regenerate on first boot)"
else
    echo -e "  ${DIM}No SSH host keys found${NC}"
fi

# ---------------------------------------------------------------------------
# Step 7: Clear logs and history
# ---------------------------------------------------------------------------
echo -e "${CYAN}[7/9] Clearing logs and history...${NC}"

# Systemd journal
journalctl --rotate 2>/dev/null || true
journalctl --vacuum-time=1s 2>/dev/null || true
echo -e "  ${RED}Cleared${NC} systemd journal"

# App log
app_log="${CONFIG_DIR}/magic_dingus_box.log"
if [[ -f "$app_log" ]]; then
    rm -f "$app_log"
    echo -e "  ${RED}Removed${NC} ${app_log}"
fi

# RetroArch launcher log
ra_log="${MAGIC_HOME}/retroarch_launcher.log"
if [[ -f "$ra_log" ]]; then
    rm -f "$ra_log"
    echo -e "  ${RED}Removed${NC} ${ra_log}"
fi

# Bash history - magic user
magic_hist="${MAGIC_HOME}/.bash_history"
if [[ -f "$magic_hist" ]]; then
    rm -f "$magic_hist"
    echo -e "  ${RED}Removed${NC} ${magic_hist}"
fi

# Bash history - root user
root_hist="/root/.bash_history"
if [[ -f "$root_hist" ]]; then
    rm -f "$root_hist"
    echo -e "  ${RED}Removed${NC} ${root_hist}"
fi

echo -e "  ${GREEN}Done${NC}"

# ---------------------------------------------------------------------------
# Step 8: Install first-boot service
# ---------------------------------------------------------------------------
echo -e "${CYAN}[8/9] Installing first-boot service...${NC}"

if [[ -f "$SYSTEMD_SRC" ]]; then
    cp "$SYSTEMD_SRC" /etc/systemd/system/magic-first-boot.service
    systemctl daemon-reload
    systemctl enable magic-first-boot.service
    echo -e "  ${GREEN}Installed and enabled${NC} magic-first-boot.service"
else
    echo -e "  ${YELLOW}Warning: ${SYSTEMD_SRC} not found${NC}"
    echo -e "  ${YELLOW}First-boot service must be installed manually before imaging.${NC}"
fi

# ---------------------------------------------------------------------------
# Step 9: Verification
# ---------------------------------------------------------------------------
echo -e "${CYAN}[9/9] Running verification checks...${NC}"
echo ""

pass=0
fail=0

check_pass() {
    echo -e "  ${GREEN}PASS${NC}  $1"
    ((pass++)) || true
}
check_fail() {
    echo -e "  ${RED}FAIL${NC}  $1"
    ((fail++)) || true
}

# Binary exists
if [[ -f "${CPP_DIR}/build/magic_dingus_box_cpp" ]]; then
    check_pass "Binary exists at ${CPP_DIR}/build/magic_dingus_box_cpp"
else
    check_fail "Binary missing at ${CPP_DIR}/build/magic_dingus_box_cpp"
fi

# ROM count
rom_count=$(find "${DATA_DIR}/roms" -type f 2>/dev/null | wc -l)
if [[ "$rom_count" -gt 0 ]]; then
    check_pass "ROMs present: ${rom_count} files in ${DATA_DIR}/roms/"
else
    check_fail "No ROMs found in ${DATA_DIR}/roms/"
fi

# Playlist count
pl_count=$(ls "${DATA_DIR}/playlists/"*.yaml 2>/dev/null | wc -l)
if [[ "$pl_count" -eq "${#GAME_PLAYLISTS[@]}" ]]; then
    check_pass "Game playlists: ${pl_count} YAML files (expected ${#GAME_PLAYLISTS[@]})"
else
    check_fail "Playlist count mismatch: found ${pl_count}, expected ${#GAME_PLAYLISTS[@]}"
fi

# RetroArch cores
core_count=$(ls "${RETROARCH_DIR}/cores/"*.so 2>/dev/null | wc -l)
if [[ "$core_count" -gt 0 ]]; then
    check_pass "RetroArch cores: ${core_count} cores in ${RETROARCH_DIR}/cores/"
else
    check_fail "No RetroArch cores found in ${RETROARCH_DIR}/cores/"
fi

# PS1 BIOS
if [[ -f "${RETROARCH_DIR}/system/scph5501.bin" ]]; then
    check_pass "PS1 BIOS present: scph5501.bin"
else
    check_fail "PS1 BIOS missing: ${RETROARCH_DIR}/system/scph5501.bin"
fi

# Video playlists removed
video_pl_remaining=0
for pl in "${VIDEO_PLAYLISTS[@]}"; do
    if [[ -f "${DATA_DIR}/playlists/${pl}" ]]; then
        ((video_pl_remaining++)) || true
    fi
done
if [[ "$video_pl_remaining" -eq 0 ]]; then
    check_pass "Video/music playlists removed"
else
    check_fail "Video/music playlists still present: ${video_pl_remaining} remaining"
fi

# Media empty
media_remaining=$(find "${DATA_DIR}/media" -type f 2>/dev/null | wc -l)
if [[ "$media_remaining" -eq 0 ]]; then
    check_pass "Media directory empty"
else
    check_fail "Media directory still has ${media_remaining} files"
fi

# First-boot service
if systemctl is-enabled magic-first-boot.service &>/dev/null; then
    check_pass "First-boot service enabled"
else
    check_fail "First-boot service not enabled"
fi

# Device identity removed
di_remaining=$(find "$INSTALL_DIR" -name "device_info.json" 2>/dev/null | wc -l)
if [[ "$di_remaining" -eq 0 ]]; then
    check_pass "Device identity removed"
else
    check_fail "device_info.json still present (${di_remaining} files)"
fi

# Saves cleared
saves_remaining=$(find "${DATA_DIR}/saves" -type f 2>/dev/null | wc -l)
states_remaining=$(find "${DATA_DIR}/states" -type f 2>/dev/null | wc -l)
if [[ "$saves_remaining" -eq 0 && "$states_remaining" -eq 0 ]]; then
    check_pass "Saves and states cleared"
else
    check_fail "Saves/states remain: ${saves_remaining} saves, ${states_remaining} states"
fi

# Settings removed
if [[ ! -f "${CONFIG_DIR}/settings.json" ]]; then
    check_pass "Settings cleared (will regenerate with factory defaults)"
else
    check_fail "settings.json still exists"
fi

echo ""
echo -e "${BOLD}Verification: ${GREEN}${pass} passed${NC}, ${RED}${fail} failed${NC}"

# ---------------------------------------------------------------------------
# Summary and next steps
# ---------------------------------------------------------------------------
echo ""
if [[ "$fail" -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}Golden image preparation complete.${NC}"
else
    echo -e "${YELLOW}${BOLD}Golden image preparation finished with ${fail} warning(s).${NC}"
    echo -e "${YELLOW}Review the failures above before proceeding.${NC}"
fi

echo ""
echo -e "${BOLD}Next steps:${NC}"
echo -e "  1. ${CYAN}sudo shutdown -h now${NC}          Power off the Pi"
echo -e "  2. Remove the SD card from the Pi"
echo -e "  3. Insert the SD card into your Mac"
echo -e "  4. ${CYAN}./scripts/golden_image/create_image.sh${NC}   Create the .img file"
echo ""
