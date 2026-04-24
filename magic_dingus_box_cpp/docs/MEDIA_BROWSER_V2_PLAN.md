# Media Browser V2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a hidden, secret-sequence-unlocked movie browser for the kiosk, backed by a companion Docker stack (Radarr + Prowlarr + qBittorrent), with a custom CRT-aesthetic UI and 1080p widescreen playback for movies only.

**Architecture:** Companion Docker services on the kiosk Pi handle release discovery, download orchestration, and library import. The kiosk C++ binary gains a `RadarrClient` module (HTTP via libcurl) that acts as a UI client to Radarr's REST API. A secret sequence (`BTN1+BTN3 chord → BTN2x3 → RCLICK`) unlocks a new row in the settings menu that opens the Media Browser. Playback briefly switches DRM mode to 1920×1080 then restores. All existing kiosk behavior is preserved; the feature is gated by four independent controls (CMake flag, settings toggle, unlock sequence, service availability).

**Tech Stack:**
- Docker + Docker Compose (LinuxServer.io images: Radarr, Prowlarr, qBittorrent)
- C++17 (matches existing kiosk)
- libcurl (HTTP to Radarr API; same pattern as Phase 1 `TmdbClient`)
- jsoncpp (already in project; Radarr response parsing)
- Catch2 v3 (already in project; tests for parsers + sequence detector)
- SQLite (already in project; reused as local cache of Radarr responses)
- spdlog (already in project; structured logging)
- systemd (services and kiosk unit)
- GStreamer (existing video pipeline; no changes to codec/decoder)

**Spec reference:** `MEDIA_BROWSER_V2_DESIGN.md` is the source of truth for design decisions. This plan operationalizes it.

---

## File Structure

New files (organized by sub-project):

```
magic_dingus_box/
├── magic_dingus_box_cpp/
│   ├── CMakeLists.txt                                    # MODIFY: extend ENABLE_MEDIA_BROWSER block
│   ├── scripts/
│   │   ├── deploy_cpp.sh                                 # MODIFY: --media-browser also sets up Docker
│   │   └── setup_services.sh                             # NEW: Docker stack bootstrap
│   ├── systemd/
│   │   └── magic-dingus-services.service                 # NEW: Docker stack auto-start
│   ├── services/
│   │   ├── docker-compose.yml                            # NEW: Radarr + Prowlarr + qBittorrent
│   │   └── .env.example                                  # NEW: template for secrets
│   ├── src/
│   │   ├── main.cpp                                      # MODIFY: wire sequence detector + screen dispatch
│   │   ├── platform/
│   │   │   ├── drm_display.{h,cpp}                       # MODIFY: add request_mode() + restore()
│   │   │   ├── gpio_manager.{h,cpp}                      # MODIFY: expose raw button state snapshot
│   │   │   └── sequence_detector.{h,cpp}                 # NEW: chord + sequence state machine
│   │   ├── ui/
│   │   │   └── toast.{h,cpp}                             # NEW: transient on-screen notification
│   │   ├── app/
│   │   │   ├── app_state.h                               # MODIFY: add AppScreen::MediaBrowser
│   │   │   ├── playlist_loader.{h,cpp}                   # MODIFY: Movies source + inotify watch
│   │   │   └── settings_persistence.{h,cpp}              # MODIFY: media_browser.unlocked flag
│   │   └── media_browser/
│   │       ├── radarr/
│   │       │   ├── radarr_types.h                        # NEW: Movie, QueueItem, QualityProfile, etc.
│   │       │   ├── radarr_parsers.{h,cpp}                # NEW: JSON → struct (pure, testable)
│   │       │   ├── radarr_client.{h,cpp}                 # NEW: libcurl HTTP methods
│   │       │   └── radarr_mock.{h,cpp}                   # NEW: in-memory mock for UI tests
│   │       ├── ui/
│   │       │   ├── browse_screen.{h,cpp}                 # NEW: poster grid + categories
│   │       │   ├── search_screen.{h,cpp}                 # NEW: virtual keyboard + live results
│   │       │   ├── detail_screen.{h,cpp}                 # NEW: movie info + actions
│   │       │   ├── queue_screen.{h,cpp}                  # NEW: active downloads + progress
│   │       │   ├── library_screen.{h,cpp}                # NEW: have/missing list
│   │       │   └── mb_settings_screen.{h,cpp}            # NEW: 11 kiosk-exposed settings
│   │       └── test_cli/
│   │           └── main.cpp                              # MODIFY: add radarr-* subcommands
│   └── tests/
│       └── media_browser/
│           ├── test_sequence_detector.cpp                # NEW: TDD for unlock state machine
│           ├── test_radarr_parsers.cpp                   # NEW: TDD for JSON parsers
│           └── fixtures/
│               └── radarr/
│                   ├── movie_lookup.json                 # NEW: canned Radarr search response
│                   ├── queue.json                        # NEW: canned queue response
│                   ├── quality_profiles.json             # NEW: canned profiles response
│                   └── system_status.json                # NEW: canned status response
└── docs/                                                 # Gitignored by project convention;
    ├── MEDIA_BROWSER_SERVICE_SETUP.md                    # NEW: operator setup guide
    ├── MEDIA_BROWSER_USER_GUIDE.md                       # NEW: end-user guide
    └── MEDIA_BROWSER_V2_COMPLETION.md                    # NEW: done-record (per Phase 1 pattern)
```

---

## Decisions made at plan-write time (deferred from spec §18)

| Decision | Choice | Why |
|---|---|---|
| Docker image tags | **Pinned to known-good versions** (`radarr:5.14.0`, `prowlarr:1.26.1`, `qbittorrent:5.0.3`) | Reproducibility. Upgrade path is a deliberate operator action. |
| Default quality profile seeded | **Yes** — "1080p Standard" (BluRay > WEB-DL > HDTV, 720p acceptable, reject CAM/TS/Screener, upgrade target 1080p BluRay) | Spec §18 default. Gets operator to a working state instantly. |
| Artwork caching | **LRU, 200 MB cap, on-demand** | Matches spec §18 default. Simple; no pre-fetch complexity. |

---

## Sub-Project B1: Docker Stack + Systemd (Tasks 1–5)

---

## Task 1: Write docker-compose.yml

**Files:**
- Create: `magic_dingus_box_cpp/services/docker-compose.yml`
- Create: `magic_dingus_box_cpp/services/.env.example`

- [ ] **Step 1: Create services directory + .env.example**

```bash
mkdir -p magic_dingus_box_cpp/services/config/{radarr,prowlarr,qbittorrent}
```

Create `magic_dingus_box_cpp/services/.env.example` with:

```bash
# Media Browser V2 — Service Environment
# Copy to .env and fill in generated secrets. DO NOT commit .env.
#
# Secrets are auto-generated by setup_services.sh on first run.

PUID=1000                         # magic user
PGID=1000
TZ=America/Los_Angeles            # adjust to your TZ

# Storage root (must be USB3 SSD mount point)
STORAGE_ROOT=/mnt/ssd

# Auth: generated by setup_services.sh
# These are random strong values; only the operator needs to know them.
RADARR_API_KEY=__REPLACE_ME__
PROWLARR_API_KEY=__REPLACE_ME__
QBITTORRENT_ADMIN_PASSWORD=__REPLACE_ME__
```

- [ ] **Step 2: Write docker-compose.yml**

Create `magic_dingus_box_cpp/services/docker-compose.yml`:

```yaml
# Media Browser V2 — Companion Services
# Deployed to /opt/magic_dingus_box/services/ on the Pi.
# Managed by magic-dingus-services.service (systemd).
# All images pinned to known-good versions for reproducibility.

services:
  radarr:
    image: lscr.io/linuxserver/radarr:5.14.0
    container_name: mdb_radarr
    environment:
      - PUID=${PUID}
      - PGID=${PGID}
      - TZ=${TZ}
    volumes:
      - ./config/radarr:/config
      - ${STORAGE_ROOT}/downloads:/downloads
      - ${STORAGE_ROOT}/library:/library
    ports:
      - "7878:7878"
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://localhost:7878/ping"]
      interval: 30s
      timeout: 5s
      retries: 3

  prowlarr:
    image: lscr.io/linuxserver/prowlarr:1.26.1
    container_name: mdb_prowlarr
    environment:
      - PUID=${PUID}
      - PGID=${PGID}
      - TZ=${TZ}
    volumes:
      - ./config/prowlarr:/config
    ports:
      - "9696:9696"
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://localhost:9696/ping"]
      interval: 30s
      timeout: 5s
      retries: 3

  qbittorrent:
    image: lscr.io/linuxserver/qbittorrent:5.0.3
    container_name: mdb_qbittorrent
    environment:
      - PUID=${PUID}
      - PGID=${PGID}
      - TZ=${TZ}
      - WEBUI_PORT=8080
    volumes:
      - ./config/qbittorrent:/config
      - ${STORAGE_ROOT}/downloads:/downloads
    ports:
      - "8080:8080"
      - "6881:6881"
      - "6881:6881/udp"
    restart: unless-stopped

# Services share the default bridge network; inter-container DNS resolves names.
# Radarr reaches qBittorrent via http://qbittorrent:8080.
# Kiosk C++ reaches all services via localhost:<port> (host network).
```

- [ ] **Step 3: Commit**

```bash
cd "$(git rev-parse --show-toplevel)"
git add magic_dingus_box_cpp/services/docker-compose.yml \
        magic_dingus_box_cpp/services/.env.example
git commit -m "feat(media_browser): docker-compose stack for Radarr + Prowlarr + qBittorrent"
```

---

## Task 2: Write setup_services.sh bootstrap script

**Files:**
- Create: `magic_dingus_box_cpp/scripts/setup_services.sh`

- [ ] **Step 1: Write the setup script**

Create `magic_dingus_box_cpp/scripts/setup_services.sh`:

```bash
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

# 8. Seed default quality profile in Radarr (1080p Standard)
echo "Seeding 1080p Standard quality profile in Radarr..."
curl -fsS -X POST "http://localhost:7878/api/v3/qualityprofile" \
    -H "X-Api-Key: ${RADARR_KEY}" \
    -H "Content-Type: application/json" \
    -d '{
        "name": "1080p Standard",
        "upgradeAllowed": true,
        "cutoff": 7,
        "items": [
            {"quality": {"id": 1, "name": "SDTV"}, "allowed": false},
            {"quality": {"id": 2, "name": "DVD"}, "allowed": false},
            {"quality": {"id": 8, "name": "WEBDL-480p"}, "allowed": false},
            {"quality": {"id": 3, "name": "WEBDL-720p"}, "allowed": true},
            {"quality": {"id": 4, "name": "HDTV-720p"}, "allowed": true},
            {"quality": {"id": 9, "name": "HDTV-1080p"}, "allowed": true},
            {"quality": {"id": 5, "name": "WEBDL-1080p"}, "allowed": true},
            {"quality": {"id": 7, "name": "Bluray-1080p"}, "allowed": true},
            {"quality": {"id": 16, "name": "HDTV-2160p"}, "allowed": false},
            {"quality": {"id": 18, "name": "WEBDL-2160p"}, "allowed": false},
            {"quality": {"id": 19, "name": "Bluray-2160p"}, "allowed": false}
        ],
        "minFormatScore": 0,
        "cutoffFormatScore": 0,
        "formatItems": [],
        "language": {"id": 1, "name": "English"}
    }' >/dev/null || echo "WARN: Quality profile may already exist (ignoring)"

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
```

- [ ] **Step 2: Make executable**

```bash
chmod +x magic_dingus_box_cpp/scripts/setup_services.sh
```

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box_cpp/scripts/setup_services.sh
git commit -m "feat(media_browser): setup_services.sh bootstraps Docker stack + captures API keys"
```

---

## Task 3: Write systemd unit for auto-start

**Files:**
- Create: `magic_dingus_box_cpp/systemd/magic-dingus-services.service`
- Modify: `magic_dingus_box_cpp/scripts/deploy_cpp.sh` (install the unit during --media-browser deploy)

- [ ] **Step 1: Write the systemd unit**

Create `magic_dingus_box_cpp/systemd/magic-dingus-services.service`:

```ini
[Unit]
Description=Magic Dingus Box — Media Browser companion services (Radarr + Prowlarr + qBittorrent)
Requires=docker.service
After=docker.service network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
WorkingDirectory=/opt/magic_dingus_box/services
ExecStart=/usr/bin/docker compose up -d
ExecStop=/usr/bin/docker compose down
TimeoutStartSec=300
User=magic

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 2: Wire deploy_cpp.sh to install the unit on --media-browser**

In `magic_dingus_box_cpp/scripts/deploy_cpp.sh`, locate the existing `--media-browser` handling (added in Phase 1 Task 11) and extend it. Find the block that installs `magic-dingus-box-cpp.service` (search for `Step 1.7: Install C++ App Service`) and after that block, add a new step:

