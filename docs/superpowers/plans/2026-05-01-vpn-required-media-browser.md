# VPN-Required Media Browser Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Media Browser a strictly VPN-required feature. All torrent-ecosystem services route through Gluetun; Pi host has IPv6 disabled and DNS via Cloudflare DoH; web admin and kiosk both gate Media Browser visibility behind a three-layer check (unlocked + VPN configured + tunnel healthy).

**Architecture:** Three independent gates, each with one job. Layer 1 (`media_browser_unlocked`) gates UI visibility. Layer 2 (`WIREGUARD_PRIVATE_KEY` non-empty in `services/.env`) gates functional endpoints + kiosk MB launch. Layer 3 (Radarr `/ping` reachable on `127.0.0.1:7878`) gates runtime use. Compose moves Prowlarr/Radarr/Byparr behind Gluetun via shared netns; setup script applies host-network changes idempotently.

**Tech Stack:** Docker Compose v3, Gluetun (qmcgaw/gluetun:v3) + WireGuard, Prowlarr 2.3.5.5327, Radarr 5.14.0, Byparr (replaces FlareSolverr), qBittorrent 5.0.3, Cloudflared (DoH proxy), Flask (web admin), C++17 + libcurl + Catch2 v3 (kiosk).

**Companion spec:** [`docs/superpowers/specs/2026-05-01-vpn-required-media-browser-design.md`](../specs/2026-05-01-vpn-required-media-browser-design.md)

**Branch:** `feature/vpn-required-media-browser` (already created off main, with the spec doc already committed as `9ca1107`).

**Build context:** All Pi-side testing uses `PI_HOST=magic@magicpi.local` (or `magic@10.55.0.1` over USB gadget). Unit tests run on the dev machine via `cmake --build build --target test_media_browser_unit && ./build/test_media_browser_unit`. Compose validation can run locally without the Pi via `docker compose -f magic_dingus_box_cpp/services/docker-compose.yml config`.

---

## Phase 1 — Documentation foundation

### Task 1: Operator setup guide (MEDIA_BROWSER_VPN_SETUP.md)

**Files:**
- Create: `magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md`

This is operator-facing reference, written first so subsequent tasks can link to it.

- [ ] **Step 1: Create the doc file with full content**

```markdown
# Media Browser VPN Setup

Magic Dingus Box's Media Browser (movie discovery, downloads, playback)
requires a working VPN. Without one, the feature is hidden from both
the kiosk UI and the web Content Manager.

## Why VPN is required

The Media Browser uses BitTorrent for movie acquisition. Even when no
peer-to-peer traffic is happening, the indexer searches and metadata
pulls reveal which trackers your Pi talks to. Magic Dingus Box closes
this leak by routing every torrent-ecosystem service (Prowlarr,
Radarr, qBittorrent, and the Cloudflare-bypass service Byparr)
through Gluetun's WireGuard tunnel. Your ISP sees only "this Pi
connects to a single VPN endpoint."

## What you need

- **ProtonVPN account, Plus tier or higher.** Free-tier ProtonVPN
  doesn't support port forwarding, which kills BitTorrent peer
  connectivity. Other WireGuard providers (Mullvad, IVPN) are not
  currently supported by Magic Dingus Box's setup workflow.
- **A WireGuard config file (.conf) downloaded from the ProtonVPN
  dashboard.** When generating it, you MUST enable the "NAT-PMP /
  Port Forwarding" toggle. Without it, peer connections fail.
- **Network access to the Pi's Content Manager** (`http://magicpi.local:5000`
  or `http://10.55.0.1:5000` over USB gadget).

## Step-by-step

1. **Unlock the Media Browser on the kiosk.** With the kiosk running,
   enter the secret sequence: BTN1+BTN3 chord → BTN2 × 3 → rotary
   click. A toast confirms the unlock. The flag persists across
   reboots.

2. **Open Content Manager and refresh.** The "Media Browser" tab
   appears in the top nav. Click it. You see a "Set up VPN" form
   because no WireGuard config has been dropped yet.

3. **Download your WireGuard config from ProtonVPN.** Go to
   protonvpn.com → Account → WireGuard → Create. Pick a Netherlands
   server (Magic Dingus Box defaults to NL endpoints). Toggle
   "NAT-PMP / Port Forwarding: ON". Download the `.conf` file.

4. **Drop the config into the Set up VPN form.** Either drag the
   `.conf` onto the upload zone or paste its contents. Click
   "Configure VPN".

5. **Wait ~90 seconds.** The setup job streams progress. It does
   the following in order:
   - Applies host network changes (IPv6 disable, DoH DNS).
   - Pulls the Byparr image from ghcr.io (with retries; ghcr.io
     DNS can be flaky on first boot).
   - Starts Gluetun and waits for the WireGuard tunnel to come up
     (verified by hitting Gluetun's `/v1/publicip/ip` internal
     endpoint).
   - Starts Prowlarr, Radarr, Byparr, qBittorrent — all sharing
     Gluetun's network namespace.
   - Pushes the Custom Format / indexer / Apps integration / qBit
     download client config from the JSON fixtures.

6. **Verify success.** When the job completes, the dashboard shows
   "All services healthy" and reports the VPN exit IP and country.
   Refresh the kiosk's Settings menu — Media Browser entries now
   appear.

## Troubleshooting

### Tunnel won't come up

Symptom: setup job exits at "Gluetun tunnel did not come up in 120s".

Likely causes:
- WireGuard config has expired or been revoked from your ProtonVPN
  dashboard. Generate a fresh one.
- WireGuard endpoint IP unreachable from your network (some ISPs
  block UDP/51820 outbound). Try a different ProtonVPN server.
- NAT-PMP toggle was OFF when the config was generated. Check by
  looking at the config file — the `Endpoint = ` line should
  reference a port-forwarding-enabled server.

Recovery: re-run the setup with a corrected config. The setup
script is idempotent.

### NAT-PMP port reads as 0 in the dashboard

Symptom: "VPN forwarded port: 0" persistently shown after the tunnel
is up.

Likely causes:
- NAT-PMP toggle was OFF when generating the WireGuard config.
- Gluetun's `FIREWALL_OUTBOUND_SUBNETS` accidentally includes
  `10.0.0.0/8`, which routes NAT-PMP requests out the LAN instead
  of through the tunnel. Magic Dingus Box's compose explicitly
  avoids this; the bug only appears if someone manually edited
  the compose file.

The 60-second qbit-port-sync.timer will pick up the correct port as
soon as Gluetun's NAT-PMP service leases one. Wait up to 2 minutes.

### Byparr image won't pull (ghcr.io DNS failure)

Symptom: setup job exits at "cannot pull byparr after 3 attempts".

Cause: ghcr.io DNS sometimes fails on a fresh Pi before DoH is fully
warmed up. The setup script retries 3× with 10s sleep; if all three
fail, it aborts.

Recovery: wait a minute, then re-run setup from the Content Manager.
By the second run, DoH is live and ghcr.io resolves cleanly.

### All indexers show 0 results

Symptom: Media Browser detail screen shows no available releases for
any movie.

Likely causes:
- Cloudflare-fronted indexers (1337x, TheRARBG) require Byparr.
  Check Byparr is running: `docker ps | grep byparr`. If absent,
  re-run setup.
