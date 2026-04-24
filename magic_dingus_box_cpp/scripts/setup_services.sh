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

SERVICES_DIR="/opt/magic_dingus_box/services"
STORAGE_ROOT="${STORAGE_ROOT:-/mnt/ssd}"
ENV_FILE="${SERVICES_DIR}/.env"

echo "=== Media Browser V2 service setup ==="

# 1. Docker install check
if ! command -v docker &>/dev/null; then
    echo "Installing Docker..."
    curl -fsSL https://get.docker.com | sh
    sudo usermod -aG docker "$(whoami)"
    echo "Docker installed. You may need to log out and back in for group changes."
fi

if ! docker compose version &>/dev/null; then
    echo "ERROR: docker compose plugin required. Install docker-compose-plugin."
    exit 1
fi

# 2. Storage layout
echo "Creating storage layout at ${STORAGE_ROOT}..."
sudo mkdir -p "${STORAGE_ROOT}"/{downloads/incomplete,downloads/complete,library/Movies,backups}
sudo chown -R "$(whoami):$(whoami)" "${STORAGE_ROOT}"

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
QBITTORRENT_ADMIN_PASSWORD=${QBIT_PW}
EOF
    chmod 600 "${ENV_FILE}"
    echo "Generated .env with random qBittorrent password."
else
    echo ".env already exists — preserving existing secrets."
fi

# 4. Start stack
cd "${SERVICES_DIR}"
echo "Starting Docker stack..."
docker compose up -d

# 5. Wait for services to initialize their configs
echo "Waiting for services to finish first-time init (60s)..."
sleep 60

# 6. Extract auto-generated API keys from Radarr/Prowlarr configs
RADARR_KEY=$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/radarr/config.xml" 2>/dev/null || echo "")
PROWLARR_KEY=$(grep -oP '(?<=<ApiKey>)[^<]+' "${SERVICES_DIR}/config/prowlarr/config.xml" 2>/dev/null || echo "")

if [ -z "${RADARR_KEY}" ] || [ -z "${PROWLARR_KEY}" ]; then
    echo "WARNING: Could not extract API keys. Services may still be starting."
    echo "Re-run this script in a minute, or extract them manually from config.xml files."
    exit 1
fi

# 7. Write keys back to .env
sed -i "s|RADARR_API_KEY=.*|RADARR_API_KEY=${RADARR_KEY}|" "${ENV_FILE}"
sed -i "s|PROWLARR_API_KEY=.*|PROWLARR_API_KEY=${PROWLARR_KEY}|" "${ENV_FILE}"

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

# 9. Print credentials to operator
cat <<EOF

======================================================================
Services initialized. SAVE THESE CREDENTIALS in a password manager:

Radarr    → http://$(hostname -I | awk '{print $1}'):7878
            API key: ${RADARR_KEY}

Prowlarr  → http://$(hostname -I | awk '{print $1}'):9696
            API key: ${PROWLARR_KEY}

qBittorrent → http://$(hostname -I | awk '{print $1}'):8080
            Username: admin
            Password: $(grep QBITTORRENT_ADMIN_PASSWORD "${ENV_FILE}" | cut -d= -f2)
            (Log in and change the default via qBit web UI immediately)

NEXT STEPS:
  1. Log into qBittorrent and change the admin password
  2. In Prowlarr: add at least one indexer (legal content only)
  3. In Radarr: connect to Prowlarr (auto-discovered) and qBittorrent
  4. Kiosk Media Browser will now talk to Radarr once ENABLE_MEDIA_BROWSER is on
======================================================================
EOF
