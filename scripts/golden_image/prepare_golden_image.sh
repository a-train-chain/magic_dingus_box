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

# Game playlists that always ship in the golden image. These are the
# canonical RetroArch playlists wired to the cores installed by
# scripts/install_cores.sh.
GAME_PLAYLISTS=(
    arcade.yaml
    atari_7800.yaml
    pc_engine.yaml
    ps1_collection.yaml
    retro_games.yaml
    sega_genesis.yaml
    snes.yaml
)

# Personal/test playlists to REMOVE explicitly (legacy from when the
# kiosk first ran on this Pi). New "default" video/music playlists
# uploaded via the Content Manager get NEW filenames and are preserved
# automatically by the whitelist logic in Step 2 — they don't need to
# be added here.
LEGACY_PLAYLISTS=(
    danny_gatton.yaml
    wes_montgomery.yaml
    paul_franklin.yaml
)

# Default video/music playlists that ship with the golden image.
# Add filenames here as you upload them via the Content Manager.
# The check in Step 2 doesn't actively keep these — it's just for
# the verification block at the end so the script confirms expected
# defaults are present.
EXPECTED_DEFAULT_PLAYLISTS=(
    # e.g. "kids_movies.yaml"
    # e.g. "kiosk_demo.yaml"
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
# Test suite preflight
# ---------------------------------------------------------------------------
# Refuse to prep a golden image from a state where tests are red.
# To override (with audit logging): pass --skip-tests as the first argument.
SKIP_TESTS=0
NEW_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --skip-tests) SKIP_TESTS=1 ;;
        *) NEW_ARGS+=("$arg") ;;
    esac
done
set -- "${NEW_ARGS[@]+"${NEW_ARGS[@]}"}"

if [[ "$SKIP_TESTS" -eq 1 ]]; then
    AUDIT_LOG="/tmp/golden_image_audit.log"
    {
        echo "WARNING: --skip-tests passed. Image being prepared without test gating."
        echo "  Time: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "  User: $(whoami)@$(hostname)"
        echo ""
    } | tee -a "$AUDIT_LOG"
else
    TESTS_RUNNER="${INSTALL_DIR}/tests/run_all.sh"
    if [[ -x "$TESTS_RUNNER" ]]; then
        echo -e "${BOLD}${CYAN}=== Running test suite preflight ===${NC}"
        if ! "$TESTS_RUNNER"; then
            echo ""
            echo -e "${RED}${BOLD}ABORT: Test suite has failures. Image not prepared.${NC}" >&2
            echo -e "${RED}  Fix the failures, or pass --skip-tests to bypass (audit-logged).${NC}" >&2
            exit 1
        fi
        echo -e "${GREEN}=== Tests green; proceeding with image prep ===${NC}"
        echo ""
    else
        echo -e "${YELLOW}WARNING: ${TESTS_RUNNER} not found; skipping preflight.${NC}" >&2
        echo -e "${YELLOW}  This script assumes tests/ is deployed alongside the rest of the repo.${NC}" >&2
    fi
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
echo -e "  ${GREEN}+${NC} Default video/music    ${DIM}${DATA_DIR}/playlists/ (any non-legacy YAML)${NC}"
echo -e "  ${GREEN}+${NC} Game thumbnails        ${DIM}${DATA_DIR}/thumbnails/${NC}"
echo -e "  ${GREEN}+${NC} Intro video            ${DIM}${DATA_DIR}/intro/${NC}"
echo -e "  ${GREEN}+${NC} Compiled binary        ${DIM}${CPP_DIR}/build/magic_dingus_box_cpp${NC}"
echo -e "  ${GREEN}+${NC} RetroArch cores        ${DIM}${RETROARCH_DIR}/cores/${NC}"
echo -e "  ${GREEN}+${NC} BIOS files             ${DIM}${RETROARCH_DIR}/system/${NC}"
echo -e "  ${GREEN}+${NC} Media Browser fixtures ${DIM}${CPP_DIR}/scripts/data/*.json (CFs, indexers, integrations)${NC}"
echo -e "  ${GREEN}+${NC} Installed packages     ${DIM}(all system packages and config)${NC}"
echo ""