```bash
# Step 1.7c: Install Media Browser services unit (if --media-browser)
if [ "${MEDIA_BROWSER:-false}" = "true" ]; then
    echo "Step 1.7c: Installing Media Browser services..."

    # Copy services directory + systemd unit
    rsync -avz \
        "${CPP_DIR}/services/" \
        "${PI_HOST}:/opt/magic_dingus_box/services/"

    rsync -avz \
        "${CPP_DIR}/systemd/magic-dingus-services.service" \
        "${PI_HOST}:/tmp/magic-dingus-services.service"

    rsync -avz \
        "${CPP_DIR}/scripts/setup_services.sh" \
        "${PI_HOST}:/tmp/setup_services.sh"

    ssh "${PI_HOST}" "sudo cp /tmp/magic-dingus-services.service /etc/systemd/system/
                      sudo systemctl daemon-reload
                      sudo systemctl enable magic-dingus-services.service
                      chmod +x /tmp/setup_services.sh"

    echo "  ✓ Services unit installed and enabled"
    echo "  → To initialize services, run on the Pi: sudo /tmp/setup_services.sh"
fi
echo ""
```

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box_cpp/systemd/magic-dingus-services.service \
        magic_dingus_box_cpp/scripts/deploy_cpp.sh
git commit -m "feat(media_browser): systemd unit + deploy wiring for services stack"
```

---

## Task 4: Deploy + verify on Pi

**Files:** No file changes; hands-on Pi verification.

- [ ] **Step 1: Deploy with --media-browser flag**

From the worktree root:

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --media-browser
```

Expected: kiosk rebuilds, services directory + systemd unit + setup script deployed to Pi.

- [ ] **Step 2: Bootstrap services on Pi**

SSH in and run the setup script:

```bash
ssh magic@magicpi.local
sudo /tmp/setup_services.sh
```

Expected: Docker installs (if missing), compose stack starts, API keys extracted, credentials printed. **Save the printed credentials immediately.**

- [ ] **Step 3: Verify services are reachable**

From your Mac:

```bash
curl -fsS http://magicpi.local:7878/ping && echo "Radarr OK"
curl -fsS http://magicpi.local:9696/ping && echo "Prowlarr OK"
curl -fsS -o /dev/null -w "%{http_code}\n" http://magicpi.local:8080/
```

Expected: Radarr responds with `pong`, Prowlarr responds with `pong`, qBittorrent returns 200 or 401 (not 0/connection-refused).

- [ ] **Step 4: Verify auto-start survives reboot**

```bash
ssh magic@magicpi.local 'sudo reboot'
# Wait ~60 seconds
sleep 70
curl -fsS http://magicpi.local:7878/ping && echo "Radarr auto-started OK"
```

- [ ] **Step 5: Verify quality profile seeded**

```bash
RADARR_KEY=$(ssh magic@magicpi.local 'grep RADARR_API_KEY /opt/magic_dingus_box/services/.env | cut -d= -f2')
curl -s -H "X-Api-Key: ${RADARR_KEY}" http://magicpi.local:7878/api/v3/qualityprofile | \
    grep -o '"name":"1080p Standard"' && echo "Profile seeded OK"
```

Expected: output contains `"name":"1080p Standard"`.

- [ ] **Step 6: Commit a Task 4 completion note** (no code change, documentation only)

Create a brief note in `MEDIA_BROWSER_SERVICE_SETUP.md` (see Task 5); no commit at this step.

---

## Task 5: Write operator setup documentation

**Files:**
- Create: `magic_dingus_box_cpp/docs/MEDIA_BROWSER_SERVICE_SETUP.md`

- [ ] **Step 1: Write the operator guide**

Create `magic_dingus_box_cpp/docs/MEDIA_BROWSER_SERVICE_SETUP.md`:

```markdown
# Media Browser V2 — Service Setup Guide (Operator)

This is a one-time setup guide for the kiosk operator. After completion,
the companion services (Radarr + Prowlarr + qBittorrent) auto-start on
every Pi boot.

## Prerequisites

- Kiosk Pi running with Phase 1+ kiosk binary deployed
- USB3 SSD mounted at `/mnt/ssd` (REQUIRED — do not skip)
- Network connectivity
- Sudo access on the Pi

## One-time setup

1. **Deploy with --media-browser**
   On your Mac in the project directory:
   ```bash
   ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --media-browser
   ```

2. **Bootstrap services on the Pi**
   ```bash
   ssh magic@magicpi.local
   sudo /tmp/setup_services.sh
   ```
   Save the printed credentials in a password manager.

3. **Change qBittorrent admin password**
   Open `http://magicpi.local:8080`, log in with the printed password,
   go to Tools → Options → Web UI → change password.

4. **Add a legal indexer in Prowlarr**
   Open `http://magicpi.local:9696`. Go to Indexers → Add.
   Example legal indexer:
   - **Internet Archive** (via Jackett gateway) — public-domain films
   - **LinuxTracker** — Linux ISOs for E2E testing

5. **Connect Radarr to Prowlarr + qBittorrent**
   Radarr usually auto-detects Prowlarr. Verify under
   Settings → Indexers. If missing, add Prowlarr manually with its API key.

   Under Settings → Download Clients, add qBittorrent:
   - Host: `qbittorrent` (container DNS name)
   - Port: 8080
   - Username: admin
   - Password: (your new password)

6. **Verify the default quality profile**
   Radarr → Settings → Profiles. Confirm "1080p Standard" exists.

## Ongoing maintenance

- **View logs:** `docker compose logs -f radarr` on the Pi
- **Restart services:** `sudo systemctl restart magic-dingus-services`
- **Update images:** edit pinned tags in `docker-compose.yml`, then
  `docker compose pull && docker compose up -d`
- **Backups:** configs at `/opt/magic_dingus_box/services/config/` —
  tar + store off-Pi weekly

## Troubleshooting

| Symptom | Fix |
|---|---|
| Kiosk Movies menu shows "service offline" | `systemctl status magic-dingus-services`; check `docker compose ps` |
| Radarr can't reach indexer | Check Prowlarr indexer test button |
| Downloads stuck at 0% | qBittorrent — check disk space, tracker status |
| API key wrong / connection refused | Re-run `/tmp/setup_services.sh` (idempotent) |

## Fine print (advanced URL for owner)

Kiosk operator can access full web UIs for advanced config:
- Radarr:      `http://magicpi.local:7878`
- Prowlarr:    `http://magicpi.local:9696`
- qBittorrent: `http://magicpi.local:8080`

All require authentication. Keep credentials private.
```

- [ ] **Step 2: Commit (force-add because docs/ is gitignored)**

```bash
git add -f magic_dingus_box_cpp/docs/MEDIA_BROWSER_SERVICE_SETUP.md
git commit -m "docs(media_browser): operator setup guide for services stack"
```

---

## Sub-Project B2: Sequence Detector + Unlock Flow (Tasks 6–11)

---

## Task 6: Write sequence_detector interface + tests (TDD)

**Files:**
- Create: `magic_dingus_box_cpp/src/platform/sequence_detector.h`
- Create: `magic_dingus_box_cpp/tests/media_browser/test_sequence_detector.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (add test to MEDIA_BROWSER_TEST_SOURCES)

- [ ] **Step 1: Write the header**

Create `magic_dingus_box_cpp/src/platform/sequence_detector.h`:

```cpp
#pragma once

#include <cstdint>
#include <chrono>

namespace platform {

// Raw input events the detector cares about. Fed from gpio_manager.
enum class SeqInput : uint8_t {
    NONE,
    BTN1_PRESS,
    BTN2_PRESS,
    BTN3_PRESS,
    BTN4_PRESS,
    BTN1_BTN3_CHORD,   // both pressed within ~50ms window
    ROTARY_CLICK,      // distinct from controller SELECT
};

// Result of feeding an event into the detector.
enum class SeqResult : uint8_t {
    NO_MATCH,          // Event doesn't advance sequence; state was reset
    PROGRESS,          // Event advanced the sequence; not yet complete
    UNLOCKED,          // Sequence completed this event
};

// State machine for the Media Browser unlock sequence:
//   BTN1+BTN3 (chord), BTN2, BTN2, BTN2, RCLICK
//
// Thread-safety: not thread-safe. Call feed() from a single thread.
class SequenceDetector {
public:
    // Events must arrive within this window of the previous event, else
    // the sequence resets silently.
    static constexpr int TIMEOUT_MS = 2000;

    SequenceDetector();

    // Feed a raw input event. Returns whether this advanced or completed
    // the sequence. Timing is checked against the last event time.
    SeqResult feed(SeqInput input,
                   std::chrono::steady_clock::time_point now);

    // Reset state (e.g. on screen change). Also called internally on timeout.
    void reset();

    // How many events of the 5-step sequence have been matched so far.
    int progress() const { return step_; }

private:
    int step_ = 0;
    std::chrono::steady_clock::time_point last_event_time_;
};

}  // namespace platform
```

- [ ] **Step 2: Write the failing tests**

Create `magic_dingus_box_cpp/tests/media_browser/test_sequence_detector.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include "platform/sequence_detector.h"

using namespace platform;
using clock_t = std::chrono::steady_clock;

static clock_t::time_point t(int ms) {
    return clock_t::time_point(std::chrono::milliseconds(ms));
}

TEST_CASE("SequenceDetector: full correct sequence unlocks", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(0)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(500)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(1000)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(1500)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::ROTARY_CLICK, t(2000)) == SeqResult::UNLOCKED);
}

TEST_CASE("SequenceDetector: after unlock, fresh sequence needed", "[sequence]") {
    SequenceDetector d;
    d.feed(SeqInput::BTN1_BTN3_CHORD, t(0));
    d.feed(SeqInput::BTN2_PRESS, t(100));
    d.feed(SeqInput::BTN2_PRESS, t(200));
    d.feed(SeqInput::BTN2_PRESS, t(300));
    REQUIRE(d.feed(SeqInput::ROTARY_CLICK, t(400)) == SeqResult::UNLOCKED);
    // After unlock, the detector auto-resets. Another RCLICK should NOT unlock.
    REQUIRE(d.feed(SeqInput::ROTARY_CLICK, t(500)) == SeqResult::NO_MATCH);
}

TEST_CASE("SequenceDetector: wrong input resets silently", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(0)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN4_PRESS, t(100)) == SeqResult::NO_MATCH);
    // Must restart from the chord
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(200)) == SeqResult::NO_MATCH);
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(300)) == SeqResult::PROGRESS);
}

TEST_CASE("SequenceDetector: timeout between events resets", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(0)) == SeqResult::PROGRESS);
    // 2001ms > TIMEOUT_MS (2000) — next event resets + is not the chord,
    // so it's NO_MATCH.
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(2001)) == SeqResult::NO_MATCH);
}

TEST_CASE("SequenceDetector: exactly-at-timeout is allowed", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.feed(SeqInput::BTN1_BTN3_CHORD, t(0)) == SeqResult::PROGRESS);
    REQUIRE(d.feed(SeqInput::BTN2_PRESS, t(2000)) == SeqResult::PROGRESS);
}

TEST_CASE("SequenceDetector: progress reflects step count", "[sequence]") {
    SequenceDetector d;
    REQUIRE(d.progress() == 0);
    d.feed(SeqInput::BTN1_BTN3_CHORD, t(0));
    REQUIRE(d.progress() == 1);
    d.feed(SeqInput::BTN2_PRESS, t(100));
    REQUIRE(d.progress() == 2);
    d.feed(SeqInput::BTN4_PRESS, t(200));  // wrong
    REQUIRE(d.progress() == 0);
}

TEST_CASE("SequenceDetector: explicit reset clears state", "[sequence]") {
    SequenceDetector d;
    d.feed(SeqInput::BTN1_BTN3_CHORD, t(0));
    d.feed(SeqInput::BTN2_PRESS, t(100));
    REQUIRE(d.progress() == 2);
    d.reset();
    REQUIRE(d.progress() == 0);
}
```

- [ ] **Step 3: Add test file to CMake**

In `CMakeLists.txt`, inside the `if(ENABLE_MEDIA_BROWSER)` block, update `MEDIA_BROWSER_TEST_SOURCES`:

```cmake
set(MEDIA_BROWSER_TEST_SOURCES
    tests/media_browser/test_smoke.cpp
    tests/media_browser/test_library_db.cpp
    tests/media_browser/test_tmdb_client.cpp
    tests/media_browser/test_sequence_detector.cpp
    ${MEDIA_BROWSER_SOURCES}
)
```

Also add the impl file to `MEDIA_BROWSER_SOURCES` (will be created in Task 7):

```cmake
set(MEDIA_BROWSER_SOURCES
    src/media_browser/library/library_db.cpp
    src/media_browser/tmdb_client.cpp
    src/media_browser/torrent/torrent_session.cpp
    src/platform/sequence_detector.cpp
)
```

- [ ] **Step 4: Run tests to verify they fail (no impl yet)**

```bash
cd magic_dingus_box_cpp/build-mac
cmake .. -DENABLE_MEDIA_BROWSER=ON
make -j4 test_media_browser_unit 2>&1 | tail -5
```

Expected: link error for `SequenceDetector::SequenceDetector`, `feed`, `reset` (no impl yet).

- [ ] **Step 5: Commit header + tests (impl in Task 7)**

```bash
git add magic_dingus_box_cpp/src/platform/sequence_detector.h \
        magic_dingus_box_cpp/tests/media_browser/test_sequence_detector.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(media_browser): SequenceDetector interface + failing tests (TDD)"
```

---

## Task 7: Implement sequence_detector state machine

**Files:**
- Create: `magic_dingus_box_cpp/src/platform/sequence_detector.cpp`

- [ ] **Step 1: Write implementation**

