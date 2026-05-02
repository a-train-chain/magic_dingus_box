# Media Browser — VPN-Required Architecture

**Date:** 2026-05-01
**Status:** Design approved, awaiting implementation plan
**Branch:** `feature/vpn-required-media-browser` (off `main`, NOT off `feature/mb-source-selection`)

## Background & Motivation

Magic Dingus Box has two functional halves:

1. **Retro gaming kiosk** — always works, no VPN required.
2. **Movie Media Browser** — discovers, downloads, and plays movies via Radarr/Prowlarr/qBittorrent/Gluetun.

Today the Media Browser leaks privacy in three ways:

1. **Only qBittorrent is VPN'd.** Prowlarr (indexer searches), Radarr (torrent grabs, queue polls), and FlareSolverr (Cloudflare bypass for indexer scraping) all exit via the bare Comcast residential IP. The ISP can see exactly which torrent indexers and tracker domains the Pi talks to, even though the actual peer traffic goes through Gluetun.
2. **The Pi prefers IPv6 outbound.** Gluetun's WireGuard tunnel is IPv4-only. When IPv6 is available on the LAN (Comcast hands out `2601:204:c202:5530::/64` typically), routes for IPv4 destinations resolve to v4 inside the tunnel but routes for v6 destinations bypass the tunnel entirely. Any service or application that picks v6 leaks.
3. **Host DNS goes through Comcast (`75.75.75.75`).** Even when actual TCP connections route through Gluetun, the indexer-domain lookups happen over the residential resolver. ISP gets a clean log of "which trackers does this household resolve."

This is also functionally limiting: the existing FlareSolverr (`v3.4.6`) can no longer bypass Cloudflare's current challenge format, and Prowlarr `1.26.1`'s bundled Cardigann definitions are stale, which is why the recent indexer-pool expansion on `feature/mb-source-selection` only kept 1 of 6 newly-added indexers alive.

Goal: make the Media Browser a strictly VPN-required feature, formalize that it cannot be enabled without a working WireGuard tunnel, route every torrent-ecosystem service through Gluetun, and bump the Cloudflare-bypass + indexer-definition stack to working versions while we're in there.

## Goals

- **Single architecture mode**, no per-deployment opt-out: when the Media Browser is enabled, all of Prowlarr / Radarr / Byparr / qBittorrent route through Gluetun.
- **Three-layer feature gating** so visibility, configuration, and runtime use are independently checked and produce distinct failure modes.
- **Close the IPv6 leak** by disabling outbound IPv6 globally on the Pi.
- **Close the DNS leak** by routing all host DNS through DNS-over-HTTPS to Cloudflare.
- **Bump Prowlarr to 2.3.5.5327** (refreshes Cardigann definitions, may rescue indexers that fell out on the source-selection branch).
- **Replace FlareSolverr with Byparr** which handles current Cloudflare challenge formats.
- **Operator setup workflow remains unchanged from the user's perspective**: drop a WireGuard `.conf` into Content Manager, wait ~90 seconds, services come up.

## Non-Goals

- No multi-VPN support. ProtonVPN-with-WireGuard remains the assumed and documented stack.
- No UI for selecting which services should/shouldn't be VPN'd. The architecture is fixed: torrent-ecosystem services are all VPN'd; everything else (kiosk binary, web admin, RetroArch, GStreamer) is not.
- No changes to indexer pool, scoring, custom formats, or `minimumSeeders` tuning. Those land separately on `feature/mb-source-selection`.
- No changes to playback pipeline (GStreamer, hardware decode, codec gating).
- No changes to the family-safe content filter.
- No changes to Confirm Remove flow.
- TMDB-from-the-kiosk-binary remains a documented accepted gap (see Open Items).

---

## Design

### 1. Three-layer feature gating

Three independent gates, each with one job. Combining them gives clean failure modes that map to operator-actionable states.

| # | Layer | Stored at | Means | Gates |
|---|---|---|---|---|
| 1 | **Unlocked** | `playback.media_browser_unlocked` in `config/settings.json` | Operator entered the secret sequence on the kiosk | Tab + Settings entry visibility |
| 2 | **Configured** | `WIREGUARD_PRIVATE_KEY` non-empty in `services/.env` | An operator dropped a WireGuard config in Content Manager | Functional `/admin/media-browser/*` endpoints; kiosk MB launch |
| 3 | **Healthy** | Live: Radarr `/ping` reachable at `localhost:7878` | Gluetun tunnel up + torrent stack running | Runtime use (kiosk shows toast on drop, web admin shows banner) |

