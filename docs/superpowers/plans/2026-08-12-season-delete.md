# Per-Season Delete Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A "Delete Season N…" row in the episode picker that removes one season's files, blocklists the release(s) that produced them, and leaves the season unmonitored — re-download stays manual via the existing "Start Season N".

**Architecture:** Season-scoped mirror of the whole-series orphan-proof Remove: four new checked-shape SonarrClient calls, one pure-logic helper set (Mac-tested), a trailing action row in SeriesDetailScreen's Episodes region, and one `spawn_mutation` body with destructive steps last. Spec: `docs/superpowers/specs/2026-08-12-season-delete-design.md`.

**Tech Stack:** C++17 kiosk (`magic_dingus_box_cpp/`), jsoncpp, Catch2 Mac test suites, live Sonarr v3 on magicpi5 for probes/fixtures.

## Global Constraints

- Repo root: `/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box ` — the directory name has a TRAILING SPACE; always quote paths.
- Board differences resolve at runtime; never `#ifdef` a board (CLAUDE.md dual-board contract). This feature is board-agnostic.
- MB screens compile ONLY into the kiosk binary — Mac suites cannot compile `series_detail_screen.cpp`. UI logic that needs Mac tests goes in `series_detail_logic.h` (pure, Renderer-free).
- Mac test build dir: `magic_dingus_box_cpp/build-mb` (MB=ON). CORRECTED 2026-08-13: there is NO per-file test binary — `test_sonarr_parsers.cpp`, `test_sonarr_client.cpp`, and `test_series_detail_logic.cpp` all compile into the single `test_media_browser_unit`. Run it directly (ctest skips it). Baseline before this feature: 6358 assertions / 392 cases.
- In-band verdicts: client methods clear `set_error({})` on entry and callers read the RETURN, never `last_error()` (shared with the ~9s background re-poll — the 2c-3 contract).
- Checked shapes: `nullopt` = transport/HTTP failure; engaged-but-empty = authoritative "none".
- ONE mutation at a time via `spawn_mutation` (WatchdogSec=10 — nothing blocking on the render thread).
- Never print secrets (API keys read from `/opt/magic_dingus_box/services/.env` on the box; use `$SONARR_API_KEY` from env in probe commands, never echo it).
- Commits: conventional style, end with `Co-Authored-By:` line per session convention.

---

### Task 1: Probe Sonarr's API on magicpi5 and capture fixtures

The TV phases (2a/2b/2c) repeatedly proved Sonarr API assumptions wrong; nothing is implemented until these three questions are answered from the LIVE service. No code changes in this task — its deliverable is fixture files + a probe-notes doc.

**Files:**
- Create: `magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_history_series.json`
- Create: `magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_episodefiles.json`
- Create: `docs/superpowers/plans/2026-08-12-season-delete-probe-notes.md`

**Interfaces:**
- Consumes: SSH to `magic@10.0.0.227`; Sonarr at `localhost:8989` on the box; GoT (with its wrong-language season 3) present in the library.
- Produces: the two fixture JSONs Task 2's parser tests embed, plus written answers to P1–P3 that Task 3/6 depend on.

- [ ] **Step 1: Find the series id and confirm the season-history endpoint (P1)**

```bash
ssh magic@10.0.0.227 '
  set -a; . /opt/magic_dingus_box/services/.env; set +a
  SID=$(curl -fsS --max-time 10 -H "X-Api-Key: $SONARR_API_KEY" \
    "http://localhost:8989/api/v3/series" \
    | python3 -c "import json,sys; s=[x for x in json.load(sys.stdin) if \"game of thrones\" in x[\"title\"].lower()]; print(s[0][\"id\"])")
  echo "SID=$SID"
  # P1a: does /history/series accept seasonNumber server-side?
  curl -fsS --max-time 10 -H "X-Api-Key: $SONARR_API_KEY" \
    "http://localhost:8989/api/v3/history/series?seriesId=$SID&seasonNumber=3" \
    | python3 -c "import json,sys; d=json.load(sys.stdin); print(\"records:\", len(d)); print(\"seasons seen:\", sorted({r.get(\"episode\",{}).get(\"seasonNumber\", r.get(\"seasonNumber\",-1)) for r in d}))"
'
```
Expected: a record count and `seasons seen: [3]` (server-side filter works) OR mixed seasons (client-side filtering needed — note which field carries the season). Record the answer in the probe notes.

- [ ] **Step 2: Capture the history fixture**

```bash
ssh magic@10.0.0.227 '
  set -a; . /opt/magic_dingus_box/services/.env; set +a
  curl -fsS --max-time 10 -H "X-Api-Key: $SONARR_API_KEY" \
    "http://localhost:8989/api/v3/history/series?seriesId=<SID from step 1>"
' > "magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_history_series.json"
python3 -m json.tool "magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_history_series.json" | head -40
```
Expected: valid JSON array; note per-record fields actually present: `id`, `eventType`, `downloadId`, and where the season number lives (`seasonNumber` directly, or `episode.seasonNumber`). **Scrub check:** grep the fixture for `apikey`/`ApiKey` — history records must not embed credentials; if any appear, strip those fields before committing.

