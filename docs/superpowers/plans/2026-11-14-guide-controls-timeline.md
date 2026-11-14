# Guide Controls and Timeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore single-tap watching, make holding Select enter Pick Ahead, and keep a short readable guide with explicit overnight gaps.

**Architecture:** Pure input timing translates Select edges into tap/hold actions; guide navigation owns their meaning and main.cpp only supplies facts. Pure view layout uses a fixed time window, clipped programme cells and untunable off-air cells. The existing schedule and whole-programme playback remain authoritative.

**Tech Stack:** C++17, Catch2, CMake, existing GLES kiosk renderer and Linux GPIO/evdev input; no new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-14-guide-controls-timeline-design.md` (approved, including execution without a further plan-approval stop).

## Global Constraints

- Work only in the existing `movie-channels` worktree. Preserve the original Movie Channels implementation and its ledger; this plan has a separate SDD workspace.
- Holding Select for **500 ms** enters Pick Ahead. Live mode, List style, menus and games retain their current Select behavior. Black/menu hold-volume remains independent.
- Keep the existing **20-minute Modern TV / 15-minute CRT** window in every guide state, including future selection. All rows use the same time axis. Keep five-minute ticks; pan, never zoom, to future programmes.
- A manually selected programme finishes whole, including after 1 AM. Live movie channels stop at 1 AM and return at 6 AM. No schedule or playback deadline changes.
- Use existing guide typography/colors and its text-size floor. The incumbent interface is the visual authority. No generic redesign, new product configuration or dependency installation.
- No new filesystem/network work on the render loop; pure units run on the Mac, kiosk wiring is syntax-checked and built on the Pi. Support MB ON/OFF and both board profiles.
- Use only `/private/tmp/mdb-movie-channels-codex/scratchpad/build`, one configuration at a time. Remove it after the controller finishes verification/deployment. Keep all ledger/worktree artifacts.
- Commit locally. No pushes, PR creation or shared-branch merges. The controller handles the authorized local merge to pi5-test, dry-run, Bluetooth-preserving deployment and hardware checks.
- Implementers/reviewers use Sol 6.1; reserve Astra for the final integration review or a genuinely difficult escalation. Workers must not create subagents.

## Review Focus

- Release/repeat events after a hold or context change must not tune, open another screen or double-trigger; Task 1 tests and audits the main-loop route.
- Holding from fullscreen must open on the tuned row, not a stale previously browsed row; off-air/empty/Picture rows must not acquire an invalid selection; Task 1 tests.
- Future films longer than the window must leave all other row widths/time scales unchanged; Task 2 tests both canvases and long/short neighbours.
- Off-air gaps crossing midnight or an offset transition must align with the actual civil schedule and report the next morning accurately; Task 2 tests.
- Narrow clipped cells and hint rows must stay readable without overlap or lone ellipses/arrows; Task 2 tests and Task 3 syntax/hardware preparation.

## Common commands

From the worktree root, configure the single build with:

```bash
cmake -S magic_dingus_box_cpp -B /private/tmp/mdb-movie-channels-codex/scratchpad/build -DBUILD_KIOSK=OFF -DBUILD_TESTS=ON -DENABLE_MEDIA_BROWSER=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_DEPLOYMENT_TARGET=15.7
```

Set `-DENABLE_MEDIA_BROWSER=OFF` on the same directory for OFF runs. Use `-j2`.
Kiosk syntax helper: `/private/tmp/mdb-movie-channels-codex/scratchpad/stubs/check_main.sh`; inspect its invocation before using it. Do not create another build.

---

### Task 1: Single-tap watching and hold-to-browse

**Files:**
- Create: `magic_dingus_box_cpp/src/app/guide_select_hold.h` (small header-only pure timing helper)
- Modify: `magic_dingus_box_cpp/src/app/guide_nav.{h,cpp}`, `magic_dingus_box_cpp/src/main.cpp`
- Test: `magic_dingus_box_cpp/tests/app/test_guide_nav.cpp` (include the timing-helper cases here; no extra CMake source needed)

**Interfaces:**
- Produces `app::GuideSelectHold` with `Clock = std::chrono::steady_clock`, `enum class Action { None, Tap, Hold }`, `void press(Clock::time_point)`, `Action release(Clock::time_point)`, `Action tick(Clock::time_point)`, `void cancel()`, `bool pending() const`.
- Threshold `inline constexpr std::chrono::milliseconds kGuideSelectHoldThreshold{500}`. A hold emits once at/after the threshold, including a release arriving before the next tick; its release emits no Tap. A repeated press while pending does not reset the timer. Unmatched release emits None; cancel discards the gesture.
- Add `GuideEvent::PickAhead` for explicit hold entry. `GuideEvent::Select` means a completed short tap in Pick Ahead and keeps its existing meaning elsewhere.
- Consumes existing `GuideFacts`, `guide_input`, `GuideCursor::pick_at`, `guide_visible`, and existing PhoneRemote/GPIO press/release facts. No platform event layout change is needed.

- [ ] **Step 1: Write focused failing tests.** At 499 ms release emits Tap; at 500 ms tick emits Hold once and release emits None; release at 500 ms emits Hold if unticked; duplicate press preserves the original deadline; cancel/unmatched release emits None. In Pick Ahead, Select on another visible row returns tune immediately; explicit PickAhead enters on-now without tuning; its later Select on a future airing produces on_demand. Hidden Select opens only; hidden PickAhead opens on tuned row and enters it. Test invalid/off-air/Picture rows and stale prior cursor, plus Live mode and horizontal-entry behavior.
- [ ] **Step 2: Run RED.** Build `test_app_unit` and run its guide filters; record the expected failures before implementing. Existing tests that asserted two-press entry must be updated to the approved explicit PickAhead event, not removed.
- [ ] **Step 3: Implement timing and navigation, then wire main.cpp.** Gather the tuned row's eligibility/airings when a hidden hold opens it. In Pick Ahead guide context, collect Select press/release and poll the monotonic timer every frame. Route Tap to Select and Hold to PickAhead. Consume corresponding releases. Cancel on Settings/MB/game/intro/style/mode transitions, explicit channel/navigation changes, or a lost eligible context, so a stale gesture cannot affect a new target. Preserve selected programme identity through a held gesture; normal menu/keyboard/game input remains on the existing route. Do not add polling I/O or unrelated input refactors.
- [ ] **Step 4: Verify GREEN.** Run focused guide cases while iterating; then full `test_app_unit` in ON and OFF once each. Syntax-check changed main.cpp with the existing typed helper in both configurations. Record any unsupported syntax check honestly for the real Pi build.
- [ ] **Step 5: Commit** `feat: tap to watch and hold to browse the guide`, self-review, and write the task report.

### Task 2: Fixed timeline, continuation markers and off-air cells

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/channel_guide_view.h`, `magic_dingus_box_cpp/src/ui/renderer_channel_guide.cpp` (painting only if view changes require it)
- Test: `magic_dingus_box_cpp/tests/ui/test_channel_guide_view.cpp`

