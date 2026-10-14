# Converge system units — boxes run the tree's systemd units

**Status:** design for owner approval, NOT implemented (2026-10-14).
Touches the OTA path of every box sold. Read with `OTA_UPDATE_GUARANTEES.md`
and `2026-10-05-update-sh-split-design.md` ("which update.sh actually runs").

## Problem

OTA never reinstalls unit files. Every unit fix since v1.9.8 therefore
shipped twice: once in the tree (the truth, used by fresh provisioning) and
once as a hand-written drop-in in `setup_memory_tuning.sh` (the only way to
reach a fielded box). The second copy is where the bugs live. On
2026-10-14 the missing-search drop-in's empty `OnBootSec=` reset every
trigger in the unit, so the timer fired once per boot and then never
again — 10 days without a retry on the owner's box, while `verify_box.sh`
said SHIPPABLE. The tree's own unit was correct the whole time.

The goal: one step, run by every delivery path, that makes the units
installed in `/etc/systemd/system` equal to the units in the box's tree,
safely, and undoes itself on rollback. Unit fixes then ship as ordinary
unit-file edits.

## What is there today (findings)

**Writers.** Nine scripts copy units into `/etc/systemd/system`, each for
its own subset, each only when it happens to run:

| Writer | Units | Runs on a field box |
|---|---|---|
| `deploy_cpp.sh` | kiosk, web, first-boot, usb-gadget, cpu-performance, services | developer deploys only |
| `setup_services.sh` | the 15 Media Browser units (not `magic-dingus-services`) | Content Manager Configure/Reconfigure |
| `setup_harness_services.sh` | standby watcher, 2 LED units, power-switch-check | by hand |
| `install_deps.sh` | content-manager-redirect | Phone Remote bootstrap (once) |
| `setup_audio_service.sh` | magic-dingus-audio | every hook run (via `setup_memory_tuning.sh`) |
| `setup_ota_recovery.sh` | magic-dingus-ota-recovery | start of every OTA |
| `prepare_golden_image.sh`, `prepare_for_cloning.sh` | magic-first-boot | clone tooling |
| `setup_pi.sh`, `systemd/setup_boot_service.sh`, `migrate_to_cpp.sh` | kiosk, web | legacy, by hand |

Nothing installs `wifi-powersave-off.service`, yet it is installed and
enabled on the owner's box (an image-era leftover with no delivery path).

**Owner's box (magicpi5, 1.10.0, read-only inspection today).** Of the 29
tree units installed, exactly 3 differ from the tree — the three that
`setup_memory_tuning.sh` step 1c patches with drop-ins:

| Unit | Installed | Tree |
|---|---|---|
| `magic-dingus-missing-search.timer` | `OnBootSec=3min` | `OnBootSec=11min` |
| `magic-dingus-smoke-test.service` | no start timeout | `TimeoutStartSec=300` |
| `magic-dingus-storage-attach.service` | `TimeoutStartSec=180` | `TimeoutStartSec=600` |

Drop-ins present: kiosk `{memory-protect,stop-timeout,audio-service}.conf`,
the three timing drop-ins, `system.slice.d/mdb-memory.conf`. The kiosk unit
already matches the tree, so `stop-timeout.conf` is redundant there today.
Two units are deliberately installed-but-disabled
(`power-switch-check`, `magic-first-boot`). The six legacy root-`systemd/`
units (`magic-mpv*`, `magic-ui*`) are absent.

**No unit file has changed since the v1.10.0 release commit** (only
`setup_boot_service.sh`, a script). This matters for rollback, below.

**Which code runs when** (from the update.sh-split note): on a hop N → N+1
the install, its failed-install rollback and the power-cut recovery are all
N's `update.sh`; the only N+1 code on that hop is what N's updater runs by
path from the new tree — `setup_memory_tuning.sh` among them. A
user-initiated rollback runs the currently installed (newer) `update.sh`.
The hook runs with the kiosk STOPPED and before the verified start, so a
failed start rolls back after it ran.

## Approaches considered

**A. A converge script with a policy table, called from
`setup_memory_tuning.sh`; rollback converges back. (Recommended.)** Reaches
every path on the very release that ships it, including the old updater's
first OTA. One place decides what a unit's change means on a live box.

