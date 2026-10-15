# Splitting `update.sh` — constraints and a safe incremental plan

**Status:** proposal, NOT implemented (2026-10-05). `update.sh` is ~2,760
lines; this note records why it cannot simply be cut into files, and the
order in which it safely can be. Read with `OTA_UPDATE_GUARANTEES.md`.

## Which `update.sh` actually runs

| Command | Whose code runs |
|---|---|
| `check`, `channel`, `install`, `rollback` | The web admin execs `<install>/magic_dingus_box_cpp/scripts/update.sh` (`admin.py` `UPDATE_SCRIPT`) — the copy **currently installed**. A hop from release N to N+1 is executed entirely by **N's** updater. |
| `recover` (boot, after a power cut) | `magic-dingus-ota-recovery.service` runs the **backup's** copy: `/home/magic/.magic_dingus_box_backup/magic_dingus_box_cpp/scripts/update.sh recover`. |
| `deploy_cpp.sh` steps 1.59 / 1.6 | `source`s the box's `update.sh` to call `refresh_out_of_tree_files` and `ensure_web_server_dep` (the dispatcher is skipped when sourced: the `BASH_SOURCE` guard). |
| `test_update.bats` | `source`s it to unit-test functions; many tests also `grep`/`sed` text windows out of the file. |

During `install`, bash has already parsed every function (they are all
defined before the dispatcher, and the dispatcher's `case` is read as one
compound command). The install rsync does not use `--inplace`, so it
replaces `update.sh` by rename and the running process keeps reading the
old inode. Therefore **everything the install does is OLD code**: the four
rsync exclude lists, the build memory plan, `verify_kiosk_started`,
`rollback_internal`, and the helper *list* in `refresh_out_of_tree_files`
(whose file *contents* come from the new tree — so a helper added to that
list in release N is first refreshed on the hop N → N+1).

The only NEW code that runs on the hop that ships it is what the old
updater invokes **by path** from `INSTALL_DIR` after the rsync:
`setup_network_hardening.sh`, `setup_memory_tuning.sh` (→
`setup_audio_service.sh`, `restart_stale_cascade_watcher.sh`),
`converge_custom_formats.sh`, `setup_phone_remote_uinput.sh` /
`install_deps.sh`, `install_cores.sh`. `setup_ota_recovery.sh` runs
*before* the rsync, i.e. from the old tree.

Corollary: **any change to `update.sh` — including the split itself — is
first exercised on the hop AFTER the release that ships it.** A broken
loader is discovered by the first customer box that updates *away* from
that release.

## What a split must preserve

1. **Source everything at startup, never mid-run.** Libraries sourced
   before any command runs come from the same (old) tree as the caller.
   A lazy `source` after the rsync would load the NEW library into the OLD
   caller — version skew in the one script that cannot afford it — and an
   rsync `--delete` of a renamed lib would make it vanish under the caller.
2. **Resolve libraries relative to `BASH_SOURCE[0]`, never `INSTALL_DIR`.**
   During `recover`, `INSTALL_DIR` holds the half-installed tree; the
   backup's updater must load the backup's libraries. The same rule makes
   `source update.sh` work from `deploy_cpp.sh` and the bats suite.
3. **A missing library is an early, total refusal** — checked before the
   lock, the backup or any `systemctl` call, reported through
   `json_response` (so define the JSON/log helpers in the entrypoint or the
   first library and fail with a hand-written JSON line if even that is
   missing).
4. **The entrypoint path never changes.** `admin.py` and the recovery
   unit's `ExecStart=` hard-code it, and OTA never rewrites unit files.
5. **Every library ships in the tarball.** `release.yml`'s source archive
   excludes any path component named `build` and every `*.log`; add each
   library to the "Assert tarball layout" list so a dropped file blocks the
   release instead of a box.
6. **Rollback to a pre-split release keeps working.** The backup of a
   pre-split tree has no libraries and needs none; a post-split rollback
   restores the libraries with the rest of `scripts/` (the backup rsync
   covers the whole tree). Never exclude the library directory anywhere.
7. **Tests that read `update.sh` as text must move with the code.**
   `test_update.bats` extracts functions with `grep -A N` / `sed` windows,
   and `tests/local/update_rsync_excludes.bats` asserts the four exclude
   lists by reading the file. A moved function makes those assertions
   silently test nothing — re-point each one in the same commit.

## Incremental plan (one step per release, each rehearsed)

0. **Lint gate first** (done 2026-10-05): `update.sh` is covered by the
   blocking ShellCheck gate (`tests/shellcheck_gate.sh`, `-S info`), so
   every extraction step is linted, including the new libraries.
1. **Pure functions only.** Move the I/O-free helpers — `version_valid`,
   `_num_cmp`, `version_cmp`, `version_lt`, `version_is_prerelease`,
   `build_memory_plan`, `kiosk_start_verdict`, `rsync_exit_ok`,
   `expected_elf_machine` — to `scripts/lib/ota_pure.sh`, sourced at the
   top per rules 1-3. Tests source the library directly. Smallest possible
   first use of the loader.
2. **Rehearse the loader across the three boundaries** with
   `tests/ota_rehearsal/run.sh` and `hw_rehearsal.sh`: pre-split → split
   (old updater installs libraries it never loads), split → split+1 (first
   hop *executed* by the loader — the real test), split → rollback to
   pre-split, and a power cut mid-install (`recover` from both a pre-split
   and a split backup). Only after this passes does step 3 start.
3. **One exclude list.** Define the operator-content excludes once
   (`scripts/lib/ota_excludes.sh`, a bash array) and use it in all four
   rsyncs — the "update ALL FOUR lists" rule becomes structural, which is
   the biggest safety win of the whole split. Keep the
   `data/thumbnails/systems/***` `--include` ordered before its exclude;
   keep `update_rsync_excludes.bats` asserting the *expanded* lists.
4. **I/O groups, one per release:** build (`run_build` …
   `install_kiosk_binary`), kiosk verification, channel/check
   (`fetch_beta_release`, `check_update`), out-of-tree refresh, marker +
   recovery. Rehearse each like step 2.
5. **End state:** `update.sh` = configuration + library loading +
   dispatcher, at the same path forever.

**Non-goal:** re-exec'ing the NEW tree's updater mid-install to remove the
one-release lag. It makes the old release's rollback depend on new code —
the property that keeps a bad release recoverable on every box in the
field.
