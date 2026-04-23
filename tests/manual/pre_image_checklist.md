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

## Phase 4 — Bezel cycling

In Modern TV mode:
- [ ] MDB-1974 bezel renders correctly in kiosk
- [ ] MDB-1986 bezel renders correctly in kiosk
- [ ] MDB-KV19 bezel renders correctly in kiosk
- [ ] At least ONE old retro TV bezel still works (e.g., Vintage TV)
- [ ] Switching bezel mid-session: change in Settings, return to kiosk, change appears
- [ ] Bezel stays during in-game play (not just kiosk)
- [ ] Bezel does NOT obscure RetroArch's in-game menu (Z+Start / Select+Start)
- [ ] After exiting a game, bezel is back in kiosk

## Phase 5 — CRT mode (if you have a CRT TV)

- [ ] Switch to CRT_V mode in Settings
- [ ] Kiosk UI renders at 640×480
- [ ] Launch a game; it runs at 640×480 fullscreen with no bezel
- [ ] Exit game; UI returns to 640×480 cleanly
- [ ] Switch back to Modern TV mode in Settings; confirm full restore

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

## Phase 8 — Network + OTA sanity

- [ ] WiFi reconnects automatically on cold boot
- [ ] Web admin reachable at the Pi's IP
- [ ] Upload a small video file via web admin → confirms upload-handling works
- [ ] OTA check: web admin's "check for updates" returns the current version (not "newer available" since we just bumped to this version)

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