**B. Reinstall units from `update.sh`** (like `refresh_out_of_tree_files`).
Rejected: new `update.sh` code only runs on the hop after it ships, and it
misses deploy, first boot and the source-box sync.

**C. Keep drop-ins, but generate them** from a tree-vs-installed diff.
Rejected: a drop-in cannot express "this key is gone", and resetting a list
key is exactly the trap that caused the outage. The installed unit stays
stale and every reader has to merge two files to know the truth.

**D. Per-unit policy as `X-MDB-*` keys inside each unit file** (systemd
ignores `X-` keys). Self-describing, but it edits all 29 units, so the first
converge would rewrite every unit on every box and spread policy over 29
files. Rejected for v1; the table in A can move into the units later.

## Design

### 1. What the tree owns

Tree-owned units are every `*.service` / `*.timer` file in the tree's three
unit directories, identified by file NAME:

- `magic_dingus_box_cpp/systemd/`
- `magic_dingus_box_cpp/scripts/missing_search/`, `scripts/episode_priority/`
- `systemd/` (repo root)

The six legacy root units (`magic-mpv*.service`, `magic-ui*.service`) are
listed as `ignore` and never touched (deleting them from the tree is a
follow-up). No two directories ship the same name; a test pins that.

The canonical source is always the tree file. `deploy_cpp.sh`'s staging
copies under `/opt/magic_dingus_box/systemd/` are not sources.

### 2. The policy table

A table in the new script, one row per tree unit. A bats test fails when an
in-tree unit has no row, or a row has no tree file, so a new unit cannot
ship without someone choosing its policy.

- **gate** — `base` (every box) or `mb` (fresh install, enable, start and
  re-arm only when `<install>/services/.env` exists). An already installed
  MB unit is still kept current on a box without `.env`: refreshing a file
  never starts anything, and every MB unit carries its own
  `ConditionPathExists=.env`.
- **if absent** — `skip` (never install; another provisioner owns it),
  `install` (file only, never enabled), `enable`, `enable-now` (timers:
  enable and arm). Only ever applied to a unit that is NOT installed. An
  installed unit's enablement is never changed.
- **on change** — `none` (applies at its next start, boot or OTA restart),
  `try-restart` (restart only if running; only long-running services with
  no user-visible state), `rearm` (timers, below).
- **retires drop-in** — a drop-in this unit's tree content makes redundant.

| Unit | Gate | If absent | On change | Retires drop-in |
|---|---|---|---|---|
| `magic-dingus-box-cpp.service` | base | enable | none — never restarted | `stop-timeout.conf` |
| `magic-dingus-web.service` | base | enable | none — the OTA restarts web last | |
| `magic-dingus-audio.service` | base | enable | none | |
| `magic-dingus-ota-recovery.service` | base | enable | none | |
| `usb-gadget-network.service` | base | enable | none | |
| `magic-first-boot.service` | base | install (never enabled) | none | |
| `magic-cpu-performance.service` | base | skip | none | |
| `wifi-powersave-off.service` | base | skip | none | |
| `content-manager-redirect.service` | base | skip (install_deps provisions dnsmasq) | try-restart | |
| `kiosk-standby-watcher.service` | base | skip (harness installer) | try-restart | |
| `led-boot-sequence.service`, `led-shutdown-animation.service` | base | skip | none | |
| `power-switch-check.service` | base | skip | none | |
| `magic-dingus-services.service` | mb | enable (not now) | none — its stop is `compose down` | |
| `gluetun-cascade-restart.service` | mb | skip (needs `/usr/local/bin` helper) | try-restart | |
| `magic-dingus-clear-cooldowns.service` | mb | skip (helper) | none | |
| `magic-dingus-sync-qbit-password.service` | mb | skip (helper) | none | |
| `qbit-port-sync.service` / `.timer` | mb | skip (helper) / skip | none / rearm | |
| `magic-dingus-auto-blocklist.service` / `.timer` | mb | skip (helper) / skip | none / rearm | |
| `magic-dingus-storage-attach.service` | mb | enable | none | `mdb-timeout.conf` |
| `magic-dingus-library-import.service` | mb | enable | none | |
| `magic-dingus-smoke-test.service` / `.timer` | mb | install / enable-now | none / rearm | service: `mdb-timeout.conf` |
| `magic-dingus-missing-search.service` / `.timer` | mb | install / enable-now | none / rearm | timer: `mdb-boot-delay.conf` |
| `magic-dingus-episode-priority.service` / `.timer` | mb | install / enable-now | none / rearm | |