- [ ] **Step 3: Probe mark-as-failed semantics (P2 + P3) on a THROWAWAY record**

P2: does `POST /api/v3/history/failed/{id}` exist and return 200? P3: with the season UNMONITORED, does marking failed trigger a search? Use the wrong-language season 3 grab — it is the record we want blocklisted anyway, and season 3 will be deleted in the acceptance test regardless.

```bash
ssh magic@10.0.0.227 '
  set -a; . /opt/magic_dingus_box/services/.env; set +a
  # Unmonitor season 3 first (mirrors worker stage a):
  python3 - <<PY
import json, os, urllib.request
key=os.environ["SONARR_API_KEY"]; base="http://localhost:8989/api/v3"
def req(method, path, body=None):
    r=urllib.request.Request(base+path, method=method,
        headers={"X-Api-Key":key,"Content-Type":"application/json"},
        data=None if body is None else json.dumps(body).encode())
    with urllib.request.urlopen(r, timeout=10) as resp: return resp.status, resp.read()
sid=<SID>
st, raw = req("GET", f"/series/{sid}"); s=json.loads(raw)
for season in s["seasons"]:
    if season["seasonNumber"]==3: season["monitored"]=False
print("PUT series:", req("PUT", f"/series/{sid}", s)[0])
st, raw = req("GET", f"/history/series?seriesId={sid}")
hist=json.loads(raw)
grabs=[r for r in hist if r["eventType"]=="grabbed"]  # find season-3 grab id by the field step 1 identified
print("candidate grab ids:", [r["id"] for r in grabs][:5])
PY
'
```
Then POST the season-3 grab's id to `/api/v3/history/failed/<id>` (same header pattern), print the HTTP status, and immediately check `GET /api/v3/blocklist?pageSize=20` for the release title and `GET /api/v3/command` (recent) for any auto-fired search.
Expected recorded answers: P2 = status code + blocklist entry present/absent; P3 = whether a `SeasonSearch`/`EpisodeSearch` command appeared (expected: none, season unmonitored). If P3 shows a search DID fire, the worker's stage order (unmonitor first) is insufficient — record what config drives it (`GET /api/v3/config/downloadclient` → `autoRedownloadFailed`) and note that stage (d) must temporarily disable it or the design needs revisiting. **Stop and surface to Alex if so.**

- [ ] **Step 4: Capture the episode-files fixture**

```bash
ssh magic@10.0.0.227 '
  set -a; . /opt/magic_dingus_box/services/.env; set +a
  curl -fsS --max-time 10 -H "X-Api-Key: $SONARR_API_KEY" \
    "http://localhost:8989/api/v3/episodefile?seriesId=<SID>"
' > "magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_episodefiles.json"
python3 -m json.tool "magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_episodefiles.json" | head -20
```
Expected: array of objects each with `id` and `seasonNumber`. Also probe the bulk delete endpoint's SHAPE without executing it: `curl -fsS -X DELETE .../api/v3/episodefile/bulk` with an EMPTY body and record the error status/message (404 route-missing vs 400 bad-body tells us the route exists).

- [ ] **Step 5: Write probe notes + commit**

Write `docs/superpowers/plans/2026-08-12-season-delete-probe-notes.md` with: SID, P1 answer (server-side seasonNumber param? which field carries season), P2 answer (status, blocklist confirmed), P3 answer (no auto-search / what was found), bulk-route answer, and any field-name surprises the fixtures show.

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box "
git add magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_history_series.json \
        magic_dingus_box_cpp/tests/media_browser/fixtures/sonarr_episodefiles.json \
        docs/superpowers/plans/2026-08-12-season-delete-probe-notes.md
git commit -m "test(tv): capture live Sonarr history + episodefile fixtures; season-delete probe notes

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Season-history + episode-file parsers

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_types.h` (append near `EpisodeInfo`)
- Modify: `magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_parsers.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_parsers.cpp`
- Test: `magic_dingus_box_cpp/tests/media_browser/test_sonarr_parsers.cpp` (append)

**Interfaces:**
- Consumes: Task 1 fixtures (embed representative excerpts as string literals — the suite's existing convention; reconcile field names against the captured files first).
- Produces:
```cpp
// sonarr_types.h
struct SeasonHistory {
    std::vector<int> grabbed_history_ids;     // eventType == "grabbed"
    std::vector<int> imported_history_ids;    // eventType == "downloadFolderImported"
    std::vector<std::string> download_hashes; // distinct, lowercased
};
struct EpisodeFileInfo {
    int id = 0;
    int season_number = 0;
};
// sonarr_parsers.h (static members of SonarrParsers)
// PROBE AMENDMENT (2026-08-13): /history/series records carry NO season
// field — season scoping happens SERVER-SIDE via &seasonNumber=N (probe
// P1 confirmed it filters correctly). The parser therefore takes the
// response as already season-scoped and does NOT filter:
static SeasonHistory parse_season_history(const std::string& json);
static std::vector<EpisodeFileInfo> parse_episode_files(const std::string& json);
```

- [ ] **Step 1: Write the failing tests** (append to `test_sonarr_parsers.cpp`; adjust field paths to what Task 1's fixtures actually showed — the excerpts below assume `episode.seasonNumber` fallback to top-level `seasonNumber`, which is what the probe must confirm)

```cpp
TEST_CASE("parse_season_history buckets by eventType (input is season-scoped)") {
    // Shape from the live capture (fixtures/sonarr_history_series.json —
    // captured WITH &seasonNumber, so records carry no season field).
    // downloadId deliberately mixed-case — parser lowercases. NOTE the
    // grabbed record's data.downloadUrl carries a REDACTED token in the
    // fixture (live Prowlarr key in the real response — never log these).
    const std::string json = R"([
      {"id": 501, "eventType": "grabbed", "downloadId": "ABCDEF123456",
       "data": {"downloadUrl": "http://localhost:9696/2/download?apikey=REDACTED"}},
      {"id": 502, "eventType": "downloadFolderImported", "downloadId": "ABCDEF123456"}
    ])";
    auto h = media_browser::SonarrParsers::parse_season_history(json);
    REQUIRE(h.grabbed_history_ids == std::vector<int>{501});
    REQUIRE(h.imported_history_ids == std::vector<int>{502});
    REQUIRE(h.download_hashes == std::vector<std::string>{"abcdef123456"});
}

