# OTA Update Guarantees

This document is a contract with operators: **what does an over-the-air (OTA) update actually do to my Magic Dingus Box?**

It's also a contract with future contributors: **the rsync `--exclude` lists in [`magic_dingus_box_cpp/scripts/update.sh`](magic_dingus_box_cpp/scripts/update.sh) must continue to honor every entry in the "preserved" table below.** If you're editing those rsync calls, read this file first.

The guarantees here apply to OTA updates from any prior version (v1.0.x / v1.1.0 / v1.2.0 / v1.3.0 / v1.4.x / v1.5.x) to any subsequent version. They do NOT apply to a fresh SD-card flash from a golden image — that's a different code path (`first_boot.sh`).

## What gets UPDATED on every OTA

These flow through from the GitHub release tarball, replacing whatever was on the Pi:

| Path | Why |
|---|---|
| `magic_dingus_box_cpp/src/**` | Kiosk C++ source. Rebuilt on-Pi via `cmake .. && make -j2` after rsync. |
| `magic_dingus_box_cpp/scripts/**` (incl. `update.sh`, `deploy_cpp.sh`, `setup_services.sh`, `kiosk_standby_watcher.sh`) | All shipping scripts. |
| `scripts/golden_image/**` (`first_boot.sh`, `prepare_for_cloning.sh`, `restore_after_cloning.sh`) | Clone tooling. Updated on the Pi so future re-clones from a customer Pi (rare but supported) use current logic. |
| `magic_dingus_box_cpp/assets/**` (bezels, fonts, logos, Marquee wood-frame) | Visual assets. Bezel updates and Marquee wood-frame revisions (`assets/marquee/marquee_frame.png`) flow through cleanly. |
| `magic_dingus_box_cpp/data/intro/intro.30fps.mov` | Boot intro video. |
| `magic_dingus_box_cpp/data/thumbnails/systems/*.png` | The 7 system-tile thumbnails (arcade/atari7800/genesis/nes/pcengine/ps1/snes). Per-system dirs (e.g. `data/thumbnails/arcade/`) are NOT updated — see preserved table. |
| `magic_dingus_box_cpp/scripts/data/*.json` | Codified Radarr/Prowlarr/qBit fixtures. Future Media Browser re-provisioning uses current scoring rules, indexer URLs, etc. |
| `services/docker-compose.yml` | Includes the `FIREWALL_OUTBOUND_SUBNETS` narrowed-subnet fix that makes ProtonVPN NAT-PMP work; future critical fixes here flow through automatically. |
| `magic_dingus_box_cpp/systemd/**` (unit files) | Reinstalled and `systemctl daemon-reload`'d after rsync. |
| `CMakeLists.txt`, top-level build configs | Used during the on-Pi rebuild. |
| `VERSION`, `CHANGELOG.md` | Updated metadata. |

## What's PRESERVED — the contract

These paths are explicitly excluded from update.sh's rsync (`--exclude` list). **Adding new categories of operator content?** Edit the rsync exclude lists in `update.sh` (4 occurrences: backup, install, internal rollback, user-initiated rollback) AND add an entry below.

| Path | Why preserved | What lives here |
|---|---|---|
| `magic_dingus_box_cpp/data/media/*` | Operator-uploaded videos. NOT in git. | Sacred Steel clips, curator-uploaded music videos, anything the operator uploaded via the Content Manager. |
| `magic_dingus_box_cpp/data/playlists/*` | Operator may have customized default playlists OR added their own. | The 11 default `*.yaml` playlists ship in git, but updating them via OTA would clobber operator edits. Default playlist YAMLs live in `data/playlists/` post-flash and stay frozen at whatever version was on the SD when flashed. |
| `magic_dingus_box_cpp/data/roms/*` | Operator's ROM library. Gitignored (copyright). | Per-system ROM files. |
| `magic_dingus_box_cpp/data/thumbnails/{arcade,atari7800,genesis,n64,nes,pcengine,ps1,snes}/*` | Per-game cover art. Gitignored. Populated by `deploy_cpp.sh` from the operator's local thumbnails folder during the golden-image build. | ~157 game-cover PNGs. |
| `magic_dingus_box_cpp/data/saves/*` | Game SRAM saves (Zelda character, etc.). | Per-core SRAM files. |
| `magic_dingus_box_cpp/data/states/*` | RetroArch auto-resume save states. | `<rom>.state.auto` per game. |
| `magic_dingus_box_cpp/data/device_info.json` | Per-Pi device identity (UUID, hostname). | Generated at first boot; stable across the Pi's life. |
| `config/*` | Kiosk settings (display mode, audio, bezel selection, master volume, Media Browser unlock flag). Plus WiFi credentials in NetworkManager. | `config/settings.json`. |
| `magic_dingus_box_cpp/build/*` | Local build artifacts. Always rebuilt fresh during install. | CMake cache, object files, the kiosk binary. |
| `services/.env` | Per-Pi Media Browser secrets. NOT in git. | WireGuard private key, ProtonVPN credentials, auto-generated Radarr/Prowlarr/qBit API keys, qBit admin password. |
| `services/config/*` | Per-Pi Media Browser stack runtime state. NOT in git. | Radarr library DB, Prowlarr indexer sync history, qBit fastresume + cookies, Gluetun VPN runtime state, FlareSolverr state. |

