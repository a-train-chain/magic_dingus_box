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

- **ProtonVPN account, Plus tier or higher (recommended).** Free-tier
  ProtonVPN doesn't support port forwarding, which kills BitTorrent peer
  connectivity. Other WireGuard providers DO work — the setup accepts any
  standard WireGuard `.conf` and falls back to a custom-provider
  configuration — but only ProtonVPN's NAT-PMP port forwarding is wired
  into the port-sync service, so incoming peer connectivity is best on
  ProtonVPN.
- **A WireGuard config file (.conf) downloaded from the ProtonVPN
  dashboard.** When generating it, you MUST enable the "NAT-PMP /
  Port Forwarding" toggle. Without it, peer connections fail.
- **Network access to the Pi's Content Manager**
  (`http://<your-box>.local:5000` — each unit is named `magicpi-XXXX` at
  first boot; the kiosk's Settings → Connect a Device screen shows the
  exact address — or `http://dingus.box` / `http://10.55.0.1:5000` over
  the USB-C cable).

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
   - Applies host network changes (disable IPv6, swap DNS to
     Cloudflare 1.1.1.1 so the ISP resolver no longer sees host
     queries).
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

Cause: ghcr.io DNS sometimes hiccups on a fresh Pi or after a network
reconnect. The setup script retries 3× with 10s sleep; if all three
fail, it aborts.

Recovery: wait a minute, then re-run setup from the Content Manager.
By the second run, DNS is generally stable.

### All indexers show 0 results

Symptom: Media Browser detail screen shows no available releases for
any movie.

Likely causes:
- Cloudflare-fronted indexers (1337x, TheRARBG) require Byparr.
  Check Byparr is running: `docker ps | grep byparr`. If absent,
  re-run setup.
- Indexer site is genuinely down or has changed its API. Check the
  Prowlarr UI directly (`http://localhost:9696` through an SSH tunnel
  to the box — it's loopback-bound) for per-indexer health.

## Privacy notes

**What the ISP can't see** after this setup:

- Indexer searches (Prowlarr's queries to 1337x, YTS, etc. all exit
  via the VPN).
- Movie metadata fetched by Radarr (Radarr's TMDB calls exit via VPN
  because Radarr is in Gluetun's netns).
- BitTorrent peer connections (qBittorrent has always been behind
  Gluetun).
- The ISP's own DNS resolver no longer sees host-level queries
  (resolv.conf points at Cloudflare 1.1.1.1).

**What the ISP can still see** (accepted gaps):

- The kiosk binary's own TMDB calls. Magic Dingus Box's main
  application process — the kiosk binary itself — calls
  `api.themoviedb.org` directly from the host network when browsing
  movies in the kiosk UI. This is metadata only (poster fetches,
  search queries) and never touches torrent indexers, but it does
  reveal "this Pi looks at TMDB." Routing through Radarr's
  metadata proxy is future work.
- Host-level DNS queries are sent in cleartext UDP/53 to Cloudflare
  (1.1.1.1). The ISP no longer resolves them, but anyone observing
  the wire between the Pi and Cloudflare can still see *which*
  domains the host queries. Affects only host-level non-VPN'd
  traffic (kiosk-binary TMDB, OTA updates from GitHub, apt). All
  torrent-related DNS happens inside Gluetun's netns and is
  encrypted via the WireGuard tunnel.
- The TLS Server Name Indication (SNI) for any non-VPN'd outbound
  TLS connection — also cleartext until ECH is widely deployed.
  Same scope as the DNS leak: kiosk-binary TMDB, OTA, apt.

Future work: switch DNS to dnscrypt-proxy or systemd-resolved with
DNS-over-TLS to encrypt the host-level queries. Was originally
attempted with cloudflared, but Cloudflare deprecated cloudflared's
DNS-Proxy feature in 2026.2.0.

For a deeper threat model write-up, see
`MEDIA_BROWSER_PLAYBACK_AND_DOWNLOADS.md` "Architecture" section.
