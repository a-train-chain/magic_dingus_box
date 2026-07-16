# RetroArch Performance Headroom Design

## Goal

Give RetroArch gameplay on the Raspberry Pi 4B (2 GB) as much CPU, memory,
I/O, and thermal headroom as possible, and reclaim the PS1 audio latency that
was raised to mask underruns — without changing aspect ratio, viewport,
bezels, screen positioning, controller mappings, saves, or the launch/return
architecture. Improvements that apply to all seven cores are preferred;
PS1-specific improvements are additionally in scope because PCSX-ReARMed is
the heaviest core.

## Baseline and prerequisite

All work builds on `main` after `codex/retroarch-kms-launch-recovery` is
play-validated and merged. That branch already establishes: Vulkan with the
`khr_display` direct-display context, supervised KMS takeover with a
15-second timeout and readiness marker, unified launch-failure recovery,
`alsathread` gameplay audio, PS1 frame skipping explicitly disabled, and PS1
`audio_latency = 128`. The launch/return architecture was reviewed for this
design and is sound; no structural changes are proposed.

## Live evidence

Measured on the production Pi (2026-07-16):

- PCSX-ReARMed used 16–19% of one core during Tony Hawk's Pro Skater 2 at
  full clocks. PS1 is not compute-bound; remaining smoothness risks come from
  interference, memory pressure, and thermals.
- Nothing quiets down for games. Movies invoke
  `scripts/playback_services_pause.sh` (stops Radarr/Prowlarr/Byparr,
  ~320 MB RAM and ~6% CPU) plus qBittorrent `pause_all`
  (`playback_screen.cpp:80,102,199`); the game path
  (`controller.cpp` `emulated_game` branch) pauses nothing. qBittorrent was
  observed at 11% CPU while the kiosk idled; all five containers run with no
  resource limits; `VpnHealthMonitor` curls Radarr every 10 s during games
  (its skip guard checks only `video_active`, which is movie-only); the
  Media Browser `ArtworkCache` can hold up to 256 MB of poster textures
  through a game session.
- Memory pressure is real: 314 MB was in zram swap at observation time on the
  1.8 GiB-usable board.
- Thermals: `force_turbo=1` with `over_voltage=6` pins ARM and GPU clocks at
  maximum 24/7. The board idled at 73 °C on passive cooling and
  `get_throttled` showed `0x80000` (soft temperature limit hit since boot).
  Hard progressive throttling starts at 80 °C. No in-game throttling was
  observed in today's sessions (66–68 °C), so this track is preventative.
- `gpu_mem=128` reserves firmware memory the KMS/V3D stack does not use;
  76 MB is the documented safe value, reclaiming ~52 MB.
- The PS1 audio underruns that forced 48 → 128 ms occurred while the system
  was ~95% idle, on both `alsa` and `alsathread` drivers — a bursty-delivery
  signature, not a compute shortage. `pcsx_rearmed_spu_thread = enabled` is
  the prime suspect: upstream defaults it to disabled with a documented
  audio-glitch warning; it exists to help CPU-starved devices, which this is
  not.
- The installed May-2026 aarch64 PCSX-ReARMed build uses the ari64 ARM64
  dynamic recompiler (`pcsx_rearmed_drc = enabled` is correct and must stay).
  `pcsx_rearmed_gpu_thread_rendering` (threaded software rasterization,
  the accepted Pi 4 setting for heavy scenes) is currently unset (disabled).

## Design

Four tracks, implemented and gated independently, in order. A regression is
always attributable to exactly one track.

### Track 1 — Game quiet mode (all cores)

Reuse the movie pause machinery in the game-launch path:

- On game launch, before the DRM handoff: invoke the same qBittorrent
  `pause_all` and `playback_services_pause.sh pause` operations the movie
  path uses. The invocation is asynchronous (fire-and-forget) so launch
  latency does not grow; containers finishing their stop during the first
  seconds of gameplay is acceptable.
- On every game-exit path (normal quit, launch failure, timeout — the
  unified recovery path from the KMS-recovery work): invoke the matching
  resume operations, also asynchronously. The existing boot-recovery unpause
  (`main.cpp` startup) remains the crash safety net.
- Quiet mode is a no-op on unprovisioned Pis (no `services/.env`, no Docker
  stack): the script and qBit client already tolerate absent services; the
  game path must not fail or log errors when they are absent.
- `VpnHealthMonitor` skips polling while a game session is active (extend
  the existing `video_active` skip to also cover the game-running state) and
  for a 90-second grace period after resume, so a restarting Radarr cannot
  flip the "tunnel down" toast after quitting a game. The failure counter
  resets when the grace period starts.
- `ArtworkCache` is cleared before the DRM handoff (while the GL context is
  still current), releasing up to 256 MB of poster textures. Textures are
  disk-cache-backed and rebuild lazily on return; the fetcher thread is left
  alive.

### Track 2 — PS1 audio latency reclaim

Hypothesis-driven A/B on the Pi, using Tony Hawk's Pro Skater 2 (the known
worst case) and the ALSA acceptance harness from the audio-buffer work
(≥30-second soak; zero trigger-time resets; playback delay never zero;
`avail_max` within the buffer):

1. Set `pcsx_rearmed_spu_thread = "disabled"` (inline SPU: steady per-frame
   audio delivery, affordable at the measured CPU usage) and
   `audio_latency_ms_for_core(PS1)` 128 → 64. Run the harness.
2. If clean, attempt 48. Ship the lowest clean value (48 preferred, else 64).
3. If 64 is not clean with inline SPU, revert to `spu_thread = enabled` and
   `audio_latency = 128` (status quo) and close the track as no-change.

