# Media Browser — Operator's Guide

End-user guide for the Magic Dingus Box Media Browser feature. Written for the
kiosk operator (the person who maintains the device), not the casual viewer.

---

## What this is

The Media Browser is a hidden feature of the Magic Dingus Box kiosk that lets
you search for, add, and manage a personal movie collection directly from the
box's physical controls — no laptop, no web UI, no mouse. When enabled, it
surfaces a "Movies" entry in the main kiosk playlist browser; selecting it
opens a full browse / search / download / library experience powered by a
Radarr + Prowlarr + qBittorrent Docker stack running on the Pi.

It is hidden by default. Fresh-from-the-factory units show no trace of it —
the main UI looks like any other kiosk-mode video playback box. A specific
physical-button sequence unlocks the feature. Hiding it again is one toggle
away. This is deliberate: the feature is powerful and should only be accessible
to an operator who knows it exists.

---

## The secret sequence

To unlock the Media Browser, perform this sequence from the main playlist
screen using the kiosk's physical controls:

1. **Press and hold BTN1 + BTN3 together** (the two outer buttons) for about
   half a second
2. **Tap BTN2 three times** in under two seconds
3. **Click the rotary encoder** (push it in) once

If successful, a toast notification "Media Browser unlocked" appears on
screen for about 2 seconds, and a new "Movies" entry appears in the main
playlist browser. The sequence has a short timeout between steps — if you
pause too long the state resets and you start over.

If it doesn't work the first time: make sure you're on the main playlist
screen (not in a settings menu or mid-video), and that the rotary click is a
press-in, not a rotate.

---

## Navigation

The Media Browser is designed to be operated entirely from the Dingus Box's
four physical buttons and rotary encoder.

| Input | Action |
|---|---|
| **Rotary rotate** | Navigate horizontally / traverse lists |
| **Rotary click** | Select / confirm |
| **BTN1** | Up (vertical navigation) |
| **BTN3** | Down (vertical navigation) |
| **BTN4** | Back / cancel (pops to previous screen) |
| **BTN2** | Reserved — currently no-op inside Movies screens |

If you have a gamepad plugged in (for retro gaming), it also works in the
Media Browser: **D-pad** replaces buttons/rotary for navigation, **A** is
select, **B** is back. Controller-free operation is the supported primary path
though — every screen is reachable without a gamepad attached.

---

## Screens

The Media Browser has six screens. Moving between them uses the chip bar on
the Browse screen or BTN4 to pop back.

**Browse** — The landing screen. Chip bar across the top offers four content
feeds (Popular, New, Trending, Recommended) and four navigation destinations
(Search, Library, Queue, Settings). A poster grid below fills with content
from whichever content chip is selected.

**Search** — Virtual on-screen QWERTY keyboard on the left, live search
results on the right. Typing triggers a debounced query to Radarr; results
appear as they arrive. CANCEL on the keyboard routes focus to the results
pane without closing the screen.

**Detail** — Opened from any movie poster. Shows poster, title, year, runtime,
plot, and action buttons. For a movie not yet in your library: an **Add to
Library** button. For one already in your library: **Play**, **Remove**, and
**Search Again** (re-run the release search if a better copy might be
available).

**Queue** — Active downloads. Each item shows a progress bar with live rate
(KB/s or MB/s), peer count, and ETA. Select an item and click twice in quick
succession to cancel it.

**Library** — Your local collection. Filter chips at the top: **All**,
**Unwatched**, **Missing** (monitored but not yet downloaded), **Recent**
(added in the last 30 days). Posters show state badges. Selecting opens
Detail with the Play / Remove / Search Again action set.

**Movies Settings** — Operator controls specific to the Media Browser: three
service status dots (Radarr / Prowlarr / qBittorrent — green up, red down),
a quality profile selector, and a "Hide Movies" checkbox that re-locks the
feature.

---

## Adding a movie

The happy-path workflow, start to finish:

1. Unlock Movies (if not already unlocked), open it from the main playlist
2. From Browse or Search, find the movie you want
3. Click its poster to open Detail
4. Click **Add to Library**
5. Radarr immediately searches your enabled indexers (via Prowlarr) for a
   release matching the configured quality profile (default HD-1080p)
6. When a release is found, it's sent to qBittorrent (routed through the
   Gluetun VPN tunnel), which begins downloading
7. Monitor progress from the Queue screen — rate, peers, ETA update live
8. When the download finishes, Radarr imports the file into
   `/mnt/ssd/library/Movies/<Title> (<Year>)/`
