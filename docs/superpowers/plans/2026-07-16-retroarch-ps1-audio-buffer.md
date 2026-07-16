# RetroArch PS1 Audio-Buffer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate PS1 HDMI audio underruns by restoring a 64 ms frontend audio buffer for PS1 games only, without altering the existing video, scaling, bezel, input, save, or non-PS1 behavior.

**Architecture:** Add a portable core-name-to-audio-latency contract beside the existing RetroArch launch-contract helpers. Reuse one internal PS1 predicate for both core options and audio latency, then have only the gameplay config consume the selected value. Preserve the core-downloader config at 48 ms.

**Tech Stack:** C++17, Catch2, CMake/CTest, RetroArch/libretro, ALSA, Raspberry Pi DRM/KMS/Vulkan, Bash deployment tooling.

## Global Constraints

- Preserve the exact Modern TV video contract: 1920x1080/60, viewport `(251, 10, 1415, 1059)`, strict 4:3 scaling, bezel overlay, Vulkan `khr_display`, non-threaded video, two swapchain images, VSync, and frame delay 4.
- Preserve CRT Native 640x480 behavior.
- Preserve disabled PS1 frame skipping and every existing PCSX-ReARMed performance/core option.
- Preserve controller mappings, hotkeys, saves, audio routing, volume, and the 15-second launch/recovery behavior.
- Keep all non-PS1 gameplay and the core downloader at 48 ms.
- Deploy to the Pi for validation but do not merge the feature branch.

## Task 1: Add the PS1-only audio-latency launch contract

**Files:**

- Modify: `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.h`
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp`

**Step 1: Write the failing contract tests**

Add Catch2 cases proving that `pcsx_rearmed_libretro`, `beetle_psx_libretro`, and `swanstation_libretro` select 64 ms, while representative NES and SNES core names select 48 ms.

**Step 2: Run the focused test target and verify RED**

Run:

```bash
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4
```

Expected: compilation fails because `audio_latency_ms_for_core` is not declared.

**Step 3: Implement the smallest production contract**

Declare this in `launch_contract.h`:

```cpp
int audio_latency_ms_for_core(const std::string& core_name);
```

In `launch_contract.cpp`, extract the existing PS1 core-name test into an internal `is_ps1_core()` helper and use it in both `write_core_options()` and `audio_latency_ms_for_core()`. Return 64 for PS1 and 48 for every other core.

In the game-launch config in `retroarch_launcher.cpp`, replace the literal `audio_latency = "48"` with the helper's result. Leave the core-downloader literal at 48 ms.

**Step 4: Run focused and full portable verification**

Run:

```bash
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4
ctest --test-dir magic_dingus_box_cpp/build --output-on-failure
git diff --check
```

Expected: all tests pass and the diff has no whitespace errors.

**Step 5: Review the generated-config boundary**

Inspect the diff and search every `audio_latency` assignment. Confirm the gameplay assignment uses the new helper, the downloader remains 48, and no video/input/save/core-option contract changed.

**Step 6: Commit**

```bash
git add magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp \
  magic_dingus_box_cpp/src/retroarch/launch_contract.h \
  magic_dingus_box_cpp/src/retroarch/launch_contract.cpp \
  magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp
git commit -m "fix(retroarch): prevent PS1 audio underruns"
```

## Task 2: Deploy and validate the live ALSA behavior

**Files:**

- Verify: `/tmp/retroarch_mdb.cfg` on `magicpi.local`
- Verify: `/proc/asound/card1/pcm0p/sub0/hw_params` on `magicpi.local`
- Verify: `/proc/asound/card1/pcm0p/sub0/status` on `magicpi.local`

**Step 1: End any active emulator cleanly**

If RetroArch is running, send SIGTERM and confirm the kiosk returns to the menu so auto-save and DRM/input recovery complete normally.

**Step 2: Deploy and build on the Pi**

Run:

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Then run the Pi-side tests and confirm the kiosk service is healthy.

**Step 3: Launch Tony Hawk's Pro Skater 2 for at least 35 seconds**

Use the existing emulator smoke harness with playlist 4, game 27 and a 35-second play interval. Confirm the direct KMS/Vulkan display becomes ready within the 15-second deadline.

**Step 4: Measure the live ALSA stream concurrently**

Confirm `/proc/asound/card1/pcm0p/sub0/hw_params` reports `buffer_size: 3072`. Sample ALSA status for at least 30 seconds and record trigger-time changes, minimum delay, maximum delay, and maximum `avail_max`.

Expected acceptance:

- zero trigger-time resets;
- minimum playback delay above zero;
- maximum `avail_max` no greater than 3,072 frames.

**Step 5: Verify the full generated contract**

Confirm `/tmp/retroarch_mdb.cfg` contains `audio_latency = "64"`, disabled PS1 frame skipping, and the exact existing video/scaling/bezel values from Global Constraints. Confirm a clean return to the menu.

**Step 6: Run the seven-core smoke matrix**

Run one title on each installed core with the existing smoke harness. Confirm all seven launch, display, and return successfully, and that non-PS1 generated configs retain 48 ms.

**Step 7: Final verification and handoff**

Run fresh local tests, Pi-side tests, service health checks, thermal/throttling checks, and `git status --short`. Report measured audio evidence to the user and leave the feature branch unmerged for their listening test.
