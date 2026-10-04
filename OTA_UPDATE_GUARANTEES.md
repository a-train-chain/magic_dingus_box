# OTA Update Guarantees

This document is a contract with operators: **what does an over-the-air (OTA) update actually do to my Magic Dingus Box?**

It's also a contract with future contributors: **the rsync `--exclude` lists in [`magic_dingus_box_cpp/scripts/update.sh`](magic_dingus_box_cpp/scripts/update.sh) must continue to honor every entry in the "preserved" table below.** If you're editing those rsync calls, read this file first.

The guarantees here apply to OTA updates from any prior version (v1.0.x / v1.1.0 / v1.2.0 / v1.3.0 / v1.4.x / v1.5.x) to any subsequent version. They do NOT apply to a fresh SD-card flash from a golden image — that's a different code path (`first_boot.sh`).

## What gets UPDATED on every OTA

These flow through from the GitHub release tarball, replacing whatever was on the Pi:

| Path | Why |
|---|---|
| `magic_dingus_box_cpp/src/**` | Kiosk C++ source. Rebuilt on-Pi (in `build.new/`, at `-j1` or `-j2` per `build_memory_plan`) after rsync when no usable pre-compiled binary exists — see "The TV stays on while a source build compiles". |
| `magic_dingus_box_cpp/scripts/**` (incl. `update.sh`, `deploy_cpp.sh`, `setup_services.sh`, `kiosk_standby_watcher.sh`) | All shipping scripts. |
| `scripts/golden_image/**` (`first_boot.sh`, `prepare_for_cloning.sh`, `restore_after_cloning.sh`) | Clone tooling. Updated on the Pi so future re-clones from a customer Pi (rare but supported) use current logic. |
| `magic_dingus_box_cpp/assets/**` (bezels, fonts, logos, Marquee wood-frame) | Visual assets. Bezel updates and Marquee wood-frame revisions (`assets/marquee/marquee_frame.png`) flow through cleanly. |
| `magic_dingus_box_cpp/data/intro/intro.30fps.mov` | Boot intro video. |
| `magic_dingus_box_cpp/data/thumbnails/systems/*.png` | The system-tile thumbnails. Delivered via an rsync `--include 'data/thumbnails/systems/***'` that precedes the blanket `data/thumbnails/*` exclude (before 2026-07-30 the exclude blocked these, contradicting this table). Per-system dirs (e.g. `data/thumbnails/arcade/`) are NOT updated — see preserved table. |
| NEW files in `magic_dingus_box_cpp/data/playlists/` | **Add-only** playlist sync (2026-07-30): after the main rsync, a `*.yaml` in the release that does not exist on the box is copied in — so a new console's default playlist reaches existing boxes. Two gates: (1) **same-system dedupe** — skipped if any existing playlist covers the same `emulator_system` (pre-`games_*`-era boxes have `arcade.yaml` etc.; verified their normalized system values match the new set, so no duplicate menu rows); (2) **content existence** — skipped unless at least one item is playable on the box (youtube item, or a local path that exists), so a new console's playlist waits until its ROMs arrive. Playlists that already exist on the box are never touched (operator edits win). Trade-off: a default playlist the operator deleted comes back on the next OTA while its content is present. |
| RetroArch cores (post-install bootstrap) | Cores are binary `.so` files, NOT in the tarball. After install, update.sh scans the box's live playlists for every referenced `emulator_core` and runs `install_cores.sh` (apt + christianhaitian's aarch64 repo) if any is missing from `~/.config/retroarch/cores`. Added 2026-07-30 — before this, a release adding N64/Dreamcast would have shipped code that references cores no fielded box had. |
| `magic_dingus_box_cpp/scripts/data/*.json` | Codified Radarr/Sonarr/Prowlarr/qBit fixtures. The Custom Format fixtures are also *applied* on every OTA — see the converge row below. The remaining fixtures (indexers, Byparr proxy, Apps integration, download client, quality definitions, qBit category) are still applied only by a provisioning run. |
| **Radarr + Sonarr Custom Formats and both `Any` profile score maps** (converge step) | **As of v1.9.14.** `update.sh` invokes `magic_dingus_box_cpp/scripts/converge_custom_formats.sh` as root on every OTA — the same script `setup_services.sh` calls at provisioning time, so the two cannot drift. It reconciles every format in `scripts/data/{radarr,sonarr}_custom_formats.json` (create / update-if-drifted / leave-alone, matched by NAME) and reapplies both `SCORE_MAP`s plus `minFormatScore`, the cutoff and the allowed-quality tiers to each service's `Any` profile. Fully idempotent — a converged box makes zero writes. **Skips cleanly (exit 0) and never fails an OTA** when the box has no `services/.env`, no API key for a service, or the service does not answer its bounded readiness probe; a genuine failure is a logged warning, never an abort. Before this, shipping a Custom Format change delivered the fixture JSON to every box and reconciled it into *none* of them — the box kept downloading against the old rules with the new rules sitting on disk beside them (found 2026-08-13, one release after the English-audio formats were added). |
| `services/docker-compose.yml` | Includes the `FIREWALL_OUTBOUND_SUBNETS` narrowed-subnet fix that makes ProtonVPN NAT-PMP work; future critical fixes here flow through automatically. **True only as of v1.9.7** — see the note below. |