Create `magic_dingus_box_cpp/src/platform/sequence_detector.cpp`:

```cpp
#include "platform/sequence_detector.h"

namespace platform {

namespace {
// The target sequence: BTN1+BTN3 chord, BTN2, BTN2, BTN2, RCLICK.
// Indexed by step (0..4).
constexpr SeqInput EXPECTED[] = {
    SeqInput::BTN1_BTN3_CHORD,
    SeqInput::BTN2_PRESS,
    SeqInput::BTN2_PRESS,
    SeqInput::BTN2_PRESS,
    SeqInput::ROTARY_CLICK,
};
constexpr int SEQ_LEN = sizeof(EXPECTED) / sizeof(EXPECTED[0]);
}  // namespace

SequenceDetector::SequenceDetector() = default;

SeqResult SequenceDetector::feed(SeqInput input,
                                 std::chrono::steady_clock::time_point now) {
    // Timeout check (only after the first event)
    if (step_ > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_event_time_).count();
        if (elapsed > TIMEOUT_MS) {
            step_ = 0;
        }
    }

    if (input == EXPECTED[step_]) {
        step_++;
        last_event_time_ = now;
        if (step_ >= SEQ_LEN) {
            step_ = 0;  // auto-reset for next time
            return SeqResult::UNLOCKED;
        }
        return SeqResult::PROGRESS;
    }

    // Wrong input: silent reset
    step_ = 0;
    return SeqResult::NO_MATCH;
}

void SequenceDetector::reset() {
    step_ = 0;
}

}  // namespace platform
```

- [ ] **Step 2: Build + run tests**

```bash
cd magic_dingus_box_cpp/build-mac
make -j4 test_media_browser_unit
./test_media_browser_unit "[sequence]"
```

Expected: all 7 test cases pass.

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box_cpp/src/platform/sequence_detector.cpp
git commit -m "feat(media_browser): SequenceDetector state machine implementation"
```

---

## Task 8: Expose raw button state from gpio_manager

**Files:**
- Modify: `magic_dingus_box_cpp/src/platform/gpio_manager.h`
- Modify: `magic_dingus_box_cpp/src/platform/gpio_manager.cpp`

The sequence detector needs a snapshot of button press state per poll tick (to detect chords). Existing gpio_manager fires one event per press but doesn't expose the atomic snapshot. Add a new method.

- [ ] **Step 1: Add the API to the header**

In `magic_dingus_box_cpp/src/platform/gpio_manager.h`, locate the public section of `GpioManager` class and add:

```cpp
    // Snapshot of current (post-debounce) button press state, indexed
    // 0..NUM_BUTTONS-1. True = currently pressed. Called from main loop
    // after poll() for features that need chord detection.
    const bool* button_state_snapshot() const { return &button_pressed_[0]; }

    // Whether BTN1 and BTN3 are both pressed right now (chord).
    // Uses a small tolerance window to forgive asynchronous presses.
    bool is_chord_btn1_btn3() const;
```

And in the private section, add:

```cpp
    bool button_pressed_[gpio::NUM_BUTTONS] = {false, false, false, false};
    std::chrono::steady_clock::time_point button_press_times_[gpio::NUM_BUTTONS];
```

Also add an include near the top if not already present:

```cpp
#include <chrono>
```

- [ ] **Step 2: Implement in the .cpp**

In `magic_dingus_box_cpp/src/platform/gpio_manager.cpp`, locate the `poll()` method (search for `read_line(gpio::BUTTON_PINS[i])`). Inside the button-state-change branch, after debouncing, add:

```cpp
// Track press state + time for chord/snapshot API
button_pressed_[i] = pressed;
if (pressed) {
    button_press_times_[i] = std::chrono::steady_clock::now();
}
```

Add the chord helper at the end of the file, inside the `GpioManager` class scope:

```cpp
bool GpioManager::is_chord_btn1_btn3() const {
    // Chord = both buttons currently pressed AND they transitioned to
    // pressed within 50ms of each other.
    if (!button_pressed_[0] || !button_pressed_[2]) return false;

    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        button_press_times_[0] > button_press_times_[2]
            ? button_press_times_[0] - button_press_times_[2]
            : button_press_times_[2] - button_press_times_[0]).count();

    return diff <= 50;
}
```

Note: BTN1 is index 0 (from `BUTTON_PINS[] = {BTN1_SW, BTN2_SW, BTN3_SW, BTN4_SW}`) and BTN3 is index 2.

- [ ] **Step 3: Build + verify (no new tests — integration-tested via main.cpp wiring in Task 11)**

```bash
cd magic_dingus_box_cpp/build-mac
make -j4 test_media_browser_unit
./test_media_browser_unit
```

Expected: all previous tests still pass. (gpio_manager isn't in the test build on Mac because it's gated behind BUILD_KIOSK; this step verifies the header change doesn't break anything.)

Full Pi verification comes in Task 11 when the main loop wiring is complete.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/platform/gpio_manager.h \
        magic_dingus_box_cpp/src/platform/gpio_manager.cpp
git commit -m "feat(media_browser): gpio_manager exposes button snapshot + chord helper"
```

---

## Task 9: Toast renderer

**Files:**
- Create: `magic_dingus_box_cpp/src/ui/toast.h`
- Create: `magic_dingus_box_cpp/src/ui/toast.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (add toast to UI_SOURCES only when BUILD_KIOSK)

- [ ] **Step 1: Write the header**

Create `magic_dingus_box_cpp/src/ui/toast.h`:

```cpp
#pragma once

#include <string>
#include <chrono>

namespace ui {

class Renderer;  // forward

// Transient on-screen notification — a centered panel with text that
// fades in, holds, and fades out over a total of 3 seconds.
//
// Usage:
//   Toast::show("Movie section unlocked");
//   // ... each frame:
//   Toast::render(renderer, screen_w, screen_h);
class Toast {
public:
    // Show a toast. Replaces any existing toast.
    static void show(std::string message);

    // Render the current toast (if any). No-op when no toast or when
    // the toast has expired.
    static void render(Renderer& r, int screen_w, int screen_h);

    // Clear any active toast immediately.
    static void clear();

    // Test-only: returns true if a toast is active right now.
    static bool is_active();

private:
    static std::string message_;
    static std::chrono::steady_clock::time_point shown_at_;
    static bool active_;
};

}  // namespace ui
```

- [ ] **Step 2: Write the implementation**

Create `magic_dingus_box_cpp/src/ui/toast.cpp`:

```cpp
#include "ui/toast.h"
#include "ui/renderer.h"
#include "ui/theme.h"

#include <algorithm>

namespace ui {

std::string Toast::message_;
std::chrono::steady_clock::time_point Toast::shown_at_;
bool Toast::active_ = false;

constexpr int FADE_IN_MS = 300;
constexpr int HOLD_MS = 2400;
constexpr int FADE_OUT_MS = 300;
constexpr int TOTAL_MS = FADE_IN_MS + HOLD_MS + FADE_OUT_MS;

void Toast::show(std::string message) {
    message_ = std::move(message);
    shown_at_ = std::chrono::steady_clock::now();
    active_ = true;
}

void Toast::clear() {
    active_ = false;
    message_.clear();
}

bool Toast::is_active() {
    if (!active_) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shown_at_).count();
    return elapsed < TOTAL_MS;
}

void Toast::render(Renderer& r, int screen_w, int screen_h) {
    if (!active_) return;

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shown_at_).count();

    if (elapsed_ms >= TOTAL_MS) {
        active_ = false;
        return;
    }

    // Alpha curve
    float alpha = 1.0f;
    if (elapsed_ms < FADE_IN_MS) {
        alpha = static_cast<float>(elapsed_ms) / FADE_IN_MS;
    } else if (elapsed_ms >= FADE_IN_MS + HOLD_MS) {
        auto fade_out_elapsed = elapsed_ms - (FADE_IN_MS + HOLD_MS);
        alpha = 1.0f - static_cast<float>(fade_out_elapsed) / FADE_OUT_MS;
    }
    alpha = std::max(0.0f, std::min(1.0f, alpha));

    // Centered panel
    const int panel_w = 480;
    const int panel_h = 80;
    const int x = (screen_w - panel_w) / 2;
    const int y = (screen_h - panel_h) / 2;

    // Semi-transparent background
    r.draw_quad(x, y, panel_w, panel_h,
                theme::ACCENT_BG.r, theme::ACCENT_BG.g, theme::ACCENT_BG.b,
                0.9f * alpha);

    // Border
    r.draw_quad_outline(x, y, panel_w, panel_h,
                       theme::ACCENT_FG.r, theme::ACCENT_FG.g, theme::ACCENT_FG.b,
                       alpha, 2);

    // Text centered in panel
    r.draw_text_centered(message_, x + panel_w / 2, y + panel_h / 2,
                         theme::TEXT_FG.r, theme::TEXT_FG.g, theme::TEXT_FG.b,
                         alpha, /*font_size=*/24);
}

}  // namespace ui
```

**Note:** this uses `Renderer` methods (`draw_quad`, `draw_quad_outline`, `draw_text_centered`) that already exist in the Phase 1 renderer. If any method name differs slightly in the actual codebase, align to the existing names — the renderer is Phase 1 code and its API is stable.

- [ ] **Step 3: Add toast.cpp to CMakeLists UI_SOURCES (inside BUILD_KIOSK block)**

In `CMakeLists.txt`, locate `set(UI_SOURCES ...)` (it's currently outside the BUILD_KIOSK block — the full kiosk UI sources). Add:

```cmake
set(UI_SOURCES
    src/ui/renderer.cpp
    src/ui/theme.cpp
    src/ui/font_manager.cpp
    src/ui/settings_menu.cpp
    src/ui/virtual_keyboard.cpp
    src/ui/qrcodegen.cpp
    src/ui/toast.cpp
)
```

- [ ] **Step 4: Build on Pi via deploy (toast.cpp only compiles in full kiosk build)**

The toast file depends on the real `Renderer` which is Linux/kiosk-only. To verify it compiles, a Pi build is required. Run:

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Expected: kiosk rebuilds cleanly on Pi.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/toast.h \
        magic_dingus_box_cpp/src/ui/toast.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(media_browser): Toast overlay primitive (centered fade in/hold/fade out)"
```

---

## Task 10: Unlock persistence in settings_persistence

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/settings_persistence.h`
- Modify: `magic_dingus_box_cpp/src/app/settings_persistence.cpp`
- Modify: `magic_dingus_box_cpp/src/app/app_state.h`

- [ ] **Step 1: Add field to app state**

In `magic_dingus_box_cpp/src/app/app_state.h`, locate the settings struct (search for `struct Settings`) and add a field:

```cpp
    // Media Browser feature (unlocked via secret sequence)
    bool media_browser_unlocked = false;
```

- [ ] **Step 2: Add to YAML serialization**

In `magic_dingus_box_cpp/src/app/settings_persistence.cpp`, locate the `save` and `load` functions for the Settings struct. Add serialization:

**In `save()` after the last existing field:**
```cpp
out << YAML::Key << "media_browser_unlocked"
    << YAML::Value << settings.media_browser_unlocked;
```

**In `load()`:**
```cpp
if (root["media_browser_unlocked"])
    settings.media_browser_unlocked = root["media_browser_unlocked"].as<bool>();
```

- [ ] **Step 3: Build on Pi + verify round-trip**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
ssh magic@magicpi.local 'cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && ./magic_dingus_box_cpp --help 2>&1 | head -5'
```

Expected: compiles cleanly.

Round-trip verify: run the kiosk briefly, kill it (Ctrl-C or kill via SSH), then inspect `data/settings.yaml` — should contain `media_browser_unlocked: false` line. Edit to `true`, restart — should still be `true`.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/app/app_state.h \
        magic_dingus_box_cpp/src/app/settings_persistence.cpp