Rules behind the choices:

- **The kiosk is never restarted by converge.** On an OTA it is stopped
  when the hook runs and the OTA starts it; on a deploy the deploy restarts
  it. A changed kiosk unit otherwise applies at its next start.
- **`skip` wherever the unit needs something the tree does not carry**: a
  `/usr/local/bin` helper that only `setup_services.sh` creates
  (`refresh_out_of_tree_files` refreshes helpers but never creates them),
  dnsmasq config, harness wiring. Installing those alone would arm a unit
  that fails every run.
- **`enable` for MB units whose ExecStart is in the tree.** This delivers
  timers that `setup_services.sh` added after a box was provisioned (the
  "missing-search was written but never installed" class) without a
  Reconfigure. It is a behavior change: see Decision 1.
- Units with no row (only possible when converging an older tree, see
  Rollback) get the safest policy: base, skip, none.

### 3. The converge run

`magic_dingus_box_cpp/scripts/converge_system_units.sh [--check] [--tree DIR]`,
root, idempotent, serialized by `flock -w 60 /run/lock/mdb-units.lock`.
`--tree` defaults to the tree the script lives in.

1. **Plan.** For each tree unit: `current` (installed, byte-identical),
   `changed`, `absent-install` (absent, policy and gate allow), `absent-skip`,
   or `skipped-link`. A destination that is a SYMLINK is never written: a
   masked unit is `/etc/systemd/system/X -> /dev/null`, and `install(1)`
   over it would silently unmask it.
2. **Write.** For `changed` / `absent-install`: copy to
   `/etc/systemd/system/.<name>.mdb-tmp` (mode 0644, root), then `mv -f`.
   Same-directory rename, so systemd never sees a half-written unit (and
   ignores the temp name — it is not a unit name).
3. **Retire.** Each unit in the state file (step 7) that the tree no longer
   ships: `systemctl disable --now --no-block`, then remove the file. This
   is what makes a rollback remove a unit the rolled-back release added. A
   symlinked/masked unit is left alone. See Decision 2.
4. **Drop-in cleanup.** For each `retires drop-in` entry: remove the file
   only if the unit's installed copy now equals the tree AND the drop-in
   carries the `(setup_memory_tuning.sh)` marker line (only our own files
   are ever removed). A unit whose write failed keeps its drop-in, so it
   never loses the fix.
5. **`daemon-reload`** once, if anything was written, retired or removed.
6. **Apply.** Every start, stop or restart is `--no-block`, which makes the
   run safe at boot (first boot, power-cut recovery) where a blocking job
   can deadlock against the boot transaction.
   - `absent-install` + `enable` → `systemctl enable`;
     `enable-now` → `systemctl enable --now --no-block`.
   - `changed` + `try-restart` → `systemctl --no-block try-restart`.
   - `rearm` timers (gated): when the timer is enabled and `SubState=elapsed`
     → `systemctl --no-block restart`. A `waiting` timer is never restarted
     (a restart can fire it immediately); `daemon-reload` hands it its new
     triggers — to be confirmed on hardware during validation (if not, the
     policy becomes "restart a changed waiting timer").
     This replaces `setup_memory_tuning.sh`'s one-off missing-search re-arm.
7. **State.** Write `/var/lib/magic-dingus/system_units` (root, 0644): the
   names of every tree unit installed after this run. Losing the file only
   disables retirement until the next run (fail-safe). Clones inherit it,
   which is correct: they inherit the units too.

**Failure posture.** Per-unit errors are logged and counted; the run
continues. Exit 0 when everything converged, 2 when something failed (the
caller logs a warning). Callers never abort on it.

**`--check`** does steps 1, 3 and 4 read-only (no root needed — unit files
are world-readable) and prints one line per finding: `DRIFT <unit>`,
`MISSING <unit>` (absent, policy and gate say install), `STALE <unit>`
(state-file unit the tree no longer ships), `DROPIN <path>` (a retire-listed
drop-in still present on a converged unit). Exit 1 on any finding.

**Test seams** (same conventions as `setup_memory_tuning.sh`):
`MAGIC_TUNING_ROOT` prefixes `/etc`, `/var/lib` and `/run/lock`;
`MAGIC_SKIP_SYSTEMCTL=true` skips every systemctl call. The apply phase is
tested with a `systemctl` stub on `PATH` that logs calls and answers
`is-enabled` / `show` from fixture files.

