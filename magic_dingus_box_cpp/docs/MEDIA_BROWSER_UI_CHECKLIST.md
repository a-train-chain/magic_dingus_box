# Media Browser V2 — UI Test Checklist

Manual end-to-end verification walk-through. Operator runs this on the Pi after
deploying the flag-ON build. Each row is a discrete test step with an expected
result; tick the box only after observing the expected behavior.

---

## 0. Prerequisites

- [ ] Pi is booted and on HDMI — kiosk app is up and showing the main playlist browser — expected: normal video playback UI, no crash
- [ ] `systemctl status magic-dingus-box-cpp.service` is `active (running)` — expected: green dot, recent logs
- [ ] `cd /opt/magic_dingus_box/magic_dingus_box_cpp/services && docker compose ps` — expected: `radarr`, `prowlarr`, `qbittorrent`, `gluetun`, `flaresolverr` all `Up (healthy)`
- [ ] `curl -s http://127.0.0.1:7878/api/v3/system/status -H "X-Api-Key: $(grep RADARR_API_KEY services/.env | cut -d= -f2)"` returns JSON — expected: `{"version":"5.x.x",...}`
- [ ] `media_browser.unlocked` is currently `false` in `config/settings.json` — expected: feature hidden (no Movies entry in main playlist browser)

---

## 1. Secret Unlock Sequence

Performed on the kiosk's physical hardware controls (the 4 buttons + rotary encoder).

- [ ] From the main playlist screen, press & hold **BTN1 + BTN3** together for ~500ms — expected: no visible change (step 1 of 3 consumed silently)
- [ ] Tap **BTN2 three times** in under 2 seconds — expected: no visible change (step 2 of 3 consumed silently)
- [ ] Click the **rotary encoder** once — expected: on-screen toast "Media Browser unlocked" appears for ~2s
- [ ] Return to main playlist browser — expected: "Movies" category now visible in the top-level playlist list
- [ ] Open Movies entry — expected: Media Browser launches on Browse screen

If the sequence fails at any step it silently resets; start over from BTN1+BTN3.

---

## 2. Browse Screen

The landing screen. Top row is a chip bar, below is a poster grid.

- [ ] 8 chips visible across top: **Popular / New / Trending / Recommended / Search / Library / Queue / Settings** — expected: first chip highlighted
- [ ] Rotate encoder clockwise — expected: highlight advances through all 8 chips, grid content updates for each content chip (Popular/New/Trending/Recommended)
- [ ] Rotate counter-clockwise — expected: highlight walks back through chips
- [ ] Select "Search" chip (click rotary) — expected: Search screen opens
- [ ] Return to Browse (BTN4) — expected: back on Browse with same chip highlighted
- [ ] Select "Library" chip — expected: Library screen opens
- [ ] Return to Browse — expected: back on Browse
- [ ] Select "Queue" chip — expected: Queue screen opens
- [ ] Return to Browse — expected: back on Browse
- [ ] Select "Settings" chip — expected: Movies Settings screen opens
- [ ] Return to Browse — expected: back on Browse
- [ ] Move highlight onto a content chip (e.g., Popular) — expected: poster grid populated below
- [ ] Press **BTN3** (down) — expected: focus drops into the poster grid, first poster highlighted
- [ ] Rotate encoder to traverse posters — expected: highlight moves horizontally, wraps on row boundaries
- [ ] Press **BTN3** again — expected: highlight moves to the row below
- [ ] Press **BTN1** (up) from the top poster row — expected: focus returns to the chip bar
- [ ] Click rotary on a poster — expected: Detail screen opens for that movie

---

## 3. Search Screen

Virtual keyboard on the left, results list on the right.

- [ ] Virtual QWERTY keyboard has focus on open — expected: "Q" key highlighted (or last-used key)
- [ ] Type a known movie name one character at a time (e.g., "THE MATRIX") — expected: query string builds in the query bar at top
- [ ] After ~500ms of idle typing — expected: live results appear on the right pane (debounced fetch to Radarr)
- [ ] Press **CANCEL** key on virtual keyboard — expected: focus shifts to the results list (NOT stranded — user can still operate)
- [ ] Press **BTN1** to return focus to keyboard — expected: keyboard active again
- [ ] With results populated, navigate to a result row using rotary — expected: row highlight moves through results
- [ ] Click rotary on a result — expected: Detail screen opens for that movie
- [ ] Back to Search (BTN4) — expected: returns with query + results preserved
- [ ] Clear query (BACKSPACE all) — expected: results pane clears cleanly, no stale rows
- [ ] Type a gibberish query that returns 0 hits — expected: "No results" message (not a hang or crash)

---

## 4. Detail Screen

Opened from Browse, Search, Queue, or Library. Shows poster, plot, metadata, action buttons.

### 4a. Movie NOT yet in library

- [ ] Poster + title + year + runtime + plot render — expected: all fields populated (plot may be truncated with ellipsis)
- [ ] Action row shows **Add to Library** button — expected: button highlighted by default
- [ ] Click **Add to Library** — expected: toast "Added <title>"; button updates to indicate in-library state
- [ ] Open Queue (BTN4 to origin → Queue chip) — expected: the movie now shows in the active downloads list (assuming Radarr found a release)

