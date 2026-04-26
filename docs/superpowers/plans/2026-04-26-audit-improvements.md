# Post-Audit System Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Land seven improvements identified by the 2026-04-26 audit — eliminate sync-HTTP UI freezes, codify drifting service config, fix concurrency tech debt, clean up cosmetic noise.

**Architecture:** Mirror the existing async-fetch pattern (generation counter + atomic ready flag + worker thread vector + destructor join) that BrowseScreen already uses (commit `8849b77`). Codify infrastructure config in idempotent shell scripts. Migrate AppState legacy public fields to existing private thread-safe accessors.

**Tech Stack:** C++17 / GStreamer / Radarr v3 API / Prowlarr v1 / qBittorrent web API / PulseAudio / systemd / bash.

---

## Background and Conventions

**Async fetch reference:** `magic_dingus_box_cpp/src/media_browser/ui/browse_screen.{h,cpp}` is the canonical example. Pattern:

```cpp
// header members
std::atomic<uint64_t>      <name>_current_gen_{0};
std::mutex                 <name>_result_mtx_;
std::vector<ResultT>       <name>_pending_;
std::atomic<bool>          <name>_result_ready_{false};
std::vector<std::thread>   <name>_workers_;
bool loading_ = false;

// public entry: bumps gen, spawns worker, returns immediately
void load_x(...) {
    loading_ = true;
    const uint64_t my_gen = <name>_current_gen_.fetch_add(1) + 1;
    <name>_workers_.emplace_back(&ThisClass::run_load_x, this, my_gen, args...);
}
// worker: does sync HTTP, drops result if gen is stale
void run_load_x(uint64_t gen, args...) {
    auto result = sync_http_call(args...);
    if (gen != <name>_current_gen_.load()) return;        // pre-lock fast check
    {
        std::lock_guard<std::mutex> lk(<name>_result_mtx_);
        if (gen != <name>_current_gen_.load()) return;    // post-lock recheck
        <name>_pending_ = std::move(result);
    }
    <name>_result_ready_.store(true);
}
// drained from update() each frame: cheap atomic load, lock only when ready
void apply_pending() {
    if (!<name>_result_ready_.exchange(false)) return;
    std::lock_guard<std::mutex> lk(<name>_result_mtx_);
    // move <name>_pending_ into the live state vector
    loading_ = false;
}
// destructor MUST join all workers so no thread outlives `this`:
~ThisClass() {
    <name>_current_gen_.fetch_add(1);
    for (auto& t : <name>_workers_) if (t.joinable()) t.join();
}
```

**Build/deploy commands** (all run from worktree root):
```bash
# Local build of unit tests (where applicable):
cd magic_dingus_box_cpp/build && cmake .. && make -j4 test_media_browser_unit
./test_media_browser_unit

# Cross-compile + sync to Pi:
PI_HOST=magic@magicpi.local PI_DIR=/opt/magic_dingus_box \
  ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build

# Restart kiosk on Pi:
ssh magic@magicpi.local "sudo systemctl restart magic-dingus-box-cpp.service"
```

**Verification convention:** after each task, the implementer must (a) build cleanly with no new warnings, (b) deploy to Pi and confirm the kiosk service stays Active, (c) demonstrate the specific behavior via logs or live API.

---

## Task 1: Trigger Mario Galaxy re-grab as x264 (controller-inline)

**Files:** none (live Radarr API call only)

**Context:** Mario Galaxy is currently in the library as a 2.5 GB HEVC x265 file. The HEVC custom format penalty was just bumped from -100 to -250 (live Radarr). With that fix in place, asking Radarr to search again will pull a hardware-decodable x264 release.

**Steps:**

- [ ] **Step 1.1: Find Mario Galaxy's Radarr id**
```bash
KEY=$(ssh magic@magicpi.local "grep RADARR_API_KEY /opt/magic_dingus_box/services/.env | cut -d= -f2")
ssh magic@magicpi.local "curl -s -H \"X-Api-Key: $KEY\" http://localhost:7878/api/v3/movie | python3 -c 'import json,sys; [print(m[\"id\"], m[\"title\"]) for m in json.load(sys.stdin) if \"Galaxy\" in m[\"title\"]]'"
```

