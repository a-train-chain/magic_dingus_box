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
   From your Mac, open an SSH tunnel (see "Fine print" section below):
   ```bash
   ssh -L 8080:localhost:8080 magic@magicpi.local
   ```
   Then browse to `http://localhost:8080`, log in with the printed
   password, go to Tools → Options → Web UI → change password.

4. **Add a legal indexer in Prowlarr**
   With an SSH tunnel open (`ssh -L 9696:localhost:9696 magic@magicpi.local`),
   browse to `http://localhost:9696`. Go to Indexers → Add.
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
   Radarr → Settings → Profiles. Confirm "HD-1080p" exists (Radarr ships
   this built-in on every fresh install; the kiosk uses it by default for
   new movie adds).

## VPN setup (ProtonVPN + Gluetun)

qBittorrent traffic is routed through a Gluetun VPN container with kill-switch.
If the VPN drops, no traffic leaves the container — your real IP never leaks.

### Get ProtonVPN OpenVPN credentials

1. Sign up for the free tier at https://account.protonvpn.com/signup
2. Log in to the web dashboard at https://account.protonvpn.com/account-password
3. Scroll to **OpenVPN / IKEv2 username** — this username/password pair is
   DIFFERENT from your account email/password. Use this pair (not your login).

### Set credentials on the Pi

```bash
ssh magic@magicpi.local
sudo -e /opt/magic_dingus_box/services/.env
# Set these:
#   VPN_USERNAME=your-openvpn-username
#   VPN_PASSWORD=your-openvpn-password
#   VPN_COUNTRIES=Netherlands   # free tier supports: Netherlands, Japan, United States
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
