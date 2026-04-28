# Changelog

All notable changes to Magic Dingus Box will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.5.2] - 2026-04-28

Cloning hygiene patch. Extends `first_boot.sh` to wipe three more categories of operator-specific content from the cloned image so a fresh-flashed Pi doesn't inherit the source operator's gameplay state, save states, or uploaded videos.

### Changed
- **`first_boot.sh` Step 6e: wipe inherited operator-content directories on the cloned Pi.** Three new wipes, all on the cloned Pi only (the source Pi keeps its content):
  - `data/saves/<core>/*.srm` — RetroArch SRAM saves. Source operator's Zelda character / Mario progress / etc. won't carry over to the next operator's Pi.
  - `data/states/<core>/*.state.auto` — Auto-resume save states. Same fix.
  - `data/media/*` — Operator-uploaded videos. Often gigabytes of operator-curated content (Content Manager → Upload flow) that has no business on a stranger's Pi and inflates every flashed image (mp4 compresses poorly through gzip).
- The directories themselves are kept (only their contents are wiped), so RetroArch and the kiosk's media-upload path don't have to recreate them on first use.
- All three paths remain in update.sh's OTA preservation list — they're still untouched by future OTAs on real operator Pis. `first_boot.sh` is the only stage that can distinguish "fresh clone state" from "operator's working Pi", which is the right place for this cleanup.

### Why this release
v1.5.1 closed the silent-OTA-exit bug. With OTA reliable, the next workflow gap is "I want to clone my working Pi as a starter image for new operators, but I don't want them to receive my saves/uploaded videos." Pre-v1.5.2, the workflow required manual stash-and-restore around `clone_live_sd.sh`. Post-v1.5.2, the source Pi keeps everything; flashed Pis automatically arrive clean.

## [1.5.1] - 2026-04-28

OTA-path patch release. Fixes two bugs in `update.sh`'s `get_binary_url()` that caused **a silent mid-install exit on every v1.5.0 → next-version OTA**, leaving the kiosk in a non-running state. Discovered while testing the v1.4.0 → v1.5.0 OTA against a Pi with the full Media Browser stack — the rsync portion (and the operator-content preservation that the v1.4.3 release added) worked perfectly, but the script silently terminated before the rebuild + service-restart steps. **Anyone updating away from v1.5.0 should upgrade through this version.**

### Fixed
- **`update.sh install` no longer exits silently after the rsync stage.** Two compounding bugs in `get_binary_url()`:
  - **Double-`v` URL.** Callers may pass either `v1.5.0` or `1.5.0`; the function unconditionally prepended `v` via `v${version}`, producing `/releases/tags/vv1.5.0` → 404 from GitHub.
  - **`grep` no-match killed the script.** The `grep | head | sed` pipeline that extracts a binary asset URL exits with status 1 when `grep` finds zero matches (which is the *common* case — most v1.x.y releases ship source tarballs only, no pre-compiled binary asset). Under the script-level `set -euo pipefail`, that nonzero pipeline propagated and terminated the entire install. VERSION had already been rsync'd to the new value, but the rebuild + service-restart steps never ran. Operator-visible symptom: kiosk goes black after the update progress bar reached "checking_binary 35%" and never comes back; no error reported to the web admin's update job. Same family of bug as the v1.4.3 SIGPIPE fix in `check_update()` — the v1.4.3 fix didn't cover this second function.
  - Fix: strip a single leading `v` at the function entry, and run the parse pipeline in a subshell with `set +e +o pipefail` so a no-match grep no longer kills the parent script. Subshell pattern (rather than `set +e` directly inside the function) is deliberate — it scopes the flag relaxation to `get_binary_url` only, so the install path's strict mode remains in force for the rest of the script and a rollback still triggers cleanly on any real install failure that occurs later.

### Recovery for Pis stuck mid-OTA on v1.4.0 → v1.5.0

If you ran v1.4.0/v1.4.1/v1.4.2 → v1.5.0 OTA before v1.5.1 was published and your kiosk is sitting at v1.5.0 VERSION but inactive, SSH in and finish what update.sh started:

```bash
cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && cmake .. && make -j2 && \
  sudo systemctl daemon-reload && \
  sudo systemctl restart magic-dingus-box-cpp.service
```

Operator content (`services/.env`, `services/config/*`, ROMs, saves, settings) is intact — the rsync's exclude lists worked correctly; only the post-rsync rebuild step was missed.

## [1.5.0] - 2026-04-28

Visual quality release: a complete rewrite of the CRT effects shader pipeline with five compounding upgrades, fully opt-in via a runtime "CRT Engine: Classic | Enhanced" toggle in Settings → Display. The Classic engine is bit-for-bit identical to v1.4.3, so an OTA from any earlier version produces zero visible change until the operator flips the toggle. Every effect was upgraded; the existing 7 sliders (Scanlines, Color Warmth, Phosphor Glow, RGB Mask, Screen Bloom, Interlacing, Flicker) now drive physically-motivated CRT physics rather than alpha-blended overlays.