- [ ] **Step 1.2: Trigger MoviesSearch command**
```bash
# POST to /api/v3/command with name=MoviesSearch and movieIds=[<id>]
ssh magic@magicpi.local "curl -s -X POST -H \"X-Api-Key: $KEY\" -H 'Content-Type: application/json' \
  -d '{\"name\":\"MoviesSearch\",\"movieIds\":[<ID>]}' http://localhost:7878/api/v3/command"
```

- [ ] **Step 1.3: Confirm a new x264 grab appears in the queue or history**
Wait ~30 s, then poll `/api/v3/queue` and `/api/v3/history?eventType=1`. Confirm the new release title contains `x264` and not `x265|HEVC|h.265`.

- [ ] **Step 1.4: No commit needed** (live API only, no code change).

---

## Task 2: Remove vestigial `/library/Movies/` from setup script

**Files:** Modify `magic_dingus_box_cpp/scripts/setup_services.sh:52`

**Context:** Radarr writes movies to `/mnt/ssd/library/<Title> (<Year>)/`, no `Movies` subdirectory. The setup script creates `library/Movies` which is then never used. Cosmetic but confusing.

- [ ] **Step 2.1: Edit line 52**

Change:
```bash
sudo mkdir -p "${STORAGE_ROOT}"/{downloads/incomplete,downloads/complete,library/Movies,backups}
```
to:
```bash
sudo mkdir -p "${STORAGE_ROOT}"/{downloads/incomplete,downloads/complete,library,backups}
```

- [ ] **Step 2.2: Commit**
```bash
git add magic_dingus_box_cpp/scripts/setup_services.sh
git commit -m "fix(setup): drop unused library/Movies subdir — Radarr writes to library/<Title>/"
```

---

## Task 3: PulseAudio HDMI profile warning suppression

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/init_audio.sh`
- Possibly modify: `~/.config/pulse/default.pa` (Pi-side, generated at runtime)

**Context:** Every kiosk start logs:
```
module-alsa-card.c: Failed to find a working profile.
module.c: Failed to load module "module-alsa-card" (argument: "device_id=2 ... platform-fef05700.hdmi ...")
```
The Pi has two HDMI controllers in DT (`fef00700.hdmi` working, `fef05700.hdmi` no profile). PulseAudio scans both at boot via `module-udev-detect`. Suppressing the second halves audio init noise and saves ~200 ms of probing.

**Investigation steps for the implementer (do these BEFORE writing the fix — actual approach depends on what the audit reveals):**
1. SSH to Pi and run `pactl list cards short` and `cat /proc/asound/cards` — confirm `fef05700.hdmi` exists but has no working profile.
2. Check what `init_audio.sh` currently does and where the udev-detect runs from.
3. Pick one of:
   - **(a)** Pass `device_id=` blacklist arg to `module-udev-detect` in `default.pa`.
   - **(b)** Have `init_audio.sh` call `pactl set-card-profile <id> off` for the dud card after PulseAudio comes up.
   - **(c)** If dud card has no PCM and no profile entries, ignore — warning is benign and any fix is brittle.

- [ ] **Step 3.1: Live diagnose on Pi**
```bash
ssh magic@magicpi.local "pactl list cards short && echo --- && cat /proc/asound/cards"
```
Document findings in commit message.

- [ ] **Step 3.2: Implement chosen fix in `init_audio.sh`**
Code depends on findings. Must be idempotent and not break the working `platform-fe00b840.mailbox` and `platform-fef00700.hdmi` cards.

- [ ] **Step 3.3: Restart kiosk and verify no profile-load warnings in journal**
```bash
ssh magic@magicpi.local "sudo systemctl restart magic-dingus-box-cpp.service && sleep 8 && \
  journalctl -u magic-dingus-box-cpp.service --since '1 minute ago' | grep -iE 'profile|module-alsa-card' | head"