**Interfaces:**
- Consumes Task 1's explicit Pick Ahead state and the existing schedule APIs `on_air`, `lineup`, `back_on_air_at`, `airing_after` and `UtcOffsetAt`.
- `GuideView` remains the complete rendering contract. Extend `GuideCell` with `bool off_air = false`, `bool continues_before = false`, `bool continues_after = false` as needed; no scheduling/input authority comes from these painting facts.
- Timing: `window_s = crt ? 900.0 : 1200.0` always. Live uses the existing five-minute floor. For a valid future selection, pan to that programme's start rounded down to a five-minute local tick, with the UTC-offset lookup applied consistently. No runtime-dependent zoom or tick interval.

- [ ] **Step 1: Write focused failing tests.** A selected three-hour film leaves window width and tick spacing identical to a short programme in both canvases; its start is visible and clipped right edge marked, with neighbouring playlist clips retaining proportional widths. Returning to on-now restores the live window. Test programmes clipping left, right, both and neither, including narrow cells. A movie ending at 1 AM produces a dim untunable gap through the visible window, with the full copy when it fits and `OFF AIR` when narrow; a normal playlist crossing 1 AM does not. Last live/future movie panel forecasts the actual next film at 6 AM, including midnight/offset transitions. Preserve retained on-demand panel behavior and the 24-hour navigation clamp.
- [ ] **Step 2: Run RED.** Build/run focused `test_ui_unit` guide cases and record failures that prove the unwanted zoom/blank gap/current next-morning omission.
- [ ] **Step 3: Implement the pure view and minimal painting.** Remove future-film window expansion. Keep selected clipped cells highlighted and title fitting honest. Use small continuation chevrons consistent with the existing guide, reserving their space from text. Render off-air intervals from actual scheduled closure to next opening, intersected with the viewport, without creating selectable programmes. Copy: `OFF AIR · BACK AT 6:00 AM` (format the actual returned civil time), fall back to `OFF AIR` if necessary without shrinking the font. For a last-movie NEXT panel, resolve the next opening then `on_air` there using the existing civil-time APIs. Follow the existing CRT panel-density conventions. Update footer hints to press/watch and hold/pick-ahead in Pick Ahead row mode; programme mode additionally communicates Back/channels. Fit both canvases without crowding the logo.
- [ ] **Step 4: Verify GREEN.** Run focused UI cases during iteration, then full `test_ui_unit` ON/OFF. Syntax-check renderer if modified. Inspect a pure representative view's geometry for long title/narrow gap/CRT hint fit; actual screenshots belong to controller hardware acceptance.
- [ ] **Step 5: Commit** `fix: keep guide timeline short and label overnight gaps`, self-review, and write the task report.

