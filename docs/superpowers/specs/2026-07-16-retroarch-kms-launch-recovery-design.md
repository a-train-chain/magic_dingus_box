# RetroArch KMS Launch Recovery Design

## Goal

Make every console launch reliably transition from the animated Magic Dingus
Box loading screen to RetroArch on the Raspberry Pi 4B. A launch that cannot
take over the DRM/KMS display within 15 seconds must be terminated and must
return to a usable kiosk menu. Existing game rendering, scaling, bezels,
performance tuning, controls, audio, and saves must remain unchanged.

## Evidence and root cause

Live logs from the production Pi show the same sequence for NES, SNES,
Genesis, PS1, PC Engine, Atari 7800, and FBNeo launches:

1. RetroArch starts and finds the requested libretro core.
2. The core, BIOS when applicable, and ROM load successfully.
3. RetroArch accepts the requested 1920x1080 video size.
4. Video initialization stops at `[ERROR] [Wayland]: Failed to connect to
   Wayland server.`

The kiosk intentionally runs directly on DRM/KMS with no X11 or Wayland
compositor. The generated RetroArch config selects Vulkan but does not select
a video context. The launcher also exports `DISPLAY=:0`, which falsely implies
that a display server exists. RetroArch 1.20.0 therefore chooses its compiled
Wayland context instead of KMS and never takes over the display.

The current emulator smoke test does not detect this failure. It marks the
launch successful when `screen_mode` changes to `RetroArch`, but that state is
written before the core, ROM, or video context initializes. The test then sees
an alive but display-less RetroArch process as a successful launch.

## Historical Modern TV contract

The Modern TV behavior was introduced in commit `bc1849f` and documented in
the changelog and the Pi-local Modern Display and RetroArch Bezel guides. The
current Pi confirms the intended split:

- The Magic Dingus Box kiosk UI runs at 1280x720.
- A Modern TV RetroArch session requests a 1920x1080 output mode.
- Emulator cores continue rendering at their native low resolutions. For
  example, PS1 stays at native 1x because
  `pcsx_rearmed_neon_enhancement_enable` is disabled.
- RetroArch scales the native core frame into the established 4:3 custom
  viewport at `(251, 10, 1415, 1059)`.
- The selected 1920x1080 bezel overlay is drawn over that viewport.
- CRT Native remains a separate 640x480 path without a custom viewport or
  RetroArch bezel overlay.

This repair must not change that arrangement. In particular, it must not make
cores render internally at 1080p, add shaders, enable smoothing, alter the
viewport, or change the kiosk UI's 1280x720 mode.

## Video and performance invariants

The following generated RetroArch behavior is frozen by regression tests:

- `video_driver = "vulkan"`
- `video_context_driver = "kms"` (new explicit context selection)
- `video_threaded = "false"`
- `video_max_swapchain_images = "2"`
- `video_vsync = "true"`
- `video_frame_delay = "4"`
- `video_hard_sync = "false"`
- `video_shader_enable = "false"`
- `video_smooth = "false"`
- Core-requested rotation remains enabled for vertical FBNeo games.
- Modern TV remains 1920x1080 with custom viewport
  `(251, 10, 1415, 1059)` and aspect-ratio index 22.
- The selected bezel `.cfg`, full opacity, and hide-in-menu behavior remain
  enabled in Modern TV mode.
- CRT Native remains 640x480 with custom viewport disabled and aspect-ratio
  index 23.
- PS1 native 1x rendering and the existing CPU-saving SPU settings remain
  unchanged.
- All per-core controller mappings, save paths, auto-save/load behavior,
  audio routing, and volume calculations remain unchanged.

`video_threaded` must remain false. Commit `f810b7c` records a controlled A/B
test on this Pi: threaded video caused repeated Vulkan KMS `QueuePresent`
swapchain destruction, while non-threaded video produced clean launches and
full-speed 2D/PS1 gameplay. The two-image swapchain is part of the same known
stable configuration.

The Pi's existing system-level performance settings are outside this repair.
The production device already runs the CPU governor in `performance` mode with
its established overclock. No clock, governor, thermal, or service settings
will be changed.

## Launch architecture

### 1. Prepare while the kiosk owns the display

The controller validates the core and ROM, stops GStreamer, and keeps invoking
the existing loading callback. In Modern TV mode, the callback continues to
draw the spinner inside the selected bezel. Immediately before dropping DRM
master, it presents a final complete loading frame.