### 4. Where it runs

`setup_memory_tuning.sh` calls it as a new step 1a, first, best-effort
(`|| log WARNING`). That is the hook every path already runs:

| Path | Who runs the hook | Kiosk at that moment |
|---|---|---|
| OTA install (every updater since v1.9.10) | the old updater runs the NEW tree's hook | stopped (before the verified start) |
| `deploy_cpp.sh` step 1.68 | deploy | running (deploy restarts it later) |
| `first_boot.sh` step 6g | first boot | not started yet |
| `sync_source_box.sh` | source-box sync | running |
| rollback (all three runners, new) | see Rollback | stopped |

Steps 1b and 1c of `setup_memory_tuning.sh` stop WRITING `stop-timeout.conf`
and the three timing drop-ins (the converge removes them), and step 5's
missing-search re-arm goes. Drop-ins that stay, and why:

| Drop-in | Why it stays a drop-in |
|---|---|
| kiosk `memory-protect.conf` + `system.slice.d/mdb-memory.conf` | memory posture: only meaningful with the cmdline flag the same script owns |
| kiosk `audio-service.conf` | carries the magic user's REAL uid, resolved per box |
| `zz-policy-override.conf` (operator, test-only) | not ours |

Other writers keep copying their units for now: they copy the same tree
file, so they cannot disagree, and the converge runs on every path anyway.
Routing them through `converge_system_units.sh --only <unit>` is a
follow-up.

`sync_source_box.sh` additionally pushes the new script and fails the sync
unless `converge_system_units.sh --check` is clean, so an image always
carries converged units.

### 5. Rollback

All three rollback runners call a new `update.sh` function,
`converge_units_after_restore`, right after `refresh_out_of_tree_files`:

1. Before the restore rsync, copy the rolled-back-from tree's
   `converge_system_units.sh` to a temp file (the rsync deletes it).
2. After the rsync: if the RESTORED tree has `converge_system_units.sh`,
   run it (`sudo -n bash`) — the restored tree's units, with the restored
   tree's own policy. Otherwise run the stashed copy with
   `--tree <install dir>`: the restored tree's units under the newer
   policy table (unknown names get base/skip/none).
3. Never fails the rollback; skipped in `MAGIC_SKIP_SYSTEMCTL` test mode.

Retirement (step 3 of the run) is what removes a unit the rolled-back
release added: the state file lists it, the restored tree does not ship it.

**Who gets this, honestly.** User-initiated rollback runs the installed
(newer) updater, so it gets converge-back from the release that ships this
(call it C). A failed-install rollback and the power-cut recovery run the
OLDER updater, so on the hop INTO C they cannot: the box keeps C's units
with C−1's tree. That is safe because of a rollout rule:

> **C ships with zero unit-file edits.** Its units are byte-identical to
> v1.10.0's, so "kept C's units" IS "converged to the restored tree". Unit
> comments that now say "OTA never replaces installed units" are corrected
> in C+1, when every rollback runner understands convergence.