### Task 3: Documentation and integrated verification

**Files:**
- Modify: `CLAUDE.md`, `CHANGELOG.md`, and tracked `magic_dingus_box_cpp/docs/OWNER_GUIDE.md` if it describes these controls
- Create in this plan's SDD workspace: `verification-report.md`, `hardware-checklist.md`

**Interfaces:**
- Consumes Tasks 1 and 2 and the approved spec. No new runtime behavior.
- Produces command/result evidence for the final review and a concise hardware checklist for the controller's authorized deployment.

- [ ] **Step 1: Update docs** to describe one tap, 500 ms hold, fixed 20/15-minute timeline with clipped films, explicit overnight gaps and next morning. Replace outdated two-press/fit-whole-film wording in current guidance; historical approved specs remain historical.
- [ ] **Step 2: Run the complete C++ matrix** using the one scratch build: ON targets `test_retroarch_unit test_ui_unit test_app_unit test_platform_unit test_display_unit test_utils_unit test_video_unit test_phone_remote_unit test_media_browser_unit`, then all except Media Browser OFF. Build with `-j2`, run every executable, save logs and exact counts/exit status. Run applicable main/renderer syntax checks both ways. Do not rerun unrelated script/Python suites unless code in those areas changes.
- [ ] **Step 3: Prepare hardware checks** for the controller: physical tap immediately tunes once; hold enters without tuning on release, fullscreen hold opens correct row, Back/phone arrows/hold-volume work; short timeline preserves playlist widths while picking long films; continuation/gap/NEXT labels fit TV and CRT; no watch-state writes/watchdog restart. Carry forward actual physical SSD unplug, exact three-file approved cleanup, fresh-baseline restoration, and no real-Pi4 pass claim. Do not operate the Pi yourself.
- [ ] **Step 4: Commit** `docs: update guide controls and timeline behavior`, self-review and report evidence. Leave the single build for final review/integration; the controller removes it at the end.
