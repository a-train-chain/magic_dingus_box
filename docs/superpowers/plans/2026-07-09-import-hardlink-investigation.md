# Fix 2 Investigation — Why imports are slow, and what a fix would take

**Status:** Investigation only (user chose "investigate more first"). NO
storage changes made. This is the findings report + migration plan for a
future approval decision.

**Date:** 2026-07-09. Author: Claude (opus-4-8), from live probes on
magicpi + the import-infra pipeline map.

---

## The finding, in one paragraph

When a movie finishes downloading, Radarr **copies** it from
`/mnt/ssd/downloads/complete/…` into `/mnt/ssd/library/…` instead of
instant-hardlinking it. Both directories are on the **same** ext4 filesystem
(`/dev/sda1`, the USB SSD — device id `2049` for both), so a hardlink *should*
be instant and free. It isn't, because inside the Radarr container `/downloads`
and `/library` are **two separate Docker bind mounts**, and the kernel refuses
`link()` across mount points (`EXDEV`), so Radarr falls back to a full byte copy.
Measured SSD write throughput is **79.7 MB/s**, so this copy costs about
**15 s for a 1.2 GB movie and ~50 s for a 4 GB one** — a real window where the
movie shows as "downloaded" but isn't yet playable. (`copyUsingHardlinks=True`
is already set in Radarr; it's silently defeated by the mount layout.)

Verified live: the two Toy Story copies have **different inodes**
(downloads: `12582953`, library: `4456450`) — proof of copy, not hardlink.

There is **no microSD → thumb-drive transfer**. Everything (downloads + library)
lives on the one USB SSD; the microSD holds only the OS. The user's instinct
about "a second stage" was correct — the second stage is this copy, not a
device-to-device move.

---

## What a fix requires (single-parent-mount restructure)

The fix is to give both `downloads` and `library` a **single shared parent
mount** inside the containers, so hardlinks work:

Today (two mounts, hardlink breaks):
```
  host /mnt/ssd/downloads  ->  container /downloads
  host /mnt/ssd/library    ->  container /library
```
Target (one mount, hardlink works):
```
  host /mnt/ssd  ->  container /data      (downloads + library live under /data)
  Radarr root folder: /data/library
  qBit save path:     /data/downloads/complete
```

**Crucially, the host files do NOT move** — they're already at
`/mnt/ssd/{downloads,library}` on one filesystem. Only the *container-side path
label* changes (`/library` → `/data/library`). So this is a path-**remap**, not
a data migration. But every stored absolute path must be rewritten in lockstep,
or the library reads as "missing."

### Blast radius on the existing populated library: MEDIUM-HIGH

The 17 movies with files (83 GB) all have paths stored under `/library` in
`radarr.db`, and seeding torrents have paths under `/downloads` in qBit's
fastresume. Every one must be rewritten atomically with the mount change:

| # | Change | Where | Risk | Notes |
|---|---|---|---|---|
| 1 | Collapse Radarr's two mounts to `- ${STORAGE_ROOT}:/data` | `services/docker-compose.yml:26-27` | med | the enabling change |
| 2 | Same for qBit | `services/docker-compose.yml:198` | high | existing seeds' fastresume point at `/downloads` → error until bulk "Set location" to `/data/downloads` |
| 3 | Rewrite Radarr root folder `/library`→`/data/library` **and** every `Movie.Path` prefix in `radarr.db` (SQL, NOT the "change root folder" UI which would try to physically MOVE files) | `config/radarr/radarr.db` (runtime, part of golden image — not in repo) | high | do before restart or whole library reads missing |
| 4 | Kiosk container prefix `/library/`→`/data/library/` | `radarr_client.h:24` OR env `MDB_CONTAINER_LIBRARY_PREFIX=/data/library/` (no recompile) | med | host prefix `/mnt/ssd/library/` stays unchanged |
| 5 | qBit save/temp path prefs | `setup_services.sh:598-605`, `qbit_categories.json` | med | set `save_path=/data/downloads/complete` |
| 6 | DownloadedMoviesScan reconcile path | `setup_services.sh:1930` | low | `/downloads/complete`→`/data/downloads/complete` |
| 7 | Verify the second host-path derivation | `playlist_loader.h:24` (`/mnt/ssd/library/Movies`) | med | hardcodes a `/Movies` subdir the Radarr setup says was dropped — verify against live root folder |

Host-side free-space probes (`/mnt/ssd/library` in browse/detail/library
screens) and `web/admin.py` STORAGE_ROOT are UNCHANGED — the host layout stays
put; only container mount labels move.

On a **fresh install** the same restructure is LOW risk (fixture + compose
edits only, no existing paths to rewrite).

---

## Options, ranked

**Option A — Do the restructure (one-time migration).** Makes every future
import instant (~0 s vs 15-50 s). Requires the coordinated `radarr.db` +
qBit-fastresume rewrite above, done carefully with the stack stopped and a
`radarr.db` backup. ~30-60 min of careful work + verification. Best long-term;
highest one-time risk. **Recommend only with a full db backup + a tested
rollback, and ideally rehearsed on a clone first.**

