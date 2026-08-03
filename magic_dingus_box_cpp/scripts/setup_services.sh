#!/usr/bin/env bash
# Media Browser V2 — Companion services bootstrap.
#
# Run once on the Pi after the first deploy with --media-browser.
# Idempotent: safe to re-run.
#
# What it does:
#   1. Verifies Docker + docker-compose are installed (installs if missing)
#   2. Creates /mnt/ssd storage layout
#   3. Generates .env with random secrets (if not already present)
#   4. Starts the stack for the first time
#   5. Captures the auto-generated Radarr/Prowlarr API keys
#   6. Writes them back to .env for subsequent restarts
#   7. Seeds the "1080p Standard" quality profile in Radarr
#   8. Prints credentials to stdout ONCE for the operator

set -euo pipefail

# Resolve our own absolute path NOW, before any `cd` — later steps cd into
# /opt/magic_dingus_box/services for docker-compose, which breaks any
# subsequent BASH_SOURCE-relative paths.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SKIP_HOST_NETWORKING=0
for arg in "$@"; do
    case "$arg" in
        --skip-host-networking) SKIP_HOST_NETWORKING=1 ;;
    esac
done

# 0. Pi host networking — close IPv6 + DNS leak paths.
#
# Why: ProtonVPN's WireGuard tunnel is IPv4-only. When the Pi prefers
# IPv6 outbound (Comcast hands out v6 by default), routes for v6
# destinations bypass the tunnel entirely. Disable IPv6 globally so
# every outbound connection takes the v4 path through Gluetun (or
# the host's own non-VPN'd v4 path for kiosk/OTA traffic, which is
# the documented accepted gap).
#
# DNS: replace the ISP's resolver (Comcast's 75.75.75.75 by default)
# with Cloudflare's 1.1.1.1 so the ISP can no longer log every domain
# the host queries.
#
# Earlier versions of this script attempted DoH via cloudflared, but
# Cloudflare deprecated the cloudflared DNS-Proxy feature in 2026.2.0
# (see https://developers.cloudflare.com/changelog/2025-11-11-cloudflared-proxy-dns/).
# Plain `nameserver 1.1.1.1` is the simpler replacement — DNS queries
# are cleartext UDP/53 to 1.1.1.1, which the ISP can still observe in
# transit, but importantly:
#   - The host's torrent-related DNS (indexer searches, ghcr.io, etc.)
#     happens *inside* Gluetun's netns where DNS goes through the VPN.
#   - Only host-level non-VPN'd traffic (TMDB metadata from the kiosk
#     binary, OTA updates from GitHub, apt updates) leaks domain names.
# Future work: if DoH becomes important, switch to dnscrypt-proxy
# (apt install dnscrypt-proxy) or systemd-resolved with DNSOverTLS.
#
# Idempotent: writing the same files on every run is a no-op. Skip
# this whole block with `--skip-host-networking` for partial re-runs.
if [ "${SKIP_HOST_NETWORKING}" -eq 0 ]; then
    echo "=== Step 0: Pi host networking ==="

    # 0a. Disable systemd-resolved so it doesn't fight us over resolv.conf.
    #
    # If active, systemd-resolved binds 127.0.0.53:53 and may symlink
    # /etc/resolv.conf to its stub file, which would be re-created on
    # reboot and overwrite our nameserver line.
    if systemctl is-enabled systemd-resolved.service &>/dev/null || \
       systemctl is-active systemd-resolved.service &>/dev/null; then
        echo "Disabling systemd-resolved (was active)..."
        systemctl disable --now systemd-resolved.service || true
    fi
    # If /etc/resolv.conf is a symlink (likely to systemd-resolved's stub),
    # remove it before writing our real file so we don't write through.
    if [ -L /etc/resolv.conf ]; then
        rm -f /etc/resolv.conf
    fi

    # 0b. Disable IPv6 globally
    cat > /etc/sysctl.d/99-magic-dingus-disable-ipv6.conf <<'EOF'
net.ipv6.conf.all.disable_ipv6 = 1
net.ipv6.conf.default.disable_ipv6 = 1
net.ipv6.conf.lo.disable_ipv6 = 1
EOF
    sysctl -p /etc/sysctl.d/99-magic-dingus-disable-ipv6.conf >/dev/null
    echo "IPv6 disabled globally."

    # 0c. Install jq if missing — needed by the tunnel-up gate (Step 4.5)
    # to parse gluetun's /v1/publicip/ip JSON response.
    if ! command -v jq &>/dev/null; then
        echo "Installing jq..."
        apt-get install -y -qq jq
    fi

    # 0d. Stop NetworkManager from rewriting /etc/resolv.conf so our
    # nameserver entry survives reboots and reconnects.
    mkdir -p /etc/NetworkManager/conf.d
    cat > /etc/NetworkManager/conf.d/99-dns.conf <<'EOF'
[main]
dns=none
EOF
    systemctl reload NetworkManager 2>/dev/null || true

    # 0e. Point /etc/resolv.conf at Cloudflare 1.1.1.1.
    # Write atomically via temp file + rename so concurrent name lookups
    # never see a zero-length resolv.conf.
    cat > /etc/resolv.conf.new <<'EOF'
nameserver 1.1.1.1
nameserver 1.0.0.1
options edns0 trust-ad
EOF
    mv /etc/resolv.conf.new /etc/resolv.conf

    # 0f. Verify DNS works
    if ! getent hosts cloudflare.com >/dev/null 2>&1; then
        echo "WARNING: DNS test query failed. Continuing — could be transient."
        echo "         Verify with: getent hosts cloudflare.com"
    else
        echo "DNS active: nameserver 1.1.1.1."
    fi

    # 0g. Clean up any stale cloudflared install/config from earlier
    # script versions that attempted DoH. Idempotent.
    if [ -f /etc/systemd/system/cloudflared.service ] || \
       systemctl is-enabled cloudflared.service &>/dev/null; then
        echo "Removing stale cloudflared service unit (DNS-Proxy was deprecated)..."
        systemctl disable --now cloudflared.service 2>/dev/null || true
        rm -f /etc/systemd/system/cloudflared.service
        rm -rf /etc/cloudflared
        rm -f /etc/apt/sources.list.d/cloudflared.list
        rm -f /usr/share/keyrings/cloudflare-main.gpg
        systemctl daemon-reload
    fi

    echo "=== Step 0 complete ==="
fi

SERVICES_DIR="/opt/magic_dingus_box/services"
STORAGE_ROOT="${STORAGE_ROOT:-/mnt/ssd}"
ENV_FILE="${SERVICES_DIR}/.env"

echo "=== Media Browser V2 service setup ==="

# 1. Docker install check + group membership
# The user under which systemd runs the services unit (User=magic) must be in
# the docker group, otherwise `docker compose up` fails with EACCES on the
# Docker socket on every boot. We handle install + group membership together.
#
# Note: this script runs via `sudo`, so $(whoami) is root. We target the real
# invoking user via $SUDO_USER (set when the script is invoked with sudo).
TARGET_USER="${SUDO_USER:-magic}"

if ! command -v docker &>/dev/null; then
    echo "Installing Docker..."
    curl -fsSL https://get.docker.com | sh
    echo "Docker installed."
fi

if ! docker compose version &>/dev/null; then
    echo "ERROR: docker compose plugin required. Install docker-compose-plugin."
    exit 1
fi

# Ensure target user is in the docker group (idempotent — usermod -aG
# silently succeeds if already a member).
echo "Ensuring ${TARGET_USER} is in docker group..."
usermod -aG docker "${TARGET_USER}"

# 2. Storage layout
echo "Creating storage layout at ${STORAGE_ROOT}..."
# Radarr writes movies directly to ${STORAGE_ROOT}/library/<Title (Year)>/, no
# Movies subdirectory. The earlier setup created library/Movies/ which then sat
# empty forever — drop it.
sudo mkdir -p "${STORAGE_ROOT}"/{downloads/incomplete,downloads/complete,library,library/tv,backups}
# Sonarr's root folder must exist before any import fires. It has been
# destroyed twice by an unidentified empty-dir sweep (2026-08-02); the
# keep-file makes it permanently non-empty so sweeps cannot match it.
# storage_attach.sh re-ensures both the dir and the keep-file on every
# mount activation; ownership lands via the chown -R below.
[ -f "${STORAGE_ROOT}/library/tv/.mdb-keep" ] || sudo touch "${STORAGE_ROOT}/library/tv/.mdb-keep"
# This script runs via sudo, so $(whoami) is root. Use TARGET_USER (resolved
# from $SUDO_USER above) so the storage tree ends up owned by the magic user
# whose UID/GID the Docker containers run under (PUID/PGID in .env). Without
# this, Radarr and qBittorrent get EACCES on every download write.
sudo chown -R "${TARGET_USER}:${TARGET_USER}" "${STORAGE_ROOT}"

# 3. Generate .env if missing
if [ ! -f "${ENV_FILE}" ]; then
    echo "Generating ${ENV_FILE} with random secrets..."
    QBIT_PW=$(openssl rand -base64 18 | tr -d '=+/')
    cat > "${ENV_FILE}" <<EOF
PUID=$(id -u)
PGID=$(id -g)
TZ=$(timedatectl show -p Timezone --value 2>/dev/null || echo "UTC")
STORAGE_ROOT=${STORAGE_ROOT}
RADARR_API_KEY=__WILL_BE_SET_AFTER_FIRST_START__
PROWLARR_API_KEY=__WILL_BE_SET_AFTER_FIRST_START__
SONARR_API_KEY=__WILL_BE_SET_AFTER_FIRST_START__
QBITTORRENT_ADMIN_PASSWORD=${QBIT_PW}
EOF
    chmod 600 "${ENV_FILE}"
    echo "Generated .env with random qBittorrent password."
else
    echo ".env already exists — preserving existing secrets."
fi

# 4. Start stack

# 4a. Pre-pull Byparr (ghcr.io DNS race mitigation).
#
# On a fresh Pi the first ghcr.io DNS lookup sometimes fails before
# DoH (Step 0) is fully warm. Letting `docker compose up` lazily
# pull byparr causes a confusing failure mid-startup. Pre-pull with
# explicit retries instead.
echo "Pre-pulling Byparr (ghcr.io DNS can be flaky on first boot)..."
BYPARR_DIGEST="ghcr.io/thephaseless/byparr@sha256:01a46a2865d9a6db5eb8ead04ec0dd33b8fbe233e8565ae70b50d4cc0af4cfb0"
PULL_OK=0
for i in 1 2 3; do
    if docker pull "${BYPARR_DIGEST}"; then
        PULL_OK=1
        break
    fi
    echo "Pull attempt $i failed; sleeping 10s..."
    sleep 10
done
if [ "${PULL_OK}" -eq 0 ]; then
    echo "ERROR: cannot pull byparr after 3 attempts. Likely cause:"
    echo "       ghcr.io DNS still recovering. Re-run setup in a minute."
    exit 1
fi

cd "${SERVICES_DIR}"
echo "Starting Docker stack..."
#
# --remove-orphans tears down containers that used to be in compose but
# aren't anymore (e.g., the old mdb_flaresolverr from before the Byparr
# swap). Without it, an upgrade leaves the orphan running and holding
# its old port mapping, which blocks Gluetun from rebinding.
docker compose up -d --remove-orphans

# 3.35. Hardlink-layout advisory (informational — never mutates anything).
# The compose file provides a ${STORAGE_ROOT}:/data parent mount that lets
# Radarr hardlink imports (instant, no duplicate storage) once its root
# folder is /data/library. If an EXISTING radarr.db is still on the legacy
# /library root folder, imports keep COPYING (~15-50s/movie + 2x storage
# while seeding). We do NOT auto-migrate here — rewriting a populated
# library's paths is an attended operation with a backup. Just surface the
# opportunity so the operator knows the one-command fix exists.
RADARR_DB_PATH="${SERVICES_DIR}/config/radarr/radarr.db"
if command -v sqlite3 >/dev/null 2>&1 && [ -f "${RADARR_DB_PATH}" ]; then
    CUR_ROOT_FOLDER="$(sqlite3 "${RADARR_DB_PATH}" 'SELECT Path FROM RootFolders LIMIT 1;' 2>/dev/null || echo '')"
    if [ "${CUR_ROOT_FOLDER}" = "/library" ]; then
        echo ""
        echo "  NOTE: Radarr root folder is on the legacy /library layout, so imports"
        echo "        COPY finished downloads into the library (~15-50s each + a duplicate"
        echo "        on-disk copy per seeding torrent). To switch to instant hardlink"
        echo "        imports, run (attended, makes a radarr.db backup first):"
        echo "            ${SCRIPT_DIR}/migrate_hardlink_layout.sh"
        echo ""
    fi
fi

# 3.4. Smooth-playback tuning — sysctls, readahead, container-pause helper.
# These are cheap kernel-level tweaks that materially improve 1080p
# H.264 playback smoothness on the Pi 4B. See each file's header for
# the rationale. (SCRIPT_DIR is resolved at the top of the file before
# any cd, so it survives the `cd "${SERVICES_DIR}"` on line 219.)

# (a) sysctl: lower swappiness + cache pressure for steadier hardware-
#     decoder buffer allocations during playback.
if [ -f "${SCRIPT_DIR}/data/sysctl-magic-playback.conf" ]; then
    sudo install -m 0644 \
        "${SCRIPT_DIR}/data/sysctl-magic-playback.conf" \
        /etc/sysctl.d/99-magic-playback.conf
    sudo sysctl --system >/dev/null 2>&1 || true
    echo "Playback sysctls applied (vm.swappiness=10, vfs_cache_pressure=50)."
fi

# (b) udev: bump readahead on USB block devices (the library SSD) to
#     4 MB so the GStreamer demuxer doesn't stall on micro I/O latency.
if [ -f "${SCRIPT_DIR}/data/udev-99-magic-readahead.rules" ]; then
    sudo install -m 0644 \
        "${SCRIPT_DIR}/data/udev-99-magic-readahead.rules" \
        /etc/udev/rules.d/99-magic-readahead.rules
    sudo udevadm control --reload-rules >/dev/null 2>&1 || true
    # Trigger the rule against the currently-attached devices so the
    # change applies without requiring a reboot or USB re-plug.
    sudo udevadm trigger --subsystem-match=block --action=change >/dev/null 2>&1 || true
    echo "Readahead bumped to 4 MB on USB block devices."
fi

# (c) Container-pause helper. Called by PlaybackScreen via std::system()
#     to freeze Radarr/Prowlarr/Byparr for the duration of a movie.
if [ -f "${SCRIPT_DIR}/playback_services_pause.sh" ]; then
    sudo install -m 0755 \
        "${SCRIPT_DIR}/playback_services_pause.sh" \
        /usr/local/bin/playback_services_pause.sh
fi

# (d) Add the kiosk user to the `docker` group so the pause helper above
#     can `docker pause` without sudo. Idempotent: usermod -aG is a
#     no-op if magic is already a member. Group changes don't propagate
#     into running sessions, but the kiosk service is restarted at the
#     end of provisioning anyway.
if id magic >/dev/null 2>&1 && getent group docker >/dev/null 2>&1; then
    if ! id -nG magic | tr " " "\n" | grep -qx docker; then
        sudo usermod -aG docker magic
        echo "Added 'magic' to the 'docker' group (takes effect on next login / kiosk restart)."
    fi
fi

