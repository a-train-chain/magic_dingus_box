# Season-delete probe notes (Task 1)

Date run: 2026-08-13, ~09:35-09:39 UTC. Box: magicpi5 (`magic@10.0.0.227`),
Sonarr v4.0.19.2979 at `localhost:8989`.

## Status: BLOCKED

The task's precondition — "GoT (with its wrong-language season 3) present
in the library" — is false. Sonarr's series list is **empty (0 series)**.
The Game of Thrones series record and its files were deleted from Sonarr
shortly before this probe session started, by something other than this
task. P1/P2/P3 could not be answered against real data as a result. Full
evidence below.

## What was found

1. `GET /api/v3/series` → `[]` (0 series, any title). No GoT, no other
   show. This is not a title-matching miss — the library is genuinely
   empty.
2. Sonarr's own persisted log (`GET /api/v3/log`) shows the deletion
   happening live, inside the current container's uptime, timestamped:
   ```
   2026-08-13T08:37:01Z  Completed scanning disk for Game of Thrones   (series still exists here)
   2026-08-13T08:37:47Z  Application is shutting down...
   2026-08-13T08:38:49Z  Application started.
   2026-08-13T08:39:47Z  Application is shutting down...
   2026-08-13T08:56:16Z  Application started.                          (current container instance)
   2026-08-13T08:58:04Z  Attempting to send '/data/library/tv/Game of Thrones' to recycling bin
   2026-08-13T08:58:04Z  Recycling Bin has not been configured, deleting permanently. /data/library/tv/Game of Thrones
   ```
   The "recycling bin / deleting permanently" log line is emitted by
   Sonarr's `DeleteSeries` path (`RecycleBinProvider.DeleteFolder`,
   triggered by `DELETE /api/v3/series/{id}?deleteFiles=true`). This
   happened ~2 minutes after the container's most recent restart and
   ~37 minutes before this probe session's first API call — i.e. before
   I touched the box at all.
3. `docker exec mdb_sonarr ls -la /data/library/tv/` confirms the folder
   is now empty except a `.mdb-keep` placeholder (mtime 08:58 UTC,
   matching the deletion timestamp). `.mdb-keep` is created by
   `magic_dingus_box_cpp/scripts/storage_attach.sh` /
   `setup_services.sh` whenever the folder is (re)created — its presence
   here is a side effect of the delete emptying the dir, not evidence of
   a storage re-link.
4. Cross-checks to scope the blast radius:
   - **Radarr (movies) is unaffected**: 21 movies still present, so this
     is not a full-stack reset.
   - **qBittorrent shows 0 torrents total** (checked via its own API,
     read-only login). Whatever removed GoT from Sonarr also appears to
     have cleared qBit's torrent list — consistent with the 4-step
     "Confirm Remove" pattern documented in CLAUDE.md (cancel queue →
     delete associated torrents in qBit → remove series with
     deleteFiles=true), though CLAUDE.md only documents that flow for
     Radarr/movies today.
   - `GET /api/v3/importlist` → `[]` — no import list sync could have
     auto-deleted the series; this was not an automated list-reconcile.
   - `magic-dingus-storage-attach.service` journal shows its last run at
     08:36:23 UTC (before the deletion), reporting "bind is live — no
     action needed" — the storage re-link path did not fire in the
     relevant window and is not implicated.
   - No `prepare_for_cloning.sh` / `restore_after_cloning.sh` /
     `clone_live_sd.sh` process was running (`ps aux` checked).
   - The design spec (`docs/superpowers/specs/2026-08-12-season-delete-design.md`)
     confirms GoT season 3 (wrong-language) is the real motivating case
     for this whole feature — so its loss is a genuine, not cosmetic,
     regression to the probe's ground truth.
5. **Root cause is unknown.** I did not find a log line that names the
   caller/client of the DELETE request (Sonarr doesn't log request
   source by default), and I did not pursue it further per the task's
   "don't guess" instruction. Given the timing (right after this
   session's plan commit landed, on the exact branch/box this feature
   targets), the most likely explanations are a manual test of a
   remove-series-style flow against Sonarr on this box, or an unrelated
   operator action — either way this needs a human answer, not a guess
   recorded here as fact.

## P1 — does `/history/series` accept `seasonNumber` server-side?