echo -e "${RED}${BOLD}REMOVING:${NC}"
echo -e "  ${RED}-${NC} Legacy playlists       ${DIM}(${LEGACY_PLAYLISTS[*]})${NC}"
echo -e "  ${RED}-${NC} User media             ${DIM}${DATA_DIR}/media/*, ${INSTALL_DIR}/dev_data/${NC}"
echo -e "  ${RED}-${NC} Device identity        ${DIM}device_info.json (anywhere under ${INSTALL_DIR})${NC}"
echo -e "  ${RED}-${NC} Game saves             ${DIM}${DATA_DIR}/saves/*, ${DATA_DIR}/states/*${NC}"
echo -e "  ${RED}-${NC} Settings               ${DIM}${CONFIG_DIR}/settings.json${NC}"
echo -e "  ${RED}-${NC} SSH host keys          ${DIM}/etc/ssh/ssh_host_*${NC}"
echo -e "  ${RED}-${NC} WiFi connections       ${DIM}NetworkManager 802-11-wireless profiles${NC}"
echo -e "  ${RED}-${NC} Media Browser state    ${DIM}services/.env, services/config/{radarr,prowlarr,qbittorrent,gluetun,flaresolverr}/*${NC}"
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
echo -e "${CYAN}[2/9] Removing legacy video/music playlists...${NC}"

playlist_dir="${DATA_DIR}/playlists"
removed_count=0
for pl in "${LEGACY_PLAYLISTS[@]}"; do
    if [[ -f "${playlist_dir}/${pl}" ]]; then
        rm -f "${playlist_dir}/${pl}"
        echo -e "  ${RED}Removed${NC} ${pl}"
        ((removed_count++)) || true
    else
        echo -e "  ${DIM}Not found: ${pl}${NC}"
    fi
done

# Inventory remaining playlists. Game playlists are expected. Default
# video/music playlists (uploaded via Content Manager to seed the
# golden image with example content) are also expected — they show up
# as INFO entries below so the operator sees what's shipping.
preserved_count=0
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
    if [[ "$is_game" == true ]]; then
        continue
    fi

    is_legacy=false
    for lp in "${LEGACY_PLAYLISTS[@]}"; do
        if [[ "$basename_f" == "$lp" ]]; then
            is_legacy=true
            break
        fi
    done
    if [[ "$is_legacy" == true ]]; then
        # Already removed above; if it's still here something's wrong.
        echo -e "  ${YELLOW}Warning: legacy playlist still present after removal: ${basename_f}${NC}"
        continue
    fi

    # Anything else is a default/example playlist that ships with the
    # image. List it for transparency.
    echo -e "  ${GREEN}Preserved${NC} ${basename_f} ${DIM}(ships in golden image)${NC}"
    ((preserved_count++)) || true
done
echo -e "  ${GREEN}Done${NC} (removed ${removed_count} legacy, preserved ${preserved_count} default)"

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
# Step 8: Clear WiFi, apt cache, cloud-init, and system state
# ---------------------------------------------------------------------------
echo -e "${CYAN}[8/12] Clearing WiFi connections...${NC}"

wifi_count=0
while IFS= read -r conn_name; do
    [[ -z "$conn_name" ]] && continue
    nmcli connection delete "$conn_name" 2>/dev/null && \
        echo -e "  ${RED}Removed${NC} WiFi: ${conn_name}" && \
        ((wifi_count++)) || true
done < <(nmcli -t -f NAME,TYPE connection show | grep ':802-11-wireless$' | cut -d: -f1)

if [[ "$wifi_count" -eq 0 ]]; then
    echo -e "  ${DIM}No WiFi connections to remove${NC}"
else
    echo -e "  ${GREEN}Done${NC} (removed ${wifi_count} connection(s))"
fi

echo -e "${CYAN}[9/12] Cleaning apt cache...${NC}"