> **v1.9.7 correction — `services/docker-compose.yml` was DELETED, not updated.**
> Boxes keep this file at the flattened path `/opt/magic_dingus_box/services/docker-compose.yml`, but the repo only has it at `magic_dingus_box_cpp/services/`, and `deploy_cpp.sh` is what flattens it. The release tarball therefore contained no top-level `services/` directory, so the install `rsync --delete` **removed the file from every fielded box on its first OTA**. The `services/.env` and `services/config/*` excludes kept the *directory* alive, so nothing looked wrong until something ran `docker compose` — which then failed with `no configuration file provided: not found` (exit 14). Customer box `magicpi-dc8a` hit this on Media Browser → Reconfigure after 1.9.3 → 1.9.5 → 1.9.6; the same box would also have failed to bring the stack up on its next reboot (`magic-dingus-services.service` runs `docker compose up -d` from that directory).
>
> Fixed in three places, none of which is a new rsync exclude — an exclude would protect a stale compose file forever and break the guarantee above:
> 1. `.github/workflows/release.yml` stages a top-level `services/` (mirrored from `magic_dingus_box_cpp/services/`) into the tarball at package time, so the rsync *delivers* the file. This also repairs boxes still running the old `update.sh`, because their existing rsync simply finds the file in the tarball.
> 2. `update.sh` re-creates `services/docker-compose.yml` from the in-tree copy if it is absent after the install rsync.
> 3. `setup_services.sh` self-heals the same way immediately before `docker compose up`, and now names the missing file instead of leaving docker's opaque error to speak for itself.
>
> The four rsync exclude lists are unchanged by this fix.
| `/usr/local/bin/{playback_services_pause.sh, gluetun_cascade_restart.sh, clear_radarr_cooldowns.py, sync_qbit_password.sh, auto_blocklist_stuck_warnings.py, qbit-port-sync.sh}` and `/etc/dnsmasq.d/usb0.conf` | **Refreshed as of v1.9.8** (`refresh_out_of_tree_files` in `update.sh`). These are copies made *outside* `/opt` by `setup_services.sh` / `install_deps.sh` at provisioning time, and the install rsync only ever writes inside `INSTALL_DIR` — so before v1.9.8 they were frozen at image-cut time on every fielded box. Refresh only, never provision: each target is copied **only if it already exists**, so an OTA can never hand a box a helper it was not set up with. Skipped entirely in test mode (`MAGIC_SKIP_SYSTEMCTL=true`). |
| `CMakeLists.txt`, top-level build configs | Used during the on-Pi rebuild. |
| `CHANGELOG.md` | Updated metadata. |

### What does NOT flow through OTA — read this before promising a fix reaches the field

