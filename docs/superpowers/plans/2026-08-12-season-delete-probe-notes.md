# Season-delete probe notes (Task 1)

Date run: 2026-08-13, ~09:35-09:39 UTC. Box: magicpi5 (`magic@10.0.0.227`),
Sonarr v4.0.19.2979 at `localhost:8989`.

## REVISED RUN — 2026-08-13, ~09:50-10:15 UTC — Status: DONE_WITH_CONCERNS

The original run below was blocked: Game of Thrones (series id 7) had been
deleted from Sonarr before that session started. By the time this revised
run started, series id 7 had been **restored by the user** and was actively
downloading season 3 for real — it is explicitly OUT OF SCOPE for this run
(no monitoring changes, no mark-as-failed, no deletes; verified untouched
at the end, see Cleanup). Instead of touching series 7, this run created a
disposable throwaway series (**Breaking Bad, tvdbId 81189, Sonarr series id
8**), added `monitor: "none"` / `searchForMissingEpisodes: false`, then
manually monitored exactly one episode (S01E01, episode id 654), fired an
`EpisodeSearch`, waited for it to grab + import (took ~9 minutes — much
faster than the 45-minute poll budget), ran P1/P2/P3 against that series
and its real history/episode-file data, captured both fixtures from it,
and then fully deleted the throwaway series per the mandatory cleanup step.
Series 7 was never modified.

### P1 — does `/history/series` accept `seasonNumber` server-side? ANSWERED: YES

Confirmed with a differential test against series 8 (before AND after
import):
- `?seriesId=8&seasonNumber=1` → 1 record pre-import (`grabbed`), 2 records
  post-import (`grabbed` + `downloadFolderImported`).
- `?seriesId=8&seasonNumber=2` (a season with zero history) → **0 records**,
  both times.
- `?seriesId=8` with no `seasonNumber` param → same record count as
  `seasonNumber=1` (all of series 8's history happens to be season 1, so
  this alone wasn't conclusive; the season=2 differential is what proves
  the filter is server-side and not just "ignored").

**Surprise: the season number is NOT a field on the history record itself.**
Each record has no `seasonNumber` key and no nested `episode` object at
all — only `episodeId` (int). A record's raw shape (redacted, see fixture):
```json
{
  "episodeId": 654,
  "seriesId": 8,
  "sourceTitle": "Breaking Bad S01E01      BDrip 1080p",
  "eventType": "grabbed",
  "downloadId": "C1F8F37C7C583B1D68D2FB828E32CD973CF404F7",
  "data": { "...": "release/import metadata, see fixture" },
  "id": 164
}
```
This matters for the design: `get_season_history_checked` can rely
entirely on the server-side `seasonNumber` query param to scope results
(confirmed working) — it does **not** need to (and cannot, from this
endpoint alone) cross-reference `episodeId → episode.seasonNumber`
client-side. The design spec's assumption ("filters as expected") holds.

**Second surprise — credential leak in `data.downloadUrl` on `grabbed`
records.** The `grabbed` event's `data.downloadUrl` field is a live
Prowlarr NZB/download proxy URL with an embedded `apikey=<32-hex>` query
param (Prowlarr's own API key, not Sonarr's). This is real, live Sonarr
history data, not fixture-only — any consumer of this endpoint (including
a future debug log or UI that echoes raw history) must not print
`data.downloadUrl` verbatim. The committed fixture has this value redacted
to `apikey=REDACTED`; nothing else in the JSON was altered. **Action item
for Task 2/3:** the C++ history parser must not surface `data.downloadUrl`
in any client-visible or logged form.

### P2 — does `POST /api/v3/history/failed/{id}` return 200? ANSWERED: YES

`POST /api/v3/history/failed/164` (the season-1 `grabbed` record) →
**HTTP 200**, empty body (`{}`). Immediately after, `GET
/api/v3/blocklist?pageSize=20` showed a new entry:
```
id=3, seriesId=8, sourceTitle="Breaking Bad S01E01      BDrip 1080p", date=2026-08-13T10:05:24Z
```
Confirmed both the status code and the blocklist side effect exactly as
the design spec's stage (d) expects.