apt_size_before=$(du -sh /var/cache/apt/archives/ 2>/dev/null | cut -f1)
apt-get clean -y 2>/dev/null
apt_size_after=$(du -sh /var/cache/apt/archives/ 2>/dev/null | cut -f1)
echo -e "  ${RED}Cleaned${NC} apt cache (${apt_size_before} -> ${apt_size_after})"

echo -e "${CYAN}[10/12] Removing cloud-init...${NC}"

if dpkg -l cloud-init &>/dev/null 2>&1; then
    apt-get purge -y cloud-init 2>/dev/null
    rm -rf /etc/cloud /var/lib/cloud
    echo -e "  ${RED}Removed${NC} cloud-init and its data"
else
    echo -e "  ${DIM}cloud-init not installed${NC}"
fi

# Clean up disabled playlists directory
disabled_dir="${DATA_DIR}/playlists/disabled"
if [[ -d "$disabled_dir" ]]; then
    rm -rf "$disabled_dir"
    echo -e "  ${RED}Removed${NC} disabled playlists directory"
fi

# Clean PulseAudio state (will regenerate on boot via init_audio.sh)
if [[ -d "${MAGIC_HOME}/.config/pulse" ]]; then
    rm -rf "${MAGIC_HOME}/.config/pulse"
    echo -e "  ${RED}Cleared${NC} PulseAudio state"
fi

# ---------------------------------------------------------------------------
# Step 11: Clean Media Browser services state
# ---------------------------------------------------------------------------
# The Media Browser stack (Radarr / Prowlarr / qBittorrent / Gluetun /
# FlareSolverr) accumulates per-device state that must NOT ship in the
# golden image:
#   - .env: WireGuard private key (per-Pi VPN credential — sharing it
#     across cloned Pis would either prevent simultaneous connections or
#     violate ProtonVPN's multi-device policy), auto-generated qBit
#     password, auto-generated Radarr/Prowlarr API keys (every cloned
#     Pi would have identical keys; security risk).
#   - config/radarr/*: Movie library (which films user added, history,
#     queue), download client settings, plus the per-instance API key in
#     config.xml.
#   - config/prowlarr/*: per-instance API key, indexer sync history,
#     application connection (which embeds the per-Pi Radarr API key).
#   - config/qbittorrent/*: active torrents, fastresume files, auth
#     cookies, session state.
#   - config/gluetun/*: WireGuard runtime state (resolved server, etc).
#   - config/flaresolverr/*: ephemeral browser session state (if any).
#
# On first boot of a cloned Pi, the operator (via Content Manager UI or
# setup_services.sh on Pi) provides a fresh WG config and runs the
# bootstrap. setup_services.sh then idempotently re-creates the entire
# stack from codified fixtures (custom formats, quality profiles,
# indexers, app integrations, download client config) — so the cloned
# Pi reaches the EXACT same configuration state as the source Pi, just
# with fresh per-device credentials and an empty library.
echo -e "${CYAN}[11/13] Cleaning Media Browser services state...${NC}"

services_dir="${INSTALL_DIR}/services"
if [[ -d "$services_dir" ]]; then
    # Stop the Docker stack cleanly (idempotent — succeeds if not running).
    if [[ -f "${services_dir}/docker-compose.yml" ]]; then
        if command -v docker &>/dev/null; then
            (cd "$services_dir" && docker compose down 2>&1 | sed 's/^/    /' || true)
            echo -e "  ${GREEN}Stopped${NC} Media Browser Docker stack"
        else
            echo -e "  ${DIM}Docker not installed; skipping compose down${NC}"
        fi
    else
        echo -e "  ${DIM}docker-compose.yml not found; nothing to stop${NC}"
    fi

    # Wipe the .env (WG creds, API keys, qBit password).
    if [[ -f "${services_dir}/.env" ]]; then
        rm -f "${services_dir}/.env"
        echo -e "  ${RED}Removed${NC} services/.env (WG creds + API keys)"
    else
        echo -e "  ${DIM}services/.env not present${NC}"
    fi

    # Wipe per-service config dirs. Each service regenerates fresh state
    # on next start. The directories themselves are kept (mounted by
    # docker-compose); only their contents are removed.
    for svc in radarr prowlarr qbittorrent gluetun flaresolverr; do
        cfg_dir="${services_dir}/config/${svc}"
        if [[ -d "$cfg_dir" ]]; then
            file_count=$(find "$cfg_dir" -mindepth 1 2>/dev/null | wc -l)
            find "$cfg_dir" -mindepth 1 -delete 2>/dev/null || true
            echo -e "  ${RED}Cleared${NC} services/config/${svc}/ (${file_count} entries)"
        else
            echo -e "  ${DIM}services/config/${svc}/ not present${NC}"
        fi
    done
