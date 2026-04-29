# Media Browser V2 — Service Setup Guide (Operator)

This is a one-time setup guide for the kiosk operator. After completion,
the companion services (Radarr + Prowlarr + qBittorrent + Gluetun + FlareSolverr)
auto-start on every Pi boot.

> **🎯 Recommended path: Content Manager UI (no SSH required)**
>
> Since v1.5.0 the supported flow is fully UI-driven and never requires SSH:
>
> 1. Open the Content Manager: `http://magicpi-XXXX.local:5000`
> 2. On the Pi, enter the kiosk's secret-sequence chord to unlock Media Browser
>    visibility (BTN1+BTN3 chord → BTN2 ×3 → rotary click).
> 3. Refresh the Content Manager — a **"Media Browser"** tab appears.
> 4. Drop in your WireGuard `.conf` file from the ProtonVPN dashboard
>    (**NAT-PMP must be enabled when generating it**).
> 5. The backend writes `services/.env`, runs `setup_services.sh` in the
>    background, and the frontend polls progress for ~90 seconds.
> 6. When it returns "healthy", every Custom Format, indexer, app
>    integration, and download client is already wired up from
>    [`magic_dingus_box_cpp/scripts/data/`](../scripts/data/) fixtures —
>    Radarr scoring rules, Prowlarr indexers (TPB, YTS, LimeTorrents,
>    TorrentDownload + 5 disabled-but-pre-configured), FlareSolverr proxy
>    binding, qBittorrent download category, and quality definitions.
>
> The rest of this document covers the **legacy manual flow** for power users,
> debugging, and recovery scenarios where the UI path can't be used.

## Prerequisites (manual flow)

- Kiosk Pi running with Phase 1+ kiosk binary deployed
- USB3 SSD mounted at `/mnt/ssd` (REQUIRED — do not skip)
- Network connectivity
- Sudo access on the Pi

## Manual one-time setup

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
   The script is **idempotent** — safe to re-run if anything fails partway.
   It generates random secrets in `/opt/magic_dingus_box/services/.env`,
   starts the Docker stack, captures Radarr + Prowlarr API keys after
   first boot, and writes them back to `.env`. Save the printed
   credentials in a password manager.

3. **Drop in WireGuard config (PREFERRED — NAT-PMP-capable)**
   The codified setup expects a WireGuard config from ProtonVPN with
   NAT-PMP enabled. Place the file at
   `/opt/magic_dingus_box/services/config/gluetun/wireguard/wg0.conf`
   and ensure `services/.env` has `VPN_TYPE=wireguard` (the default).
   `magic-dingus-services.service` will pick up the change on next restart.

4. **Verify Custom Formats + indexers were applied**
   `setup_services.sh` runs the codified fixture scripts at the end:
   - [`scripts/data/radarr-custom-formats.json`](../scripts/data/radarr-custom-formats.json) — H.264 prefer / HEVC reject / Remux reject / HDR reject scoring
   - [`scripts/data/prowlarr-indexers.json`](../scripts/data/prowlarr-indexers.json) — TPB, YTS, LimeTorrents, TorrentDownload (CloudFlare-tagged)
   - [`scripts/data/prowlarr-flaresolverr.json`](../scripts/data/prowlarr-flaresolverr.json) — proxy binding for `cloudflare`-tagged indexers
   - [`scripts/data/prowlarr-apps.json`](../scripts/data/prowlarr-apps.json) — Radarr integration
   - [`scripts/data/radarr-download-clients.json`](../scripts/data/radarr-download-clients.json) — qBittorrent client wiring
   - [`scripts/data/radarr-quality-definitions.json`](../scripts/data/radarr-quality-definitions.json) — 720p ≤60 MB/min / 1080p ≤100 MB/min size caps

   Re-running `setup_services.sh` is the way to re-apply these fixtures
   if you've manually edited Radarr/Prowlarr state and want to reset.

## VPN setup (ProtonVPN + Gluetun, WireGuard + NAT-PMP)

qBittorrent traffic is routed through a Gluetun VPN container with kill-switch.
If the VPN drops, no traffic leaves the container — your real IP never leaks.

