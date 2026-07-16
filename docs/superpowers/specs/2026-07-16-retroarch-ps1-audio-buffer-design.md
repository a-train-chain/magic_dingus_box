# RetroArch PS1 Audio-Buffer Design

## Goal

Eliminate HDMI audio cutouts in PCSX-ReARMed without changing video timing,
display resolution, scaling, bezel placement, frame skipping, controller
mappings, saves, or the latency of other emulator cores.

## Live evidence and root cause

Tony Hawk's Pro Skater 2 was measured while its sound repeatedly cut in and
out. RetroArch was using the configured 48 ms ALSA buffer: 2,304 frames split
into four 576-frame periods at 48 kHz. In a ten-second sample, the ALSA stream
retriggered ten times, its playback delay reached zero, and `avail_max`
reached 2,865 frames—561 frames beyond the buffer's capacity. These are direct
signs of buffer underruns and recovery.

The emulator was not compute-bound. RetroArch used about 19% of one CPU core,
the system remained about 95% idle between service bursts, ARM and V3D clocks
held at 2.0 GHz and 600 MHz, temperature remained 66–67°C, throttling stayed
at `0x0`, HDMI remained 1920x1080/60 Hz, and the display reported no underrun.

Commit `9ec8e94` reduced the previously stable audio latency from 64 ms to
48 ms at the same time it introduced the now-removed PS1 frame-skipping
safety net. While frame skipping was enabled, PCSX-ReARMed masked the small
frontend buffer by requesting 128 ms. Disabling unnecessary frame skipping
correctly restored smooth video but exposed the undersized 48 ms PS1 buffer.

The first implementation restored 64 ms, but live validation rejected it.
During a 30-second Tony Hawk run, the 3,072-frame buffer retriggered 110
times, spent 25 samples outside the RUNNING state, and reported `avail_max`
of 3,830 frames. The video launch, sustained-play, and return checks all
passed, isolating the remaining failure to audio-buffer capacity.

A second live candidate at 96 ms also failed, though it improved the result:
18 retriggers, four non-RUNNING samples, playback delay reaching zero, and
`avail_max` of 5,085 frames against a 4,608-frame buffer. Video launch,
sustained play, and menu return remained clean.

## Design

Add a portable production contract:

```cpp
int audio_latency_ms_for_core(const std::string& core_name);
```

The function returns 128 for the existing PS1 core-name family (`pcsx`,
`beetle_psx`, and `swanstation`) and 48 for every other core. The generated
game config uses that value for `audio_latency`. The core downloader remains
at its current 48 ms because it does not run emulated content.

At 48 kHz, 128 ms produces a 6,144-frame buffer. That covers the 5,085-frame
peak measured during the rejected 96 ms run with 1,059 frames of margin and
matches the minimum latency PCSX-ReARMed previously requested through its
frontend callback. It adds 80 ms of audio buffering to PS1 but does not add
video/input frames or affect non-PS1 consoles. The 128 ms value must still
pass live zero-retrigger validation; otherwise it is rejected rather than
declared fixed.

The PS1-name predicate is shared internally by core-option and audio-latency
generation so those two contracts cannot drift.

## Frozen behavior

The change must preserve:

- PS1 frame skipping explicitly disabled.
- Vulkan with the `khr_display` direct-display context.
- Non-threaded video, two swapchain images, VSync, and frame delay 4.
- Modern TV output at 1920x1080 with viewport `(251, 10, 1415, 1059)`.
- Strict 4:3 scaling and the selected bezel overlay.
- CRT Native output at 640x480.
- PS1 native 1x rendering, ARM64 dynarec, BIOS, SPU threading, CD/XA audio,
  PSX clock 57, interpolation, reverb, and dithering settings.
- All controller mappings, hotkeys, save paths, audio routing, and volume.
- A 48 ms buffer for all non-PS1 emulator cores and the core downloader.
- The 15-second launch timeout and every menu/display/input recovery path.

No core update, resampler change, output-rate change, display change,
background-service shutdown, or system-clock change is part of this repair.

## Testing and acceptance

Implementation follows a red-green test cycle:

1. Add Catch2 assertions that all supported PS1 core names select 128 ms and
   representative non-PS1 names select 48 ms.
2. Observe the test fail before adding the production function.
3. Implement the predicate and generated-config integration.
4. Run the complete portable test suite and Pi-side RetroArch test.
5. Deploy without merging and launch Tony Hawk's Pro Skater 2.
6. Confirm the live ALSA buffer is 6,144 frames and the generated PS1 config
   says `audio_latency = "128"`.
7. Sample ALSA for at least 30 seconds. Acceptance requires zero trigger-time
   resets, playback delay staying above zero, and `avail_max` staying within
   the 6,144-frame buffer.
8. Confirm frame skipping remains disabled, the exact video/bezel contract is
   unchanged, and the game returns cleanly to the menu.
9. Run one title on each of the seven installed cores before merge.

The user performs the final listening and responsiveness check before any
merge decision.