else
    echo -e "  ${DIM}services/ directory not present (Media Browser not deployed on this Pi)${NC}"
fi

# Note: /mnt/ssd/ contains downloaded movies + active torrent payloads
# but is on a SEPARATE physical disk that is NOT part of the SD card
# image. We don't touch it here — the SSD is swapped or wiped per-Pi
# independently of the SD card golden image.

# ---------------------------------------------------------------------------
# Step 12: Install first-boot service
# ---------------------------------------------------------------------------
echo -e "${CYAN}[12/13] Installing first-boot service...${NC}"

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
# Step 13: Verification
# ---------------------------------------------------------------------------
echo -e "${CYAN}[13/13] Running verification checks...${NC}"
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

# WiFi cleared
wifi_remaining=$(nmcli -t -f TYPE connection show 2>/dev/null | grep -c '802-11-wireless' || true)
if [[ "$wifi_remaining" -eq 0 ]]; then
    check_pass "WiFi connections cleared"
else
    check_fail "WiFi connections still present: ${wifi_remaining} remaining"
fi

# Cloud-init removed
if ! dpkg -l cloud-init &>/dev/null 2>&1; then
    check_pass "cloud-init removed"
else
    check_fail "cloud-init still installed"
fi

# SSH authorized_keys preserved
if [[ -f "${MAGIC_HOME}/.ssh/authorized_keys" ]] && [[ -s "${MAGIC_HOME}/.ssh/authorized_keys" ]]; then
    check_pass "SSH authorized_keys preserved for remote access"
else
    check_fail "SSH authorized_keys missing (remote access will not work)"
fi

# Media Browser state wiped (per-Pi credentials & library state must NOT
# ship in the golden image — see Step 11 for full rationale).
mb_services_dir="${INSTALL_DIR}/services"
if [[ -d "$mb_services_dir" ]]; then
    if [[ -f "${mb_services_dir}/.env" ]]; then
        check_fail "services/.env still present (would leak WG creds + API keys to every clone)"
    else
        check_pass "services/.env removed (per-Pi creds will be regenerated on first boot)"
    fi

    mb_state_remaining=0
    for svc in radarr prowlarr qbittorrent gluetun flaresolverr; do
        cfg_dir="${mb_services_dir}/config/${svc}"
        if [[ -d "$cfg_dir" ]] && [[ -n "$(find "$cfg_dir" -mindepth 1 -print -quit 2>/dev/null)" ]]; then
            ((mb_state_remaining++)) || true
        fi
    done
    if [[ "$mb_state_remaining" -eq 0 ]]; then
        check_pass "Media Browser service config dirs cleaned"
    else
        check_fail "Media Browser service config dirs still contain state (${mb_state_remaining} services)"
    fi

    # Codified fixtures must be present so first-boot setup_services.sh
    # can rebuild the stack to match the source Pi's exact configuration.
    cpp_data_dir="${CPP_DIR}/scripts/data"
    if [[ -d "$cpp_data_dir" ]] && [[ -f "${cpp_data_dir}/radarr_custom_formats.json" ]]; then
        fixture_count=$(find "$cpp_data_dir" -name '*.json' -type f 2>/dev/null | wc -l)
        check_pass "Media Browser config fixtures bundled (${fixture_count} JSON files in scripts/data/)"
    else
        check_fail "Media Browser config fixtures missing — cloned Pis won't bootstrap correctly"
    fi
else
    echo -e "  ${DIM}skip  Media Browser checks (services/ not present on this Pi)${NC}"
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