```
Expected: empty output OR only the working card's profile load. No "Failed to find a working profile" line.

- [ ] **Step 3.4: Confirm audio still routes correctly**
```bash
ssh magic@magicpi.local "pactl list short sinks"
```
Expected: at least one sink, with a stream-routable default.

- [ ] **Step 3.5: Commit**
```bash
git add magic_dingus_box_cpp/scripts/init_audio.sh
git commit -m "fix(audio): suppress dud HDMI controller PulseAudio probe warnings"
```

---

## Task 4: Codify Radarr Custom Formats in setup_services.sh

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/setup_services.sh` (append new step after line 140 / "language=Original")

**Context:** Six Custom Formats (AV1 -1000, HEVC -250, HDR -200, Remux -500, x264 +50, Trusted-groups +30) and the Any profile's formatItems referencing them are configured manually in the live Radarr. None of it is in the repo. If the Pi gets re-imaged, all that scoring intelligence is lost — which is exactly how Mario Galaxy was already grabbed as HEVC (HEVC score had silently drifted from -250 to -100). This task codifies the entire scoring system so fresh deploys reproduce it.

**Live Radarr is the source of truth** — the implementer must dump the current 6 custom formats via API to get the exact JSON specs (regex strings, etc.) and bake them into the setup script.

**Approach:** Idempotent block that for each desired Custom Format:
1. GETs `/api/v3/customformat`
2. If a CF with the target name exists → PUT update with desired spec
3. Else → POST create

Then PATCH the Any profile's `formatItems` so each CF has the right score.

- [ ] **Step 4.1: Dump live custom format specs to a fixture file**
```bash
KEY=$(ssh magic@magicpi.local "grep RADARR_API_KEY /opt/magic_dingus_box/services/.env | cut -d= -f2")
ssh magic@magicpi.local "curl -s -H \"X-Api-Key: $KEY\" http://localhost:7878/api/v3/customformat" \
  > magic_dingus_box_cpp/scripts/data/radarr_custom_formats.json
```

- [ ] **Step 4.2: Write the bash block for `setup_services.sh`**

Append a new step (Step 10) to `setup_services.sh` that:
- Reads the JSON fixture path: `${SERVICES_DIR}/scripts/data/radarr_custom_formats.json` (or bundled equivalent).
- For each CF entry: GETs `/api/v3/customformat` to find by name, then POST or PUT.
- After CFs are settled, PATCH the Any profile's `formatItems` so the score map matches.

The block should be ≤80 lines and use python3 for JSON shaping (same pattern as the language=Original step on line 122).

- [ ] **Step 4.3: Test the block in a "soft" mode against live Radarr**
First confirm the script is a no-op against the already-correct live state — i.e. running it should report "all 6 CFs match desired state" and not change anything observable.

- [ ] **Step 4.4: Test the block in a "force" mode**
Manually wreck one CF (e.g. set HEVC score back to -100), re-run the block, confirm it gets reset to -250.

- [ ] **Step 4.5: Add unit-test fixture loading to test_media_browser** (optional — only if time permits and the JSON fixture has a stable schema).

- [ ] **Step 4.6: Commit**
```bash
git add magic_dingus_box_cpp/scripts/setup_services.sh \
       magic_dingus_box_cpp/scripts/data/radarr_custom_formats.json
git commit -m "feat(setup): codify Radarr Custom Formats so fresh deploys reproduce scoring"
```

---

## Task 5: DetailScreen async migration

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp`

**Context:** Tapping any movie poster currently freezes the UI for **7-15 seconds** while three sync HTTP calls run on the render thread:
- `tmdb_.get_movie(tmdb_id_)` (~6 s VPN-tunneled)
- `radarr_.get_library()` (~1 s)
- `radarr_.get_quality_profiles()` (~1 s, only first time)

This is the biggest remaining UX freeze in the app. The fix is the BrowseScreen async pattern, exactly. Render thread shows a "Loading…" state immediately; worker thread does the HTTP off-thread; `update()` drains a finished result on a future tick.

**fetch() entry points to migrate:** `detail_screen.cpp` lines 230 (the function), 227 / 503 / 660 (call sites). The function is 79 lines.

- [ ] **Step 5.1: Read both BrowseScreen files end-to-end** to internalize the pattern.

- [ ] **Step 5.2: Add async fields and methods to `detail_screen.h`**
Mirror BrowseScreen's `tmdb_*` fields, but namespaced `detail_*`. Include a destructor that joins workers. Result struct holds `optional<TmdbDetailMovie>` + `vector<RadarrLibraryEntry>` + `vector<QualityProfile>`.

- [ ] **Step 5.3: Refactor `fetch()` to be the async entry point**
Pseudo:
```cpp
void DetailScreen::fetch() {
    movie_.reset();
    profiles_in_library_ = false;
    loading_ = true;
    const uint64_t gen = detail_current_gen_.fetch_add(1) + 1;
    detail_workers_.emplace_back(&DetailScreen::run_fetch, this, gen, tmdb_id_);
}