TEST_CASE("parse_season_history dedupes hashes and tolerates junk") {
    const std::string json = R"([
      {"id": 1, "eventType": "grabbed", "downloadId": "AAAA"},
      {"id": 2, "eventType": "grabbed", "downloadId": "AAAA"},
      {"id": 3, "eventType": "grabbed"},
      "not-an-object"
    ])";
    auto h = media_browser::SonarrParsers::parse_season_history(json);
    REQUIRE(h.grabbed_history_ids == std::vector<int>{1, 2});
    REQUIRE(h.download_hashes == std::vector<std::string>{"aaaa"});
}

TEST_CASE("parse_season_history on malformed json yields empty") {
    auto h = media_browser::SonarrParsers::parse_season_history("{nope");
    REQUIRE(h.grabbed_history_ids.empty());
    REQUIRE(h.imported_history_ids.empty());
    REQUIRE(h.download_hashes.empty());
}

TEST_CASE("parse_episode_files extracts id + seasonNumber") {
    const std::string json = R"([
      {"id": 11, "seasonNumber": 1, "path": "/data/library/tv/x/S01E01.mkv"},
      {"id": 33, "seasonNumber": 3},
      {"noid": true}
    ])";
    auto files = media_browser::SonarrParsers::parse_episode_files(json);
    REQUIRE(files.size() == 2);
    REQUIRE(files[0].id == 11);
    REQUIRE(files[0].season_number == 1);
    REQUIRE(files[1].id == 33);
    REQUIRE(files[1].season_number == 3);
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp/build-mb" && cmake --build . -j8 2>&1 | grep -E "error" | head
```
Expected: compile errors — `SeasonHistory`/`parse_season_history` not declared.

- [ ] **Step 3: Implement** (`sonarr_parsers.cpp`, following the file's existing jsoncpp reader idiom; season read prefers the field Task 1 confirmed, with the other as fallback)

```cpp
SeasonHistory SonarrParsers::parse_season_history(const std::string& json) {
    // Input is ALREADY season-scoped: the client queries
    // /history/series?seriesId=X&seasonNumber=N (probe-verified — the
    // records themselves carry no season field, so scoping cannot be
    // re-done here). Never log record contents: grabbed records embed a
    // live Prowlarr API key in data.downloadUrl.
    SeasonHistory out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;  // house helper
    std::set<std::string> seen_hashes;
    for (const auto& r : root) {
        if (!r.isObject()) continue;
        const int id = r.get("id", 0).asInt();
        const std::string ev = r.get("eventType", "").asString();
        if (id > 0 && ev == "grabbed") out.grabbed_history_ids.push_back(id);
        if (id > 0 && ev == "downloadFolderImported")
            out.imported_history_ids.push_back(id);
        std::string dl = r.get("downloadId", "").asString();
        std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);
        if (!dl.empty() && seen_hashes.insert(dl).second)
            out.download_hashes.push_back(dl);
    }
    return out;
}

std::vector<EpisodeFileInfo> SonarrParsers::parse_episode_files(
        const std::string& json) {
    std::vector<EpisodeFileInfo> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        if (!r.isObject()) continue;
        EpisodeFileInfo f;
        f.id = r.get("id", 0).asInt();
        f.season_number = r.get("seasonNumber", 0).asInt();
        if (f.id > 0) out.push_back(f);
    }
    return out;
}
```
(If the file has no `parse_json` helper, use its existing inline `Json::CharReaderBuilder` pattern verbatim.)

- [ ] **Step 4: Build + run**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp/build-mb" && cmake --build . -j8 2>&1 | tail -2 && ./test_media_browser_unit 2>&1 | tail -2
```
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box "
git add magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_types.h \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_parsers.h \
        magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_parsers.cpp \
        magic_dingus_box_cpp/tests/media_browser/test_sonarr_parsers.cpp