### P3 — does mark-as-failed on an UNMONITORED season auto-fire a search? ANSWERED: YES — CONCERN, read this before building stage (a)/(d)

**A search DID fire.** Sequence: (1) unmonitored season 1 via `PUT
/series/8` (season-level `monitored: false`, mirroring the design's stage
(a) exactly — same shape as the worker's planned `set_season_monitored`),
confirmed `season.monitored: false` in the PUT response; (2) waited 2s;
(3) `POST /history/failed/164` → 200; (4) polled `GET /api/v3/command` and
found a **new** command that did not exist in the pre-mark-failed baseline:
```
id=29116, name="EpisodeSearch", trigger="unspecified", status="completed",
message="Episode search completed. 0 reports downloaded.",
body.episodeIds=[654]
```
**Root cause, confirmed via `GET /api/v3/config/downloadclient`:**
`autoRedownloadFailed: true` (and `autoRedownloadFailedFromInteractiveSearch:
true`) — this is Sonarr's global default, and it is what fires the search.

**Why unmonitoring the SEASON did not prevent it:** Sonarr's season-level
`monitored` flag (set via `PUT /series`) and each episode's own
`monitored` flag are independent, separately-stored booleans. Setting
`season.monitored = false` does **not** cascade down and flip
`episode.monitored` for episodes already inside it. Episode 654 had been
explicitly set `monitored: true` earlier (required to fire the initial
search) and **stayed `true`** through the season-unmonitor step, because
nothing in the PUT touched it. `autoRedownloadFailed`'s trigger appears to
key off episode-level (not season-level) monitored state, so it fired
regardless of the season flag.

**Practical impact observed:** the auto-fired search found "0 reports
downloaded" — no new grab actually landed (the only known release was the
one just blocklisted) — so nothing was silently re-downloaded in this
specific run. That is a fact about this run's indexer results, not a
guarantee; a season with alternate releases available would behave
differently.