## v1.6.3 addition — Library overlay sort + filter survive OTA cleanly

v1.6.3 adds two new persisted fields to `config/settings.json` that drive the new Library slide-in overlay's sort and filter dimensions:

- **`display.mb_library_sort`** (string: `"recent"` / `"title"` / `"year"` / `"size"`) — the operator's chosen sort order for the Library grid. Defaults to `"recent"` on a fresh install.
- **`display.mb_library_filter`** (string: `"all"` / `"unwatched"` / `"missing_files"` / `"recently_added"`) — the operator's chosen filter. Defaults to `"all"` on a fresh install.

Both keys live under `config/*` which is in the `update.sh` rsync exclude list, so OTA preserves them across upgrades exactly like every other display setting. On the FIRST OTA where these keys are absent from the operator's existing `settings.json`, the load path uses the JSON `.get(key, default)` fallback to apply the canonical defaults (`"recent"` / `"all"`) — no visual regression for an operator upgrading from a build that pre-dates the keys.

The `Unwatched` filter is a deliberate placeholder until watched-history tracking lands. Selecting it persists the choice but the kiosk's `LibraryScreen::rebuild_view()` treats it as a no-op (keeps all rows) until the watched-history feature lands. Operators who pick "Unwatched" today get the same view as "All" — when watched-history ships in a later release, the same setting will start filtering correctly without requiring the operator to re-pick.

The new `Queue` tab + `BTN2 = back` input grammar are pure code changes (no new persisted state); they propagate via the standard `magic_dingus_box_cpp/src/**` rsync. The on-Pi `cmake .. && make -j2` step inside `update.sh install` rebuilds the kiosk binary with them. No new build dependencies, no new asset files, no service-side changes.

## v1.6.2 addition — Marquee CRT-overlay store + wood-frame toggle survive OTA cleanly

