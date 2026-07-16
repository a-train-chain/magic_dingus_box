# RetroArch PS1 Frame-Pacing Design

## Goal

Restore smooth PlayStation gameplay on the Magic Dingus Box without changing
the now-working RetroArch launch handoff, the Modern TV 1920x1080 output, the
4:3 game viewport, bezel placement, controller mappings, saves, or any other
emulator core.

## Live evidence and root cause

Tony Hawk's Pro Skater 2 was measured while the reported lag was visible. The
Pi was not resource-bound: RetroArch used roughly 16–18% of one CPU core, ARM
and V3D clocks held at 2.0 GHz and 600 MHz, temperature remained 66–68°C,
`get_throttled` remained `0x0`, HDMI scanned out at 1920x1080/60 Hz, and the
DRM driver reported zero HVS underruns.

The generated PCSX-ReARMed options instead enable `auto_threshold` frame
skipping at a 33% audio-buffer threshold and permit three consecutive skipped
frames. During the live session the HDMI audio buffer moved through the same
threshold range. The installed PCSX-ReARMed revision confirms that this mode
forces frame skipping below the configured occupancy and raises frontend audio
latency. The launch log recorded that latency being raised from the configured
48 ms to 128 ms. This explains both visibly choppy motion and sluggish feedback
despite substantial CPU and GPU headroom.

The frame-skipping policy was added in commit `9ec8e94` as a safety net. The
earlier configuration did not force frame skipping. Current measurements show
that the safety net is activating when it is not needed.

## Design

Extract generation of the existing per-core options into a small
`write_core_options(std::ostream&, const std::string&)` production function in
the portable RetroArch launch-contract module. The launcher will call this
function when writing `/tmp/retroarch_core_options.cfg`.

For PCSX-ReARMed-compatible core names, preserve every existing compatibility
and performance option except the frame-skipping policy. Emit
`pcsx_rearmed_frameskip_type = "disabled"` explicitly, and stop emitting the
33% threshold and three-frame interval. Explicit disablement protects the
kiosk contract from a changed core default while continuing to delete stale
per-core `.opt` overrides as it does today.

Other core names produce no PCSX-ReARMed options, matching current behavior.
The change adds no gameplay threads, polling, services, or runtime overhead.

## Frozen behavior

This change must preserve:

- Vulkan with the `khr_display` direct-display context.
- Non-threaded video, two swapchain images, VSync, and the existing frame-delay setting.
- Modern TV output at 1920x1080.
- Modern TV custom viewport `(251, 10, 1415, 1059)`, strict 4:3 scaling, and bezel overlay.
- CRT Native output at 640x480 without a custom viewport.
- PCSX-ReARMed ARM64 dynarec, real BIOS use, native 1x rendering, SPU threading, PSX clock 57, and the current audio-quality options.
- All controller mappings, hotkeys, save paths, auto-save/load behavior, audio device routing, and volume calculations.
- Every non-PS1 emulator core's generated configuration.
- The 15-second KMS-takeover timeout and all launch/return recovery behavior.

No display-resolution reduction, core update, threaded-video experiment,
overclock change, or background-service change is part of this repair.

## Testing

Implementation follows a red-green test cycle:

1. Add a Catch2 contract test that requests PCSX-ReARMed options and requires
   explicit disabled frame skipping while rejecting the old threshold and
   interval.
2. Require all retained PS1 performance and compatibility options in the same
   output so the extraction cannot silently drop them.
3. Verify a non-PS1 core produces no PCSX-ReARMed options.
4. Observe the focused test fail before changing production code.
5. Extract the writer, disable frame skipping, then run the focused test and
   the complete portable test suite.
6. Build and deploy to the Pi, launch Tony Hawk's Pro Skater 2, and verify that
   the live core-options file contains disabled frame skipping and that the
   log no longer requests 128 ms audio latency.
7. Reconfirm the active 1920x1080/60 Hz DRM mode, zero throttling, and the
   unchanged viewport/bezel configuration.
8. Run the seven-core launch/return smoke matrix before any merge decision.

The manual play test remains the final frame-pacing acceptance check because
the reported symptom is perceptual and the user requested validation before
merge.