**This is the "stop and surface to Alex" trigger the brief calls out.**
Recommendation for Task 3/6 (per the design spec's own contingency, spec
line ~72): stage (a) `set_season_monitored(id, N, false)` as currently
specified is **insufficient on its own**. Before implementing stage (d)
(`mark_history_failed`), the worker needs one of:
1. Stage (a) also explicitly unmonitors every episode in the season (not
   just the season container) — likely the cleaner fix, symmetric with
   how episodes get explicitly monitored one-at-a-time elsewhere in this
   codebase (SeriesDetailScreen's per-episode picker); or
2. Temporarily flip `autoRedownloadFailed` off in `/config/downloadclient`
   for the duration of stage (d), then restore it — riskier (global
   config mutation, must survive a crash mid-stage) and not recommended
   unless (1) proves insufient.

Flagging this explicitly for Alex per the brief's instruction — this
changes stage (a)'s scope from "unmonitor season" to "unmonitor season AND
its episodes," which Task 3/6 must account for before implementation.

### Fixtures — captured from series 8 (Breaking Bad), CREATED

- `magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_history_series.json`
  — live capture of `GET /history/series?seriesId=8&seasonNumber=1`
  post-import (2 records: `grabbed` id=164, `downloadFolderImported`
  id=185). Credential-scrubbed: `data.downloadUrl`'s `apikey=<hex>` value
  replaced with `apikey=REDACTED`; nothing else altered. Verified valid
  JSON, verified clean of `apikey|api_key|x-api-key|password|secret`
  (aside from the intentional `REDACTED` placeholder) before commit.
- `magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_episodefiles.json`
  — live capture of `GET /episodefile?seriesId=8`. One record, `id: 41`,
  `seasonNumber: 1` present directly on the object (confirms the brief's
  expected shape — episodefile records DO carry `seasonNumber` inline,
  unlike history records). No credentials present; verified clean as-is.

Both fixtures reflect a non-English (`languages: [{"name": "Chinese"}]`)
release with a negative `customFormatScore` (-10000, "Non-English title
signals") — coincidentally similar in spirit to the original GoT
wrong-language scenario this whole feature targets, though picked for
convenience (fast download) rather than deliberately.

### Bulk episodefile-delete route — unchanged, see original section below

Not re-probed this run (already answered non-destructively in the blocked
run and doesn't depend on which series exists). That finding stands as
written in the "Bulk episodefile-delete route shape" section further down
this document.

### Additional surprise (informational, outside P1-P3 scope): `GET /api/v3/queue` does not filter by `seriesId`

While confirming no series-8 rows were left in the queue post-import,
`GET /api/v3/queue?seriesId=8` returned all 9 in-flight rows for series 7
(GoT), not an empty/filtered set — the `seriesId` query param appears to
be ignored by this endpoint (unlike `/history/series`, where it works).
Client-side filtering (`records[].seriesId == target`) was used instead
to safely confirm zero series-8 rows without touching series 7's real
rows. **This matters for the design's stage (c)** ("cancel this season's
queue rows") — if the real worker code assumes `?seriesId=` scopes the
queue server-side, it will need to filter client-side instead, the same
way this probe did. Worth a follow-up probe in Task 2/3 with a larger
queue if the assumption isn't already handled that way in the existing
`cancel_queue_item`/queue-fetch code.

### Cleanup — executed and verified

Per the revision's mandatory last step:
1. Queue rows for series 8: **0** (episode had already imported and left
   the queue naturally; nothing to cancel).
2. qBittorrent torrent (`C1F8F37C7C583B1D68D2FB828E32CD973CF404F7`,
   lowercased for the qBit API) deleted via `POST
   /api/v2/torrents/delete?hashes=...&deleteFiles=true` → HTTP 200;
   re-queried immediately after, **0 torrents** with that hash remain.
3. `DELETE /api/v3/series/8?deleteFiles=true` → HTTP 200.
4. Verified: `GET /series` now lists only series 7 (Game of Thrones);
   series 8 is gone. `GET /episodefile?seriesId=8` → 404 "Series with ID
   8 does not exist" (expected — the series itself is gone, this is not a
   stray file). `GET /episodefile/41` (the specific imported file) → 404.
   `docker exec mdb_sonarr find /data/library/tv /data/downloads -iname
   "*breaking*bad*"` → no results, disk is clean. qBit's full torrent list
   (10 torrents remaining, all GoT-related) has no "breaking" match.
5. **Blocklist entry did NOT survive series deletion** — this is the one
   piece of data the brief asked to record either way. Before deleting
   series 8, `GET /blocklist` showed 2 records (id=2 for series 7,
   pre-existing/unrelated to this run; id=3 for series 8, from P2 above).
   After `DELETE /series/8?deleteFiles=true`, `GET /blocklist` shows only
   **1** record (id=2, series 7) — Sonarr appears to cascade-delete
   blocklist entries when their owning series is deleted. Implication for
   the real per-SEASON-delete feature: since that flow deletes files
   within a season but keeps the SERIES itself, this cascade should not
   apply there and the blocklist protection should persist — but this is
   worth a targeted follow-up probe in Task 2/3 (delete a season, not a
   whole series, and re-check blocklist survival) rather than assumed.
6. Series 7 (GoT) sanity-checked untouched throughout and at the end:
   `monitored: true`, season 3 `monitored: true`, still 9 active queue
   rows downloading. Never modified by this session.

---

## Status: BLOCKED (original run, preserved below for history)

The task's precondition — "GoT (with its wrong-language season 3) present
in the library" — is false. Sonarr's series list is **empty (0 series)**.
The Game of Thrones series record and its files were deleted from Sonarr
shortly before this probe session started, by something other than this
task. P1/P2/P3 could not be answered against real data as a result. Full
evidence below.

**Superseded by the REVISED RUN section above** — GoT (series 7) has
since been restored by the user; P1/P2/P3 have real answers now, from a
disposable test series (see above). This section is kept verbatim as the
historical record of the GoT-deletion incident, which is independent
knowledge worth keeping regardless of the probe re-run.

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