void DetailScreen::run_fetch(uint64_t gen, int tmdb_id) {
    DetailFetchResult r;
    r.detail = tmdb_.get_movie(tmdb_id);
    if (gen != detail_current_gen_.load()) return;
    r.library = radarr_.get_library();
    if (gen != detail_current_gen_.load()) return;
    r.profiles = radarr_.get_quality_profiles();
    if (gen != detail_current_gen_.load()) return;

    std::lock_guard<std::mutex> lk(detail_result_mtx_);
    if (gen != detail_current_gen_.load()) return;
    detail_pending_ = std::move(r);
    detail_result_ready_.store(true);
}
```

- [ ] **Step 5.4: Add `apply_pending_detail()` and call from `update()`**
Implement detail screen's `update()` if it doesn't have one yet — drains pending and slots into the existing render-state vars.

- [ ] **Step 5.5: Make Prowlarr availability fetch (already async via different path) re-trigger after detail loads**
The existing Prowlarr seeders thread keys off `movie_->title/year` becoming non-null. Confirm the trigger fires once `apply_pending_detail()` populates `movie_`.

- [ ] **Step 5.6: Render path: when `loading_ == true` AND no movie yet, draw a spinner / "Loading…" banner**
Don't crash on null `movie_`. Buttons (Play/Add/Remove) are disabled until detail is in.

- [ ] **Step 5.7: Build + run unit tests**
```bash
cd magic_dingus_box_cpp/build && make -j4 test_media_browser_unit && ./test_media_browser_unit
```
Expected: all green.

- [ ] **Step 5.8: Deploy + visually verify**
Tap a poster on the Pi. UI must not freeze; "Loading…" appears immediately; detail data fills in 1-7 s later. Switching screens during the load must not crash (worker drops stale result).

- [ ] **Step 5.9: Commit**
```bash
git add magic_dingus_box_cpp/src/media_browser/ui/detail_screen.h \
       magic_dingus_box_cpp/src/media_browser/ui/detail_screen.cpp
git commit -m "perf(detail): async TMDB + Radarr fetch — eliminates 7-15s poster-tap freeze"
```

---

## Task 6: SearchScreen async migration

**Files:**
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/search_screen.h`
- Modify: `magic_dingus_box_cpp/src/media_browser/ui/search_screen.cpp`

**Context:** Same pattern, smaller payoff. Sync HTTP at line 215 (`radarr_.lookup(query_)`) and 149 (`radarr_.get_library()`) freezes the UI ~1-5 s after typing pauses. Mirror Task 5 exactly.

- [ ] **Step 6.1: Add async fields and methods to `search_screen.h`** (use `search_*` prefix on members)

- [ ] **Step 6.2: Make `run_lookup_if_due()` and `enter()` (for library cache) dispatch to workers** rather than block

- [ ] **Step 6.3: Implement `apply_pending_search()` in `update()`**

- [ ] **Step 6.4: Render Loading… while in-flight**

- [ ] **Step 6.5: Build, deploy, visually verify** — typing into search shows "Loading…" briefly without UI freeze

- [ ] **Step 6.6: Commit**
```bash
git commit -m "perf(search): async Radarr lookup + library fetch"
```

---