- Indexer site is genuinely down or has changed its API. Check the
  Prowlarr UI directly (`http://magicpi.local:9696` — requires SSH
  tunnel since it's loopback-bound) for per-indexer health.

## Privacy notes

**What the ISP can't see** after this setup:

- Indexer searches (Prowlarr's queries to 1337x, YTS, etc. all exit
  via the VPN).
- Movie metadata fetched by Radarr (Radarr's TMDB calls exit via VPN
  because Radarr is in Gluetun's netns).
- BitTorrent peer connections (qBittorrent has always been behind
  Gluetun).
- DNS queries from the Pi host (DoH via Cloudflare encrypts these).

**What the ISP can still see** (accepted gaps):

- The kiosk binary's own TMDB calls. Magic Dingus Box's main
  application process — the kiosk binary itself — calls
  `api.themoviedb.org` directly from the host network when browsing
  movies in the kiosk UI. This is metadata only (poster fetches,
  search queries) and never touches torrent indexers, but it does
  reveal "this Pi looks at TMDB." Routing through Radarr's
  metadata proxy is future work.
- The TLS Server Name Indication (SNI) for any non-VPN'd outbound
  TLS connection. DoH hides DNS, but the SNI in the TLS handshake
  is still cleartext until ECH is widely supported. Affects: TMDB
  calls from the kiosk binary, OTA update GitHub fetches, the
  cloudflared connection itself.

For a deeper threat model write-up, see
`MEDIA_BROWSER_PLAYBACK_AND_DOWNLOADS.md` "Architecture" section.
```

- [ ] **Step 2: Verify the file renders cleanly in your editor**

Run: `head -40 magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md`
Expected: clean Markdown with no broken codeblock fences.

- [ ] **Step 3: Add a .gitignore exception so this doc IS tracked in the repo**

The repo's root `.gitignore` excludes `docs/` (`# Runtime-only: exclude
docs and markdown from repo`). All other docs in `magic_dingus_box_cpp/docs/`
are intentionally Pi-local. But this file is operator-facing — operators
must read it before they have a Pi running, so it has to be in the repo
where the README can link to it.

Edit `.gitignore` (root). Find the existing `!docs/superpowers/...`
exception block and add a new exception below it:

```
# Operator-facing setup doc — must be tracked so README can link to it.
!magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md
```

Verify: `git check-ignore -v magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md`
Expected: exits non-zero (meaning the file is NOT ignored — exception
took effect). Earlier the same command would have printed the
`docs/` rule, indicating the file WAS ignored.

- [ ] **Step 4: Commit**

```bash
git add .gitignore magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md
git commit -m "docs(mb): operator guide for VPN-required Media Browser setup"
```

---

### Task 2: Update CLAUDE.md framing

**Files:**
- Modify: `CLAUDE.md` (Media Browser section, currently around lines 211-265)

The existing section frames the Media Browser as "hidden by default behind a kiosk-side secret-sequence unlock." We're upgrading this to three-layer gating.

- [ ] **Step 1: Read the current Media Browser section**

Run: `grep -n "Feature gating\|media_browser_unlocked\|FlareSolverr" CLAUDE.md | head -20`
Note the line numbers; you'll edit those subsections.

- [ ] **Step 2: Replace the "Feature gating" subsection**

Find the section starting with `### Feature gating` and ending before `### Per-Pi setup workflow`. Replace its body with:

```markdown
### Feature gating

The Media Browser is **VPN-required and hidden by default**, gated
by three independent layers:

1. **Unlocked** — `playback.media_browser_unlocked` flag in
   `config/settings.json`, set by the kiosk-side secret sequence
   (BTN1+BTN3 chord → BTN2 × 3 → rotary click). Gates UI
   *visibility*: when locked, the Settings-menu entry and the web
   admin tab are hidden entirely.
2. **VPN configured** — `WIREGUARD_PRIVATE_KEY` non-empty in
   `services/.env`. Gates *functional* `/admin/media-browser/*`
   endpoints and the kiosk's MB launch path. The Content Manager
   tab is visible at Layer 1 alone (so the operator can drop a
   WireGuard config); the inner functions require Layer 2.
3. **Tunnel healthy** — Radarr `/ping` reachable on
   `localhost:7878`. Polled every 10s by the kiosk's
   `VpnHealthMonitor`; three consecutive failures (~30s) flips the
   in-memory flag and hides MB entries with a "tunnel down" toast.
   Recovery is silent on the first successful poll.

All four torrent-ecosystem services (Prowlarr, Radarr, Byparr,
qBittorrent) share Gluetun's network namespace. When Gluetun is
down, all four are unreachable from the host — Radarr ping is the
single signal that covers the stack.

Cloned Pis start LOCKED — `first_boot.sh` Step 6 resets the unlock
flag during first-boot setup so a fresh Pi inherits no unlock
state from the source.
```

- [ ] **Step 3: Update "Active indexers" subsection**

Find `**Active indexers** (Prowlarr → Radarr):` line. Change the inline
mention of `FlareSolverr` to `Byparr`:

Before:
```
TPB, YTS, LimeTorrents, TorrentDownload (with `cloudflare` tag → FlareSolverr).
```
After:
```
TPB, YTS, LimeTorrents, TorrentDownload (with `cloudflare` tag → Byparr,
which replaces FlareSolverr for current Cloudflare challenge formats).
```

- [ ] **Step 4: Add privacy-notes one-liner under Feature gating**

Append to the Feature gating subsection:

```markdown

**Privacy gap (accepted):** the kiosk binary's own TMDB calls exit
via the host network, not via Gluetun, because the C++ binary runs
outside Docker. Metadata only — never touches torrent indexers. See
[MEDIA_BROWSER_VPN_SETUP.md](magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md)
"Privacy notes" for the full threat model.
```

- [ ] **Step 5: Update Gluetun setup invariant**

In the "Service operations" subsection, find the line about FlareSolverr
and update it to Byparr where applicable. Keep the rest of the operations
notes as-is (Gluetun setup, qbit-port-sync.timer, etc.).

- [ ] **Step 6: Verify the file**

Run: `grep -c "FlareSolverr\|flaresolverr" CLAUDE.md`
Expected: 0 standalone references (one historical mention "replaces FlareSolverr" is OK).

Run: `grep -c "Byparr\|byparr" CLAUDE.md`
Expected: at least 2.

- [ ] **Step 7: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(claude.md): three-layer gating, Byparr replaces FlareSolverr"
```

---

### Task 3: Rewrite README.md top-of-file

**Files:**
- Modify: `README.md`

Lead with the two-halves narrative.

- [ ] **Step 1: Read the current README**

Run: `head -50 README.md`
Note where the existing intro ends.

- [ ] **Step 2: Replace the top section (everything before the first `##` after the title)**

```markdown
# Magic Dingus Box

A retro gaming and video playback kiosk for Raspberry Pi 4B.

Magic Dingus Box has two halves:

1. **Retro gaming + video playback** — always works, no internet
   required after setup. Plays NES / SNES / Genesis / PS1 / PCE /
   Atari 7800 / Arcade games via RetroArch, plus local videos and
   YouTube clips.

2. **Movie Media Browser** — discovers and downloads movies via a
   Radarr / Prowlarr / qBittorrent stack. **Requires a VPN**
   (ProtonVPN with WireGuard recommended). See
   [docs/MEDIA_BROWSER_VPN_SETUP.md](magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md)
   for setup.

Without a VPN configured, the Media Browser is fully hidden from
both the kiosk UI and the web Content Manager. Operators must
explicitly unlock it (kiosk-side secret sequence) *and* drop a
working WireGuard config (web admin) before the feature appears.
```

Keep the rest of the README (build instructions, deployment, etc.) as-is.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs(readme): lead with two-halves narrative; VPN required for MB"
```

---

## Phase 2 — Docker stack changes

### Task 4: Move Prowlarr behind Gluetun in compose

**Files:**
- Modify: `magic_dingus_box_cpp/services/docker-compose.yml`

- [ ] **Step 1: Read the current Prowlarr block**

Run: `grep -n "prowlarr:" magic_dingus_box_cpp/services/docker-compose.yml`
Note the start line of the Prowlarr service block.

- [ ] **Step 2: Edit the Prowlarr block**

Replace the Prowlarr service definition with:

```yaml
  prowlarr:
    image: lscr.io/linuxserver/prowlarr:2.3.5.5327
    container_name: mdb_prowlarr
    # Behind Gluetun: shares VPN netns. DNS handled by gluetun, no
    # per-service dns: block needed. No per-service ports: block —
    # Prowlarr's :9696 is exposed via gluetun's ports: section.
    network_mode: "service:gluetun"
    depends_on:
      gluetun:
        condition: service_healthy
    environment:
      - PUID=${PUID}
      - PGID=${PGID}
      - TZ=${TZ}
    volumes:
      - ./config/prowlarr:/config
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://localhost:9696/ping"]
      interval: 30s
      timeout: 5s
      retries: 3
```

This change does three things at once:
- Bumps image `1.26.1` → `2.3.5.5327` (refreshes Cardigann definitions).
- Adds `network_mode: "service:gluetun"` and `depends_on: gluetun: { condition: service_healthy }`.
- Drops the `dns:` and `ports:` blocks (handled by gluetun).

- [ ] **Step 3: Validate compose syntax**

Run: `docker compose -f magic_dingus_box_cpp/services/docker-compose.yml config > /dev/null`
Expected: exits 0 with no output (valid syntax). Note: this won't
catch all errors (e.g., gluetun healthcheck timing) — full validation
happens on the Pi.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/services/docker-compose.yml
git commit -m "feat(mb): move Prowlarr behind Gluetun, bump 1.26.1 -> 2.3.5.5327"
```

---

### Task 5: Move Radarr behind Gluetun in compose

**Files:**
- Modify: `magic_dingus_box_cpp/services/docker-compose.yml`

- [ ] **Step 1: Replace the Radarr service definition with**

```yaml
  radarr:
    image: lscr.io/linuxserver/radarr:5.14.0
    container_name: mdb_radarr
    # Behind Gluetun: shares VPN netns. Even Radarr's TMDB metadata
    # fetches now exit via VPN (was direct via Comcast residential IP).
    network_mode: "service:gluetun"
    depends_on:
      gluetun:
        condition: service_healthy
    environment:
      - PUID=${PUID}
      - PGID=${PGID}
      - TZ=${TZ}
    volumes:
      - ./config/radarr:/config
      - ${STORAGE_ROOT}/downloads:/downloads
      - ${STORAGE_ROOT}/library:/library
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://localhost:7878/ping"]
      interval: 30s
      timeout: 5s
      retries: 3
```

Same pattern as Prowlarr: drops `dns:` and `ports:` blocks; adds netns
+ depends_on.

- [ ] **Step 2: Validate compose syntax**

Run: `docker compose -f magic_dingus_box_cpp/services/docker-compose.yml config > /dev/null`
Expected: exits 0.

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box_cpp/services/docker-compose.yml
git commit -m "feat(mb): move Radarr behind Gluetun"
```

---

### Task 6: Replace FlareSolverr with Byparr

**Files:**
- Modify: `magic_dingus_box_cpp/services/docker-compose.yml`

The Byparr digest must be resolved at task-execution time. Use the
following command to fetch it:

```bash
docker buildx imagetools inspect ghcr.io/thephaseless/byparr:latest \
  --format '{{json .Manifest.Digest}}'
```

If `docker buildx` isn't available locally, run on the Pi:

```bash
ssh magic@magicpi.local 'docker pull ghcr.io/thephaseless/byparr:latest \
  && docker inspect ghcr.io/thephaseless/byparr:latest --format "{{index .RepoDigests 0}}"'
```

- [ ] **Step 1: Resolve the digest**

Record the digest in your shell history (e.g.,
`sha256:abc123...`). Substitute `<DIGEST>` in the next step with this
literal value.

- [ ] **Step 2: Replace the `flaresolverr:` service block with `byparr:`**

```yaml
  byparr:
    # Drop-in FlareSolverr-API replacement that handles current
    # Cloudflare challenge formats (FlareSolverr's last release v3.4.6
    # cannot). API-compatible — Prowlarr's "FlareSolverr" indexer-proxy
    # configuration in prowlarr_indexerproxies.json continues to work
    # because the proxy's *name* is unchanged; only the URL points to
    # byparr now.
    image: ghcr.io/thephaseless/byparr@<DIGEST>
    container_name: mdb_byparr
    network_mode: "service:gluetun"
    depends_on:
      gluetun:
        condition: service_healthy
    environment:
      - LOG_LEVEL=info
      - TZ=${TZ}
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "curl", "-fsS", "--max-time", "5", "http://localhost:8191/"]
      interval: 60s
      timeout: 10s
      retries: 3
```

- [ ] **Step 3: Add ports to gluetun's existing `ports:` block**

Find the `gluetun:` service block. Add `127.0.0.1:7878:7878`,
`127.0.0.1:9696:9696`, and `127.0.0.1:8191:8191` to its `ports:` list.
Keep existing entries.

```yaml
    ports:
      # qBit web UI (existing)
      - "127.0.0.1:8080:8080"
      # BitTorrent peer port (existing)
      - "6881:6881"
      - "6881:6881/udp"
      # Behind-Gluetun service admin/internal ports.
      # All loopback-bound; admin UI requires SSH tunnel.
      - "127.0.0.1:7878:7878"   # Radarr
      - "127.0.0.1:9696:9696"   # Prowlarr
      - "127.0.0.1:8191:8191"   # Byparr (Cloudflare bypass)
```

- [ ] **Step 4: Validate compose syntax**

Run: `docker compose -f magic_dingus_box_cpp/services/docker-compose.yml config > /dev/null`
Expected: exits 0.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/services/docker-compose.yml
git commit -m "feat(mb): replace FlareSolverr with Byparr; absorb ports on gluetun"
```

---

### Task 7: Update fixture URLs (Apps integration)

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/data/prowlarr_applications.json`

When Prowlarr enters Gluetun's netns, it reaches Radarr at `localhost`
(same netns) instead of via the Docker bridge hostname `radarr`.

- [ ] **Step 1: Read the current file**

Run: `cat magic_dingus_box_cpp/scripts/data/prowlarr_applications.json`
Locate the Radarr URL field — it's likely `"baseUrl": "http://radarr:7878"`
or a similar key.

- [ ] **Step 2: Replace `http://radarr:7878` with `http://localhost:7878`**

Use the Edit tool to perform the replacement on the exact line. Don't
edit other URLs.

- [ ] **Step 3: Validate JSON**

Run: `python3 -m json.tool magic_dingus_box_cpp/scripts/data/prowlarr_applications.json > /dev/null`
Expected: exits 0 (valid JSON).

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/data/prowlarr_applications.json
git commit -m "fix(mb): point Prowlarr Apps integration at localhost (shared netns)"
```

---

### Task 8: Update fixture URLs (FlareSolverr-API proxy)

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/data/prowlarr_indexerproxies.json`

- [ ] **Step 1: Read the current file**

Run: `cat magic_dingus_box_cpp/scripts/data/prowlarr_indexerproxies.json`
Locate the proxy URL — likely `"host": "http://flaresolverr:8191"` or
similar.

- [ ] **Step 2: Replace `http://flaresolverr:8191` with `http://localhost:8191`**

The proxy's *name* (likely `"name": "flaresolverr"`) MUST stay unchanged.
Only the URL changes. This keeps Prowlarr's indexer→tag bindings valid.

- [ ] **Step 3: Validate JSON**

Run: `python3 -m json.tool magic_dingus_box_cpp/scripts/data/prowlarr_indexerproxies.json > /dev/null`
Expected: exits 0.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/data/prowlarr_indexerproxies.json
git commit -m "fix(mb): point FlareSolverr-API proxy URL at localhost (shared netns)"
```

---

### Task 9: Update fixture URLs (qBit download client)

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/data/radarr_downloadclients.json`

When Radarr enters Gluetun's netns, qBittorrent (also in the netns)
is at `localhost:8080` instead of `gluetun:8080`.

- [ ] **Step 1: Read the current file**

Run: `cat magic_dingus_box_cpp/scripts/data/radarr_downloadclients.json`
Locate the qBit host field — likely `"host": "gluetun"` or a similar key.

- [ ] **Step 2: Replace `"gluetun"` with `"localhost"` in the host field only**

Don't replace other strings (e.g., container names). Edit just the
download client `host` value.

- [ ] **Step 3: Validate JSON**

Run: `python3 -m json.tool magic_dingus_box_cpp/scripts/data/radarr_downloadclients.json > /dev/null`
Expected: exits 0.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/data/radarr_downloadclients.json
git commit -m "fix(mb): point Radarr -> qBit download client at localhost (shared netns)"
```

---

## Phase 3 — Setup script changes

### Task 10: Add new Step 0 (host networking) to setup_services.sh

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh`

- [ ] **Step 1: Add the `--skip-host-networking` flag handler near the top**

Just below `set -euo pipefail` (line 17), add:

```bash
SKIP_HOST_NETWORKING=0
for arg in "$@"; do
    case "$arg" in
        --skip-host-networking) SKIP_HOST_NETWORKING=1 ;;
    esac
done
```

- [ ] **Step 2: Add Step 0 immediately after the flag-handling block, before the existing `=== Media Browser V2 service setup ===` echo**

```bash
# 0. Pi host networking — close IPv6 + DNS leak paths.
#
# Why: ProtonVPN's WireGuard tunnel is IPv4-only. When the Pi prefers
# IPv6 outbound (Comcast hands out v6 by default), routes for v6
# destinations bypass the tunnel entirely. Disable IPv6 globally so
# every outbound connection takes the v4 path through Gluetun (or
# the host's own non-VPN'd v4 path for kiosk/OTA traffic, which is
# the documented accepted gap).
#
# DNS: route host DNS through Cloudflare DoH. Plain `nameserver 1.1.1.1`
# would still leak query domains over UDP/53. DoH encrypts to
# 1.1.1.1:443 so the ISP sees only opaque TLS.
#
# Idempotent: writing the same files on every run is a no-op. Skip
# this whole block with `--skip-host-networking` for partial re-runs.
if [ "${SKIP_HOST_NETWORKING}" -eq 0 ]; then
    echo "=== Step 0: Pi host networking ==="

    # 0a. Disable IPv6 globally
    cat > /etc/sysctl.d/99-magic-dingus-disable-ipv6.conf <<'EOF'
net.ipv6.conf.all.disable_ipv6 = 1
net.ipv6.conf.default.disable_ipv6 = 1
net.ipv6.conf.lo.disable_ipv6 = 1
EOF
    sysctl -p /etc/sysctl.d/99-magic-dingus-disable-ipv6.conf >/dev/null
    echo "IPv6 disabled globally."

    # 0b. Install cloudflared if missing
    if ! command -v cloudflared &>/dev/null; then
        echo "Installing cloudflared (Cloudflare DoH proxy)..."
        # Add Cloudflare apt repo if not already present.
        if [ ! -f /etc/apt/sources.list.d/cloudflared.list ]; then
            mkdir -p /usr/share/keyrings
            curl -fsSL https://pkg.cloudflare.com/cloudflare-main.gpg \
                | tee /usr/share/keyrings/cloudflare-main.gpg >/dev/null
            echo "deb [signed-by=/usr/share/keyrings/cloudflare-main.gpg] https://pkg.cloudflare.com/cloudflared $(lsb_release -cs) main" \
                | tee /etc/apt/sources.list.d/cloudflared.list >/dev/null
            apt-get update -qq
        fi
        apt-get install -y cloudflared
    fi

    # 0c. Configure cloudflared as DoH resolver on 127.0.0.1:53
    mkdir -p /etc/cloudflared
    cat > /etc/cloudflared/config.yml <<'EOF'
proxy-dns: true
proxy-dns-port: 53
proxy-dns-address: 127.0.0.1
proxy-dns-upstream:
  - https://1.1.1.1/dns-query
  - https://1.0.0.1/dns-query
EOF
    # The cloudflared apt package ships a systemd unit named
    # cloudflared-proxy-dns.service that consumes /etc/cloudflared/config.yml.
    # Older builds named it cloudflared.service — try both.
    if systemctl list-unit-files | grep -q cloudflared-proxy-dns.service; then
        systemctl enable --now cloudflared-proxy-dns.service
    else
        systemctl enable --now cloudflared.service
    fi

    # 0d. Stop NetworkManager from rewriting /etc/resolv.conf
    mkdir -p /etc/NetworkManager/conf.d
    cat > /etc/NetworkManager/conf.d/99-dns.conf <<'EOF'
[main]
dns=none
EOF
    systemctl reload NetworkManager 2>/dev/null || true

    # 0e. Point /etc/resolv.conf at the local DoH proxy
    cat > /etc/resolv.conf <<'EOF'
nameserver 127.0.0.1
options edns0 trust-ad
EOF

    # 0f. Verify DoH resolver works
    if ! dig +short +time=3 +tries=1 cloudflare.com @127.0.0.1 >/dev/null; then
        echo "WARNING: DoH resolver test query failed. Continuing — could"
        echo "be transient. Verify with: dig cloudflare.com @127.0.0.1"
    else
        echo "DoH resolver active: 127.0.0.1 -> Cloudflare via DoH."
    fi

    echo "=== Step 0 complete ==="
fi
```

- [ ] **Step 3: Verify the script still passes shellcheck/syntax**

Run: `bash -n magic_dingus_box_cpp/scripts/setup_services.sh`
Expected: exits 0 (no syntax errors).

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/setup_services.sh
git commit -m "feat(mb): setup Step 0 — disable IPv6, route host DNS via Cloudflare DoH"
```

---

### Task 11: Add Step 4 pre-pull for Byparr

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh`

- [ ] **Step 1: Locate the "# 4. Start stack" section (around line 81)**

Run: `grep -n "^# 4\." magic_dingus_box_cpp/scripts/setup_services.sh`

- [ ] **Step 2: Insert the pre-pull block immediately above the existing `cd "${SERVICES_DIR}"` and `docker compose up -d` lines**

Substitute `<DIGEST>` with the same digest used in Task 6.

```bash
# 4a. Pre-pull Byparr (ghcr.io DNS race mitigation).
#
# On a fresh Pi the first ghcr.io DNS lookup sometimes fails before
# DoH (Step 0) is fully warm. Letting `docker compose up` lazily
# pull byparr causes a confusing failure mid-startup. Pre-pull with
# explicit retries instead.
echo "Pre-pulling Byparr (ghcr.io DNS can be flaky on first boot)..."
BYPARR_DIGEST="ghcr.io/thephaseless/byparr@<DIGEST>"
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
```

- [ ] **Step 3: Verify the script still passes syntax check**

Run: `bash -n magic_dingus_box_cpp/scripts/setup_services.sh`
Expected: exits 0.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/setup_services.sh
git commit -m "feat(mb): pre-pull Byparr with retry to dodge ghcr.io DNS race"
```

---

### Task 12: Add Step 4.5 tunnel-up gate

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh`

- [ ] **Step 1: Find the existing `# 5. Wait for services` block (around line 86)**

Run: `grep -n "^# 5\." magic_dingus_box_cpp/scripts/setup_services.sh`

- [ ] **Step 2: Insert the new gate immediately above Step 5**

```bash
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
```

- [ ] **Step 3: Verify the script still passes syntax check**

Run: `bash -n magic_dingus_box_cpp/scripts/setup_services.sh`
Expected: exits 0.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/setup_services.sh
git commit -m "feat(mb): tunnel-up gate before service config push"
```

---

### Task 13: Smoke-check script for VPN coverage

**Files:**
- Create: `magic_dingus_box_cpp/scripts/check_vpn_required.sh`

- [ ] **Step 1: Create the script**

```bash
#!/usr/bin/env bash
# Smoke-check: every torrent-ecosystem container exits via the VPN.
#
# Run on the Pi. Compares each container's reported public IP against
# Gluetun's reported public IP. Any container with a different exit IP
# is leaking. Also verifies host-level: IPv6 disabled and DoH resolver
# active.
#
# Exit codes:
#   0 — all checks pass
#   1 — one or more containers leak
#   2 — host-level networking misconfigured
#   3 — required tools missing on the Pi

set -uo pipefail

EXPECTED_VPN_CONTAINERS=(mdb_gluetun mdb_qbittorrent mdb_radarr mdb_prowlarr mdb_byparr)

# 1. Tooling checks
for tool in docker dig curl jq; do
    if ! command -v "${tool}" &>/dev/null; then
        echo "ERROR: ${tool} not installed on this Pi."
        exit 3
    fi
done

# 2. Get gluetun's reported VPN exit IP
GLUETUN_IP=$(docker exec mdb_gluetun wget -qO- --timeout=5 \
    http://localhost:8000/v1/publicip/ip 2>/dev/null \
    | jq -r .public_ip 2>/dev/null || echo "")
if [ -z "${GLUETUN_IP}" ]; then
    echo "ERROR: cannot read gluetun's exit IP. Is the tunnel up?"
    echo "       Try: docker logs mdb_gluetun"
    exit 1
fi
echo "Gluetun reports VPN exit IP: ${GLUETUN_IP}"

# 3. Per-container exit IP check
LEAKS=0
for container in "${EXPECTED_VPN_CONTAINERS[@]}"; do
    if ! docker inspect "${container}" >/dev/null 2>&1; then
        echo "  ${container}: NOT RUNNING (skipped)"
        continue
    fi
    # ifconfig.me/ip returns a plain IP string. We curl from inside
    # the container; if the container shares gluetun's netns, this
    # exits via the VPN too.
    CONTAINER_IP=$(docker exec "${container}" sh -c \
        'curl -fsS --max-time 8 ifconfig.me/ip 2>/dev/null || \
         wget -qO- --timeout=8 ifconfig.me/ip 2>/dev/null' \
        | tr -d '[:space:]')
    if [ "${CONTAINER_IP}" = "${GLUETUN_IP}" ]; then
        echo "  ${container}: exits via VPN ✓"
    else
        echo "  ${container}: LEAK — exits as ${CONTAINER_IP:-unknown}"
        LEAKS=$((LEAKS + 1))
    fi
done

# 4. Host-level: IPv6 disabled
if [ "$(cat /proc/sys/net/ipv6/conf/all/disable_ipv6)" = "1" ]; then
    echo "  host: IPv6 disabled ✓"
else
    echo "  host: WARNING — IPv6 still enabled. Setup Step 0 may not have run."
    LEAKS=$((LEAKS + 1))
fi

# 5. Host-level: DoH active (resolv.conf points at 127.0.0.1)
if grep -q "^nameserver 127.0.0.1" /etc/resolv.conf 2>/dev/null; then
    echo "  host: DoH resolver active (127.0.0.1) ✓"
else
    echo "  host: WARNING — /etc/resolv.conf does not point at local DoH."
    LEAKS=$((LEAKS + 1))
fi

if [ "${LEAKS}" -gt 0 ]; then
    echo "FAIL: ${LEAKS} leak(s) or misconfig(s) detected."
    exit 1
fi
echo "OK: all torrent-ecosystem containers exit via VPN; host correctly configured."
exit 0
```

- [ ] **Step 2: Make it executable**

```bash
chmod +x magic_dingus_box_cpp/scripts/check_vpn_required.sh
```

- [ ] **Step 3: Verify syntax**

Run: `bash -n magic_dingus_box_cpp/scripts/check_vpn_required.sh`
Expected: exits 0.

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/scripts/check_vpn_required.sh
git commit -m "feat(mb): check_vpn_required.sh — operator smoke-check for leaks"
```

---

## Phase 4 — Web admin gating

### Task 14: Add `_vpn_configured()` helper and gate function

**Files:**
- Modify: `magic_dingus_box/web/admin.py` (add helpers near the existing `_media_browser_unlocked()` around line 211)

- [ ] **Step 1: Find the existing `_media_browser_unlocked` helper**

Run: `grep -n "_media_browser_unlocked\|_media_browser_locked_response" magic_dingus_box/web/admin.py | head -5`

- [ ] **Step 2: Add the new helpers immediately after `_media_browser_locked_response`**

```python
def _vpn_configured() -> bool:
    """True iff services/.env exists AND has a non-empty WIREGUARD_PRIVATE_KEY.

    Layer 2 of the three-layer Media Browser gate. Failure-closed:
    any error reading the .env returns False so a malformed file
    can't accidentally allow access.
    """
    try:
        env = _read_env_file(SERVICES_ENV)
        return bool(env.get("WIREGUARD_PRIVATE_KEY", "").strip())
    except Exception:
        return False


def _vpn_required_response():
    """Standard 403 used when Layer 2 (VPN configured) fails."""
    return error_response(
        "vpn_not_configured",
        "VPN must be configured in the Media Browser tab before using this feature",
        status=403,
    )


def _check_media_browser_gates(*, require_vpn: bool = True):
    """Run the Layer 1 + (optionally) Layer 2 gates.

    Returns None on pass, or a 403 Response on fail. Endpoints that
    are part of the VPN-setup flow itself (status, setup,
    setup-status, reset) pass require_vpn=False so the operator can
    reach them before configuring VPN.
    """
    if not _media_browser_unlocked():
        return _media_browser_locked_response()
    if require_vpn and not _vpn_configured():
        return _vpn_required_response()
    return None
```

- [ ] **Step 3: Verify the file imports needed (`SERVICES_ENV`, `_read_env_file`) are already present**

Run: `grep -n "SERVICES_ENV\|_read_env_file" magic_dingus_box/web/admin.py | head -10`
Expected: both already defined elsewhere in the file (used by existing
`_env_has_wireguard_key`). If not, the implementer should locate where
they're defined and ensure they're in scope.

- [ ] **Step 4: Run a quick import check**

```bash
python3 -c "import importlib.util; spec = importlib.util.spec_from_file_location('admin', 'magic_dingus_box/web/admin.py'); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); print('imports OK')"
```
Expected: prints "imports OK". If syntax errors, fix before continuing.

(Note: the import may fail because of Flask app initialization; if so,
fall back to `python3 -c "import ast; ast.parse(open('magic_dingus_box/web/admin.py').read()); print('syntax OK')"`.)

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/admin.py
git commit -m "feat(mb): add _vpn_configured + _check_media_browser_gates helpers"
```

---

### Task 15: Replace 8 scattered unlock checks with the gate helper

**Files:**
- Modify: `magic_dingus_box/web/admin.py` (8 endpoints listed in spec §4b)

The 8 existing `if not _media_browser_unlocked(): return _media_browser_locked_response()` checks live around lines 2361, 2418, 2529, 2572, 2768, 2808, 2855, plus one more (the spec mentions 8). Each gets converted to a one-liner using the new helper.

The mapping (per spec §4b table):

| Endpoint | Layer 2 required? |
|---|---|
| `GET /admin/media-browser/status` | no |
| `POST /admin/media-browser/setup` | no |
| `GET /admin/media-browser/setup-status/<job_id>` | no |
| `POST /admin/media-browser/reset` | no |
| `GET /admin/media-browser/credentials` | yes |
| `GET /admin/media-browser/health-summary` | yes |
| `POST /admin/media-browser/restart` | yes |
| All other `/admin/media-browser/*` endpoints | yes |

- [ ] **Step 1: Locate the 8 occurrences**

Run: `grep -n "_media_browser_unlocked()" magic_dingus_box/web/admin.py`
Expected: ~9 lines (1 helper definition + 8 call sites).

- [ ] **Step 2: For each call site, replace the two-line check with one line**

Old:
```python
        if not _media_browser_unlocked():
            return _media_browser_locked_response()
```

New (Layer 1 only — used by `status`, `setup`, `setup-status`, `reset`):
```python
        if (resp := _check_media_browser_gates(require_vpn=False)):
            return resp
```

New (Layer 1 + Layer 2 — all other endpoints):
```python
        if (resp := _check_media_browser_gates()):
            return resp
```

Use the spec §4b table to decide `require_vpn` per endpoint. Walk
each occurrence in order, using the line-number context from Step 1
to identify which endpoint it belongs to.

- [ ] **Step 3: Verify all 8 are replaced**

Run: `grep -c "_media_browser_unlocked()" magic_dingus_box/web/admin.py`
Expected: 1 (only the helper definition itself remains).

Run: `grep -c "_check_media_browser_gates" magic_dingus_box/web/admin.py`
Expected: at least 9 (1 definition + 8 call sites).

- [ ] **Step 4: Syntax check**

```bash
python3 -c "import ast; ast.parse(open('magic_dingus_box/web/admin.py').read()); print('syntax OK')"
```
Expected: prints "syntax OK".

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/admin.py
git commit -m "refactor(mb): replace 8 unlock checks with three-layer gate helper"
```

---

### Task 16: Extend `/admin/media-browser/visibility` response shape

**Files:**
- Modify: `magic_dingus_box/web/admin.py` (around line 2344)

- [ ] **Step 1: Locate the existing visibility endpoint**

Run: `grep -n "@app.get(\"/admin/media-browser/visibility\")" magic_dingus_box/web/admin.py`

- [ ] **Step 2: Replace the body**

Old:
```python
    @app.get("/admin/media-browser/visibility")
    def media_browser_visibility():  # type: ignore[no-redef]
        """Public — return whether the Media Browser tab should be rendered.

        Always 200, never errors. The frontend uses this on page init to
        decide whether to render the tab nav button + section at all. All
        OTHER /admin/media-browser/* routes additionally enforce the same
        check server-side and return 403 when locked.
        """
        return success_response(data={"visible": _media_browser_unlocked()})
```

New:
```python
    @app.get("/admin/media-browser/visibility")
    def media_browser_visibility():  # type: ignore[no-redef]
        """Public — return whether the Media Browser tab should be rendered.

        Always 200, never errors. Returns two flags:
          - visible: Layer 1 (unlock). Whether to render the tab DOM
            at all.
          - vpn_configured: Layer 2 (WireGuard config dropped). When
            visible=true and vpn_configured=false, the frontend shows
            a "Configure VPN" form instead of the dashboard.

        Other /admin/media-browser/* routes enforce the same gates
        server-side via _check_media_browser_gates and return 403
        (`media_browser_locked` or `vpn_not_configured`) on failure.
        """
        return success_response(data={
            "visible": _media_browser_unlocked(),
            "vpn_configured": _vpn_configured(),
        })
```

- [ ] **Step 3: Syntax check**

```bash
python3 -c "import ast; ast.parse(open('magic_dingus_box/web/admin.py').read()); print('syntax OK')"
```
Expected: prints "syntax OK".

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box/web/admin.py
git commit -m "feat(mb): /visibility returns vpn_configured for configure-vs-use UI"
```

---

### Task 17: Rename flaresolverr→byparr in tear-down + restart targets

**Files:**
- Modify: `magic_dingus_box/web/admin.py` (around line 2916, the reset/teardown flow)

- [ ] **Step 1: Locate the targets list**

Run: `grep -n '"flaresolverr"\|flaresolverr' magic_dingus_box/web/admin.py`

- [ ] **Step 2: For each occurrence in the tear-down/reset flow, rename `flaresolverr` to `byparr`**

The container name in compose changed (Task 6) so any code referencing
`mdb_flaresolverr` or the bare service name `flaresolverr` must
update to `mdb_byparr` / `byparr` respectively. Use the Edit tool
per-occurrence; don't blanket replace because some references might
be in historical comments.

- [ ] **Step 3: Verify**

Run: `grep -n "flaresolverr\|FlareSolverr" magic_dingus_box/web/admin.py`
Expected: only historical mentions in comments, if any. No live code
references.

Run: `grep -c "byparr\|Byparr" magic_dingus_box/web/admin.py`
Expected: at least the count of removed flaresolverr live references.

- [ ] **Step 4: Syntax check**

```bash
python3 -c "import ast; ast.parse(open('magic_dingus_box/web/admin.py').read()); print('syntax OK')"
```
Expected: prints "syntax OK".

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box/web/admin.py
git commit -m "fix(mb): web admin references byparr (not flaresolverr) in tear-down"
```

---

### Task 18: Frontend — surface `vpn_configured` from /visibility

**Files:**
- Modify: `magic_dingus_box/web/static/manager.js`

The existing frontend already handles the configure-vs-use UI through the
`/admin/media-browser/status` endpoint's `configured` field — it shows a
"Configure VPN" form when `configured: false` and the dashboard when
`configured: true`. So the only real change here is propagating the new
`vpn_configured` field from `/visibility` to a small JS state flag, which
makes future enhancements possible (e.g., showing a yellow badge on the
tab nav button when unlocked-but-unconfigured).

- [ ] **Step 1: Update `checkMediaBrowserVisibility()` (around line 5633)**

Locate this code block (around line 5640):

```javascript
        visible = !!(data && data.ok && data.data && data.data.visible === true);
```

Replace with:

```javascript
        visible = !!(data && data.ok && data.data && data.data.visible === true);
        // Layer 2 surface: tracked for any future UI that wants to badge
        // the tab nav when unlocked-but-unconfigured. The existing
        // dashboard already detects `configured: false` from /status and
        // shows a Configure VPN form, so no DOM change is required here
        // today.
        window.mbVpnConfigured = !!(data && data.ok && data.data && data.data.vpn_configured === true);
```

- [ ] **Step 2: Verify nothing else regressed**

Run: `grep -c "mbVpnConfigured\|vpn_configured" magic_dingus_box/web/static/manager.js`
Expected: at least 2 (one for the new variable assignment, one for
the destructure). No other occurrences.

- [ ] **Step 3: Commit**

```bash
git add magic_dingus_box/web/static/manager.js
git commit -m "feat(mb): frontend reads vpn_configured from /visibility for future use"
```

---

## Phase 5 — Kiosk C++ changes

### Task 19: Add new AppState flags

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (around line 201, where `media_browser_unlocked` lives)

- [ ] **Step 1: Locate the existing flag**

Run: `grep -n "media_browser_unlocked" magic_dingus_box_cpp/src/app/app_state.h`
Expected: line ~201.

- [ ] **Step 2: Add two new flags directly under `media_browser_unlocked`**

```cpp
    // Layer 1: operator-side unlock via secret sequence (persisted to settings.json).
    bool media_browser_unlocked = false;

    // Layer 2: WIREGUARD_PRIVATE_KEY non-empty in services/.env at boot.
    // Re-read on each Settings menu open so an operator who configures
    // VPN via Content Manager doesn't need to restart the kiosk.
    bool media_browser_vpn_configured = false;

    // Layer 3: Radarr /ping reachable on localhost:7878. Owned by
    // VpnHealthMonitor (background thread). Three consecutive failed
    // polls (~30s at 10s interval) flips this true→false; recovery
    // is instant on first successful poll.
    bool media_browser_vpn_healthy = false;
```

- [ ] **Step 3: Compile to verify the header still parses**

```bash
cd magic_dingus_box_cpp && mkdir -p build && cd build && cmake .. -DENABLE_MEDIA_BROWSER=ON > /dev/null 2>&1 && cmake --build . --target test_media_browser_unit -j4 2>&1 | tail -5
```
Expected: compiles successfully (these are just two new POD members; no
code uses them yet).

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/app/app_state.h
git commit -m "feat(mb): AppState adds vpn_configured + vpn_healthy flags"
```

---

### Task 20: Read `media_browser_vpn_configured` at boot

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/settings_persistence.cpp` (the load function, around line 420)

The existing code at line 420 reads `media_browser_unlocked` from
the playback section of settings.json. Layer 2 isn't from settings.json
— it's from `services/.env`. Add a small helper.

- [ ] **Step 1: Locate the load function**

Run: `grep -n "media_browser_unlocked" magic_dingus_box_cpp/src/app/settings_persistence.cpp`
Expected: lines 242 (save) and 420 (load).

- [ ] **Step 2: Add a small static helper at the top of the file (anonymous namespace)**

```cpp
namespace {

// Reads /opt/magic_dingus_box/services/.env if present and returns true
// iff WIREGUARD_PRIVATE_KEY is set to a non-empty value. Failure-closed:
// missing file or any I/O error returns false.
bool read_vpn_configured_from_services_env() {
    static constexpr const char* kEnvPath = "/opt/magic_dingus_box/services/.env";
    std::ifstream f(kEnvPath);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        // Match WIREGUARD_PRIVATE_KEY=<non-empty>. Ignore quoted/unquoted form.
        constexpr std::string_view kKey = "WIREGUARD_PRIVATE_KEY=";
        if (line.rfind(kKey, 0) != 0) continue;
        std::string val = line.substr(kKey.size());
        // Strip surrounding quotes and whitespace.
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '"' || s.front() == '\'')) s.erase(s.begin());
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back()  == '"' || s.back()  == '\'')) s.pop_back();
        };
        trim(val);
        return !val.empty();
    }
    return false;
}

}  // namespace
```

Add `#include <fstream>` and `#include <string_view>` to the includes
block at the top if not already present.

- [ ] **Step 3: Call the helper in the load function near the unlocked-flag read**

Find the line `state.media_browser_unlocked = playback.get("media_browser_unlocked", false).asBool();`
(line 420). Add immediately after:

```cpp
        state.media_browser_vpn_configured = read_vpn_configured_from_services_env();
```

- [ ] **Step 4: Build**

```bash
cd magic_dingus_box_cpp && cmake --build build --target test_media_browser_unit -j4 2>&1 | tail -5
```
Expected: compiles.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/app/settings_persistence.cpp
git commit -m "feat(mb): load vpn_configured from services/.env at boot"
```

---

### Task 21: VpnHealthMonitor header + first failing test

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.h`
- Create: `magic_dingus_box_cpp/tests/media_browser/test_vpn_health_monitor.cpp`

TDD pass — write the failing test first, then the header to satisfy
just the test's requirements.

- [ ] **Step 1: Create the test file with the first test (debounce on 3 consecutive failures)**

```cpp
// tests/media_browser/test_vpn_health_monitor.cpp
#include <catch2/catch_test_macros.hpp>
#include "media_browser/health/vpn_health_monitor.h"
#include "app/app_state.h"

#include <atomic>

namespace {
// Test ping function: returns whatever the controller sets.
struct ScriptedPinger {
    std::atomic<bool> next_result{false};
    bool operator()() const { return next_result.load(); }
};
}  // namespace

TEST_CASE("VpnHealthMonitor flips Healthy after first successful poll",
          "[vpn_health_monitor]") {
    app::AppState state;
    REQUIRE(state.media_browser_vpn_healthy == false);

    ScriptedPinger pinger;
    pinger.next_result = true;

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));   // tight poll interval for tests

    monitor.start();
    // Allow at least one poll cycle.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    monitor.stop();

    REQUIRE(state.media_browser_vpn_healthy == true);
}

TEST_CASE("VpnHealthMonitor stays Healthy through 2 failures (debounce)",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = true;

    ScriptedPinger pinger;
    pinger.next_result = false;   // every poll fails

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));

    monitor.start();
    // Let exactly 2 polls happen (~10ms). Should NOT flip yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    REQUIRE(state.media_browser_vpn_healthy == true);
    monitor.stop();
}

TEST_CASE("VpnHealthMonitor flips Unhealthy after 3 consecutive failures",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = true;

    ScriptedPinger pinger;
    pinger.next_result = false;

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));

    monitor.start();
    // Let at least 4 polls happen (~20ms). Three failures + buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    monitor.stop();

    REQUIRE(state.media_browser_vpn_healthy == false);
}

TEST_CASE("VpnHealthMonitor recovers immediately on first successful poll",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = false;

    ScriptedPinger pinger;
    pinger.next_result = true;

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));

    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    monitor.stop();

    REQUIRE(state.media_browser_vpn_healthy == true);
}
```

- [ ] **Step 2: Create the header file (minimal — satisfies the test's API expectations only)**

```cpp
// src/media_browser/health/vpn_health_monitor.h
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace app { struct AppState; }

namespace media_browser {

// Polls a "VPN healthy?" probe in the background and updates
// state.media_browser_vpn_healthy.
//
// Three-strikes debounce: 3 consecutive failed polls flip healthy
// true→false. Recovery is instant on first success. Initial state
// is whatever the AppState already has — no implicit reset on
// start().
class VpnHealthMonitor {
public:
    using PingFn = std::function<bool()>;

    // Default constructor uses an HTTP ping to localhost:7878/ping
    // with a 3s curl timeout.
    explicit VpnHealthMonitor(app::AppState& state);

    // Test seam: inject a ping function and a custom poll interval.
    VpnHealthMonitor(app::AppState& state,
                     PingFn ping_fn,
                     std::chrono::milliseconds poll_interval);

    ~VpnHealthMonitor();

    // No-op if already running.
    void start();
    // Joins the worker thread. Idempotent.
    void stop();

    // For diagnostics.
    int consecutive_failures() const { return consecutive_failures_.load(); }

private:
    void run();

    app::AppState& state_;
    PingFn ping_fn_;
    std::chrono::milliseconds poll_interval_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> consecutive_failures_{0};
    std::thread worker_;

    static constexpr int kFailureThreshold = 3;
};

}  // namespace media_browser
```

- [ ] **Step 3: Wire the new test into CMake**

In `magic_dingus_box_cpp/CMakeLists.txt`, find `MEDIA_BROWSER_TEST_SOURCES`
(around line 342). Append:

```cmake
        tests/media_browser/test_vpn_health_monitor.cpp
        src/media_browser/health/vpn_health_monitor.cpp
```

(The `.cpp` doesn't exist yet; we'll create it in the next task.
For now CMake will fail to find it. That's expected — Step 4 confirms
the test file fails to link, which is what TDD expects.)

- [ ] **Step 4: Run the build to confirm test fails to link (vpn_health_monitor.cpp missing)**

```bash
cd magic_dingus_box_cpp && cmake --build build --target test_media_browser_unit -j4 2>&1 | tail -10
```
Expected: link error referencing `VpnHealthMonitor::VpnHealthMonitor`,
`::start`, `::stop`, or `::~VpnHealthMonitor` (undefined symbols). This
is the failing-test signal — implementation is missing.

- [ ] **Step 5: Commit (red phase)**

```bash
git add magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.h \
       magic_dingus_box_cpp/tests/media_browser/test_vpn_health_monitor.cpp \
       magic_dingus_box_cpp/CMakeLists.txt
git commit -m "test(mb): VpnHealthMonitor — debounce + recovery tests (red)"
```

---

### Task 22: Implement VpnHealthMonitor

**Files:**
- Create: `magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.cpp`

- [ ] **Step 1: Create the implementation file**

```cpp
// src/media_browser/health/vpn_health_monitor.cpp
#include "media_browser/health/vpn_health_monitor.h"

#include "app/app_state.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

namespace media_browser {

namespace {

// Default ping: GET http://127.0.0.1:7878/ping with 3s timeout.
// Returns true on HTTP 2xx, false on any error or non-2xx.
bool default_radarr_ping() {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:7878/ping");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);   // HEAD-equivalent; we don't read body
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && http_code >= 200 && http_code < 300;
}

}  // namespace

VpnHealthMonitor::VpnHealthMonitor(app::AppState& state)
    : VpnHealthMonitor(state, &default_radarr_ping, std::chrono::seconds(10)) {}

VpnHealthMonitor::VpnHealthMonitor(app::AppState& state,
                                   PingFn ping_fn,
                                   std::chrono::milliseconds poll_interval)
    : state_(state), ping_fn_(std::move(ping_fn)),
      poll_interval_(poll_interval) {}

VpnHealthMonitor::~VpnHealthMonitor() {
    stop();
}

void VpnHealthMonitor::start() {
    if (worker_.joinable()) return;   // already running
    stop_flag_.store(false);
    worker_ = std::thread([this] { run(); });
}

void VpnHealthMonitor::stop() {
    stop_flag_.store(true);
    if (worker_.joinable()) worker_.join();
}

void VpnHealthMonitor::run() {
    while (!stop_flag_.load()) {
        bool ok = ping_fn_();
        if (ok) {
            // Recovery is instant — any successful poll flips healthy.
            consecutive_failures_.store(0);
            state_.media_browser_vpn_healthy = true;
        } else {
            int n = consecutive_failures_.fetch_add(1) + 1;
            if (n >= kFailureThreshold) {
                // Three-strikes: flip unhealthy.
                state_.media_browser_vpn_healthy = false;
            }
        }
        std::this_thread::sleep_for(poll_interval_);
    }
}

}  // namespace media_browser
```

- [ ] **Step 2: Build the test binary**

```bash
cd magic_dingus_box_cpp && cmake --build build --target test_media_browser_unit -j4 2>&1 | tail -5
```
Expected: compiles, links.

- [ ] **Step 3: Run the tests**

```bash
./magic_dingus_box_cpp/build/test_media_browser_unit '[vpn_health_monitor]'
```
Expected: all 4 test cases pass (4 assertions per failure-and-recovery
matrix).

- [ ] **Step 4: Commit (green phase)**

```bash
git add magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.cpp
git commit -m "feat(mb): implement VpnHealthMonitor — three-strike debounce"
```

---

### Task 23: Wire VpnHealthMonitor into the production source list

**Files:**
- Modify: `magic_dingus_box_cpp/CMakeLists.txt`

The vpn_health_monitor.cpp was added to MEDIA_BROWSER_TEST_SOURCES in
Task 21. It also needs to compile into the production kiosk binary
(`MEDIA_BROWSER_SOURCES`).

- [ ] **Step 1: Locate `MEDIA_BROWSER_SOURCES` in CMakeLists.txt**

Run: `grep -n "MEDIA_BROWSER_SOURCES" magic_dingus_box_cpp/CMakeLists.txt`
Expected: a `set(MEDIA_BROWSER_SOURCES ...)` near line 320-340.

- [ ] **Step 2: Append the new source to `MEDIA_BROWSER_SOURCES`**

Add this line within the source list:

```cmake
        src/media_browser/health/vpn_health_monitor.cpp
```

- [ ] **Step 3: Build the kiosk target**

```bash
cd magic_dingus_box_cpp && cmake --build build --target magic_dingus_box_cpp -j4 2>&1 | tail -5
```
Expected: compiles. (This may not be possible on a Mac dev box because
the kiosk depends on libdrm/EGL/etc. If so, defer the build verification
to the Pi via `./scripts/deploy_cpp.sh --build`.)

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/CMakeLists.txt
git commit -m "build(mb): include vpn_health_monitor.cpp in kiosk binary"
```

---

### Task 24: Instantiate VpnHealthMonitor in main.cpp + toast on transitions

**Files:**
- Modify: `magic_dingus_box_cpp/src/main.cpp`

- [ ] **Step 1: Locate the kiosk's main loop and toast manager**

Run: `grep -n "TostManager\|ToastManager\|toast_manager" magic_dingus_box_cpp/src/main.cpp | head`

- [ ] **Step 2: Add the include and instantiation**

Near other media_browser includes:

```cpp
#include "media_browser/health/vpn_health_monitor.h"
```

Inside `main()`, after AppState is initialized AND after settings have
loaded (so `media_browser_vpn_configured` is already populated), add:

```cpp
    // Layer 3 monitor — only meaningful when Layers 1+2 already pass.
    // Otherwise the Settings menu won't expose MB anyway, so save the
    // background polling work.
    std::unique_ptr<media_browser::VpnHealthMonitor> vpn_health_monitor;
    if (state.media_browser_unlocked && state.media_browser_vpn_configured) {
        vpn_health_monitor =
            std::make_unique<media_browser::VpnHealthMonitor>(state);
        vpn_health_monitor->start();
    }
    // RAII destructor on exit will call stop().
```

- [ ] **Step 3: Add toast-on-transition tracking inside the main loop**

Where the main loop runs (find the `while (!state.should_exit)` block
or equivalent), add tracking:

```cpp
    bool prev_vpn_healthy = state.media_browser_vpn_healthy;
    // ... inside the loop, once per frame:
    if (vpn_health_monitor) {
        bool now = state.media_browser_vpn_healthy;
        if (prev_vpn_healthy && !now) {
            // true → false: tunnel just dropped
            toast_manager.show("Media Browser unavailable — VPN tunnel down",
                               std::chrono::seconds(8));
        }
        // false → true: silent recovery (no toast)
        // Unknown(initial false) → true on boot: also silent
        prev_vpn_healthy = now;
    }
```

(Adapt the toast call to match the existing toast_manager API in
`magic_dingus_box_cpp/src/ui/toast.h`.)

- [ ] **Step 4: Cross-compile / deploy + build on Pi**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```
Expected: builds successfully on the Pi.

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/main.cpp
git commit -m "feat(mb): kiosk runs VpnHealthMonitor; toast on tunnel drop"
```

---

### Task 25: Three-layer guard in settings_menu.cpp

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.cpp`

- [ ] **Step 1: Locate the existing Layer-1 check**

Run: `grep -n "media_browser_unlocked" magic_dingus_box_cpp/src/ui/settings_menu.cpp`
Expected: lines 329 and 623 (or thereabouts).

- [ ] **Step 2: Re-read VPN-configured flag on Settings menu open**

Find the menu-open handler (likely `void SettingsMenu::open()` or
similar). Add at the start:

```cpp
    // Layer 2 may have changed since boot if the operator just
    // dropped a WireGuard config via Content Manager. Re-read it
    // on each open so the UI reflects current state without
    // requiring a kiosk restart.
    if (app_state_) {
        std::ifstream f("/opt/magic_dingus_box/services/.env");
        bool found = false;
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                constexpr std::string_view kKey = "WIREGUARD_PRIVATE_KEY=";
                if (line.rfind(kKey, 0) != 0) continue;
                std::string val = line.substr(kKey.size());
                while (!val.empty() && (val.back() == ' ' || val.back() == '\t' ||
                                        val.back() == '"' || val.back() == '\'')) val.pop_back();
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t' ||
                                        val.front() == '"' || val.front() == '\'')) val.erase(val.begin());
                found = !val.empty();
                break;
            }
        }
        app_state_->media_browser_vpn_configured = found;
    }
```

(Add `#include <fstream>` and `#include <string_view>` to the file's
includes if not present.)

- [ ] **Step 3: Replace the line-329 Layer-1 guard with three-layer logic**

Before:
```cpp
        if (app_state_ && app_state_->media_browser_unlocked) {
            // ... render Media Browser entries ...
        }
```

After:
```cpp
        if (app_state_ && app_state_->media_browser_unlocked) {
            if (!app_state_->media_browser_vpn_configured) {
                // Layer 1 pass, Layer 2 fail: show a disabled-style row
                // pointing at Content Manager.
                render_disabled_row(
                    "Media Browser",
                    "Configure VPN in Content Manager to enable");
            } else if (!app_state_->media_browser_vpn_healthy) {
                // Layer 3 fail: hidden. Toast (fired in main.cpp) is the
                // user-visible signal; no row rendered here so the
                // operator can clearly see the feature has degraded.
            } else {
                // All three pass — render the existing Media Browser entries.
                // ... existing code unchanged ...
            }
        }
```

The existing render code goes inside the third (else) branch.

The `render_disabled_row(label, helper_text)` function may not exist
yet — if not, mirror the pattern of an existing disabled-row helper
in this file (search for `disabled` or `gray` in settings_menu.cpp).
If no such helper exists, render with reduced alpha and no input
binding using the Renderer's existing API.

- [ ] **Step 4: Apply the same three-layer logic at line 623**

Run: `grep -n "media_browser_unlocked" magic_dingus_box_cpp/src/ui/settings_menu.cpp`
Apply the same three-layer-guard pattern as Step 3 at the line ~623
location (the second use site).

- [ ] **Step 5: Build on the Pi**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```
Expected: builds successfully.

- [ ] **Step 6: Commit**

```bash
git add magic_dingus_box_cpp/src/ui/settings_menu.cpp
git commit -m "feat(mb): three-layer guard on Settings menu MB entries"
```

---

### Task 26: Inline TMDB privacy comment

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp`

- [ ] **Step 1: Locate the HTTP request site**

Run: `grep -n "themoviedb\|api.themoviedb\|CURLOPT_URL" magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp | head`

- [ ] **Step 2: Add a privacy-gap comment immediately above the curl_easy_setopt(CURL_OPT_URL, ...) line**

```cpp
    // Privacy gap (accepted): this HTTPS call exits via the host
    // network, NOT via Gluetun, because the kiosk binary runs outside
    // Docker. ISPs see the SNI cleartext (api.themoviedb.org) on every
    // call. Metadata only — never reveals torrent activity. Documented
    // in magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md "Privacy
    // notes". Future work: route through Radarr's metadata proxy.
```

- [ ] **Step 3: Verify build**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```
Expected: compiles (comment-only change).

- [ ] **Step 4: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp
git commit -m "docs(mb): inline comment flagging TMDB-from-host privacy gap"
```

---

## Phase 6 — Integration test on Pi

### Task 27: Manual integration test sweep

This task has no commit at the end — it's a verification gate, not a
code change. If any check fails, fix the failure inline (creating a
new task in the plan if the fix is non-trivial).

**Prerequisites:**
- A test Pi reachable via `magic@magicpi.local` (or USB gadget).
- A valid ProtonVPN WireGuard config (`.conf`) with NAT-PMP enabled.
- `services/.env` does NOT yet exist on the Pi (or you've taken a
  backup so you can wipe it for the test).

- [ ] **Step 1: Deploy the new code and wipe Pi state**

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
ssh magic@magicpi.local 'sudo systemctl stop magic-dingus-services magic-dingus-web magic-dingus-box-cpp; sudo rm -rf /opt/magic_dingus_box/services/{config,.env}; sudo systemctl start magic-dingus-web magic-dingus-box-cpp'
```
Expected: kiosk and web admin start; services stack is absent (Layer 2 fails).

- [ ] **Step 2: Verify Media Browser tab is hidden in Content Manager**

Open `http://magicpi.local:5000` in a browser. Look for any "Media
Browser" tab.
Expected: NOT present (Layer 1 fails — no unlock yet).

- [ ] **Step 3: Enter the secret sequence on the kiosk**

Physically: BTN1+BTN3 chord → BTN2 × 3 → rotary click. Watch for
the "unlocked" toast.

Refresh Content Manager.
Expected: Media Browser tab now visible. Click it: shows "Configure
VPN" form (Layer 1 pass, Layer 2 fail).

- [ ] **Step 4: Drop the WireGuard config**

Drag the `.conf` file onto the upload zone, or paste its contents.
Click Configure VPN.

Expected: setup-job streams progress for ~90s. Final state: dashboard
shows "All services healthy", reports a non-empty VPN exit IP, and
the country.

- [ ] **Step 5: Run check_vpn_required.sh on the Pi**

```bash
ssh magic@magicpi.local 'sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/check_vpn_required.sh'
```
Expected: every container reports "exits via VPN ✓"; host reports
"IPv6 disabled ✓" and "DoH resolver active ✓"; final line "OK".

- [ ] **Step 6: Verify kiosk now shows MB entries**

Look at the kiosk's Settings menu.
Expected: Media Browser entries visible (all 3 layers pass).

- [ ] **Step 7: Simulate tunnel drop**

```bash
ssh magic@magicpi.local 'docker stop mdb_gluetun'
```
Expected: within ~30s (3 polls), kiosk hides MB entries from the
Settings menu and shows a toast: "Media Browser unavailable — VPN
tunnel down."

- [ ] **Step 8: Recover**

```bash
ssh magic@magicpi.local 'docker start mdb_gluetun'
```
Expected: within ~10s, MB entries silently reappear in the Settings
menu (no toast on recovery).

- [ ] **Step 9: Test reset flow**

In Content Manager: click "Reset Media Browser" (or equivalent).
Expected: services stop; `services/.env` is wiped; `byparr` (not
`flaresolverr`) appears in any progress log; tab returns to
"Configure VPN" form state.

If all 9 steps pass, the implementation is verified. No commit —
this task is a checkpoint.

If any step fails, file a bug task in this plan with reproduction
details and revert via `git revert` if the failure indicates a
broken commit.

---

## Final cleanup

### Task 28: Smoke-check the full plan against current state

- [ ] **Step 1: Verify branch state**

```bash
git log --oneline main..HEAD
```
Expected: shows the spec-doc commit (9ca1107) plus all tasks above
(~27 commits in addition).

- [ ] **Step 2: Verify no leftover FlareSolverr live references**

```bash
grep -rn "flaresolverr\|FlareSolverr" magic_dingus_box_cpp/services/ \
    magic_dingus_box_cpp/scripts/ magic_dingus_box/web/ \
    magic_dingus_box_cpp/src/ \
    --include="*.yml" --include="*.json" --include="*.py" \
    --include="*.cpp" --include="*.h" --include="*.sh"
```
Expected: only matches in JSON-fixture *names* (e.g.,
`prowlarr_indexerproxies.json` references the *proxy name*
`flaresolverr` which is intentionally retained for compatibility).
No live container/image/url references.

- [ ] **Step 3: Verify no stale `_media_browser_unlocked()` direct calls**

```bash
grep -rn "_media_browser_unlocked()" magic_dingus_box/web/admin.py
```
Expected: 1 line only — the helper definition itself.

- [ ] **Step 4: Push the branch and open a PR**

```bash
git push -u origin feature/vpn-required-media-browser
gh pr create --title "feat(mb): VPN-required Media Browser architecture" --body "$(cat <<'EOF'
## Summary

Formalizes the Media Browser as a strictly VPN-required feature:

- Three-layer gating (unlocked / VPN configured / tunnel healthy)
- All four torrent-ecosystem services behind Gluetun (Prowlarr, Radarr, Byparr, qBittorrent)
- Pi host: IPv6 globally disabled; DNS via Cloudflare DoH
- Prowlarr 1.26.1 → 2.3.5.5327; FlareSolverr → Byparr (working Cloudflare bypass)
- Web admin: distinct `vpn_not_configured` 403 + `/visibility` returns `vpn_configured` flag
- Kiosk: VpnHealthMonitor polls Radarr `/ping` every 10s; three-strike debounce; toast on drop

Companion specs:
- `docs/superpowers/specs/2026-05-01-vpn-required-media-browser-design.md`
- `docs/superpowers/plans/2026-05-01-vpn-required-media-browser.md`

## Test plan

- [ ] Manual integration test sweep on a fresh Pi (Task 27 in the plan)
- [ ] `check_vpn_required.sh` passes
- [ ] `test_media_browser_unit '[vpn_health_monitor]'` passes (4 cases)
- [ ] `docker compose config` validates the new compose
- [ ] Kiosk Settings menu shows correct state in all 4 quadrants:
      locked, unlocked-no-vpn, unlocked-vpn-down, unlocked-vpn-healthy

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

This is the only task that touches the remote (`git push` + `gh pr
create`). All prior commits stay local until this point.

---

## Cross-cutting notes

**Frequent commits.** Every task ends with a single commit. There are
~27 code-touching commits across this plan — granular enough to revert
any individual change without taking down the rest.

**No shortcuts on the kiosk binary.** Pi-side build (`./scripts/deploy_cpp.sh
--build`) is the source of truth. Local Mac dev builds may not link
because of libdrm/EGL/etc. The unit tests (`test_media_browser_unit`)
do compile and run on Mac.

**Byparr digest pinning is real risk.** If `ghcr.io/thephaseless/byparr`
is yanked or the latest tag becomes unstable, fall back to the
last-known-good FlareSolverr (`ghcr.io/flaresolverr/flaresolverr:v3.4.6`)
with a noisy log warning that current Cloudflare won't be bypassed.

**Out of scope (do NOT touch in this PR):**
- Indexer pool changes (lives on `feature/mb-source-selection`)
- `minimumSeeders` tuning (same)
- Custom format scoring changes (same)
- Release picker UI (same)
- TMDB-from-host fix via Radarr metadata proxy (future PR)
- `magic_dingus_box_cpp/docs/MEDIA_BROWSER_PLAYBACK_AND_DOWNLOADS.md` updates.
  Per `.gitignore`, the bulk of `magic_dingus_box_cpp/docs/` is excluded
  from the repo (Pi-local reference material only). Updating it would
  require adding another `.gitignore` exception. Spec §7d's request to
  refresh that doc's architecture diagram is acknowledged but deferred —
  CLAUDE.md (Task 2) carries the canonical reframe; the Pi-local
  PLAYBACK_AND_DOWNLOADS.md can be hand-edited on the source Pi as a
  separate maintenance pass when convenient.