# 3.5. Install + enable the gluetun-cascade-restart watcher (idempotent).
# Whenever Gluetun (re)starts in isolation — manual `docker stop`, an
# OOM kill, anything not orchestrated through `docker compose` — the
# four containers sharing its netns (Radarr, Prowlarr, qBit, Byparr)
# end up with stale socket bindings and become unreachable from the
# host. The watcher subscribes to `docker events --filter container=
# mdb_gluetun --filter event=start` and runs `compose restart` on the
# dependents so they reattach cleanly.
SYSTEMD_DIR="${SCRIPT_DIR}/../systemd"
if [ -f "${SCRIPT_DIR}/gluetun_cascade_restart.sh" ] && \
   [ -f "${SYSTEMD_DIR}/gluetun-cascade-restart.service" ]; then
    sudo install -m 0755 \
        "${SCRIPT_DIR}/gluetun_cascade_restart.sh" \
        /usr/local/bin/gluetun_cascade_restart.sh
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/gluetun-cascade-restart.service" \
        /etc/systemd/system/gluetun-cascade-restart.service
    sudo systemctl daemon-reload
    sudo systemctl enable --now gluetun-cascade-restart.service
    echo "Gluetun cascade-restart watcher installed + active."
else
    echo "Note: gluetun-cascade-restart assets not found, skipping watcher install."
fi

# 3.6. Install + enable the weekly smoke-test timer (idempotent).
# Why a timer: setup_services.sh runs verify_services.sh once at the
# end of provisioning, but Radarr/Prowlarr/qBit Docker images update
# upstream and their schemas drift. Without periodic re-checks, a
# regression silently breaks the Movies pipeline for an unknown
# stretch of time before the user notices. Weekly cadence catches
# drift within 7 days; failures land in the journal.
if [ -f "${SYSTEMD_DIR}/magic-dingus-smoke-test.service" ] && \
   [ -f "${SYSTEMD_DIR}/magic-dingus-smoke-test.timer" ]; then
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/magic-dingus-smoke-test.service" \
        /etc/systemd/system/magic-dingus-smoke-test.service
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/magic-dingus-smoke-test.timer" \
        /etc/systemd/system/magic-dingus-smoke-test.timer
    sudo systemctl daemon-reload
    sudo systemctl enable --now magic-dingus-smoke-test.timer
    echo "Weekly smoke-test timer installed + active."
else
    echo "Note: smoke-test unit files not found, skipping timer install."
fi

# 3.7. Install + enable boot-time Radarr cooldown-reset oneshot.
# Background: Radarr's IndexerFactory persists a 24-hour cooldown in
# IndexerStatus.DisabledTill (radarr.db SQLite) after consecutive
# search failures. The cooldown survives container restarts and full
# Pi reboots — so a single DNS hiccup at 3 AM can lock the indexer
# chain out until the next morning. From the operator's perspective:
# smoke test green, containers green, but "no source found" on every
# Detail page. Pre-fix recovery required manual SQLite editing.
#
# This oneshot runs after magic-dingus-services brings the docker
# stack up, waits for Radarr to be reachable, then nulls out any
# active DisabledTill values. Idempotent — if no cooldowns are
# active it's a no-op. Best-effort: failure here does not block
# boot or the kiosk.
if [ -f "${SYSTEMD_DIR}/magic-dingus-clear-cooldowns.service" ] && \
   [ -f "${SCRIPT_DIR}/clear_radarr_cooldowns.py" ]; then
    sudo install -m 0755 \
        "${SCRIPT_DIR}/clear_radarr_cooldowns.py" \
        /usr/local/bin/clear_radarr_cooldowns.py
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/magic-dingus-clear-cooldowns.service" \
        /etc/systemd/system/magic-dingus-clear-cooldowns.service
    sudo systemctl daemon-reload
    sudo systemctl enable magic-dingus-clear-cooldowns.service
    echo "Boot-time cooldown reset installed + enabled."
else
    echo "Note: cooldown-clear assets not found, skipping install."
fi

# 3.7b. Install + enable the movie-drive hot-plug re-link.
# Radarr/qBittorrent bind SUBDIRECTORIES of ${STORAGE_ROOT}, and Docker
# resolves a bind source once at container start. If the stack came up
# while the drive was unplugged, those binds point at empty placeholder
# directories on the SD card; plugging the drive in later mounts it on the
# HOST but the running containers keep looking at the old empty dirs —
# Radarr shows an empty library and imports fail silently. Mount
# propagation can't help: the mount event is at the PARENT of the binds.
# This unit is WantedBy=mnt-ssd.mount, so it fires on every attach; the
# script no-ops unless the containers are genuinely stale.
if [ -f "${SYSTEMD_DIR}/magic-dingus-storage-attach.service" ] && \
   [ -f "${SCRIPT_DIR}/storage_attach.sh" ]; then
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/magic-dingus-storage-attach.service" \
        /etc/systemd/system/magic-dingus-storage-attach.service
    sudo systemctl daemon-reload
    sudo systemctl enable magic-dingus-storage-attach.service
    echo "Movie-drive hot-plug re-link installed + enabled."
else
    echo "Note: storage-attach assets not found, skipping install."
fi

# 3.8. Install + enable boot-time qBittorrent password resync.
# Background: qBit's WebUI password is persisted as a PBKDF2 hash in
# qBittorrent.conf. When the docker container is RECREATED (which
# `docker compose up -d` does on any config change), an in-progress
# password change can be lost if qBit gets SIGKILL'd before flushing
# config to disk. The new container then starts with the OLD password,
# Radarr can't authenticate, and the smoke test surfaces "qBit login
# with .env password FAILED" until manually re-synced.
#
# This oneshot re-applies Step 7.5's logic on every boot: try the
# .env password first (no-op happy path), fall back to docker's
# default `adminadmin`, then call setPreferences to lock in .env's
# value. Same pattern as 3.7 (cooldown reset) — idempotent oneshot
# after magic-dingus-services.
if [ -f "${SYSTEMD_DIR}/magic-dingus-sync-qbit-password.service" ] && \
   [ -f "${SCRIPT_DIR}/sync_qbit_password.sh" ]; then
    sudo install -m 0755 \
        "${SCRIPT_DIR}/sync_qbit_password.sh" \
        /usr/local/bin/sync_qbit_password.sh
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/magic-dingus-sync-qbit-password.service" \
        /etc/systemd/system/magic-dingus-sync-qbit-password.service
    sudo systemctl daemon-reload
    sudo systemctl enable magic-dingus-sync-qbit-password.service
    echo "Boot-time qBit password resync installed + enabled."
else
    echo "Note: qBit-password-sync assets not found, skipping install."
fi

# 3.85. Install + enable qBittorrent listen-port sync timer.
# Every time Gluetun re-handshakes with the VPN provider (container
# restart, reconnect, exit rotation), ProtonVPN issues a new NAT-PMP
# forwarded port. Until qBit's listen_port matches that port, incoming
# peer connections from the swarm fail at the Gluetun firewall —
# torrents stall at metaDL / stalledDL with 0 peers even when seeders
# clearly exist. This 60s-cadence timer keeps the two in sync.
#
# The legacy in-place version of this script (deployed by some
# earlier setup_services.sh that's no longer in the repo) had
# qBit password hard-coded to "adminadmin" — silently broke the
# moment Step 7.5 rotated qBit's password. Caught in production on
# 2026-05-26 when Sling Blade refused to leave metaDL even though
# 138 releases with up to 98 seeders existed. New version reads
# password from .env, fails-soft if qBit is mid-restart.
if [ -f "${SYSTEMD_DIR}/qbit-port-sync.service" ] && \
   [ -f "${SYSTEMD_DIR}/qbit-port-sync.timer" ] && \
   [ -f "${SCRIPT_DIR}/qbit_port_sync.sh" ]; then
    sudo install -m 0755 \
        "${SCRIPT_DIR}/qbit_port_sync.sh" \
        /usr/local/bin/qbit-port-sync.sh
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/qbit-port-sync.service" \
        /etc/systemd/system/qbit-port-sync.service
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/qbit-port-sync.timer" \
        /etc/systemd/system/qbit-port-sync.timer
    sudo systemctl daemon-reload
    sudo systemctl enable --now qbit-port-sync.timer
    echo "qBit-port-sync timer (every 60s) installed + active."
else
    echo "Note: qbit-port-sync assets not found, skipping install."
fi

# 3.9. Install + enable the auto-blocklist-stuck-warnings timer.
# Background: Radarr's failure handling auto-retries on FAILED downloads
# (qBit error, stalled torrent) via autoRedownloadFailed=true, but
# WARNING downloads (completed in qBit but Radarr can't import — e.g.
# the file inside is a .exe, .txt, or otherwise unusable) sit in the
# queue indefinitely waiting for an operator decision. Scam torrents
# that pass our search-time Custom Format filters but turn out to be
# junk content fall into this category.
#
# This timer-driven watchdog scans every 15 minutes for warning-state
# items whose statusMessages contain known-bad signatures (executable
# file extensions, "no videos in folder", "unsupported extension")
# and blocklists them, then triggers a fresh search so Radarr picks
# a real alternative.
if [ -f "${SYSTEMD_DIR}/magic-dingus-auto-blocklist.service" ] && \
   [ -f "${SYSTEMD_DIR}/magic-dingus-auto-blocklist.timer" ] && \
   [ -f "${SCRIPT_DIR}/auto_blocklist_stuck_warnings.py" ]; then
    sudo install -m 0755 \
        "${SCRIPT_DIR}/auto_blocklist_stuck_warnings.py" \
        /usr/local/bin/auto_blocklist_stuck_warnings.py
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/magic-dingus-auto-blocklist.service" \
        /etc/systemd/system/magic-dingus-auto-blocklist.service
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/magic-dingus-auto-blocklist.timer" \
        /etc/systemd/system/magic-dingus-auto-blocklist.timer
    sudo systemctl daemon-reload
    sudo systemctl enable --now magic-dingus-auto-blocklist.timer
    echo "Auto-blocklist timer (every 15 min) installed + active."

    # Library import on MOVIES-drive mount: registers pre-loaded movies
    # (/mnt/ssd/library/<Title (Year)>/) with Radarr so the kiosk Library
    # populates when a drive is attached — fresh provisions, replacement
    # SDs, and swapped drives all self-heal. Requires the fstab entry
    # (LABEL=MOVIES → /mnt/ssd, x-systemd.automount) which is added below.
    sudo install -m 0644 \
        "${SYSTEMD_DIR}/magic-dingus-library-import.service" \
        /etc/systemd/system/magic-dingus-library-import.service
    if ! grep -q "LABEL=MOVIES" /etc/fstab; then
        echo "LABEL=MOVIES /mnt/ssd ext4 defaults,nofail,x-systemd.automount,x-systemd.device-timeout=5 0 2" \
            | sudo tee -a /etc/fstab >/dev/null
        echo "fstab: MOVIES-drive automount entry added."
    fi
    sudo systemctl daemon-reload
    sudo systemctl enable magic-dingus-library-import.service
    echo "Library-import-on-mount service installed + enabled."
else
    echo "Note: auto-blocklist assets not found, skipping install."
fi

# 4.0. Install + enable the missing-movies-search timer.
#
# This was WRITTEN, DOCUMENTED and TESTED but never installed on any box.
# Its unit files live in scripts/missing_search/ rather than in systemd/,
# which is the only directory the install blocks above look in, so nothing
# ever copied them into /etc/systemd/system. Caught by a repo audit:
# `systemctl is-enabled magic-dingus-missing-search.timer` returned
# "not-found" on a fully provisioned box while CLAUDE.md documented it as
# running every 4 hours.
#
# What it plugs: "Add to Library" sets addOptions.searchForMovie=true, so
# Radarr fires exactly ONE auto-search the instant a movie is added. If
# that single search comes up empty (release not posted yet, indexer in
# transient cooldown, Byparr mid-challenge), Radarr does not retry at a
# useful cadence — RSS sync only catches brand-new releases going forward.
# The title then sits with no download until someone opens the release
# picker by hand. missing_search.py retries the missing backlog so those
# self-heal; it is idempotent, and a run with nothing missing is a no-op.
MISSING_SEARCH_DIR="${SCRIPT_DIR}/missing_search"
if [ -f "${MISSING_SEARCH_DIR}/magic-dingus-missing-search.service" ] && \
   [ -f "${MISSING_SEARCH_DIR}/magic-dingus-missing-search.timer" ] && \
   [ -f "${MISSING_SEARCH_DIR}/missing_search.py" ]; then
    # NOTE: no copy to /usr/local/bin. Unlike auto_blocklist_stuck_warnings.py,
    # this unit's ExecStart points at the deployed repo path
    # (/opt/magic_dingus_box/.../scripts/missing_search/missing_search.py), so a
    # /usr/local/bin copy would never be executed and would silently go stale.
    sudo install -m 0644 \
        "${MISSING_SEARCH_DIR}/magic-dingus-missing-search.service" \
        /etc/systemd/system/magic-dingus-missing-search.service
    sudo install -m 0644 \
        "${MISSING_SEARCH_DIR}/magic-dingus-missing-search.timer" \
        /etc/systemd/system/magic-dingus-missing-search.timer
    sudo systemctl daemon-reload
    sudo systemctl enable --now magic-dingus-missing-search.timer
    echo "Missing-movies-search timer (every 4h) installed + active."
else
    echo "Note: missing-search assets not found, skipping install."
fi

# 4.1. Install + enable the episode-priority timer (Quick Start ordering).
#
# The kiosk's Quick Start fires a single-episode E1 search alongside every
# season search so something watchable lands in minutes (~2 GB single vs a
# 10-30 GB pack). When several per-episode torrents are queued at once,
# qBittorrent downloads them in the order they were ADDED — indexer-response
# order, not episode order. episode_priority.py keeps each series-season's
# single-episode torrents downloading in EPISODE order so a binge always has
# the next episode arriving first. Idempotent: a correctly ordered queue
# produces zero API calls, and an unreachable qBit exits 0 (no unit flap at
# the 2-minute cadence).
EPISODE_PRIORITY_DIR="${SCRIPT_DIR}/episode_priority"
if [ -f "${EPISODE_PRIORITY_DIR}/magic-dingus-episode-priority.service" ] && \
   [ -f "${EPISODE_PRIORITY_DIR}/magic-dingus-episode-priority.timer" ] && \
   [ -f "${EPISODE_PRIORITY_DIR}/episode_priority.py" ]; then
    # NOTE: no copy to /usr/local/bin — same rule as missing-search above:
    # this unit's ExecStart points at the deployed repo path, so a
    # /usr/local/bin copy would never be executed and would silently go stale.
    sudo install -m 0644 \
        "${EPISODE_PRIORITY_DIR}/magic-dingus-episode-priority.service" \
        /etc/systemd/system/magic-dingus-episode-priority.service
    sudo install -m 0644 \
        "${EPISODE_PRIORITY_DIR}/magic-dingus-episode-priority.timer" \
        /etc/systemd/system/magic-dingus-episode-priority.timer
    sudo systemctl daemon-reload
    sudo systemctl enable --now magic-dingus-episode-priority.timer
    echo "Episode-priority timer (every 2min) installed + active."
else
    echo "Note: episode-priority assets not found, skipping install."
fi

# 4.4. Phone Remote — udev rule + group membership for /dev/uinput access.
#
# Grants the magic-dingus-web service user permission to open uinput so the
# phone can drive a virtual gamepad. Without this, UInput() raises EACCES.
UINPUT_RULES_SRC="${SCRIPT_DIR}/data/90-magicdingus-uinput.rules"
if [[ -f "$UINPUT_RULES_SRC" ]]; then
    echo "Installing /dev/uinput udev rule for Phone Remote..."
    # Ensure the uinput kernel module is loaded NOW and on every boot.
    # On vanilla Debian Bookworm it isn't auto-loaded; on Raspberry Pi OS
    # it usually is, but persist it explicitly so udev triggers below have
    # a device node to match against.
    sudo modprobe uinput || true
    echo "uinput" | sudo tee /etc/modules-load.d/uinput.conf > /dev/null
    sudo install -m 0644 "$UINPUT_RULES_SRC" /etc/udev/rules.d/90-magicdingus-uinput.rules
    sudo udevadm control --reload-rules
    sudo udevadm trigger --name-match=uinput || true
    # Add the magic-dingus-web service user to the input group.
    SERVICE_USER="$(systemctl show -p User --value magic-dingus-web.service 2>/dev/null)"
    SERVICE_USER="${SERVICE_USER:-magic}"  # fallback to "magic" if service not installed yet
    if id -nG "$SERVICE_USER" 2>/dev/null | grep -qw input; then
        echo "  ✓ user $SERVICE_USER already in input group"
    else
        sudo usermod -a -G input "$SERVICE_USER"
        echo "  ✓ added $SERVICE_USER to input group"
        # Restart the running web service so it picks up the new group
        # membership. Without this restart, the existing process retains
        # its original supplementary groups and UInput() will EACCES until
        # next manual restart or reboot.
        if systemctl is-active --quiet magic-dingus-web.service 2>/dev/null; then
            sudo systemctl restart magic-dingus-web.service
            echo "  ✓ magic-dingus-web.service restarted to pick up input group"
        fi
    fi
