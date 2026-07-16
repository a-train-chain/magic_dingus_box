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

The 128 ms candidate failed too: 22 retriggers, 12 non-RUNNING samples, and
`avail_max` of 6,695 frames against a 6,144-frame buffer. Because the demand
peak continued to move above every larger hardware buffer, capacity alone
was disproven as the root cause.

The generated gameplay config explicitly forces `audio_driver = "alsa"`.
RetroArch's verbose log confirms this starts its synchronous audio driver.
The installed RetroArch 1.20 binary also contains `alsathread`, and the
official RetroArch changelog says the threaded wrapper became the default
for ALSA devices on threaded builds in version 1.9.5. The explicit kiosk
override was therefore defeating RetroArch's intended Linux audio path and
tying hardware feeding to brief emulation/Vulkan-present stalls.

## Design

Add portable production contracts:

```cpp
const char* audio_driver_for_gameplay();
int audio_latency_ms_for_core(const std::string& core_name);
```

`audio_driver_for_gameplay()` returns `alsathread` for every emulator core so
the worker thread can keep the HDMI device fed independently of the
emulation/video thread. The generated game config uses it instead of the
synchronous `alsa` override. The core downloader remains on synchronous ALSA
because it does not run emulated content.

`audio_latency_ms_for_core()` returns the originally approved 64 ms for the
existing PS1 core-name family (`pcsx`, `beetle_psx`, and `swanstation`) and
48 ms for every other core. This avoids retaining the 128 ms workaround once
the driver architecture is corrected. Audio threading does not enable video
threading and does not add video or input frames.

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
- Threaded ALSA for emulator gameplay, with synchronous ALSA retained for the
  core downloader.
- A 64 ms buffer for PS1, and 48 ms for all non-PS1 emulator cores and the
  core downloader.
- The 15-second launch timeout and every menu/display/input recovery path.

No core update, resampler change, output-rate change, display change,
background-service shutdown, or system-clock change is part of this repair.

## Testing and acceptance

Implementation follows a red-green test cycle:

1. Add Catch2 assertions that gameplay selects `alsathread`, all supported
   PS1 core names select 64 ms, and representative non-PS1 names select 48 ms.
2. Observe the test fail before adding the production function.
3. Implement the predicate and generated-config integration.
4. Run the complete portable test suite and Pi-side RetroArch test.
5. Deploy without merging and launch Tony Hawk's Pro Skater 2.
6. Confirm the generated PS1 config says `audio_driver = "alsathread"` and
   `audio_latency = "64"`, and the live hardware buffer is 3,072 frames.
7. Sample ALSA for at least 30 seconds. Acceptance requires zero trigger-time
   resets, playback delay staying above zero, and `avail_max` staying within
   the 3,072-frame hardware buffer.
8. Confirm frame skipping remains disabled, the exact video/bezel contract is
   unchanged, and the game returns cleanly to the menu.
9. Run one title on each of the seven installed cores and confirm the
   generated gameplay config retains `alsathread` before merge.

The user performs the final listening and responsiveness check before any
merge decision.