**Not answered.** No series id exists to query. The one attempt made
(with an empty `SID` before the empty-library problem was diagnosed)
returned HTTP 400 from Sonarr, which is only informative as "you must
pass a valid seriesId" — it says nothing about server-side season
filtering. This needs to be re-run once a series with real history
exists again.

## P2 — does `POST /api/v3/history/failed/{id}` return 200 (mark-as-failed)?

**Not answered.** No history record exists to mark failed (no series,
no grabs, no history at all right now).

## P3 — does marking failed on an unmonitored season auto-fire a search?

**Not answered**, for the same reason. This is the safety-critical
question (the brief's "stop and surface to Alex" trigger), so it must
be re-run for real once data exists — do not assume "no auto-search"
from this session; nothing was tested.

## Bulk episodefile-delete route shape — ANSWERED (does not require GoT)

This one probe doesn't depend on any series existing, so it was run.
Confirmed **non-destructive**: both attempts throw in Sonarr's own
`GetFiles`/`Any()`/`First()` lookup logic before reaching any delete
code path — nothing was deleted server-side.

- `DELETE /api/v3/episodefile/bulk` with body `{}` →
  **HTTP 500**, `System.ArgumentNullException: Value cannot be null.
  (Parameter 'source')` — the request deserializes into a resource
  whose `episodeFileIds` is null, and the controller calls `.Any()` on
  it before validating.
- `DELETE /api/v3/episodefile/bulk` with body `{"episodeFileIds": []}` →
  **HTTP 500**, `System.InvalidOperationException: Sequence contains no
  elements` — it does look the ids up (`MediaFileService.GetFiles`), and
  something downstream (`.First()`/similar) on the empty result throws.

**Conclusions for Task 2/3's client code:**
- The route **exists** (not a 404 — this is real Sonarr v3 API surface,
  confirmed on this Sonarr version 4.0.19.2979).
- The expected body shape is `{"episodeFileIds": [<int>, ...]}`.
- Sonarr does **not** guard the empty-list case — it 500s rather than
  no-op-200 or 400. **The client must never call this endpoint with an
  empty id list**; callers are responsible for guarding that themselves
  (matches the design spec's stage `f`, which is reached only after
  filtering episode files to the target season — but that filter step
  must itself check for a non-empty result before calling bulk-delete).
- No signal yet on the response shape for a *valid* non-empty id list
  (200 body format, partial-failure semantics) — that still needs a
  probe against a real series/episode files once one exists.

## Fixtures

**Neither fixture file was created.** Fabricating
`sonarr_history_series.json` / `sonarr_episodefiles.json` from
imagined/synthetic data would be worse than leaving them absent — the
entire point of this task is empirical ground truth from the live
service, and fake fixtures would silently poison Task 2's parser tests
with wrong assumptions (exactly the failure mode the task brief opens
by warning about: "The TV phases repeatedly proved Sonarr API
assumptions wrong"). Task 2 cannot proceed on real fixtures until this
task is re-run successfully.

## Field-name / API surface notes gathered anyway

- Sonarr version on this box: `4.0.19.2979` (`packageVersion`
  `4.0.19.2979-ls320`, linuxserver.io image), `migrationVersion: 217`,
  `databaseVersion: 3.53.2`.
- `GET /api/v3/health` currently reports one unrelated warning:
  `IndexerLongTermStatusCheck` — EZTV (Prowlarr) unavailable >6h. Not
  related to the GoT deletion.
- `GET /api/v3/rootfolder` → one root, `/data/library/tv`, `accessible:
  true`, ~158 GB free. The mount itself is healthy; this was not a
  storage/mount failure.

## Recommendation for Alex

1. Confirm whether you (or another session/script) intentionally
   removed Game of Thrones from Sonarr around 2026-08-13T08:58Z. If so,
   re-add it (or another series with a real wrong-language/bad season)
   so Task 1 can be re-run against live grab/history data — the probes
   are meaningless without a season that actually has history, an
   imported file, and a real downloadId to mark failed and blocklist.
2. If this was **not** intentional, treat it as a standalone incident
   worth its own investigation (something issued
   `DELETE /api/v3/series/{id}?deleteFiles=true` against this Sonarr
   instance and also cleared qBittorrent's torrent list) — independent
   of whether Task 1 is re-run.
3. Task 1 must be re-executed in full (P1, P2, P3, both fixtures) before
   Task 2 starts. Nothing downstream should treat this session as having
   produced usable ground truth beyond the bulk-route shape finding
   above.
