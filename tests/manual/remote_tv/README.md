# Driving a real box's TV remotely (attended checklist runs)

The tools used for the 2026-11-15 Pi 5 hardware checklist run
(`docs/superpowers/validation/2026-11-15-pi5-checklist-run.md`). They press
buttons through the web admin's virtual gamepad, read the kiosk's state, and
capture the exact frame the kiosk draws — enough to run most on-screen
checklist rows without being at the TV. Nothing here is deployed.

**Only with the owner's go-ahead.** A run changes settings, switches
channels, launches games and restarts the kiosk. Check nobody is using the TV
(`tv.sh st`), and coordinate with any other session using the box.

| File | Runs on | What |
|---|---|---|
| `tv.sh` | Mac | `start`/`end` the gamepad, `p` press, `st` kiosk state, `shot NAME` capture → `shots/NAME.png` |
| `mdbdrive.py` | box (root) | One long-lived virtual gamepad in a transient unit, fed through a root-only FIFO |
| `launch_game.sh N` | Mac | Guide → Settings → Video Games → library N → launch; reports the core and the HDMI PCM state |
| `after_game.sh` | box | Ends RetroArch like a normal quit, waits for the kiosk, reports where it returned and the audio state |

```bash
export PI_HOST=magic@<box ip>          # never defaulted: two boxes are usually reachable
tests/manual/remote_tv/tv.sh start     # the gamepad (transient unit mdb-drive, RuntimeMaxSec=6h)
tests/manual/remote_tv/tv.sh st        # screen, overlay, channel, now_playing, settings row
tests/manual/remote_tv/tv.sh p tap OK
tests/manual/remote_tv/tv.sh p tap DOWN 3 0.7     # count, gap in seconds
tests/manual/remote_tv/tv.sh shot guide-open      # → tests/manual/remote_tv/shots/guide-open.png
tests/manual/remote_tv/tv.sh end       # stop and remove the gamepad
```

`after_game.sh` is copied to the box first (`scp … $PI_HOST:/tmp/`) and run
there as the kiosk user.

## What the kiosk does (learned the hard way)

- **One gamepad for the whole run.** A fresh virtual device per press loses
  its first press while the kiosk opens the new evdev node; `mdbdrive.py`
  keeps one open.
- **Space presses ≥ 0.7 s** in Settings and the game library; 0.4 s apart,
  some are dropped. `kiosk_status.json` can lag a press: confirm with the
  saved `config/settings.json` or a capture.
- **Channel Guide style:** BLACK opens Settings only while the guide is open
  (OK first, then BLACK). OK on the tuned row closes the guide; on another row
  it tunes. Above the top row sits the PICTURE / CROP control (16:9 and other
  non-4:3 videos only).
- **Game library:** opens with the cursor on Back. UP ~11 clamps to the first
  library (Arcade); then DOWN N (alphabetical: Arcade, Atari 7800, Dreamcast,
  Genesis, NES, N64, PC Engine, PlayStation, SNES). It reports no
  `highlighted_label`: confirm with a capture or `pgrep -a retroarch`.
- **Ending a game:** `sudo pkill -TERM -x retroarch` (what `after_game.sh`
  does). The virtual pad is not player 1, so the quit chord never reaches
  RetroArch.
- **Media Browser:** RED opens an "Exit?" dialog (RIGHT, OK to leave).
- **For Master Shuffle**, `kiosk_status` names the *source* playlist; the
  kiosk journal's `Channel N: joining …` lines are the truth for tuning.
- **Sound** is checked from ALSA state, not by ear: during a game RetroArch
  owns `/proc/asound/cardN/pcm0p/sub0` and it is RUNNING; after it,
  PulseAudio owns it and `pactl list short sinks` shows RUNNING.

## Captures

- `screenshot_request` captures the full composite of the next drawn frame
  (1920x1080 in Modern TV; CRT Native is the 640x480 canvas stretched).
- `tv.sh shot` deletes each capture from the box after fetching it: the kiosk
  keeps only the newest 10, and the owner's own captures must survive. A
  capture requested while RetroArch owns the screen is written when the kiosk
  comes back — delete it too.
- Cropping on the Mac: `sips -c H W --cropOffset Y X` (Y first).
- Write shell helpers for bash, not zsh (`$3:r` is a zsh modifier, and zsh
  does not word-split `$VAR`).

## Before and after a run

1. Snapshot (root-only, e.g. `/var/tmp/mdb-checklist-snapshot`):
   `/opt/magic_dingus_box/config`, `data/playlists`, `data/media_info.json`,
   the time zone (`timedatectl show -p Timezone --value`) and a hash of
   `services/.env` (never its contents).
2. Upload test media and playlists through the Content Manager API (as an
   owner would), and delete them the same way afterwards so their
   `media_info.json` entries go too.
3. Restore `settings.json` from the snapshot (stop the kiosk, copy, start),
   diff playlists and media_info against it, restore the time zone, delete
   the snapshot, run `verify_box.sh --with-services`.
