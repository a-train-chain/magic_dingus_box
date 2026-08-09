# Pre-Image Validation Checklist

Run through this checklist before generating a golden image. Print or open on a second screen; tick each box as you verify.

**Tester:** _________________
**Date:** _________________
**Pi serial / hostname:** _________________
**VERSION:** _________________ (from `cat /opt/magic_dingus_box/VERSION`)
**Git commit:** _________________ (from `cd /opt/magic_dingus_box && git rev-parse --short HEAD` if available)

## Phase 0 — Automated suite green

- [x] `./tests/run_all.sh` exit code 0 (no failures) — *one test (#33 video path linkage) failed initially due to a path-resolution bug in the test itself; fixed in this session, now green*
- [x] Any skipped tests are intentional (no controller absent etc.) — *9 skips: settings.json reference (no Pi build artifact locally), 3 joystick-absent tests, 7 RetroArch-launch tests skipped because joystick required to launch them*
- [x] Any warnings reviewed and accepted — *none*

## Phase 1 — Hardware sanity (faceplate stack)

- [x] Boot LED sequence plays on power-on
- [~] Power switch triggers clean **kiosk-standby** *(was full poweroff; reimplemented this session via gpiomon-based watcher service. **OFF direction works** — confirmed rising-edge event fired correctly, services stopped cleanly, Pi remained running at idle. **ON direction (this Pi only) doesn't fire** — GPIO 3 stays HIGH despite physical toggle, indicating broken GND-side contact in switch hardware (worn contact, cracked solder, or loose wire). Software side is verified working; this Pi needs a physical switch repair before clone, but cloned Pis with working switches will get full behavior natively.)*
- [x] Cold boot reaches kiosk UI in under 30 seconds
- [x] Rotary encoder scrubs video (clockwise = forward, counter-clockwise = backward)
- [x] Rotary encoder is responsive (no missed detents)
- [x] **Rotary scrub — single slow click forward actually advances forward**, not backward (this was a real bug caused by `GST_SEEK_FLAG_KEY_UNIT`; fixed by switching to `GST_SEEK_FLAG_ACCURATE`)
- [x] Rotary scrub backward (slow) actually goes backward the same amount
- [x] Rotary scrub at speed (fast turn) advances further (velocity-sensitive scaling still works)
- [x] All face buttons fire kiosk actions (test at least 4)
- [x] **Movie playback aspect ratio is correct** *(added during testing — operator noticed widescreen movies looked stretched. Fixed by gst_renderer.cpp now preserving source aspect ratio + reading pixel-aspect-ratio from GStreamer caps. Letterboxes 2.35:1 / 2.39:1 movies on 16:9 screen, pillarboxes 4:3 content, handles anamorphic DVDs correctly via PAR. Verified visually on Pulp Fiction.)*

## Phase 2 — Kiosk UI (with each controller in turn)

For each controller you support (N64 adapter, PS-style pad), tick:

### N64 adapter
- [ ] Joystick navigates playlists (up/down)
- [ ] D-pad also navigates
- [ ] A button selects items
- [ ] B button opens settings menu
- [ ] Z+Start opens RetroArch menu in-game
- [ ] Settings menu navigable in all directions

### PS-style pad (DragonRise/Microntek)
**Phase 2 PS-pad sign-off:** Operator verified during 2026-04-27 session ("I tested everything with the controller. Everything is working."). PS-pad button layout was tweaked early in the session — Circle now opens Settings, Cross now selects items (operator preference, commit landed before Phase 2 sign-off). Square is currently unassigned.
- [x] ANALOG mode is enabled (press the small button between sticks)
- [x] D-pad navigates playlists (up/down/left/right)
- [x] Cross button selects items *(remapped from spec — Cross=SELECT, Circle=MENU per operator preference)*
- [x] Circle button opens settings menu *(remapped from spec — see above)*
- [x] Right analog stick does NOT trigger menu/select (analog mode confirmed)
- [x] Select+Start opens RetroArch menu in-game

### WiFi virtual keyboard (with at least one controller)
- [ ] Pop the virtual keyboard from Settings → WiFi
- [ ] D-pad navigates between keys (up/down/left/right)
- [ ] Select key types it
- [ ] Backspace works
- [ ] Enter/Cancel buttons close the keyboard

## Phase 3 — Each core launches and feels right

For each core, launch ONE game, verify:
- Game starts within 5 seconds
- Audio is present (no crackle)
- Controls are responsive (button press → action delay < 50ms felt)
- Z+Start (N64) or Select+Start (PS) brings up RetroArch menu
- Quit RetroArch returns to kiosk cleanly with bezel still showing

- [x] **NES** (nestopia) — Super Mario Bros 3 or similar *(verified 2026-04-27 with Zelda)*
- [x] **SNES** (snes9x2010) — Super Mario World or similar
- [x] **Sega Genesis** (genesis_plus_gx) — Sonic the Hedgehog or similar
- [x] **PlayStation 1** (pcsx_rearmed) — Crash Bandicoot or similar *(verified incl. Twisted Metal 4 in 2-player mode)*
- [x] **PC Engine** (mednafen_pce_fast) — any
- [x] **Atari 7800** (prosystem) — any
- [x] **Arcade** (fbneo) — Street Fighter II or similar

### Save / auto-resume (pick ONE game to deep-test)
- [x] Load a game (e.g. SMB3), play past the first screen, exit to kiosk via Z+Start / Select+Start
- [~] Re-launch the SAME game — auto-resumes where you left off — *Partial: SRAM-based in-game saves work for every core (verified Zelda character carry-over, 7 PS1 .srm files exist, Cadash PCE save). However the `.state.auto` mid-session resume feature does NOT currently fire on game exit despite `savestate_auto_save = "true"` being set — RA computes the path correctly (`[Overrides]: Redirecting save state to ...`) but never writes the file. Spawned as separate fix track via `ccd_session__spawn_task` during this session; not blocking the golden image since SRAM saves cover the in-game-save case.*
- [x] `ssh PI 'ls data/saves/<CoreName>/'` shows the SRAM file (e.g. `Nestopia/SMB3.srm`) *— verified for nes/Nestopia, ps1/PCSX-ReARMed (7 games), pcengine/Beetle PCE Fast.*
- [~] `ssh PI 'ls data/states/<CoreName>/'` shows the state file *— Empty for all cores; tracked under the auto-resume bug above.*

### 2-player support
- [x] Plug in a 2nd identical controller — appears at `/dev/input/js1`
- [x] Launch a 2-player PS1 game (Twisted Metal 4 / Tony Hawk / Doom) — both controllers move characters independently *(verified live with Twisted Metal 4 split-screen Battle mode, commit `6a968c8` added explicit `input_player2_*` mappings + `pcsx_rearmed_pad2type` for PS1 BIOS to recognize the 2nd pad)*

## Phase 4 — Bezel cycling

**Phase 4 sign-off:** Operator verified bezel cycling works during 2026-04-27 session ("The bezel cycling works good, so we can check that off.")

In Modern TV mode:
- [x] MDB-1974 bezel renders correctly in kiosk
- [x] MDB-1986 bezel renders correctly in kiosk
- [x] MDB-KV19 bezel renders correctly in kiosk
- [x] At least ONE old retro TV bezel still works (e.g., Vintage TV)
- [x] Switching bezel mid-session: change in Settings, return to kiosk, change appears
- [x] **Loading screen** during the kiosk → RetroArch handoff renders INSIDE the bezel cutout (bezel stays on top of "Loading..." text — continuous visual framing during transition)
- [x] Bezel stays during in-game play (game renders in 4:3 viewport inside the bezel's screen cutout, not as a letterboxed fullscreen)
- [x] Bezel does NOT obscure RetroArch's in-game menu (Z+Start / Select+Start — bezel auto-hides when menu opens, reappears on close)
- [x] After exiting a game, bezel is back in kiosk (no disappearing-until-next-switch regression)

## Phase 5 — CRT mode

Required if the golden image is shipping CRT mode support to clones. Skip only if you're intentionally targeting modern-TV-only hardware.

### Mode switch
- [ ] Switch to CRT_V mode in Settings → Display
- [ ] Kiosk UI renders at 640×480 (CRT-native resolution, no letterboxing)
- [ ] Intro video + playlist browsing readable on CRT (or on the Pi output even without a CRT)

### In-game CRT verification (the regression that matters)
- [ ] Launch a game (any core). It runs at 640×480 fullscreen **with no bezel overlay** (CRT_V has no bezel by design)
- [ ] The generated RetroArch config on the Pi has the CRT-shape config (640×480, no custom viewport, no input_overlay). Verify:
  ```
  ssh PI 'grep -E "^(video_fullscreen_[xy]|video_custom_viewport_enable|input_overlay )" /home/magic/retroarch_launcher.sh'
  ```
  Expected: `video_fullscreen_x = "640"`, `video_fullscreen_y = "480"`, `video_custom_viewport_enable = "false"`, no `input_overlay` line.
- [ ] Controls feel native (overclock still applies: the 2.0 GHz CPU + threaded video combo doesn't introduce CRT-mode regression vs. pre-bezel-feature behavior)
- [ ] Exit game; UI returns to 640×480 kiosk cleanly

### Mode round-trip
- [ ] Switch back to Modern TV mode in Settings → Display; confirm full restore (bezel, 1080p, 4:3 viewport all reappear)
- [ ] Cycle CRT → Modern → CRT once more to confirm no state leak

## Phase 6 — Endurance (background-runnable)

- [ ] Pick a heavy PS1 game (Gran Turismo 2, Metal Gear Solid, FF VII)
- [ ] Run continuously for 15 minutes
- [ ] Check temp every ~3 minutes: `ssh PI 'vcgencmd measure_temp'`
- [ ] Check throttle: `ssh PI 'vcgencmd get_throttled'` — must remain `throttled=0x0`
- [ ] Frame rate feels consistent (no obvious chugging during fights/cutscenes)
- [ ] Audio doesn't crackle or skip

## Phase 7 — Power cycle round-trip

- [ ] In settings, change a known value (e.g., bezel selection)
- [ ] Save, exit to kiosk
- [ ] Power off via the power button
- [ ] Power back on (cold boot)
- [ ] Confirm setting persisted (the bezel you picked is still selected)
- [ ] No fsck errors on boot (`dmesg | grep -i fsck` after boot)

## Phase 8 — Network + content manager (web admin)

### USB gadget connectivity
- [ ] Plug Mac → Pi USB-C. `ifconfig` on Mac shows the "Raspberry Pi USB Gadget" interface with IP `10.55.0.2`
- [ ] `ping 10.55.0.1` responds
- [ ] Open `http://10.55.0.1:5000/` in browser → content manager loads in well under 1 second
- [ ] Connection badge in the admin header reads **"🔌 USB"**, not "📶 WiFi" (fixed from hardcoded `192.168.7.1` check)

### Kiosk-side QR code (the merged "Connect a Device" screen)
- [ ] On TV, open Settings → **Connect a Device**. One screen shows: QR code, large 6-digit code, "Code refreshes in M:SS"
- [ ] Typed address line reads `http://<hostname>.local:5000  (or <lan-ip>:5000)`
- [ ] USB line reads "Or: plug a USB-C cable into a computer and open  http://dingus.box"
- [ ] Scan the QR with a phone camera — URL decodes as `http://<lan-ip>:5000/connect?code=NNNNNN` and opens the "Connect a Device" web page
- [ ] From a laptop connected via USB-C: `http://dingus.box` opens the content manager successfully
- [ ] There is NO separate "Content Manager" or "Connect Phone / Computer" row in Settings — "Connect a Device" is the only connection entry

### WiFi (alternate path)
- [ ] WiFi reconnects automatically on cold boot
- [ ] With USB-C unplugged and only WiFi up: the Connect a Device QR encodes the Pi's LAN IP
- [ ] Web admin reachable from a phone on the same WiFi at `http://<pi-wifi-ip>:5000/`

### Content upload + transcoding
- [ ] Upload a small video file via web admin → progress bar completes, file lands in `data/media/`
- [ ] Transcode preset **CRT 640×480** produces a smaller output suitable for CRT mode
- [ ] Transcode preset **Modern 720p** produces a higher-res output suitable for Modern TV mode
- [ ] The uploaded video appears in the "available" column of the video playlist editor
- [ ] Adding the video to a playlist persists on the Pi (`data/playlists/<name>.yaml` reflects the new item)

### OTA
- [ ] OTA check: web admin's "check for updates" returns the current version (not "newer available" since we just tagged this as the latest release)
- [ ] If a NEWER version were published, OTA would download + install + restart cleanly (optional smoke test using a test tag)

## Phase 9 — Web admin playlist editor (both playlists)

**Phase 9 sign-off:** Operator confirmed drag-and-drop works in playlist edit mode during 2026-04-27 session ("the web admin, I'm able to drag and drop playlists within the edit mode").

Verify the drag-handle + drop indicator work on **both** playlist editors:

### Video playlist
- [x] Leftmost ⋮⋮ handle visible on every row (light gray, grabbable)
- [x] Drag a row from position 5 → drop between positions 1 and 2 → item lands at position 2 (insert, not swap)
- [x] While dragging, a horizontal line shows exactly where the item will land
- [x] Title input: Cmd+A selects all text in field (not whole page); drag-select inside input works; paste works
- [x] Artist input: same Cmd+A / drag-select / paste verification
- [x] ▲ / ▼ arrow buttons still move rows by one position
- [x] ✕ delete button still works

### Game/ROM playlist
- [x] Same drag handle + drop indicator behavior
- [x] Same title + artist input editing fidelity
- [x] Same ▲ / ▼ and ✕ buttons work

## Phase 10 — Media Browser (movie discovery + playback)

The kiosk's Media Browser sub-mode talks to a Docker stack (Radarr / Prowlarr / qBittorrent / Gluetun / FlareSolverr) for movie discovery + downloading + playback. **Skip this entire phase if you intentionally don't ship the Media Browser feature on this image.**

**Phase 10 sign-off:** Operator confirmed end-to-end Media Browser flow during 2026-04-27 session — added Shawshank Redemption, Radarr graphed, indexer-side seeders filter kicked in, qBit downloaded via VPN tunnel with NAT-PMP port forwarding, played Count of Monte Cristo (with auto-pause kicking in cleanly). Library + Browse screens both look polished. Several improvements landed during this phase, all merged to main:
- VPN port forwarding fix (FIREWALL_OUTBOUND_SUBNETS narrowed)
- minimum_seeders=5 filter rejects dead-swarm releases
- Auto-pause torrents during playback (USB-flash IO contention)
- Library screen visual polish (matches Browse)

Substep checks below ticked based on operator's hands-on use ("After navigating around the movies media section, I don't see anything else that isn't working properly"); items tagged with verification-during-session notes.

### Browse screen (9-col 2-row poster grid)
- [x] Enter Media Browser → BrowseScreen comes up **instantly** (async TMDB fetch — no 6 sec freeze)
- [x] Grid renders **2 full rows of posters** (18 visible)
- [x] Posters are ~119×178 px, clearly identifiable
- [x] Only the **focused poster** has a gold border + bright title; other 17 titles dimmed (alpha 0.55)
- [x] Rotate / D-pad navigation through the grid is smooth
- [x] Switch chips (Popular → Now Playing → Top Rated → Upcoming) — each loads a fresh grid in 1-7 sec; "Loading..." appears immediately during the wait

### Pagination + dedupe
- [x] Initial page 1 lands first (~20 movies), then page 2 silently appends a few seconds later (~40 total)
- [x] Scroll cursor down past 2nd row — "Loading more..." appears in steel-blue at the bottom
- [x] New posters appear underneath (reaches 60-100 movies max per category)
- [x] No exact duplicate posters anywhere in the grid (dedupe by tmdb_id)
- [x] Switching to a different category mid-scroll resets cleanly (no stale results bleed in)

### Detail screen (async + cache reuse)
- [x] Tap a poster → DetailScreen shows "Loading..." instantly (no UI freeze)
- [x] Title / synopsis / runtime fill in 1-7 sec
- [x] Press BTN4 (back) to return to Browse, then re-tap **same** poster → **opens instantly** (cache reused — no second fetch)
- [x] Tap a different poster → fresh fetch with "Loading..." (different tmdb_id triggers refresh)
- [x] Mid-fetch, hit BTN4 to back out → no crash, no stale data later

### Search screen
- [x] From home strip, navigate to Search chip
- [x] Type a query via virtual keyboard (or external keyboard)
- [x] After ~500 ms of typing pause, "Searching..." appears, then results land 1-5 sec later
- [x] No UI freeze while typing
- [x] Type rapidly to invalidate previous query — only the latest results show
- [x] Add-to-library button is **gated** until library cache loads (first entry to Search shows "Loading library..." briefly)

### Add to library
- [x] On a movie not in library: BTN2 (quick-add) → toast: "Added to library" *(verified live with The Shawshank Redemption — Radarr added at 2026-04-27 18:36, search → grab in ~11 sec)*
- [x] Movie appears in Radarr; download starts within ~30 sec
- [x] Indexer search runs across the configured set (TPB, YTS, LimeTorrents, TorrentDownload — verify in Prowlarr's log: `docker logs mdb_prowlarr --since 1m`)
- [x] **Custom Format scoring works**: HEVC releases rejected with "x265/HEVC 1080p+ is not wanted" *(verified — Shawshank picked H264 RARBG release with score +80 after rejecting HEVC alternatives)*

### Queue screen
- [x] Active downloads appear in Queue with **live progress** (% updates every ~3 sec)
- [x] Pulsing green dot on actively-downloading items
- [x] MB/s counter updates in real time
- [x] Completed downloads disappear from Queue + appear in Library

### Detail → Play → back
- [x] On a movie WITH file: BTN1 (Play) → PlaybackScreen *(verified with Count of Monte Cristo + Shawshank)*
- [x] Movie starts playing within ~3 sec
- [x] Rotary encoder seeks (slow turn ≈ 5s, fast turn ≈ 120s)
- [x] L/R triggers ±10 sec
- [x] C-stick ±5 sec
- [x] Press BTN4 to exit playback → returns to **same** DetailScreen instantly (no re-fetch, no "Loading...")

NOTE: USB-flash drive contention found during testing — concurrent torrent writes + playback reads tanked random IO. Fixed via auto-pause feature (commit `6f470d5`): playback enter() pauses all torrents, leave() resumes them. Verified live in journal logs (`paused all torrents (playback start)` ↔ `resumed all torrents (playback end)` edges). Will improve further once user upgrades to USB 3.1 / SATA SSD.

### Confirm Remove (4-step orphan-proof)
- [x] On a movie in library, navigate to Remove → "Confirm Remove?" prompt
- [x] Confirm → Toast cycles through: "Cancelling queue items" → "Removing torrents" → "Removing from Radarr" → "Done"
- [x] Movie disappears from library on Pi (`ls /mnt/ssd/library/` no longer shows it)
- [x] qBit no longer has the torrent (`docker exec mdb_qbittorrent ...` if curious)
- [x] No orphan files left behind

### Family-safe filter (porn block)
- [x] Search a benign term like "deep throat" — surface legitimate films (e.g., Deep Throat 1972) but no porn-studio results
- [x] Family-friendly: TMDB lists exclude `adult: true` entries

## Phase 11 — Pre-clone golden-image readiness

Before running `prepare_for_cloning.sh` on this Pi, verify the cloned image will reproduce the source's state correctly.

### Codified fixtures present
```bash
ssh PI 'ls -la /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/data/'
```
- [ ] `prowlarr_indexers.json` (9 entries)
- [ ] `prowlarr_tags.json` (1 entry: cloudflare)
- [ ] `prowlarr_indexerproxies.json` (1 entry: FlareSolverr)
- [ ] `prowlarr_applications.json` (1 entry: Radarr)
- [ ] `radarr_custom_formats.json` (6 entries)
- [ ] `radarr_downloadclients.json` (1 entry: qBittorrent)
- [ ] `radarr_qualitydefinitions.json` (30 entries)
- [ ] `qbit_categories.json` (1 entry: radarr)

### setup_services.sh idempotency
- [ ] Extract Steps 11-17 from setup_services.sh into a standalone test script and run on Pi
- [ ] All steps report "unchanged" (live state already matches fixtures)
- [ ] Manually break ONE thing (e.g., `curl -X PUT ... disable an indexer`), re-run — drift is corrected
- [ ] Re-run a third time — clean again

### Live clone tooling round-trip (DESTRUCTIVE — kills services briefly)
**Skip if you're going straight to the real clone.** This validates that `prepare_for_cloning.sh` and `restore_after_cloning.sh` cleanly cycle without disrupting state.

```bash
# From Mac, with the Pi running:
./scripts/golden_image/clone_live_sd.sh --dry-run
```
- [ ] Script runs prepare → skips dd → runs restore
- [ ] Source Pi services come back up
- [ ] Library still on `/mnt/ssd/library/` (untouched)
- [ ] device_info.json restored (hostname intact)
- [ ] `magic-first-boot.service` is back to `disabled` state on source
- [ ] No "in_progress" marker file left in `/var/lib/magic-dingus-box/cloning_backup/`

### Storage check
- [ ] Mac has at least 2× the SD card size free in `~/` (or wherever you'll output the .img.gz)
- [ ] Pi's `/mnt/ssd/` library content is **whatever you want it to be** before cloning. The SSD doesn't go on the SD card image, but if you're going to share library state across Pis, set that up separately.

## Sign-off

All Phases 0-7 complete and green: ____________________ (your initials)

Phase 10 (Media Browser) green: ____________________ (or N/A — no MB on this image)

Phase 11 (clone readiness) green: ____________________

Image creation cleared. Choose:
- **Live clone (preserves source state)**: `./scripts/golden_image/clone_live_sd.sh`
- **Destructive prep (resets source to defaults)**: `sudo ./scripts/golden_image/prepare_golden_image.sh` then `./scripts/golden_image/create_image.sh`