### 4b. Movie already IN library

- [ ] Open a library movie from the Library screen — expected: Detail opens with 3 action buttons: **Play / Remove / Search Again**
- [ ] Rotate encoder to cycle action buttons — expected: highlight moves Play → Remove → Search Again → Play
- [ ] Click **Search Again** — expected: toast "Searching for new release"; returns to Detail
- [ ] Click **Remove** — expected: 2-click confirmation (first click arms, second click confirms); toast "Removed"; returns to origin screen
- [ ] Re-add the movie, then click **Play** — expected: kiosk hands off to GStreamer playback of the file in `/mnt/ssd/library/Movies/`

---

## 5. Queue Screen

Active downloads from qBittorrent via Radarr.

- [ ] If there are no active downloads — expected: "Queue is empty" message, no crash
- [ ] With at least one movie queued — expected: card per item showing title, progress bar, rate (KB/s or MB/s), peers count, ETA
- [ ] Rate, peers, and ETA update every few seconds — expected: values tick (not frozen) as the download progresses
- [ ] Navigate to a queue item — expected: item highlighted
- [ ] Click once on selected item — expected: "Press again to cancel" inline prompt arms
- [ ] Click a second time within 3s — expected: item cancels, toast "Cancelled", removed from list
- [ ] Click once then wait >3s — expected: arming state clears; next click is a fresh arm (not an accidental cancel)

---

## 6. Library Screen

Local collection — movies whose files exist on disk.

- [ ] Top row shows filter chips: **All / Unwatched / Missing / Recent** — expected: All chip highlighted by default
- [ ] Each chip cycled — expected: grid repopulates with the correct subset
- [ ] **Unwatched** chip — expected: only movies not yet played (heuristic: never seeked past 80%)
- [ ] **Missing** chip — expected: movies monitored in Radarr but file not yet on disk (download stuck/failed)
- [ ] **Recent** chip — expected: movies added within last 30 days
- [ ] Navigate cursor through the grid — expected: highlight moves across posters
- [ ] State indicator overlay on each poster — expected: watched/unwatched/missing badge visible
- [ ] Click rotary on a library movie — expected: Detail screen opens with Play / Remove / Search Again actions

---

## 7. Movies Settings Screen

In-app operator controls. NOT a substitute for full Radarr admin (that requires SSH tunnel).

- [ ] Service status dots row: **Radarr / Prowlarr / qBittorrent** — expected: all three green if stack is up
- [ ] Stop one service on the Pi via `docker compose stop radarr` — expected: Radarr dot turns red within ~15s (next poll)
- [ ] Restart it — expected: dot returns to green within ~15s
- [ ] Quality profile field — expected: current profile shown (default "HD-1080p")
- [ ] Cycle quality profile (click to open selector, rotate to pick, click to commit) — expected: profile updates, persists to Radarr settings
- [ ] **Hide Movies** checkbox — expected: toggling ON sets `media_browser.unlocked=false` and kicks user back to main playlist; Movies entry gone from playlist list

---

## 8. Back Navigation (BTN4)

Navigation stack discipline — BTN4 must return to the ORIGIN, not always Browse.

- [ ] Browse → click poster → Detail; press BTN4 — expected: returns to Browse
- [ ] Browse → Search chip → Search → select result → Detail; press BTN4 — expected: returns to Search (with query preserved), NOT to Browse
- [ ] Browse → Library chip → Library → select poster → Detail; press BTN4 — expected: returns to Library (with filter preserved)
- [ ] Browse → Queue chip → Queue → select item → Detail; press BTN4 — expected: returns to Queue
- [ ] From Browse, press BTN4 — expected: exits Media Browser cleanly, returns to main playlist browser
- [ ] From Detail, BTN4 twice in rapid succession — expected: Detail → origin → main playlist (no double-pop skip)

---

## 9. Controller-Free Navigation (Hardware-Only)

Magic Dingus Box ships without a gamepad attached. All Media Browser navigation
must work using only the 4 physical buttons + rotary encoder.

- [ ] **BTN1** = Up (vertical up in grids / focus up through chips) — expected: highlight moves up
- [ ] **BTN3** = Down (vertical down) — expected: highlight moves down
- [ ] **Rotary rotate** = horizontal navigation / list traversal — expected: highlight advances along current axis
- [ ] **Rotary click** = select / confirm — expected: activates highlighted item
- [ ] **BTN4** = back / cancel — expected: pops navigation stack
- [ ] **BTN2** — expected: no-op inside Movies screens (reserved for future use, does nothing)
- [ ] Pass the entire checklist above using ONLY the 4 buttons + rotary — expected: every action reachable, no dead ends

### 9a. Controller parity (if a gamepad is plugged in)

- [ ] D-pad up/down — expected: same as BTN1/BTN3
- [ ] D-pad left/right — expected: same as rotary rotate
- [ ] A button — expected: same as rotary click
- [ ] B button — expected: same as BTN4

---

## 10. Sign-off

- [ ] All above boxes ticked by: ________________ on ________________
- [ ] Observed any failures? Log them in the session notes with reproduction steps.