**Decision matrix:**

| State | Web admin tab? | Kiosk MB Settings entry? |
|---|---|---|
| Layer 1 fail (locked) | hidden | hidden |
| Layer 1 pass, Layer 2 fail (no VPN config) | **visible — shows VPN-setup form** | hidden, with row showing "Configure VPN in Content Manager" |
| Layer 1 + 2 pass, Layer 3 fail (tunnel down) | visible, banner "Tunnel down" | hidden, toast "Media Browser unavailable — VPN tunnel down" |
| All three pass | visible, fully functional | visible, fully functional |

**Why Layer 1 alone gates the web-admin tab:** the operator's per-Pi setup workflow (CLAUDE.md) requires the Content Manager's Media Browser tab to be visible after unlock so they can drop the WireGuard config there. If Layer 2 gated the tab, there'd be no path to configure VPN.

**Why Layer 3 uses Radarr `/ping` as the signal:** Radarr (and Prowlarr, and Byparr) share Gluetun's network namespace once we're done. When Gluetun is down, all of them are unreachable from the host. One HTTP call covers the whole stack's health, requires no special port mapping for Gluetun's control server, and avoids `docker exec` from the kiosk binary.

**First-boot race ("Gluetun starting but not yet healthy"):** Layer 3 monitor uses a debounce — three consecutive failed polls at 10s intervals (~30s) before flipping `media_browser_vpn_healthy` from `true` to `false`. Recovery is instant on the first successful poll. Initial state at boot is `Unknown`, treated as `false` for visibility purposes but suppresses the "tunnel down" toast.

### 2. Docker stack — single mode, four services behind Gluetun

**File:** `magic_dingus_box_cpp/services/docker-compose.yml`

End-state network topology:

```
gluetun (mdb_gluetun)        ← VPN tunnel + control server (:8000 internal)
  ├─ qbittorrent             (existing — already in netns)
  ├─ prowlarr                (NEW: enters netns)
  ├─ radarr                  (NEW: enters netns)
  └─ byparr                  (NEW: replaces flaresolverr, in netns)
```

#### 2a. Move Prowlarr / Radarr / Byparr behind Gluetun

For each of these three services in `docker-compose.yml`:

- Add `network_mode: "service:gluetun"`.
- Add `depends_on: { gluetun: { condition: service_healthy } }`.
- **Drop** the per-service `dns:` block (gluetun handles DNS for the shared netns).
- **Drop** the per-service `ports:` block (port publishing happens on gluetun).
- Container name unchanged (`mdb_radarr`, `mdb_prowlarr`); for byparr: `mdb_byparr`.
- Healthcheck remains on the same internal port; docker compose runs healthchecks inside the container's netns.

#### 2b. Gluetun absorbs port publishing

Add to gluetun's existing `ports:` block:

```yaml
ports:
  # qBit web UI (existing)
  - "127.0.0.1:8080:8080"
  # BitTorrent peer port (existing)
  - "6881:6881"
  - "6881:6881/udp"
  # NEW: Radarr admin UI
  - "127.0.0.1:7878:7878"
  # NEW: Prowlarr admin UI
  - "127.0.0.1:9696:9696"
  # NEW: Byparr (Cloudflare bypass, internal use)
  - "127.0.0.1:8191:8191"
```

All admin/internal ports stay loopback-only. Only the BitTorrent peer port (6881) is publicly bindable per existing convention.

#### 2c. Bump Prowlarr 1.26.1 → 2.3.5.5327

Image: `lscr.io/linuxserver/prowlarr:2.3.5.5327`. The 2.x series ships current Cardigann definitions, which is what makes the bump worthwhile. The Prowlarr config-on-disk format is forward-compatible across this jump per LinuxServer's release notes.

#### 2d. Replace FlareSolverr with Byparr

Image: `ghcr.io/thephaseless/byparr@sha256:<digest-resolved-at-PR-time>`. Byparr is API-compatible with FlareSolverr (same `/v1` endpoint shape) so Prowlarr's existing FlareSolverr proxy entry continues to work. The proxy *name* in Prowlarr stays `flaresolverr` so the indexer→tag bindings already in `prowlarr_indexers.json` don't drift.

**Pinned tag rationale:** the file's "all images pinned for reproducibility" invariant requires a sha-pinned digest, not `:latest`. Resolve `docker buildx imagetools inspect ghcr.io/thephaseless/byparr:latest --format '{{json .Manifest.Digest}}'` at PR-write time and commit the literal digest.

