# RetroArch PS1 Frame-Pacing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop PCSX-ReARMed from repeatedly dropping frames and forcing 128 ms audio latency while preserving every existing display, bezel, core, input, save, and launch-recovery contract.

**Architecture:** Move the existing PCSX-ReARMed option serialization into the portable RetroArch launch-contract module, where Catch2 can exercise the real production writer. Explicitly disable the installed core's frame-skipping mode, retain all other PS1 options byte-for-byte, and leave common video generation untouched.

**Tech Stack:** C++17, Catch2 v3, CMake/CTest, Bash deployment scripts, RetroArch 1.20.0, PCSX-ReARMed, DRM/KMS, Vulkan.

## Global Constraints

- Keep Modern TV output at 1920x1080 and custom viewport `(251, 10, 1415, 1059)`.
- Keep strict 4:3 scaling and the selected bezel overlay unchanged.
- Keep CRT Native output at 640x480 without a custom viewport.
- Keep `video_driver="vulkan"`, `video_context_driver="khr_display"`, `video_threaded="false"`, two swapchain images, VSync, frame delay 4, shaders off, and smoothing off.
- Keep the 15-second KMS takeover deadline and the existing menu/display/input/audio recovery path.
- Keep PCSX-ReARMed ARM64 dynarec, native 1x rendering, SPU threading, BIOS handling, PSX clock 57, audio features, controller mappings, saves, and volume unchanged.
- Do not change any non-PS1 core option.
- Do not update cores, lower resolution, change clocks, stop background services, or merge the branch.

---

### Task 1: Add a testable PS1 core-options contract

**Files:**
- Modify: `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.h`
- Modify: `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp`
- Modify: `magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp:752-780`

**Interfaces:**
- Consumes: a generated launcher core name such as `pcsx_rearmed_libretro`.
- Produces: `void retroarch::write_core_options(std::ostream&, const std::string&)`.

- [ ] **Step 1: Write the failing PCSX-ReARMed contract tests**

Append these cases to `magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp`:

```cpp
TEST_CASE("PS1 core disables frame skipping and preserves native performance options",
          "[retroarch][core-options][ps1]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "pcsx_rearmed_libretro");
    const std::string config = output.str();

    require_line(config, "pcsx_rearmed_pad1type = \"analog\"");
    require_line(config, "pcsx_rearmed_spu_thread = \"enabled\"");
    require_line(config, "pcsx_rearmed_nocdaudio = \"disabled\"");
    require_line(config, "pcsx_rearmed_noxadecoding = \"disabled\"");
    require_line(config, "pcsx_rearmed_frameskip_type = \"disabled\"");
    require_line(config, "pcsx_rearmed_gpu_slow_llists = \"disabled\"");
    require_line(config, "pcsx_rearmed_drc = \"enabled\"");
    require_line(config, "pcsx_rearmed_icache_emulation = \"enabled\"");
    require_line(config, "pcsx_rearmed_psxclock = \"57\"");
    require_line(config, "pcsx_rearmed_spu_interpolation = \"off\"");
    require_line(config, "pcsx_rearmed_spu_reverb = \"disabled\"");
    require_line(config, "pcsx_rearmed_neon_enhancement_enable = \"disabled\"");
    require_line(config, "pcsx_rearmed_dithering = \"enabled\"");
    REQUIRE(config.find("pcsx_rearmed_frameskip_threshold") == std::string::npos);
    REQUIRE(config.find("pcsx_rearmed_frameskip_interval") == std::string::npos);
    REQUIRE(config.find("auto_threshold") == std::string::npos);
}

TEST_CASE("non-PS1 core emits no PCSX-ReARMed options",
          "[retroarch][core-options]") {
    std::ostringstream output;
    retroarch::write_core_options(output, "nestopia_libretro");
    REQUIRE(output.str().empty());
}
```

- [ ] **Step 2: Run the focused test target and verify RED**

Run:

```bash
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4
```

Expected: compilation fails because `retroarch::write_core_options` is not declared.

- [ ] **Step 3: Declare and implement the minimal production writer**

Add this declaration to `magic_dingus_box_cpp/src/retroarch/launch_contract.h`:

```cpp
void write_core_options(std::ostream& out, const std::string& core_name);
```

Add this implementation to `magic_dingus_box_cpp/src/retroarch/launch_contract.cpp`:

```cpp
void write_core_options(std::ostream& out, const std::string& core_name) {
    const bool is_ps1 = core_name.find("pcsx") != std::string::npos ||
                        core_name.find("beetle_psx") != std::string::npos ||
                        core_name.find("swanstation") != std::string::npos;
    if (!is_ps1) {
        return;
    }

    out << "pcsx_rearmed_pad1type = \"analog\"\n";
    out << "pcsx_rearmed_spu_thread = \"enabled\"\n";
    out << "pcsx_rearmed_nocdaudio = \"disabled\"\n";
    out << "pcsx_rearmed_noxadecoding = \"disabled\"\n";
    out << "pcsx_rearmed_frameskip_type = \"disabled\"\n";
    out << "pcsx_rearmed_gpu_slow_llists = \"disabled\"\n";
    out << "pcsx_rearmed_drc = \"enabled\"\n";
    out << "pcsx_rearmed_icache_emulation = \"enabled\"\n";
    out << "pcsx_rearmed_psxclock = \"57\"\n";
    out << "pcsx_rearmed_spu_interpolation = \"off\"\n";
    out << "pcsx_rearmed_spu_reverb = \"disabled\"\n";
    out << "pcsx_rearmed_neon_enhancement_enable = \"disabled\"\n";
    out << "pcsx_rearmed_dithering = \"enabled\"\n";
}
```