**Use WireGuard, not OpenVPN.** OpenVPN works for the tunnel itself but
cannot do NAT-PMP port forwarding, so incoming peer connections from the
swarm can never reach qBittorrent and download speeds collapse to "leech-only"
levels. WireGuard with NAT-PMP enabled is the only configuration the
companion-services stack is tested with. The legacy OpenVPN env vars
(`VPN_USERNAME` / `VPN_PASSWORD` / `VPN_COUNTRIES`) are unused on
WireGuard configs.

### Get ProtonVPN WireGuard config

1. Sign up at https://account.protonvpn.com/signup (paid plan recommended —
   free tier blocks P2P/BitTorrent traffic regardless of protocol).
2. Open the WireGuard configuration page in your dashboard.
3. **Toggle "NAT-PMP / Port Forwarding" ON before generating** — this
   setting is baked into the generated `.conf` and cannot be retroactively
   added. If you forget, regenerate the config.
4. Download the `.conf` file.

### Set the config on the Pi (preferred: Content Manager UI)

Drop the `.conf` into the **Media Browser** tab of the Content Manager.
The backend writes it to `services/config/gluetun/wireguard/wg0.conf`,
ensures `services/.env` has `VPN_TYPE=wireguard`, and restarts
`magic-dingus-services.service`. ~90 seconds later: Gluetun reports
healthy, and `qbit-port-sync.timer` (running once/minute) syncs
qBittorrent's listen port to whatever Gluetun's NAT-PMP lease is currently
forwarding.

### Set the config on the Pi (manual)

```bash
ssh magic@magicpi.local
sudo cp /path/to/your-wg0.conf /opt/magic_dingus_box/services/config/gluetun/wireguard/wg0.conf
sudo chown magic:magic /opt/magic_dingus_box/services/config/gluetun/wireguard/wg0.conf
sudo systemctl restart magic-dingus-services.service
```

### Verify VPN is up + kill-switch works

```bash
# Check IP as seen by the Internet — should be Dutch (VPN exit node),
# NOT your home IP:
sudo docker exec mdb_gluetun wget -qO- https://ifconfig.me
# Expected: a Netherlands IP, e.g. 185.x.x.x

# Kill-switch test: kill gluetun, verify qBittorrent can't reach the internet:
sudo docker stop mdb_gluetun
sudo docker exec mdb_qbittorrent wget -qO- --timeout=5 https://ifconfig.me
# Expected: connection timeout (kill-switch blocking traffic)
sudo docker start mdb_gluetun
# qBittorrent automatically regains internet when Gluetun recovers.
```

### NOTE — ProtonVPN free tier P2P restriction

ProtonVPN free tier **does not permit P2P / BitTorrent** traffic on its servers.
The tunnel will come up and qBittorrent's WebUI will work, but actual
torrent downloads will be blocked or severely throttled until you upgrade
to a paid plan. All non-BitTorrent traffic works fine (HTTPS downloads
from Internet Archive, etc.).

### Radarr → qBittorrent hostname

When adding qBittorrent as a download client in Radarr's web UI, use hostname
**`gluetun`** (not `qbittorrent`), because qBittorrent shares Gluetun's
network namespace. Port stays `8080`.

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

## Fine print — admin access via SSH tunnel

All service web UIs are bound to `127.0.0.1` on the Pi — they are NOT
reachable from other devices on the LAN (defense in depth; zero
network attack surface even if a malicious device joins your Wi-Fi).

To access them from your Mac/phone for admin config, open an SSH
tunnel from a trusted device:

```bash
ssh -L 7878:localhost:7878 \
    -L 9696:localhost:9696 \
    -L 8080:localhost:8080 \
    -L 8191:localhost:8191 \
    magic@magicpi.local
```

Then on that device, browse to:

- Radarr:       `http://localhost:7878`
- Prowlarr:     `http://localhost:9696`
- qBittorrent:  `http://localhost:8080`
- FlareSolverr: `http://localhost:8191`

Keep the SSH session open while using the UIs. Close the SSH session
when done — the tunnels close with it.