| Path | Reality |
|---|---|
| `/etc/systemd/system/*.service`, `*.timer` (the installed copies of `magic_dingus_box_cpp/systemd/**`) | **NOT reinstalled** — with ONE exception: `magic-dingus-ota-recovery.service` is installed/refreshed by `update.sh` itself (via `setup_ota_recovery.sh`) at the start of every install, because the update.sh that relies on it is the one that delivers it. Everything else: This table claimed otherwise until v1.9.8. `update.sh` rsyncs the unit *sources* into `/opt` and runs `systemctl daemon-reload`, but it never writes `/etc/systemd/system` — daemon-reload re-reads a directory the OTA never touched. Unit files on a fielded box are frozen at provisioning time. Harmless so far (the only unit changed between v1.9.3 and v1.9.7 was a comment), but **a unit-file change is not a shippable fix**: it reaches a box only via the Content Manager's Media Browser Configure/Reconfigure flow, which re-runs `setup_services.sh`. |
| `/etc/NetworkManager/**`, `/etc/sysctl.d/**`, `/etc/resolv.conf` | Not written by any rsync. Delivered by `setup_network_hardening.sh`, which `update.sh` *does* invoke as root on every OTA — that is the one supported route for network posture. |
| Anything else outside `/opt/magic_dingus_box` | Not touched, except the explicitly enumerated refresh row above. |
| Long-running services that execute tree code (e.g. `content-manager-redirect.service` → `scripts/content_manager_redirect.py`) | The new file lands, but the process keeps running the OLD code until something restarts it. The OTA restarts only `magic-dingus-web` and the kiosk — plus `gluetun-cascade-restart.service` when its `/usr/local/bin` script is newer than the running process (`restart_stale_cascade_watcher.sh`, run by `setup_memory_tuning.sh`, so it works even on the hop executed by an older `update.sh` that copied the script without restarting the watcher). |
| Docker containers | Not recreated by the OTA. A `docker-compose.yml` change applies at the next `compose up -d` — which `magic-dingus-services` runs on every boot — so compose-level fixes (e.g. per-container log rotation) arm on the box's next restart. `/etc/docker/daemon.json` is never written by OTA. |

**Reaching field boxes — the rule.** Boxes in the field update with the
`update.sh` they already have, not the one in the new release. That
version rsyncs the tree, then runs the NEW tree's
`setup_network_hardening.sh` and `setup_memory_tuning.sh` as root, then
`daemon-reload` and starts the kiosk. Those two scripts are therefore the
only root-level delivery path a fix has on the update that ships it. A
system-side fix (a unit setting, a service restart, anything under `/etc`)
must ride one of them — as a systemd drop-in rather than a unit-file edit,
idempotent, and never able to fail the script. Examples: the kiosk's
`TimeoutStopSec=20` is a drop-in written by `setup_memory_tuning.sh`; the
port-80 redirect is `try-restart`ed by `setup_network_hardening.sh`; the
PulseAudio unit (`magic-dingus-audio.service`) and the kiosk drop-in that
orders the kiosk after it are installed by `setup_audio_service.sh`, which
`setup_memory_tuning.sh` calls. The reverse direction: when a rollback
(internal, user-initiated or boot recovery) restores a tree without
`audio_service.sh`, `update.sh`'s `retire_audio_service_if_absent` removes
our marker-tagged `~/.config/pulse/client.conf` (`autospawn = no`), disables
and stops the audio unit and removes the kiosk drop-in, so the restored
release's PulseAudio autospawn recovery works again. Only rollbacks run by
this `update.sh` or a later one get this — a rollback to v1.9.14 performed
by v1.9.14's own updater leaves the `client.conf` behind.

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
| `magic_dingus_box_cpp/data/paired_remotes.json`, `data/flask_secret.key`, `data/pairing_session.json`, `data/pairing_audit.log` | Phone Remote pairing state. None are in git, so before these excludes were added (2026-07-30) the install rsync's `--delete` wiped them — **every paired phone was silently unpaired on every OTA**. | Paired-device records, the HMAC cookie-signing secret, in-flight pairing session, pairing audit log. |
| `magic_dingus_box_cpp/data/kiosk_status.json`, `data/text_input_queue.jsonl`, `data/seek_request.json` | Transient kiosk↔web runtime files; excluded so an OTA can't yank them out from under the running web admin. | Status broadcast, phone-remote text-input queue, seek requests. |
| `/config/*` (anchored to the install root) | Kiosk settings (display mode, audio, bezel selection, master volume, Media Browser unlock flag). Plus WiFi credentials in NetworkManager. | `config/settings.json`. |
| `config/controller_profiles.json` | Captured controller mappings from the Controller Setup wizard. Covered by the existing `/config/*` exclude; listed here so nobody "cleans it up". | Per-model button/axis profiles keyed by USB VID/PID. |
| `magic_dingus_box_cpp/build/*` | Local build artifacts. Always rebuilt fresh during install — **in `build.new/`, swapped in only after the new binary is verified** (see the 2026-10 section below); the live `build/` is never deleted first. | CMake cache, object files, the kiosk binary. |
| `magic_dingus_box_cpp/data/pending_revocations.txt`, `magic_dingus_box_cpp/data/upload_temp/` | Per-box runtime state, never in a release tarball, so the install `--delete` removed them on every OTA. Excluded from all four lists as of the 2026-10 hardening (`deploy_cpp.sh` already excluded both). | Phone-unpair revocations not yet applied; half-finished Content Manager uploads. |
| `services/.env` | Per-Pi Media Browser secrets. NOT in git. | WireGuard private key, ProtonVPN credentials, auto-generated Radarr/Prowlarr/qBit API keys, qBit admin password. |
| `services/config/*` | Per-Pi Media Browser stack runtime state. NOT in git. | Radarr library DB, Prowlarr indexer sync history, qBit fastresume + cookies, Gluetun VPN runtime state, FlareSolverr state. |
| `magic_dingus_box_cpp/data/media_browser.db*` | Media Browser watch state. NOT in git (`prepare_for_cloning.sh` deliberately wipes it, so it can never ship in a tarball). Excluded as of **v1.9.8** — before that, every OTA of every fielded box deleted it and `WatchStore` silently re-created an empty schema, so nothing errored and nothing warned. | Resume positions, watched/unwatched flags and NEW-badge state for movies **and** TV, plus the `-wal`/`-shm` sidecars. The library itself repopulates from Radarr/Sonarr; only the per-household viewing history was lost. |
| `/VERSION` (backup + install rsyncs only; anchored) | Not operator content — a control file. Excluded from the **install** rsync so the new version is stamped only after a verified kiosk start, and from the **backup** rsync so `$BACKUP_DIR/VERSION` can be written afterwards as a completion marker. Both rollback rsyncs deliberately keep it (a rollback must restore the old number) and also `cp` it explicitly. Pinned by `tests/local/update_rsync_excludes.bats`. | The single line of text the Content Manager reports as the box's version. |