The PS1-name predicate (`pcsx`, `beetle_psx`, `swanstation`) stays shared
between core-option and audio-latency generation. Non-PS1 cores stay at
48 ms. The user performs the final listening check.

### Track 3 — PS1 heavy-scene headroom

- Add `pcsx_rearmed_gpu_thread_rendering` to the generated PS1 core options.
  Test `async` first (research-recommended, maximum benefit); if the test
  titles show visual glitches, fall back to `sync`; if that also glitches,
  omit the option (status quo). One value ships globally — there is no
  per-game override mechanism.
- Add `video_frame_delay_auto = "true"` beside the existing
  `video_frame_delay = "4"` in the generated video config, for all cores.
  RetroArch 1.20 then treats 4 as the target and automatically backs the
  delay off if frames run long, converting a potential stutter into a
  4 ms latency give-back. Both keys are asserted by the config-emission
  regression tests.
- The Pi smoke test's PS1 case additionally asserts the ari64 dynarec
  initialization line in RetroArch's verbose log, so a future core update
  that silently drops to the interpreter (a multi-× slowdown) fails the
  smoke run. The exact log string is captured from a live launch during
  implementation.

### Track 4 — Boot configuration (thermal and memory)

Two one-line, individually revertible edits to the production Pi's
`/boot/firmware/config.txt`:

- Remove `force_turbo=1`. The `performance` governor and `arm_freq=2000`
  keep ARM at full clock during all operation; the change lets the GPU/core
  clocks idle down between GPU work, lowering the baseline temperature on
  passive cooling. `over_voltage=6` continues to apply at turbo clocks.
- Change `gpu_mem=128` to `gpu_mem=76`, reclaiming ~52 MB of RAM. The
  KMS/V3D stack allocates from CMA, not firmware memory; video decode uses
  V4L2/CMA and is unaffected.

Acceptance: power-cycle, then a 30-minute sustained PS1 soak with
temperature logged below 80 °C and no under-voltage/capping/throttling bits
(0–3, 16–18) in `get_throttled`; no perceptible frame-pacing change (V3D
clock ramping is the risk — revert `force_turbo` removal if stutter
appears); movie playback, boot splash, serial console, and rotary encoder
re-verified; idle temperature recorded before/after.

These edits are applied to the production Pi and propagate to future clones
through the live-SD cloning pipeline (config.txt travels with the image). No
repo scripts change; the golden-image docs record the settings.

CMA reduction (512 MB → 256 MB) was considered and deferred: Track 1 frees
more memory at lower risk, and CMA pages remain usable by movable
allocations. Revisit only if memory pressure persists after Track 1.

## Frozen behavior

Every track preserves:

- Modern TV 1920x1080 output, custom viewport `(251, 10, 1415, 1059)`,
  aspect-ratio index 22, bezel overlay keys, and CRT Native 640x480 with
  aspect-ratio index 23 — all existing config-emission tests keep passing.
- `video_driver = "vulkan"`, `video_context_driver = "khr_display"`,
  `video_threaded = "false"`, `video_max_swapchain_images = "2"`,
  `video_vsync = "true"`, `video_frame_delay = "4"`.
- PS1 native 1x rendering (`neon_enhancement_enable = disabled`), ari64
  dynarec, BIOS, PSX clock 57, dithering, CD/XA audio, and — unless Track 2's
  gate passes — the current SPU threading and 128 ms buffer.
- All controller mappings, hotkeys, save paths, auto-save/load, audio device
  routing, and volume calculations.
- The 15-second KMS-takeover supervision and every launch/return recovery
  path.
- The `performance` CPU governor, `arm_freq=2000`, and `over_voltage=6`.
- Movie playback's existing pause/resume behavior.

## Explicitly rejected

- Threaded video (A/B-proven Vulkan-KMS swapchain thrash on this device).
- Runahead and rewind for PS1 (confirmed infeasible on Pi 4; runahead would
  also roughly double core memory on the 2 GB board).
- SCHED_FIFO / isolcpus / CPU pinning (no measured emulation benefit in any
  credible source; real starvation risk; no shipping distro does it).
- RetroArch audio driver change (already `alsathread`, the Pi consensus).
- Internal resolution, shaders, smoothing, or any viewport/bezel change.
- Launch-preamble sleep removal (previously deferred for input-reliability
  risk; unrelated to steady-state headroom).
- Runahead for the 8/16-bit cores — feasible on Pi 4 and a real input-lag
  win, but a latency feature rather than headroom; recorded as a possible
  future project.

## Testing

- Local: Catch2 contract tests extend the existing config-emission suite —
  quiet-mode hooks invoked on the game path (launch and every exit path),
  VpnHealthMonitor guard behavior, PS1 core options with/without
  `gpu_thread_rendering`, `video_frame_delay_auto` present for all cores,
  audio-latency predicate values.
- Pi, per track: Track 1 — launch a game, verify containers stopped and qBit
  paused within ~10 s, quit, verify resume + no "tunnel down" toast + Media
  Browser fully functional; kill the kiosk mid-game and verify boot recovery
  resumes services. Track 2 — ALSA harness soak per the decision tree.
  Track 3 — visual A/B on a test set including THPS2 and one 3D-heavy title;
  dynarec assertion in smoke log. Track 4 — the thermal soak protocol above.
- Global regression after each track: the seven-core launch/quit/restart
  smoke matrix (baseline 14/14), plus one movie playback and one Media
  Browser browse/search pass.
- Before/after measurements recorded in the PR: idle temperature, free
  memory and zram usage during gameplay, ALSA retrigger counts, and
  `get_throttled`.