**Option B — Leave it; rely on the new UI (RECOMMENDED for now).** Fix 1 (just
shipped) makes the 15-50 s copy window **visible and understood**: the movie
shows "Importing…" (amber) on Queue and "Downloaded — importing, ready in a few
seconds" on Detail, then flips to Play automatically. The delay still exists but
is no longer confusing — which was the user's actual complaint. Zero risk.

**Option C — Shrink the window without restructuring.** Not really available:
the copy speed is SSD-bound (79.7 MB/s is already healthy for USB SATA), and
`copyUsingHardlinks` is already on. The only real lever is the mount layout
(Option A).

---

## Recommendation

Ship Fix 1 (done). Hold Option A until the user explicitly wants instant
imports AND we can (a) back up `radarr.db`, (b) rehearse on the golden-image
clone, (c) do it attended. It's a good improvement but it's a populated-library
migration — not something to do unattended or without a rollback. Bring it back
as its own change with a rehearsal step.

---

---

## UPDATE (2026-07-09, later) — groundwork done + migration rehearsed

Two corrections + concrete deliverables since the original writeup:

### Correction 1: there is no "free fixture fix" — the root folder is in the golden image
The original plan implied fresh Pis build their Radarr config from repo
fixtures, so a compose change would auto-correct new installs. **Wrong.**
`setup_services.sh` never creates a Radarr root folder (no
`POST /api/v3/rootfolder` anywhere). The root folder `/library` and every
movie path live in `config/radarr/radarr.db`, which is part of the GOLDEN
IMAGE and reused as-is on every clone (the repo ships only
`docker-compose.yml` + `.env.example` under `services/` — `config/` is
golden-image-only). So both new and existing Pis inherit `/library`; a
config-only change can't fix that. The real fix is a one-time `radarr.db`
path rewrite (per-Pi, or once on the golden master).

### Correction 2: the migration surface is SMALLER than feared
Full schema scan of the live `radarr.db` (Radarr schema v240): only **two**
columns hold `/library` — `RootFolders.Path` (1 row) and `Movies.Path`
(18 rows). `MovieFiles.RelativePath` is relative (no rewrite). No
`/downloads` anywhere in radarr.db (qBit owns those). So the Radarr side is
a 19-row, exact-prefix UPDATE — trivial and transactional.

### What was shipped now (groundwork, zero risk to the live Pi)
- **Backward-compatible compose** (`services/docker-compose.yml`): added
  `${STORAGE_ROOT}:/data` to Radarr + qBit **alongside** the existing
  `/downloads` + `/library` mounts. Purely additive — the current Pi's
  paths still resolve, the new mount is inert until activated. Validated
  with `docker compose config`.
- **Tested migration script** (`scripts/migrate_hardlink_layout.sh`):
  backs up radarr.db → transactional `/library`→`/data/library` rewrite
  with integrity + movie-count guards (auto-restores backup on any
  mismatch) → qBit save path to `/data/downloads` + `setLocation` all
  torrents → restart + verify Radarr still sees the files. Idempotent.
  shellcheck-clean.
- **setup_services.sh advisory**: on an existing `/library` install, prints
  the one-command migration hint (never auto-runs it).

### Rehearsal results (offline, against a copy of the live radarr.db)
- SQL rewrite: 18/18 `Movies.Path` + root folder → `/data/library`, 0
  `/library` remaining, `PRAGMA integrity_check = ok`, movie count intact.
- Path validity: 17/18 migrated container paths (`/data/library/X`) map to
  a real host folder (`/mnt/ssd/library/X`). The 1 exception is
  "Three Days of the Condor" — the known stuck/never-imported movie, not a
  migration flaw.
- qBit API shape confirmed against live qBit v5.0.3: current
  `save_path=/downloads/complete`, `temp_path=/downloads/incomplete` — the
  exact values the script rewrites to `/data/downloads/*`; both
  `setPreferences` + `setLocation` endpoints present.

**Net:** the migration is now a proven, one-command, attended operation
with automatic rollback-on-mismatch. It has NOT been run on the live Pi or
the golden image — that remains a deliberate, user-triggered step. When you
want instant imports (or to reclaim the ~30-40 GB of double-stored seeding
copies), run `migrate_hardlink_layout.sh` during a quiet moment and keep
the printed backup until playback is confirmed.

## Reproduction / evidence commands (for the record)

```
# same filesystem (both 2049):
stat -c %d /mnt/ssd/downloads ; stat -c %d /mnt/ssd/library
# copy not hardlink (different inodes):
ls -li "/mnt/ssd/downloads/complete/Toy Story (1995) [1080p]"
ls -li "/mnt/ssd/library/Toy Story (1995)"
# radarr sees separate mounts (the EXDEV cause):
docker exec mdb_radarr df -P /downloads /library
# copy throughput:
dd if=/dev/zero of=/mnt/ssd/.t bs=1M count=500 conv=fdatasync ; rm /mnt/ssd/.t
```
