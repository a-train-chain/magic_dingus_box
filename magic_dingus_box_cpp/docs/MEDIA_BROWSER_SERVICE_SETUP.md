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