A box on v1.9.x jumping straight to C and failing its start keeps C's (=
1.10.0's) units on the 1.9.14 tree. Checked: the 1.10.0 kiosk unit only
`Wants=` the audio unit, which is skipped there (`ConditionPathExists=` on
`audio_service.sh`), so the 1.9.14 `init_audio.sh` starts PulseAudio itself
as before; the timing values are pure improvements. This is the same
residue a 1.9.14 → 1.10.0 failed rollback already leaves today
(`OTA_UPDATE_GUARANTEES.md`: "only rollbacks run by this update.sh or a
later one get this"). The container rehearsal's `old_cli` scenario asserts
the kiosk and web still start after it.

### 6. `verify_box.sh`

A "System units" section runs `converge_system_units.sh --check` against the
box's tree. `DRIFT`, `MISSING` and `STALE` are **FAIL**, `DROPIN` is WARN.

FAIL, because after C every delivery path converges, so a mismatch can only
mean the converge is failing on this box or someone edited `/etc` by hand —
both real defects, and drift is exactly how missing-search went silent for
10 days while this script said SHIPPABLE. The fix line names one command:
`sudo bash <tree>/magic_dingus_box_cpp/scripts/converge_system_units.sh`.
A redundant drop-in on a converged unit only restates the unit's own value,
so it is a WARN. See Decision 3.

### 7. What the owner's box would see

`--check` today: `DRIFT` × 3 (the three units above) and `DROPIN` for the
kiosk's `stop-timeout.conf` (its unit already matches). One run: those three
rewritten, four drop-ins removed (`stop-timeout.conf`, the two
`mdb-timeout.conf`, `mdb-boot-delay.conf`), one `daemon-reload`, nothing
restarted (the missing-search timer is `waiting`), state file written.
Effective configuration unchanged — the drop-ins already said the same
thing.

## Testing and validation

- `scripts/tests/test_converge_system_units.bats` (fake root, stubbed
  systemctl): identical → no write, no reload; drift → atomic rewrite +
  reload; masked/symlinked → never written; absent + skip / install /
  enable / enable-now; MB gate with and without `.env`; installed-disabled
  unit stays disabled; kiosk never restarted; try-restart only for changed
  try-restart units; elapsed timer re-armed, waiting timer not; retirement
  from the state file; drop-in removed only when converged AND marker-tagged;
  `--tree` against another tree; `--check` output and exit codes; idempotent
  second run makes zero writes and zero systemctl mutations.
- Table completeness: every in-tree unit has a row, every row a file, no
  name in two directories, `ignore` only for the legacy six.
- `test_memory_tuning.bats`: the drop-in tests flip to "not written, and
  removed by the converge"; the effective-trigger test runs against the
  tree's missing-search timer instead.
- `test_update.bats`: both rollback paths call `converge_units_after_restore`
  after the restore; restored tree's script preferred; stash used when
  absent; skipped in test mode; never fails the rollback.
- `verify_box` parsing of `--check` output.
- Container rehearsal (`tests/ota_rehearsal/run.sh`): after install,
  installed units equal NEW's tree and the four drop-ins are gone; after the
  NEW web admin's rollback, units equal OLD's tree; after `old_cli`'s
  OLD-updater rollback, kiosk and web still start.
- Hardware rehearsal (`hw_rehearsal.sh`) **only with the owner's go-ahead**,
  from v1.9.14 on magicpi5: the existing `field_state_1.9.14.txt` plus the
  git-derived `old-unit` copies reproduce a field box's stale units (the
  harness's unit dirs gain `scripts/missing_search/` and
  `scripts/episode_priority/`, which the converge now owns).

Docs: CLAUDE.md's "Because OTA never re-installs unit files, this script is
also the delivery path for in-tree unit fixes..." passage and the other
"OTA never replaces unit files" mentions; `OTA_UPDATE_GUARANTEES.md`'s
"NOT reinstalled" row and "Reaching field boxes — the rule" (a unit fix is
now a unit-file edit; the new rule is "pick its policy row").

## Decisions for the owner

1. **Fresh-install missing Media Browser units on OTA?** Recommended: yes,
   for the six whose ExecStart is in the tree (services, storage-attach,
   library-import, and the smoke-test / missing-search / episode-priority
   pairs), only on boxes with `services/.env`. No: converge only keeps
   already-installed units current, and a Reconfigure stays the only way to
   add one.
2. **Retire units the tree stops shipping?** Recommended: yes (state-file
   based, as above) — it is what makes rollback complete and lets a release
   delete a unit. No: rollback leaves a unit added by the rolled-back
   release installed (with its script gone, it fails at its next trigger).
3. **`verify_box` severity for drift:** recommended FAIL (above); WARN is the
   alternative if Box health on customer boxes should never go red for this.
4. **`wifi-powersave-off` and `magic-cpu-performance` when absent:**
   recommended skip (kept current where installed). Enable would hand them
   to every box that lacks them; both look like intended base platform, but
   neither has a provisioning path to confirm that.

## Out of scope (follow-ups)

- Route the other eight writers' file copies through
  `converge_system_units.sh --only <unit>`, leaving one writer.
- Delete the legacy units and scripts (`magic-mpv*`, `magic-ui*`,
  `setup_boot_service.sh`, `migrate_to_cpp.sh`, `scripts/setup_pi.sh`'s
  unit copy).
- C+1: correct the stale "OTA never replaces units" comments inside unit
  files; consider folding `memory-protect.conf` into the kiosk unit.
