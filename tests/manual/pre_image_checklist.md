# Pre-Image Validation Checklist

Run through this checklist before generating a golden image. Print or open on a second screen; tick each box as you verify.

**Tester:** _________________
**Date:** _________________
**Pi serial / hostname:** _________________
**VERSION:** _________________ (from `cat /opt/magic_dingus_box/VERSION`)
**Git commit:** _________________ (from `cd /opt/magic_dingus_box && git rev-parse --short HEAD` if available)

## Phase 0 — Automated suite green

- [ ] `./tests/run_all.sh` exit code 0 (no failures)
- [ ] Any skipped tests are intentional (no controller absent etc.)
- [ ] Any warnings reviewed and accepted

## Phase 1 — Hardware sanity (faceplate stack)

- [ ] Boot LED sequence plays on power-on
- [ ] Power button triggers clean shutdown
- [ ] Cold boot reaches kiosk UI in under 30 seconds
- [ ] Rotary encoder scrubs video (clockwise = forward, counter-clockwise = backward)
- [ ] Rotary encoder is responsive (no missed detents)
- [ ] **Rotary scrub — single slow click forward actually advances forward**, not backward (this was a real bug caused by `GST_SEEK_FLAG_KEY_UNIT`; fixed by switching to `GST_SEEK_FLAG_ACCURATE`)
- [ ] Rotary scrub backward (slow) actually goes backward the same amount
- [ ] Rotary scrub at speed (fast turn) advances further (velocity-sensitive scaling still works)
- [ ] All face buttons fire kiosk actions (test at least 4)

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
- [ ] ANALOG mode is enabled (press the small button between sticks)
- [ ] D-pad navigates playlists (up/down/left/right)
- [ ] Cross button opens settings menu
- [ ] Square button selects items
- [ ] Right analog stick does NOT trigger menu/select (analog mode confirmed)
- [ ] Select+Start opens RetroArch menu in-game

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

- [ ] **NES** (nestopia) — Super Mario Bros 3 or similar
- [ ] **SNES** (snes9x2010) — Super Mario World or similar
- [ ] **Sega Genesis** (genesis_plus_gx) — Sonic the Hedgehog or similar
- [ ] **PlayStation 1** (pcsx_rearmed) — Crash Bandicoot or similar
- [ ] **PC Engine** (mednafen_pce_fast) — any
- [ ] **Atari 7800** (prosystem) — any
- [ ] **Arcade** (fbneo) — Street Fighter II or similar

### Save / auto-resume (pick ONE game to deep-test)
- [ ] Load a game (e.g. SMB3), play past the first screen, exit to kiosk via Z+Start / Select+Start
- [ ] Re-launch the SAME game — auto-resumes where you left off (save state auto-load is on by default per CLAUDE.md)
- [ ] `ssh PI 'ls data/saves/<CoreName>/'` shows the SRAM file (e.g. `Nestopia/SMB3.srm`)
- [ ] `ssh PI 'ls data/states/<CoreName>/'` shows the state file

## Phase 4 — Bezel cycling

In Modern TV mode:
- [ ] MDB-1974 bezel renders correctly in kiosk
- [ ] MDB-1986 bezel renders correctly in kiosk
- [ ] MDB-KV19 bezel renders correctly in kiosk
- [ ] At least ONE old retro TV bezel still works (e.g., Vintage TV)
- [ ] Switching bezel mid-session: change in Settings, return to kiosk, change appears
- [ ] **Loading screen** during the kiosk → RetroArch handoff renders INSIDE the bezel cutout (bezel stays on top of "Loading..." text — continuous visual framing during transition)
- [ ] Bezel stays during in-game play (game renders in 4:3 viewport inside the bezel's screen cutout, not as a letterboxed fullscreen)
- [ ] Bezel does NOT obscure RetroArch's in-game menu (Z+Start / Select+Start — bezel auto-hides when menu opens, reappears on close)
- [ ] After exiting a game, bezel is back in kiosk (no disappearing-until-next-switch regression)

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

### Kiosk-side QR code
- [ ] On TV, open Settings → Info. QR code is rendered
- [ ] Label under QR reads "**USB Connection (Preferred)**" when USB-C is plugged in
- [ ] Scan the QR with a phone camera — URL decodes as `http://10.55.0.1:5000` (not `http://192.168.7.1:5000`)
- [ ] From a laptop connected via USB-C: scanned URL opens the content manager successfully

### WiFi (alternate path)
- [ ] WiFi reconnects automatically on cold boot
- [ ] When USB-C is unplugged and only WiFi is up: QR label shows "Wi-Fi Connection" and encodes the Pi's LAN IP
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

Verify the drag-handle + drop indicator work on **both** playlist editors:

### Video playlist
- [ ] Leftmost ⋮⋮ handle visible on every row (light gray, grabbable)
- [ ] Drag a row from position 5 → drop between positions 1 and 2 → item lands at position 2 (insert, not swap)
- [ ] While dragging, a horizontal line shows exactly where the item will land
- [ ] Title input: Cmd+A selects all text in field (not whole page); drag-select inside input works; paste works
- [ ] Artist input: same Cmd+A / drag-select / paste verification
- [ ] ▲ / ▼ arrow buttons still move rows by one position
- [ ] ✕ delete button still works

### Game/ROM playlist
- [ ] Same drag handle + drop indicator behavior
- [ ] Same title + artist input editing fidelity
- [ ] Same ▲ / ▼ and ✕ buttons work

## Sign-off

All Phases 0-7 complete and green: ____________________ (your initials)

Image creation cleared. Run `prepare_golden_image.sh` next.