git commit -m "feat(tv): season-history + episode-file parsers for per-season delete

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: SonarrClient season-delete surface (+ mock mirror)

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_client.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/sonarr/sonarr_client.cpp`
- Modify: the mock (find with `grep -rn "class SonarrMockClient" magic_dingus_box_cpp/`) — mirror every new virtual
- Test: `magic_dingus_box_cpp/tests/media_browser/test_sonarr_client.cpp` (append, using the suite's existing http_*-stubbing subclass pattern)

**Interfaces:**
- Consumes: Task 2 types/parsers.
- Produces (exact signatures Tasks 4–6 use):
```cpp
virtual std::optional<SeasonHistory> get_season_history_checked(int sonarr_id, int season_number);
virtual bool mark_history_failed(int history_id);          // POST /api/v3/history/failed/{id}, empty body
virtual std::optional<std::vector<EpisodeFileInfo>> get_episode_files_checked(int sonarr_id);
virtual bool delete_episode_files(const std::vector<int>& ids);  // DELETE /api/v3/episodefile/bulk
virtual bool cancel_queue_item(int queue_id, bool blocklist);    // NEW overload; existing 1-arg forwards with false
virtual bool set_episodes_monitored(const std::vector<int>& ids, bool monitored);
    // PUT /api/v3/episode/monitor {"episodeIds":[...],"monitored":bool}
    // PROBE AMENDMENT: episode.monitored is INDEPENDENT of
    // season.monitored, and auto-redownload-on-failed keys off the
    // EPISODE flag — stage (a) and the Start-Season re-monitor need this.
    // Empty ids = vacuous true, no HTTP call (same rule as
    // delete_episode_files).
```
`get_season_history_checked` builds the path
`"/api/v3/history/series?seriesId=" + id + "&seasonNumber=" + season` —
the server-side filter is REQUIRED (records carry no season field), and
its test asserts that exact path. Add a test that
`set_episodes_monitored({4,5}, false)` PUTs to `/api/v3/episode/monitor`
with both ids and `"monitored":false` in the body, and `{}` → true with
no HTTP call.

- [ ] **Step 1: Failing tests** (append; mirror the file's existing transport-stub subclass — it overrides the protected `http_get`/`http_post`/`http_delete` and records paths)

```cpp
TEST_CASE("get_season_history_checked: transport failure is nullopt, answer is engaged") {
    StubSonarrClient c;                       // the suite's existing stub subclass
    c.next_get_response = "";                 // transport failure
    REQUIRE_FALSE(c.get_season_history_checked(7, 3).has_value());
    c.next_get_response = "[]";               // Sonarr answered: no history
    auto h = c.get_season_history_checked(7, 3);
    REQUIRE(h.has_value());
    REQUIRE(h->download_hashes.empty());
    REQUIRE(c.last_get_path == "/api/v3/history/series?seriesId=7&seasonNumber=3");
}

TEST_CASE("mark_history_failed posts to /history/failed/{id}") {
    StubSonarrClient c;
    c.next_post_response = "{}";
    REQUIRE(c.mark_history_failed(501));
    REQUIRE(c.last_post_path == "/api/v3/history/failed/501");
}

TEST_CASE("delete_episode_files bulk body carries every id") {
    StubSonarrClient c;
    c.next_delete_code = 200;
    REQUIRE(c.delete_episode_files({31, 32, 33}));
    REQUIRE(c.last_delete_path == "/api/v3/episodefile/bulk");
    REQUIRE(c.last_delete_body.find("31") != std::string::npos);
    REQUIRE(c.delete_episode_files({}) == true);  // vacuous success, no HTTP call
}

TEST_CASE("cancel_queue_item blocklist variant carries stall-reaper params") {
    StubSonarrClient c;
    c.next_delete_code = 200;
    REQUIRE(c.cancel_queue_item(42, /*blocklist=*/true));
    REQUIRE(c.last_delete_path ==
        "/api/v3/queue/42?removeFromClient=true&blocklist=true&skipRedownload=true");
    REQUIRE(c.cancel_queue_item(42));  // legacy call unchanged
    REQUIRE(c.last_delete_path ==
        "/api/v3/queue/42?removeFromClient=true&blocklist=false");
}
```
NOTE: if the existing `http_delete` takes no body, `delete_episode_files` needs a body-carrying delete. Check first: `grep -n "http_delete" sonarr_client.h`. If body-less, add a protected `virtual long http_delete_body(const std::string& path, const std::string& body)` implemented with the same curl setup + `CURLOPT_CUSTOMREQUEST "DELETE"` + POSTFIELDS, and stub it in tests like the others.

- [ ] **Step 2: Build → expect compile failures** (same build command as Task 2 Step 2).

- [ ] **Step 3: Implement in `sonarr_client.cpp`** (each follows the house idiom — `set_error({})` entry clear, in-band verdict):

```cpp
std::optional<SeasonHistory>
SonarrClient::get_season_history_checked(int sonarr_id, int season_number) {
    // Same doctrine as get_series_download_hashes_checked (see that
    // comment): entry clear, nullopt = transport failure, engaged-empty =
    // real "no history". Season filtering is CLIENT-side — one fetch, one
    // parse — regardless of whether the server honours seasonNumber
    // (probe P1), so behavior cannot drift with Sonarr versions.
    set_error({});
    auto resp = http_get("/api/v3/history/series?seriesId="
                         + std::to_string(sonarr_id));
    if (resp.empty()) return std::nullopt;
    return SonarrParsers::parse_season_history(resp, season_number);
}