## Task 7: AppState thread-safe accessor migration

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/app_state.h`
- Modify: every C++ file that reads or writes `state.position` / `state.duration` (~10-15 sites — use `grep -rn "state\.\(position\|duration\)" magic_dingus_box_cpp/src` to enumerate)

**Context:** MEMORY.md flags this as known tech debt:
> `state.position` / `state.duration` (public legacy fields) are what everything actually reads/writes. `state.get_position()` / `state.get_duration()` (thread-safe private fields) exist but are NEVER written to by the controller.

Today the renderer reads racy values during seeks because the controller writes the public fields without atomicity. This task migrates every callsite to the existing thread-safe accessor pair, then deletes the legacy public fields.

**Approach:** atomic-write API. Make the legacy fields private. Add `state.set_position(double)` / `state.set_duration(double)` with `std::atomic<double>` storage (or `std::mutex` if double-atomic doesn't fly on this toolchain). Replace all writes to controller-side write the atomic; all reads use the accessor.

- [ ] **Step 7.1: Enumerate all read/write sites**
```bash
grep -rn "state\.\(position\|duration\)" magic_dingus_box_cpp/src/ \
  | grep -v '_test\.cpp\|//.*'
```
Catalogue each as READ or WRITE.

- [ ] **Step 7.2: Add atomic storage + accessors to `app_state.h`**
```cpp
private:
    std::atomic<double> pos_{0.0};
    std::atomic<double> dur_{0.0};
public:
    void   set_position(double v) { pos_.store(v, std::memory_order_relaxed); }
    void   set_duration(double v) { dur_.store(v, std::memory_order_relaxed); }
    double get_position() const   { return pos_.load(std::memory_order_relaxed); }
    double get_duration() const   { return dur_.load(std::memory_order_relaxed); }
```
(Verify `std::atomic<double>` is lock-free on the Pi 4 toolchain via `std::atomic<double>::is_always_lock_free`. If not, fall back to `std::atomic<int64_t>` storing fixed-point milliseconds.)

- [ ] **Step 7.3: Migrate every WRITE site** to call `state.set_position(...)` / `state.set_duration(...)`

- [ ] **Step 7.4: Migrate every READ site** to call `state.get_position()` / `state.get_duration()`

- [ ] **Step 7.5: Delete the public legacy `position` / `duration` fields**
Build will fail at any missed site — fix until clean.

- [ ] **Step 7.6: Run unit tests + smoke-test on Pi**
```bash
cd magic_dingus_box_cpp/build && make -j4 && ./test_media_browser_unit
PI_HOST=magic@magicpi.local ./magic_dingus_box_cpp/scripts/deploy_cpp.sh --build
ssh magic@magicpi.local "sudo systemctl restart magic-dingus-box-cpp.service"
```
On Pi: play a movie. Confirm seek bar shows correct position, fast-forward updates render position smoothly, no flicker.

- [ ] **Step 7.7: Update MEMORY.md** — strike "Known tech debt: should migrate to thread-safe accessors" line.

- [ ] **Step 7.8: Commit**
```bash
git add magic_dingus_box_cpp/src/app/app_state.h <every modified file>
git commit -m "refactor(app_state): migrate position/duration to atomic accessors

Closes long-standing tech debt called out in MEMORY.md. Every reader/writer of
state.position and state.duration now goes through get_position() / set_position()
(and likewise duration), backed by std::atomic<double>. Eliminates UB races
between the GStreamer bus thread (writer) and the render thread (reader)
during seeks."
```

---

## Final Review

After all tasks land:

- [ ] **Full code review of the entire branch**
Dispatch a code-reviewer subagent with the diff range `<plan-start-sha>..HEAD`. Expected: no high-priority issues.

- [ ] **End-to-end smoke test on Pi**
1. Boot kiosk fresh.
2. Enter Browse — must be instant (existing async).
3. Tap a poster — must not freeze (Task 5).
4. Open Search, type — must not freeze (Task 6).
5. Play a movie. Seek with rotary. Position bar must update smoothly (Task 7).
6. `journalctl` must show no "Failed to find a working profile" (Task 3).
7. Re-run `setup_services.sh` against the live Pi — must be a no-op for CFs (Task 4 idempotency).
8. Confirm Mario Galaxy was re-grabbed as x264 (Task 1).

- [ ] **Push to origin/main**
```bash
git push origin main
```
