# Magic Dingus Box — Live SD Card Cloning

This directory contains tooling to clone a running Pi's SD card over SSH
without ever physically removing the SD card. The result is a `.img.gz`
file on your Mac that you can flash to fresh SD cards using Raspberry Pi
Imager (or any similar tool).

The source Pi loses no permanent data: services come back exactly as they
were once the clone completes. Only ~1 minute of total kiosk downtime.

## Quick start

From your Mac, in the worktree:

```bash
./scripts/golden_image/clone_live_sd.sh
```

Defaults to cloning `magic@magicpi.local` to `~/golden_image_YYYY-MM-DD.img.gz`.

For a non-default Pi or output path:

```bash
./scripts/golden_image/clone_live_sd.sh \
    --pi magic@magicpi-abcd.local \
    --output ~/Desktop/my-golden.img.gz
```

To dry-run (walk through the prepare/restore steps without actually dd'ing):

```bash
./scripts/golden_image/clone_live_sd.sh --dry-run
```

## What happens during a clone

The script runs entirely inside one persistent SSH session (ssh
ControlMaster + ControlPersist). Six steps:

1. **Verify SSH connectivity** + sudo NOPASSWD on the Pi.
2. **Check SD card size + Mac free space** (need ~50% of card size for
   compressed output, or full size if `--no-compress`).
3. **Verify required local tools** (`ssh`, `dd`, `gzip`, optionally `pv`
   for a nice progress bar).
4. **Run `prepare_for_cloning.sh` on the Pi**, which:
   - Stops kiosk + Content Manager + Docker stack
   - Snapshots `device_info.json` + `/etc/hostname` + `/etc/hosts` to
     `/var/lib/magic-dingus-box/cloning_backup/` (these need to differ
     on each clone, so we remove them from disk for the dd, then put
     them back during restore)
   - Re-enables `magic-first-boot.service` (so the cloned image
     fires `first_boot.sh` on its first boot)
   - Drops a marker file at `cloning_backup/in_progress`
   - `sync; sync; drop_caches; sync`
5. **dd the SD card over SSH**, gzip-compressed in flight, written to
   the local `.img.gz` file. Progress shown via `pv` if installed.
6. **Run `restore_after_cloning.sh` on the Pi**, which:
   - Restores `device_info.json` + `/etc/hostname` + `/etc/hosts` from
     backup
   - Disables `magic-first-boot.service` (don't re-fire on source)
   - Restarts the Docker stack, Content Manager, and kiosk service
   - Removes the marker + backup files

A trap handler ensures step 6 fires even if the user `Ctrl-C`s during dd
or the network drops mid-stream — the source Pi never gets stuck in the
"in-progress" state.

## What happens when the cloned image boots on a new Pi

The new Pi inherits the source's filesystem layout but is missing the
identity files we removed in step 4. systemd starts up normally;
`magic-first-boot.service` (which was enabled by `prepare_for_cloning.sh`
on the source and persisted into the dd) fires before the kiosk service.
That runs `first_boot.sh`, which does:

| Step | What | Cloned Pi behavior |
|------|------|-------------------|
| 1 | Regenerate SSH host keys | (skipped — keys exist from source; see footnote) |
| 2 | Expand root FS to fill SD | Full SD now usable |
| 3 | Generate device identity | New UUID + new hostname `magicpi-XXXX` |
| 4 | Create required directories | (idempotent) |
| 5 | Fix ownership | (idempotent) |
| 6 | **Wipe Media Browser per-Pi state** | `services/.env`, `services/config/{radarr,prowlarr,qbittorrent,gluetun,flaresolverr}/*` removed |
| 7 | Self-disable | Won't run again |

After ~90 seconds, the kiosk is up in default mode (RetroArch + your
default playlists work). Media Browser is locked because there's no
`.env`. The operator activates it via the Content Manager UI on their
laptop (drops in a WireGuard config; `setup_services.sh` rebuilds the
entire stack from the codified fixtures in `magic_dingus_box_cpp/scripts/data/`).

> **Footnote on SSH host keys:** every cloned Pi inherits the source's
> SSH host keys. For a kiosk on a trusted LAN this is acceptable — the
> primary access path is the Content Manager (HTTP), not SSH. If you
> care about per-clone host keys, run `sudo ssh-keygen -A` and
> `sudo systemctl restart sshd` on each cloned Pi after first boot.

## Recovery if a clone goes sideways

If the Mac script crashes after running `prepare_for_cloning.sh` but
before `restore_after_cloning.sh` (network drop, kernel panic, force
quit), the source Pi will be in a degraded state: services stopped,
identity files missing. To recover, SSH to the Pi and run the restore
manually:

```bash
ssh magic@magicpi.local "sudo /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh"
```

The restore script is idempotent — running it without an in-progress
marker is a no-op. So it's always safe to run if you're not sure
whether the Pi needs restoration.

## When to use this vs. `prepare_golden_image.sh`

| Scenario | Use |
|---|---|
| Capture current Pi state with full library/data preserved on source | **`clone_live_sd.sh`** (live clone, source untouched) |
| Wipe source Pi's user data + create a "fresh defaults" image | `prepare_golden_image.sh` (destructive) → `create_image.sh` |

The live-clone tooling captures whatever state the Pi is in RIGHT NOW
(plus the per-Pi identity manipulation that lets the cloned image
self-configure). `prepare_golden_image.sh` actively wipes user state
on the source Pi (game saves, settings, downloaded movies in Radarr's
DB, etc.) — useful if you want to start fresh, but not if you have a
working library you want to keep using on the source while building
the golden image.

## Boot config (2026-07-16)

`/boot/firmware/config.txt` on the source Pi carries the RetroArch
performance-headroom settings, and cloned Pis inherit them via the SD
image:

- `force_turbo=1` **removed** — lets the SoC downclock at idle for
  thermal headroom (~6 °C cooler idle measured); the `performance`
  CPU governor still pins ARM at 2 GHz whenever the kiosk runs, so
  gameplay clocks are unchanged.
- `gpu_mem=76` (was 128) — the KMS/V3D stack allocates from CMA, not
  firmware memory, so the larger carve-out was pure waste; 76 MB is
  the firmware-recommended floor for this stack.

Rollback: `/boot/firmware/config.txt.bak-headroom` on the source Pi
holds the pre-change file (`sudo cp` it back and reboot).

## Raspberry Pi 5 (2026-07-20)

The app binary is board-agnostic (runtime detection via
`platform::PlatformProfile`; one arm64 OTA artifact serves both
boards), but **SD images are per-board concerns**. A Pi 4 golden
image will boot a Pi 5 only if it is a full Raspberry Pi OS
Bookworm 64-bit image with `kernel_2712.img` present (stock images
ship it; firmware auto-selects it on Pi 5). Checklist for building
the first Pi 5 golden image:

- **Base OS**: Raspberry Pi OS **Trixie 64-bit** — this matches the
  production Pi 4B, which runs Trixie (Debian 13, kernel 6.12,
  GStreamer 1.26, libgpiod 2.2) as of 2026-07-22 despite older repo
  comments saying Bookworm. Bookworm also boots a Pi 5; Bullseye
  cannot boot one at all.
- **config.txt**: use `[pi4]` / `[pi5]` conditional sections for any
  model-specific settings. Drop `gpu_mem=76` from the `[pi5]` path —
  the setting is ignored on Pi 5 (fully CMA-based).
- **Overlays to carry over** (same lines work on Pi 5; they bind via
  RP1/pinctrl-rp1 automatically): `dtoverlay=dwc2,dr_mode=peripheral`
  for the USB gadget — VERIFIED WORKING on Pi 5 (2026-07-22 bench):
  enumerates on the USB-C power connector; note stock-image
  cloud-init/netplan silently fails to configure usb0 (and wlan0) —
  create the NetworkManager profiles with nmcli instead. Note the
  production Pi 4 has `gpio-shutdown` **commented out** — GPIO 3 is
  handled by `kiosk-standby-watcher.service` instead (unit + script
  are in the repo); carry the watcher, not the overlay.
- **kiosk-standby-watcher caveat**: do NOT enable the watcher on a
  bench Pi with no switch harness attached — GPIO 3 floats HIGH
  (pull-up), which reads as "switch OFF" and the watcher stops the
  kiosk on every boot. Enable it only in the final image for units
  that ship with the physical switch.
- **Cooling is mandatory for Pi 5 production units**: a bare bench
  board hit 85-88 °C and hard-throttled (`throttled=0xe0006`) under
  a single x264 encode. Decode-only playback is fine even throttled
  (1080p30 H.264 measured 6.1x realtime), but RetroArch-class
  sustained load needs the Active Cooler + enclosure airflow.
- **`rotary-encoder` overlay**: NOT scripted anywhere — it is baked
  into the Pi 4 golden image's config.txt by hand. Captured from the
  production Pi 4B (2026-07-22); use this exact line:

  ```
  dtoverlay=rotary-encoder,pin_a=17,pin_b=27,relative_axis=1,steps-per-period=2
  ```

  Without it the encoder produces no `EV_REL` events and seeking dies.
- **Audio**: nothing to configure — sink resolution is dynamic as of
  the Pi 5 groundwork change. The Settings menu hides "Headphone" on
  Pi 5 (no analog jack). If a build needs analog out, add a USB DAC
  or I2S HAT; it will be picked up as the analog sink automatically.
- **Power/cooling**: 5.1V/5A PSU recommended (on 3A supplies the
  firmware caps USB at 600 mA); fit the official Active Cooler for
  sustained RetroArch/decode loads — the Pi 5 runs hotter than the
  4B and a sealed MDB enclosure needs a vent path.
- **To validate on first Pi 5 boot**: rotary encoder + buttons + LEDs
  (GPIO now found by chip label), HDMI audio after boot and after a
  RetroArch session, 1080p H.264 playback headroom (software decode
  on Pi 5 — watch CPU + RAM on the 2GB board), RetroArch launch/
  return (Vulkan V3D 7.1 — the `video_threaded=false` /
  `max_swapchain_images=2` workarounds are Pi 4-tuned and pinned
  identical on Pi 5 until re-benchmarked; see
  `test_launch_contract.cpp`), power-switch halt/wake behavior.
- **HEVC experiment (both boards, optional)**: production runs
  GStreamer 1.26, the release where the SAND-format fix for
  `v4l2slh265dec` landed (see the long comment in `gst_player.cpp`).
  The element is still force-disabled from the Bookworm era. Worth
  re-testing hardware HEVC decode on Trixie — Pi 4's rpivid block
  and Pi 5's HEVC block both go through that element. Only lift the
  disable if playback proves stable.

## Performance notes

| Network | ~32 GB SD clone time |
|---|---|
| Wired Ethernet (1 Gbps) | ~5-10 min |
| USB-Gadget Ethernet | ~30-50 min |
| WiFi 5 (802.11ac) | ~30-90 min |
| WiFi 4 (802.11n) | ~2-4 hours |

Compression is `gzip -1` (fast, not max). On typical Pi content the
output is ~50% of the source size. Use `--no-compress` to skip if
you'd rather have a raw `.img` file (some flashing tools require it).

## Output file flow

```
~/golden_image_2026-04-26.img.gz
        │
        ├──► Raspberry Pi Imager → "Use custom" → flash to SD
        │
        └──► Or via dd: gunzip < golden_image.img.gz | sudo dd of=/dev/diskN bs=4M
```