// CORRECTED 2026-08-13 (this snippet originally read `return
// last_error().empty();`). That splits the verdict across two calls,
// which the plan's own Global Constraints forbid — last_error_ is ONE
// member shared with the ~9s background series re-poll, and
// series_detail_screen already runs poll_worker_ and mut_worker_
// concurrently against this client, so the window is a full HTTP
// round-trip wide in BOTH directions (a poll's error failing a real
// success; a poll's entry-clear making a real failure look like
// success — Task 6 would then believe a release was blocklisted when
// it was not). The snippet's justifying comment was also factually
// wrong: probe P2 showed this endpoint answers 200 with an EMPTY body,
// so http_post's body-only return cannot distinguish success from
// failure at all. Fix mirrors how this same commit solved the
// identical ambiguity for DELETE (http_delete_body ← http_delete):
bool SonarrClient::mark_history_failed(int history_id) {
    set_error({});
    const long code = http_post_status(
        "/api/v3/history/failed/" + std::to_string(history_id), "");
    return code > 0 && code < 400;
}

std::optional<std::vector<EpisodeFileInfo>>
SonarrClient::get_episode_files_checked(int sonarr_id) {
    set_error({});
    auto resp = http_get("/api/v3/episodefile?seriesId="
                         + std::to_string(sonarr_id));
    if (resp.empty()) return std::nullopt;
    return SonarrParsers::parse_episode_files(resp);
}

bool SonarrClient::delete_episode_files(const std::vector<int>& ids) {
    if (ids.empty()) return true;  // nothing to delete is success, not a call
    set_error({});
    Json::Value body(Json::objectValue);
    Json::Value arr(Json::arrayValue);
    for (int id : ids) arr.append(id);
    body["episodeFileIds"] = arr;
    const long code = http_delete_body("/api/v3/episodefile/bulk",
                                       Json::FastWriter().write(body));
    return code > 0 && code < 400;
}

bool SonarrClient::cancel_queue_item(int queue_id, bool blocklist) {
    set_error({});
    // blocklist=true carries the stall-reaper's full semantics: blocklist
    // the release AND don't let Sonarr immediately re-grab it.
    const std::string path = blocklist
        ? "/api/v3/queue/" + std::to_string(queue_id)
              + "?removeFromClient=true&blocklist=true&skipRedownload=true"
        : "/api/v3/queue/" + std::to_string(queue_id)
              + "?removeFromClient=true&blocklist=false";
    const long code = http_delete(path);
    return code > 0 && code < 400;
}
bool SonarrClient::cancel_queue_item(int queue_id) {
    return cancel_queue_item(queue_id, /*blocklist=*/false);
}
```
Header: declare all five (the 2-arg cancel beside the existing 1-arg), plus `http_delete_body` if needed. Mirror every new virtual in `SonarrMockClient` (simple recorded-call + canned-return members, matching its existing style). If `mark_history_failed`'s empty-body POST returns a body the existing `http_post` treats as failure, adjust per what probe P2 recorded.

- [ ] **Step 4: Build + run** `./test_media_browser_unit` → `All tests passed`, and `./test_media_browser_unit` still green (mock signature change).

- [ ] **Step 5: Commit**

```bash
git add magic_dingus_box_cpp/src/media_browser/sonarr/ magic_dingus_box_cpp/tests/media_browser/
git commit -m "feat(tv): SonarrClient season-delete surface — season history, mark-failed, episodefile bulk, blocklisting cancel

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Pure logic — season cancel ids + delete-row model

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/series_detail_logic.h` (beside `cancel_ids_for_series` at ~line 194)
- Test: `magic_dingus_box_cpp/tests/media_browser/test_series_detail_logic.cpp` (append)

**Interfaces:**
- Consumes: `SonarrQueueItem` (has `series_id`, `season_number`, `download_id`… check the dedupe key `cancel_ids_for_series` uses and reuse it).
- Produces:
```cpp
// Queue rows to cancel for ONE season — same one-per-download dedupe as
// cancel_ids_for_series (a season pack is N rows sharing one downloadId).
inline std::vector<int> cancel_ids_for_season(
    const std::vector<SonarrQueueItem>& queue, int series_id, int season_number);

// Whether the trailing "Delete Season N…" row exists in the picker.
inline bool season_delete_row_exists(int episode_file_count, bool season_downloading);

// The row's label in its three states.
enum class SeasonDeleteState { Idle, Armed, Removing };
inline std::string season_delete_label(SeasonDeleteState s, int season_number);
```

- [ ] **Step 1: Failing tests**

```cpp
TEST_CASE("cancel_ids_for_season: only this series+season, one id per download") {
    using media_browser::SonarrQueueItem;
    SonarrQueueItem a; a.id=1; a.series_id=7; a.season_number=3; a.download_id="x";
    SonarrQueueItem b; b.id=2; b.series_id=7; b.season_number=3; b.download_id="x";  // same pack
    SonarrQueueItem c; c.id=3; c.series_id=7; c.season_number=1; c.download_id="y";  // other season
    SonarrQueueItem d; d.id=4; d.series_id=9; d.season_number=3; d.download_id="z";  // other series
    auto ids = media_browser::ui::cancel_ids_for_season({a,b,c,d}, 7, 3);
    REQUIRE(ids == std::vector<int>{1});
}