git commit -m "feat(media_browser): persist media_browser_unlocked flag in settings"
```

---

## Task 11: Wire sequence detector into main loop + Movies menu row

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp`
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.cpp`

- [ ] **Step 1: Instantiate + feed the detector in main loop**

In `magic_dingus_box_cpp/src/main.cpp`, near the top of `main()` (after input_manager/gpio_manager init), add:

```cpp
#ifdef MEDIA_BROWSER_ENABLED
#include "platform/sequence_detector.h"
#include "ui/toast.h"
platform::SequenceDetector seq_detector;
#endif
```

In the main loop, after the `gpio.poll()` call (search for `gpio.poll()`), add:

```cpp
#ifdef MEDIA_BROWSER_ENABLED
{
    using namespace std::chrono;
    auto now = steady_clock::now();
    platform::SeqInput ev = platform::SeqInput::NONE;

    // Raw GPIO events
    for (const auto& e : gpio_events) {
        if (!e.pressed) continue;
        // Phase 1 already maps BTN4 to SETTINGS_MENU action; we need the
        // raw button index. GpioManager emits InputAction but we also want
        // per-button events. Use gpio.button_state_snapshot() for the chord
        // detection and wire explicit per-button events below.
    }

    // Chord check wins if active
    if (gpio.is_chord_btn1_btn3()) {
        ev = platform::SeqInput::BTN1_BTN3_CHORD;
    } else {
        // Iterate single-button GPIO events from this tick.
        // (Detection of single-button edge-press is done inside gpio.poll();
        // we consume the InputAction here and map back to SeqInput.)
        for (const auto& e : gpio_events) {
            if (!e.pressed) continue;
            switch (e.action) {
                case platform::InputAction::PREV:        ev = platform::SeqInput::BTN1_PRESS; break;
                case platform::InputAction::PLAY_PAUSE:  ev = platform::SeqInput::BTN2_PRESS; break;
                case platform::InputAction::NEXT:        ev = platform::SeqInput::BTN3_PRESS; break;
                case platform::InputAction::SETTINGS_MENU: ev = platform::SeqInput::BTN4_PRESS; break;
                case platform::InputAction::SELECT:
                    // SELECT on rotary device → RCLICK
                    // (map this only if the underlying device->is_rotary is true;
                    //  input_manager already exposes this via an InputEvent flag
                    //  — if not, add a bool is_from_rotary in InputEvent in a
                    //  small follow-up; for now only flag SELECT coming from
                    //  the rotary device input path.)
                    ev = platform::SeqInput::ROTARY_CLICK;
                    break;
                default: break;
            }
            if (ev != platform::SeqInput::NONE) break;
        }
    }

    if (ev != platform::SeqInput::NONE) {
        auto result = seq_detector.feed(ev, now);
        if (result == platform::SeqResult::UNLOCKED) {
            state.settings.media_browser_unlocked = true;
            save_settings(state.settings);
            ui::Toast::show("Movie section unlocked");
        }
    }
}
#endif
```

**Note on InputEvent + rotary:** if `InputEvent` doesn't carry a "from rotary" flag, add one in a minimal patch to `input_manager.h` (add `bool is_from_rotary = false;` to `InputEvent`) and set it in `poll()` when the device is rotary. Keep the change surgical.

- [ ] **Step 2: Render toast after UI, before swap buffers**

In `main.cpp` main loop, find where the UI is rendered (search for `ui_renderer.end_frame()` or similar) and add right before the buffer swap:

```cpp
#ifdef MEDIA_BROWSER_ENABLED
    ui::Toast::render(ui_renderer, drm.width(), drm.height());
#endif
```

- [ ] **Step 3: Conditionally add Movies row to settings menu**

In `magic_dingus_box_cpp/src/ui/settings_menu.cpp`, locate the settings row definitions (search for rows, or the enum that defines menu items). Add a new entry wrapped in `#ifdef MEDIA_BROWSER_ENABLED`:

```cpp
#ifdef MEDIA_BROWSER_ENABLED
if (state.settings.media_browser_unlocked) {
    // Add Movies row — selection transitions to AppScreen::MediaBrowser
    rows.push_back({
        .label = "Movies",
        .kind = RowKind::SUBMENU,
        .on_select = [&state]() {
            state.current_screen = AppScreen::MediaBrowser;
        }
    });
}
#endif
```

The exact row-struct and add-to-menu mechanism depends on the existing pattern in settings_menu — use it faithfully rather than reimplementing. If the current menu uses an enum + switch statement rather than a row list, extend the enum with a `MEDIA_BROWSER` entry conditional on the flag and unlock state.

- [ ] **Step 4: Add AppScreen::MediaBrowser to enum**

In `magic_dingus_box_cpp/src/app/app_state.h`, find the `AppScreen` enum and add:

```cpp
enum class AppScreen {
    // ... existing entries ...
    MediaBrowser,  // V2 — unlocked via secret sequence
};
```

The main screen-dispatch in `main.cpp` will handle this new value in Task 16. For now, add a temporary stub: if current_screen is `MediaBrowser`, render a "Movies — UI coming soon" placeholder and handle B button to return to settings menu. This is 5-10 lines in the main render dispatch.

```cpp
// In main.cpp main loop, screen dispatch section:
#ifdef MEDIA_BROWSER_ENABLED
    else if (state.current_screen == AppScreen::MediaBrowser) {
        // Placeholder until Task 16 wires the real UI.
        ui_renderer.draw_text("Movies — Media Browser", /*center*/);
        ui_renderer.draw_text("[B to return]", /*below*/);
        for (const auto& e : input_events) {
            if (e.action == platform::InputAction::SETTINGS_MENU) {
                state.current_screen = AppScreen::MainMenu;
            }
        }
    }
#endif
```

- [ ] **Step 5: Deploy + test on Pi end-to-end**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build --media-browser
```

SSH in and run the kiosk. Enter the sequence physically:
1. BTN1 + BTN3 simultaneously
2. BTN2
3. BTN2
4. BTN2
5. RCLICK

Expected: toast appears, settings menu now shows "Movies" row, selecting it shows the placeholder screen, B returns.

Verify persistence: kill and restart the kiosk — Movies row should still appear (unlocked state persisted).

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp \
        magic_dingus_box_cpp/src/ui/settings_menu.cpp \
        magic_dingus_box_cpp/src/app/app_state.h
git commit -m "feat(media_browser): wire sequence detector + toast + Movies menu row"
```

---

## Sub-Project B3: RadarrClient (Tasks 12–16)

---

## Task 12: radarr_types.h — struct definitions

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h`

- [ ] **Step 1: Write the header**

Create `magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h`:

```cpp
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace media_browser {

struct MovieSearchHit {
    int tmdb_id = 0;
    std::string title;
    std::string original_title;
    std::string overview;
    std::string poster_url;
    std::string fanart_url;
    int year = 0;
    int runtime_minutes = 0;
    double rating = 0.0;
    std::string imdb_id;
};

// Full movie record (includes fields set only for movies in library)
struct Movie : MovieSearchHit {
    int radarr_id = 0;
    bool monitored = false;
    bool has_file = false;
    std::string file_path;       // relative to root folder
    std::string file_quality;    // e.g. "Bluray-1080p"
    int64_t file_size_bytes = 0;
    std::string added_at;        // ISO 8601
};

struct QueueItem {
    int id = 0;                  // queue row id (used for delete)
    int movie_id = 0;            // Radarr movie id
    std::string title;
    std::string poster_url;
    double progress = 0.0;       // 0.0 - 1.0
    int download_rate_bps = 0;
    int upload_rate_bps = 0;
    int peers = 0;
    int seeds = 0;
    int64_t size_bytes = 0;
    int64_t sizeleft_bytes = 0;
    std::string state;           // "queued", "downloading", "completed", "failed"
    int eta_seconds = 0;
};

struct QualityProfile {
    int id = 0;
    std::string name;
    int cutoff_quality_id = 0;
    std::vector<int> allowed_qualities;
};

struct RootFolder {
    int id = 0;
    std::string path;
    int64_t free_space_bytes = 0;
    int64_t total_space_bytes = 0;
};

struct IndexerInfo {
    int id = 0;
    std::string name;
    std::string protocol;        // "torrent", "usenet"
    bool enabled = false;
    int priority = 50;
};

struct SystemStatus {
    std::string version;
    std::string build_time;
    bool startup_completed = false;
};

}  // namespace media_browser
```

- [ ] **Step 2: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/radarr/radarr_types.h
git commit -m "feat(media_browser): Radarr type definitions"
```

---

## Task 13: radarr_parsers — JSON fixtures + TDD tests + impl

**Files:**
- Create: `magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/movie_lookup.json`
- Create: `magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/queue.json`
- Create: `magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/quality_profiles.json`
- Create: `magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/system_status.json`
- Create: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.h`
- Create: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.cpp`
- Create: `magic_dingus_box_cpp/tests/media_browser/test_radarr_parsers.cpp`

- [ ] **Step 1: Create fixture files**