else
    echo "  ⚠ ${UINPUT_RULES_SRC} not found — skipping uinput rule install"
fi

# 4.5. Wait for Gluetun tunnel to come up.
#
# All four torrent-ecosystem services depend_on gluetun's
# service_healthy condition, which means compose has already
# verified the healthcheck passes before returning. But the
# healthcheck only confirms the control server responds — the
# WireGuard tunnel may still be re-keying, or the public IP fetch
# may not have completed yet.
#
# Hit gluetun's /v1/publicip/ip directly to confirm we can reach
# the outside world *through* the tunnel. Failing here aborts
# setup with a clear error rather than letting Prowlarr fail to
# reach indexers later.
echo "Waiting for Gluetun tunnel to come up..."
TUNNEL_OK=0
for i in $(seq 1 60); do
    if docker exec mdb_gluetun wget -qO- --timeout=3 \
        http://localhost:8000/v1/publicip/ip 2>/dev/null \
        | grep -q '"public_ip"'; then
        EXIT_IP=$(docker exec mdb_gluetun wget -qO- \
            http://localhost:8000/v1/publicip/ip \
            | jq -r .public_ip 2>/dev/null || echo "(unknown)")
        echo "Tunnel up — exit IP: ${EXIT_IP}"
        TUNNEL_OK=1
        break
    fi
    sleep 2
done
if [ "${TUNNEL_OK}" -eq 0 ]; then
    echo "ERROR: Gluetun tunnel did not come up in 120s."
    echo "       Check 'docker logs mdb_gluetun' for WireGuard errors."
    echo "       Common causes: revoked key, NAT-PMP toggle off, ISP"
    echo "       blocks UDP/51820 outbound."
    exit 1
fi

# 5. Wait for services to initialize their configs
echo "Waiting for services to finish first-time init (60s)..."
sleep 60

# 6. Extract auto-generated API keys from Radarr/Prowlarr configs
RADARR_KEY=$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/radarr/config.xml" 2>/dev/null || echo "")
PROWLARR_KEY=$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/prowlarr/config.xml" 2>/dev/null || echo "")
SONARR_KEY=$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/sonarr/config.xml" 2>/dev/null || echo "")

if [ -z "${RADARR_KEY}" ] || [ -z "${PROWLARR_KEY}" ]; then
    echo "WARNING: Could not extract API keys. Services may still be starting."
    echo "Re-run this script in a minute, or extract them manually from config.xml files."
    exit 1
fi

# Sonarr is deliberately NOT part of the hard-fail above. A broken/slow
# Sonarr on a fresh provision used to abort the ENTIRE script here — before
# Step 7.5 (qBit auth hardening: disables the localhost auth bypass, sets
# the random WebUI password) and Step 7.6 (mirrors it to MDB_QBIT_PASS) ever
# ran, leaving qBittorrent on `adminadmin` with the localhost bypass ON. A
# Sonarr-only fault has no business regressing qBittorrent's security
# posture. This degrades safely: with SONARR_KEY empty, the 10-S readiness
# probe below fails closed (SONARR_READY=0), which makes 14b-S/15-S/16-S
# skip cleanly, and 15-S0/17-S are already ||-guarded so an unreachable
# Sonarr can't abort them either — later runs simply pick it back up.
if [ -z "${SONARR_KEY}" ]; then
    echo "WARN: Could not extract Sonarr API key — Sonarr may still be starting."
    echo "      Continuing without it; Sonarr-specific steps below will skip and"
    echo "      self-heal on the next run. Radarr/Prowlarr/qBit setup proceeds."
fi

# 7. Write keys back to .env
sed -i "s|RADARR_API_KEY=.*|RADARR_API_KEY=${RADARR_KEY}|" "${ENV_FILE}"
sed -i "s|PROWLARR_API_KEY=.*|PROWLARR_API_KEY=${PROWLARR_KEY}|" "${ENV_FILE}"

# Sonarr key: append-if-missing. On boxes provisioned before Sonarr
# existed, the .env has no SONARR_API_KEY= line, so a plain `sed`
# replace above would silently no-op and the key would never land.
#
# Guarded on a non-empty key: extraction above is now WARN-and-continue
# (a Sonarr-only fault must not abort the run before qBit hardening), so
# an empty SONARR_KEY reaches here whenever config.xml is momentarily
# absent or half-written — Sonarr rewriting it on a settings change is
# enough. Without this guard that transient would blank a previously
# GOOD key on a healthy box, and because the smoke test now *skips*
# rather than fails on a missing key, the degraded state would be quiet.
# There is nothing to write when the key is empty, so preserving the
# prior value is never worse.
if [ -n "${SONARR_KEY}" ]; then
    if grep -q '^SONARR_API_KEY=' "${ENV_FILE}"; then
        sed -i "s|^SONARR_API_KEY=.*|SONARR_API_KEY=${SONARR_KEY}|" "${ENV_FILE}"
    else
        echo "SONARR_API_KEY=${SONARR_KEY}" >> "${ENV_FILE}"
    fi
fi

# 7.5. qBittorrent security hardening: set the WebUI password to the
# random secret already in .env, and disable the localhost-auth-bypass.
#
# qBittorrent's Docker image starts with admin/adminadmin AND with the
# `Bypass authentication for clients on localhost` preference enabled.
# That second setting masks the password situation entirely — anything
# connecting from 127.0.0.1 (which includes Radarr via the loopback
# port mapping AND the kiosk binary) gets in regardless of credentials.
# Net effect: the random password we wrote to .env earlier was never
# actually applied, and shell-access-to-Pi → qBit-write-access was
# completely open.
#
# Fix: use qBit's WebUI API to (a) set the real password matching .env
# and (b) turn the localhost bypass OFF. After this, every client must
# auth properly, including:
#   - Radarr: already has the password in its download-client config
#     (injected from .env at setup time, see Step 15)
#   - Kiosk binary: reads MDB_QBIT_PASS from the systemd EnvironmentFile=
#     loading services/.env — but that var doesn't exist yet, so we
#     write MDB_QBIT_PASS=<same as QBITTORRENT_ADMIN_PASSWORD> to .env
#     here. The kiosk picks it up on next service restart.
#
# Idempotent: we first try logging in with the .env password; if that
# works, the password is already correct and we just verify the
# bypass setting. If login fails, fall back to the docker default
# `adminadmin` and reset the password.
echo "Hardening qBittorrent: applying random password + disabling localhost auth bypass..."
QBIT_PW=$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)
if [ -z "${QBIT_PW}" ]; then
    echo "  WARN: QBITTORRENT_ADMIN_PASSWORD missing from .env — skipping qBit hardening."