Replace the inline conditional option block in `retroarch_launcher.cpp` with:

```cpp
script_file << "cat > /tmp/retroarch_core_options.cfg << 'OPTS'\n";
write_core_options(script_file, core_name);
script_file << "OPTS\n";
```

- [ ] **Step 4: Run the focused tests and verify GREEN**

Run:

```bash
cmake --build magic_dingus_box_cpp/build --target test_retroarch_unit -j4
ctest --test-dir magic_dingus_box_cpp/build -R RetroArchUnit --output-on-failure
```

Expected: the RetroArch unit suite passes with zero failed cases.

- [ ] **Step 5: Run the complete portable test suite**

Run:

```bash
cmake --build magic_dingus_box_cpp/build -j4
ctest --test-dir magic_dingus_box_cpp/build --output-on-failure
```

Expected: build exit 0 and all registered tests pass.

- [ ] **Step 6: Review and commit the source change**

Run:

```bash
git diff --check
git diff -- magic_dingus_box_cpp/src/retroarch/launch_contract.h \
  magic_dingus_box_cpp/src/retroarch/launch_contract.cpp \
  magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp
git add magic_dingus_box_cpp/src/retroarch/launch_contract.h \
  magic_dingus_box_cpp/src/retroarch/launch_contract.cpp \
  magic_dingus_box_cpp/src/retroarch/retroarch_launcher.cpp \
  magic_dingus_box_cpp/tests/retroarch/test_launch_contract.cpp
git commit -m "fix(retroarch): disable unnecessary PS1 frame skipping"
```

Expected: a focused commit containing the regression test and production change.

---

### Task 2: Deploy and verify the real Pi

**Files:**
- No additional source files.
- Runtime outputs: `/tmp/retroarch_core_options.cfg`, `/tmp/retroarch_mdb.cfg`, `/home/magic/retroarch_launcher.log` on `magicpi.local`.

**Interfaces:**
- Consumes: the committed launcher change and the existing deployment/smoke scripts.
- Produces: live evidence that the new PS1 configuration is active and that all console launch/return paths remain healthy.

- [ ] **Step 1: Deploy and build without merging**

Run:

```bash
./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
```

Expected: deployment and Pi build exit 0, with the C++ kiosk service active afterward.

- [ ] **Step 2: Launch Tony Hawk's Pro Skater 2 and inspect the active contract**

Use the existing smoke launcher or the kiosk UI to start the installed Tony Hawk's Pro Skater 2 CHD. While RetroArch is active, run:

```bash
ssh magic@magicpi.local 'grep -E "^pcsx_rearmed_(frameskip|drc|spu_thread|neon_enhancement)" /tmp/retroarch_core_options.cfg; grep -E "^video_(context_driver|fullscreen_[xy]|custom_viewport|threaded|max_swapchain|vsync|frame_delay)" /tmp/retroarch_mdb.cfg'
```

Expected: frame-skip type is `disabled`; no threshold or interval exists; dynarec, SPU threading, and native 1x remain enabled; Modern TV resolution and viewport remain exact.

- [ ] **Step 3: Verify live timing and health**

Run during the title:

```bash
ssh magic@magicpi.local 'vcgencmd get_throttled; vcgencmd measure_temp; sudo grep -E "mode: \"1920x1080\"|enable=1|active=1" /sys/kernel/debug/dri/gpu/state; sudo cat /sys/kernel/debug/dri/gpu/hvs_underrun; tail -n 220 /home/magic/retroarch_launcher.log | grep -E "Setting audio latency|QueuePresent failed|Wayland|Using resolution"'
```

Expected: `throttled=0x0`, 1920x1080 at 60 Hz, HVS underrun 0, no QueuePresent failure, and no PS1 request for 128 ms audio latency in the current launch.

- [ ] **Step 4: Run the seven-core launch/return smoke matrix**

Run the repository's existing RetroArch hardware smoke command for Nestopia, Snes9x 2010, Genesis Plus GX, PCSX-ReARMed, Mednafen PCE Fast, ProSystem, and FBNeo.

Expected for every case: KMS takeover within 15 seconds, process remains alive during steady-state sampling, no Wayland or QueuePresent failure, clean exit, and menu/display/input recovery.

- [ ] **Step 5: Hand off the manual gameplay acceptance test**

Leave the Pi at its menu and ask the user to play Tony Hawk's Pro Skater 2 before merge.

Expected: the picture is smooth and controls no longer exhibit the prior frame-skipping latency; branch remains unmerged.