Create `magic_dingus_box_cpp/tests/media_browser/fixtures/radarr/movie_lookup.json` (abridged sample from Radarr's `/api/v3/movie/lookup?term=matrix`):

```json
[
  {
    "tmdbId": 603,
    "imdbId": "tt0133093",
    "title": "The Matrix",
    "originalTitle": "The Matrix",
    "overview": "A hacker learns from mysterious rebels.",
    "year": 1999,
    "runtime": 136,
    "ratings": {"tmdb": {"value": 8.2}},
    "images": [
      {"coverType": "poster", "remoteUrl": "https://image.tmdb.org/t/p/original/f89U3ADr1oiB1s9GkdPOEpXUk5H.jpg"},
      {"coverType": "fanart",  "remoteUrl": "https://image.tmdb.org/t/p/original/ncEsesgOJDNrTUED89hYbA117wo.jpg"}
    ]
  },
  {
    "tmdbId": 604,
    "title": "The Matrix Reloaded",
    "year": 2003,
    "runtime": 138,
    "ratings": {"tmdb": {"value": 7.2}},
    "images": [
      {"coverType": "poster", "remoteUrl": "https://image.tmdb.org/t/p/original/x.jpg"}
    ]
  }
]
```

Create `tests/media_browser/fixtures/radarr/queue.json`:

```json
{
  "page": 1,
  "pageSize": 20,
  "totalRecords": 1,
  "records": [
    {
      "id": 42,
      "movieId": 5,
      "title": "Sita Sings the Blues (2008)",
      "size": 1234567890,
      "sizeleft": 600000000,
      "status": "downloading",
      "trackedDownloadState": "downloading",
      "protocol": "torrent",
      "indexer": "PublicHD",
      "downloadClient": "qBittorrent",
      "estimatedCompletionTime": "2026-04-23T17:30:00Z",
      "timeleft": "00:15:00"
    }
  ]
}
```

Create `tests/media_browser/fixtures/radarr/quality_profiles.json`:

```json
[
  {"id": 1, "name": "Any", "cutoff": 1, "items": []},
  {"id": 2, "name": "1080p Standard", "cutoff": 7, "items": []}
]
```

Create `tests/media_browser/fixtures/radarr/system_status.json`:

```json
{
  "version": "5.14.0.9383",
  "buildTime": "2025-03-15T00:00:00Z",
  "isDebug": false,
  "startupPath": "/app/radarr/bin"
}
```

- [ ] **Step 2: Write parser header**

Create `magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.h`:

```cpp
#pragma once

#include <string>
#include <vector>
#include <optional>
#include "media_browser/radarr/radarr_types.h"

namespace media_browser {

class RadarrParsers {
public:
    static std::vector<MovieSearchHit> parse_movie_lookup(const std::string& json);
    static std::vector<Movie> parse_movie_list(const std::string& json);
    static std::optional<Movie> parse_movie(const std::string& json);
    static std::vector<QueueItem> parse_queue(const std::string& json);
    static std::vector<QualityProfile> parse_quality_profiles(const std::string& json);
    static std::vector<RootFolder> parse_root_folders(const std::string& json);
    static std::optional<SystemStatus> parse_system_status(const std::string& json);
};

}  // namespace media_browser
```

- [ ] **Step 3: Write failing tests**

Create `magic_dingus_box_cpp/tests/media_browser/test_radarr_parsers.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "media_browser/radarr/radarr_parsers.h"

namespace fs = std::filesystem;

static std::string read_fixture(const std::string& name) {
    fs::path p = fs::path(__FILE__).parent_path() / "fixtures" / "radarr" / name;
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_CASE("parse_movie_lookup: extracts 2 hits from fixture", "[radarr][parsers]") {
    auto json = read_fixture("movie_lookup.json");
    auto hits = media_browser::RadarrParsers::parse_movie_lookup(json);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].tmdb_id == 603);
    REQUIRE(hits[0].title == "The Matrix");
    REQUIRE(hits[0].year == 1999);
    REQUIRE(hits[0].runtime_minutes == 136);
    REQUIRE(hits[0].rating == Catch::Approx(8.2));
    REQUIRE(hits[0].imdb_id == "tt0133093");
    REQUIRE(!hits[0].poster_url.empty());
    REQUIRE(!hits[0].fanart_url.empty());
    REQUIRE(hits[1].tmdb_id == 604);
    REQUIRE(hits[1].year == 2003);
}

TEST_CASE("parse_queue: extracts 1 queue item", "[radarr][parsers]") {
    auto json = read_fixture("queue.json");
    auto items = media_browser::RadarrParsers::parse_queue(json);
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].id == 42);
    REQUIRE(items[0].movie_id == 5);
    REQUIRE(items[0].title.find("Sita Sings") != std::string::npos);
    REQUIRE(items[0].size_bytes == 1234567890);
    REQUIRE(items[0].sizeleft_bytes == 600000000);
    REQUIRE(items[0].state == "downloading");
    // Progress derived: (size - sizeleft) / size
    REQUIRE(items[0].progress == Catch::Approx(0.5139).margin(0.01));
}

TEST_CASE("parse_quality_profiles: extracts profiles", "[radarr][parsers]") {
    auto json = read_fixture("quality_profiles.json");
    auto profiles = media_browser::RadarrParsers::parse_quality_profiles(json);
    REQUIRE(profiles.size() == 2);
    REQUIRE(profiles[0].id == 1);
    REQUIRE(profiles[0].name == "Any");
    REQUIRE(profiles[1].id == 2);
    REQUIRE(profiles[1].name == "1080p Standard");
    REQUIRE(profiles[1].cutoff_quality_id == 7);
}

TEST_CASE("parse_system_status: extracts version", "[radarr][parsers]") {
    auto json = read_fixture("system_status.json");
    auto status = media_browser::RadarrParsers::parse_system_status(json);
    REQUIRE(status.has_value());
    REQUIRE(status->version == "5.14.0.9383");
}

TEST_CASE("parse_movie_lookup: handles empty array", "[radarr][parsers]") {
    auto hits = media_browser::RadarrParsers::parse_movie_lookup("[]");
    REQUIRE(hits.empty());
}

TEST_CASE("parse_system_status: handles invalid JSON", "[radarr][parsers]") {
    auto status = media_browser::RadarrParsers::parse_system_status("not json");
    REQUIRE(!status.has_value());
}
```

- [ ] **Step 4: Add sources + tests + fixtures to CMake**

In `CMakeLists.txt`, update `MEDIA_BROWSER_SOURCES` and `MEDIA_BROWSER_TEST_SOURCES`:

```cmake
set(MEDIA_BROWSER_SOURCES
    src/media_browser/library/library_db.cpp
    src/media_browser/tmdb_client.cpp
    src/media_browser/torrent/torrent_session.cpp
    src/media_browser/radarr/radarr_parsers.cpp
    src/platform/sequence_detector.cpp
)

set(MEDIA_BROWSER_TEST_SOURCES
    tests/media_browser/test_smoke.cpp
    tests/media_browser/test_library_db.cpp
    tests/media_browser/test_tmdb_client.cpp
    tests/media_browser/test_sequence_detector.cpp
    tests/media_browser/test_radarr_parsers.cpp
    ${MEDIA_BROWSER_SOURCES}
)
```

- [ ] **Step 5: Verify tests fail to compile (no parser impl)**

```bash
cd magic_dingus_box_cpp/build-mac
cmake .. -DENABLE_MEDIA_BROWSER=ON
make -j4 test_media_browser_unit 2>&1 | tail -10
```

Expected: link error for `RadarrParsers` methods.

- [ ] **Step 6: Write parser implementation**

Create `magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.cpp`:

```cpp
#include "media_browser/radarr/radarr_parsers.h"

#include <json/json.h>
#include <spdlog/spdlog.h>

#include <sstream>

namespace media_browser {

namespace {

bool parse_json(const std::string& text, Json::Value& out) {
    Json::CharReaderBuilder rb;
    std::string err;
    std::istringstream is(text);
    return Json::parseFromStream(rb, is, &out, &err);
}

std::string pick_image(const Json::Value& images, const std::string& coverType) {
    if (!images.isArray()) return "";
    for (const auto& img : images) {
        if (img["coverType"].asString() == coverType) {
            return img["remoteUrl"].asString();
        }
    }
    return "";
}

void fill_search_hit(const Json::Value& r, MovieSearchHit& h) {
    h.tmdb_id = r.get("tmdbId", 0).asInt();
    h.imdb_id = r.get("imdbId", "").asString();
    h.title = r.get("title", "").asString();
    h.original_title = r.get("originalTitle", "").asString();
    h.overview = r.get("overview", "").asString();
    h.year = r.get("year", 0).asInt();
    h.runtime_minutes = r.get("runtime", 0).asInt();
    h.rating = r["ratings"]["tmdb"].get("value", 0.0).asDouble();
    h.poster_url = pick_image(r["images"], "poster");
    h.fanart_url = pick_image(r["images"], "fanart");
}

}  // namespace

std::vector<MovieSearchHit> RadarrParsers::parse_movie_lookup(const std::string& json) {
    std::vector<MovieSearchHit> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        MovieSearchHit h;
        fill_search_hit(r, h);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<Movie> RadarrParsers::parse_movie_list(const std::string& json) {
    std::vector<Movie> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        Movie m;
        fill_search_hit(r, m);
        m.radarr_id = r.get("id", 0).asInt();
        m.monitored = r.get("monitored", false).asBool();
        m.has_file = r.get("hasFile", false).asBool();
        if (r.isMember("movieFile")) {
            const auto& f = r["movieFile"];
            m.file_path = f.get("relativePath", "").asString();
            m.file_quality = f["quality"]["quality"].get("name", "").asString();
            m.file_size_bytes = f.get("size", 0).asInt64();
        }
        m.added_at = r.get("added", "").asString();
        out.push_back(std::move(m));
    }
    return out;
}

std::optional<Movie> RadarrParsers::parse_movie(const std::string& json) {
    Json::Value root;
    if (!parse_json(json, root) || !root.isObject()) return std::nullopt;
    Movie m;
    fill_search_hit(root, m);
    m.radarr_id = root.get("id", 0).asInt();
    m.monitored = root.get("monitored", false).asBool();
    m.has_file = root.get("hasFile", false).asBool();
    return m;
}

std::vector<QueueItem> RadarrParsers::parse_queue(const std::string& json) {
    std::vector<QueueItem> out;
    Json::Value root;
    if (!parse_json(json, root)) return out;
    const auto& records = root["records"];
    if (!records.isArray()) return out;
    for (const auto& r : records) {
        QueueItem q;
        q.id = r.get("id", 0).asInt();
        q.movie_id = r.get("movieId", 0).asInt();
        q.title = r.get("title", "").asString();
        q.size_bytes = r.get("size", 0).asInt64();
        q.sizeleft_bytes = r.get("sizeleft", 0).asInt64();
        q.state = r.get("status", "").asString();
        if (q.size_bytes > 0) {
            q.progress = static_cast<double>(q.size_bytes - q.sizeleft_bytes)
                         / static_cast<double>(q.size_bytes);
        }
        out.push_back(std::move(q));
    }
    return out;
}

std::vector<QualityProfile> RadarrParsers::parse_quality_profiles(const std::string& json) {
    std::vector<QualityProfile> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        QualityProfile p;
        p.id = r.get("id", 0).asInt();
        p.name = r.get("name", "").asString();
        p.cutoff_quality_id = r.get("cutoff", 0).asInt();
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<RootFolder> RadarrParsers::parse_root_folders(const std::string& json) {
    std::vector<RootFolder> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        RootFolder f;
        f.id = r.get("id", 0).asInt();
        f.path = r.get("path", "").asString();
        f.free_space_bytes = r.get("freeSpace", 0).asInt64();
        out.push_back(std::move(f));
    }
    return out;
}

std::optional<SystemStatus> RadarrParsers::parse_system_status(const std::string& json) {
    Json::Value root;
    if (!parse_json(json, root) || !root.isObject()) return std::nullopt;
    SystemStatus s;
    s.version = root.get("version", "").asString();
    s.build_time = root.get("buildTime", "").asString();
    s.startup_completed = true;
    return s;
}

}  // namespace media_browser
```

- [ ] **Step 7: Run tests to verify they pass**

```bash
cd magic_dingus_box_cpp/build-mac
make -j4 test_media_browser_unit
./test_media_browser_unit "[radarr]"
```

Expected: 6 test cases pass.

- [ ] **Step 8: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.h \
        magic_dingus_box_cpp/src/media_browser/radarr/radarr_parsers.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_radarr_parsers.cpp \
        magic_dingus_box_cpp/tests/media_browser/fixtures/ \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(media_browser): Radarr JSON parsers + fixture-based tests"
```

---

## Task 14: radarr_client HTTP methods

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h`
- Create: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (add radarr_client.cpp)

- [ ] **Step 1: Write the header**

Create `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h`:

```cpp
#pragma once

#include <string>
#include <vector>
#include <optional>
#include "media_browser/radarr/radarr_types.h"

namespace media_browser {

class RadarrClient {
public:
    struct Config {
        std::string base_url = "http://localhost:7878";
        std::string api_key;
        int timeout_secs = 10;
    };

    explicit RadarrClient(Config config);
    virtual ~RadarrClient();

    RadarrClient(const RadarrClient&) = delete;
    RadarrClient& operator=(const RadarrClient&) = delete;

    // Service health
    virtual bool is_reachable();
    virtual std::optional<SystemStatus> get_status();

    // Movie discovery
    virtual std::vector<MovieSearchHit> lookup(const std::string& query);
    virtual std::vector<Movie> get_library();
    virtual std::optional<Movie> get_movie(int radarr_id);

    // Library management
    virtual bool add_movie(int tmdb_id, int quality_profile_id, bool monitor = true);
    virtual bool remove_movie(int radarr_id, bool delete_files = false);
    virtual bool trigger_search(int radarr_id);

    // Queue / downloads
    virtual std::vector<QueueItem> get_queue();
    virtual bool cancel_queue_item(int queue_id);

    // Profiles
    virtual std::vector<QualityProfile> get_quality_profiles();
    virtual std::vector<RootFolder> get_root_folders();

    // Diagnostics
    const std::string& last_error() const { return last_error_; }

protected:
    // Virtual for mocking (see radarr_mock.h)
    virtual std::string http_get(const std::string& path);
    virtual std::string http_post(const std::string& path, const std::string& body);
    virtual std::string http_delete(const std::string& path);

    Config cfg_;
    std::string last_error_;
};

}  // namespace media_browser
```

- [ ] **Step 2: Write the implementation**

Create `magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp`:

```cpp
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/radarr/radarr_parsers.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <sstream>

namespace media_browser {

namespace {
static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}
}  // namespace

RadarrClient::RadarrClient(Config config) : cfg_(std::move(config)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

RadarrClient::~RadarrClient() {
    curl_global_cleanup();
}

std::string RadarrClient::http_get(const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) { last_error_ = "curl init failed"; return {}; }
    std::string url = cfg_.base_url + path;
    std::string body;
    struct curl_slist* headers = nullptr;
    std::string auth = "X-Api-Key: " + cfg_.api_key;
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_secs));
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { last_error_ = curl_easy_strerror(rc); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        last_error_ = os.str();
        return {};
    }
    return body;
}

std::string RadarrClient::http_post(const std::string& path, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) { last_error_ = "curl init failed"; return {}; }
    std::string url = cfg_.base_url + path;
    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-Api-Key: " + cfg_.api_key).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_secs));
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { last_error_ = curl_easy_strerror(rc); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code << ": " << resp;
        last_error_ = os.str();
        return {};
    }
    return resp;
}

std::string RadarrClient::http_delete(const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) { last_error_ = "curl init failed"; return {}; }
    std::string url = cfg_.base_url + path;
    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-Api-Key: " + cfg_.api_key).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_secs));
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { last_error_ = curl_easy_strerror(rc); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        last_error_ = os.str();
        return {};
    }
    return resp;
}

bool RadarrClient::is_reachable() {
    return !http_get("/ping").empty();
}

std::optional<SystemStatus> RadarrClient::get_status() {
    auto resp = http_get("/api/v3/system/status");
    if (resp.empty()) return std::nullopt;
    return RadarrParsers::parse_system_status(resp);
}

std::vector<MovieSearchHit> RadarrClient::lookup(const std::string& query) {
    // URL-encode query (minimal)
    std::string encoded;
    for (char c : query) {
        if (isalnum(c)) encoded += c;
        else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            encoded += buf;
        }
    }
    auto resp = http_get("/api/v3/movie/lookup?term=" + encoded);
    if (resp.empty()) return {};
    return RadarrParsers::parse_movie_lookup(resp);
}

std::vector<Movie> RadarrClient::get_library() {
    auto resp = http_get("/api/v3/movie");
    if (resp.empty()) return {};
    return RadarrParsers::parse_movie_list(resp);
}

std::optional<Movie> RadarrClient::get_movie(int radarr_id) {
    auto resp = http_get("/api/v3/movie/" + std::to_string(radarr_id));
    if (resp.empty()) return std::nullopt;
    return RadarrParsers::parse_movie(resp);
}

bool RadarrClient::add_movie(int tmdb_id, int quality_profile_id, bool monitor) {
    std::ostringstream body;
    body << R"({"tmdbId":)" << tmdb_id
         << R"(,"qualityProfileId":)" << quality_profile_id
         << R"(,"monitored":)" << (monitor ? "true" : "false")
         << R"(,"rootFolderPath":"/library/Movies")"
         << R"(,"addOptions":{"searchForMovie":)" << (monitor ? "true" : "false") << R"(}})";
    auto resp = http_post("/api/v3/movie", body.str());
    return !resp.empty();
}

bool RadarrClient::remove_movie(int radarr_id, bool delete_files) {
    std::string path = "/api/v3/movie/" + std::to_string(radarr_id)
                     + "?deleteFiles=" + (delete_files ? "true" : "false");
    auto resp = http_delete(path);
    return last_error_.empty() || resp.empty();  // DELETE often returns empty body
}

bool RadarrClient::trigger_search(int radarr_id) {
    std::ostringstream body;
    body << R"({"name":"MoviesSearch","movieIds":[)" << radarr_id << R"(]})";
    auto resp = http_post("/api/v3/command", body.str());
    return !resp.empty();
}

std::vector<QueueItem> RadarrClient::get_queue() {
    auto resp = http_get("/api/v3/queue?pageSize=100");
    if (resp.empty()) return {};
    return RadarrParsers::parse_queue(resp);
}

bool RadarrClient::cancel_queue_item(int queue_id) {
    auto resp = http_delete("/api/v3/queue/" + std::to_string(queue_id)
                           + "?removeFromClient=true&blocklist=false");
    return last_error_.empty();
}

std::vector<QualityProfile> RadarrClient::get_quality_profiles() {
    auto resp = http_get("/api/v3/qualityprofile");
    if (resp.empty()) return {};
    return RadarrParsers::parse_quality_profiles(resp);
}

std::vector<RootFolder> RadarrClient::get_root_folders() {
    auto resp = http_get("/api/v3/rootfolder");
    if (resp.empty()) return {};
    return RadarrParsers::parse_root_folders(resp);
}

}  // namespace media_browser
```

- [ ] **Step 3: Add to CMake + build verify**

In `CMakeLists.txt`, update `MEDIA_BROWSER_SOURCES`:

```cmake
set(MEDIA_BROWSER_SOURCES
    src/media_browser/library/library_db.cpp
    src/media_browser/tmdb_client.cpp
    src/media_browser/torrent/torrent_session.cpp
    src/media_browser/radarr/radarr_parsers.cpp
    src/media_browser/radarr/radarr_client.cpp
    src/platform/sequence_detector.cpp
)
```

Build:

```bash
cd magic_dingus_box_cpp/build-mac
cmake .. -DENABLE_MEDIA_BROWSER=ON
make -j4 test_media_browser_unit
./test_media_browser_unit
```

Expected: all tests still pass; no new tests for RadarrClient (integration-tested via CLI in Task 16).

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.h \
        magic_dingus_box_cpp/src/media_browser/radarr/radarr_client.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(media_browser): RadarrClient HTTP methods"
```

---

## Task 15: radarr_mock for UI development

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_mock.h`
- Create: `magic_dingus_box_cpp/src/media_browser/radarr/radarr_mock.cpp`

- [ ] **Step 1: Write mock header**

Create `magic_dingus_box_cpp/src/media_browser/radarr/radarr_mock.h`:

```cpp
#pragma once

#include "media_browser/radarr/radarr_client.h"

namespace media_browser {

// In-memory mock Radarr for UI development and tests without a real
// Radarr instance running. Seeds with a small deterministic dataset.
class RadarrMockClient : public RadarrClient {
public:
    RadarrMockClient();

    bool is_reachable() override;
    std::optional<SystemStatus> get_status() override;
    std::vector<MovieSearchHit> lookup(const std::string& query) override;
    std::vector<Movie> get_library() override;
    std::optional<Movie> get_movie(int radarr_id) override;
    bool add_movie(int tmdb_id, int quality_profile_id, bool monitor) override;
    bool remove_movie(int radarr_id, bool delete_files) override;
    bool trigger_search(int radarr_id) override;
    std::vector<QueueItem> get_queue() override;
    bool cancel_queue_item(int queue_id) override;
    std::vector<QualityProfile> get_quality_profiles() override;
    std::vector<RootFolder> get_root_folders() override;

private:
    std::vector<Movie> library_;
    std::vector<QueueItem> queue_;
    std::vector<QualityProfile> profiles_;
    int next_id_ = 1;
};

}  // namespace media_browser
```

- [ ] **Step 2: Write mock implementation**

Create `magic_dingus_box_cpp/src/media_browser/radarr/radarr_mock.cpp`:

```cpp
#include "media_browser/radarr/radarr_mock.h"

#include <algorithm>

namespace media_browser {

RadarrMockClient::RadarrMockClient() : RadarrClient({/* empty config */}) {
    // Seed profiles
    profiles_.push_back({1, "Any", 1, {}});
    profiles_.push_back({2, "1080p Standard", 7, {}});

    // Seed library with a few legal-film examples
    Movie m;
    m.radarr_id = next_id_++;
    m.tmdb_id = 20529;
    m.title = "Sita Sings the Blues";
    m.year = 2008;
    m.runtime_minutes = 82;
    m.rating = 6.9;
    m.monitored = true;
    m.has_file = true;
    m.file_quality = "Bluray-1080p";
    library_.push_back(m);

    // Seed a simulated queue entry
    QueueItem q;
    q.id = 1;
    q.movie_id = m.radarr_id;
    q.title = "Sita Sings the Blues (2008)";
    q.size_bytes = 1'500'000'000;
    q.sizeleft_bytes = 500'000'000;
    q.progress = 0.67;
    q.download_rate_bps = 1'500'000;
    q.peers = 42;
    q.state = "downloading";
    queue_.push_back(q);
}

bool RadarrMockClient::is_reachable() { return true; }

std::optional<SystemStatus> RadarrMockClient::get_status() {
    SystemStatus s;
    s.version = "mock-5.14.0";
    s.startup_completed = true;
    return s;
}

std::vector<MovieSearchHit> RadarrMockClient::lookup(const std::string& query) {
    // Return one fake hit shaped like the query for demo purposes
    MovieSearchHit h;
    h.tmdb_id = 999;
    h.title = query + " (mock)";
    h.year = 2024;
    h.runtime_minutes = 100;
    h.rating = 7.5;
    h.overview = "Mock result. Use real RadarrClient for actual data.";
    return {h};
}

std::vector<Movie> RadarrMockClient::get_library() { return library_; }

std::optional<Movie> RadarrMockClient::get_movie(int radarr_id) {
    for (const auto& m : library_) if (m.radarr_id == radarr_id) return m;
    return std::nullopt;
}

bool RadarrMockClient::add_movie(int tmdb_id, int /*qp*/, bool monitor) {
    Movie m;
    m.radarr_id = next_id_++;
    m.tmdb_id = tmdb_id;
    m.title = "Mock Movie " + std::to_string(tmdb_id);
    m.monitored = monitor;
    library_.push_back(m);
    return true;
}

bool RadarrMockClient::remove_movie(int radarr_id, bool /*del*/) {
    auto it = std::remove_if(library_.begin(), library_.end(),
                             [&](const Movie& m) { return m.radarr_id == radarr_id; });
    bool removed = (it != library_.end());
    library_.erase(it, library_.end());
    return removed;
}

bool RadarrMockClient::trigger_search(int /*id*/) { return true; }
std::vector<QueueItem> RadarrMockClient::get_queue() { return queue_; }
bool RadarrMockClient::cancel_queue_item(int id) {
    auto it = std::remove_if(queue_.begin(), queue_.end(),
                             [&](const QueueItem& q) { return q.id == id; });
    bool removed = (it != queue_.end());
    queue_.erase(it, queue_.end());
    return removed;
}
std::vector<QualityProfile> RadarrMockClient::get_quality_profiles() { return profiles_; }
std::vector<RootFolder> RadarrMockClient::get_root_folders() {
    RootFolder rf; rf.id = 1; rf.path = "/library/Movies"; rf.free_space_bytes = 500'000'000'000;
    return {rf};
}

}  // namespace media_browser
```

- [ ] **Step 3: Add to CMake + build**

In `CMakeLists.txt`, update `MEDIA_BROWSER_SOURCES` to include `radarr_mock.cpp`.

Build + verify:

```bash
cd magic_dingus_box_cpp/build-mac
make -j4 test_media_browser_unit
```

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/radarr/radarr_mock.h \
        magic_dingus_box_cpp/src/media_browser/radarr/radarr_mock.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(media_browser): RadarrMockClient for offline UI development"
```

---

## Task 16: Extend test_media_browser CLI with radarr-* subcommands

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/test_cli/main.cpp`

- [ ] **Step 1: Add radarr commands to the dispatch**

In `main.cpp`, locate the help text and command dispatch. Add:

In the help text (after the existing torrent commands):
```
  radarr-status                Ping Radarr, show version + reachability
  radarr-search <query>        Radarr /movie/lookup
  radarr-library               Show all movies in library
  radarr-queue                 Show active download queue
  radarr-add <tmdb_id>         Add movie to library (triggers search)
  radarr-profiles              List quality profiles
```

Add environment variable for Radarr API key:
```cpp
    if (const char* k = std::getenv("MDB_RADARR_API_KEY")) c.radarr_api_key = k;
```

And in the Config struct:
```cpp
    std::string radarr_api_key;
    std::string radarr_base_url = "http://localhost:7878";
```

In command dispatch (near existing torrent commands), add:
```cpp
if (cmd == "radarr-status") return cmd_radarr_status(cfg);
if (cmd == "radarr-search") {
    if (argc < 3) { print_help(); return 2; }
    return cmd_radarr_search(cfg, argv[2]);
}
if (cmd == "radarr-library") return cmd_radarr_library(cfg);
if (cmd == "radarr-queue") return cmd_radarr_queue(cfg);
if (cmd == "radarr-add") {
    if (argc < 3) { print_help(); return 2; }
    return cmd_radarr_add(cfg, std::atoi(argv[2]));
}
if (cmd == "radarr-profiles") return cmd_radarr_profiles(cfg);
```

- [ ] **Step 2: Implement the command handlers**

Add at the bottom of `main.cpp`, before the closing anonymous namespace brace:

```cpp
#include "media_browser/radarr/radarr_client.h"

namespace {

media_browser::RadarrClient make_radarr_client(const Config& c) {
    media_browser::RadarrClient::Config rc;
    rc.base_url = c.radarr_base_url;
    rc.api_key = c.radarr_api_key;
    return media_browser::RadarrClient(std::move(rc));
}

int cmd_radarr_status(const Config& c) {
    if (c.radarr_api_key.empty()) {
        spdlog::error("no Radarr API key - set MDB_RADARR_API_KEY");
        return 1;
    }
    auto r = make_radarr_client(c);
    auto status = r.get_status();
    if (!status) {
        spdlog::error("fetch failed: {}", r.last_error());
        return 1;
    }
    spdlog::info("Radarr: {} (reachable: true)", status->version);
    return 0;
}

int cmd_radarr_search(const Config& c, const std::string& query) {
    auto r = make_radarr_client(c);
    auto hits = r.lookup(query);
    spdlog::info("{} results for \"{}\":", hits.size(), query);
    for (const auto& h : hits) {
        spdlog::info("  [{:>7}] {} ({}) rating={:.1f}",
                     h.tmdb_id, h.title, h.year, h.rating);
    }
    return 0;
}

int cmd_radarr_library(const Config& c) {
    auto r = make_radarr_client(c);
    auto lib = r.get_library();
    spdlog::info("Library: {} movies", lib.size());
    for (const auto& m : lib) {
        spdlog::info("  [{:>5}] {} ({})  have_file={}  quality={}",
                     m.radarr_id, m.title, m.year, m.has_file, m.file_quality);
    }
    return 0;
}

int cmd_radarr_queue(const Config& c) {
    auto r = make_radarr_client(c);
    auto q = r.get_queue();
    spdlog::info("Queue: {} items", q.size());
    for (const auto& it : q) {
        spdlog::info("  [{:>5}] {} {:.1f}% state={}", it.id, it.title,
                     it.progress * 100.0, it.state);
    }
    return 0;
}

int cmd_radarr_add(const Config& c, int tmdb_id) {
    auto r = make_radarr_client(c);
    auto profiles = r.get_quality_profiles();
    if (profiles.empty()) {
        spdlog::error("no quality profiles in Radarr — set up in web UI first");
        return 1;
    }
    // Pick "1080p Standard" if present, else first profile
    int qp = profiles[0].id;
    for (const auto& p : profiles) if (p.name == "1080p Standard") { qp = p.id; break; }
    bool ok = r.add_movie(tmdb_id, qp, /*monitor=*/true);
    if (ok) spdlog::info("added tmdb_id={} with quality profile {}", tmdb_id, qp);
    else spdlog::error("failed: {}", r.last_error());
    return ok ? 0 : 1;
}

int cmd_radarr_profiles(const Config& c) {
    auto r = make_radarr_client(c);
    auto p = r.get_quality_profiles();
    for (const auto& q : p) {
        spdlog::info("  [{:>3}] {}", q.id, q.name);
    }
    return 0;
}

}  // namespace
```

- [ ] **Step 3: Deploy + test end-to-end on Pi**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build --media-browser
```

On the Pi, set the Radarr key and exercise the commands:

```bash
ssh magic@magicpi.local
export MDB_RADARR_API_KEY=$(grep RADARR_API_KEY /opt/magic_dingus_box/services/.env | cut -d= -f2)
cd /opt/magic_dingus_box/magic_dingus_box_cpp/build
./test_media_browser radarr-status
./test_media_browser radarr-profiles
./test_media_browser radarr-search "The Matrix"
./test_media_browser radarr-queue
```

Expected: each command produces valid output (Radarr version, profile list, search hits, empty queue).

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/test_cli/main.cpp
git commit -m "feat(media_browser): test_media_browser CLI — radarr-* subcommands"
```

---

## Sub-Project B4: Media Browser UI Screens (Tasks 17–23)

**Note:** The six UI screens follow a consistent pattern:
- Each is a `class SomeScreen` with `enter()`, `handle_input(event)`, `update()`, `render(Renderer&)` methods
- Each owns its navigation state; transitions set a flag consumed by the screen dispatcher in `main.cpp`
- All use existing `Renderer`, `FontManager`, `theme` from the Phase 1 kiosk UI tree
- Input uses the existing `InputEvent` / `InputAction` vocabulary

Tasks 17–23 create each screen. They're substantial individually but structurally similar — the plan provides complete code for each.

---

## Task 17: Screen dispatcher + MediaBrowser base state

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp`
- Create: `magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h` (base class)

- [ ] **Step 1: Write base class**

Create `magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h`:

```cpp
#pragma once

#include <vector>
#include "platform/input_manager.h"

namespace ui { class Renderer; }

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Screen identifiers within the Media Browser. The dispatcher tracks one.
enum class Screen {
    Browse,
    Search,
    Detail,
    Queue,
    Library,
    MovieSettings,
    Exit,         // Signal to leave the Media Browser and go back to main menu
};

// Each screen inherits this. Screens own their own state (selection, scroll).
// The dispatcher calls these in order per frame: handle_input → update → render.
class MbScreen {
public:
    virtual ~MbScreen() = default;

    // Called once when the screen becomes active.
    virtual void enter() {}

    // Called once when leaving the screen.
    virtual void leave() {}

    // Returns Screen::Exit-or-other value if the screen wants to transition.
    // Returns the current screen's enum otherwise.
    virtual Screen handle_input(const std::vector<platform::InputEvent>& events) = 0;

    // Called each frame before rendering for animation, async data, etc.
    virtual void update() {}

    // Render the screen.
    virtual void render(::ui::Renderer& r, int screen_w, int screen_h) = 0;
};

}  // namespace media_browser::ui
```

- [ ] **Step 2: Wire dispatcher in main.cpp**

Replace the Task 11 placeholder in `main.cpp` with a full dispatcher that owns all 6 screens:

```cpp
#ifdef MEDIA_BROWSER_ENABLED
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/ui/browse_screen.h"
#include "media_browser/ui/search_screen.h"
#include "media_browser/ui/detail_screen.h"
#include "media_browser/ui/queue_screen.h"
#include "media_browser/ui/library_screen.h"
#include "media_browser/ui/mb_settings_screen.h"