## The TV stays on while a source build compiles (kiosk stopped only for the swap)

When no usable pre-compiled binary exists (none published, or it needs newer libraries than the OS has), `update.sh` compiles the kiosk on the box — 8-10 minutes at `-j2` on a Pi 4B. The kiosk used to be stopped **before** the rsync, so that whole compile was a black TV. Now the kiosk is stopped exactly once, by `stop_kiosk_for_swap`, immediately before its binary changes.

**Exact kiosk-down window.**

| Path | Before | Now |
|---|---|---|
| Pre-compiled binary | rsync + binary download + install + hooks + restart (~14 s) | binary install (a rename) + hooks + restart |
| Source build, kiosk kept up (`keep_j2` / `keep_j1`) | rsync + **whole compile** + hooks + restart | `promote_build_dir` (two renames) + hooks + restart |
| Source build, `stop_kiosk` plan | rsync + whole compile + hooks + restart | whole compile + hooks + restart (unchanged in practice) |

"Hooks" = Phone Remote/cores bootstraps, network hardening, memory tuning, Custom Format convergence — all still run with the kiosk stopped, as before (`setup_audio_service.sh` relies on it). A failed install's rollback still stops/restores/restarts the kiosk itself.

**Why the old kiosk may run against the new tree.** From the install rsync to the swap, the OLD binary runs beside the NEW tree — exactly what the web admin has always done during an update. `build/` (the binary *and* the unit's `WorkingDirectory=`, against which the kiosk resolves `../assets/...` and `path_resolver` candidates) is excluded from the rsync and replaced only at the swap; the compile writes only `build.new/`. rsync replaces each file by rename, so an open file keeps its old contents and a later open sees a whole old or whole new file. After startup the kiosk reads only image/font assets and system tiles from the tree; the scripts it runs are in `/usr/local/bin` (refreshed only after the verified start); settings, playlists and its runtime files are excluded. Worst case is cosmetic (an asset the release renamed fails to load until the restart). The swap itself must NOT happen under a running kiosk — renaming `build/` away deletes its working directory — which is why the stop sits right before `promote_build_dir` / `install_kiosk_binary`. A final `stop_kiosk_for_swap` before `verify_kiosk_started` guarantees that check always starts a stopped unit (starting a running unit would "verify" the old process).

**The decision — `build_memory_plan` (pure, table-tested).** Inputs: board (`/proc/device-tree/model`, same prefix rule as `PlatformProfile`), `MemAvailable` measured with the kiosk running, whether the kiosk is `active` (reached READY), and whether Media Browser services can be paused.

| Condition (first match wins) | Plan |
|---|---|
| board not Pi 4 / Pi 5 | `stop_kiosk` (today's behaviour — the thresholds are Pi measurements) |
| kiosk not `active` (TV off → exit-69 restart loop, or stopped) | `stop_kiosk` (no picture to keep; each restart's startup unpause would undo a services pause) |
| `MemAvailable` ≥ 1300 MiB | `keep_j2` |
| `MemAvailable` ≥ 700 MiB | `keep_j1` (twice as slow; the TV works) |
| services pausable | `pause_services` → re-measure → decide again without pausing |
| otherwise | `stop_kiosk`, `-j2` |

Thresholds: 600 MiB per make job (CLAUDE.md's cc1plus peak; `deploy_cpp.sh` measured ~430 MB and budgets 600) + 100 MiB reserve. The per-job budget already carries ~170 MiB/job over the measured peak, so a build started exactly at its floor has ~440 MiB (`-j2`) / ~270 MiB (`-j1`) of real slack for a viewer who starts a movie or a game mid-build. Never wider than `-j2` (the kiosk needs CPU). The tarball and its extracted copy are deleted from `TEMP_DIR` before measuring — `/tmp` is a RAM tmpfs on Trixie.

**Services pause.** Uses `/usr/local/bin/playback_services_pause.sh` — the kiosk's own quiet-mode script and marker (`/tmp/mdb_playback_services_paused`), so the cascade watcher and Content Manager already honour it; qBittorrent and Gluetun are untouched (downloads continue). Only on a provisioned box (`services/.env`), and never when the marker already exists (the kiosk paused them for a movie/game in progress — that memory is already in the measurement, and they are the kiosk's to resume). Paused only to keep the TV, never to upgrade `-j1` to `-j2`: while paused, the kiosk's tunnel monitor stops seeing Radarr and hides Movies (with a "tunnel down" toast) until the update finishes. **Resumed on every exit:** right after the compile (before the hooks — Custom Format convergence needs Radarr/Sonarr up), in `fail_install`, and by an `EXIT` trap the `install` dispatcher arms (killed job, `set -e` abort). Only what this update paused is resumed. A SIGKILL skips even the trap; the next kiosk start's crash-recovery unpause or the next boot (`compose up -d`; the tmpfs marker is gone) restores them. The script's compose-lock wait is bounded at 20 s; `update.sh` closes its own fd 9 (the single-flight lock) for it.

**Safety net.** The compile runs at `nice 19`, best-effort/lowest I/O priority, and `oom_score_adj 1000`, so if memory runs out anyway the kernel kills `cc1plus`, not the kiosk or RetroArch. A build that dies that way (gcc's "Killed signal terminated program", ld killed by signal 9, make exit 137) is retried once with the kiosk stopped at `-j1`; a real compile error is not retried and rolls back as before. build.new + verify + promote, the in-progress marker and boot recovery, rollback, the flock single-flight, the exit-69 acceptance and the `systemd-run` launch are all unchanged.

**Progress.** No new stage names. `building` carries "Compiling from source - the TV keeps working meanwhile (about 8-10 / 15-20 minutes)..." when the kiosk is kept up; `stopping_services` is now emitted at the swap ("Stopping the kiosk to switch to the new version...").

**Needs hardware validation before trusting the numbers further:** real `MemAvailable` with the kiosk up on a Pi 4B (games-only and Media Browser) and a Pi 5 2 GB; peak RSS of `cc1plus` at `-j1`/`-j2` on the current tree; menu/video/game responsiveness during a `-j1` build on a Pi 4B; and a game or movie started mid-build (expect the compiler, not the session, to be the casualty).

**Reaching the field:** like every `update.sh` change, this applies on the update AFTER the one that ships it.

## 2026-10 hardening — an interrupted or headless update can no longer brick or roll back a box

Seven defects in `update.sh`, each re-verified by reading before it was fixed:

1. **The kiosk binary is never missing or truncated.** `run_build` used to `rm -rf build/` and then compile for 8-10 minutes — a power cut or a killed job in that window left no kiosk at all (and the unit crash-loops forever, `StartLimitIntervalSec=0`). It now builds in `build.new/`, checks the result with `verify_kiosk_binary` (non-empty, 64-bit little-endian ELF for this CPU — `aarch64` on the Pis — executable), and only then swaps directories with two renames. The pre-compiled-binary path no longer `cp`s over the live file (cp truncates first: ENOSPC = truncated kiosk): it stages `<binary>.new`, verifies, and `mv -f`s it into place; a failed extract falls back to compiling, a failed install rolls back. Both used to be bare commands under `set -e` that exited with the kiosk stopped and no rollback.
2. **Power loss mid-install is recovered at the next boot.** Once the backup is complete, `update.sh` writes `/home/magic/.magic_dingus_box_backup.ota_in_progress`; it is removed only after a verified start (or a completed rollback). `magic-dingus-ota-recovery.service` (oneshot, `Before=` the kiosk and the web admin, `ConditionPathExists=` the marker, runs the **backup's** copy of `update.sh recover`) restores the backup if the marker survived. If VERSION already equals the marker's target, the update had completed and only the marker is cleared. A later install that finds the marker restores first, so it can never back up a half-installed tree over the only good backup. Installed by `update.sh` (every install), `deploy_cpp.sh`, `sync_source_box.sh` and `first_boot.sh`. **The marker can never ship in a golden image:** its name matches `prepare_for_cloning.sh`'s existing `/home/magic/.magic_dingus_box_backup*` tripwire (clone refused), and `first_boot.sh` deletes it.
3. **A good update is no longer rolled back on a box with no TV connected.** The kiosk now exits **69** (`platform::kExitNoDisplay`, `src/platform/kiosk_exit.h`) when DRM works but no connector is connected, instead of 1. `update.sh` treats a NEW process exiting 69 as "the binary runs" and keeps the update; systemd still restarts it every 5 s (69 is deliberately not in `RestartPreventExitStatus=`), so switching the TV on later works. The old "is-active 2 s after start" check is replaced by `verify_kiosk_started`: poll for up to 90 s, then require the kiosk to stay active with the same PID and no automatic restarts for 10 s — a crash right after READY now rolls back.
4. **rsync exit 23 is a failure.** It was accepted as "OK", so a root-owned file the install could not replace produced a half-applied update reported as success. Before the backup and before both restores, `normalize_tree_ownership` re-owns anything in the tree not owned by the update user (`sudo -n find … -exec chown`, like `deploy_cpp.sh` Step 0.9), **pruning `services/config` and `services/.env`**, which keep their ownership. That also unblocks the backup, which used to fail forever on an unreadable root 0600 file. Exit 24 (vanished source files) stays OK.
5. **Rollback puts the out-of-tree helpers back too.** `refresh_out_of_tree_files` now runs after both rollback paths, and on install only after the verified start — before, a rolled-back update left `/usr/local/bin` helpers and `/etc/dnsmasq.d/usb0.conf` at the release it had just rolled back.
6. **The Phone Remote bootstrap no longer runs `setup_services.sh`.** Run unprivileged, it always died at Step 0, so the uinput rule never arrived and `install_deps.sh` re-ran every OTA (and if it had worked, it would have restarted the web service mid-update and started Docker on games-only boxes). The uinput rule + `input` group step is now `setup_phone_remote_uinput.sh`, called by both `setup_services.sh` and (via `sudo -n`) `update.sh`.
7. **Exclude gaps closed.** `data/pending_revocations.txt` and `data/upload_temp/` added to all four lists. `config/*` and `VERSION` are now anchored (`/config/*`, `/VERSION`): unanchored, rsync matched them against the end of every path, so any nested `*/config/*` or `*/VERSION` (e.g. `build/_deps/*/VERSION`) was silently never delivered or backed up. Pinned by `tests/local/update_rsync_excludes.bats`.

**Reaching the field:** like every `update.sh` change, these take effect on the update AFTER the one that ships them (boxes update with the `update.sh` they already have). The kiosk's exit-69 change ships in the binary immediately, but the OLD `update.sh` on that first hop still treats it as a failed start — so a box doing that hop with its TV off will roll back once; the next attempt (with the TV on, or any later release) goes through.

## 2026-07-30 audit (pre-golden-image) — three contract fixes

Audited ahead of the Pi 5 golden image. Three changes to `update.sh`, applied to all four rsync lists (backup / install / internal rollback / user rollback):

1. **Phone Remote pairing state now survives OTA.** `data/paired_remotes.json`, `data/flask_secret.key`, `data/pairing_session.json`, `data/pairing_audit.log` (plus transient `kiosk_status.json` / `text_input_queue.jsonl` / `seek_request.json`) were not excluded and not in the release tarball, so the install rsync's `--delete` removed them — every successful OTA unpaired all phones and rotated the cookie-signing secret. Now excluded.
2. **System-tile thumbnails actually update now.** This doc always promised `data/thumbnails/systems/*.png` flows through OTA, but the blanket `data/thumbnails/*` exclude blocked it. An `--include 'data/thumbnails/systems/***'` placed before the exclude fixes it; per-game cover-art dirs stay preserved. Required for shipping new consoles (N64/Dreamcast tiles) to existing boxes.
3. **Add-only playlist sync.** New default playlists in a release are copied in only if the filename doesn't already exist on the box; existing playlists (operator-edited or not) are never overwritten. Without this, a box that OTA'd to an N64/Dreamcast-capable build would have the cores and code but no playlist to expose the new system.

Filter behavior was verified by simulation (fresh tile added, changed tile updated, per-game art / operator playlists / media / pairing files all preserved, add-only playlist copy).

Same-day follow-up audit (full pipeline review) found and fixed three more:

4. **The Media Browser was being compiled OUT of every OTA build.** `ENABLE_MEDIA_BROWSER` defaults OFF in CMakeLists; production builds get it from deploy_cpp.sh's `-DENABLE_MEDIA_BROWSER=ON`. Neither the release workflow's QEMU binary build nor update.sh's on-Pi `run_build` passed the flag. Historically masked on-Pi by the long-lived build dir's CMake cache; the 2026-07-29 clean-build fix (`rm -rf build`) unmasked it — the very next OTA would have removed the entire movie kiosk from every box (and the CI binary asset, which update.sh PREFERS, always lacked it). Both build paths now pass `-DENABLE_MEDIA_BROWSER=ON` (+ `libcurl4-openssl-dev` added to the CI container).
5. **Source-tarball selection hardened.** `check_update` picked the first `.tar.gz` asset — correct only by upload-order luck. If the ARM64 binary asset ever sorted first, install would rsync `--delete` a binary-only tree over the install dir. Now matches `magic-dingus-box-*.tar.gz` explicitly, and `install_update` refuses to proceed unless the extracted tree contains `magic_dingus_box_cpp/src` + `CMakeLists.txt`.
6. **Repo playlist set reconciled with the golden image.** The 10 pre-`games_*` playlists tracked in git matched no deployed box; replaced with the golden Pi's live set (9 `games_*` + 4 default video playlists). Fielded boxes keep their old-name playlists (preserved + system-dedupe gate); fresh checkouts and future OTAs now ship what production actually runs.

## v1.6.4 (2026-04-30)

Confirmed safe-to-ship via:
- Build clean from cmake on Pi 4B
- Smoke test: every overlay opens/closes cleanly, BTN2 modal across every screen, BTN2 pause preserved on Playback, similar-films pre-fetch hits TMDB on playback start, quick-add via Radarr returns expected toasts
- Settings persistence: 12 new fields write/read on restart

The 12 new Discover filter fields (`display.mb_popular_filter_*` and `display.mb_toprated_filter_*`) live under `config/*`, which is in the `update.sh` rsync exclude list. OTA preserves them exactly like every other display setting. On the first OTA where these keys are absent, the load path applies canonical defaults (all "Any") — no visual regression for operators upgrading from v1.6.3 or earlier.

The footer hint label changes are pure render-string changes in the C++ source; they propagate via the standard `magic_dingus_box_cpp/src/**` rsync and on-Pi rebuild. No new assets, no new services, no new build dependencies.

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
- **systemd unit gains an `EnvironmentFile=` line** — the kiosk unit (`magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service`) now declares `EnvironmentFile=-/opt/magic_dingus_box/services/.env` so the kiosk process inherits API keys from the codified Docker stack's `.env`. The `-` prefix makes the load optional, so unprovisioned Pis (no `services/.env` yet) still boot the kiosk cleanly. **Correction (v1.9.8):** this bullet used to claim the change was "propagated via the standard `systemd/**` rsync" with `daemon-reload` putting it in effect. It is not — the OTA rsync writes the unit *source* into `/opt` and never installs it to `/etc/systemd/system`. Boxes that have this line got it from a provisioning run (`setup_services.sh`), not from an update. See "What does NOT flow through OTA" above.
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
