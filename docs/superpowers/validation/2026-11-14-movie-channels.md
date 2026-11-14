# Movie Channels hardware validation — 2026-11-14

Movie Channels, daily lineups and Pick Ahead were implemented and reviewed on
`movie-channels` at `0cbbeea`, then merged locally into `pi5-test` at `245fa2f`.
The merge preserves the newer Bluetooth work through `3347645`, including all
nine recorded Bluetooth file blobs. Nothing was pushed or merged into a shared
branch. Sol 6.1 handled most implementation; Astra handled daily lineups and the
hardest integration review/fixes.

## Build and deployment

- Merged Mac build: nine suites with Media Browser ON and eight OFF, totaling
  3,120 test-case executions and 113,721 assertions. All passed. Focused script,
  Python and kiosk syntax checks are recorded in the SDD ledger.
- Six actual deployment rsync dry runs reported zero deletions. All 562 deployed
  tracked source/deployment files matched their merged-checkout SHA256 values.
- `PI_HOST=magic@10.0.0.227 ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build`
  completed successfully with `-j2`, compiling the real Linux kiosk, including
  input, rendering and controller integration. The resulting aarch64 binary's
  dependencies resolved on the Pi.
- Tested on the owner's Raspberry Pi 5 Model B Rev 1.1, 2 GB, with the SSD mounted
  and HDMI TV attached. No watchdog or unexpected kiosk restart occurred.

## Observed behavior

- Eight eligible movie channels matched an independent library/rules comparison:
  35 schedulable films from 36 downloaded files. Kids contained six films, all
  G/PG and Family/Animation. Locking removed movie rows and promptly left movie
  playback; unlocking restored them. List style excluded movie channels.
- Modern TV fullscreen used the whole screen without a bezel; whole/fill worked,
  the per-film setting survived restart, and the guide preview stayed whole.
  The CRT Native canvas was also exercised on the HDMI TV, not on a physical CRT.
- Live movies parked at 1 AM and returned automatically at 6 AM. An explicit
  ordinary-channel selection canceled the automatic return. Written playlists
  and Master Shuffle changed at the 4 AM boundary; restart rejoined the same
  civil schedule. Tests changed only the local timezone, never UTC.
- A manually selected future movie started at zero and continued past 1 AM.
  A synthetic seek near EOF led to actual completion, then parking on channel 00
  while movie channels were off air. The subsequent 6 AM return worked.
- Synthetic phone-gamepad and EV_REL/REL_X inputs exercised Pick Ahead navigation,
  future selection, Back, the 24-hour clamp and long-film timeline layout.
  Explicitly selecting on-now while on-demand rejoined live playback.
- Movie quiet mode paused the owned services and active seeders. The first
  restoration occurred 121.24–121.92 seconds after the last movie sample.
  A separate operator fixture proved that a pre-existing paused seeder, speed
  cap and Docker-paused service remained untouched while kiosk-owned work resumed.
- Movie → SNES game → movie preserved the pause beyond five minutes and returned
  to fresh live playback. The exact two test-game save/state files were restored.
- Controlled updater leases passed both overlap orders, live-lease preservation
  across kiosk startup, final-owner restoration and stale-lease recovery. No
  actual update was installed and no power interruption was performed.

## Restoration and health

Original settings and media-info bytes, timezone link/bytes, all 627 inventory
entries and all 74 WatchStore rows matched the private baseline exactly. Six
containers were running/unpaused, both original seeders were active, and the
original speed-cap mode was restored. Test helpers, screenshots on the Pi,
timezone files and the Pi4 policy override were removed. The original test clips
were retained because deletion approval had not been received.

The final `verify_box.sh` run reported **40 passed, 0 failed, 2 warnings**. A
pre-existing companion-services startup timeout from 01:11 UTC was resolved by
rerunning its idempotent start under the compose lock; no containers were recreated.
The remaining warnings were missing optional Dreamcast BIOS and recent VPN/port
forwarding interruptions. Kiosk, web and audio were active; SSD and NTP were healthy.

## Remaining acceptance and release gates

- Physical knob/hold-volume and actual paired phone/pad operation.
- Owner-coordinated physical SSD unplug/reconnect mid-film; no forced unmount was
  used as a substitute. Library removal/reorder and MB playback/deferred-add
  overlap were covered by automated tests, not this hardware run.
- Active downloading transfer behavior: this library had two seeders, no active
  download. All available files used H264, so the Pi4 unsupported-codec rejection
  had unit coverage but no real-library negative fixture.
- Real Pi 4B/Trixie hardware validation before release or golden-image preparation.
  The temporary Pi4 policy rehearsal on Pi5 passed its available checks and was
  fully removed; it does not satisfy real-board validation.

Detailed task reports and the 21-row hardware checklist remain in the feature
worktree's `.superpowers/sdd/2026-11-13-movie-channels/` workspace. Restricted
private snapshots and local screenshots remain in the task scratchpad; they are
not committed or included in release artifacts.