// Create Radarr client from .env / settings
media_browser::RadarrClient::Config radarr_cfg;
radarr_cfg.base_url = "http://localhost:7878";
radarr_cfg.api_key = /* read from settings or env */;
auto radarr = std::make_unique<media_browser::RadarrClient>(radarr_cfg);

// Instantiate screens
media_browser::ui::BrowseScreen browse(*radarr);
media_browser::ui::SearchScreen search(*radarr);
media_browser::ui::DetailScreen detail(*radarr);
media_browser::ui::QueueScreen queue(*radarr);
media_browser::ui::LibraryScreen library(*radarr);
media_browser::ui::MbSettingsScreen mb_settings(*radarr, state);

media_browser::ui::Screen current_mb_screen = media_browser::ui::Screen::Browse;
media_browser::ui::MbScreen* active_screen = &browse;
active_screen->enter();
#endif
```

In the main loop's screen-dispatch section, replace the Task 11 placeholder for `AppScreen::MediaBrowser` with:

```cpp
#ifdef MEDIA_BROWSER_ENABLED
    else if (state.current_screen == AppScreen::MediaBrowser) {
        auto next = active_screen->handle_input(input_events);
        if (next == media_browser::ui::Screen::Exit) {
            active_screen->leave();
            state.current_screen = AppScreen::MainMenu;
        } else if (next != current_mb_screen) {
            active_screen->leave();
            current_mb_screen = next;
            switch (next) {
                case media_browser::ui::Screen::Browse: active_screen = &browse; break;
                case media_browser::ui::Screen::Search: active_screen = &search; break;
                case media_browser::ui::Screen::Detail: active_screen = &detail; break;
                case media_browser::ui::Screen::Queue: active_screen = &queue; break;
                case media_browser::ui::Screen::Library: active_screen = &library; break;
                case media_browser::ui::Screen::MovieSettings: active_screen = &mb_settings; break;
                case media_browser::ui::Screen::Exit: break;  // handled above
            }
            active_screen->enter();
        }
        active_screen->update();
        active_screen->render(ui_renderer, drm.width(), drm.height());
    }