else
    QBIT_COOKIE=$(mktemp)
    LOGIN_OK=0
    # Try the .env password first (idempotent re-run path)
    if curl -fsS -c "${QBIT_COOKIE}" -X POST "http://localhost:8080/api/v2/auth/login" \
        -d "username=admin" --data-urlencode "password=${QBIT_PW}" 2>/dev/null \
        | grep -q "Ok."; then
        LOGIN_OK=1
        echo "  ✓ qBit accepts .env password (already correct)"
    fi
    # Fall back to docker default
    if [ "${LOGIN_OK}" -eq 0 ]; then
        if curl -fsS -c "${QBIT_COOKIE}" -X POST "http://localhost:8080/api/v2/auth/login" \
            -d "username=admin&password=adminadmin" 2>/dev/null | grep -q "Ok."; then
            LOGIN_OK=1
            echo "  qBit was on default credentials — applying random password..."
        fi
    fi
    # Fall back to the modern session temporary password: qBittorrent 5.x
    # removed the adminadmin default entirely and instead prints
    # "A temporary password is provided for this session: XXXX" in the
    # container log at startup. Hit live on the first Pi 5 provisioning
    # (2026-07-22) — without this, fresh installs pulling current images
    # dead-end here.
    if [ "${LOGIN_OK}" -eq 0 ]; then
        QBIT_TMP_PW=$(docker logs mdb_qbittorrent 2>&1 | grep "temporary password" | tail -1 | awk '{print $NF}')
        if [ -n "${QBIT_TMP_PW}" ] && curl -fsS -c "${QBIT_COOKIE}" -X POST "http://localhost:8080/api/v2/auth/login" \
            -d "username=admin" --data-urlencode "password=${QBIT_TMP_PW}" 2>/dev/null | grep -q "Ok."; then
            LOGIN_OK=1
            echo "  qBit was on a session temporary password — applying random password..."
        fi
    fi
    if [ "${LOGIN_OK}" -eq 0 ]; then
        echo "  WARN: could not log in to qBit with .env password, default, or temporary password."
        echo "        qBit may have been hand-configured; verify manually."
    else
        # Apply password + disable localhost bypass. POST takes a
        # 'json' form param with the changed prefs only.
        PREFS_JSON=$(python3 -c "
import json,sys
print(json.dumps({
    'web_ui_username': 'admin',
    'web_ui_password': sys.argv[1],
    'bypass_local_auth': False,
    'bypass_auth_subnet_whitelist_enabled': False,
    # Torrent queueing must stay ON: episode_priority.py's queue
    # ordering is a silent no-op without it (priorities read -1), and
    # the 3-active-downloads cap is part of the disk-contention story.
    # qBit 5.x ships it on by default; this pins against drift.
    'queueing_enabled': True,
    'max_active_downloads': 3,
}))" "${QBIT_PW}")
        if curl -fsS -b "${QBIT_COOKIE}" -X POST \
            "http://localhost:8080/api/v2/app/setPreferences" \
            --data-urlencode "json=${PREFS_JSON}" >/dev/null 2>&1; then
            echo "  ✓ qBit WebUI password set + localhost-auth-bypass disabled"
        else
            echo "  WARN: failed to apply qBit preferences — verify manually"
        fi
    fi
    rm -f "${QBIT_COOKIE}"
fi

# 7.6. Make MDB_QBIT_PASS available to the kiosk via .env (loaded by
# the kiosk's EnvironmentFile= directive). The kiosk's QbittorrentClient
# defaults to "adminadmin" if MDB_QBIT_PASS is unset — which would
# silently fail authentication after Step 7.5 disables the localhost
# bypass. Idempotent: we only write the line if it's missing or stale.
if [ -n "${QBIT_PW}" ]; then
    if grep -q '^MDB_QBIT_PASS=' "${ENV_FILE}"; then
        sed -i "s|^MDB_QBIT_PASS=.*|MDB_QBIT_PASS=${QBIT_PW}|" "${ENV_FILE}"
    else
        echo "MDB_QBIT_PASS=${QBIT_PW}" >> "${ENV_FILE}"
    fi
    # Restart kiosk service if it's running, so it picks up the new env
    # var. Skip silently if the service isn't loaded (fresh Pi during
    # initial bootstrap before deploy_cpp.sh has run).
    if systemctl is-active --quiet magic-dingus-box-cpp.service 2>/dev/null; then
        echo "  Restarting kiosk so it picks up MDB_QBIT_PASS..."
        sudo systemctl restart magic-dingus-box-cpp.service || true
    fi
fi

# 8. Verify Radarr's built-in HD-1080p profile is available.
# Radarr ships 6 built-in profiles on fresh install (Any, SD, HD-720p, HD-1080p,
# Ultra-HD, HD - 720p/1080p). We use HD-1080p (id 4) as the kiosk default rather
# than seeding a custom one — avoids schema-drift issues across Radarr versions
# (minUpgradeFormatScore requirement changed in 5.14+).
echo "Verifying Radarr built-in HD-1080p profile..."
PROFILE_JSON=$(curl -fsS -H "X-Api-Key: ${RADARR_KEY}" http://localhost:7878/api/v3/qualityprofile || echo "[]")
if echo "${PROFILE_JSON}" | grep -q '"name":"HD-1080p"'; then
    echo "  ✓ HD-1080p profile present"
else
    echo "  WARN: HD-1080p profile not found in Radarr response. Verify via web UI."
fi

# 9. Set quality profile "Any" language to "Original" (auto-adapts per
# movie) instead of the default English-only. The English-only setting
# rejects multilingual releases that include English audio because
# Radarr's title parser detects the FIRST language tag in the release
# title — for new theatrical releases that often comes back as Spanish
# / French / Italian. "Original" auto-adapts: each movie gets matched
# against its own original language. Multilingual releases satisfy
# both because they pass at least one language match.
#
# This is the live config we hit in production with Mario Galaxy 2026
# — every release from a 14-result search was rejected with "English
# is wanted, but found Spanish" until the profile was relaxed.
echo "Setting 'Any' quality profile language to 'Original' (auto-adapts per movie)..."
# Fetch is ||-guarded exactly like Step 8's, and for the same reason: Step 6
# proves only that Radarr WROTE its config.xml, not that its API answers. On a
# cold SD card / slow first-time DB init, Radarr reaches this line with a valid
# key and a dead API. Unguarded, curl emits nothing, `json.load(sys.stdin)`
# raises, and the `VAR=$(...)` assignment aborts the ENTIRE provisioning run
# under `set -euo pipefail` — silently skipping Prowlarr indexers, the Radarr
# Apps integration, the qBit category, quality definitions and the smoke test,
# none of which have anything to do with this one profile tweak. The "[]"
# fallback flows through the parser to an empty ANY_PROFILE and a WARN.
#
# The parse is hardened alongside it: a `try` around json.load covers a
# truncated/HTML body, and the isinstance() check covers Radarr answering 200
# with an error OBJECT instead of the expected list (iterating a dict yields
# str keys, and `p['name']` on a str raises TypeError — same fatal path).
QUALITY_PROFILE_LIST=$(curl -fsS -H "X-Api-Key: ${RADARR_KEY}" \
    http://localhost:7878/api/v3/qualityprofile 2>/dev/null || echo "[]")
ANY_PROFILE=$(printf '%s' "${QUALITY_PROFILE_LIST}" | python3 -c "import sys,json
try:
    ps = json.load(sys.stdin)
except Exception:
    ps = []
p = next((p for p in ps if isinstance(p, dict) and p.get('name')=='Any'), None)
if p:
    p['language'] = {'id':-2, 'name':'Original'}
    print(json.dumps(p))
else:
    print('')")
if [[ -n "${ANY_PROFILE}" ]]; then
    PROFILE_ID=$(printf '%s' "${ANY_PROFILE}" | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
    curl -fsS -X PUT -H "X-Api-Key: ${RADARR_KEY}" -H "Content-Type: application/json" \
        -d "${ANY_PROFILE}" \
        "http://localhost:7878/api/v3/qualityprofile/${PROFILE_ID}" >/dev/null \
        && echo "  ✓ 'Any' profile language set to Original" \
        || echo "  WARN: failed to update 'Any' profile language; verify via web UI"
else
    echo "  WARN: Radarr 'Any' profile not readable (service may still be starting) — language left unchanged; a later run applies it."
fi

# 9.5. Radarr API readiness probe — gates every hard-fail Radarr step below.
#
# Mirrors the Sonarr probe in Step 10-S, and sits HERE for the same reason it
# sits there: this is the last point before the script's FIRST hard-fail
# Radarr contact. Steps 8 and 9 above are ||-guarded and degrade to a WARN,
# but Step 10 onward run `VAR=$(python3 <<PYEOF ...)` heredocs that make
# unguarded urllib calls — under `set -euo pipefail` any one of them aborts
# the ENTIRE run the moment Radarr doesn't answer.
#
# Step 6's key extraction is NOT this check: it proves Radarr wrote its
# config.xml, which happens early in startup, well before the DB is ready to
# serve /api/v3. A cold SD card reaches Step 10 with a good key and a dead API.
#
# The probe result GATES the steps below — it must not merely delay them. A
# bare for-loop-then-continue (which is what Step 14 used to do) falls through
# identically whether Radarr answered or never came up, so it bought time
# without buying safety. If Radarr is still down after 60s, we WARN-and-skip
# the Radarr-specific steps: dooming those on an unreachable Radarr is fine and
# intended, but taking Prowlarr's tag/proxy/indexers, the qBit category and the
# smoke test down with it is not. Every skipped step is idempotent, so a later
# re-run applies it.
#
# --max-time bounds the loop in WALL CLOCK, not just iterations. A stopped
# container refuses the connection and fails instantly, but a container that
# is up with its port bound and the app wedged mid-init accepts the socket and
# never answers — an untimed curl blocks there indefinitely and the "60s"
# probe becomes unbounded. 5s is generous for a loopback /system/status, and a
# single slow response costs one iteration rather than failing the probe.
echo "Probing Radarr API readiness..."
RADARR_READY=0
for i in {1..30}; do
    if curl -fsS --max-time 5 -o /dev/null -H "X-Api-Key: ${RADARR_KEY}" \
        http://localhost:7878/api/v3/system/status 2>/dev/null; then
        RADARR_READY=1
        break
    fi
    sleep 2
done
if [ "${RADARR_READY}" -ne 1 ]; then
    echo "  WARN: Radarr not reachable after 60s — Radarr-specific steps below will skip and self-heal on the next run."
else
    echo "  ✓ Radarr API responding"
fi

# 10. Codify Radarr Custom Formats + Any-profile score map.
#
# The kiosk's Pi 4 hardware can only smoothly decode H.264 in the
# 720p-1080p range. Anything else (AV1, HEVC at 1080p, HDR, Remux)
# either has no hardware decoder or blows the size budget. We enforce
# the codec/quality choice with a 3-layer filter and the middle layer
# is a Custom Format score map applied to the "Any" quality profile:
# every grab must net at least minFormatScore=-200 across the six
# formats below.
#
# These formats and scores were originally crafted by hand in the
# Radarr UI, which left them at risk of silent drift — we caught HEVC
# scoring slipping from -250 to -100 in production, which let HEVC
# files slip through the score floor and get downloaded. Codifying the
# spec in scripts/data/radarr_custom_formats.json + reapplying it on
# every setup run eliminates that whole class of drift.
#
# Match-by-name (NOT by id): the JSON file's "id" is just a hint for
# humans diffing changes. Radarr assigns ids server-side and they can
# vary across deploys, so the script always looks up the live id by
# matching the CF "name" field.
#
# Idempotency: a CF with matching name+specifications is left alone;
# a CF with the right name but stale specifications gets PUT'd; a
# missing CF gets POSTed. The Any profile's formatItems are only PUT
# back when at least one score (or minFormatScore) actually drifted.
echo "Configuring Radarr Custom Formats + 'Any' profile score map..."
CF_DATA_FILE="${SCRIPT_DIR}/data/radarr_custom_formats.json"
if [[ ! -f "${CF_DATA_FILE}" ]]; then
    echo "  WARN: ${CF_DATA_FILE} not found — skipping. Custom Formats may already be configured manually; verify via web UI."
elif [ "${RADARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Radarr not reachable — skipping Custom Formats/profile reconciliation (later runs will apply it)."
else
    # Run the full GET → match → POST/PUT for each CF, then PATCH the
    # Any profile's formatItems, all in one python heredoc so we can
    # share state (the live CF list, name→id map, drift counters)
    # without round-tripping through bash variables.
    CF_SUMMARY=$(python3 - "${CF_DATA_FILE}" "${RADARR_KEY}" <<'PYEOF'
import json, sys, urllib.request, urllib.error

cf_data_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:7878/api/v3"

# Desired Any-profile score map. Keep this in lockstep with the
# CLAUDE.md "Quality configuration" section — it's the source of truth
# the script enforces, mirrored on disk for human review.
SCORE_MAP = {
    "AV1 codec (UNWATCHABLE on Pi 4)": -1000,
    "x265/HEVC 1080p+":                -250,
    "HDR / Dolby Vision":              -200,
    "Remux / Raw-HD":                  -500,
    "x264 codec (BONUS)":               50,
    # Release-group policy, RETUNED FOR PI 5 (2026-07-26).
    #
    # These were ONE format ("Trusted small-release groups", +30) that
    # lumped quality rips (RARBG/SURGE/EVO) together with groups whose
    # entire identity is making SMALL files (YIFY/YTS/GalaxyRG/ION10).
    # That bonus stacked with x264's +50, so a low-bitrate YIFY encode
    # scored +80 and beat a genuinely better release at +50 — the rules
    # were optimizing for file size while claiming to optimize quality.
    # Correct on the Pi 4B (hardware H.264, tight storage); wrong now.
    #
    # Measured on Pi 5 (2026-07-26): 1080p software decode costs 36% of
    # 400% CPU for H.264 and 41% for HEVC — roughly 3.5 of 4 cores idle.
    # There is no decode headroom problem to protect against, and the
    # library SSD had 175GB free. So prefer the better encode.
    "Quality release groups":           30,
    "Low-bitrate size-optimized groups": -30,
    # LEGACY: the pre-split format. Boxes provisioned before 2026-07-26
    # still have it in Radarr, and the profile reconciler below only
    # touches formats named in this map — so it must stay here, scored
    # 0, to neutralize it. Removing this line would leave those boxes
    # silently running the old +30 small-file bias forever. It is
    # deliberately absent from radarr_custom_formats.json so fresh
    # provisions never create it.
    "Trusted small-release groups":      0,
    # Scam-rejection formats: well below the -200 minFormatScore floor
    # so a single match makes a release uneligible regardless of other
    # bonuses. Observed in production tonight: trash indexers (TPB-via-
    # Knaben aggregator, the "UIndex.org" prefix farm) post torrents
    # matching newly-released theatrical titles where the content is
    # actually a 0-byte .txt file or a .exe malware payload. Quality
    # profile alone doesn't catch them because the release name reads
    # as legit (proper year, 1080p tag, codec tag). These two CFs
    # detect the giveaways in the title itself.
    "Malware/scam executable in title": -10000,
    "Known scam aggregator branding":   -10000,
    # Title-level foreign-language defense. The quality profile's Language
    # setting (Original) is the primary defense, but Radarr's title parser
    # can be fooled into tagging a release as English when the torrent
    # title contains English text alongside non-Latin characters (e.g.
    # "Дьявол носит Prada 2 / The Devil Wears Prada 2" — parser saw the
    # English half and the cyrillic Russian audio slipped past). This
    # CF catches that via title regex: any cyrillic/CJK/korean char OR
    # an explicit foreign-dub keyword in the title scores -10000, well
    # below minFormatScore=-200 so the release is uneligible regardless
    # of other bonuses.
    "Non-English title signals":        -10000,
}
MIN_FORMAT_SCORE = -200

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def cf_specs_match(live, desired):
    # We compare only the fields we care about (name, regex value,
    # negate, required, includeCustomFormatWhenRenaming). Radarr adds
    # server-side fields like "id" we must ignore.
    if live.get("name") != desired.get("name"): return False
    if live.get("includeCustomFormatWhenRenaming") != desired.get("includeCustomFormatWhenRenaming"): return False
    ls, ds = live.get("specifications", []), desired.get("specifications", [])
    if len(ls) != len(ds): return False
    for a, b in zip(ls, ds):
        if a.get("name") != b.get("name"): return False
        if a.get("implementation") != b.get("implementation"): return False
        if bool(a.get("negate")) != bool(b.get("negate")): return False
        if bool(a.get("required")) != bool(b.get("required")): return False
        # Compare regex value (the only field that ever drifts).
        av = next((f.get("value") for f in a.get("fields", []) if f.get("name") == "value"), None)
        bv = next((f.get("value") for f in b.get("fields", []) if f.get("name") == "value"), None)
        if av != bv: return False
    return True

with open(cf_data_path) as f:
    desired_cfs = json.load(f)

live_cfs = http("GET", "/customformat")
live_by_name = {c["name"]: c for c in live_cfs}

created, updated, unchanged = [], [], []
name_to_id = {}

for desired in desired_cfs:
    name = desired["name"]
    # Strip the spec-file's "id" so it doesn't conflict with whatever
    # Radarr assigns server-side.
    payload = {k: v for k, v in desired.items() if k != "id"}
    if name in live_by_name:
        live = live_by_name[name]
        if cf_specs_match(live, payload):
            unchanged.append(name)
            name_to_id[name] = live["id"]
        else:
            payload["id"] = live["id"]
            result = http("PUT", "/customformat/%d" % live["id"], payload)
            updated.append(name)
            name_to_id[name] = result["id"]
    else:
        result = http("POST", "/customformat", payload)
        created.append(name)
        name_to_id[name] = result["id"]

# Now reconcile the Any profile's formatItems. Radarr exposes one
# formatItems entry per CF (auto-synced); we just adjust scores.
profiles = http("GET", "/qualityprofile")
any_profile = next((p for p in profiles if p["name"] == "Any"), None)
profile_changed = False
score_changes = []

if any_profile is None:
    print("WARN: 'Any' profile missing; cannot apply score map", file=sys.stderr)
else:
    if any_profile.get("minFormatScore") != MIN_FORMAT_SCORE:
        any_profile["minFormatScore"] = MIN_FORMAT_SCORE
        profile_changed = True
        score_changes.append("minFormatScore=%d" % MIN_FORMAT_SCORE)
    for fi in any_profile.get("formatItems", []):
        fname = fi.get("name")
        if fname in SCORE_MAP:
            want = SCORE_MAP[fname]
            if fi.get("score") != want:
                fi["score"] = want
                profile_changed = True
                score_changes.append("%s=%+d" % (fname, want))

    # Codify the quality TIER policy. Three pieces:
    #   1. cutoff = Bluray-720p (id 6) — once Radarr has Bluray-720p
    #      or anything ranked higher in the items[] list, stop
    #      searching for "upgrades."
    #   2. allowed quality tiers — only the 720p+1080p H.264 tiers.
    #      720p is preferred (lower in the items[] list = lower tier
    #      = preferred when both exist? no, items are ordered low→
    #      high quality, so 1080p tiers WIN ties. 720p preference
    #      isn't enforced via cutoff alone; it'd require items
    #      reordering or per-quality custom-format scoring. We allow
    #      both and accept that Radarr picks 1080p when available —
    #      that's the right behavior for old catalog titles where
    #      720p is scarce. The custom-format scoring elsewhere keeps
    #      AV1/HEVC/HDR/Remux out so we don't regress on hardware
    #      decode anyway.)
    #   3. disallow SD / 4K / Remux / raw-DVD tiers entirely so
    #      operator can't accidentally grab them via Pick a Source.
    #
    # The fixture is name-based (not id-based) so it survives Radarr
    # version bumps that re-number quality ids.
    ALLOWED_QUALITY_NAMES = {
        "HDTV-720p", "WEB 720p", "Bluray-720p",
        "HDTV-1080p", "WEB 1080p", "Bluray-1080p",
    }
    DESIRED_CUTOFF_QUALITY_NAME = "Bluray-720p"

    # cutoff is an int id that names a row in items[] (or a sub-row in
    # a quality-group). We resolve by walking items + their nested
    # qualities so the fixture stays name-based.
    def find_quality_id(items, target_name):
        for item in items:
            if item.get("name") == target_name and item.get("id"):
                return item["id"]
            for sub in (item.get("items") or []):
                q = sub.get("quality") or {}
                if q.get("name") == target_name:
                    return q.get("id")
            q = item.get("quality") or {}
            if q.get("name") == target_name:
                return q.get("id")
        return None

    desired_cutoff = find_quality_id(any_profile.get("items", []), DESIRED_CUTOFF_QUALITY_NAME)
    if desired_cutoff and any_profile.get("cutoff") != desired_cutoff:
        any_profile["cutoff"] = desired_cutoff
        profile_changed = True
        score_changes.append("cutoff=%s(id=%d)" % (DESIRED_CUTOFF_QUALITY_NAME, desired_cutoff))

    def item_name(item):
        return item.get("name") or (item.get("quality") or {}).get("name")

    def enforce_allowed(items, group_allowed=False):
        # MUST recurse. Radarr nests real qualities inside group rows:
        # "WEB 2160p" is a group whose children are WEBDL-2160p and
        # WEBRip-2160p. Walking only the top level flips the GROUP to
        # disallowed while its children stay allowed, so the
        # 720p/1080p-only policy was never actually enforced for any
        # grouped tier and 4K/SD releases stayed eligible. Caught by the
        # weekly smoke test, which correctly inspects leaves:
        #   "extra-allowed=WEBDL-2160p,WEBDL-480p,WEBRip-2160p,WEBRip-480p"
        #
        # NOTE ALLOWED_QUALITY_NAMES holds GROUP names ("WEB 720p"), not
        # leaf names ("WEBDL-720p") — a leaf is allowed when its own name
        # is listed OR its containing group is. Testing leaves against the
        # group-name set alone disallows every WEB tier, which starves
        # search results (WEB-DL is the most common source). Learned the
        # hard way on hardware: an earlier version of this fix left only
        # 4 qualities allowed.
        changed = False
        for item in items:
            nm = item_name(item)
            named = nm in ALLOWED_QUALITY_NAMES if nm else False
            want_allowed = named or group_allowed
            children = item.get("items") or []
            if children:
                if enforce_allowed(children, want_allowed):
                    changed = True
                # Group row reflects its leaves, so the Radarr UI checkbox
                # never contradicts what is actually eligible.
                want_allowed = any(c.get("allowed") for c in children)
            if item.get("allowed") != want_allowed:
                item["allowed"] = want_allowed
                changed = True
                score_changes.append("%s.allowed=%s" % (nm, want_allowed))
        return changed

    if enforce_allowed(any_profile.get("items", [])):
        profile_changed = True

    if profile_changed:
        http("PUT", "/qualityprofile/%d" % any_profile["id"], any_profile)

print(json.dumps({
    "created": created,
    "updated": updated,
    "unchanged": unchanged,
    "profile_changed": profile_changed,
    "score_changes": score_changes,
}))
PYEOF
)
    # Pretty-print the summary for the operator. The python block emits
    # a single JSON line on stdout; we feed it back via stdin to avoid
    # any shell-quoting issues with embedded double quotes.
    printf '%s' "${CF_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
if s["profile_changed"]:
    print("  ✓ Any profile score map updated: " + ", ".join(s["score_changes"]))
else:
    print("  ✓ Any profile score map already matches desired state")
'
fi

# 10-S. Codify Sonarr Custom Formats + Any-profile score map.
#
# Sonarr twin of the Radarr block above. Same codec/quality policy: the
# kiosk decodes H.264 720p/1080p; AV1/HEVC-1080p+/HDR/Remux and non-English
# / scam releases are pushed below the -200 minFormatScore floor. All CF
# specs are ReleaseTitleSpecification title regexes (identical fixture to
# Radarr's), so they port verbatim. Two Sonarr-specific choices:
#   * SCORE_MAP omits the legacy "Trusted small-release groups" (0) entry —
#     that entry only exists to neutralize pre-2026-07-26 RADARR boxes;
#     Sonarr is greenfield and never had it.
#   * ALLOWED_QUALITY_NAMES lists LEAF quality names (HDTV-720p, WEBDL-720p,
#     …) rather than group names. Sonarr's default profile grouping isn't
#     assumed; a leaf is allowed when its own name matches, and group rows
#     reflect their leaves. No profile-language mutation (Sonarr v4 handles
#     language separately from the quality profile).
echo "Configuring Sonarr Custom Formats + 'Any' profile score map..."
SONARR_CF_DATA_FILE="${SCRIPT_DIR}/data/sonarr_custom_formats.json"
if [[ ! -f "${SONARR_CF_DATA_FILE}" ]]; then
    echo "  WARN: ${SONARR_CF_DATA_FILE} not found — skipping. Verify Sonarr Custom Formats via web UI."
else
    # Sonarr readiness probe. This is the script's FIRST hard-fail Sonarr
    # API contact (the python below makes ~10 unguarded urllib calls), so
    # the warm-up wait lives HERE, not later: a still-initializing Sonarr
    # either answers within this loop or every later Sonarr step was
    # doomed anyway. (Radarr's equivalent probe sits in Step 14 because
    # its earlier contacts at Steps 8-9 are ||-guarded.)
    #
    # The probe result GATES the python block below — it must not just
    # delay it. A bare for-loop-then-continue falls through identically
    # whether Sonarr answered or never came up; if Sonarr is still down
    # after 60s, the python's first http() call raises an uncaught
    # urllib error, and under `set -euo pipefail` the `VAR=$(python3 ...)`
    # assignment aborts the ENTIRE setup_services.sh — killing Prowlarr's
    # cloudflare tag, FlareSolverr proxy, indexers, Radarr Apps
    # integration, qBit category, quality definitions, and the final
    # smoke test, none of which have anything to do with Sonarr. Dooming
    # *later Sonarr steps* on an unreachable Sonarr is fine and intended;
    # dooming unrelated steps is not, so we WARN-and-skip instead.
    SONARR_READY=0
    for i in {1..30}; do
        if curl -fsS -o /dev/null -H "X-Api-Key: ${SONARR_KEY}" \
            http://localhost:8989/api/v3/system/status 2>/dev/null; then
            SONARR_READY=1
            break
        fi
        sleep 2
    done
    if [ "${SONARR_READY}" -ne 1 ]; then
        echo "  WARN: Sonarr not reachable after 60s — skipping Custom Formats/profile reconciliation (later runs will apply it)."
    else
        SONARR_CF_SUMMARY=$(python3 - "${SONARR_CF_DATA_FILE}" "${SONARR_KEY}" <<'PYEOF'
import json, sys, urllib.request, urllib.error

cf_data_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:8989/api/v3"

SCORE_MAP = {
    "AV1 codec (UNWATCHABLE on Pi 4)": -1000,
    "x265/HEVC 1080p+":                -250,
    "HDR / Dolby Vision":              -200,
    "Remux / Raw-HD":                  -500,
    "x264 codec (BONUS)":               50,
    "Quality release groups":           30,
    "Low-bitrate size-optimized groups": -30,
    "Malware/scam executable in title": -10000,
    "Known scam aggregator branding":   -10000,
    "Non-English title signals":        -10000,
}
MIN_FORMAT_SCORE = -200

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def cf_specs_match(live, desired):
    if live.get("name") != desired.get("name"): return False
    if live.get("includeCustomFormatWhenRenaming") != desired.get("includeCustomFormatWhenRenaming"): return False
    ls, ds = live.get("specifications", []), desired.get("specifications", [])
    if len(ls) != len(ds): return False
    for a, b in zip(ls, ds):
        if a.get("name") != b.get("name"): return False
        if a.get("implementation") != b.get("implementation"): return False
        if bool(a.get("negate")) != bool(b.get("negate")): return False
        if bool(a.get("required")) != bool(b.get("required")): return False
        av = next((f.get("value") for f in a.get("fields", []) if f.get("name") == "value"), None)
        bv = next((f.get("value") for f in b.get("fields", []) if f.get("name") == "value"), None)
        if av != bv: return False
    return True

with open(cf_data_path) as f:
    desired_cfs = json.load(f)

live_cfs = http("GET", "/customformat")
live_by_name = {c["name"]: c for c in live_cfs}

created, updated, unchanged = [], [], []
name_to_id = {}

for desired in desired_cfs:
    name = desired["name"]
    payload = {k: v for k, v in desired.items() if k != "id"}
    if name in live_by_name:
        live = live_by_name[name]
        if cf_specs_match(live, payload):
            unchanged.append(name)
            name_to_id[name] = live["id"]
        else:
            payload["id"] = live["id"]
            result = http("PUT", "/customformat/%d" % live["id"], payload)
            updated.append(name)
            name_to_id[name] = result["id"]
    else:
        result = http("POST", "/customformat", payload)
        created.append(name)
        name_to_id[name] = result["id"]

profiles = http("GET", "/qualityprofile")
any_profile = next((p for p in profiles if p["name"] == "Any"), None)
profile_changed = False
score_changes = []

if any_profile is None:
    print("WARN: 'Any' profile missing; cannot apply score map", file=sys.stderr)
    print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged,
                      "profile_changed": False, "score_changes": []}))
    sys.exit(0)

if any_profile.get("minFormatScore") != MIN_FORMAT_SCORE:
    any_profile["minFormatScore"] = MIN_FORMAT_SCORE
    profile_changed = True
    score_changes.append("minFormatScore=%d" % MIN_FORMAT_SCORE)
for fi in any_profile.get("formatItems", []):
    fname = fi.get("name")
    if fname in SCORE_MAP:
        want = SCORE_MAP[fname]
        if fi.get("score") != want:
            fi["score"] = want
            profile_changed = True
            score_changes.append("%s=%+d" % (fname, want))

# Leaf-name allowed set: only the eight H.264 720p/1080p tiers.
ALLOWED_QUALITY_NAMES = {
    "HDTV-720p", "WEBDL-720p", "WEBRip-720p", "Bluray-720p",
    "HDTV-1080p", "WEBDL-1080p", "WEBRip-1080p", "Bluray-1080p",
}
DESIRED_CUTOFF_QUALITY_NAME = "Bluray-720p"

def find_quality_id(items, target_name):
    for item in items:
        if item.get("name") == target_name and item.get("id"):
            return item["id"]
        for sub in (item.get("items") or []):
            q = sub.get("quality") or {}
            if q.get("name") == target_name:
                return q.get("id")
        q = item.get("quality") or {}
        if q.get("name") == target_name:
            return q.get("id")
    return None

desired_cutoff = find_quality_id(any_profile.get("items", []), DESIRED_CUTOFF_QUALITY_NAME)
if desired_cutoff:
    if any_profile.get("cutoff") != desired_cutoff:
        any_profile["cutoff"] = desired_cutoff
        profile_changed = True
        score_changes.append("cutoff=%s(id=%d)" % (DESIRED_CUTOFF_QUALITY_NAME, desired_cutoff))
else:
    print("WARN: could not resolve quality id for cutoff '%s' (Sonarr may have renamed it); cutoff left unchanged" % DESIRED_CUTOFF_QUALITY_NAME, file=sys.stderr)

def item_name(item):
    return item.get("name") or (item.get("quality") or {}).get("name")

def enforce_allowed(items, group_allowed=False):
    changed = False
    for item in items:
        nm = item_name(item)
        named = nm in ALLOWED_QUALITY_NAMES if nm else False
        want_allowed = named or group_allowed
        children = item.get("items") or []
        if children:
            if enforce_allowed(children, want_allowed):
                changed = True
            want_allowed = any(c.get("allowed") for c in children)
        if item.get("allowed") != want_allowed:
            item["allowed"] = want_allowed
            changed = True
            score_changes.append("%s.allowed=%s" % (nm, want_allowed))
    return changed

if enforce_allowed(any_profile.get("items", [])):
    profile_changed = True

if profile_changed:
    http("PUT", "/qualityprofile/%d" % any_profile["id"], any_profile)

print(json.dumps({
    "created": created,
    "updated": updated,
    "unchanged": unchanged,
    "profile_changed": profile_changed,
    "score_changes": score_changes,
}))
PYEOF
)
        printf '%s' "${SONARR_CF_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
if s["profile_changed"]:
    print("  ✓ Sonarr Any profile score map updated: " + ", ".join(s["score_changes"]))
else:
    print("  ✓ Sonarr Any profile score map already matches desired state")
'
    fi
fi

# 11. Prowlarr: cloudflare tag.
#
# A handful of indexers (Demonoid, EZTV, Internet Archive, Magnetz,
# Torrent Downloads, TorrentDownload) sit behind Cloudflare's bot-block
# and only resolve when Prowlarr routes the request through the
# FlareSolverr indexer-proxy in Step 12. The wiring is done with a
# shared "cloudflare" tag — set on both the FlareSolverr proxy AND
# every Cloudflare-protected indexer. Prowlarr matches tags between
# the two and routes requests for tagged indexers via the tagged proxy.
#
# We seed the tag first so Steps 12 + 13 can reference it by id.
echo "Configuring Prowlarr 'cloudflare' tag..."
PROWLARR_TAGS_FILE="${SCRIPT_DIR}/data/prowlarr_tags.json"
if [[ ! -f "${PROWLARR_TAGS_FILE}" ]]; then
    echo "  WARN: ${PROWLARR_TAGS_FILE} not found — skipping. Tag may already be configured manually; verify via web UI."
    PROWLARR_CLOUDFLARE_TAG_ID=""
else
    # Idempotent: GET /tag, find by label, POST only if missing. Capture
    # the resulting id (live or just-created) to a single-line stdout
    # for the bash-side variable so subsequent steps can pass it as
    # the tag id when reconciling proxy/indexer "tags": [...] arrays.
    TAG_RESULT=$(python3 - "${PROWLARR_TAGS_FILE}" "${PROWLARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
tags_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:9696/api/v1"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

with open(tags_path) as f:
    desired_tags = json.load(f)

live_tags = http("GET", "/tag") or []
live_by_label = {t["label"]: t for t in live_tags}

created, unchanged = [], []
label_to_id = {}
for desired in desired_tags:
    label = desired["label"]
    if label in live_by_label:
        unchanged.append(label)
        label_to_id[label] = live_by_label[label]["id"]
    else:
        result = http("POST", "/tag", {"label": label})
        created.append(label)
        label_to_id[label] = result["id"]

print(json.dumps({"created": created, "unchanged": unchanged, "label_to_id": label_to_id}))
PYEOF
)
    # Pretty-print + extract the cloudflare tag id for the next steps.
    echo "${TAG_RESULT}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("unchanged", s["unchanged"])
'
    PROWLARR_CLOUDFLARE_TAG_ID=$(echo "${TAG_RESULT}" | python3 -c "import json,sys; print(json.load(sys.stdin)['label_to_id'].get('cloudflare', ''))")
fi

# 12. Prowlarr: FlareSolverr indexer proxy.
#
# FlareSolverr is a headless-browser sidecar (own container) that
# solves Cloudflare JS challenges and returns the decoded HTML.
# Prowlarr's "indexer proxy" feature can route requests for any
# tag-matched indexer through it — that's how the cloudflare-tagged
# indexers in Step 13 actually reach their sites in production.
#
# The fixture stores the tag membership as `tags_by_label` (a list of
# human-readable labels) so the file stays diff-friendly across
# deploys. We translate label → id at apply time using Step 11's map.
echo "Configuring Prowlarr FlareSolverr indexer proxy..."
PROWLARR_PROXIES_FILE="${SCRIPT_DIR}/data/prowlarr_indexerproxies.json"
if [[ ! -f "${PROWLARR_PROXIES_FILE}" ]]; then
    echo "  WARN: ${PROWLARR_PROXIES_FILE} not found — skipping. Proxy may already be configured manually; verify via web UI."
else
    PROXY_SUMMARY=$(python3 - "${PROWLARR_PROXIES_FILE}" "${PROWLARR_KEY}" "${PROWLARR_CLOUDFLARE_TAG_ID:-}" <<'PYEOF'
import json, sys, urllib.request
proxies_path, api_key, cloudflare_tag_id = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:9696/api/v1"

# Build a single label→id map. Right now only "cloudflare" is in
# scope, but doing it as a dict keeps the path open for additional
# tags without restructuring the fixture format.
LABEL_TO_ID = {}
if cloudflare_tag_id:
    LABEL_TO_ID["cloudflare"] = int(cloudflare_tag_id)

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    # Reduce a Servarr-style fields[] array (each entry has name+value
    # plus a pile of UI-only metadata) to a flat {name: value} dict.
    # We only ever care about (name, value) for drift detection.
    return {f["name"]: f.get("value") for f in (fields or [])}

def resolve_tags(desired):
    # Translate fixture's `tags_by_label` to the live id list.
    return sorted(LABEL_TO_ID[lbl] for lbl in desired.get("tags_by_label", []) if lbl in LABEL_TO_ID)

def shape_payload(desired):
    # Strip the diff-friendly `tags_by_label` key and replace with the
    # API's id-based `tags`. Drop fixture-only `id` if any.
    payload = {k: v for k, v in desired.items() if k not in ("tags_by_label", "id")}
    payload["tags"] = resolve_tags(desired)
    return payload

def match(live, desired_payload):
    # Compare the fields we own. Servarr adds server-side keys (id,
    # supports*, etc.) that we ignore.
    keys = ("name", "implementation", "configContract",
            "onHealthIssue", "includeHealthWarnings")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    # Subset comparison: only verify the fields we specify — any
    # server-injected extras are ignored.
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    for k, v in des_fd.items():
        if live_fd.get(k) != v:
            return False
    return True

with open(proxies_path) as f:
    desired_proxies = json.load(f)

live_proxies = http("GET", "/indexerproxy") or []
live_by_name = {p["name"]: p for p in live_proxies}

created, updated, unchanged = [], [], []
for desired in desired_proxies:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/indexerproxy/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/indexerproxy", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
    echo "${PROXY_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
fi

# 13. Prowlarr: indexers.
#
# Nine Cardigann-backed public-tracker definitions captured from the
# reference Pi. Four are enabled by default (LimeTorrents, TPB,
# TorrentDownload, YTS) — those are the ones the Media Browser
# actively queries via Radarr+Prowlarr. The other five are kept
# pre-configured but disabled so an operator can flip them on later
# without re-discovering URLs / definitionFiles. The cloudflare-
# tagged ones (Demonoid, EZTV, Internet Archive, Magnetz, Torrent
# Downloads, TorrentDownload) route through Step 12's FlareSolverr.
#
# Match by name (server-assigned ids vary). We treat enable + tags +
# fields as the "owned" fields; if any drift, PUT to reset. URL lists
# (`indexerUrls`, `legacyUrls`) and capabilities aren't owned — those
# come from the Cardigann definitionFile and are recomputed by
# Prowlarr on every save. Comparing them would force pointless PUTs.
echo "Configuring Prowlarr indexers..."
PROWLARR_INDEXERS_FILE="${SCRIPT_DIR}/data/prowlarr_indexers.json"
if [[ ! -f "${PROWLARR_INDEXERS_FILE}" ]]; then
    echo "  WARN: ${PROWLARR_INDEXERS_FILE} not found — skipping."
else
    INDEXER_SUMMARY=$(python3 - "${PROWLARR_INDEXERS_FILE}" "${PROWLARR_KEY}" "${PROWLARR_CLOUDFLARE_TAG_ID:-}" <<'PYEOF'
import json, sys, urllib.request
indexers_path, api_key, cloudflare_tag_id = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:9696/api/v1"

# EZTV is enabled for season-pack coverage but is cloudflare-tagged and
# routes through Byparr — its Cardigann definition can 404/challenge at
# apply time. Treat an EZTV apply failure as a warning, not fatal, so a
# transient Byparr/EZTV outage never bricks a whole setup run. The stable
# indexers the smoke test asserts on are unaffected.
NON_FATAL_ENABLED = {"EZTV"}

LABEL_TO_ID = {}
if cloudflare_tag_id:
    LABEL_TO_ID["cloudflare"] = int(cloudflare_tag_id)

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    return {f["name"]: f.get("value") for f in (fields or [])}

def resolve_tags(desired):
    return sorted(LABEL_TO_ID[lbl] for lbl in desired.get("tags_by_label", []) if lbl in LABEL_TO_ID)

def shape_payload(desired):
    # Strip diff-friendly metadata (`tags_by_label`, fixture-only id,
    # `added` timestamp) and replace with id-based `tags`.
    payload = {k: v for k, v in desired.items()
               if k not in ("tags_by_label", "id", "added")}
    payload["tags"] = resolve_tags(desired)
    return payload

def match(live, desired_payload):
    # Owned fields: name, implementation, configContract, definitionName,
    # enable, priority, appProfileId, tags, and the fields[] map. Anything
    # else (capabilities, indexerUrls, legacyUrls, language, encoding,
    # description, supportsRss, etc.) comes from the Cardigann definition
    # and is regenerated server-side — comparing it would cause needless
    # churn since Prowlarr can mutate those between calls.
    keys = ("name", "implementation", "configContract", "definitionName",
            "enable", "priority", "appProfileId")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    # Subset comparison on fields[]: Prowlarr injects a pile of base/
    # torrentBase settings (queryLimit, grabLimit, seedRatio, etc.)
    # that aren't in the fixture and we don't want to fight over. We
    # only verify every field WE specify matches the live value; any
    # extras are server-managed defaults and ignored.
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    for k, v in des_fd.items():
        if live_fd.get(k) != v:
            return False
    return True

with open(indexers_path) as f:
    desired_indexers = json.load(f)

live_indexers = http("GET", "/indexer") or []
live_by_name = {i["name"]: i for i in live_indexers}

created, updated, unchanged, skipped = [], [], [], []
for desired in desired_indexers:
    name = desired["name"]
    payload = shape_payload(desired)
    # A single indexer must not sink the whole provisioning run. The
    # common failure is a Cardigann definitionName that Prowlarr's
    # upstream repo has since removed (e.g. "demonoid-clone" 404'd on
    # indexers.prowlarr.com, making Prowlarr 500 the POST — hit live on
    # the first Pi 5 provisioning, 2026-07-22). These are the
    # pre-configured-but-DISABLED convenience indexers; losing one is a
    # warning, not a fatal. The active indexers that the smoke test
    # asserts on are unaffected because they still exist upstream.
    try:
        if name in live_by_name:
            live = live_by_name[name]
            if match(live, payload):
                unchanged.append(name)
            else:
                payload["id"] = live["id"]
                http("PUT", "/indexer/%d" % live["id"], payload)
                updated.append(name)
        else:
            http("POST", "/indexer", payload)
            created.append(name)
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as e:
        # Re-raise for ENABLED indexers — a live/active indexer failing
        # is a real problem the operator needs to see fail loudly. EZTV is
        # the deliberate exception: enabled but allowed to fail soft
        # (HTTP error OR Byparr-challenge timeout).
        if desired.get("enable", False) and name not in NON_FATAL_ENABLED:
            sys.stderr.write("FATAL: enabled indexer %r failed: %s\n" % (name, e))
            raise
        skipped.append(name)

print(json.dumps({"created": created, "updated": updated,
                  "unchanged": unchanged, "skipped": skipped}))
PYEOF
)
    echo "${INDEXER_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
show("skipped (disabled or soft-fail, stale/unreachable upstream)", s.get("skipped", []))
'
fi

# 14. Prowlarr → Radarr Apps integration.
#
# Prowlarr "applications" is the auto-sync that pushes indexer changes
# to Radarr. Without this, every time we add/edit an indexer the
# operator would have to mirror it manually in Radarr's settings.
#
# The fixture's `apiKey` field is sanitized to "********" (the same
# masked value Prowlarr's GET /applications returns for security).
# We inject the live RADARR_KEY at apply time, which is fine because:
#   - On create, the real key is needed and goes through.
#   - On a re-run, the GET response masks the key to "********" — we
#     can't compare against our live key, so we just check non-secret
#     fields (URLs, syncCategories, syncLevel) for drift. If those
#     match, we no-op; if they don't, we PUT with the freshly-injected
#     real key (which won't drift because we control RADARR_KEY).
#
# Radarr readiness: gated on RADARR_READY from Step 9.5 (Radarr is the API
# key consumer here — if it's down we can't even prove our key works). This
# block used to run its own bare `for i in {1..30}` warm-up loop that broke
# out on success and simply fell through on failure — identical control flow
# either way, so a never-ready Radarr proceeded straight into the python
# below, whose first http() call raised an uncaught urllib error and took the
# whole script down with it. The wait now happens once, at Step 9.5, and its
# result actually decides whether this block runs.
echo "Configuring Prowlarr → Radarr Apps integration..."
PROWLARR_APPS_FILE="${SCRIPT_DIR}/data/prowlarr_applications.json"
if [[ ! -f "${PROWLARR_APPS_FILE}" ]]; then
    echo "  WARN: ${PROWLARR_APPS_FILE} not found — skipping."
elif [ "${RADARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Radarr not reachable — skipping Prowlarr → Radarr Apps integration (later runs will apply it)."
else
    APPS_SUMMARY=$(python3 - "${PROWLARR_APPS_FILE}" "${PROWLARR_KEY}" "${RADARR_KEY}" "${SONARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
apps_path, prowlarr_key, radarr_key, sonarr_key = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
BASE = "http://localhost:9696/api/v1"

def http(method, path, body=None):
    headers = {"X-Api-Key": prowlarr_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    return {f["name"]: f.get("value") for f in (fields or [])}

def inject_api_key(payload):
    # Select the key by target application: the Radarr app gets
    # RADARR_KEY, the Sonarr app gets SONARR_KEY. Before this fix the
    # single radarr_key was injected into every app, so a Sonarr app
    # would have been configured with Radarr's key and silently failed
    # to sync. Mutate a deep-copied list so the source dict stays clean.
    key = sonarr_key if payload.get("implementation") == "Sonarr" else radarr_key
    new_fields = []
    for f in payload.get("fields", []):
        if f.get("name") == "apiKey":
            f = dict(f)
            f["value"] = key
        new_fields.append(f)
    payload = dict(payload)
    payload["fields"] = new_fields
    return payload

def shape_payload(desired):
    payload = {k: v for k, v in desired.items() if k != "id"}
    return inject_api_key(payload)

def match(live, desired_payload):
    # Owned fields: name, syncLevel, enable, implementation. For
    # `fields[]` we compare every value EXCEPT `apiKey`, since the
    # API masks apiKey to "********" on GET — we can't tell whether
    # the live key matches our injected one. In practice that's fine:
    # the only way the key drifts is if RADARR_KEY rotated, in which
    # case we'd want to push the new one anyway, which Step 14 does
    # not detect (acceptable trade-off — operator can re-create the
    # app integration manually if they rotate keys).
    keys = ("name", "syncLevel", "enable", "implementation", "configContract")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    # Drop apiKey from both before comparing — it's masked on GET.
    live_fd.pop("apiKey", None)
    des_fd.pop("apiKey", None)
    # Subset comparison: Prowlarr 2.x adds server-injected fields like
    # `syncRejectBlocklistedTorrentHashesWhileGrabbing` that aren't in
    # the fixture. Only verify our specified fields match.
    for k, v in des_fd.items():
        if live_fd.get(k) != v:
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    return True

with open(apps_path) as f:
    desired_apps = json.load(f)

live_apps = http("GET", "/applications") or []
live_by_name = {a["name"]: a for a in live_apps}

created, updated, unchanged = [], [], []
for desired in desired_apps:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/applications/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/applications", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
    echo "${APPS_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
fi

# 14b. Radarr: minimum-seeders threshold per indexer.
#
# Filters out dead-swarm releases at search time. Without this,
# Radarr happily grabs releases from old (10-30+ year) movies whose
# trackers still report cached "9 seeders exist" but no peer is
# actually online — qBit then sits in `metaDL` forever. Symptom is
# a download that hangs at 0% with 0 connected peers despite the
# torrent metadata claiming a healthy swarm.
#
# We set minimumSeeders=3 on every Radarr indexer (the indexer
# objects on Radarr's side, populated by the Apps-integration sync
# in Step 14). At search time, indexers that return releases with
# fewer than 3 reported seeders never even reach Radarr's scoring
# pass, so the operator never gets a "stalled" download from a
# dead swarm.
#
# 3 is a deliberately conservative floor: high enough to weed out
# graveyard torrents (observed live: "Old School (2003)" RARBG
# releases reporting 1-2 stale seeders that qBit could never reach,
# hanging at metaDL 0%), low enough that legitimate niche releases
# (foreign films, documentaries, just-released indie movies) still
# pass. We don't compete on "fastest possible" downloads — we just
# refuse to start ones that won't progress.
#
# Idempotent: PUT only when the live value differs from 3. Default
# Radarr value when an indexer is first synced from Prowlarr is 1
# (effectively no filter), so on a fresh setup every indexer gets
# bumped on first run and stays at 3 thereafter. NOTE: a Prowlarr
# Apps re-sync (e.g. adding an indexer) can reset these back to 1 —
# re-run this script or the standalone bump if downloads start
# stalling at metaDL again.
echo "Configuring Radarr indexer minimum_seeders threshold..."
if [ "${RADARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Radarr not reachable — skipping Radarr indexer minimum-seeders bump (later runs will apply it)."
else
    SEEDER_SUMMARY=$(python3 - "${RADARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
api_key = sys.argv[1]
BASE = "http://localhost:7878/api/v3"
TARGET = 3

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

result = {"updated": [], "unchanged": [], "skipped_disabled": []}
for ix in http("GET", "/indexer") or []:
    name = ix.get("name", "?")
    if not (ix["enableRss"] or ix["enableAutomaticSearch"] or ix["enableInteractiveSearch"]):
        result["skipped_disabled"].append(name)
        continue
    bumped = False
    for fld in ix.get("fields", []):
        if fld["name"] == "minimumSeeders":
            old = fld.get("value")
            if old != TARGET:
                fld["value"] = TARGET
                bumped = True
            break
    if bumped:
        http("PUT", "/indexer/%d?forceSave=true" % ix["id"], ix)
        result["updated"].append(name)
    else:
        result["unchanged"].append(name)

print(json.dumps(result))
PYEOF
)
    echo "${SEEDER_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("updated to min=5  ", s["updated"])
show("already at 5      ", s["unchanged"])
show("skipped (disabled)", s["skipped_disabled"])
'
fi

# 14b-S. Sonarr: minimum-seeders threshold per indexer.
#
# Sonarr twin of the Radarr 14b block above — same rationale (dead-swarm
# releases from Prowlarr's sync default of 1 hang qBit at metaDL 0%),
# same idempotency (PUT only when the live value differs from 3), same
# caveat (a Prowlarr apps re-sync can reset these to 1; a script re-run
# restores them).
echo "Configuring Sonarr indexer minimum_seeders threshold..."
if [ "${SONARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Sonarr not reachable — skipping Sonarr indexer minimum-seeders bump (later runs will apply it)."
else
    SONARR_SEEDER_SUMMARY=$(python3 - "${SONARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
api_key = sys.argv[1]
BASE = "http://localhost:8989/api/v3"
TARGET = 3

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

result = {"updated": [], "unchanged": [], "skipped_disabled": []}
for ix in http("GET", "/indexer") or []:
    name = ix.get("name", "?")
    if not (ix["enableRss"] or ix["enableAutomaticSearch"] or ix["enableInteractiveSearch"]):
        result["skipped_disabled"].append(name)
        continue
    bumped = False
    for fld in ix.get("fields", []):
        if fld["name"] == "minimumSeeders":
            old = fld.get("value")
            if old != TARGET:
                fld["value"] = TARGET
                bumped = True
            break
    if bumped:
        http("PUT", "/indexer/%d?forceSave=true" % ix["id"], ix)
        result["updated"].append(name)
    else:
        result["unchanged"].append(name)

print(json.dumps(result))
PYEOF
)
    echo "${SONARR_SEEDER_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("updated to min=3  ", s["updated"])
show("already at 3      ", s["unchanged"])
show("skipped (disabled)", s["skipped_disabled"])
'
fi

# 14c. Indexer-sync fallback: directly inject Prowlarr indexers that
# failed Radarr's add-time test.
#
# Some Cardigann-defined indexers (notably LimeTorrents) don't return
# any movie-tagged results when Radarr probes them with an empty
# query in category 2000 during the indexer-test that runs on every
# add. Prowlarr's app-sync logs "Query successful, but no results in
# the configured categories were returned" and silently SKIPS those
# indexers — so they never appear in Radarr at all, even though they
# are enabled and healthy in Prowlarr and would return plenty of
# results to a real movie search.
#
# Without this step, the user's Movies search will systematically miss
# indexers like LimeTorrents — which often carry the only seeded copy
# of older catalog titles (1990s/early-2000s movies that the YIFY/
# scene/web-rip indexers don't track). Symptom: "added to library"
# but "no releases found" for valid older titles.
#
# Architectural fix: after the standard Prowlarr→Radarr sync, detect
# Prowlarr-enabled indexers that did NOT make it to Radarr, and
# INSERT them directly into Radarr's SQLite Indexers table. We
# bypass the test-on-add by writing the row directly. Three things
# make this safe:
#
#   1. The Prowlarr master API key authenticates against any indexer's
#      Newznab/Torznab proxy URL (verified empirically) — so we don't
#      need per-indexer per-app keys (which Prowlarr only issues at
#      successful sync time). Same key is used in Settings.apiKey.
#
#   2. Radarr's Indexers table schema is stable across versions
#      (Id, Name, Implementation, Settings, ConfigContract,
#      EnableRss, EnableAutomaticSearch, EnableInteractiveSearch,
#      Priority, Tags, DownloadClientId).
#
#   3. The injection is idempotent: we skip indexers that already
#      exist by name in Radarr's table. So normal Prowlarr sync runs
#      first and gets first claim; we only fill the gaps.
#
# Radarr must be restarted at the end so its in-memory indexer cache
# picks up the new rows.
echo "Configuring Radarr indexer fallback (direct DB insert for test-failed indexers)..."
RADARR_DB="/opt/magic_dingus_box/services/config/radarr/radarr.db"
if [ "${RADARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Radarr not reachable — skipping indexer fallback injection (later runs will apply it)."
else
    INDEXER_FALLBACK_SUMMARY=$(python3 - "${PROWLARR_KEY}" "${RADARR_KEY}" "${RADARR_DB}" <<'PYEOF'
import json, sys, sqlite3, urllib.request, os

prowlarr_key, radarr_key, radarr_db = sys.argv[1], sys.argv[2], sys.argv[3]

def http_get(url, key):
    req = urllib.request.Request(url, headers={"X-Api-Key": key})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())

# 1) Enabled Prowlarr indexers (the desired set from the operator's
#    perspective — Prowlarr is the source of truth for which indexers
#    are active).
prowlarr_idx = http_get("http://localhost:9696/api/v1/indexer", prowlarr_key)
prowlarr_enabled = [i for i in prowlarr_idx if i.get("enable")]

# 2) Existing Radarr indexers — name is the join key. Synced indexers
#    from Prowlarr arrive named "<Indexer> (Prowlarr)".
radarr_idx = http_get("http://localhost:7878/api/v3/indexer", radarr_key)
radarr_names = {i["name"] for i in radarr_idx}

# 3) Diff: which Prowlarr-enabled indexers are absent from Radarr?
# Skip indexers with no Movies capability (e.g. EZTV, TV-only): Prowlarr
# rightly never syncs them to Radarr, so their absence from Radarr is
# CORRECT — not a sync failure for this fallback to repair. Injecting
# one would point Radarr's movie searches at a TV-only indexer and trip
# the smoke test's exact-set indexer assertion.
def has_movies_caps(p):
    cats = (p.get("capabilities") or {}).get("categories") or []
    return any(c.get("name") == "Movies" for c in cats)

missing = [
    p for p in prowlarr_enabled
    if f"{p['name']} (Prowlarr)" not in radarr_names
    and has_movies_caps(p)
]

if not missing:
    print(json.dumps({"injected": [], "already_synced": [
        f"{p['name']} (Prowlarr)" for p in prowlarr_enabled
    ]}))
    sys.exit(0)

if not os.path.exists(radarr_db):
    print(json.dumps({"error": f"Radarr DB not found at {radarr_db}", "missing": [p["name"] for p in missing]}))
    sys.exit(1)

# 4) Build a Settings JSON for each missing indexer. The Newznab
#    (Torznab in DB terminology) settings shape is what Prowlarr's
#    app-sync writes when sync succeeds. We replicate it field-for-
#    field, with the Prowlarr master key as apiKey and the per-
#    indexer Newznab proxy URL.
def build_settings(prowlarr_indexer_id):
    return {
        "minimumSeeders": 5,
        "seedCriteria": {},
        "rejectBlocklistedTorrentHashesWhileGrabbing": False,
        "requiredFlags": [],
        "baseUrl": f"http://localhost:9696/{prowlarr_indexer_id}/",
        "apiPath": "/api",
        "apiKey": prowlarr_key,
        "categories": [2000, 2010, 2020, 2030, 2040, 2045, 2050, 2060, 2070, 2080],
        "animeCategories": [],
        "additionalParameters": "",
        "multiLanguages": [],
    }

# 5) INSERT directly. We open the DB even while Radarr is running — SQLite
#    handles concurrent writes via WAL/lock — but we restart Radarr
#    afterwards so its in-memory indexer cache reloads.
con = sqlite3.connect(radarr_db, timeout=30)
cur = con.cursor()

injected = []
for p in missing:
    name = f"{p['name']} (Prowlarr)"
    settings_json = json.dumps(build_settings(p["id"]))
    cur.execute("""
        INSERT INTO Indexers (
            Name, Implementation, Settings, ConfigContract,
            EnableRss, EnableAutomaticSearch, EnableInteractiveSearch,
            Priority, Tags, DownloadClientId
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        name,
        "Torznab",
        settings_json,
        "TorznabSettings",
        1, 1, 1,           # all three enable flags on
        25,                # default priority
        "[]",              # no tags
        0,                 # no specific download client
    ))
    injected.append(name)

con.commit()
con.close()

print(json.dumps({"injected": injected, "already_synced": list(radarr_names)}))
PYEOF
)
    INJECTED=$(printf '%s' "${INDEXER_FALLBACK_SUMMARY}" | python3 -c '
import json, sys
try:
    s = json.loads(sys.stdin.read())
except Exception:
    s = {}
inj = s.get("injected", [])
if inj: print(",".join(inj))
')
    if [[ -n "${INJECTED}" ]]; then
        echo "  injected (DB direct): ${INJECTED}"
        echo "  Restarting Radarr so it reloads the indexer cache..."
        docker restart mdb_radarr >/dev/null
        # Wait for Radarr to be reachable again so subsequent steps
        # (qBit download client config, custom format scoring) don't
        # race against an unhealthy API. Re-arm RADARR_READY from the
        # result: we just took the API down deliberately, so the flag
        # set back at Step 9.5 is stale until it answers again. If it
        # never comes back, clearing the flag makes Steps 15/16 skip
        # with a WARN instead of aborting the rest of the run.
        RADARR_READY=0
        for i in {1..30}; do
            if curl -fsS --max-time 5 -o /dev/null -H "X-Api-Key: ${RADARR_KEY}" \
                http://localhost:7878/api/v3/system/status 2>/dev/null; then
                RADARR_READY=1
                break
            fi
            sleep 2
        done
        if [ "${RADARR_READY}" -ne 1 ]; then
            echo "  WARN: Radarr did not come back within 60s of the restart — remaining Radarr steps will skip."
        fi
    else
        echo "  all enabled Prowlarr indexers already present in Radarr"
    fi
fi

# 15. Radarr → qBittorrent download client.
#
# Radarr's grab pipeline: indexer search → magnet/.torrent URL →
# hand off to a configured download client. The fixture wires the
# qBittorrent container (reachable through Gluetun's network at
# `gluetun:8080`) with category=radarr so all Radarr-grabbed torrents
# land in their own qBit category and can be cleaned up
# independently of the operator's personal torrents.
#
# Like Step 14 the fixture has a sanitized `password` field; we
# inject the live ${QBIT_PW} (read from .env) at apply time. The
# password is masked to "********" on subsequent GETs, so drift
# detection ignores it (same trade-off as the apiKey in Step 14).
echo "Configuring Radarr → qBittorrent download client..."
RADARR_DLCLIENTS_FILE="${SCRIPT_DIR}/data/radarr_downloadclients.json"
if [[ ! -f "${RADARR_DLCLIENTS_FILE}" ]]; then
    echo "  WARN: ${RADARR_DLCLIENTS_FILE} not found — skipping."
else
    QBIT_PW=$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)
    if [[ -z "${QBIT_PW}" ]]; then
        echo "  WARN: QBITTORRENT_ADMIN_PASSWORD missing from ${ENV_FILE} — skipping."
    elif [ "${RADARR_READY:-0}" -ne 1 ]; then
        echo "  WARN: Radarr not reachable — skipping Radarr → qBittorrent download client (later runs will apply it)."
    else
        DLCLIENT_SUMMARY=$(python3 - "${RADARR_DLCLIENTS_FILE}" "${RADARR_KEY}" "${QBIT_PW}" <<'PYEOF'
import json, sys, urllib.request
clients_path, api_key, qbit_pw = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:7878/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    return {f["name"]: f.get("value") for f in (fields or [])}

def inject_password(payload):
    new_fields = []
    for f in payload.get("fields", []):
        if f.get("name") == "password":
            f = dict(f)
            f["value"] = qbit_pw
        new_fields.append(f)
    payload = dict(payload)
    payload["fields"] = new_fields
    return payload

def shape_payload(desired):
    payload = {k: v for k, v in desired.items() if k != "id"}
    return inject_password(payload)

def match(live, desired_payload):
    keys = ("name", "enable", "protocol", "priority", "implementation",
            "configContract", "removeCompletedDownloads",
            "removeFailedDownloads")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    # Drop the masked password from comparison (same logic as the
    # apiKey in Step 14). Also drop fields the live response carries
    # that aren't in the fixture (Radarr appends per-implementation
    # advanced defaults like initialState, recentMoviePriority, etc.
    # that are server-side managed and we don't want to fight over).
    live_fd.pop("password", None)
    des_fd.pop("password", None)
    # Compare only the keys the fixture cares about; ignore any extras
    # Radarr added on its own.
    for k in des_fd:
        if live_fd.get(k) != des_fd[k]:
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    return True

with open(clients_path) as f:
    desired_clients = json.load(f)

live_clients = http("GET", "/downloadclient") or []
live_by_name = {c["name"]: c for c in live_clients}

created, updated, unchanged = [], [], []
for desired in desired_clients:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/downloadclient/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/downloadclient", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
        echo "${DLCLIENT_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
    fi
fi

# 15-S0. Sonarr root folder /data/library/tv.
#
# Greenfield — Sonarr uses the hardlink-capable /data mount from day one
# (host /mnt/ssd/library/tv, created in Step 2). POST /rootfolder requires
# the path to exist and be writable by PUID; the mkdir + chown in Step 2
# guarantees both. Idempotent: skip if already present.
echo "Configuring Sonarr root folder /data/library/tv..."
if [ "${SONARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Sonarr not reachable — later runs will apply it."
else
    # The curl inside the substitution MUST be ||-guarded: under the script's
    # set -euo pipefail, a bare failing curl in a $(pipeline) aborts the whole
    # run before the tolerant python path ever executes (same convention as
    # PROFILE_JSON at Step 8: `curl ... || echo "[]"`).
    SONARR_RF_PRESENT=$( (curl -fsS -H "X-Api-Key: ${SONARR_KEY}" \
        http://localhost:8989/api/v3/rootfolder 2>/dev/null || echo "[]") \
        | python3 -c "import sys,json
try: print(any(r.get('path')=='/data/library/tv' for r in json.load(sys.stdin)))
except Exception: print(False)")
    if [[ "${SONARR_RF_PRESENT}" == "True" ]]; then
        echo "  ✓ Sonarr root folder /data/library/tv already present"
    else
        if curl -fsS -X POST -H "X-Api-Key: ${SONARR_KEY}" -H "Content-Type: application/json" \
            -d '{"path":"/data/library/tv"}' \
            http://localhost:8989/api/v3/rootfolder >/dev/null 2>&1; then
            echo "  ✓ Sonarr root folder /data/library/tv created"
        else
            echo "  WARN: failed to create Sonarr root folder; verify /data/library/tv is writable"
        fi
    fi
fi

# 15-S. Sonarr → qBittorrent download client.
#
# Sonarr twin of the Radarr download-client block. Same qBittorrent
# container (reachable via gluetun:8080), category=sonarr so Sonarr's
# torrents segregate from Radarr's. Password injected from .env at apply
# time; masked on GET so drift-detection ignores it. Field name is
# tvCategory (Sonarr) vs Radarr's movieCategory — compared generically.
echo "Configuring Sonarr → qBittorrent download client..."
SONARR_DLCLIENTS_FILE="${SCRIPT_DIR}/data/sonarr_downloadclients.json"
if [[ ! -f "${SONARR_DLCLIENTS_FILE}" ]]; then
    echo "  WARN: ${SONARR_DLCLIENTS_FILE} not found — skipping."
else
    QBIT_PW=$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)
    if [[ -z "${QBIT_PW}" ]]; then
        echo "  WARN: QBITTORRENT_ADMIN_PASSWORD missing from ${ENV_FILE} — skipping."
    elif [ "${SONARR_READY:-0}" -ne 1 ]; then
        echo "  WARN: Sonarr not reachable — skipping Sonarr download-client reconciliation (later runs will apply it)."
    else
        SONARR_DLCLIENT_SUMMARY=$(python3 - "${SONARR_DLCLIENTS_FILE}" "${SONARR_KEY}" "${QBIT_PW}" <<'PYEOF'
import json, sys, urllib.request
clients_path, api_key, qbit_pw = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = "http://localhost:8989/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

def fields_to_dict(fields):
    return {f["name"]: f.get("value") for f in (fields or [])}

def inject_password(payload):
    new_fields = []
    for f in payload.get("fields", []):
        if f.get("name") == "password":
            f = dict(f)
            f["value"] = qbit_pw
        new_fields.append(f)
    payload = dict(payload)
    payload["fields"] = new_fields
    return payload

def shape_payload(desired):
    payload = {k: v for k, v in desired.items() if k != "id"}
    return inject_password(payload)

def match(live, desired_payload):
    keys = ("name", "enable", "protocol", "priority", "implementation",
            "configContract", "removeCompletedDownloads",
            "removeFailedDownloads")
    for k in keys:
        if live.get(k) != desired_payload.get(k):
            return False
    live_fd = fields_to_dict(live.get("fields"))
    des_fd = fields_to_dict(desired_payload.get("fields"))
    live_fd.pop("password", None)
    des_fd.pop("password", None)
    for k in des_fd:
        if live_fd.get(k) != des_fd[k]:
            return False
    if sorted(live.get("tags", [])) != sorted(desired_payload.get("tags", [])):
        return False
    return True

with open(clients_path) as f:
    desired_clients = json.load(f)

live_clients = http("GET", "/downloadclient") or []
live_by_name = {c["name"]: c for c in live_clients}

created, updated, unchanged = [], [], []
for desired in desired_clients:
    name = desired["name"]
    payload = shape_payload(desired)
    if name in live_by_name:
        live = live_by_name[name]
        if match(live, payload):
            unchanged.append(name)
        else:
            payload["id"] = live["id"]
            http("PUT", "/downloadclient/%d" % live["id"], payload)
            updated.append(name)
    else:
        http("POST", "/downloadclient", payload)
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
        echo "${SONARR_DLCLIENT_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def show(label, items):
    if items:
        print("  " + label + ": " + ", ".join(items))
show("created  ", s["created"])
show("updated  ", s["updated"])
show("unchanged", s["unchanged"])
'
    fi
fi

# 16. Radarr quality definitions (custom 720p/1080p size limits).
#
# Radarr ships with default size limits per quality (e.g. WEBDL-1080p
# default maxSize≈400 MB/min). Wasteful for a kiosk — the third layer
# of the quality filter (CLAUDE.md "Quality configuration") tightens
# the budget. The fixture carries the Pi 5 tuning (2026-07-26 retune):
#   720p qualities → maxSize 60 MB/min, preferredSize 40 MB/min
#   1080p qualities → maxSize 100 MB/min, preferredSize 70 MB/min
# One golden image serves both boards, so the Python below overrides
# preferredSize back to the original lean values (25/40) when it finds
# itself on a Pi 4B — hardware H.264 decodes either size fine there,
# but Pi 4 units ship with tighter storage and small-files-preferred
# was the deliberate Pi 4-era choice. maxSize is identical on both.
#
# The fixture lists every quality (including SD + 4K + Remux) so
# re-running the script also corrects values that may have drifted
# from a UI-side edit. Quality.id is stable across Radarr versions,
# so we match by quality.id (not quality.name, which has been
# renamed across major versions in the past).
echo "Configuring Radarr quality definitions..."
RADARR_QUALITY_FILE="${SCRIPT_DIR}/data/radarr_qualitydefinitions.json"
if [[ ! -f "${RADARR_QUALITY_FILE}" ]]; then
    echo "  WARN: ${RADARR_QUALITY_FILE} not found — skipping."
elif [ "${RADARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Radarr not reachable — skipping Radarr quality definitions (later runs will apply it)."
else
    QD_SUMMARY=$(python3 - "${RADARR_QUALITY_FILE}" "${RADARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
qd_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:7878/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

with open(qd_path) as f:
    desired_qds = json.load(f)

# Board gate: the fixture is the Pi 5 tuning; a Pi 4B keeps the lean
# preferred sizes (see the shell comment above). Quality ids are the
# stable Radarr identifiers: 720p family (HDTV/WEBDL/WEBRip/Bluray) and
# 1080p family respectively.
def pi_model():
    try:
        with open("/proc/device-tree/model", "rb") as f:
            return f.read().decode(errors="replace")
    except OSError:
        return ""

if pi_model().startswith("Raspberry Pi 4 "):
    PI4_PREFERRED = {4: 25, 5: 25, 14: 25, 6: 25,   # 720p family
                     9: 40, 3: 40, 15: 40, 7: 40}   # 1080p family
    for d in desired_qds:
        if d["quality"]["id"] in PI4_PREFERRED:
            d["preferredSize"] = PI4_PREFERRED[d["quality"]["id"]]

live_qds = http("GET", "/qualitydefinition") or []
# Live response: each entry has top-level `id` (definition id) and
# nested `quality.id` (quality id, stable across versions). The PUT
# endpoint takes the top-level definition id, so we keep both.
live_by_quality_id = {q["quality"]["id"]: q for q in live_qds}

unchanged, updated, missing = [], [], []
for desired in desired_qds:
    qid = desired["quality"]["id"]
    if qid not in live_by_quality_id:
        # Should never happen — Radarr ships every built-in quality
        # on first start. If it does, log it and skip rather than
        # POST (the API doesn't support creating new quality defs).
        missing.append(desired["quality"]["name"])
        continue
    live = live_by_quality_id[qid]
    drift = (
        live.get("minSize") != desired.get("minSize")
        or live.get("maxSize") != desired.get("maxSize")
        or live.get("preferredSize") != desired.get("preferredSize")
    )
    if not drift:
        unchanged.append(desired["quality"]["name"])
        continue
    # PUT with the live entry as a base + the three size fields
    # overwritten. We keep weight/title/quality intact so Radarr
    # doesn't fight us on server-managed fields.
    payload = dict(live)
    payload["minSize"] = desired.get("minSize")
    payload["maxSize"] = desired.get("maxSize")
    payload["preferredSize"] = desired.get("preferredSize")
    http("PUT", "/qualitydefinition/%d" % live["id"], payload)
    updated.append(desired["quality"]["name"])

print(json.dumps({"updated": updated, "unchanged": unchanged, "missing": missing}))
PYEOF
)
    echo "${QD_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
# Quality definitions tend to be many (30+). Print a count summary
# rather than spelling each one to keep output readable.
def count(label, items):
    if items:
        print("  " + label + ": " + str(len(items)) + " (" + ", ".join(items[:3]) + ("..." if len(items) > 3 else "") + ")")
count("updated  ", s["updated"])
count("unchanged", s["unchanged"])
count("missing  ", s["missing"])
'
fi

# 16-S. Sonarr quality definitions (720p/1080p MB-per-minute caps).
#
# Sonarr twin of the Radarr block. Its quality-id space differs, so the
# fixture (sonarr_qualitydefinitions.json) was captured LIVE from this
# Sonarr and carries Sonarr's own ids. Match by quality.id (stable), PUT
# on drift. Pi 4B keeps leaner preferred sizes, selected by leaf NAME
# (Sonarr ids aren't known ahead of time, unlike Radarr's hardcoded map).
echo "Configuring Sonarr quality definitions..."
SONARR_QUALITY_FILE="${SCRIPT_DIR}/data/sonarr_qualitydefinitions.json"
if [[ ! -f "${SONARR_QUALITY_FILE}" ]]; then
    echo "  WARN: ${SONARR_QUALITY_FILE} not found — skipping."
elif [ "${SONARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Sonarr not reachable — skipping Sonarr quality-definition reconciliation (later runs will apply it)."
else
    SONARR_QD_SUMMARY=$(python3 - "${SONARR_QUALITY_FILE}" "${SONARR_KEY}" <<'PYEOF'
import json, sys, urllib.request
qd_path, api_key = sys.argv[1], sys.argv[2]
BASE = "http://localhost:8989/api/v3"

def http(method, path, body=None):
    headers = {"X-Api-Key": api_key, "Content-Type": "application/json"}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read()
        return json.loads(raw) if raw else None

with open(qd_path) as f:
    desired_qds = json.load(f)

# Board gate: fixture is the Pi 5 tuning; a Pi 4B keeps leaner preferred
# sizes. Select by leaf NAME (720p family → 25, 1080p family → 40).
CAP720  = {"HDTV-720p", "WEBDL-720p", "WEBRip-720p", "Bluray-720p"}
CAP1080 = {"HDTV-1080p", "WEBDL-1080p", "WEBRip-1080p", "Bluray-1080p"}
def pi_model():
    try:
        with open("/proc/device-tree/model", "rb") as f:
            return f.read().decode(errors="replace")
    except OSError:
        return ""

if pi_model().startswith("Raspberry Pi 4 "):
    for d in desired_qds:
        nm = d["quality"]["name"]
        if nm in CAP720:
            d["preferredSize"] = 25
        elif nm in CAP1080:
            d["preferredSize"] = 40

live_qds = http("GET", "/qualitydefinition") or []
live_by_quality_id = {q["quality"]["id"]: q for q in live_qds}

unchanged, updated, missing = [], [], []
for desired in desired_qds:
    qid = desired["quality"]["id"]
    if qid not in live_by_quality_id:
        missing.append(desired["quality"]["name"])
        continue
    live = live_by_quality_id[qid]
    drift = (
        live.get("minSize") != desired.get("minSize")
        or live.get("maxSize") != desired.get("maxSize")
        or live.get("preferredSize") != desired.get("preferredSize")
    )
    if not drift:
        unchanged.append(desired["quality"]["name"])
        continue
    payload = dict(live)
    payload["minSize"] = desired.get("minSize")
    payload["maxSize"] = desired.get("maxSize")
    payload["preferredSize"] = desired.get("preferredSize")
    http("PUT", "/qualitydefinition/%d" % live["id"], payload)
    updated.append(desired["quality"]["name"])

print(json.dumps({"updated": updated, "unchanged": unchanged, "missing": missing}))
PYEOF
)
    echo "${SONARR_QD_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
def count(label, items):
    if items:
        print("  " + label + ": " + str(len(items)) + " (" + ", ".join(items[:3]) + ("..." if len(items) > 3 else "") + ")")
count("updated  ", s["updated"])
count("unchanged", s["unchanged"])
count("missing  ", s["missing"])
'
fi

# 17. qBittorrent: `radarr` category.
#
# Categories in qBit are just label-+-savePath pairs but they let us
# segregate Radarr's torrents from anything else and apply per-
# category cleanup later (the Confirm-Remove flow in the kiosk
# walks Radarr history → asks qBit to delete every torrent ever
# associated with the movie). The fixture leaves savePath empty —
# qBit then defaults each torrent's location to the global
# default-save-path, which lives on /downloads inside the container.
#
# qBit's web API needs cookie auth (no API key), so we log in once
# into a temp cookie jar and reuse it for the createCategory call.
# The createCategory endpoint returns 409 on duplicate name — our
# idempotency check just fetches existing categories first and only
# POSTs missing ones.
echo "Configuring qBittorrent categories..."
QBIT_CATS_FILE="${SCRIPT_DIR}/data/qbit_categories.json"
if [[ ! -f "${QBIT_CATS_FILE}" ]]; then
    echo "  WARN: ${QBIT_CATS_FILE} not found — skipping."
else
    QBIT_PW="${QBIT_PW:-$(grep '^QBITTORRENT_ADMIN_PASSWORD=' "${ENV_FILE}" | cut -d= -f2-)}"
    if [[ -z "${QBIT_PW}" ]]; then
        echo "  WARN: QBITTORRENT_ADMIN_PASSWORD missing from ${ENV_FILE} — skipping."
    else
        QBIT_COOKIE="/tmp/qbit-setup-$$.cookie"
        # Login. SID cookie lives in $QBIT_COOKIE for downstream calls.
        # qBit returns plain "Ok." on success, "Fails." on bad creds.
        LOGIN_RESP=$(curl -fsS --connect-timeout 10 \
            -X POST -d "username=admin&password=${QBIT_PW}" \
            -c "${QBIT_COOKIE}" \
            http://localhost:8080/api/v2/auth/login || echo "fail")
        if [[ "${LOGIN_RESP}" != "Ok." ]]; then
            echo "  WARN: qBittorrent login failed (response: ${LOGIN_RESP}); skipping category setup."
            rm -f "${QBIT_COOKIE}"
        else
            QBIT_SUMMARY=$(python3 - "${QBIT_CATS_FILE}" "${QBIT_COOKIE}" <<'PYEOF'
import json, sys, urllib.request, urllib.parse
cats_path, cookie_path = sys.argv[1], sys.argv[2]
BASE = "http://localhost:8080/api/v2"

# Load the SID cookie out of the curl jar (Netscape cookie format).
# Format: domain<TAB>flag<TAB>path<TAB>secure<TAB>expiration<TAB>name<TAB>value
# qBit sends the cookie HttpOnly which curl annotates by prefixing the
# domain with `#HttpOnly_`. We split tab-fields on every line that has
# enough columns rather than filtering "#"-prefixed lines (which
# would skip the actual cookie row).
sid = ""
with open(cookie_path) as f:
    for line in f:
        if not line.strip() or line.startswith("# "):
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) >= 7 and parts[5] == "SID":
            sid = parts[6]
            break
if not sid:
    print(json.dumps({"error": "could not parse SID cookie"}))
    sys.exit(0)

def http(method, path, form=None):
    headers = {"Cookie": "SID=" + sid}
    data = None
    if form is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
        data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read()
        return raw.decode() if raw else ""

with open(cats_path) as f:
    desired_cats = json.load(f)

live_raw = http("GET", "/torrents/categories")
live_cats = json.loads(live_raw) if live_raw.strip() else {}

created, updated, unchanged = [], [], []
for desired in desired_cats:
    name = desired["name"]
    save_path = desired.get("savePath", "")
    if name in live_cats:
        if (live_cats[name].get("savePath", "") or "") == save_path:
            unchanged.append(name)
        else:
            http("POST", "/torrents/editCategory",
                 {"category": name, "savePath": save_path})
            updated.append(name)
    else:
        http("POST", "/torrents/createCategory",
             {"category": name, "savePath": save_path})
        created.append(name)

print(json.dumps({"created": created, "updated": updated, "unchanged": unchanged}))
PYEOF
)
            rm -f "${QBIT_COOKIE}"
            echo "${QBIT_SUMMARY}" | python3 -c '
import json, sys
s = json.loads(sys.stdin.read())
if s.get("error"):
    print("  WARN: " + s["error"])
else:
    def show(label, items):
        if items:
            print("  " + label + ": " + ", ".join(items))
    show("created  ", s["created"])
    show("updated  ", s["updated"])
    show("unchanged", s["unchanged"])
'
        fi
    fi
fi

# 17. Reconciliation: trigger Radarr to scan + import any completed
# torrents that landed before the download client was wired up.
#
# Edge case this fixes: if a prior setup_services.sh run failed to
# load radarr_downloadclients.json (e.g., the $(dirname "$0") bug we
# fixed earlier this session), Radarr had no download client and any
# torrents that completed in qBit were orphaned — at 100% in qBit but
# never imported into the library. With the download client now
# correctly configured, the on-completion auto-import path is live for
# FUTURE downloads, but EXISTING completed torrents in qBit (from before
# the client was configured) are invisible to Radarr until something
# triggers a scan. Without this step, the operator would see "downloaded"
# in qBit but "still downloading" in the kiosk library — confusing and
# unrecoverable without an SSH tunnel + Radarr web UI.
#
# RefreshMonitoredDownloads makes Radarr poll its now-configured download
# client and pick up any unknown completed torrents. DownloadedMoviesScan
# then walks the configured download directory and imports anything that
# matches a library movie. Both are no-ops on a clean setup with no
# orphaned downloads.
echo "Reconciling Radarr import state (catches completed-but-orphaned torrents)..."
RECONCILE_OK="✓"
curl -fsS -X POST -H "X-Api-Key: ${RADARR_KEY}" -H "Content-Type: application/json" \
    -d '{"name":"RefreshMonitoredDownloads"}' \
    "http://localhost:7878/api/v3/command" >/dev/null \
    || RECONCILE_OK="WARN"
sleep 5
curl -fsS -X POST -H "X-Api-Key: ${RADARR_KEY}" -H "Content-Type: application/json" \
    -d '{"name":"DownloadedMoviesScan","path":"/downloads/complete","importMode":"Auto"}' \
    "http://localhost:7878/api/v3/command" >/dev/null \
    || RECONCILE_OK="WARN"
if [[ "${RECONCILE_OK}" == "✓" ]]; then
    echo "  ✓ Radarr scan + import triggered (any orphaned downloads will reconcile within ~30s)"
else
    echo "  WARN: failed to trigger Radarr import scan; verify via web UI"
fi

# 17-S. Sonarr import reconcile — catch completed-but-orphaned episodes.
#
# Sonarr twin of the Radarr reconcile above. RefreshMonitoredDownloads
# polls the now-configured download client; DownloadedEpisodesScan walks
# the fixed /downloads/complete container mount (same path Radarr's
# reconcile above uses) and imports anything matching a monitored series.
# The path is a fixed container mount, not something chosen per-box at
# provisioning time. Both commands no-op on a clean setup with no
# orphaned downloads.
echo "Reconciling Sonarr import state (catches completed-but-orphaned episodes)..."
if [ "${SONARR_READY:-0}" -ne 1 ]; then
    echo "  WARN: Sonarr not reachable — later runs will apply it."
else
    SONARR_RECONCILE_OK="✓"
    curl -fsS -X POST -H "X-Api-Key: ${SONARR_KEY}" -H "Content-Type: application/json" \
        -d '{"name":"RefreshMonitoredDownloads"}' \
        "http://localhost:8989/api/v3/command" >/dev/null \
        || SONARR_RECONCILE_OK="WARN"
    sleep 5
    curl -fsS -X POST -H "X-Api-Key: ${SONARR_KEY}" -H "Content-Type: application/json" \
        -d '{"name":"DownloadedEpisodesScan","path":"/downloads/complete","importMode":"Auto"}' \
        "http://localhost:8989/api/v3/command" >/dev/null \
        || SONARR_RECONCILE_OK="WARN"
    if [[ "${SONARR_RECONCILE_OK}" == "✓" ]]; then
        echo "  ✓ Sonarr scan + import triggered (any orphaned downloads reconcile within ~30s)"
    else
        echo "  WARN: failed to trigger Sonarr import scan; verify via web UI"
    fi
fi

# 18. Run smoke test. Hard-asserts every link in the chain (indexers,
# download client, quality profile, custom formats, qBit auth, kiosk
# password env var, live indexer search). Non-zero exit means setup
# regressed somewhere; the operator sees the failures inline instead
# of weeks later when an empty Movies tab gets reported.
#
# We DON'T fail setup_services.sh itself on smoke-test failure — the
# services may still be coming up (Prowlarr lazy-loads indexers on
# first search, Radarr's app-sync runs on a 60s timer). The exit code
# is captured but only printed; the credentials block always runs so
# the operator at least gets API keys + login info if they need to
# debug interactively.
if [[ -x "${SCRIPT_DIR}/verify_services.sh" ]]; then
    echo ""
    echo "Running smoke test..."
    if "${SCRIPT_DIR}/verify_services.sh"; then
        SMOKE_TEST_OK=1
    else
        SMOKE_TEST_OK=0
        echo ""
        echo "⚠  Smoke test reported failures. Credentials below — debug interactively."
        echo "   Re-run: ${SCRIPT_DIR}/verify_services.sh"
    fi
else
    SMOKE_TEST_OK=-1
    echo "  (verify_services.sh not found or not executable — skipping smoke test)"
fi

# 9. Print credentials to operator
cat <<EOF

======================================================================
Services initialized. SAVE THESE CREDENTIALS in a password manager:

All service UIs are bound to 127.0.0.1 on this Pi (zero LAN attack
surface). Admin access requires an SSH tunnel from a trusted device:

  ssh -L 7878:localhost:7878 \\
      -L 9696:localhost:9696 \\
      -L 8989:localhost:8989 \\
      -L 8080:localhost:8080 \\
      -L 8191:localhost:8191 \\
      magic@magicpi.local

Then from that device:

Radarr    → http://localhost:7878 (via SSH tunnel only — see operator guide)
            API key: ${RADARR_KEY}

Sonarr    → http://localhost:8989 (via SSH tunnel only — see operator guide)
            API key: ${SONARR_KEY}

Prowlarr  → http://localhost:9696 (via SSH tunnel only — see operator guide)
            API key: ${PROWLARR_KEY}

qBittorrent → http://localhost:8080 (via SSH tunnel only — see operator guide)
            Username: admin
            Password: $(grep QBITTORRENT_ADMIN_PASSWORD "${ENV_FILE}" | cut -d= -f2)
            (Localhost auth bypass DISABLED; this password is the only way in)

NEXT STEPS:
  Indexers, quality profile, qBit download client, and Custom Formats
  are configured automatically. The kiosk Media Browser is ready as
  soon as ENABLE_MEDIA_BROWSER is on.

  Re-run smoke test anytime: ${SCRIPT_DIR}/verify_services.sh
======================================================================
EOF