Once DRM master is dropped, the kiosk must stop trying to page-flip. The last
loading frame remains scanned out while RetroArch starts. This removes the
current stream of expected-but-noisy `Failed to set CRTC: Permission denied`
messages and prevents the kiosk from competing with RetroArch during takeover.

### 2. Select the correct standalone display context

The isolated RetroArch config adds `video_context_driver = "kms"` next to the
existing Vulkan driver. The child process removes `DISPLAY`, `WAYLAND_DISPLAY`,
`XDG_SESSION_TYPE`, and `SDL_VIDEODRIVER` instead of exporting a fake
`DISPLAY=:0`. It retains the real `HOME` and `XDG_RUNTIME_DIR` values needed by
RetroArch. This makes the direct DRM/KMS execution environment explicit while
preserving every resolution, viewport, overlay, and performance key listed
above.

### 3. Supervise only startup

The generated launcher starts RetroArch as a child process and monitors it for
up to 15 seconds. Startup is considered ready only when the live RetroArch PID
has opened a KMS card node matching `/dev/dri/card*`. Opening a render node by
itself is insufficient because Vulkan can do that before it has a scanout
context.

The supervisor writes a per-launch readiness marker after the KMS card is
opened, then stops polling and simply waits for RetroArch to exit. It consumes
no CPU and adds no latency during gameplay. The marker is deleted before each
launch so stale state cannot create a false success.

If RetroArch exits before readiness or does not open a KMS card within 15
seconds, the supervisor sends `SIGTERM`, waits briefly, escalates to `SIGKILL`
if necessary, cleans up its marker and temporary config, and returns a distinct
startup-failure status.

### 4. Restore the kiosk on every exit path

The controller treats a missing readiness marker or startup-failure status as
a failed launch. The same cleanup path runs after normal game exit, early
RetroArch exit, timeout, script failure, or fork/exec failure:

1. Ensure the RetroArch process group is gone.
2. Reacquire DRM master using the existing retries.
3. Restore the kiosk's 1280x720 display mode through the existing reset path.
4. Reinitialize input devices and restore audio routing.
5. Clear `is_loading_game` and RetroArch status fields.
6. On failure, close the settings/game-browser overlay, return to the main
   playlist menu, and show `Unable to start game` through the existing error
   display.

The result returned by the launcher must reflect startup success rather than
merely successful `fork()`. A game that reached KMS and was later quit is a
successful launch even if RetroArch's final shell status reflects a user-driven
signal.

## Testing strategy

Implementation follows test-driven development.

### Local regression tests

- Generated Modern TV config contains explicit KMS and preserves every frozen
  1920x1080 viewport, bezel, and performance key.
- Generated CRT Native config contains explicit KMS and preserves the existing
  640x480 path.
- Generated launch environment removes compositor variables.
- A fake launcher process that never opens a KMS card times out, is terminated,
  and reports startup failure. The test uses a reduced injected timeout while
  production remains fixed at 15 seconds.
- A fake launcher process that opens the injected test KMS path creates the
  readiness marker and is not killed by the startup watchdog.
- Early child exit and exec failure both report failure and leave no stale
  readiness marker.

### Pi integration tests

The existing smoke harness is changed so `screen == "retroarch"` is not enough
to claim success. It waits for the current launch's readiness marker and a live
RetroArch process, and rejects these log signatures:

- `Failed to connect to Wayland server`
- `QueuePresent failed`
- startup timeout or early exit

The production Pi test matrix launches at least one real title for each of the
seven shipped cores: Nestopia, Snes9x 2010, Genesis Plus GX, PCSX-ReARMed,
Mednafen PCE Fast, ProSystem, and FBNeo. Each case must:

1. Animate and present the loading screen.
2. Reach the KMS readiness marker within 15 seconds.
3. Remain alive long enough to exercise steady-state video.
4. Show no Wayland or Vulkan presentation failure.
5. Quit and return to a responsive kiosk menu with DRM, input, bezel, and audio
   restored.

A forced bad-context launch verifies the negative path: RetroArch is cancelled
after the bounded startup period and the kiosk returns to the menu rather than
remaining on the loading screen.

## Out of scope

- Changing emulator cores, ROM formats, BIOS files, or controller mappings.
- Changing internal core resolution or enabling enhanced-resolution options.
- Changing the Modern TV viewport, bezel assets, overlay opacity, or output
  resolution.
- Re-enabling threaded video, shaders, filtering, or smoothing.
- Changing the kiosk UI's display mode selection or movie/video rendering.
- Refactoring the full generated shell launcher into a new process architecture.