**ghcr.io DNS race mitigation:** Pi-side first-pull of byparr has been observed to fail with DNS timeouts (the user flagged this directly). Add a pre-step in `setup_services.sh` Step 4:

```bash
echo "Pre-pulling Byparr (ghcr.io DNS can be flaky on first boot)..."
for i in 1 2 3; do
    if docker pull ghcr.io/thephaseless/byparr@sha256:<digest>; then
        break
    fi
    [ $i -eq 3 ] && { echo "ERROR: cannot pull byparr after 3 attempts"; exit 1; }
    echo "Pull attempt $i failed; sleeping 10s..."
    sleep 10
done
```

#### 2e. Container-internal URL substitutions

When Prowlarr and Radarr enter Gluetun's netns, inter-service URLs change because they're now on `localhost` to each other:

| Where | Before | After |
|---|---|---|
| Prowlarr's Apps integration: Radarr URL | `http://radarr:7878` | `http://localhost:7878` |
| Radarr's qBit download client: Host | `gluetun` | `localhost` |
| Prowlarr's FlareSolverr proxy URL | `http://flaresolverr:8191` | `http://localhost:8191` |

These are codified in the JSON fixtures pushed by `setup_services.sh` (step numbers verified against the current script):

- `magic_dingus_box_cpp/scripts/data/prowlarr_applications.json` — pushed in Step 14 (Apps integration). Radarr URL changes from `http://radarr:7878` → `http://localhost:7878`.
- `magic_dingus_box_cpp/scripts/data/prowlarr_indexerproxies.json` — pushed in Step 12 (FlareSolverr-API proxy). URL changes from `http://flaresolverr:8191` → `http://localhost:8191`. Proxy entry *name* stays `flaresolverr` so indexer→tag bindings don't drift.
- `magic_dingus_box_cpp/scripts/data/radarr_downloadclients.json` — pushed in Step 15 (Radarr → qBit download client). Host changes from `gluetun` → `localhost`.

### 3. Pi host networking — close leak paths

`setup_services.sh` gets a new **Step 0** (runs first, before any Docker work) that idempotently applies host-level network changes. Add a `--skip-host-networking` flag for re-runs that don't want to touch host config (useful when iterating on services config without touching system-level settings).

#### 3a. Disable IPv6 globally

Write `/etc/sysctl.d/99-magic-dingus-disable-ipv6.conf`:

```
net.ipv6.conf.all.disable_ipv6 = 1
net.ipv6.conf.default.disable_ipv6 = 1
net.ipv6.conf.lo.disable_ipv6 = 1
```

Apply with `sysctl -p /etc/sysctl.d/99-magic-dingus-disable-ipv6.conf` so the change takes effect without reboot.

**Side-effect analysis:**

- **Kiosk binary's TMDB calls** — fall back to IPv4 fine. (TMDB has both A and AAAA records; resolver behavior favors A when v6 is administratively disabled.)
- **OTA update GitHub API calls** — IPv4 fine.
- **mDNS (`magicpi.local`)** — uses IPv4 multicast `224.0.0.251` independently of IPv6; unaffected.
- **Apt / system updates** — IPv4 fine.
- **WireGuard tunnel inside Gluetun** — already IPv4-only; unaffected.

#### 3b. Host DNS via Cloudflare DNS-over-HTTPS

Why DoH not just `nameserver 1.1.1.1`: a static `1.1.1.1` resolver in `/etc/resolv.conf` swaps Comcast for Cloudflare as the destination, but the queries themselves still travel as cleartext UDP/53 — the ISP still sees "this Pi looked up `1337x.to`." DoH (DNS-over-HTTPS) tunnels the queries inside TLS to `1.1.1.1:443` so the ISP sees only an opaque encrypted stream.

Steps:

1. `apt install cloudflared` (use Cloudflare's `pkg.cloudflare.com` repo if not already on the Pi — installer adds it idempotently).
2. Write `/etc/cloudflared/config.yml`:
   ```yaml
   proxy-dns: true
   proxy-dns-port: 53
   proxy-dns-address: 127.0.0.1
   proxy-dns-upstream:
     - https://1.1.1.1/dns-query
     - https://1.0.0.1/dns-query
   ```
3. `systemctl enable --now cloudflared-proxy-dns.service` (created by the apt package; the unit file already exists, we just enable it).
4. Write `/etc/NetworkManager/conf.d/99-dns.conf`:
   ```ini
   [main]
   dns=none
   ```
   This stops NetworkManager from rewriting `/etc/resolv.conf` from DHCP.
5. Reload NM (`systemctl reload NetworkManager`), then write `/etc/resolv.conf`:
   ```
   nameserver 127.0.0.1
   options edns0 trust-ad
   ```
   `chattr +i /etc/resolv.conf` is *not* used (some distros' DHCP clients still try to write it; immutable bit causes confusing failures). Idempotency is enough.

**Container-side DNS:** Docker's internal `127.0.0.11` resolver inherits host DNS. For services in Gluetun's netns, gluetun handles DNS via the VPN provider's DNS servers (already configured by gluetun image). For services not in gluetun's netns (none after this change), the host's DoH resolver applies. Net effect: every DNS query from the Pi now leaves the host either via DoH-to-Cloudflare or via Gluetun-to-VPN-provider. Neither is Comcast.

**SNI nuance for non-VPN'd traffic:** DoH hides the DNS query but does not hide the destination IP or the TLS Server Name Indication. For host-level outbound traffic that doesn't go through Gluetun (kiosk binary's TMDB calls, OTA update GitHub calls, the cloudflared connection itself), the ISP can still infer the destination domain from the cleartext SNI in the TLS handshake. This is a meaningful privacy improvement (no Comcast-resolver query log) but not a complete one for non-VPN'd traffic. Documented in the privacy notes; ECH is the eventual fix and is out of scope for this PR.

#### 3c. Existing skip-when-unconfigured guard

Keep `ConditionPathExists=/opt/magic_dingus_box/services/.env` on `magic-dingus-services.service` as-is. Unprovisioned Pis cleanly skip the Docker stack instead of fail-looping.

### 4. Web admin (`magic_dingus_box/web/admin.py`)

#### 4a. Helpers + decorator

Replace the eight scattered `if not _media_browser_unlocked(): return _media_browser_locked_response()` checks with a centralized gate function:

```python
def _vpn_configured() -> bool:
    """True iff services/.env exists AND has a non-empty WIREGUARD_PRIVATE_KEY."""
    try:
        env = _read_env_file(SERVICES_ENV)
        return bool(env.get("WIREGUARD_PRIVATE_KEY", "").strip())
    except Exception:
        return False


def _vpn_required_response():
    return error_response(
        "vpn_not_configured",
        "VPN must be configured in the Media Browser tab before using this feature",
        status=403,
    )


def _check_media_browser_gates(*, require_vpn: bool = True):
    """Run the appropriate gates. Returns None on pass, or a 403 Response on fail.

    Endpoints that are part of the VPN-setup flow itself pass require_vpn=False so
    the operator can reach them before configuring VPN.
    """
    if not _media_browser_unlocked():
        return _media_browser_locked_response()
    if require_vpn and not _vpn_configured():
        return _vpn_required_response()
    return None
```

Each guarded endpoint becomes a one-liner:

```python
@app.get("/admin/media-browser/<some-route>")
def some_route():
    if (resp := _check_media_browser_gates()):
        return resp
    # ... rest of handler
```

#### 4b. Endpoint-level gating policy

Actual endpoint names verified against `admin.py`:

| Endpoint | Layer 1 (unlock)? | Layer 2 (VPN configured)? |
|---|---|---|
| `GET /admin/media-browser/visibility` | informational only | informational only |
| `GET /admin/media-browser/status` | yes | **no** (operator polls during setup to check progress) |
| `POST /admin/media-browser/setup` (WG drop-in target) | yes | **no** (this is what configures VPN) |
| `GET /admin/media-browser/setup-status/<job_id>` | yes | **no** |
| `POST /admin/media-browser/reset` | yes | **no** (must work even when VPN is broken — that's why you'd reset) |
| `GET /admin/media-browser/credentials` | yes | yes |
| `GET /admin/media-browser/health-summary` | yes | yes |
| `POST /admin/media-browser/restart` | yes | yes |
| Everything else (indexers, profiles, library list, queue, search, etc.) | yes | yes |

#### 4c. `/admin/media-browser/visibility` response shape

Today: `{visible: bool}`. New: `{visible: bool, vpn_configured: bool}`. The frontend uses `visible` to decide whether to render the tab DOM at all (Layer 1), and `vpn_configured` to decide whether to render the "configure VPN" form vs. the full dashboard (Layer 2). Always returns 200 — this endpoint is the one public probe.

```python
@app.get("/admin/media-browser/visibility")
def media_browser_visibility():
    return success_response(data={
        "visible": _media_browser_unlocked(),
        "vpn_configured": _vpn_configured(),
    })
```

#### 4d. Frontend (`static/manager.js`)

The page-init visibility check already exists; extend it to read `vpn_configured`. When `visible == true && vpn_configured == false`, render the tab nav button + a "Set up VPN" form section in place of the full dashboard. When both are true, render the existing dashboard.

The "Reset Media Browser" tear-down flow at `admin.py` ~line 2916 currently lists `["radarr", "prowlarr", "qbittorrent", "gluetun", "flaresolverr"]` — rename `flaresolverr` → `byparr` in that list. (Stopping gluetun stops all four netns-sharing services automatically; no orphan-container concerns.)

### 5. Kiosk C++ (`magic_dingus_box_cpp/src/`)

#### 5a. New module: `media_browser/health/vpn_health_monitor.{h,cpp}`

Background thread that polls Radarr `/ping` on `127.0.0.1:7878` every 10 seconds.

```cpp
namespace media_browser {

enum class VpnHealthState {
    Unknown,    // boot-time before first poll
    Healthy,    // last poll succeeded
    Unhealthy,  // 3+ consecutive failures
};

class VpnHealthMonitor {
public:
    VpnHealthMonitor(app::AppState& state);
    ~VpnHealthMonitor();

    void start();   // spawns worker thread
    void stop();    // joins worker thread

    VpnHealthState state() const;

private:
    void run();
    bool ping_radarr();   // GET http://127.0.0.1:7878/ping, 3s timeout

    app::AppState& state_;
    std::atomic<bool> stop_flag_{false};
    std::thread worker_;
    int consecutive_failures_{0};
    static constexpr int kFailureThreshold = 3;
    static constexpr std::chrono::seconds kPollInterval{10};
};

}  // namespace media_browser
```

The monitor writes two flags into AppState:
- `state.media_browser_vpn_healthy` — true while Layer 3 is up
- (existing) updates a small ring buffer of last-poll timestamps for debug

Monitor lifecycle: started in `main.cpp` after AppState is initialized but only when `media_browser_unlocked == true && media_browser_vpn_configured == true`. Stopped on shutdown.

#### 5b. AppState additions

In `magic_dingus_box_cpp/src/app/app_state.h`, alongside the existing `media_browser_unlocked` flag:

```cpp
bool media_browser_unlocked = false;       // existing — Layer 1
bool media_browser_vpn_configured = false; // NEW — Layer 2 (set at boot)
bool media_browser_vpn_healthy = false;    // NEW — Layer 3 (live, owned by monitor)
```

`media_browser_vpn_configured` is set at boot by reading `/opt/magic_dingus_box/services/.env` and checking for non-empty `WIREGUARD_PRIVATE_KEY`. The kiosk re-reads it once each time the Settings menu opens, so an operator who configures VPN via Content Manager sees the change reflected on the next Settings open without needing to restart the kiosk.

#### 5c. Settings menu changes

`magic_dingus_box_cpp/src/ui/settings_menu.cpp` line 329 currently checks `app_state_->media_browser_unlocked` to decide whether to render Media Browser entries. Extend this:

```cpp
if (app_state_ && app_state_->media_browser_unlocked) {
    if (!app_state_->media_browser_vpn_configured) {
        // Layer 1 pass, Layer 2 fail: show a disabled row with helper text
        render_disabled_row("Media Browser",
                            "Configure VPN in Content Manager to enable");
    } else if (!app_state_->media_browser_vpn_healthy) {
        // Layer 1 + 2 pass, Layer 3 fail: hidden (toast handles user-visible signal)
        // Don't render the entry.
    } else {
        // All three pass — render normally.
        render_media_browser_entries();
    }
}
```

Three states give three distinct UX outcomes. A locked Pi shows nothing. An unlocked-but-unconfigured Pi shows a clear "go configure VPN" pointer. A configured Pi with a tunnel drop just hides the entries silently (toast already explains).

#### 5d. Toast on tunnel drop (`main.cpp`)

When `media_browser_vpn_healthy` transitions `true → false`:

```cpp
toast_manager_.show("Media Browser unavailable — VPN tunnel down",
                    /*duration*/ std::chrono::seconds(8));
```

When transitions `false → true` (recovery): silent. No toast — the operator either is or isn't paying attention; no need to interrupt them.

When transitions `Unknown → false`: silent. Boot-time before the first successful poll shouldn't trigger a "tunnel down" toast.

#### 5e. TMDB-from-host gap

The kiosk binary's `TmdbClient` calls `api.themoviedb.org` directly from the host process. After this work, host-level outbound traffic still leaves via Comcast (only torrent-ecosystem services route through Gluetun). ISP can therefore still see "this Pi calls TMDB" — metadata only, not torrent activity.

This is an accepted gap. Mitigations:

- Document in `MEDIA_BROWSER_VPN_SETUP.md` "Privacy notes" section.
- One-line callout in `CLAUDE.md`.
- Inline comment in `tmdb_client.cpp` at the HTTP call site, pointing at the doc.

Future work (out of scope here): route TMDB through Radarr's metadata proxy (Radarr already fronts TMDB internally for its own use). Tracked as a follow-up, not blocking this PR.

### 6. Setup script (`magic_dingus_box_cpp/scripts/setup_services.sh`)

Existing script is 1162 lines; targeted additions only.

#### 6a. New Step 0: host networking (idempotent)

At the very top, before the Docker install check. Wrapped in a `--skip-host-networking` flag for partial re-runs. Applies the IPv6 disable and DoH DNS config from §3.

#### 6b. New Step 4.5: Gluetun tunnel-up gate

Between `docker compose up -d` and the existing 60s sleep:

```bash
echo "Waiting for Gluetun tunnel to come up..."
for i in {1..60}; do
    if docker exec mdb_gluetun wget -qO- --timeout=3 http://localhost:8000/v1/publicip/ip 2>/dev/null | grep -q '"public_ip"'; then
        EXIT_IP=$(docker exec mdb_gluetun wget -qO- http://localhost:8000/v1/publicip/ip | jq -r .public_ip)
        echo "Tunnel up — exit IP: ${EXIT_IP}"
        break
    fi
    sleep 2
    [ $i -eq 60 ] && { echo "ERROR: Gluetun tunnel did not come up in 120s"; exit 1; }
done
```

Failing here aborts setup with a clear message rather than letting Prowlarr/Radarr fail to reach indexers later in the script.

#### 6c. Step 4 update: pre-pull Byparr

Per §2d, pre-pull Byparr with retry logic before `docker compose up -d`.

#### 6d. URL substitutions in fixture-push steps

Per §2e, update three URLs in the JSON fixtures: `prowlarr_applications.json` (Step 14, Radarr URL), `prowlarr_indexerproxies.json` (Step 12, FlareSolverr-API URL), and `radarr_downloadclients.json` (Step 15, qBit host).

#### 6e. No tuning changes

`minimumSeeders`, indexer pool, custom formats: untouched. Those changes belong on `feature/mb-source-selection`.

### 7. Documentation

#### 7a. README.md (top-of-file rewrite)

Lead with the two-halves narrative:

```markdown
# Magic Dingus Box

A retro gaming and video playback kiosk for Raspberry Pi 4B.

Magic Dingus Box has two halves:

1. **Retro gaming + video playback** — always works, no internet required after setup.
   Plays NES/SNES/Genesis/PS1/PCE/Atari 7800/Arcade games via RetroArch, plus local
   videos and YouTube clips.

2. **Movie Media Browser** — discovers and downloads movies via a Radarr/Prowlarr/
   qBittorrent stack. **Requires a VPN** (ProtonVPN with WireGuard recommended).
   See [docs/MEDIA_BROWSER_VPN_SETUP.md](magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md)
   for setup.

Without a VPN configured, the Media Browser is fully hidden from both the kiosk UI
and the web admin. Operators must explicitly unlock it (kiosk-side secret sequence)
*and* drop a working WireGuard config (web admin) before the feature appears.
```

#### 7b. CLAUDE.md updates

Replace the "Media Browser (Movie Playback + Downloads)" section's framing:

- Update "Feature gating" subsection: replace single-flag explanation with three-layer model.
- Add note: "All four torrent-ecosystem services (Prowlarr, Radarr, Byparr, qBittorrent) share Gluetun's network namespace. When Gluetun is down, all four are unreachable from the host."
- Update "Active indexers" subsection to note Byparr (not FlareSolverr) handles Cloudflare bypass.
- Add "Privacy notes" subsection documenting the TMDB-from-kiosk-host gap.

#### 7c. NEW: `magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md`

Operator guide. Sections:

- **Why VPN is required** — short explanation (1 paragraph): torrent metadata + traffic both go through the VPN; ISP can't see indexer searches or peer connections.
- **What you need** — ProtonVPN account (free tier insufficient because no port forwarding; Plus tier or higher), WireGuard config from the dashboard, NAT-PMP enabled when generating.
- **Step-by-step** — exact UI flow: kiosk secret sequence → unlock → Content Manager refresh → Media Browser tab appears → drop `.conf` → wait for setup job → done.
- **Troubleshooting** —
  - Tunnel won't come up
  - NAT-PMP port reads as 0
  - Byparr image won't pull (DNS race)
  - Indexers all show 0 results
- **Privacy notes** — what the ISP can and can't see. Explicit callout for the TMDB-from-host gap.

#### 7d. `magic_dingus_box_cpp/docs/MEDIA_BROWSER_PLAYBACK_AND_DOWNLOADS.md` updates

Update the "Architecture" section topology diagram to reflect single-mode all-behind-gluetun. Update the "Service operations" subsection to note the four-services-in-gluetun-netns invariant.

### 8. Testing

#### 8a. New unit test: `tests/media_browser/test_vpn_health_monitor.cpp`

Mock the HTTP ping. Cover:
- Initial state is `Unknown`.
- One successful poll → `Healthy`, sets `state.media_browser_vpn_healthy = true`.
- Three consecutive failed polls → `Unhealthy`, sets flag false.
- Two failed polls → still `Healthy` (debounce protects against flapping).
- Recovery: any successful poll while in `Unhealthy` → immediately `Healthy`.
- Stop joins thread within reasonable time.

#### 8b. New smoke-check script: `magic_dingus_box_cpp/scripts/check_vpn_required.sh`

Operator-runnable on a Pi. For each of the four expected-VPN'd containers (gluetun, qbittorrent, radarr, prowlarr, byparr), run `docker exec <container> wget -qO- ifconfig.me/ip` and verify the result equals gluetun's reported `/v1/publicip/ip`. Exits non-zero with a clear diff if any container's exit IP differs (indicating a leak).

Also verifies host-level: `curl -6 -m 5 ifconfig.me` should fail (IPv6 disabled), and `dig +short cloudflare.com` should resolve via `127.0.0.1` (DoH active).

#### 8c. Manual integration test (per CLAUDE.md doc convention)

On a fresh Pi with no `.env`:
1. Verify Media Browser tab is hidden in Content Manager.
2. Enter secret sequence on kiosk → tab appears, but in "Configure VPN" state.
3. Drop ProtonVPN WG config → setup runs → ~90s later all four services healthy, exit IP visible in dashboard.
4. Verify kiosk Settings menu now shows Media Browser entries.
5. Stop gluetun (`docker stop mdb_gluetun`) — within 30s, kiosk hides MB entries and shows toast.
6. Restart gluetun — within 10s, MB entries silently reappear.
7. Run `check_vpn_required.sh` and verify all containers report the gluetun exit IP.

### 9. Component data flow

```
                                     ┌─────────────────────────────┐
                                     │  Pi host (no VPN, IPv4-only)│
                                     │                             │
TMDB ◄──────────────────────────────┤  TmdbClient (in kiosk bin)  │
GitHub OTA ◄─────────────────────────┤  update.sh                  │
Cloudflare DoH (1.1.1.1:443) ◄───────┤  cloudflared (resolver)     │
                                     │                             │
                                     │  ┌───────────────────────┐  │
                                     │  │  gluetun netns        │  │
ProtonVPN WG endpoint ◄──────────────┼──│  ┌─────┐ ┌─────────┐  │  │
TMDB (via Radarr internal) ◄─────────┼──│  │ rad │ │ prow    │  │  │
Indexer trackers ◄───────────────────┼──│  │ arr │ │ larr    │  │  │
BitTorrent peers ◄───────────────────┼──│  └──┬──┘ └─────────┘  │  │
Cloudflare-fronted indexers ◄────────┼──│     │  ┌──────┐       │  │
                                     │  │     ▼  │ byparr│       │  │
                                     │  │  ┌──────────┐  │       │  │
                                     │  │  │ qbittor- │  │       │  │
                                     │  │  │ rent     │  │       │  │
                                     │  │  └──────────┘  │       │  │
                                     │  └────────────────┘       │  │
                                     │                             │
                                     │  Web admin (Flask, no VPN) │
                                     │  Kiosk binary (no VPN)     │
                                     └─────────────────────────────┘
```

### 10. Files touched

**New:**
- `magic_dingus_box_cpp/src/media_browser/health/vpn_health_monitor.{h,cpp}`
- `magic_dingus_box_cpp/scripts/check_vpn_required.sh`
- `magic_dingus_box_cpp/docs/MEDIA_BROWSER_VPN_SETUP.md`
- `tests/media_browser/test_vpn_health_monitor.cpp`
- `docs/superpowers/specs/2026-05-01-vpn-required-media-browser-design.md` (this doc)
- `docs/superpowers/plans/2026-05-01-vpn-required-media-browser.md` (next step, via writing-plans skill)

**Modified:**
- `magic_dingus_box_cpp/services/docker-compose.yml` — Prowlarr/Radarr/Byparr enter gluetun netns; ports absorbed; Prowlarr 2.x bump; FlareSolverr→Byparr swap
- `magic_dingus_box_cpp/scripts/setup_services.sh` — new Step 0 (host net), Step 4 pre-pull, Step 4.5 tunnel gate, URL substitutions
- `magic_dingus_box_cpp/scripts/data/prowlarr_applications.json` — Radarr URL `radarr` → `localhost`
- `magic_dingus_box_cpp/scripts/data/prowlarr_indexerproxies.json` — proxy URL `flaresolverr` → `localhost`
- `magic_dingus_box/web/admin.py` — `_vpn_configured` helper, `_check_media_browser_gates` decorator-style helper, replace 8 scattered checks, extend `/visibility` response shape, rename flaresolverr→byparr in tear-down
- `magic_dingus_box/web/static/manager.js` — read `vpn_configured` from `/visibility`, render configure-vs-use UI accordingly
- `magic_dingus_box_cpp/src/app/app_state.h` — add `media_browser_vpn_configured` and `media_browser_vpn_healthy` flags
- `magic_dingus_box_cpp/src/main.cpp` — instantiate VpnHealthMonitor when Layers 1+2 pass; toast on Layer 3 transition
- `magic_dingus_box_cpp/src/ui/settings_menu.cpp` — extend Layer 1 guard to three layers, render disabled row in unconfigured state
- `magic_dingus_box_cpp/src/media_browser/tmdb_client.cpp` — inline comment about TMDB-from-host gap
- `README.md` — top-of-file rewrite
- `CLAUDE.md` — three-layer-gating reframe + Byparr + privacy notes

## Error Handling

- **Gluetun unreachable at boot** — kiosk renders MB Settings entries as hidden (Layer 3 fail), no toast (Unknown→Unhealthy isn't a "drop" event).
- **Gluetun drops mid-session** — kiosk hides MB entries within ~30s (3 polls), shows toast once. Web admin's `/admin/media-browser/status` reports `services_running: false`; the dashboard banner switches to "VPN tunnel down."
- **Byparr ghcr.io pull fails** — `setup_services.sh` retries 3× then aborts with a clear error. Operator can re-run `setup_services.sh` once DNS recovers (the script is idempotent).
- **Operator drops invalid WireGuard config** — Gluetun fails healthcheck; tunnel-up gate in Step 4.5 times out at 120s; setup_services exits with error. `setup-job/*` endpoint surfaces the tail of the log to the operator.
- **`/admin/media-browser/visibility` while web admin can't read settings.json** — falls through to `{visible: false, vpn_configured: false}` (failure-closed). Same pattern as existing `_media_browser_unlocked()`.
- **Race between operator-drops-VPN-config and setup-job-finishing** — kiosk's Layer 2 flag is set at boot; on first successful Settings menu open after setup completes, re-read `services/.env` to pick up the new `WIREGUARD_PRIVATE_KEY`.

## Open Items

- **TMDB-from-host gap.** Accepted as a documented limitation. Future work: route through Radarr's metadata proxy. Not blocking this PR.
- **Restart-via-Content-Manager button** for "Gluetun is down, kick the stack." Not blocking; operator can run `systemctl restart magic-dingus-services` manually or via SSH.
- **DoH-via-cloudflared package install path.** The script assumes either a working apt source for cloudflared or that the package is preinstalled in the golden image. Need to confirm during plan-writing whether to bake cloudflared into the golden image or install at setup_services.sh time.
- **Byparr digest pin.** Resolve at PR-write time. Implementer must `docker buildx imagetools inspect ghcr.io/thephaseless/byparr:latest` and commit the literal digest in compose. If thephaseless's image is unstable, fall back to the previous-known-good FlareSolverr version with a noisy warning rather than ship broken Cloudflare bypass.