TEST_CASE("season_delete_row_exists: files OR live downloads") {
    using media_browser::ui::season_delete_row_exists;
    REQUIRE(season_delete_row_exists(5, false));
    REQUIRE(season_delete_row_exists(0, true));
    REQUIRE_FALSE(season_delete_row_exists(0, false));
}

TEST_CASE("season_delete_label three states") {
    using media_browser::ui::SeasonDeleteState;
    using media_browser::ui::season_delete_label;
    REQUIRE(season_delete_label(SeasonDeleteState::Idle, 3)    == "Delete Season 3\xE2\x80\xA6");
    REQUIRE(season_delete_label(SeasonDeleteState::Armed, 3)   == "Confirm delete Season 3");
    REQUIRE(season_delete_label(SeasonDeleteState::Removing, 3)== "Removing season\xE2\x80\xA6");
}
```
(If `SonarrQueueItem` has no `download_id` member, look at what `cancel_ids_for_series` dedupes on — release `title` per the struct comment "identical across a pack's rows" — and dedupe on the same field; adjust the test accordingly.)

- [ ] **Step 2: Build → fail. Step 3: Implement** (mirror `cancel_ids_for_series`'s body with the season filter added; label/exists helpers are 3-liners). **Step 4: Build + `./test_media_browser_unit` green. Step 5: Commit** `feat(tv): pure season-delete logic — cancel ids, row eligibility, labels`.

---

### Task 5: Episode picker UI — trailing delete row (arm/confirm only, no worker yet)

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp` (Episodes-region input ~lines 1355–1410, `render_episode_region`, footer hints ~line 2010)

**Interfaces:**
- Consumes: Task 4 helpers.
- Produces (members Task 6 consumes):
```cpp
bool season_del_armed_ = false;
std::chrono::steady_clock::time_point season_del_armed_at_{};
static constexpr int kSeasonDelConfirmMs = 4000;   // matches kWholeConfirmMs
bool season_del_inflight_ = false;   // render-thread; set at spawn, cleared at drain
bool season_delete_row_present() const;   // Task 4 helper fed from rows_/downloading set
int  episode_nav_count() const;           // episodes + the delete row when present
```