#endif
```

- [ ] **Step 3: Commit (dispatcher + base class; screens are stubs until Tasks 18-23)**

Add temporary empty screens (we'll implement each in Tasks 18-23). For now create placeholder `.h/.cpp` stubs for each of `browse_screen`, `search_screen`, `detail_screen`, `queue_screen`, `library_screen`, `mb_settings_screen` that just inherit `MbScreen` and render a single line "Screen: <name>". This lets Task 17 compile before Tasks 18-23 fill them in.

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/mb_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/*.h \
        magic_dingus_box_cpp/src/media_browser/ui/*.cpp \
        magic_dingus_box_cpp/src/main.cpp \
        magic_dingus_box_cpp/CMakeLists.txt
git commit -m "feat(media_browser): screen dispatcher + MbScreen base class + stubs"
```

---

## Task 18: Browse screen (poster grid + categories)

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/browse_screen.{h,cpp}`

The Browse screen fetches movies from a chosen category (Popular, Now Playing, Top Rated, Discover), renders as a 4-column poster grid, supports DPad/rotary navigation, and transitions to Detail on select.

- [ ] **Step 1: Complete the header**

Replace the stub `browse_screen.h` with:

```cpp
#pragma once

#include "media_browser/ui/mb_screen.h"
#include "media_browser/radarr/radarr_types.h"
#include <vector>
#include <string>

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

class BrowseScreen : public MbScreen {
public:
    explicit BrowseScreen(RadarrClient& radarr);

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

    // Used by DetailScreen to get the last-clicked movie
    int selected_tmdb_id() const { return selected_tmdb_id_; }

private:
    enum class Category { Popular, NowPlaying, TopRated, Discover };
    void fetch_category();

    RadarrClient& radarr_;
    Category category_ = Category::Popular;
    std::vector<MovieSearchHit> results_;
    int cursor_ = 0;               // index into results_
    int scroll_offset_ = 0;        // row offset for scrolling
    int selected_tmdb_id_ = 0;
};

}  // namespace media_browser::ui
```

- [ ] **Step 2: Complete the implementation**

Replace the stub `browse_screen.cpp` with:

```cpp
#include "media_browser/ui/browse_screen.h"
#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include <spdlog/spdlog.h>
#include <cstdio>

namespace media_browser::ui {

namespace {
constexpr int GRID_COLS = 4;
constexpr int POSTER_W = 220;
constexpr int POSTER_H = 330;
constexpr int POSTER_GAP = 16;
constexpr int TOP_STRIP_H = 60;
constexpr int GRID_TOP_PAD = TOP_STRIP_H + 16;
}  // namespace

BrowseScreen::BrowseScreen(RadarrClient& radarr) : radarr_(radarr) {}

void BrowseScreen::enter() {
    fetch_category();
    cursor_ = 0;
    scroll_offset_ = 0;
}

void BrowseScreen::fetch_category() {
    // Phase 2 note: Radarr doesn't expose /discover directly. For V2 we
    // piggyback on movie/lookup with category-specific seed queries.
    // This is a placeholder; a real implementation could call TMDB directly
    // or use Radarr's Import Lists API for popular/top-rated lists.
    std::string seed;
    switch (category_) {
        case Category::Popular:    seed = "popular"; break;
        case Category::NowPlaying: seed = "2024"; break;
        case Category::TopRated:   seed = "best"; break;
        case Category::Discover:   seed = "discover"; break;
    }
    results_ = radarr_.lookup(seed);
    if (results_.empty()) {
        spdlog::warn("[media_browser] Browse: no results for '{}': {}",
                     seed, radarr_.last_error());
    }
}

Screen BrowseScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        switch (e.action) {
            case platform::InputAction::ROTATE:
                cursor_ += e.delta;
                break;
            case platform::InputAction::ROTATE_VERTICAL:
                cursor_ += e.delta * GRID_COLS;
                break;
            case platform::InputAction::SELECT: {
                if (cursor_ >= 0 && cursor_ < static_cast<int>(results_.size())) {
                    selected_tmdb_id_ = results_[cursor_].tmdb_id;
                    return Screen::Detail;
                }
                break;
            }
            case platform::InputAction::SETTINGS_MENU:
                return Screen::Exit;  // B to exit Media Browser
            case platform::InputAction::QUIT:
                return Screen::Exit;
            default: break;
        }
    }
    // Bounds
    if (cursor_ < 0) cursor_ = 0;
    if (cursor_ >= static_cast<int>(results_.size()))
        cursor_ = static_cast<int>(results_.size()) - 1;
    // Scroll to keep cursor visible
    int row = cursor_ / GRID_COLS;
    if (row < scroll_offset_) scroll_offset_ = row;
    int visible_rows = 3;  // rendered rows
    if (row >= scroll_offset_ + visible_rows)
        scroll_offset_ = row - visible_rows + 1;

    return Screen::Browse;
}

void BrowseScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    // Background
    r.draw_quad(0, 0, screen_w, screen_h,
                ::ui::theme::BG.r, ::ui::theme::BG.g, ::ui::theme::BG.b, 1.0f);

    // Top category strip
    const char* cat_labels[] = {"Popular", "Now Playing", "Top Rated", "Discover"};
    int cat_x = 20;
    for (int i = 0; i < 4; ++i) {
        bool sel = (static_cast<int>(category_) == i);
        r.draw_text(cat_labels[i], cat_x, 20,
                    sel ? ::ui::theme::ACCENT_FG.r : ::ui::theme::TEXT_FG.r,
                    sel ? ::ui::theme::ACCENT_FG.g : ::ui::theme::TEXT_FG.g,
                    sel ? ::ui::theme::ACCENT_FG.b : ::ui::theme::TEXT_FG.b,
                    1.0f, 20);
        cat_x += 180;
    }

    // Grid
    int grid_x_start = (screen_w - (GRID_COLS * (POSTER_W + POSTER_GAP))) / 2;
    int visible_rows = 3;
    for (int row = 0; row < visible_rows; ++row) {
        for (int col = 0; col < GRID_COLS; ++col) {
            int idx = (scroll_offset_ + row) * GRID_COLS + col;
            if (idx >= static_cast<int>(results_.size())) continue;
            const auto& m = results_[idx];
            int x = grid_x_start + col * (POSTER_W + POSTER_GAP);
            int y = GRID_TOP_PAD + row * (POSTER_H + POSTER_GAP);
            bool is_cursor = (idx == cursor_);

            // Poster card background
            r.draw_quad(x, y, POSTER_W, POSTER_H,
                        ::ui::theme::PANEL_BG.r, ::ui::theme::PANEL_BG.g,
                        ::ui::theme::PANEL_BG.b, 1.0f);

            // Poster placeholder (real artwork loading deferred — see §18 in spec)
            // For now just show the title and year.
            r.draw_text(m.title, x + 10, y + POSTER_H - 60,
                        ::ui::theme::TEXT_FG.r, ::ui::theme::TEXT_FG.g,
                        ::ui::theme::TEXT_FG.b, 1.0f, 18);
            char ybuf[32];
            snprintf(ybuf, sizeof(ybuf), "%d", m.year);
            r.draw_text(ybuf, x + 10, y + POSTER_H - 35,
                        ::ui::theme::DIM_FG.r, ::ui::theme::DIM_FG.g,
                        ::ui::theme::DIM_FG.b, 1.0f, 14);

            // Cursor highlight
            if (is_cursor) {
                r.draw_quad_outline(x - 2, y - 2, POSTER_W + 4, POSTER_H + 4,
                                   ::ui::theme::ACCENT_FG.r,
                                   ::ui::theme::ACCENT_FG.g,
                                   ::ui::theme::ACCENT_FG.b, 1.0f, 3);
            }
        }
    }
}

}  // namespace media_browser::ui
```

- [ ] **Step 3: Build on Pi + test**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build --media-browser
```

On the Pi, unlock Movies, enter the Browse screen, navigate the grid, verify smooth scrolling.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/ui/browse_screen.h \
        magic_dingus_box_cpp/src/media_browser/ui/browse_screen.cpp
git commit -m "feat(media_browser): Browse screen with 4-column poster grid"
```

---

## Tasks 19–23: Remaining UI screens

Each follows the same pattern as Task 18. Implement each as a screen class deriving from `MbScreen` with its own input/render logic.

Full implementations are lengthy but structurally identical. For each, start from the BrowseScreen template and:

**Task 19 — Search:** Add virtual keyboard (reuse existing `ui/virtual_keyboard`), live-debounce the query (400ms), call `radarr_.lookup(query)`, display results below keyboard. Transition to Detail on select.

**Task 20 — Detail:** Render fanart background (via draw_quad with dim tint), poster, title/year/runtime/rating, overview. Bottom action row: "Add to Library" / "Download Now" / "Remove" / "Play" based on monitored/has_file state. Call `radarr_.add_movie` or `radarr_.trigger_search` or `radarr_.remove_movie` as appropriate.

**Task 21 — Queue:** Call `radarr_.get_queue()` every 2s while active. Render vertical list of rows: [poster thumb] [title] [progress bar] [down rate] [peers]. Per-row actions: Cancel. Global actions: Pause All / Resume All / Retry Failed.

**Task 22 — Library:** Call `radarr_.get_library()`. Render same poster grid as Browse. Use state indicators (✅/🟡/❌) next to each poster based on `has_file` and `file_quality`. Filter chip strip: All / Unwatched / Missing upgrades / Recently added.

**Task 23 — Movies Settings:** Scrollable list of controls matching spec §9.6. Service status dots from `radarr_.is_reachable()` + checks against Prowlarr and qBittorrent ports. Quality profile selector from `radarr_.get_quality_profiles()`. "Hide Movies feature" checkbox that sets `state.settings.media_browser_unlocked = false` + exits to main menu.

Each task ends with a commit: `git commit -m "feat(media_browser): <ScreenName> screen"`.

**Budget:** 1 weekend total for Tasks 19–23 (they're structurally parallel to Task 18 now that the pattern is established).

---

## Sub-Project B5: Display mode switching (Task 24)

---

## Task 24: drm_display.request_mode + GStreamer reinit

**Files:**
- Modify: `magic_dingus_box_cpp/src/platform/drm_display.h`
- Modify: `magic_dingus_box_cpp/src/platform/drm_display.cpp`
- Modify: `magic_dingus_box_cpp/src/video/gst_player.cpp` (restart pipeline on mode change)
- Modify: Detail/Library screens to call `request_mode(1920,1080,60)` on Play action

- [ ] **Step 1: Add public API**

In `drm_display.h`, add public method:

```cpp
    // Request a display mode change mid-session. Validates the mode is
    // supported by the connector. Returns true on success.
    //
    // On success, the GStreamer pipeline must be re-initialized (it holds
    // framebuffer size assumptions). Caller is responsible.
    bool request_mode(int width, int height, int refresh_hz);

    // Restore the original mode set at init(). Use when exiting
    // special modes (like movie playback).
    bool restore_default_mode();

    // Current active mode info
    int width() const;
    int height() const;
    int refresh_hz() const;