9. The movie now appears on the Library screen with a file, and also in the
   main kiosk playlist browser under the auto-synthesized **Movies**
   playlist — so it can be played from both places

---

## Re-locking the feature

There are two ways to hide the Movies entry again (both set
`media_browser.unlocked = false` in `config/settings.json`):

1. **Main Settings → System → Hide Media Browser** — the global kill switch,
   reachable from the standard kiosk settings menu
2. **Movies → Settings → Hide Movies checkbox** — in-feature toggle on the
   Movies Settings screen

Either path has the same effect: the Movies entry disappears from the main
playlist browser, the unlock sequence is required again to restore access.
Your library, queue, and settings are preserved across lock/unlock cycles —
only visibility changes.

---

## Service admin via SSH tunnel

The Media Browser deliberately binds Radarr, Prowlarr, and qBittorrent to
`127.0.0.1` on the Pi. That means their full admin web UIs are NOT reachable
from any other machine on the network. To open them, tunnel the ports through
SSH from your laptop:

```bash
ssh -L 7878:localhost:7878 -L 9696:localhost:9696 -L 8080:localhost:8080 magic@magicpi.local
```

With that shell open, on your laptop browse to:

- **http://localhost:7878** — Radarr (root folder setup, quality profiles,
  custom formats, per-movie monitoring rules)
- **http://localhost:9696** — Prowlarr (add/remove indexers, test indexer
  connectivity, view search stats)
- **http://localhost:8080** — qBittorrent (per-torrent controls, speed
  limits, categories, connection status, VPN health)

This split is intentional: the in-kiosk Settings screen exposes the 11
operator controls that belong on a TV remote. Everything else — indexer
management, per-movie quality overrides, torrent-level tweaking — stays in
the full web admin, which requires the SSH tunnel. No drive-by access from
the LAN.

---

## Legal content

The Media Browser is content-agnostic: Radarr searches whatever indexers you
configure in Prowlarr, and downloads whatever those indexers return. Legitimate
uses include:

- **Internet Archive** feeds (public domain films, Open Source Movies collection)
- **Creative Commons** feature releases
- Indexers for media you already own and are backing up

Note: the kiosk routes qBittorrent through ProtonVPN's **free tier** by
default, which blocks P2P traffic at the protocol level. So out-of-the-box,
torrenting won't actually complete — this is a safety floor, not a bug. If
you want P2P downloads to work, you have to either upgrade ProtonVPN to a
paid plan that permits P2P, switch to a different VPN provider in Gluetun,
or use HTTP-based indexers that don't require P2P.

Either way: the operator is responsible for the legality of what they
download. The Magic Dingus Box provides the plumbing; content selection and
rights compliance are on you.

---

## Troubleshooting

**"Movies menu isn't there."** You haven't unlocked it, or you re-locked it
and forgot. Run the secret sequence again (BTN1+BTN3 chord → BTN2×3 →
rotary click). If you see no toast, the sequence isn't being detected —
try pressing firmly, making sure the BTN1+BTN3 chord is simultaneous.

**"Services offline" (red dots on Settings screen).** The Docker stack is
slow to warm up. On a fresh boot, wait 60 seconds before checking — health
checks need time to hit their targets. If still red after 2 minutes, SSH in
and run `cd /opt/magic_dingus_box/magic_dingus_box_cpp/services && docker
compose ps` to see which container is failing, then `docker compose logs
<service>` for the error.

**"Search returns 0 results."** Most commonly: Docker's internal DNS didn't
come back cleanly after a Pi reboot. Fix with `sudo systemctl restart
docker` on the Pi (the Media Browser containers will auto-restart as part
of the cycle). If results are still empty, the issue is indexer-side — SSH
tunnel to Prowlarr (port 9696) and check that at least one indexer shows a
green test-status dot.

**"Can't find a particular movie."** Prowlarr ships with a conservative
default indexer set. To add more: tunnel in, open Prowlarr, go to **Indexers
→ Add Indexer**, pick from the catalog. FlareSolverr is already wired up for
indexers that need CloudFlare bypass.

**"Download starts then stalls at 0 KB/s forever."** Almost always the VPN
P2P block (see the Legal content section above). Check qBittorrent's
connection status via the SSH tunnel — if it reports "Firewalled" with 0
peers, the VPN is blocking P2P. Upgrade ProtonVPN or switch provider in
Gluetun's config.
