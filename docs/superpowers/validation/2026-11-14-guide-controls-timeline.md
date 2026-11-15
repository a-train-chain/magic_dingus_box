# Guide controls and short timeline validation — 2026-11-14

The owner approved tap-to-watch, hold-to-Pick-Ahead, a fixed short guide timeline,
and an explicit 1–6 AM movie-channel gap. Implementation used Sol 6.1; Astra
reviewed the hardest whole-branch interactions. Work remains local to
`movie-channels` and `pi5-test`; no push, PR, or shared-branch merge was performed.

## Observed on the Pi 5

The first refinement deployment was `pi5-test` at `b454f6d`, preserving the
Bluetooth work through `3347645`. Its full merged Mac matrix passed nine suites
with Media Browser ON and eight OFF: 3,150 case executions / 114,475 assertions.
All six deployment rsync dry runs showed zero deletions. The actual Linux build
and restart succeeded, and all 417 tracked C++ source/script/CMake files in the
verification helper's scope matched their local hashes.

- Modern TV shows a fixed 20-minute window; CRT Native shows 15 minutes, both
  with five-minute ticks. Short playlist clips remain legible beside long films.
  Future film selection pans to its start without zooming. Long films clip at
  viewport edges with continuation chevrons and visible selection highlighting.
- At the 1 AM live cutoff, the final film's panel ends at 1 AM, NEXT resolves to
  the actual 6 AM opening, and the gap says `OFF AIR · BACK AT 6:00 AM` or the
  narrower `OFF AIR`. CRT retains its intentionally reduced panel details.
- Synthetic Select tap tunes/closes once; keyboard holds survive repeat events
  and their release leaves Pick Ahead open. A stale release from another source
  does not complete a newer gesture. Opening Settings cancels a pending Select.
- The original Movie Channels run separately verified that manually selected
  future films start at zero, continue past 1 AM, finish naturally, and return
  correctly. This refinement does not change that playback policy.

CRT Native was rendered on the attached HDMI TV, not on a physical CRT.
Screenshots and restricted state snapshots are kept locally, outside Git.

## Native input finding and follow-up

Closing an input device 150 ms into Select initially acted as a tap. A separate
process reading raw evdev events observed a kernel-generated release and
`SYN_REPORT=1` roughly 16 ms before `ENODEV`; same-poll device-loss filtering could
therefore miss it. The follow-up conservatively cancels Select releases in
ambiguous packets, preserving normal packets and repeat-only holds. Nonzero SYN
values are not treated as a unique public disconnect API.

Follow-up feature commit `8fe38c5` passed independent scoped review with no
findings. Local integration `b6df5f5` retained both includes in the sole merge
conflict and preserved all 1,027 unrelated tracked entries, all five intended
patches, and Bluetooth ancestry. Its full merged Mac matrix passed nine suites
ON and eight OFF: **3,166 case executions / 118,655 assertions**, all 22 recorded
commands successful. Six fresh deployment rsync dry runs again showed zero
deletions. The authorized native deploy completed successfully, including real Linux
InputManager compilation, and restarted the kiosk as PID 308949. All 418 tracked
C++ source/script/CMake files in the verification scope matched the checkout.
The existing bundled fmt/GCC warnings remained; no compilation error occurred.

Three exact abandoned-press trials kept the guide open and accepted the first
fresh tap; a fourth screenshot confirmed no delayed Hold. Repeat2/SYN1 packets
before and after the threshold preserved Hold and did not tune on release.
A synthetic 700 ms gamepad hold from fullscreen opened programme selection.
One tap on a future film started it at a sampled 0.017258 seconds, 0.602 seconds
after the input, and playback advanced normally. No unexpected kiosk restart
occurred; observer PIDs were the old and deliberately restarted instances only.

## Restoration and remaining acceptance

Before the follow-up build, exact original timezone link/bytes were restored,
both test zones removed, and Modern TV, the original channel and default Live
mode restored. All 627 inventory entries, 74 WatchStore rows and media-info bytes
matched the private baseline. No physical SSD preparation, remount, unplug or
writer lease occurred.

Final restoration matched original settings and media-info bytes/metadata,
exact timezone link/bytes, and all 74 WatchStore rows. Inventory is exactly the
original 627 entries minus the three approved deletions: the MDB Test Clips YAML
and MDB_Test_16x9.mp4 / MDB_Test_9x16.mp4. The additional MDB_Test_16x9_v2.mp4 was
outside that approval and remains. The playlist reload completed.

All six containers returned running/unpaused, original speed caps were restored,
and both original seeders resumed active state. One formerly uploading seeder
is now stalledUP; neither remains stopped. Kiosk-owned pause markers match the
baseline. The SSD stayed mounted ext4 rw,relatime throughout. Test timezones,
remote helpers/screenshots and the observer were removed/stopped. The sole Mac
scratch build was deleted; worktrees, ledgers and local evidence were retained.

The final box check reported **41 passed, 0 failed, 2 warnings**. The warnings
were the optional Dreamcast BIOS and a currently down VPN tunnel with repeated
connection retries; movie downloads are waiting for reconnection. No VPN
configuration or credentials were changed. The checker’s SHIPPABLE label does
not satisfy the outstanding real-board/physical acceptance gates below.

Physical acceptance remains pending: the new knob tap/hold interaction, actual
paired phone/pad operation, and owner-coordinated SSD unplug/reconnect. Earlier
owner feedback confirmed picture/sound, encoder navigation and black/menu
hold-volume, before the new Select interaction was deployed. Real Pi 4B/Trixie
hardware remains a release/golden-image gate.

The final source integration is `pi5-test` at `b6df5f5`; this report is a later
documentation-only commit. PR #16 remains open/draft at `facb2d34`, with all 28
reported checks green for that older pushed head. New local code was not pushed.