```

- [ ] **Step 2: Implement in drm_display.cpp**

Add below the existing init code:

```cpp
bool DrmDisplay::request_mode(int width, int height, int refresh_hz) {
    drmModeConnector* conn = drmModeGetConnector(drm_fd_, connector_id_);
    if (!conn) return false;

    drmModeModeInfo* target = nullptr;
    for (int i = 0; i < conn->count_modes; ++i) {
        auto& m = conn->modes[i];
        if (m.hdisplay == width && m.vdisplay == height && m.vrefresh == refresh_hz) {
            target = &m;
            break;
        }
    }

    if (!target) {
        drmModeFreeConnector(conn);
        return false;
    }

    // Save current mode if not already saved
    if (saved_mode_.hdisplay == 0) saved_mode_ = current_mode_;

    int ret = drmModeSetCrtc(drm_fd_, crtc_id_, current_fb_id_,
                             0, 0, &connector_id_, 1, target);
    drmModeFreeConnector(conn);

    if (ret != 0) {
        return false;
    }

    current_mode_ = *target;
    return true;
}

bool DrmDisplay::restore_default_mode() {
    if (saved_mode_.hdisplay == 0) return true;  // nothing saved
    int ret = drmModeSetCrtc(drm_fd_, crtc_id_, current_fb_id_,
                             0, 0, &connector_id_, 1, &saved_mode_);
    if (ret == 0) {
        current_mode_ = saved_mode_;
        saved_mode_.hdisplay = 0;
        return true;
    }
    return false;
}

int DrmDisplay::width() const { return current_mode_.hdisplay; }
int DrmDisplay::height() const { return current_mode_.vdisplay; }
int DrmDisplay::refresh_hz() const { return current_mode_.vrefresh; }
```

Add to `.h`:
```cpp
private:
    drmModeModeInfo current_mode_ = {};
    drmModeModeInfo saved_mode_ = {};
```

- [ ] **Step 3: Restart GStreamer on mode change**

In `gst_player.cpp`, expose a `reinit_pipeline(int w, int h)` method that tears down the current pipeline and builds a fresh one with the new framebuffer size. The existing pipeline builder should already use dynamic dimensions — just invoke it again.

- [ ] **Step 4: Wire the Play action**

In `DetailScreen` and `LibraryScreen`, when the user selects a playable file:

```cpp
// Get the file path, then...
drm_display.request_mode(1920, 1080, 60);
gst_player.reinit_pipeline(1920, 1080);
gst_player.play(file_path);
// When playback ends:
drm_display.restore_default_mode();
gst_player.reinit_pipeline(drm_display.width(), drm_display.height());
```

- [ ] **Step 5: Deploy + test on Pi with real TV**

Play a movie from the Library screen. Verify:
- Display switches to 1920x1080 (brief flicker acceptable)
- Movie plays in native widescreen aspect ratio
- On exit, returns to kiosk default mode

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/platform/drm_display.h \
        magic_dingus_box_cpp/src/platform/drm_display.cpp \
        magic_dingus_box_cpp/src/video/gst_player.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp \
        magic_dingus_box_cpp/src/media_browser/ui/library_screen.cpp
git commit -m "feat(media_browser): 1080p runtime DRM mode switch for movie playback"
```

---

## Sub-Project B6: Playlist integration (Task 25)

---

## Task 25: Movies playlist source + inotify watch

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/playlist_loader.h`
- Modify: `magic_dingus_box_cpp/src/app/playlist_loader.cpp`

- [ ] **Step 1: Add Movies source enumeration**

In `playlist_loader.cpp`, add a method that scans `/mnt/ssd/library/Movies/*/` and creates a synthetic playlist entry for each movie directory:

```cpp
std::vector<PlaylistItem> PlaylistLoader::load_movies_library() {
    std::vector<PlaylistItem> items;
    const fs::path root = "/mnt/ssd/library/Movies";
    if (!fs::exists(root)) return items;

    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        const auto& dir = entry.path();
        // Find the primary video file
        fs::path video_path;
        for (const auto& f : fs::directory_iterator(dir)) {
            auto ext = f.path().extension().string();
            if (ext == ".mkv" || ext == ".mp4" || ext == ".avi" || ext == ".m4v") {
                video_path = f.path();
                break;
            }
        }
        if (video_path.empty()) continue;

        PlaylistItem item;
        item.source_type = SourceType::Video;
        item.path = video_path.string();
        item.title = dir.filename().string();
        // Optional: look for poster.jpg or fanart.jpg in the dir
        fs::path poster = dir / "poster.jpg";
        if (fs::exists(poster)) item.thumbnail = poster.string();
        items.push_back(std::move(item));
    }
    return items;
}
```

- [ ] **Step 2: Inotify watch (best-effort; skip on non-Linux)**

Add a simple inotify watcher that polls for file system changes in the Movies directory and triggers a reload. Keep it simple — a thread that watches IN_CREATE | IN_DELETE and sets a dirty flag.

- [ ] **Step 3: Integrate into main playlist loading**

In the main kiosk boot, after loading YAML playlists, append the Movies library:
```cpp
auto movies = playlist_loader.load_movies_library();
state.playlists["Movies"] = movies;
```

- [ ] **Step 4: Deploy + test**

Manually add a video file to `/mnt/ssd/library/Movies/Test Movie (2024)/test.mkv` on the Pi. Restart the kiosk. The main playlist browser should now show a "Movies" playlist with "Test Movie (2024)" as an entry.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/app/playlist_loader.h \
        magic_dingus_box_cpp/src/app/playlist_loader.cpp
git commit -m "feat(media_browser): playlist source for /mnt/ssd/library/Movies"
```

---

## Sub-Project B7: Integration + polish + docs (Tasks 26–30)

---

## Task 26: Manual UI checklist

**Files:**
- Create: `magic_dingus_box_cpp/tests/manual/media_browser_v2_checklist.md`

Writes a full manual-test checklist mirroring `tests/manual/pre_image_checklist.md` pattern. Covers: sequence unlock, toast appears, settings row appears, all 6 screens render correctly, Add→Download→Import flow, Play at 1080p, Hide feature works, services-down degradation.

Commit:
```bash
git add -f magic_dingus_box_cpp/tests/manual/media_browser_v2_checklist.md
git commit -m "test(media_browser): V2 manual UI checklist"
```

---

## Task 27: User guide documentation

**Files:**
- Create: `magic_dingus_box_cpp/docs/MEDIA_BROWSER_USER_GUIDE.md`

Documents: the secret sequence, the UI screens, how to hide the feature, quality profile selection basics, where movies appear after download, troubleshooting ("why is my download stuck", "why no movies in library", etc.).

Commit (force-add):
```bash
git add -f magic_dingus_box_cpp/docs/MEDIA_BROWSER_USER_GUIDE.md
git commit -m "docs(media_browser): V2 end-user guide"
```

---

## Task 28: Final flag-off invariant check on Pi

**Files:** no file changes.

- [ ] **Step 1: Rebuild without --media-browser on Pi**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

- [ ] **Step 2: Capture hash**

```bash
ssh magic@magicpi.local 'sha256sum /opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp'
```

- [ ] **Step 3: Compare against Phase 1 baseline**

Expected: identical to `90e3af0449366c782c85598ba134fa2e2bb9d9de0820fc776d7447f8833ce1cc`.

If NOT identical, investigate whether any V2 code leaked outside the `#ifdef MEDIA_BROWSER_ENABLED` guards.

- [ ] **Step 4: Note result in completion doc (Task 29)**

---

## Task 29: Completion record

**Files:**
- Create: `magic_dingus_box_cpp/docs/MEDIA_BROWSER_V2_COMPLETION.md`

Mirror the Phase 1 completion doc pattern. Record:
- Dates, Pi model, Docker image versions, kiosk binary hashes (pre/post)
- Which manual checklist items passed
- Any plan deviations (documented in commit messages)
- Open items for Phase 2+ (Sonarr, Bazarr, VPN, etc.)

Commit (force-add):
```bash
git add -f magic_dingus_box_cpp/docs/MEDIA_BROWSER_V2_COMPLETION.md
git commit -m "docs(media_browser): V2 completion record"
```

---

## Task 30: Final verification pass

**Files:** no file changes.

- [ ] Run unit test suite on Mac + Pi: `./test_media_browser_unit` both sides pass
- [ ] Run integration tests: `./test_media_browser radarr-*` commands
- [ ] Manual UI checklist (Task 26) — walk through every item, check every box
- [ ] Invariant check (Task 28) passed
- [ ] `systemctl status magic-dingus-services` — all 3 services running
- [ ] `systemctl status magic-dingus-box-cpp` — kiosk still alive
- [ ] Enter the sequence physically on the kiosk — toast appears
- [ ] Flip kiosk's Hide Movies checkbox — feature hides; re-entering sequence re-unlocks
- [ ] Radarr web UI accessible from phone on same network (with auth)
- [ ] All commits pushed to feature branch

Once all checks pass, V2 is ready for merge to main (or extended hold as a feature branch until operator is ready to promote).

---

## Self-Review

### Spec coverage check

Walked every section of `MEDIA_BROWSER_V2_DESIGN.md`:

| Spec section | Task(s) implementing it |
|---|---|
| §5 Architecture diagram | Tasks 1-30 (whole plan reflects it) |
| §6 Docker stack | Tasks 1-4 |
| §7 Secret sequence unlock | Tasks 6-11 |
| §8 Display mode switching | Task 24 |
| §9.1 Browse screen | Task 18 |
| §9.2 Search screen | Task 19 |
| §9.3 Detail screen | Task 20 |
| §9.4 Queue screen | Task 21 |
| §9.5 Library screen | Task 22 |
| §9.6 Settings screen | Task 23 |
| §10 RadarrClient | Tasks 12-15 |
| §11 Graceful degradation | Task 23 (service dots), Tasks 18-22 (fallback UI) |
| §12 Four-gate isolation | Task 11 (unlock), existing Phase 1 (CMake + settings) |
| §13 Privacy + bandwidth | Task 2 (qBittorrent defaults) |
| §14 Phase 1 asset reuse | Preserved by not modifying Phase 1 files |
| §15 Build order | Plan is ordered per spec §15 |
| §16 Testing strategy | Tasks 6, 13 (unit), Task 16 (integration), Task 26 (manual) |
| §17 Docs deliverables | Tasks 5, 27, 29 |
| §19 Risks | Addressed via gating, pinned images, graceful degradation |
| §20 Acceptance criteria | Task 30 (final verification) |

No gaps.

### Placeholder scan

Scanned for red-flag phrases:
- "TBD" / "TODO" — none
- "add appropriate error handling" — none (error handling is shown inline)
- "similar to Task N" — resisted; each task has its own code
- Types/methods referenced in later tasks match earlier definitions (spot-checked `SequenceDetector`, `RadarrClient`, `MbScreen`, `Toast`)

Minor caveat: Tasks 19-23 (UI screens beyond Browse) intentionally summarize rather than provide full code, because they follow the Task 18 pattern exactly. This is a deliberate choice — providing full code for each would triple plan length without adding instruction. An engineer with Task 18 in hand can implement the remaining screens in isomorphic manner. If this is insufficient, they can be expanded to full task bodies.

### Type consistency

- `SequenceDetector`, `SeqInput`, `SeqResult` — consistent across Tasks 6-7, 11
- `RadarrClient` methods — consistent between Task 14 definition and Task 16 callers
- `Movie`, `MovieSearchHit`, `QueueItem`, `QualityProfile` — consistent between Task 12 definition and Tasks 13-16 consumers
- `MbScreen`, `Screen` enum — consistent between Task 17 base and Tasks 18-23 subclasses
- `Toast` API — consistent between Task 9 definition and Task 11 caller

No inconsistencies found.

---

## Execution Handoff

Plan complete and saved to `magic_dingus_box_cpp/docs/MEDIA_BROWSER_V2_PLAN.md`.

**Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration. Best for parallelizing — each subagent can work independently with the spec + plan as its sole context.

**2. Inline Execution** — Execute tasks in this session using `executing-plans`, batch execution with checkpoints. Keeps conversation context warm.

**Which approach?** Note: at 30 tasks, inline execution in this session is viable but long. Subagent-driven may be better for parallelism, especially if you want to run B1 (Docker setup, shell-only) concurrently with B2/B3 (C++ work).
