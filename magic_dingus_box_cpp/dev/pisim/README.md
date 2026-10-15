# Off-Pi dev loop

There is no usable emulator of either board. QEMU's `raspi4b` machine
(9.0+) lacks PCIe — so no USB, no Ethernet — and has no V3D GPU or HDMI;
there is no Pi 5 machine at all. What you CAN reproduce faithfully off-Pi
is the **userspace**: same Debian Trixie, same aarch64 glibc and library
versions, same compiler. On an Apple Silicon Mac that runs natively.

So the loop is four tiers, each catching what the previous one cannot:

| Tier | Command | Time | Catches |
|---|---|---|---|
| 1. Mac unit suites | `cmake --build build-test && ctest --test-dir build-test` | seconds | logic regressions |
| 2. Pi-userspace container | `dev/pisim/pisim.sh check` | ~1 min incremental | Linux-only compile errors (DRM, GBM, evdev, libgpiod, GStreamer, sd_notify), all 9 suites on aarch64 Linux, shell lint, web pytest, compiled-out capabilities |
| 3. Pi-userspace VM (optional) | UTM, see below | manual | the real kiosk running end-to-end on DRM/KMS: UI, menus, video playback, a real pad via USB passthrough |
| 4. Real board | `PI_HOST=… dev/pisim/pisim.sh push` | ~10 s | hardware: V3D perf, v4l2 H.264, GPIO, HDMI, thermals, real RAM pressure |

## Tier 2 — the container

```bash
dev/pisim/pisim.sh check     # build real kiosk binary + run everything
dev/pisim/pisim.sh shell     # poke around inside Trixie/arm64
```

`build` fails if the binary lacks sd_notify, the Media Browser or GPIO —
the release workflow's assertions, run on every build instead of only at
tag time.

## Tier 4 — push the binary, don't compile on the Pi

The container binary is built the same way as the OTA release binary
(`release.yml` builds it in `debian:trixie` too), so it runs on either board:

```bash
PI_HOST=magic@magicpi-ab12.local dev/pisim/pisim.sh push
```

`push` prints the target's hostname and model, uploads to `.new`, runs
`ldd` on the box to confirm every library resolves, keeps the old binary
as `.prev`, then uses the same stop → wait → start sequence as
`deploy_cpp.sh`, and restores `.prev` automatically if the kiosk does not
come up. It only replaces the binary. Script, systemd or asset changes
still go through `deploy_cpp.sh` (without `--build`).

## Tier 3 — running the real kiosk in a VM

The kiosk only needs a DRM/KMS device with GBM/EGL/GLES, and
`drm_display.cpp` scans `/dev/dri/card*` generically. A Debian Trixie
arm64 VM with a virtio-gpu device provides that. Suggested setup
(UTM, QEMU backend, HVF acceleration):

1. Debian 13 arm64 netinst, display device `virtio-gpu-gl-pci` (GPU
   acceleration on). Boot to a console, no desktop.
2. `magic_dingus_box_cpp/scripts/install_deps.sh --media-browser`, build
   in the VM (or share the repo folder and reuse `build-pisim/`).
3. Impersonate a board: `MDB_PI_MODEL_OVERRIDE="Raspberry Pi 4 Model B Rev 1.5"`
   (or `"Raspberry Pi 5 Model B Rev 1.0"`). This is honored only when
   `/proc/device-tree/model` does not exist, so it can never re-profile a
   real box. It exercises the board's menu gating (N64/Dreamcast hidden on
   Pi 4), quiet modes and audio policy.
4. Approximate the Pi 4B's **memory** envelope:
   `systemd-run --scope -p MemoryMax=1500M ./magic_dingus_box_cpp`.
   CPU cannot be approximated: an M-series core is several times an A72,
   so frame pacing, software decode headroom and emulator speed are
   **only** meaningful on real hardware.
5. Pass a real controller through (UTM USB passthrough) to exercise
   evdev, controller detection and the RetroArch launch/return handoff.

Not reproducible in a VM: v4l2 hardware H.264 (software decode is used
instead), GPIO buttons and LEDs, the rotary encoder, HDMI hotplug and
mode lists, the 3.5 mm jack, thermals and throttling.
Validate those on both boards before cutting an image (dual-board rule 7).