v1.6.2 adds an independent CRT-overlay intensity store for the Media Browser menu screens (separate from the kiosk's home-menu CRT settings) and a wood-frame visibility toggle for movie playback. The OTA contract for these is:

- **New `display.mb_*` keys in `config/settings.json`**:
  - `display.mb_playback_show_frame` (bool, default `true`) — controls whether the wood-frame overlay stays visible during playback.
  - `display.mb_scanline_intensity` / `mb_warmth_intensity` / `mb_glow_intensity` / `mb_rgb_mask_intensity` / `mb_bloom_intensity` / `mb_interlacing_intensity` / `mb_flicker_intensity` (floats, 0.0–1.0) — the Marquee menu CRT overlay stack. Cycled OFF / Low / Medium / High via `MovieSettings → "CRT overlay"` rows.
  - These all live under `config/*` which is in the `update.sh` rsync exclude list, so OTA preserves them across upgrades exactly like every other display setting.
  - On the FIRST OTA where these keys are absent from the operator's existing `settings.json`, the kiosk's load path falls back to inheriting the corresponding home-menu values (so an operator with scanlines at 0.5 on their home menu sees scanlines at 0.5 on the Marquee menus the first frame after upgrade — no visual regression). After the operator changes any value through MovieSettings, the divergence persists.
- **Code** — fully shipped via the standard rsync of `magic_dingus_box_cpp/src/**`. No new build dependencies. The `gst_renderer::set_render_inset()` API and the fill-width pillarbox elimination for Marquee playback are pure code; the on-Pi `cmake .. && make -j2` step inside `update.sh install` rebuilds the kiosk binary with them.
- **Wood-frame asset replaced** — `magic_dingus_box_cpp/assets/marquee/marquee_frame.png` is updated to a polished mahogany variant. Flows through the standard `assets/**` rsync. Operators see the new frame on the next kiosk start after OTA.
- **systemd unit gains an `EnvironmentFile=` line** — the kiosk unit (`magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service`) now declares `EnvironmentFile=-/opt/magic_dingus_box/services/.env` so the kiosk process inherits API keys from the codified Docker stack's `.env`. The `-` prefix makes the load optional, so unprovisioned Pis (no `services/.env` yet) still boot the kiosk cleanly. Propagated via the standard `systemd/**` rsync; `daemon-reload` runs as part of `update.sh install` so the new unit is in effect on the next service restart.
- **No new offscreen-state preservation needed** — the Marquee CRT effects use the existing legacy procedural overlay path (`render_crt_effects`); they don't add any new GPU resources. The wood-frame texture is lazily reloaded by the existing `load_marquee_frame()` path, idempotent across OTA-rebuilds.
- **Reversion is a settings flip, not a downgrade** — operators who don't want the new behaviors can turn off the wood frame during playback (`MovieSettings → Library → "Wood frame during playback" = Off`) and zero out the CRT overlay intensities. No file restoration, no rebuild, no OTA rollback necessary. The `v1.6.1` git tag remains the closest revert point if a hard rollback is ever needed at the source level.

## v1.5.0 addition — the Enhanced CRT pipeline survives OTA cleanly

v1.5.0 introduces an opt-in Enhanced CRT shader pipeline that lives entirely inside two existing kiosk source files (`magic_dingus_box_cpp/src/ui/renderer.{h,cpp}`) and one new flag in `config/settings.json` (`display.enhanced_crt_enabled`). The OTA contract for this is:

- **Code** — fully shipped via the standard rsync of `magic_dingus_box_cpp/src/**`. No new build dependencies, no new asset files, no service-side changes. A v1.4.x Pi → v1.5.0 OTA gets the new shader code transparently and the on-Pi `cmake .. && make -j2` step inside `update.sh install` rebuilds the kiosk binary with it.
- **Operator preference** — the `enhanced_crt_enabled` flag and all 7 effect intensities are persisted in `config/settings.json`, which is in update.sh's exclude list (see "What's PRESERVED — the contract" above, `config/*` row). So an operator who has flipped Enhanced ON keeps it on after OTA; an operator who left it OFF (the default) keeps the v1.4.3 procedural-overlay look until they choose to opt in.
- **No new offscreen-state preservation needed** — the scene FBO (`scene_fbo_`) and bloom FBOs (`bloom_a_fbo_`, `bloom_b_fbo_`) are GPU-side resources lazily recreated on every kiosk start. They are not files on disk; nothing about OTA touches them. The `reset_gl()` path that handles RetroArch handoff also handles them correctly via `destroy_scene_fbo()` / `destroy_bloom_fbos()` followed by lazy reconstruction on the next active frame.
- **Reversion is a settings flip, not a downgrade** — if the new look is unwanted, the operator toggles "CRT Engine: Classic" in Settings → Display, which sets `enhanced_crt_enabled = false` and restores byte-identical v1.4.3 rendering. No file restoration, no rebuild, no OTA rollback necessary. The `v1.4.3-pre-crt-rework` git tag is the absolute revert point if a hard rollback is ever needed at the source level.

## Specifically: things operators worry about

| Question | Answer |
|---|---|
| "Will my uploaded videos be deleted?" | No. `data/media/*` is excluded. Also gitignored, so the release tarball doesn't even contain default video content — there's nothing for OTA to push out. |
| "Will my ROMs be deleted?" | No. `data/roms/*` is excluded and gitignored. |
| "Will my game saves be lost?" | No. `data/saves/*` and `data/states/*` are excluded. |
| "Will my WiFi password be wiped?" | No. WiFi profiles live at `/etc/NetworkManager/system-connections/` which is outside `INSTALL_DIR` entirely; OTA's rsync never touches it. |
| "Will I lose my Media Browser VPN config?" | No, as of v1.4.3. `services/.env` is preserved. Pre-v1.4.3 OTA would have wiped this — this was the main bug fixing motivation for v1.4.3. |
| "Will I lose my Radarr movie library?" | No, as of v1.4.3. `services/config/*` is preserved. Pre-v1.4.3 OTA would have wiped Radarr DB, Prowlarr indexer state, qBit history. |
| "Will my game cover art disappear?" | No, as of v1.4.3. `data/thumbnails/{system}/*` per-system dirs are preserved. |
| "Will my display settings (CRT vs Modern TV, bezel selection) reset?" | No. `config/*` is excluded. |
| "Will my CRT Engine choice (Classic vs Enhanced) reset?" | No, as of v1.5.0. The `display.enhanced_crt_enabled` flag is part of `config/settings.json` and inherits the same `config/*` exclusion. Operators who opted into the Enhanced shader pipeline keep it across updates; operators on Classic stay on Classic. |
| "Will my hostname change?" | No. `data/device_info.json` is excluded. |
| "Will OTA push default videos onto my Pi?" | No. Default videos aren't in git, so the release tarball doesn't carry them. OTA literally cannot re-push them. |
| "Will OTA modify my customized playlists?" | No. `data/playlists/*` is excluded — if you've edited a default playlist YAML, your edits stay. Side effect: you also won't get curator updates to default playlists via OTA — that's the deliberate trade-off. |

## How update.sh actually does this

The full preservation list lives in 4 rsync invocations inside `magic_dingus_box_cpp/scripts/update.sh`:

1. **Backup creation** (`create_backup` / line ~400): excludes the same paths so the backup doesn't blow up on disk space (no point round-tripping multi-GB ROMs).
2. **Install** (`install_update` / line ~480): the main rsync that lays the new release down over the install dir. All exclude entries here are critical.
3. **Internal rollback** (`rollback_internal` / line ~620): triggered when an install fails partway. Uses the same exclude list so a rollback doesn't undo operator content the install didn't touch.
4. **User-initiated rollback** (`rollback` / line ~680): same logic, same exclude list.

Adding a new "this should be preserved" path? Update **all four** lists. Inconsistency between them is a recipe for partial-update data loss.

## The version-detection path

`update.sh check` queries `https://api.github.com/repos/a-train-chain/magic_dingus_box/releases/latest`. **Tags alone don't trigger updates** — you need a published GitHub *Release* (via `gh release create v1.X.Y --notes ...` or the GitHub web UI) for OTA to see it.

Releases for v1.0.0–v1.0.17 and v1.1.0–v1.3.0 are published. v1.4.0 and v1.4.1 are git tags only (intentional — they were stepping stones during the v1.4.x release cycle). v1.4.2 onwards: every patch tagged in git also gets a GitHub Release.

A Pi running an older version that runs OTA will see whatever is `/releases/latest` — currently **v1.6.3** — and jump straight there. No multi-hop sequencing required.

## Testing the contract before shipping a new release

Before publishing a release that touches any rsync exclude list (or adds new operator-content paths), verify on a real Pi:

```bash
# 1. Note current state of operator content
ssh magic@PI_HOST '
  ls /opt/magic_dingus_box/magic_dingus_box_cpp/data/playlists/ | wc -l
  ls /opt/magic_dingus_box/magic_dingus_box_cpp/data/saves/ | head -3
  cat /opt/magic_dingus_box/services/.env | head -1   # should NOT be empty
  cat /opt/magic_dingus_box/config/settings.json | jq .display.mode
'

# 2. Trigger update
ssh magic@PI_HOST 'cd /opt/magic_dingus_box && \
    ./magic_dingus_box_cpp/scripts/update.sh install <version> <download_url>'

# 3. Verify the same paths are intact
ssh magic@PI_HOST '
  ls /opt/magic_dingus_box/magic_dingus_box_cpp/data/playlists/ | wc -l   # same count
  ls /opt/magic_dingus_box/magic_dingus_box_cpp/data/saves/ | head -3      # same files
  cat /opt/magic_dingus_box/services/.env | head -1                         # still populated
  cat /opt/magic_dingus_box/config/settings.json | jq .display.mode         # same value
  cat /opt/magic_dingus_box/VERSION                                          # NEW version
'
```

If anything in the "before" snapshot doesn't survive intact in the "after" snapshot, the OTA contract is broken — fix the rsync exclude lists before publishing.