Mechanics (all in the existing idioms — no new patterns):
- `episode_nav_count()` = `season_episode_indices(episodes_season_).size() + (season_delete_row_present() ? 1 : 0)`. Every place the Episodes region clamps/pages on `n` (rotary at ~1363, PREV/NEXT paging at ~1374/1380, page-count computation at ~1358) uses `episode_nav_count()` instead.
- Focus index == episode count → the delete row is focused. SELECT dispatch (~1398): focused-on-delete-row + Idle → arm (`season_del_armed_ = true`, stamp time); + Armed → Task 6's spawn (this task: leave a `// Task 6` no-op that just disarms); episode rows unchanged.
- Disarm in `expire_confirms()` (existing per-frame call) when `now - season_del_armed_at_ > kSeasonDelConfirmMs`, and whenever focus moves off the row, whenever `region_` leaves Episodes, and in `fetch()`.
- Mutual exclusion: SELECT on the row is inert while `remove_pending_` or `mut_in_flight_`; conversely arming whole-series Remove is unchanged (spawn_mutation's one-at-a-time already serializes the workers — this gate is about not ARMING two confirms at once, mirror the `remove_pending_` clear at ~line 675 to also clear `season_del_armed_`).
- Render: `render_episode_region` draws one extra row after the last episode row using `season_delete_label(state, episodes_season_)` — Idle in the theme's danger/red accent used by the Remove button, Armed brighter (copy the Remove/ConfirmRemove paint), Removing dimmed. Focus ring identical to episode rows.
- Footer: RotaryPress hint reads `"Select"` instead of `"Play"` when the delete row is focused (ternary at ~line 2023).

- [ ] **Step 1: Implement** exactly the above. There is no Mac-compilable test for this file (Global Constraints) — correctness here is the pure helpers (Task 4, already tested) plus hardware eyes in Task 7.
- [ ] **Step 2: Compile-verify on the Pi** (scratch build, NEVER deploy_cpp defaults):

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp"
rsync -az --checksum --exclude 'build*' --exclude '.git' --exclude 'data/roms' --exclude 'data/media' \
  --exclude 'data/thumbnails' --exclude 'data/saves' --exclude 'data/states' \
  --exclude '*.mp4' --exclude '*.mkv' --exclude '*.mov' \
  ./ magic@10.0.0.227:~/mdb_scratch_seasondel/
ssh magic@10.0.0.227 'cd ~/mdb_scratch_seasondel && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_MEDIA_BROWSER=ON -DBUILD_TESTS=OFF .. >cfg.log 2>&1 && make -j3 2>&1 | tail -3'
```
Expected: `Built target magic_dingus_box_cpp`.
- [ ] **Step 3: Commit** `feat(tv): trailing Delete Season row in the episode picker — arm/confirm UI`.

---

### Task 6: The season-remove mutation (stages a–g)

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.h` (guarded outputs)
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/series_detail_screen.cpp` (SELECT-on-Armed dispatch + `drain_mutation`)

**Interfaces:**
- Consumes: Tasks 3–5. New guarded members (beside `mut_removed_`):
```cpp
bool mut_season_removed_ = false;   // guarded by mut_mtx_
int  mut_season_number_ = 0;        // guarded by mut_mtx_
```
- Produces: the working feature.

- [ ] **Step 1: Wire the Armed SELECT to this body** (replacing Task 5's no-op; `sid`, `season`, `title` captured by value; runs on `spawn_mutation`'s worker):

```cpp
const int sid = series_->sonarr_id;
const int season = episodes_season_;
const std::string title = detail_ ? detail_->title : std::string("This series");
season_del_armed_ = false;
season_del_inflight_ = true;
spawn_mutation([this, sid, season, title]() {
    auto fail = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(mut_mtx_);
        mut_toast_ = title + ": " + msg + " \xE2\x80\x94 season NOT deleted; retry is safe";
    };
    // (a) Unmonitor FIRST — season AND episodes. Probe-verified: the two
    //     flags are independent and auto-redownload-on-failed keys off
    //     the EPISODE flag, so season-only unmonitoring lets stage (d)
    //     fire searches behind our back.
    if (!sonarr_.set_season_monitored(sid, season, false))
        return fail("couldn't unmonitor Season " + std::to_string(season));
    auto eps = sonarr_.get_episodes_checked(sid);
    if (!eps) return fail("couldn't list episodes");
    std::vector<int> season_ep_ids;
    for (const auto& e : *eps)
        if (e.season_number == season) season_ep_ids.push_back(e.id);
    if (!sonarr_.set_episodes_monitored(season_ep_ids, false))
        return fail("couldn't unmonitor the season's episodes");
    // (b) Season history — authoritative; nullopt aborts before anything destructive.
    auto hist = sonarr_.get_season_history_checked(sid, season);
    if (!hist) return fail("Sonarr history unavailable");
    // (c) Cancel this season's live queue rows WITH blocklist.
    for (int qid : cancel_ids_for_season(sonarr_.get_queue(), sid, season)) {
        if (!sonarr_.cancel_queue_item(qid, /*blocklist=*/true))
            return fail("couldn't cancel an in-flight download");
    }
    // (d) Blocklist the imported grabs (mark-as-failed).
    for (int hid : hist->imported_history_ids) {
        if (!sonarr_.mark_history_failed(hid))
            return fail("couldn't blocklist the downloaded release");
    }
    // (e) Purge the season's torrents. Warn-and-continue: the torrent may
    //     already be gone, and the destructive file delete below is still
    //     correct without it. Null qbit_ = Task 7 contract, skip.
    int torrents_left = 0;
    if (qbit_) {
        for (const auto& h : hist->download_hashes)
            if (!qbit_->delete_torrent(h, /*delete_files=*/true)) ++torrents_left;
    }
    // (f) THE destructive step, last: this season's files, from a fresh
    //     authoritative listing (files can land between picker load and now).
    auto files = sonarr_.get_episode_files_checked(sid);
    if (!files) return fail("couldn't list episode files");
    std::vector<int> ids;
    for (const auto& f : *files)
        if (f.season_number == season) ids.push_back(f.id);
    if (!sonarr_.delete_episode_files(ids))
        return fail("couldn't delete the season's files");
    // (g) Publish.
    std::lock_guard<std::mutex> lk(mut_mtx_);
    mut_season_removed_ = true;
    mut_season_number_ = season;
    mut_toast_ = title + ": Season " + std::to_string(season) +
        " removed \xE2\x80\x94 Start Season " + std::to_string(season) +
        " re-downloads it" +
        (torrents_left ? " (a torrent needs manual cleanup in qBittorrent)" : "");
});
```

- [ ] **Step 1b: Start-Season re-monitor (probe amendment)** — in the
  existing `Action::NextSeason` dispatch (the "Start Season N" button),
  BEFORE its `trigger_season_search`, add the same episode-id collection
  as stage (a) and call `sonarr_.set_episodes_monitored(ids, true)`.
  Idempotent for seasons that never went through a delete; REQUIRED for
  re-downloading a deleted season (its episodes are individually
  unmonitored and Sonarr's SeasonSearch skips unmonitored episodes —
  cascade semantics are NOT to be relied on, probe-verified). This runs
  inside whatever worker the NextSeason flow already uses — if its
  current path is render-thread-only client calls, keep the addition in
  the same place those calls already happen (do not invent a new worker
  for it).

- [ ] **Step 2: Drain** — in `drain_mutation()` (same `mut_tmdb_id_`/`mut_fetch_gen_` gates as every outcome): on any verdict clear `season_del_inflight_`; if `mut_season_removed_` (consume + reset it and `mut_season_number_`): `region_ = DetailRegion::Seasons;` then the existing full-refresh path (`needs_refresh_ = true; fetch();` — copy whatever the whole-series remove's non-navigating siblings do to refresh rows), and show `mut_toast_` via `::ui::Toast::show` exactly where the existing drain shows toasts. On a fail-toast (no `mut_season_removed_`), the row returns to Idle — retry is the same two presses.
- [ ] **Step 3: Pi scratch compile** (same commands as Task 5 Step 2). Expected: links clean.
- [ ] **Step 4: Commit** `feat(tv): orphan-proof per-season delete — unmonitor, blocklist, purge, remove files`.

---

### Task 7: Full verification + hardware acceptance (the actual GoT case)

**Files:** none (verification only; fixes fold back into the task that owns them).

- [ ] **Step 1: Mac suites** — `build-mb`: full build + run `test_media_browser_unit` directly (it contains every media-browser test file). Then the MB=OFF dir (`build`): full build + all 8 suites (guards the flag-off path). Expected: all green.
- [ ] **Step 2: Deploy to magicpi5 for real** — `./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build` with `PI_HOST=magic@10.0.0.227` explicitly (never the default host). Kiosk restarts onto the new binary.
- [ ] **Step 3: Hardware acceptance — against a DISPOSABLE test series.**
  PROBE AMENDMENT: the original GoT case was resolved manually on
  2026-08-13 (MULTi pack blocklisted, "Non-English release markers" CF
  live at -10000, English S03E01-E10 re-grabbed) — GoT is now the
  user's good data and must NOT be deleted. Instead: add a throwaway
  series via the kiosk, grab ONE episode of one season (monitor just
  that episode, EpisodeSearch, wait for import), then:
  1. That season's episode picker → trailing `Delete Season N…` row present; a season with no files/queue shows none. GoT's pickers show the row too (files exist) — LOOK, don't press.
  2. Press SELECT on the row → `Confirm delete Season N`; wait 5 s → disarms.
  3. Arm + confirm → "Removing season…" → Seasons view, toast, season shows not-downloaded.
  4. Verify via API: no episodefiles for that season; blocklist carries the test release; season AND its episodes `monitored=false`; qBit torrent gone; ALL GoT files untouched (count before/after).
  5. Press `Start Season N` on the test series → verify its episodes re-monitor (`GET /episode?seriesId` shows monitored=true for that season) and a SeasonSearch fires that does NOT re-grab the blocklisted release.
  6. Cleanup: whole-series Remove on the test series (also the regression check for that flow); episode playback from a GoT picker unchanged; paging with the extra row correct on an overflowing season (GoT S3 has 10 episodes — page it, don't press).
- [ ] **Step 4: Commit any fixes, re-run the failing tier, then run the full local bats** (`./tests/run_local_tests.sh`) to make sure nothing else moved.

---

### Task 8: Docs + changelog

**Files:**
- Modify: `CHANGELOG.md` (Unreleased section)
- Modify: `magic_dingus_box_cpp/docs/MEDIA_BROWSER_PLAYBACK_AND_DOWNLOADS.md` (the section describing the Confirm Remove flow gains a per-season paragraph)
- Modify: `OWNER_GUIDE.md` §10 (one sentence: how to delete a season)

- [ ] **Step 1: CHANGELOG** under `## [Unreleased]`:

```markdown
### Added
- **Delete one season without touching the rest of the series.** The
  episode picker gains a trailing "Delete Season N…" row (two presses to
  confirm): cancels the season's downloads, blocklists the release(s)
  that produced it — so a wrong-language or junk season can't come back —
  removes its torrents and files, and unmonitors the season.
  "Start Season N" re-downloads it on demand. Whole-series Remove is
  unchanged.
```
- [ ] **Step 2: The two doc edits** (each one short paragraph, matching surrounding voice; OWNER_GUIDE stays non-technical: "Scroll past the last episode and select **Delete Season…** twice.").
- [ ] **Step 3: Commit** `docs: per-season delete — changelog, MB deep-dive, owner guide`.

---

## Self-Review (performed)

- **Spec coverage:** decisions 1–3 → Tasks 5/6; probe-before-build → Task 1; four client calls + queue-cancel semantics → Task 3; pure-logic tests → Task 4; stage table a–g → Task 6 Step 1 (order preserved, destructive last, (e) warn-and-continue); idempotent retry → fail() aborts pre-destruction + Task 7.3.2 disarm test; watch-history untouched → no task touches WatchStore; hardware acceptance incl. blocklist + different-release re-grab → Task 7 Step 3.
- **Placeholders:** none — every code step carries code; the two "check first" branches (dedupe field, http_delete body support) name the exact grep and both outcomes.
- **Type consistency:** `SeasonHistory`/`EpisodeFileInfo` (Task 2) are consumed by those names in Tasks 3 and 6; `cancel_queue_item(int,bool)` matches between Tasks 3 and 6; `season_delete_label` states match Task 5's render usage.
- **Known open risk:** probe P3 (mark-failed triggering a search despite unmonitored) has an explicit stop-and-surface instruction in Task 1 rather than a silent assumption.