### Added
- **Render-to-FBO substrate.** When the Enhanced engine is active, kiosk video + UI are routed into an offscreen RGBA8 FBO at HDMI mode size; a final composite shader reads that scene texture and produces final pixels. This is the precondition for every effect that reacts to scene content (which the v1.4.3 procedural overlay couldn't do at all).
- **Lottes-style aperture-grille subpixel mask.** Replaces the v1.4.3 column-darkening with proper per-channel R/G/B subpixel attenuation (Trinitron stripes). A red object now actually paints the R subpixels along its column.
- **Brightness-modulated Gaussian scanlines.** Beam width grows with luma (`σ = 0.30 + 0.15·L³`), so dark areas show crisp black gaps and bright areas show soft gaps where lines fade — the highlight-bloom-into-adjacent-lines character of a real CRT. Replaces the static sine-wave pattern.
- **Color Warmth as gamma + temperature shift.** The slider now drives a gamma boost (2.2 → 2.4 at full intensity) plus a per-channel D65 → ~5000K temperature shift `(1.00, 0.92, 0.82)`. Punchy, deeper blacks; warm without dyeing the screen orange.
- **RGB convergence error.** The Phosphor Glow slider now also drives quadratic-radial RGB beam misalignment — R drifts outward, B drifts inward, with offset growing toward the corners. Visible color fringing at the edges of the screen, just like an aged consumer CRT.
- **Luma-driven halation/bloom.** The Screen Bloom slider now produces a real two-pass dual-Kawase downsample chain (1/2-res → 1/4-res, with luma threshold ~0.7 on the first pass) screen-blended back into the composite. White text on dark backgrounds glows; the previous "fake center hotspot" white glow that ignored content is gone.
- **`CRT Engine: Classic | Enhanced` toggle** in Settings → Display. Live A/B comparison without touching slider values; persisted to `config/settings.json` as `display.enhanced_crt_enabled` and preserved across OTAs (in update.sh's `config/*` exclude list).

### Performance
- 60-second sustained-load measurement on Pi 4B with all 7 effects at MAX intensity + Enhanced engine + video playback running: V3D GPU stayed pegged at full 600 MHz throughout, SoC temperature ranged 62.3–64.2 °C (no rise across the run), `vcgencmd get_throttled` reported `0x0` at every 10-second sample point. Frame budget never approached 16.6 ms / frame at 1080p. Composite cost (FBO+shader) is estimated < 1 ms for Phases 1-4 active and ~1.5 ms additional when Phase 5 halation kicks in.
- All bloom/scene FBOs are lazily allocated only when their gating slider is non-zero — slider-OFF stays at the same cost as the legacy path.
- Cleanup paths (`reset_gl()` for RetroArch handoff, plus `cleanup()`) handle every new resource correctly. RetroArch launch/return cycles tested.

### Fixed (caught during the rewrite)
- **`gst_renderer::render_quad()` was hard-binding `GL_FRAMEBUFFER, 0` mid-render** ("Ensure we are drawing to the default framebuffer"), which would have broken the Enhanced engine by clobbering the scene FBO and sending video to the default framebuffer where the composite would then overwrite it with a black FBO. Removed the explicit bind with a long-form comment so the bug can't be silently reintroduced. Caller now owns target-framebuffer selection.

### Tuned
- **Effect cycle now correctly reads Off → Low → Medium → High.** Previously the cycle values (`0.15`/`0.30`/`0.50`) and the label thresholds (`≤0.35` Low / `≤0.6` Medium / `>0.6` High) disagreed, so a fresh cycle showed `Off → Low (15%) → Low (30%) → Medium (50%)` and the operator never saw a "High" label. New cycle values are `0` / `0.25` / `0.50` / `0.75` (clean off / quarter / half / three-quarters); thresholds placed at midpoints (`0.05` / `0.375` / `0.625`) so cycle output always lands in its correct tier. Migration is gentle — operators on legacy `0.15`/`0.30`/`0.50` values read as the right tier today and step up to clean values on next click.
- **Effect sublabels refreshed** to describe what the Enhanced engine actually does:
  - Phosphor Glow: "Radial glow" → "Vignette + RGB convergence"
  - Screen Bloom: "Bright glow" → "Phosphor halation"
  - Color Warmth: "Temperature" → "Warm tone + gamma"
  - Scanlines: "CRT lines" → "Horizontal CRT lines"
  - RGB Mask: "RGB stripes" → "Subpixel R/G/B stripes"
  - Interlacing: "Video lines" → "Alternate-line darken"
  - Flicker: "Subtle pulse" → "Brightness wobble"
  - CRT Engine: "v1.4.3 vs new pipeline" → "Classic / Enhanced"
- Internal `CYCLE_PHOSPHOR_MASK` enum renamed to `CYCLE_RGB_MASK` to match the operator-visible "RGB Mask" label (the original name predated the v1.4.0 rename).

### Reversible
A `v1.4.3-pre-crt-rework` git tag marks the pre-rework state. If the new shader is undesired:
- Operator-level: flip "CRT Engine" back to Classic in Settings → Display.
- Source-level: `git checkout v1.4.3-pre-crt-rework -- magic_dingus_box_cpp/src/ui/renderer.{h,cpp}` and rebuild.

## [1.4.3] - 2026-04-28

OTA-path patch release. Caught while verifying the v1.4.2 update flow end-to-end. **Anyone updating from v1.0.x → v1.4.3 should upgrade through this version, NOT v1.4.2 directly** — v1.4.2's update.sh would wipe operator content that v1.4.3 properly preserves.

### Fixed
- **OTA update preserves operator content (`services/.env`, `services/config/*`, `data/thumbnails/*`).** The rsync `--exclude` lists in update.sh's backup, install, and rollback paths missed three paths that contain operator-specific state. Updating from v1.4.2 (or earlier) would silently wipe:
  - `services/.env` — per-Pi WireGuard private key, ProtonVPN credentials, auto-generated Radarr/Prowlarr API keys, qBit admin password. Operator's Media Browser would die and they'd have to redo the entire WG-config drop + setup_services.sh flow.
  - `services/config/{radarr,prowlarr,qbittorrent,gluetun,flaresolverr}/*` — per-Pi stack runtime state (Radarr library DB, Prowlarr indexer sync history, qBit fastresume + cookies, Gluetun VPN runtime). Operator would lose every movie they've added.
  - `data/thumbnails/*` — game cover art populated externally by `deploy_cpp.sh` from the operator's local thumbnails folder (gitignored, so the GitHub release tarball doesn't contain them). Operator would lose all 157 game cover images.
- **`update.sh check` was silently failing.** Some shell + GitHub-payload combinations triggered a `SIGPIPE`-from-`head`-killing-upstream-`grep` cascade inside `check_update()` that the script's `set -euo pipefail` propagated as a fatal error, killing the function before its `cat << EOF` JSON output. The web admin's "Check for Updates" button received an unparseable error response. Relaxed `set -e/pipefail` inside `check_update()` only; the install path's strict mode stays in force so a mid-extract failure still triggers clean rollback.
- **GitHub Releases for v1.4.0 and v1.4.1 were missing.** Both versions were tagged in git but never published as GitHub *Releases*, which is what `update.sh` actually queries (`/releases/latest`). An out-of-date Pi running v1.0.x to v1.3.0 would see "v1.3.0 is latest" (the last published Release) and never offer an update. v1.4.2 published a Release for the first time since v1.3.0; v1.4.3 continues that going forward.

## [1.4.2] - 2026-04-28

Patch release with two CRT-TV display fixes caught during physical CRT-TV verification on the v1.4.1 Pi. Both bugs share the same root class — code that assumed the HDMI mode dims (1280×720) match the renderer's logical content viewport, when in CRT_NATIVE the kiosk uses a 640×480 logical space inside that 1280×720 framebuffer.

### Fixed
- **Pillarboxed video on CRT TV.** The aspect-ratio-preserving math in `GstRenderer::render_quad()` (added in v1.4.0 for Modern TV's Media Browser playback so 2.35:1 movies don't stretch vertically) was running in CRT_NATIVE mode too — its only gate was `letterbox_mode_=false`. Result: 4:3 transcoded videos got 160px pillar bars added on the Pi side, then the CRT TV's built-in HDMI→4:3 converter added ANOTHER round of bars at the receiving end, producing a window-boxed image with margins on all four sides. Added `GstRenderer::set_aspect_preserve(bool)` (defaults true so Modern TV behavior is unchanged); `main.cpp` flips it false in CRT_NATIVE so `render_quad` falls through to its original fill-the-framebuffer behavior. CRT TV now receives a fully-filled 1280×720 signal and its downstream converter handles aspect correction on the receiving end.
- **Toast notifications rendered off-screen in CRT mode.** `Toast::render` was being called with `mode.width/height` (the raw HDMI mode dims, 1280×720), but the renderer's projection matrix is set up for the *content viewport* dims (640×480 in CRT, 960×720 in Modern TV when letterbox is active). The "Wi-Fi connected" panel computed its position as `(1280-480)/2 = 400` logical, then that 400 was interpreted in 640×480 space → projected to 62% from the left, ending up in the lower-right corner with the right side clipped off-screen. Added `Renderer::get_width()/get_height()` returning the active content-viewport dims; main.cpp passes those to `Toast::render` instead. Side benefit: also fixes a previously-unreported ~6% horizontal off-center in Modern TV's main-playlist letterboxed mode (Renderer at 960×720, caller was passing 1280×720).

### Tooling
- `clone_live_sd.sh` gained a `--yes / -y` flag for non-interactive runs. The `Continue with live clone? [y/N]` prompt's `read -r` doesn't reliably accept piped stdin in all environments, so harness-driven invocations would silently abort. The DRY_RUN path already had a parallel skip; this just adds a flag-driven bypass for the same pattern. Used during the v1.4.0/v1.4.1 cloning runs.

## [1.4.1] - 2026-04-27

Patch release with two clone-quality fixes caught during the v1.4.0 image's first flash-test on a fresh Pi (`magicpi-9768`). No source-code refactors, no new features — just two papercuts that would have shipped to every operator otherwise.

### Fixed
- **Cloned Pis without a wired faceplate boot to a dead kiosk** (`a4f3430`): the standby watcher's `reconcile_initial_state()` read GPIO 3 as HIGH and treated it as "switch in OFF position — stop services," killing the kiosk service ~1 second after it started. The HIGH read is ambiguous though — it can mean either "switch wired and OFF" OR "no switch wired at all and the line is floating via the kernel pull-up." The latter applies to every fresh clone before faceplate assembly, AND to anyone who flashes the image to a bare Pi 4 with no switch. Watcher now only acts on actual GPIO transitions during runtime; HIGH at boot is logged but ignored. LOW at boot still calls `start_services` (unambiguous — line is actively pulled to ground). Trade-off: an operator who power-cycles their box with the switch already in OFF position will see the kiosk briefly come up before they re-flip the switch off, but that's strictly better than "boot to a black screen with no recovery path."
- **`first_boot.sh` now updates `/etc/hosts` to match the regenerated hostname** (`9c3cb9c`): cloud-init's `manage_etc_hosts=True` writes a `127.0.1.1 magicpi magicpi` line that systemd's NSS-files resolver uses for self-lookup. `first_boot.sh` Step 3 was already calling `hostnamectl set-hostname magicpi-XXXX` to give cloned Pis unique hostnames, but it wasn't touching `/etc/hosts`. The mismatch made every `sudo` (and many hostname-self-lookup code paths) emit `sudo: unable to resolve host magicpi-XXXX: Temporary failure in name resolution` and pay a small DNS-timeout delay. Cosmetic in isolation but accumulated across boot scripts and operator commands. Now sed-replaces the `127.0.1.1` line to match the new hostname; falls back to appending if no entry exists.

Both fixes verified live on `magicpi-9768` during the v1.4.0 image's flash test before re-cloning to v1.4.1.

## [1.4.0] - 2026-04-27

The "ready for the golden image" release. Closes out a long testing pass with end-to-end verification of every kiosk surface (controllers, all 7 emulator cores, bezel cycling, web admin, Media Browser pipeline) plus a stack of fixes uncovered along the way. Tagged at `1.4.0` because it ships several user-facing features (Media Browser auto-pause-on-playback, audio normalization in upload, 2-player support, tighter Library UI) on top of the bezel/PS-pad/overclock work that was already pending in the prior `[Unreleased]` block.

### Added
- **Three custom MDB bezels** (MDB-1974 wood-grain console, MDB-1986 Memphis neon, MDB-KV19 sleek black broadcast monitor) shipped as the new defaults in [bezels.json](magic_dingus_box_cpp/assets/bezels/bezels.json) with matching RetroArch overlay `.cfg` sidecars.
- **RetroArch bezel overlay in Modern TV mode**: launching a game in `MODERN_TV` mode now writes a 1920×1080 RetroArch config with a 4:3 `custom_viewport` at `(251, 10, 1415, 1059)` (the geometric intersection of all bezel families' transparent cutouts) and `input_overlay` pointing at the user's selected bezel `.cfg`. The bezel frames the game so a modern 16:9 TV presents retro titles inside their period-appropriate "screen". CRT_NATIVE mode launches at 640×480 with no overlay (byte-identical to pre-feature behavior).
- **Auto-detected dual controller mappings**: kiosk now scans `/dev/input/js*` and reads VID/PID from sysfs at game-launch time, classifying as `N64_ADAPTER` (`0e6d:111d`), `PS_STYLE_DRAGONRISE` (`0079:0006`), or `UNKNOWN`. Dispatches to per-core mapping tables tailored to each controller. PS-style pad gets PS1 1:1 mapping, classic SNES face button positions, Genesis A/B/C, FBNeo 6-button fighter layout, etc. Unknown / no-controller falls back to N64 mapping.
- **Kiosk UI input support for PS-style USB pads**: button codes 288–299 now recognized alongside N64 codes. Detects 8-bit-axis controllers (`abs_min == 0`, `abs_max ≤ 255`) and treats `ABS_X/Y` extremes as digital D-pad presses. `ABS_Z`/`ABS_RZ` (right stick on these pads when in analog mode) explicitly ignored.
- **Tier 1 performance overclock**: CPU 1.8 → 2.0 GHz, V3D GPU 500 → 600 MHz, GPU memory split 76 → 256 MB, force_turbo, performance governor pinned via new `magic-cpu-performance.service` (one-shot at boot). Net ~+15% headroom for heavier cores like PS1 at 1080p output. CPU/GPU clocks set in `/boot/firmware/config.txt` (captured by golden image).
- **Two-player support across all 7 cores**: launcher now emits `input_player2_*` button mappings alongside player 1, plus `pcsx_rearmed_pad2type` so the PS1 BIOS recognizes the second pad. Verified live with Twisted Metal 4 split-screen Battle. Hotkeys (RA menu toggle / exit combo) intentionally stay player-1-only so both controllers don't fight over them.
- **Auto-pause torrents during movie playback**: PlaybackScreen now calls qBittorrent's `/torrents/stop` (qBit 5.x; with `/pause` fallback for 4.x) when a movie starts, and `/start`/`/resume` when the user exits. Eliminates random-IO contention on USB-flash media that was making rotary scrubbing freeze. Only resumes torrents the kiosk paused — never re-starts ones the operator manually stopped before entering playback.
- **Smart movie release scoring with seeder filter**: indexer-level `minimumSeeders=5` filter rejects dead-swarm releases at search time. Codified in `setup_services.sh` Step 14b so cloned Pis inherit the threshold via the standard re-provisioning flow. Net effect: Radarr never picks a release that won't actually progress, even if its tracker reports cached "9 seeders exist" but no peer is actually online.
- **Audio normalization in upload pipeline (Retro Ripper match)**: video upload UI's "Normalize audio volume" checkbox now actually fires the FFmpeg `loudnorm` filter — both `/admin/upload-and-transcode` and `/admin/smart-upload` previously dropped the form field on the floor. Loudnorm parameters changed from broadcast quiet (`I=-23:LRA=7:tp=-2`) to match the Retro Ripper companion (`I=-16:TP=-1:LRA=11`) so curator-supplied content and self-uploaded phone clips on the same playlist sound consistent. Default on. Smart-upload's "direct copy" shortcut is suppressed when normalize is requested so the loudnorm pass actually runs.
- **Port-80 redirect to Content Manager**: tiny Python HTTP service on port 80 issues 302 redirects to `:5000`, letting operators type `magicpi.local` (or any IP the Pi is reachable at) without remembering the port number. Runs as `content-manager-redirect.service` with `CAP_NET_BIND_SERVICE`. Preserves path + query + fragment.
- **Zero-config DHCP for plugged-in Mac/PC via dnsmasq**: when an operator plugs their laptop into the Pi's USB-C, the Pi now acts as a DHCP server on `usb0` and auto-assigns `10.55.0.10+` to the host. No manual interface configuration. Config in `scripts/data/dnsmasq-usb0.conf`, scoped to usb0 only (port=0 disables the DNS half so it doesn't fight systemd-resolved on wlan0).
- **Kiosk standby switch via GPIO 3**: `kiosk-standby-watcher.service` runs `gpiomon` on GPIO 3 and stops kiosk + Content Manager + Docker stack on rising edge (switch OFF), starts them on falling edge (switch ON). Replaces the previous full-poweroff behavior — Pi stays running so it boots back into the kiosk in seconds when the operator flips the switch back on.
- **Wi-Fi connection feedback Toasts + "Connecting..." status**: the Wi-Fi sub-menu now shows "Connecting to <SSID>..." the moment the operator presses Enter on the password prompt, with a Toast on success/failure once `nmcli` finishes. Previously the sub-menu sat silent for 5–10 sec and then suddenly flipped to "Connected" with no in-progress feedback.
- **Idempotent setting-menu re-entry**: `enter_submenu()` preserves transient state (selected_index, scroll_offset, confirm-dialog flags) when called with the same section it's already in, instead of resetting them. Fixes a regression where pressing Disconnect → Confirm was a one-frame flicker because the SELECT handler re-ran `enter_submenu(WIFI)` and clobbered the just-flipped confirm flag.
- **Auto-resume save state for cloned Pis** (cloned-image only): added `libretro_info_path` to the generated RetroArch config so `core_info_list` populates correctly. Without it, RA's `command_event_save_auto_state()` early-returned at the savestate-support check (reading 0 from a zero-initialized `core_info_t`) and `.state.auto` files never got written despite `savestate_auto_save = "true"` being honored. SRAM in-game saves were unaffected because they go through a different code path.
- **Defensive WiFi credential wipe on cloned Pis**: `first_boot.sh` Step 6c removes `/etc/NetworkManager/system-connections/*.nmconnection` so cloned Pis don't auto-attempt to join the source operator's home network. New operator joins via the kiosk's Wi-Fi setup UI in ~30 seconds.

### Changed
- **Library screen matches Browse screen**: 9-column grid (was 5), focus-only border (was every-poster gold border), left-aligned titles (was centered with marker-zone reservation), no blinking ◂ cursor (the focused border + accent-color title carry that signal). Corner status dot (green = good file, gold = upgrade available, red = missing) preserved. Net effect: Library and Browse feel like the same UI; visual clutter dropped, focus indicator is unmissable.
- **Modern TV mode shows more rows**: main playlist list (8 → up to 14), settings menu items (7 → up to 10), game-browser playlist (8 → up to 11). Each call site derives `max_visible` from `height_ + item_height` and floors at the original CRT-tuned value so CRT layout is byte-identical. Renderer publishes the on-screen row count back to the input handler so scrolling only kicks in when the selection actually goes off-screen — previously you'd press DOWN onto "Back" and the top row would disappear despite obviously fitting.
- **Audio loudnorm parameters match Retro Ripper**: `I=-23:LRA=7:tp=-2` → `I=-16:TP=-1:LRA=11`. See "Audio normalization in upload pipeline" in Added for full rationale.
- **PS-pad button mapping (operator preference)**: Cross now SELECT, Circle now MENU. Square is unassigned. Reflects the user-facing remap landed during Phase 2 testing — locked in before Phase 2 sign-off.
- **Per-Pi reset on cloned-Pi first boot now also wipes** `services/.env`, `services/config/{radarr,prowlarr,qbittorrent,gluetun,flaresolverr}/*`, the `media_browser_unlocked` flag in `settings.json`, and (now) WiFi credentials. Operator restores fresh per-Pi state via the Content Manager UI on the cloned Pi (drops in a new WireGuard config; `setup_services.sh` rebuilds everything else from the codified fixtures in `scripts/data/`).
- **`deploy_cpp.sh` now syncs the top-level `scripts/golden_image/` directory**. Previously these scripts (`first_boot.sh`, `prepare_for_cloning.sh`, `restore_after_cloning.sh`, `prepare_golden_image.sh`) lived outside `magic_dingus_box_cpp/` and silently drifted between repo and Pi over time. Caught during the pre-clone audit when the Pi turned out to have an outdated `first_boot.sh` that would have leaked the source's VPN credentials and Media Browser unlock state to every clone.
- **Settings menu MENU button = "back one level"**: previously always closed the menu entirely; now treats MENU as a back-stack pop, only closing when at the top level. Mirrors how operators expect nested settings UIs to behave.

### Fixed
- **QR code encodes Wi-Fi IP instead of usb0 gadget IP** (the headline bug operators kept hitting): `WifiManager::get_ip_address()` was running `hostname -I` and taking the first token. On a Pi with the USB-Gadget service running, `usb0`'s static `10.55.0.1` consistently lists before `wlan0`'s real Wi-Fi IP, so the "Wi-Fi URL" displayed in the Content Manager submenu (and encoded in the QR) was secretly the USB IP. Replaced with `getifaddrs()` + explicit `wlan0` lookup.
- **VPN port forwarding (NAT-PMP) silently broken** for the entire stack: `FIREWALL_OUTBOUND_SUBNETS=192.168.0.0/16,10.0.0.0/8,172.16.0.0/12` looked correct but secretly shadowed ProtonVPN's `10.2.0.1` WireGuard gateway — Gluetun's iptables routed NAT-PMP requests out the LAN interface instead of through the tunnel, and the port-forwarding service gave up after 9 retries (`read udp …:5351: i/o timeout`). Result: dashboard read "VPN port unavailable" forever, qBit was firewalled, torrents downloaded slowly. Narrowed `FIREWALL_OUTBOUND_SUBNETS` to specific `/24` and `/16` LAN subnets that don't include `10.2.0.0/16`. Verified live: gluetun obtains forwarded port 38764 within 1 sec of restart, qbit-port-sync picks it up, dashboard flips to "✓ synced".
- **Two-player not working**: launcher emitted only `input_player1_*` bindings. With autoconfig disabled (we delete the file to force manual mappings for predictable behavior across DragonRise/Microntek pads), a 2nd identical pad showed up at `/dev/input/js1` but generated no in-game effect. See "Two-player support across all 7 cores" in Added for the fix.
- **Modern TV settings menu would scroll prematurely**: `move_selection()` had a hardcoded `max_visible_items=7` that didn't match the renderer's now-dynamic row count. Selecting "Back" with 9 rows visible would still bump scroll_offset to 1 and pop "Video Games" off the top. Renderer now publishes `max_visible_items` back to the input handler.
- **QR code shows wrong USB URL** (legacy bug): hardcoded to `http://192.168.7.1:5000`. Now reads live `usb0` IPv4 via `getifaddrs`.
- **`usb0` carrier check was IPv4-only**: settings menu marked usb0 "active" any time the gadget service had assigned `10.55.0.1`, even with no cable plugged in. Now requires `IFF_RUNNING` flag in addition to IPv4 presence.
- **Content Manager submenu QR could go stale**: now auto-rebuilds once per second while open so the QR + label reflect carrier-state changes (cable plugged/unplugged) without requiring the user to navigate out and back in.
- **Wi-Fi scan returned only 1 network**: `nmcli --rescan yes` doesn't actually wait. Fixed with explicit `nmcli dev wifi rescan` + 4-second sleep + plain `list` query.
- **Wi-Fi Disconnect was a no-op**: the SELECT handler re-ran `enter_submenu(WIFI)` after the action lambda flipped `wifi_disconnect_confirm_=true`, immediately resetting the flag back to false. See "Idempotent setting-menu re-entry" in Added for the structural fix.
- **Movie playback aspect ratio was stretched**: `gst_renderer.cpp` ignored source aspect — widescreen movies looked vertically stretched on a 16:9 TV. Now letterboxes 2.35:1 / 2.39:1 movies, pillarboxes 4:3 content, handles anamorphic DVDs correctly via pixel-aspect-ratio extraction from GStreamer caps.
- **Bezel disappearing after RetroArch exit**: removed a stale `static loaded_bezel_path` tracker in the main render loop that blocked `load_bezel()` from being called after `reset_gl()` destroyed the bezel texture. The renderer's own dedupe handles repeat calls correctly.
- **Bezel obscuring RetroArch in-game menu**: set `input_overlay_hide_in_menu = "true"` so the bezel auto-hides when the RetroArch menu opens (Z+Start hotkey).
- **Loading screen z-order**: the brief "Loading..." overlay during the kiosk → RetroArch handoff now renders the bezel on top of the loading text instead of the loading text covering the bezel — continuous visual framing through the transition.
- **Phase 0 test #33 (video playlist linkage) false-failed** on newly-uploaded videos because the test cd'd to `$PI_INSTALL_ROOT` instead of `$PI_INSTALL_ROOT/data`, so playlist-relative paths like `media/foo.mp4` didn't resolve.
- **Reorder warning** in `SettingsMenuManager` constructor init list (initialized fields out of declaration order — compiler initializes in declaration order regardless, so a mismatched init list is misleading at best, footgun at worst).

### Improved
- **PS1 core (`pcsx_rearmed`) tuning**: SPU reverb disabled, SPU interpolation off — small CPU savings with no audible impact on most titles. Combined with the global `video_threaded = true` (was false), notably smoother on Pi 4B.
- **RetroArch launcher refactor**: extracted duplicated video-config blocks from `launch_drm()` and `open_core_downloader_direct()` into a single `write_video_config(stream, opts)` helper that branches on `display_mode`. Per-core controller mapping logic split into `get_mapping_n64_adapter()` and `get_mapping_ps_style()` with a `get_mapping(type, core_name)` dispatcher.
- **RetroArch autoconfig file** now matches whichever controller is detected (`0e6d_111d.cfg` for N64, `0079_0006.cfg` for PS-style, none for UNKNOWN).
- **"or visit magicpi.local on any device"** hint added below the Content Manager QR for users without a phone camera handy.

### Pre-image testing pass
- All 7 cores launched, played, exited cleanly: NES, SNES, Genesis, PS1 (incl. 2-player Twisted Metal 4), PC Engine, Atari 7800, Arcade
- All 4 video bezels (1974, 1986, KV19, plus a Vintage TV) cycled correctly in Modern TV mode
- Content Manager web admin drag-and-drop verified for both video and game playlists
- Media Browser end-to-end: TMDB browse → search → add to library → indexer search → grab → download via VPN tunnel → playback with auto-pause → confirm-remove orphan-proof cleanup
- Pre-image checklist [tests/manual/pre_image_checklist.md](tests/manual/pre_image_checklist.md) updated with sign-off notes for Phases 0-4, 8-10
- Phase 5 (CRT mode) deferred until a CRT TV is available — code path validated via mode round-trip but visual verification pending
- Phase 6 (15-min PS1 endurance) optional, not blocking

## [1.3.0] - 2026-02-22

### Added
- 148 curated games across 7 systems: Arcade (16), Atari 7800 (20), NES (20), PC Engine (15), SNES (20), Sega Genesis (20), PlayStation 1 (34), with cover art thumbnails
- Multi-disc PS1 support via .m3u files (Final Fantasy VII, Metal Gear Solid, Gran Turismo 2, Resident Evil 2)
- D-pad hold-to-repeat for accelerated menu navigation
- Analog stick-to-D-pad axis mappings for SNES, Genesis, Atari 7800, and PC Engine cores

### Fixed
- **Critical:** OTA updates now preserve game saves (`data/saves/`, `data/states/`) during install, rollback, and backup
- RetroArch shell escaping uses single-quote wrapping (fixes games with apostrophes like "Tony Hawk's Pro Skater")
- Intro video no longer cuts off early
- Controller hotkey combo (Z + Start) restored for RetroArch menu toggle
- WiFi connection timeout (30s) with specific error messages instead of hanging
- WiFi shell injection eliminated (fork/execvp for nmcli commands)
- Input device last-resort recovery after RetroArch exits
- Playlist loader validates emulated_game items at load time (skips entries missing core or path)
- Empty ROM path validation before RetroArch launch
- File descriptor close limit uses sysconf() instead of hardcoded 256
- Removed dead keepalive process cleanup code and duplicate config writes in RetroArch launcher
- Removed broken Double Dragon and Metal Slug from arcade playlist

### Improved
- Master Shuffle uses GL-drawn crossed-arrows icon instead of text marker
- Now-playing playlist shown with green text instead of dot indicator
- Playlists numbered from 01 after Master Shuffle entry
- Code quality: security, performance, and reliability hardening pass

## [1.2.0] - 2026-02-21

### Added
- Systemd watchdog with automatic crash recovery (Type=notify, 10s watchdog)
- Now-playing indicator (accent dot) on current playlist in playlist list
- Error overlay banner for game launch failures (4-second auto-dismiss)
- Master shuffle history with "Previous" support (up to 10 entries)
- EOS callback infrastructure for GStreamer end-of-stream events

### Fixed
- **Security:** Shell injection in WiFi manager (SSID/password via fork/execvp instead of popen)
- **Security:** Tightened delete_media path validation to media directories only
- Atomic settings file write (temp file + rename) prevents corruption on crash
- Missing settings file no longer treated as error on first run
- GStreamer play() no longer blocks forever (3-second timeout replaces infinite wait)
- RetroArch uses isolated temp config (/tmp/retroarch_mdb.cfg) instead of overwriting user config
- Removed exit(0) in core downloader that killed the entire application
- Hardcoded XDG_RUNTIME_DIR replaced with dynamic getuid() in RetroArch launcher
- Replaced std::system("chmod") with POSIX chmod() calls
- Virtual keyboard text centering uses actual font metrics
- format_time() handles negative seconds gracefully
- Game scroll offset resets correctly when switching playlists
- WiFi saved networks reconnect without prompting for password
- WiFi disconnect requires two-press confirmation
- Removed dead "Controllers" menu link from Games submenu
- Removed const_cast antipattern in settings menu (uses mutable members)
- Analog stick auto-repeat fires immediately on direction change
- Playlist switching timeout reduced from 5s to 2s
- Watchdog paused during RetroArch gameplay to prevent false kills

### Improved
- RGB mask CRT shader uses smoothstep-based subpixel column darkening
- WiFi scan results cross-reference saved connections for accurate status

## [1.1.0] - 2026-02-08

### Added
- Rotary encoder video seeking with velocity-sensitive progress bar
- Playlist package import/export (ZIP with videos + playlist YAML)
- Playlist deletion with orphaned video cleanup
- Alphabetical playlist sorting on device

### Fixed
- Audio output setting (HDMI/Headphone) now persists across reboots
- Audio output configured before intro video plays (no mid-intro switching)
- Upload size limited to 8GB default (prevents storage exhaustion)
- ZIP extraction size limited to 10GB (prevents ZIP bomb attacks)
- XSS vulnerability in notification messages
- Playlist existence check now runs before extracting videos in package import

### Improved
- Content Manager UI with drag-and-drop playlist management
- YAML formatting handles non-string types safely
- Deploy script uses configurable paths instead of hardcoded values

## [1.0.7] - 2026-01-18

### Fixed
- OTA rollback functions no longer stop web service (same fix as 1.0.6 for main install)
- Rollback rsync now uses --no-group --no-owner to avoid permission errors
- Rollback handles rsync exit codes 23/24 gracefully

## [1.0.6] - 2026-01-18

### Fixed
- OTA update now works reliably - web service stays running during update
- Only C++ app is stopped during update, web service restarts at the end

## [1.0.5] - 2026-01-18

### Fixed
- OTA update subprocess now survives when web service is stopped during update

## [1.0.4] - 2026-01-18

### Changed
- Test release to verify OTA update system works end-to-end

## [1.0.3] - 2026-01-18

### Fixed
- OTA update rsync permission errors (added --no-group --no-owner flags)
- OTA update content directory detection for release tarballs
- Handle rsync exit codes 23/24 gracefully

## [1.0.2] - 2026-01-18

### Fixed
- OTA update backup directory permissions (now uses user-writable location)

## [1.0.1] - 2026-01-18

### Added
- Audio output selection (HDMI, Headphone Jack, Auto) via PulseAudio
- Game volume offset control for RetroArch (-3dB, -6dB, -12dB options)
- Audio submenu in Settings for easy audio configuration
- Audio settings persistence (saved to config file)

### Improved
- Video transcoding now normalizes audio to -23 LUFS (EBU R128 broadcast standard)
- Consistent volume levels across all transcoded videos
- System volume control via ALSA Master/PCM mixers

## [1.0.0] - 2026-01-16

### Added
- Initial stable release
- C++ kiosk engine with DRM/KMS for true kiosk mode
- Video playback via GStreamer with GL texture rendering
- RetroArch integration for NES, N64, and PS1 emulation
- Web admin interface for remote content management
- Playlist management with YAML format
- Video transcoding with CRT (640x480) and modern (720p) presets
- Smart upload with automatic transcode detection
- USB Ethernet Gadget support for fast content transfers
- WiFi configuration via on-screen menu
- QR code display for easy web admin access
- USB/WiFi network detection with priority handling
- M3U playlist generation for multi-disc PS1 games
- Backup and restore functionality
- Over-the-air (OTA) update system via GitHub Releases

### Technical Details
- OpenGL ES 3.0 rendering with immediate-mode 2D UI
- evdev input handling for controllers and keyboards
- stb_truetype font rasterization
- YAML-based playlist and settings persistence
- Flask-based REST API for web admin
- Systemd service integration for auto-start
