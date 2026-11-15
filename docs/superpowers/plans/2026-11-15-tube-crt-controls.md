# Tube CRT Engine — Controls (Plan 2 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every Tube row, the mask, the look, Reset and Degauss editable on the TV (Settings → Display) and in a new Content Manager **CRT Look** tab, with a change from the web on the TV within a fraction of a second — built on the core, hidden behind a "renderer ready" constant until Plan 3, and with no change to what any box draws.

**Architecture:** Decisions live in pure headers: `src/tube/tube_edit.h` (edit mode) and `src/app/tube_menu_logic.h` (which rows exist, what they say). `settings_menu.cpp` only builds rows from them. The web side never writes `settings.json`: it appends seq-tagged ops (row, mask, look, reset, engine, degauss) to an op log, `data/tube_request.json`. The kiosk notices a new file version by (inode, mtime, size) ten times a second, applies the ops above its `handled_seq` in order through the core's setters, publishes `handled_seq` in `kiosk_status.json`, and saves through the existing asynchronous save one second after the last applied op. One generated `static/tube_rows.json` feeds the page, checked against the C++ tables from both sides. Tube is offered only where `app::tube_offered(state)` is true: a Pi 5 (the core's gate) AND `AppState::tube_renderer_ready`, initialised from the constant `kTubeRendererReady = false`, which Plan 3 flips.

**Tech Stack:** C++17, Catch2 (`test_app_unit`, `test_ui_unit`, `test_phone_remote_unit`), jsoncpp, Flask + pytest, plain classic-script JavaScript (helpers checked under node from pytest).

**Spec:** `docs/superpowers/specs/2026-11-15-tube-crt-engine-design.md` — §6 Controls, §7 Live tuning (op log), the Degauss line of §8, §2 (gate closed), and the Addendum. Plan 1 (`docs/superpowers/plans/2026-11-15-tube-crt-core.md`) is built; its headers in `magic_dingus_box_cpp/src/tube/` are what this plan consumes.

**Open owner questions (Addendum):** this plan depends on neither. CRT Native's inset concerns picture fit, corner radius and margin, which are not rows; the menu and page mark "n/a" exactly the rows `tube::neutral_in_crt_native` names, so if the owner's answer adds a row there, both front ends follow. The speed-row jump is a per-frame renderer matter (Plan 3).

**Releasing before Plan 3 is safe.** With `kTubeRendererReady = false` a released box shows nothing new: the TV's CRT Engine row offers Classic/Enhanced exactly as today, no Tube row or Degauss appears, the CRT Look tab is not shown, and a POST is refused without writing. The only visible differences are internal: a `tube` block in `kiosk_status.json` (`gate: false`, `reason`) and the GET answering with that reason.

## Global Constraints

- The repository folder's name ends in a space: `.../📺 MDB/magic_dingus_box `. Work in the worktree `.worktrees/tube-crt-controls` (branch `tube-crt-controls`); use paths relative to its root.
- Dual-board contract: board differences only through `state.platform_profile.tube_crt_engine`. Every Tube decision in this plan takes `app::tube_offered(state)` (that gate AND `tube_renderer_ready`); never read the profile field directly. No `#ifdef` on a board.
- No new system dependency (kiosk or web). New C++ under `src/tube/` is header-only and standard-library-only.
- `config/settings.json` is written only by the kiosk, only through `app::SettingsPersistence::save_settings` (asynchronous). Tube state is changed only through `tube::set_row` / `set_mask` / `select_look` / `reset_to_look`; the engine only through `DisplaySettings::set_crt_engine`. The op log is input: the kiosk validates it again even though the web did.
- No file WRITE on the main loop from this plan: the op log is stat'ed at most every 100 ms and read only when (inode, mtime, size) changed; every filesystem call on the loop uses an `std::error_code` overload or POSIX `::stat`, never a throwing one. The kiosk never writes or deletes `tube_request.json`.
- Screens spawn, drain and paint: `settings_menu.cpp`, `renderer_settings.cpp` and `settings_input.cpp` call the pure functions of Tasks 1–2 and add only wiring.
- Web: route modules never import `admin`; mutating routes use `ctx.require_csrf`; errors are `admin_common.error_response(code, message, status, details)`; the new static script loads last, with `?v=`; any changed static file bumps its `?v=`.
- Exact values: op-log poll period **100 ms**; save **1 s** after the last applied op, and once at shutdown; kiosk status is fresh for **10 s** (`ts`); op-log file cap **16 KiB**; at most **64** ops per POST.
- Known and accepted: an op applied less than about a second before a kiosk crash may not have been saved, and an op written in the moment before a kiosk restart is adopted without being applied. In both cases the TV and the page then show the value the kiosk really has.
- Mac build (one directory; the Mac's disk is nearly full — delete it at the end). `test_app_unit` is skipped without yaml-cpp (`brew install yaml-cpp`):
  ```bash
  cmake -S magic_dingus_box_cpp -B build-tube2 -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF
  cmake --build build-tube2 --target test_app_unit test_ui_unit test_phone_remote_unit -j8
  ```
  Web: `python -m pytest magic_dingus_box/web/tests/<file> -q` from the repo root (as `.github/workflows/test-local.yml` runs it). `main.cpp`, `settings_input.cpp` and `renderer_settings.cpp` are kiosk-only: they compile in CI's `kiosk-build` job, not on the Mac.
- One commit per task; messages end with the session's attribution line.

## Review Focus

1. **Two phones tuning at once.** Both phones' changes land; a change to one row never undoes another row; on the same row the later op wins and both pages then show it. (Task 8 `test_two_pages_ops_both_land_in_seq_order`; Task 4 `"tube ops: applied in order, each through the core's setters"`)
2. **The page left open across a kiosk restart.** Nothing is re-applied at boot; the page shows "Waiting for the TV…" while the status is stale; afterwards it shows what the kiosk really has. (Task 4 `"tube ops: the file found at boot is adopted, never applied"`; Task 7 `"tube live: a file left from before a restart is adopted, not applied"`; Task 8 `test_get_says_kiosk_not_running_when_status_is_stale`)
3. **A TV edit and a web change at the same time.** A web change to another row never undoes the TV's row (ops carry one row each). On the same row, whichever reaches the kiosk last wins — within one 100 ms poll — and the TV and the page both show the winner within about a second. (Task 4 `"tube ops: a web op touches only its own row"`; Task 5 `"status_writer: a tube change is written at once"`)
4. **A slider dragged faster than the kiosk polls.** The TV ends exactly on the released value; ops are applied in seq order and never twice. (Task 7 `"tube live: two writes inside one mtime tick are both seen"` and `"tube live: ops written between polls all land, in order"`; Task 8 `test_rapid_posts_keep_only_the_newest_op_per_row`; Task 9 `sender keeps one POST in flight and the newest value per row wins`)
5. **A box where Tube is not offered** (a Pi 4B, or any box while the renderer is not ready). The TV offers Classic/Enhanced and the seven old rows only; a Pi 5 backup's saved Tube shows as Enhanced and is not rewritten until pressed; the CRT Look tab is not shown; a POST writes nothing. (Task 6a `"Tube menu: a Pi 4B is offered Classic and Enhanced only"` and `"Tube menu: a Pi 5 with the renderer not ready offers nothing new"`; Task 8 `test_not_offered_is_reported_and_writes_nothing`; Task 9 `test_tab_buttons_are_hidden_by_default`)
6. **BTN4 (the box's back button) in edit mode and in a group.** BTN4 leaves edit mode (saving once), then the group, then Display — one level per press, never out of Display from inside edit mode. (Task 6b `"Tube menu: BTN4 leaves edit mode, then the group"`)
7. **No write on the main loop.** Polling, applying and parsing never write; the only write is the debounced asynchronous save. (Task 7 `"tube live: polling never writes"`)

## Protocol cases

| Case | What happens | Pinned by |
|---|---|---|
| Slider dragged faster than the poll | The page has one POST in flight; meanwhile the newest value per row replaces older unsent ones. The server gives each op the next seq and keeps only the newest unhandled op per row key. The kiosk sees every file version — each `os.replace` is a new inode — and applies everything above `handled_seq` in order. | Task 7, Task 8, Task 9 |
| Two phones | Their POSTs append to one log under the server's lock. The kiosk applies both in seq order; different rows both survive; the same row ends on the later op. | Task 8 `test_two_pages_ops_both_land_in_seq_order` |
| TV edit while a web op is in flight | The TV changes its row at once. The web op touches only its own row, so other rows are safe. On the same row the op applied last wins. | Task 4 `"tube ops: a web op touches only its own row"` |
| Page left open across a kiosk restart | At boot the kiosk adopts the file's top seq and applies nothing. The page counts its op as confirmed once `handled_seq` reaches the op's seq, then shows the kiosk's real value. So an op adopted unapplied shows as not taken, rather than being silently re-applied. | Task 4, Task 7, Task 9 `confirmation drops what the kiosk has handled` |
| Kiosk crash less than 1 s after applying | That op may never have been saved; after the restart the kiosk and the page show the saved value. Accepted. | (documented, Global Constraints) |
| Kiosk restarting with an old op log | Adopted, never applied: nothing is undone. | Task 7 |
| Kiosk older than this feature | It never reads the file. Its status has no `tube` block, so GET answers `kiosk_too_old`, the tab stays hidden, and POST is refused without writing. | Task 8 `test_get_says_kiosk_too_old_without_a_tube_block` |
| Web admin restart | The server keeps no seq in memory: `seq = max(top seq in the file, handled_seq) + 1`. | Task 8 `test_seq_continues_past_what_the_kiosk_handled` |
| Web writing while the kiosk reads | Temp file in the same directory + `os.replace` is atomic; the kiosk reads one whole version or the next. | Task 8 `test_rapid_posts_keep_only_the_newest_op_per_row` (no temp leftovers) |
| A box whose op log was wiped (clone) while a page is open | Its `handled_seq` restarts low. The page treats a `handled_seq` lower than the last it saw as a fresh kiosk: it forgets its sent ops and shows the kiosk's values. | Task 9 `a handled_seq that goes backwards resets the page` |

---

### Task 1: The words the controls need

**Files:**
- Modify: `magic_dingus_box_cpp/src/tube/tube_groups.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_groups.cpp`

**Interfaces:**
- Consumes: `tube::RowInfo { std::string_view label, hint1, hint2; }`, `row_info(Effect)`, `kEffects`, `kMasks`, `kLooks`, `index_of`, `resolve`, `select_look`, and `moves_on_its_own(const Resolved&)` from `tube_inputs.h`.
- Produces (namespace `tube`):
  - `constexpr const RowInfo& mask_choice(Mask) noexcept;` — `label` is the word shown for the mask; the hints describe it.
  - `constexpr const RowInfo& look_choice(Look) noexcept;` — `label` is the look's display word.
  - Six reworded hints (below). `kMaskRowInfo` is unchanged.

Words (Sound Factory's `CRT_MASK_WORD` / `CRT_MASK_DETAIL` / `CRT_LOOK_DETAIL` at `4ef74a6`; slot's second line reworded; look words in the menu's title case):

| mask | label | hint1 | hint2 |
|---|---|---|---|
| grille | `GRILLE` | `aperture grille · default` | `the sharpest mask` |
| magentaGreen | `MAGENTA-GREEN` | `magenta and green stripes` | `the brightest mask` |
| dots | `DOTS` | `shadow-mask dots · TV tube` | `costs the most light` |
| slot | `SLOT` | `stripes in staggered slots` | `a 1990s TV set` |
| none | `OFF` | `no phosphor mask` | `scanlines and glass only` |

| look | label | hint1 | hint2 |
|---|---|---|---|
| pvm | `PVM` | `the broadcast monitor` | `nothing moves · default` |
| monitor | `Monitor` | `a crisp PC monitor` | `nothing moves` |
| trinitron | `Trinitron` | `a consumer Trinitron` | `moves a little` |
| tv | `TV` | `a 1990s set, slot mask` | `some of it moves` |
| arcade | `Arcade` | `a cabinet tube, dots` | `some of it moves` |
| everything | `Everything` | `every effect a tube shows` | `all of it moves` |

Hints that quote millimetres become shares of the picture's width (a millimetre is `1/196.608` of it, `tube_inputs.h`'s `kTubeMmAcross`). Replace hint2 of: `rollWidth` → `0: 4 % · 100: 31 % of picture width`; `tracking` → `0 off · up to 1.8 % of picture width`; `colorDrift` → `0 off · up to 0.4 % of picture width`; `trackingWidth` → `0: 1 % · 100: 16 % of picture width`; `flagging` → `0 off · up to 2 % of picture width`; `puritySize` → `0: 5 % · 100: 36 % of picture width`.

- [ ] **Step 1: Write the failing test** (append to `test_tube_groups.cpp`; add `#include "tube/tube_inputs.h"`)

```cpp
TEST_CASE("tube groups: mask and look words", "[tube]") {
    REQUIRE(mask_choice(Mask::magentaGreen).label == "MAGENTA-GREEN");
    REQUIRE(mask_choice(Mask::none).label == "OFF");
    REQUIRE(mask_choice(Mask::slot).hint2 == "a 1990s TV set");
    for (auto m : kMasks) { REQUIRE_FALSE(mask_choice(m).hint1.empty()); REQUIRE_FALSE(mask_choice(m).hint2.empty()); }
    REQUIRE(look_choice(Look::tv).label == "TV");
    REQUIRE(look_choice(Look::everything).label == "Everything");
    REQUIRE(look_choice(Look::tv).hint1 == "a 1990s set, slot mask");
}
TEST_CASE("tube groups: a look's hint says whether it moves", "[tube]") {
    for (auto look : kLooks) {
        TubeSetting s; select_look(s, look);
        const bool says_still = look_choice(look).hint2.rfind("nothing moves", 0) == 0;
        INFO(look_choice(look).label);
        REQUIRE(says_still == !moves_on_its_own(resolve(s)));
    }
}
TEST_CASE("tube groups: no hint quotes the panel's millimetres", "[tube]") {
    for (auto e : kEffects) {
        REQUIRE(row_info(e).hint1.find(" mm") == std::string_view::npos);
        REQUIRE(row_info(e).hint2.find(" mm") == std::string_view::npos);
    }
    REQUIRE(row_info(Effect::rollWidth).hint2 == "0: 4 % · 100: 31 % of picture width");
    REQUIRE(row_info(Effect::puritySize).hint2 == "0: 5 % · 100: 36 % of picture width");
}
```

- [ ] **Step 2: Run, see it fail** — `cmake --build build-tube2 --target test_app_unit -j8` fails: `mask_choice` undeclared.
- [ ] **Step 3: Implement** in `tube_groups.h`: two index-aligned `detail::` arrays and the two accessors; edit the six hint strings in `detail::kRowInfo`. Say in the header comment that the six millimetre hints were converted for a TV.
- [ ] **Step 4: Run** `./build-tube2/test_app_unit "[tube]"` — all pass.
- [ ] **Step 5: Commit** — `tube: mask and look words, and hints in shares of the picture`

---

### Task 2: Edit mode, the menu's decisions and the two gates (pure)

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_edit.h`
- Create: `magic_dingus_box_cpp/src/app/tube_menu_logic.h` (header-only, so `test_ui_unit` needs no new source)
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (next to `CrtEngine`; `AppState` members), `magic_dingus_box_cpp/src/app/settings_persistence.cpp` (its private `crt_engine_to_string` / `crt_engine_from_string` call the new pair)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_edit.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_menu_logic.cpp`

**Interfaces:**
- Consumes: `tube::TubeSetting`, `resolve`, `set_row`, `set_mask`, `moved_keys`, `step_row`, `row_info`, `kGroups`, `Group`, `kMasks`, `kLooks`, `look_choice`, `mask_choice`, `neutral_in_crt_native`; `app::CrtEngine`, `app::effective_crt_engine`; `AppState::platform_profile.tube_crt_engine`.
- Produces in `app_state.h` (namespace `app`):
  - `constexpr std::string_view crt_engine_name(CrtEngine) noexcept;` (`"classic"|"enhanced"|"tube"`), `constexpr std::string_view crt_engine_word(CrtEngine) noexcept;` (`"Classic"|"Enhanced"|"Tube"`), `constexpr std::optional<CrtEngine> parse_crt_engine(std::string_view) noexcept;` — the only spelling of the engine words.
  - `inline constexpr bool kTubeRendererReady = false;` — Plan 3 sets it to `true`.
  - ```cpp
    // The op log's progress and the Degauss presses. In memory only.
    struct TubeLiveState {
        std::uint64_t handled_seq = 0;      // highest op seq read (applied or skipped); published
        std::uint64_t degauss_requests = 0; // presses on the TV or the web; Plan 3 consumes the increase
        bool adopted = false;               // the op log has been looked at since boot
    };
    ```
    `AppState` members, beside each other: `TubeLiveState tube_live;` and `bool tube_renderer_ready = kTubeRendererReady;` (a field so tests can open it).
  - `inline bool tube_offered(const AppState& s) noexcept { return s.platform_profile.tube_crt_engine && s.tube_renderer_ready; }` (after `AppState`).
  - `inline std::string_view tube_not_offered_reason(const AppState& s) noexcept;` — `""` when offered, `"needs_pi5"` when the board gate is closed, else `"not_ready"`.
- Produces in `tube_edit.h` (namespace `tube`):
  - `struct Edit { bool active = false; bool mask = false; Effect effect = Effect::brightness; bool changed = false; };`
  - `void begin_edit(Edit&, std::optional<Effect> effect) noexcept;` — `nullopt` is the MASK row.
  - `constexpr int edit_direction(int delta, bool vertical_axis) noexcept;` — knob and left/right: the sign of `delta`; up/down (`ROTATE_VERTICAL`, up is `-1`): minus the sign.
  - `constexpr Mask next_mask(Mask, int direction) noexcept;` — through `kMasks`, wrapping.
  - `bool edit_turn(Edit&, TubeSetting&, int direction) noexcept;` — a value row: `set_row(e, step_row(e, resolve(s).effects[i], direction))`; the mask row: `set_mask(next_mask(resolve(s).mask, direction))`. True when the resolved value changed (then `changed = true`); false when inactive or clamped.
  - `bool end_edit(Edit&) noexcept;` — returns `changed` (save once), always leaves the edit inactive and unchanged.
- Produces in `tube_menu_logic.h` (namespace `app`; every `bool offered` is `tube_offered(state)` at the call site):
  - `struct DisplayCrtRows { bool classic; bool tube; };` `DisplayCrtRows display_crt_rows(CrtEngine saved, bool offered) noexcept;` — from `effective_crt_engine(saved, offered)`: Tube → tube rows only; otherwise the seven Classic/Enhanced rows only.
  - `CrtEngine next_crt_engine(CrtEngine saved, bool offered) noexcept;` — steps from the EFFECTIVE engine: Classic → Enhanced → Tube (offered) or → Classic (not offered); Tube → Classic.
  - `std::string crt_engine_label(CrtEngine saved, bool offered);` (`"CRT Engine: " + crt_engine_word(effective)`) and `std::string_view crt_engine_sublabel(bool offered);` (`"Classic / Enhanced / Tube"` / `"Classic / Enhanced"`).
  - `tube::Look next_look(tube::Look) noexcept;` (wraps); `std::string tube_look_label(const tube::TubeSetting&);` (`"Look: TV"`, `"Look: TV (2 moved)"`); `std::string tube_look_sublabel();` — the six look words joined by `" · "` (`"PVM · Monitor · Trinitron · TV · Arcade · Everything"`, the §6 sketch).
  - `std::string tube_reset_sublabel(const tube::TubeSetting&);` (`"nothing moved"`, `"1 row moved"`, `"3 rows moved"`).
  - `std::string tube_group_label(const tube::Group&);` (sentence case: `COLOUR PURITY` → `Colour purity`) and `std::string tube_group_sublabel(const tube::Group&, const tube::TubeSetting&);` — `"<n> rows"` counting the MASK row, plus `" · <m> moved"` when `m > 0`.
  - `enum class TubeRowKind { Value, Mask, NotApplicable };` `TubeRowKind tube_row_kind(std::optional<tube::Effect> effect, bool crt_native) noexcept;` — `nullopt` is the MASK row, n/a in CRT Native; a value row is n/a when `crt_native && neutral_in_crt_native(e)`.
  - `std::string tube_value_label(tube::Effect, int value, bool editing);` (`"HUM BAR: 20"`, editing `"HUM BAR: < 20 >"`); `std::string tube_mask_label(tube::Mask, bool editing);` (`"MASK: SLOT"`, `"MASK: < SLOT >"`); `std::string tube_na_label(std::string_view row_label);` (`"SCANLINES: n/a"`); `inline constexpr std::string_view kTubeNaSublabel = "a real tube draws this itself";`

- [ ] **Step 1: Write the failing tests**

`test_tube_edit.cpp`:
```cpp
using namespace tube;
TEST_CASE("tube edit: a turn steps from the resolved value and saves once", "[tube]") {
    TubeSetting s; Edit e;                               // pvm: hum 0
    begin_edit(e, Effect::hum); REQUIRE(e.active);
    REQUIRE(edit_turn(e, s, +1)); REQUIRE(resolve(s).effects[index_of(Effect::hum)] == 5);
    REQUIRE(edit_turn(e, s, +1)); REQUIRE(resolve(s).effects[index_of(Effect::hum)] == 10);
    REQUIRE(edit_turn(e, s, -1)); REQUIRE(edit_turn(e, s, -1));
    REQUIRE(s == TubeSetting{});                         // back on the look: nothing moved
    REQUIRE(end_edit(e)); REQUIRE_FALSE(e.active);
    REQUIRE_FALSE(end_edit(e));                          // leaving twice saves once
}
TEST_CASE("tube edit: clamped turns change nothing; size rows step a pixel", "[tube]") {
    TubeSetting s; Edit e; begin_edit(e, Effect::hum);
    REQUIRE_FALSE(edit_turn(e, s, -1));
    REQUIRE_FALSE(end_edit(e));
    begin_edit(e, Effect::maskSize); edit_turn(e, s, +1);
    REQUIRE(resolve(s).effects[index_of(Effect::maskSize)] == 20);
}
TEST_CASE("tube edit: the mask row cycles its five words", "[tube]") {
    TubeSetting s; Edit e; begin_edit(e, std::nullopt);  // pvm: grille
    edit_turn(e, s, +1); REQUIRE(resolve(s).mask == Mask::magentaGreen);
    edit_turn(e, s, -1); edit_turn(e, s, -1); REQUIRE(resolve(s).mask == Mask::none);
    REQUIRE(next_mask(Mask::none, +1) == Mask::grille);
}
TEST_CASE("tube edit: knob and D-pad agree on up; an idle edit does nothing", "[tube]") {
    REQUIRE(edit_direction(+1, false) == +1); REQUIRE(edit_direction(-3, false) == -1);
    REQUIRE(edit_direction(-1, true) == +1);  REQUIRE(edit_direction(+1, true) == -1);
    REQUIRE(edit_direction(0, true) == 0);
    TubeSetting s; Edit e; REQUIRE_FALSE(edit_turn(e, s, +1)); REQUIRE(s == TubeSetting{});
}
```

`test_tube_menu_logic.cpp`:
```cpp
using app::CrtEngine;
TEST_CASE("tube menu logic: Tube is offered only on a Pi 5 with the renderer ready", "[tube]") {
    app::AppState s;
    REQUIRE_FALSE(s.tube_renderer_ready);                         // kTubeRendererReady is false
    REQUIRE(app::tube_not_offered_reason(s) == "needs_pi5");
    s.platform_profile.tube_crt_engine = true;
    REQUIRE_FALSE(app::tube_offered(s)); REQUIRE(app::tube_not_offered_reason(s) == "not_ready");
    s.tube_renderer_ready = true;
    REQUIRE(app::tube_offered(s)); REQUIRE(app::tube_not_offered_reason(s).empty());
}
TEST_CASE("tube menu logic: the engine cycle offers Tube only when offered", "[tube]") {
    REQUIRE(app::next_crt_engine(CrtEngine::Classic, true) == CrtEngine::Enhanced);
    REQUIRE(app::next_crt_engine(CrtEngine::Enhanced, true) == CrtEngine::Tube);
    REQUIRE(app::next_crt_engine(CrtEngine::Tube, true) == CrtEngine::Classic);
    REQUIRE(app::next_crt_engine(CrtEngine::Enhanced, false) == CrtEngine::Classic);
    REQUIRE(app::next_crt_engine(CrtEngine::Tube, false) == CrtEngine::Classic);  // shown as Enhanced
    REQUIRE(app::crt_engine_label(CrtEngine::Tube, false) == "CRT Engine: Enhanced");
    REQUIRE(app::crt_engine_label(CrtEngine::Tube, true) == "CRT Engine: Tube");
    REQUIRE(app::crt_engine_sublabel(false) == "Classic / Enhanced");
    REQUIRE(app::display_crt_rows(CrtEngine::Tube, true).tube);
    REQUIRE_FALSE(app::display_crt_rows(CrtEngine::Tube, true).classic);
    REQUIRE(app::display_crt_rows(CrtEngine::Tube, false).classic);
    REQUIRE_FALSE(app::display_crt_rows(CrtEngine::Tube, false).tube);
    REQUIRE(app::parse_crt_engine("tube") == CrtEngine::Tube);
    REQUIRE(app::parse_crt_engine("Tube") == std::nullopt);
    REQUIRE(app::crt_engine_word(CrtEngine::Enhanced) == "Enhanced");
}
TEST_CASE("tube menu logic: labels", "[tube]") {
    using namespace tube;
    TubeSetting s; select_look(s, Look::tv);
    REQUIRE(app::tube_look_label(s) == "Look: TV");
    REQUIRE(app::tube_look_sublabel() == "PVM · Monitor · Trinitron · TV · Arcade · Everything");
    REQUIRE(app::tube_reset_sublabel(s) == "nothing moved");
    set_row(s, Effect::hum, 20); set_mask(s, Mask::dots);
    REQUIRE(app::tube_look_label(s) == "Look: TV (2 moved)");
    REQUIRE(app::tube_reset_sublabel(s) == "2 rows moved");
    REQUIRE(app::tube_group_sublabel(kGroups[6], s) == "4 rows · 1 moved");   // SUPPLY
    REQUIRE(app::tube_group_sublabel(kGroups[2], s) == "3 rows · 1 moved");   // MASK counts its word row
    REQUIRE(app::tube_group_sublabel(kGroups[0], s) == "5 rows");
    REQUIRE(app::tube_group_label(kGroups[9]) == "Colour purity");
    REQUIRE(app::next_look(Look::everything) == Look::pvm);
    REQUIRE(app::tube_value_label(Effect::hum, 20, false) == "HUM BAR: 20");
    REQUIRE(app::tube_value_label(Effect::hum, 20, true) == "HUM BAR: < 20 >");
    REQUIRE(app::tube_mask_label(Mask::magentaGreen, false) == "MASK: MAGENTA-GREEN");
    REQUIRE(app::tube_row_kind(Effect::scanlines, true) == app::TubeRowKind::NotApplicable);
    REQUIRE(app::tube_row_kind(Effect::lineSize, true) == app::TubeRowKind::Value);
    REQUIRE(app::tube_row_kind(std::nullopt, true) == app::TubeRowKind::NotApplicable);
    REQUIRE(app::tube_row_kind(std::nullopt, false) == app::TubeRowKind::Mask);
    REQUIRE(app::tube_na_label("SCANLINES") == "SCANLINES: n/a");
}
```

- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement** the two headers and the `app_state.h` additions; point `settings_persistence.cpp`'s two private helpers at `crt_engine_name` / `parse_crt_engine`.
- [ ] **Step 4: Run** `./build-tube2/test_app_unit` in full — all pass (the persistence tests prove the helper swap changed nothing).
- [ ] **Step 5: Commit** — `tube: edit mode, the menu's decisions and the renderer-ready gate, pure`

---

### Task 3: One generated row file for the web page

**Files:**
- Create: `magic_dingus_box_cpp/src/app/tube_rows_json.h` (in `app/`, because it takes the engine words from `app_state.h`)
- Create: `magic_dingus_box/web/static/tube_rows.json` (generated, checked in)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_rows_json.cpp`, `magic_dingus_box/web/tests/test_tube_rows_json.py`

**Interfaces:**
- Consumes: Task 1; Task 2's `crt_engine_name`, `crt_engine_word`; `tube::kEffects`, `effect_key`, `row_info`, `row_step`, `kGroups` (`key`, `label`, `has_mask_row`, `rows`), `kMasks`, `mask_name`, `kLooks`, `look_name`, `look_values`, `kLookDefault`, `kRowMin`, `kRowMax`, `neutral_in_crt_native`; Task 2's `tube_group_label`; `magic_dingus_box_cpp/scripts/dev/check_tube_parity.py`'s `parse_tables(rows_text, looks_text)`.
- Produces:
  - `std::string app::tube_rows_json();` (inline) — deterministic bytes, 2-space indentation, UTF-8 written as-is (escape only `"` and `\`), ending in `\n`.
  - `magic_dingus_box/web/static/tube_rows.json`, shape (key order as written):
    ```json
    {"version": 1, "row_min": 0, "row_max": 100, "default_look": "pvm",
     "engines": [{"key": "classic", "word": "Classic"}, {"key": "enhanced", "word": "Enhanced"}, {"key": "tube", "word": "Tube"}],
     "rows": [{"key": "brightness", "label": "BRIGHTNESS", "hint": ["…", "…"], "step": 5, "crt_native_neutral": false}, … 44 in canonical order],
     "mask": {"label": "MASK", "crt_native_neutral": true,
              "choices": [{"key": "grille", "word": "GRILLE", "hint": ["…", "…"]}, … 5]},
     "groups": [{"key": "picture", "label": "PICTURE", "title": "Picture", "rows": ["brightness", …]}, … 10;
                the mask group's rows are ["mask", "maskStrength", "maskSize"]],
     "looks": [{"key": "pvm", "word": "PVM", "hint": ["…", "…"], "mask": "grille", "values": {"brightness": 55, … 44}}, … 6]}
    ```
  - Regeneration: `MDB_WRITE_TUBE_ROWS_JSON=1 ./build-tube2/test_app_unit "[tube_rows_json]"` rewrites the file and passes.
- The Python check stands alone: it compares the JSON to the three headers' TEXT, so it guards the file on a machine where `test_app_unit` is skipped for want of yaml-cpp.

- [ ] **Step 1: Write the failing tests**

`test_tube_rows_json.cpp`:
```cpp
namespace fs = std::filesystem;
const fs::path kRowFile = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path()
                          / "magic_dingus_box" / "web" / "static" / "tube_rows.json";

TEST_CASE("tube rows json: the checked-in file is what the tables generate", "[tube][tube_rows_json]") {
    const std::string want = app::tube_rows_json();
    if (std::getenv("MDB_WRITE_TUBE_ROWS_JSON")) { std::ofstream(kRowFile, std::ios::binary) << want; }
    std::ifstream f(kRowFile, std::ios::binary);
    REQUIRE(f.good());
    const std::string have((std::istreambuf_iterator<char>(f)), {});
    INFO("regenerate: MDB_WRITE_TUBE_ROWS_JSON=1 ./build-tube2/test_app_unit \"[tube_rows_json]\"");
    REQUIRE(have == want);
}
TEST_CASE("tube rows json: it parses and holds every row", "[tube][tube_rows_json]") {
    Json::Value root; std::istringstream in(app::tube_rows_json());
    REQUIRE(Json::parseFromStream(Json::CharReaderBuilder{}, in, &root, nullptr));
    REQUIRE(root["rows"].size() == 44);
    REQUIRE(root["rows"][24]["key"].asString() == "hum");
    REQUIRE(root["rows"][24]["hint"][0].asString() == "soft at low · pronounced at 100");
    REQUIRE(root["rows"][10]["step"].asInt() == 10);
    REQUIRE(root["rows"][5]["crt_native_neutral"].asBool());          // scanlines
    REQUIRE(root["groups"][2]["rows"][0].asString() == "mask");
    REQUIRE(root["groups"][9]["title"].asString() == "Colour purity");
    REQUIRE(root["looks"][3]["values"]["hum"].asInt() == 60);         // tv
    REQUIRE(root["looks"][3]["mask"].asString() == "slot");
    REQUIRE(root["mask"]["choices"][4]["word"].asString() == "OFF");
    REQUIRE(root["engines"][2]["key"].asString() == "tube");
}
```

`test_tube_rows_json.py` (loads `check_tube_parity.py` with `importlib.util.spec_from_file_location`; reads `tube_rows.h`, `tube_looks.h`, `tube_groups.h` as text):
- `test_keys_and_looks_match_the_headers` — `[r["key"] for r in rows]` equals `parse_tables(...)["keys"]`; each look's `values` (in key order) and `mask` equal `parse_looks`.
- `test_labels_and_hints_match_kRowInfo` — first cut the block with `re.search(r"kRowInfo\{\{(.*?)\}\};", text, re.S)` (so `kMaskRowInfo` and Task 1's arrays are not matched), then the triples `\{"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\}` in that block, in order, equal each row's `label` + `hint`; exactly 44 triples.
- `test_groups_match_kGroups` — cut `kGroups\{\{(.*?)\}\};` with `re.S`, then each entry `\{"(\w+)",\s*"([^"]+)",\s*(true|false),\s*detail::rows_of\(([^)]*)\)\}` with `re.S` (entries span lines); the `Effect::x` names in order equal the group's rows, with `"mask"` first when the bool is `true`; exactly 10 entries.
- `test_steps` — `step` is 10 for `lineSize` and `maskSize`, 5 otherwise.
- `test_engines` — `[e["key"] for e in engines] == ["classic", "enhanced", "tube"]`.
- Every block regex must match, or the test fails (never a silent pass on an unparsed header).

- [ ] **Step 2: Run, see them fail** — the C++ build fails (`tube_rows_json.h` missing); `python -m pytest magic_dingus_box/web/tests/test_tube_rows_json.py -q` fails (file missing).
- [ ] **Step 3: Implement** `tube_rows_json.h`, then generate the file with the command above.
- [ ] **Step 4: Run** both suites — pass. Change one hint locally, see both fail, revert.
- [ ] **Step 5: Commit** — `tube: one generated row file for the web page, checked from both sides`

---

### Task 4: The op log — parsing and applying

**Files:**
- Create: `magic_dingus_box_cpp/src/app/tube_ops.h`, `magic_dingus_box_cpp/src/app/tube_ops.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (`APP_SOURCES` and the `test_app_unit` source list: `src/app/tube_ops.cpp`)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_ops.cpp`

**Interfaces:**
- Consumes: Task 2 (`TubeLiveState`, `parse_crt_engine`); `tube::parse_look`, `parse_mask`, `parse_effect`, `is_row_value`, `select_look`, `reset_to_look`, `set_mask`, `set_row`; `AppState::DisplaySettings::{crt_engine, tube, set_crt_engine}`.
- Produces (namespace `app`):
  - The file `data/tube_request.json`, written only by the web admin (Task 8):
    ```json
    {"version": 1, "ops": [
      {"seq": 12, "op": "look", "look": "tv"},
      {"seq": 13, "op": "row", "key": "hum", "value": 20},
      {"seq": 14, "op": "mask", "mask": "dots"},
      {"seq": 15, "op": "reset"},
      {"seq": 16, "op": "engine", "engine": "tube"},
      {"seq": 17, "op": "degauss"}]}
    ```
  - `inline constexpr std::size_t kTubeOpsMaxBytes = 16 * 1024;`
  - `enum class TubeOpKind { Row, Mask, Look, Reset, Engine, Degauss };`
  - `struct TubeOp { std::uint64_t seq = 0; TubeOpKind kind = TubeOpKind::Reset; tube::Effect effect = tube::Effect::brightness; int value = 0; tube::Mask mask = tube::Mask::grille; tube::Look look = tube::kLookDefault; CrtEngine engine = CrtEngine::Classic; };`
  - `struct TubeOpsParse { std::optional<std::vector<TubeOp>> ops; std::string error; };`
  - `TubeOpsParse parse_tube_ops(std::string_view text);` — the whole file is refused, with an error naming the problem, when: longer than `kTubeOpsMaxBytes`; not JSON or not an object; `version != 1`; `ops` not an array; an op not an object; `seq` not an unsigned integer ≥ 1 or not strictly greater than the previous op's; `op` not one of `row|mask|look|reset|engine|degauss`; a `row` without a known `key` or with a `value` whose `type()` is not `Json::intValue`/`Json::uintValue` (jsoncpp's `isInt()`/`isIntegral()` accept `20.0`) or that fails `tube::is_row_value`; an unknown `mask`/`look`/`engine` word. Extra fields are ignored. An empty `ops` array is valid.
  - `void adopt_tube_ops(const std::vector<TubeOp>* found, TubeLiveState& live) noexcept;` — the first look after boot (`found == nullptr`: no file or an unreadable one): `adopted = true`, `handled_seq = ` the last op's seq (unchanged when there is none). Never applies anything.
  - `struct TubeOpsResult { bool changed = false; std::size_t applied = 0; std::size_t skipped = 0; };`
  - `TubeOpsResult apply_tube_ops(const std::vector<TubeOp>&, AppState::DisplaySettings&, TubeLiveState&, bool offered);` — for each op with `seq > handled_seq`, in order: `handled_seq = seq`; when `!offered`, count it skipped and do nothing else; otherwise Row → `tube::set_row` (a `false` return counts skipped), Mask → `set_mask`, Look → `select_look`, Reset → `reset_to_look`, Engine → `set_crt_engine`, Degauss → `++degauss_requests`; count applied. Ops at or below `handled_seq` are ignored. `changed` is true when the engine or the tube setting differs from before the call.

- [ ] **Step 1: Write the failing test**

```cpp
namespace {
std::string log_of(const std::string& ops) { return R"({"version":1,"ops":[)" + ops + "]}"; }
std::vector<app::TubeOp> parsed(const std::string& text) {
    auto p = app::parse_tube_ops(text); INFO(p.error); REQUIRE(p.ops); return *p.ops; }
const std::string kOps = log_of(
    R"({"seq":12,"op":"look","look":"tv"},{"seq":13,"op":"row","key":"hum","value":20},)"
    R"({"seq":14,"op":"mask","mask":"dots"},{"seq":15,"op":"engine","engine":"tube"},{"seq":16,"op":"degauss"})");
}
TEST_CASE("tube ops: applied in order, each through the core's setters", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; app::adopt_tube_ops(nullptr, live);
    const auto r = app::apply_tube_ops(parsed(kOps), d, live, true);
    REQUIRE(r.changed); REQUIRE(r.applied == 5); REQUIRE(r.skipped == 0);
    tube::TubeSetting want; tube::select_look(want, tube::Look::tv);
    tube::set_row(want, tube::Effect::hum, 20); tube::set_mask(want, tube::Mask::dots);
    REQUIRE(d.tube == want);
    REQUIRE(d.crt_engine == app::CrtEngine::Tube); REQUIRE(d.enhanced_crt_enabled);
    REQUIRE(live.handled_seq == 16); REQUIRE(live.degauss_requests == 1);
    const auto again = app::apply_tube_ops(parsed(kOps), d, live, true);     // nothing twice
    REQUIRE_FALSE(again.changed); REQUIRE(again.applied == 0); REQUIRE(live.degauss_requests == 1);
}
TEST_CASE("tube ops: a web op touches only its own row", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; app::adopt_tube_ops(nullptr, live);
    tube::set_row(d.tube, tube::Effect::noise, 35);                          // the TV, just now
    app::apply_tube_ops(parsed(log_of(R"({"seq":1,"op":"row","key":"hum","value":20})")), d, live, true);
    REQUIRE(*d.tube.rows[tube::index_of(tube::Effect::noise)] == 35);
    REQUIRE(*d.tube.rows[tube::index_of(tube::Effect::hum)] == 20);
    app::apply_tube_ops(parsed(log_of(R"({"seq":2,"op":"row","key":"noise","value":10})")), d, live, true);
    REQUIRE(*d.tube.rows[tube::index_of(tube::Effect::noise)] == 10);        // same row: the later op wins
}
TEST_CASE("tube ops: the file found at boot is adopted, never applied", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live;
    const auto ops = parsed(kOps);
    app::adopt_tube_ops(&ops, live);
    REQUIRE(live.adopted); REQUIRE(live.handled_seq == 16); REQUIRE(live.degauss_requests == 0);
    REQUIRE(d.tube == tube::TubeSetting{});
    REQUIRE(app::apply_tube_ops(ops, d, live, true).applied == 0);
    app::TubeLiveState empty; app::adopt_tube_ops(nullptr, empty);
    REQUIRE(empty.adopted); REQUIRE(empty.handled_seq == 0);
}
TEST_CASE("tube ops: a box where Tube is not offered consumes ops without applying them", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; app::adopt_tube_ops(nullptr, live);
    const auto r = app::apply_tube_ops(parsed(kOps), d, live, false);
    REQUIRE_FALSE(r.changed); REQUIRE(r.skipped == 5); REQUIRE(live.handled_seq == 16);
    REQUIRE(d.crt_engine == app::CrtEngine::Classic); REQUIRE(live.degauss_requests == 0);
}
TEST_CASE("tube ops: any bad op refuses the whole file", "[tube]") {
    const std::vector<std::string> bad = {
        "", "not json", "[]", R"({"version":2,"ops":[]})", R"({"version":1,"ops":{}})",
        log_of("5"), log_of(R"({"seq":0,"op":"reset"})"), log_of(R"({"seq":"3","op":"reset"})"),
        log_of(R"({"seq":3,"op":"reset"},{"seq":3,"op":"reset"})"),
        log_of(R"({"seq":4,"op":"reset"},{"seq":3,"op":"reset"})"),
        log_of(R"({"seq":1,"op":"wobble"})"),
        log_of(R"({"seq":1,"op":"row","key":"wobble","value":5})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":101})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":-1})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":"20"})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":20.0})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":true})"),
        log_of(R"({"seq":1,"op":"mask","mask":"Slot"})"), log_of(R"({"seq":1,"op":"look","look":"vhs"})"),
        log_of(R"({"seq":1,"op":"engine","engine":"plasma"})"),
        std::string(app::kTubeOpsMaxBytes + 1, ' ')};
    for (const auto& text : bad) {
        INFO(text.substr(0, 120));
        const auto p = app::parse_tube_ops(text);
        REQUIRE_FALSE(p.ops); REQUIRE_FALSE(p.error.empty());
    }
    REQUIRE(app::parse_tube_ops(log_of("")).ops->empty());
}
```

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** with jsoncpp (`Json::CharReaderBuilder`, `strictRoot`).
- [ ] **Step 4: Run** `./build-tube2/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: the web's op log, parsed and applied in order`

---

### Task 5: The kiosk publishes the tube state

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/status_writer.cpp` (`write_now`, before the `platform_policy_override` block)
- Test: `magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp`

**Interfaces:**
- Consumes: Task 2 (`tube_live`, `tube_offered`, `tube_not_offered_reason`, `crt_engine_name`); `app::effective_crt_engine`; `tube::resolve`, `moved_keys`, `effect_key`, `mask_name`, `look_name`; `state.display_settings.mode`.
- Produces: `root["tube"]` in `kiosk_status.json`.
  - Offered: `{"gate": true, "engine", "effective_engine", "crt_native": bool, "look", "mask" (resolved), "custom", "moved": [keys, then "mask"], "values": {44 keys: int}, "handled_seq", "degauss_requests"}` (counters as `Json::UInt64`).
  - Not offered: exactly `{"gate": false, "reason": "needs_pi5"|"not_ready", "engine", "effective_engine"}`, the effective engine computed with `tube_offered`.
  - No new write cadence: the existing change detection writes a changed body at once.

- [ ] **Step 1: Write the failing tests**

```cpp
namespace {
void tube_state(app::AppState& state) {
    state.platform_profile.tube_crt_engine = true; state.tube_renderer_ready = true;
    state.display_settings.mode = app::DisplayMode::MODERN_TV;
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    tube::select_look(state.display_settings.tube, tube::Look::tv);
    tube::set_row(state.display_settings.tube, tube::Effect::hum, 20);
    tube::set_mask(state.display_settings.tube, tube::Mask::dots);
}
Json::Value written_tube(const app::AppState& state, const fs::path& tmp) {
    fs::remove(tmp); app::StatusWriter w(tmp.string()); w.write_now(state); w.flush();
    Json::Value root; std::ifstream(tmp) >> root; return root["tube"];
}
}
TEST_CASE("status_writer publishes the tube state", "[remote][status][tube]") {
    app::AppState state; tube_state(state);
    state.tube_live.handled_seq = 12; state.tube_live.degauss_requests = 3;
    const Json::Value t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube.json");
    REQUIRE(t["gate"].asBool());
    REQUIRE(t["engine"].asString() == "tube"); REQUIRE(t["effective_engine"].asString() == "tube");
    REQUIRE_FALSE(t["crt_native"].asBool());
    REQUIRE(t["look"].asString() == "tv"); REQUIRE(t["mask"].asString() == "dots");
    REQUIRE(t["custom"].asBool());
    REQUIRE(t["moved"].size() == 2);
    REQUIRE(t["moved"][0].asString() == "hum"); REQUIRE(t["moved"][1].asString() == "mask");
    REQUIRE(t["values"].size() == 44);
    REQUIRE(t["values"]["hum"].asInt() == 20); REQUIRE(t["values"]["brightness"].asInt() == 58);
    REQUIRE(t["handled_seq"].asUInt64() == 12); REQUIRE(t["degauss_requests"].asUInt64() == 3);
}
TEST_CASE("status_writer: Tube not offered publishes the engine and the reason only", "[remote][status][tube]") {
    app::AppState state; tube_state(state); state.tube_renderer_ready = false;
    Json::Value t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube2.json");
    REQUIRE(t.getMemberNames() == std::vector<std::string>{"effective_engine", "engine", "gate", "reason"});
    REQUIRE(t["reason"].asString() == "not_ready");
    REQUIRE(t["effective_engine"].asString() == "enhanced"); REQUIRE(t["engine"].asString() == "tube");
    state.platform_profile.tube_crt_engine = false;
    t = written_tube(state, fs::temp_directory_path() / "mdb_status_tube2.json");
    REQUIRE(t["reason"].asString() == "needs_pi5");
}
TEST_CASE("status_writer: a tube change is written at once", "[remote][status][tube]") {
    // StatusWriter(path, sink) with a sink that records bodies; tube_state; write_now;
    // set_row(hum, 25); write_now; flush → 2 bodies, the second has "hum":25 (no 2 s heartbeat wait).
}
```
(Write the last case out in full in the shape of the others.)

- [ ] **Step 2: Run, see them fail** — `cmake --build build-tube2 --target test_phone_remote_unit -j8 && ./build-tube2/test_phone_remote_unit "[tube]"`.
- [ ] **Step 3: Implement** a file-local `Json::Value tube_status_json(const AppState&)` in `status_writer.cpp` and one line in `write_now`.
- [ ] **Step 4: Run** `./build-tube2/test_phone_remote_unit` in full — pass.
- [ ] **Step 5: Commit** — `tube: the kiosk publishes the tube state for the web page`

---

### Task 6a: The TV menu — the Display rows

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.h`, `magic_dingus_box_cpp/src/ui/settings_menu.cpp` (`build_display_submenu`)
- Test: `magic_dingus_box_cpp/tests/ui/test_settings_menu_tube.cpp` (globbed into `test_ui_unit`)

**Interfaces:**
- Consumes: Tasks 1–2 (`tube_offered`, `display_crt_rows`, `next_crt_engine`, `crt_engine_label`, `crt_engine_sublabel`, `next_look`, `tube_look_label`, `tube_look_sublabel`, `tube_reset_sublabel`, `tube_group_label`, `tube_group_sublabel`); `app::SettingsPersistence::save_settings`; `ui_test::fake_settings_saves()`.
- Produces:
  - `MenuSection`: `TOGGLE_ENHANCED_CRT` renamed `CYCLE_CRT_ENGINE`; new `CYCLE_TUBE_LOOK`, `TUBE_RESET_LOOK`, `TUBE_GROUP` (a group row, and the submenu it opens in Task 6b), `TUBE_GROUP_BACK`, `TUBE_VALUE`, `TUBE_MASK`, `TUBE_DEGAUSS`. Not-applicable rows (Task 6b) use the existing `INFO`.
  - `SettingsMenuManager` public: `void enter_tube_group(std::size_t group);` (declared here; Task 6a's group rows call it, Task 6b builds what it opens — until then it only records the index and enters `TUBE_GROUP` with an empty list plus "Back").
  - Private: `std::size_t tube_group_index_ = 0;` `int tube_group_return_index_ = 0;`

Display submenu, decided here (every `offered` is `app::tube_offered(*app_state_)`):
- The engine row (label `crt_engine_label`, sublabel `crt_engine_sublabel`, action `set_crt_engine(next_crt_engine(saved, offered))`) stays where the toggle is.
- After it, by `display_crt_rows(saved, offered)`: either the seven existing rows, unchanged; or:
  `tube_look_label` / `tube_look_sublabel()` (action `select_look(next_look(look))`),
  `"Reset to look"` / `tube_reset_sublabel` (action `reset_to_look`; no save when nothing was moved),
  the ten `kGroups` rows (`tube_group_label` / `tube_group_sublabel`, section `TUBE_GROUP`, action `enter_tube_group(i)`),
  `"Degauss"` / `"demagnetize the tube"` (action `++tube_live.degauss_requests`; no save, no toast: nothing is drawn until Plan 3).
- Then `"Back"`.
- Every action that changes the engine or the tube calls `rebuild_current_submenu()`, then `save_settings` — as the Menu Style and Guide Mode rows do.

- [ ] **Step 1: Write the failing test** (helpers: `offer_tube(state)` sets `platform_profile.tube_crt_engine = true`, `tube_renderer_ready = true`, `mode = MODERN_TV`; `row(sm, prefix)` returns the first submenu item whose label starts with `prefix`, or `nullptr`; `press(sm, prefix)` COPIES that row's `action` and then calls the copy — an action rebuilds `submenu_items_`, which would destroy a `std::function` called in place; `labels(sm)`)

```cpp
TEST_CASE("Tube menu: the engine cycles through Tube when offered and the rows follow", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    REQUIRE(row(sm, "CRT Engine: Classic"));
    CHECK(row(sm, "CRT Engine: ")->sublabel == "Classic / Enhanced / Tube");
    CHECK(row(sm, "Scanlines: ")); CHECK_FALSE(row(sm, "Look: "));
    press(sm, "CRT Engine: "); press(sm, "CRT Engine: ");
    CHECK(state.display_settings.crt_engine == app::CrtEngine::Tube);
    CHECK(ui_test::fake_settings_saves() == 2);
    CHECK_FALSE(row(sm, "Scanlines: "));
    CHECK(row(sm, "Look: ")->sublabel == "PVM · Monitor · Trinitron · TV · Arcade · Everything");
    const auto all = labels(sm);
    const auto at = std::find(all.begin(), all.end(), "CRT Engine: Tube");
    CHECK(std::vector<std::string>(at + 1, all.end()) == std::vector<std::string>{
        "Look: PVM", "Reset to look", "Picture", "Scanlines", "Mask", "Light", "Glass", "Signal",
        "Supply", "Character", "Sync", "Colour purity", "Degauss", "Back"});
}
TEST_CASE("Tube menu: a Pi 4B is offered Classic and Enhanced only", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; state.tube_renderer_ready = true;   // board gate closed
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);                      // a Pi 5 backup
    tube::set_row(state.display_settings.tube, tube::Effect::hum, 20);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    REQUIRE(row(sm, "CRT Engine: Enhanced"));
    CHECK(row(sm, "CRT Engine: ")->sublabel == "Classic / Enhanced");
    CHECK(row(sm, "Scanlines: ")); CHECK_FALSE(row(sm, "Look: ")); CHECK_FALSE(row(sm, "Degauss"));
    CHECK(state.display_settings.crt_engine == app::CrtEngine::Tube);   // nothing rewritten by looking
    press(sm, "CRT Engine: ");
    CHECK(state.display_settings.crt_engine == app::CrtEngine::Classic);
    press(sm, "CRT Engine: ");
    CHECK(state.display_settings.crt_engine == app::CrtEngine::Enhanced);
    CHECK(*state.display_settings.tube.rows[tube::index_of(tube::Effect::hum)] == 20);  // kept
}
TEST_CASE("Tube menu: a Pi 5 with the renderer not ready offers nothing new", "[settings_menu][tube]") {
    // platform_profile.tube_crt_engine = true, tube_renderer_ready left at its default (false):
    // the same assertions as the Pi 4B case — "CRT Engine: Classic", sublabel "Classic / Enhanced",
    // the seven rows, no "Look: ", no "Degauss"; pressing twice goes Classic → Enhanced → Classic.
}
TEST_CASE("Tube menu: picking a look clears what was moved; Reset keeps the look", "[settings_menu][tube]") {
    // offered, engine Tube, set_row(hum, 20): row "Look: PVM (1 moved)"; press → "Look: Monitor",
    // moved_keys empty, one save; set_row(noise, 30); press "Reset to look" → look still monitor,
    // nothing moved, sublabel "nothing moved", one more save; press Reset again → no further save.
}
TEST_CASE("Tube menu: Degauss records a request and saves nothing", "[settings_menu][tube]") {
    // offered, engine Tube: press "Degauss" → tube_live.degauss_requests == 1, saves unchanged.
}
```
(Write the sketched cases out in full in the shape of the first two.)

- [ ] **Step 2: Run, see it fail** — `cmake --build build-tube2 --target test_ui_unit -j8`.
- [ ] **Step 3: Implement** the sections, the renamed engine row and the Tube rows in `build_display_submenu`. Update existing Display tests only if they relied on the old engine label.
- [ ] **Step 4: Run** `./build-tube2/test_ui_unit` in full — pass.
- [ ] **Step 5: Commit** — `tube: the TV menu's Display rows — engine, look, reset, groups, degauss`

---

### Task 6b: The TV menu — group submenus, edit mode, back and input

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.h`, `magic_dingus_box_cpp/src/ui/settings_menu.cpp`, `magic_dingus_box_cpp/src/ui/renderer_settings.cpp` (the header text and colour, ~l.72–113), `magic_dingus_box_cpp/src/app/settings_input.cpp`
- Test: `magic_dingus_box_cpp/tests/ui/test_settings_menu_tube.cpp`

**Interfaces:**
- Consumes: Task 6a (sections, `enter_tube_group`, the two private fields); Task 2 (`tube::Edit`, `begin_edit`, `edit_turn`, `end_edit`, `edit_direction`, `tube_row_kind`, `tube_value_label`, `tube_mask_label`, `tube_na_label`, `kTubeNaSublabel`, `tube_group_label`, `display_crt_rows`, `tube_offered`); Task 1 (`mask_choice`); `tube::row_info`, `resolve`, `kGroups`; `app::ContentSignature`.
- Produces — `SettingsMenuManager` public: `void exit_tube_group();` `std::size_t tube_group_index() const;` `bool is_tube_editing() const;` `void begin_tube_edit(std::optional<tube::Effect> effect);` `void step_tube_edit(int direction);` `void end_tube_edit();` `std::string submenu_title() const;`; private `std::vector<MenuItem> build_tube_group_submenu();` and `tube::Edit tube_edit_;`. `settings_menu.h` includes `../tube/tube_edit.h` (standard library only).

Decided here:
- **A group submenu** (`current_submenu_ == TUBE_GROUP`): when `has_mask_row`, the MASK row first; then each row of the group; then `"Back"` (`TUBE_GROUP_BACK`, action `exit_tube_group()`). A value row: label `tube_value_label(e, resolve(tube).effects[i], editing this row)`, sublabel `row_info(e).hint1`, section `TUBE_VALUE`, action `begin_tube_edit(e)`. The MASK row: `tube_mask_label`, sublabel `mask_choice(mask).hint1`, section `TUBE_MASK`, action `begin_tube_edit(std::nullopt)`. When `tube_row_kind(..., mode == CRT_NATIVE)` is `NotApplicable`: `tube_na_label(label)`, sublabel `kTubeNaSublabel`, section `INFO`, no action.
- `enter_tube_group` remembers the cursor in `tube_group_return_index_`; `exit_tube_group` ends any edit and returns to `DISPLAY` with the cursor back on that row (clamped). `enter_submenu(TUBE_GROUP)` builds `build_tube_group_submenu()`, so `rebuild_current_submenu()` works inside a group — except that when `display_crt_rows(saved, tube_offered).tube` is false (the engine was changed from the web while the group was open), it ends any edit and calls `exit_tube_group()` instead.
- **Edit mode:** `begin_tube_edit` → `tube::begin_edit` + rebuild. `step_tube_edit(direction)` → `tube::edit_turn`; rebuild when it returns true; never saves. `end_tube_edit` → `tube::end_edit`; rebuild; `save_settings` once when it returned true. `close()`, `force_close()`, `exit_submenu()`, `exit_tube_group()` and a fresh (non-re-entry) `enter_submenu` call `end_tube_edit()` first.
- **BTN4** (`toggle()`), inserted just before the "Depth 2" check: `if (tube_edit_.active) { end_tube_edit(); return; }` then `if (current_submenu_ == MenuSection::TUBE_GROUP) { exit_tube_group(); return; }`. (The box's pads never send `QUIT`; BTN4 is their back.)
- **Header:** `submenu_title()` returns what the renderer's header shows: `"Settings"` at the top level, the existing words for the existing submenus (`"Video games"`, `"Display"`, `"Audio"`, `"System info"`, `"Wi-Fi"`, `"Wi-Fi Networks"`), and `tube_group_label(kGroups[tube_group_index_])` in a group. `renderer_settings.cpp` takes its header text from it and gives `TUBE_GROUP` Display's gold (`theme_->highlight3`).
- **Redraw:** `is_static_for_redraw()` is unchanged (edit mode moves nothing on its own). `redraw_signature()` adds `tube_edit_.active`, `tube_edit_.mask`, `index_of(tube_edit_.effect)` and `tube_group_index_`; values reach it through the row labels.
- **Input** (`settings_input.cpp`, just before `// Normal settings menu handling`):
  ```cpp
  if (settings_menu.is_tube_editing()) {
      switch (ev.action) {
          case InputAction::ROTATE:
          case InputAction::ROTATE_VERTICAL:
              settings_menu.step_tube_edit(
                  tube::edit_direction(ev.delta, ev.action == InputAction::ROTATE_VERTICAL));
              break;
          case InputAction::SELECT:
          case InputAction::QUIT:
              if (ev.pressed) settings_menu.end_tube_edit();   // QUIT (keyboard) leaves edit mode, not the menu
              break;
          default: break;
      }
      return true;
  }
  ```
  BTN4 reaches `toggle()` through `handle_menu_button` (short press); BTN4 held + turn is still master volume.

- [ ] **Step 1: Write the failing test** (same file and helpers as Task 6a)

```cpp
TEST_CASE("Tube menu: a group opens its rows and Back returns to it", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Supply");
    REQUIRE(sm.get_current_submenu() == MenuSection::TUBE_GROUP);
    CHECK(sm.submenu_title() == "Supply");
    CHECK(labels(sm) == std::vector<std::string>{
        "FLICKER: 0", "HUM BAR: 0", "BRIGHTNESS LIMIT: 20", "BREATHING: 15", "Back"});
    CHECK(row(sm, "HUM BAR")->sublabel == "soft at low · pronounced at 100");
    press(sm, "Back");
    CHECK(sm.get_current_submenu() == MenuSection::DISPLAY);
    CHECK(sm.submenu_title() == "Display");
    CHECK(sm.get_current_highlighted_label() == "Supply");
}
TEST_CASE("Tube menu: edit mode steps the value and saves once on leaving", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Supply"); press(sm, "HUM BAR");
    REQUIRE(sm.is_tube_editing());
    CHECK(sm.get_submenu_items()[1].label == "HUM BAR: < 0 >");
    const auto saves = ui_test::fake_settings_saves();
    sm.step_tube_edit(+1); sm.step_tube_edit(+1);
    CHECK(sm.get_submenu_items()[1].label == "HUM BAR: < 10 >");
    CHECK(ui_test::fake_settings_saves() == saves);           // not per step
    sm.end_tube_edit();
    CHECK(ui_test::fake_settings_saves() == saves + 1);
    CHECK(sm.get_submenu_items()[1].label == "HUM BAR: 10");
    sm.end_tube_edit(); CHECK(ui_test::fake_settings_saves() == saves + 1);
}
TEST_CASE("Tube menu: BTN4 leaves edit mode, then the group", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.open(); sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Supply"); press(sm, "HUM BAR"); sm.step_tube_edit(+1);
    const auto saves = ui_test::fake_settings_saves();
    sm.toggle();
    CHECK_FALSE(sm.is_tube_editing());
    CHECK(sm.get_current_submenu() == MenuSection::TUBE_GROUP);
    CHECK(ui_test::fake_settings_saves() == saves + 1);
    sm.toggle();
    CHECK(sm.get_current_submenu() == MenuSection::DISPLAY);
    CHECK(sm.get_current_highlighted_label() == "Supply");
    sm.toggle();
    CHECK(sm.get_current_submenu() == MenuSection::BACK);
}
TEST_CASE("Tube menu: closing the menu mid-edit saves the edit", "[settings_menu][tube]") {
    // begin edit on HUM BAR, step +1, sm.close() → !is_tube_editing(), one save.
}
TEST_CASE("Tube menu: an engine change from outside leaves the open group", "[settings_menu][tube]") {
    // in the Supply group with HUM BAR in edit mode and stepped once; set_crt_engine(Enhanced) directly
    // (as the op log would); rebuild_current_submenu() → current submenu DISPLAY, not editing, one save,
    // and row "Scanlines: " present.
}
TEST_CASE("Tube menu: CRT Native shows the tube's own rows as n/a", "[settings_menu][tube]") {
    // mode CRT_NATIVE, offered, engine Tube: "Scanlines" group labels == {"SCANLINES: n/a",
    // "SCANLINE SIZE: 10", "BEAM SWELL: 80", "TRUE RASTER: n/a", "Back"}; the n/a rows have section INFO,
    // no action and sublabel kTubeNaSublabel; "Mask" group == {"MASK: n/a", "MASK STRENGTH: n/a",
    // "MASK SIZE: n/a", "Back"}.
}
TEST_CASE("Tube menu: the MASK row cycles in edit mode", "[settings_menu][tube]") {
    // "Mask" group: press "MASK" → label "MASK: < GRILLE >", sublabel "aperture grille · default";
    // step +1 → "MASK: < MAGENTA-GREEN >"; end → one save, resolve(tube).mask == magentaGreen.
}
TEST_CASE("Tube menu: edit mode stays static and redraws on each step", "[settings_menu][redraw][tube]") {
    // open() and finish the slide (as test_settings_menu_redraw.cpp does); enter Supply; s0 = signature;
    // begin edit on HUM BAR → signature != s0, is_static_for_redraw() still true; s1 = signature;
    // step +1 → signature != s1.
}
```
(Write the sketched cases out in full.)

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** the group submenu, edit mode, `toggle()`, `submenu_title()` and the signature in `settings_menu.{h,cpp}`; the header in `renderer_settings.cpp`; the input block in `settings_input.cpp`.
- [ ] **Step 4: Run** `./build-tube2/test_ui_unit` and `./build-tube2/test_app_unit` in full — pass. `settings_input.cpp` and `renderer_settings.cpp` are checked by CI's `kiosk-build` job.
- [ ] **Step 5: Commit** — `tube: the TV menu's groups, edit mode and back button`

---

### Task 7: The kiosk polls, applies and saves

**Files:**
- Create: `magic_dingus_box_cpp/src/app/tube_live.h`, `magic_dingus_box_cpp/src/app/tube_live.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (`APP_SOURCES` and `test_app_unit`: `src/app/tube_live.cpp`), `magic_dingus_box_cpp/src/main.cpp`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_live.cpp`

**Interfaces:**
- Consumes: Task 4 (`parse_tube_ops`, `adopt_tube_ops`, `apply_tube_ops`, `kTubeOpsMaxBytes`); Task 2 (`TubeLiveState`, `tube_offered`); `SettingsPersistence::save_settings`, `set_file_sink_for_tests`, `flush_pending_saves`; `app::ContentSignature`; `config::get_data_path()`; Task 6b's `SettingsMenuManager::rebuild_current_submenu` (main.cpp only).
- Produces (namespace `app`):
  - `class TubeRequestPoller` —
    `using Clock = std::chrono::steady_clock;`
    `static constexpr std::chrono::milliseconds kPollPeriod{100};` `static constexpr std::chrono::seconds kSaveQuiet{1};`
    `explicit TubeRequestPoller(std::string path);`
    `bool poll(AppState& state, Clock::time_point now);` — every call: saves (`save_settings`) when a debounced save is due. At most every `kPollPeriod`: the file's version is (`st_ino` from `::stat`, `std::filesystem::last_write_time(p, ec)`, `std::filesystem::file_size(p, ec)`) — `os.replace` always brings a new inode, so two writes inside one coarse mtime tick are still told apart. Only on the first look or a new version: read it (≤ `kTubeOpsMaxBytes`; larger is a parse error) and parse. First look since boot → `adopt_tube_ops` (`nullptr` when absent or unparsable). Later → `apply_tube_ops(..., tube_offered(state))`; when it reports `changed`, arm the save at `now + kSaveQuiet` (each later change re-arms it). A parse error logs one `LOG_WARN` per file version. Returns `changed`. Nothing here writes or throws.
    `void flush(AppState& state);` — saves now if a save is armed.
  - `void add_tube_redraw_signature(ContentSignature&, const AppState&);` — adds `crt_engine`, the look, the mask override (or a sentinel), each row override (or a sentinel), and `tube_live.degauss_requests`.
- main.cpp wiring (decided here):
  - Construct `app::TubeRequestPoller tube_requests(config::get_data_path() + "/tube_request.json");` next to the other web-to-kiosk paths.
  - In the loop, right after `controller.poll_text_input_queue(state);`:
    `if (tube_requests.poll(state, std::chrono::steady_clock::now()) && settings_menu.is_active()) settings_menu.rebuild_current_submenu();`
  - Redraw signature block: `app::add_tube_redraw_signature(sig, state);` next to the `display_settings.mode` line.
  - Shutdown, before `flush_pending_saves`: `tube_requests.flush(state);`

- [ ] **Step 1: Write the failing test** (helpers: a fresh temp dir per test; `write_log(path, ops_json)` writes `<path>.tmp` then `std::filesystem::rename`s it over `path`; `row_op(seq, value)` gives `{"seq":<seq>,"op":"row","key":"hum","value":<value>}`; `offered(state)` sets both gates true; `SaveCounter` installs a counting `set_file_sink_for_tests` sink and clears it in its destructor, `count()` calls `flush_pending_saves(1s)` first; `T0` a fixed time point; `hum(state)` reads the resolved hum)

```cpp
TEST_CASE("tube live: no file at start, then an op applies", "[tube]") {
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0)); REQUIRE(state.tube_live.adopted);
    write_log(path, row_op(1, 20));
    REQUIRE_FALSE(poller.poll(state, T0 + 50ms));          // inside the 100 ms period
    REQUIRE(poller.poll(state, T0 + 100ms));
    REQUIRE(hum(state) == 20); REQUIRE(state.tube_live.handled_seq == 1);
}
TEST_CASE("tube live: a file left from before a restart is adopted, not applied", "[tube]") {
    write_log(path, row_op(9, 20));
    app::AppState state; offered(state);
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    REQUIRE(state.display_settings.tube == tube::TubeSetting{});
    REQUIRE(state.tube_live.handled_seq == 9);
    REQUIRE_FALSE(poller.poll(state, T0 + 1s));            // same version: not read again
}
TEST_CASE("tube live: two writes inside one mtime tick are both seen", "[tube]") {
    // adopt at T0; write_log(row_op(1, 10)); t1 = last_write_time; poll T0+100ms (hum 10);
    // write_log(row_op(2, 15)) — the same length — then last_write_time(path, t1): only the inode
    // differs; poll T0+200ms → true, hum 15.
}
TEST_CASE("tube live: ops written between polls all land, in order", "[tube]") {
    // adopt at T0; write row_op(1,10); then a log with row_op(1,10), mask op seq 2 "dots", row_op(3,20)
    // (all before the next poll); poll T0+100ms → hum 20, mask dots, handled_seq 3.
}
TEST_CASE("tube live: saves once, a second after the last change", "[tube]") {
    SaveCounter saves;
    // adopt at T0; write seq 1, poll T0+100ms (applied); write seq 1..2, poll T0+600ms (applied);
    // poll T0+1500ms → saves.count() == 0; poll T0+1600ms → 1; poll T0+3s → still 1.
}
TEST_CASE("tube live: polling never writes", "[tube]") {
    SaveCounter saves;
    // adopt at T0; write seq 1; record the file's bytes and last_write_time; poll every 10 ms from
    // T0 to T0+900ms → saves.count() == 0 and the file's bytes and mtime unchanged (the kiosk never
    // writes the op log; the only write is the save at T0+1100ms).
}
TEST_CASE("tube live: flush saves a pending change at once", "[tube]") {
    // apply one op, flush(state) → saves.count() == 1; poll after T0+5s → still 1.
}
TEST_CASE("tube live: an unreadable file is skipped and the next good one applies", "[tube]") {
    // adopt at T0; write "not json"; poll T0+100ms → false, nothing changed;
    // write_log(row_op(1, 20)); poll T0+200ms → applied.
}
TEST_CASE("tube live: not offered, ops are consumed and nothing changes", "[tube]") {
    // state with tube_renderer_ready false; adopt; write a log with an engine "tube" op seq 1;
    // poll → false, crt_engine Classic, handled_seq 1, no save armed (flush → 0 saves).
}
TEST_CASE("tube live: any tube change moves the redraw signature", "[tube]") {
    app::AppState state; app::ContentSignature a, b, c;
    app::add_tube_redraw_signature(a, state);
    tube::set_row(state.display_settings.tube, tube::Effect::hum, 20); app::add_tube_redraw_signature(b, state);
    ++state.tube_live.degauss_requests;                                  app::add_tube_redraw_signature(c, state);
    REQUIRE(a.value() != b.value()); REQUIRE(b.value() != c.value());
}
```
(Write the sketched cases out in full.)

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** `tube_live.{h,cpp}` and the four main.cpp edits.
- [ ] **Step 4: Run** `./build-tube2/test_app_unit` in full — pass.
- [ ] **Step 5: Commit** — `tube: the kiosk applies the web's ops at once and saves when they stop`

---

### Task 8: The web route module

**Files:**
- Create: `magic_dingus_box/web/admin_display.py`
- Modify: `magic_dingus_box/web/admin.py` (import `admin_display` in both import forms; `admin_display.register(app, ctx)` after `admin_media_browser_ops.register(app, ctx)`, before `ctx.sock = sock`)
- Test: `magic_dingus_box/web/tests/test_admin_display.py`

**Interfaces:**
- Consumes: Task 3 (`static/tube_rows.json`); Task 4 (the op log's shape); Task 5 (the status `tube` block, `handled_seq`, `reason`); `ctx.data_dir`, `ctx.require_csrf`; `admin_common.error_response`, `success_response`.
- Produces (`admin_display.py`, no import of `admin`):
  - Constants: `REQUEST_NAME = "tube_request.json"`, `STATUS_NAME = "kiosk_status.json"`, `STATUS_MAX_AGE_S = 10.0`, `MAX_OPS_PER_POST = 64`, `MAX_FILE_BYTES = 16 * 1024`.
  - `load_rows() -> dict` — `static/tube_rows.json`, read once in `register`.
  - `kiosk_tube(data_dir: Path, now: float | None = None) -> tuple[dict | None, str | None]` — `(tube block, None)` when offered, else `(None, reason)`: `"kiosk_not_running"` (status missing, unparsable, or `ts` older than `STATUS_MAX_AGE_S`), `"kiosk_too_old"` (no `tube` key), or the block's own `reason` (`"needs_pi5"` / `"not_ready"`) when `gate` is false.
  - `validate_ops(body, rows: dict) -> tuple[list | None, dict | None]` — `(ops, None)` or `(None, {"index": i, "field": f})`: body a dict with `ops` a list of 1..`MAX_OPS_PER_POST` dicts; `op` one of `row|mask|look|reset|engine|degauss`; row: `key` a row key and `value` with `type(value) is int` (so `bool` is refused) within `row_min..row_max`; mask/look/engine: a known word from the row file; reset/degauss: no other field read. The clean ops keep only the fields named here.
  - `append_ops(existing: list, new: list, handled_seq: int) -> tuple[list, int]` — pure: `top = max(highest seq in existing, handled_seq)`; keep the existing ops with `seq > handled_seq`; for each new op in order: `top += 1`, give it `seq = top`, and when it is a row op drop any kept row op with the same `key`; append. Returns `(ops, top)`.
  - `register(app, ctx) -> None` with:
    - `GET /admin/display/tube` → always 200: `success_response({"available": True, "rows": ROWS, "state": tube})` or `{"available": False, "reason": reason}` (no rows); with `?state_only=1` the `rows` key is left out.
    - `POST /admin/display/tube` (`@require_csrf`) → in order: `validate_ops` → 400 `VALIDATION_ERROR` with `details`; `kiosk_tube` → 409 `TUBE_UNAVAILABLE` with `details={"reason": reason}`, nothing written; then under a module `threading.Lock`: read the existing log (missing or unparsable = no ops), `append_ops(existing, ops, tube["handled_seq"])`; if the serialized file would exceed `MAX_FILE_BYTES` → 409 `TUBE_BACKLOG` (the kiosk is not keeping up), nothing written; write `{"version": 1, "ops": [...]}` with `tempfile.mkstemp(dir=data_dir, prefix=".tube_request.", suffix=".tmp")` + `os.replace` (no fsync: transient; on failure remove the temp file and return 500 `INTERNAL_ERROR`) → `success_response({"seq": top})`.
  - Never touches `settings.json`, `crt_engine` or `enhanced_crt_enabled`.

- [ ] **Step 1: Write the failing tests** (helpers: `write_status(data_dir, tube, ts=None)` writes `{"schema": 1, "ts": ..., "tube": tube}`; `OPEN` an offered block with `handled_seq` 0 and 44 `values`; `HUM = {"op": "row", "key": "hum", "value": 20}`; `post(client, ops)` posts `{"ops": ops}` to `/admin/display/tube`; `log(data_dir)` loads the op log)

```python
def test_get_returns_rows_and_state(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    data = client.get("/admin/display/tube").get_json()["data"]
    assert data["available"] is True
    assert len(data["rows"]["rows"]) == 44 and data["state"] == OPEN
    data = client.get("/admin/display/tube?state_only=1").get_json()["data"]
    assert "rows" not in data and data["state"]["handled_seq"] == 0

def test_get_says_kiosk_not_running_when_status_is_stale(client, temp_data_dir):
    assert client.get("/admin/display/tube").get_json()["data"]["reason"] == "kiosk_not_running"
    write_status(temp_data_dir, OPEN, ts=time.time() - 30)
    data = client.get("/admin/display/tube").get_json()["data"]
    assert data["available"] is False and data["reason"] == "kiosk_not_running"

def test_get_says_kiosk_too_old_without_a_tube_block(client, temp_data_dir):
    (temp_data_dir / "kiosk_status.json").write_text(json.dumps({"schema": 1, "ts": time.time()}))
    assert client.get("/admin/display/tube").get_json()["data"]["reason"] == "kiosk_too_old"
    assert post(client, [HUM]).status_code == 409

@pytest.mark.parametrize("reason", ["needs_pi5", "not_ready"])
def test_not_offered_is_reported_and_writes_nothing(client, temp_data_dir, reason):
    write_status(temp_data_dir, {"gate": False, "reason": reason, "engine": "enhanced",
                                 "effective_engine": "enhanced"})
    data = client.get("/admin/display/tube").get_json()["data"]
    assert data == {"available": False, "reason": reason}
    r = post(client, [HUM])
    assert r.status_code == 409 and r.get_json()["error"]["details"]["reason"] == reason
    assert not (temp_data_dir / "tube_request.json").exists()

def test_post_appends_ops_with_increasing_seq(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    assert post(client, [{"op": "look", "look": "tv"}, HUM]).get_json()["data"]["seq"] == 2
    assert post(client, [{"op": "degauss"}]).get_json()["data"]["seq"] == 3
    assert log(temp_data_dir) == {"version": 1, "ops": [
        {"seq": 1, "op": "look", "look": "tv"},
        {"seq": 2, "op": "row", "key": "hum", "value": 20},
        {"seq": 3, "op": "degauss"}]}

def test_handled_ops_are_dropped_and_seq_continues(client, temp_data_dir):
    write_status(temp_data_dir, OPEN); post(client, [HUM, HUM])
    write_status(temp_data_dir, {**OPEN, "handled_seq": 2})
    post(client, [{"op": "reset"}])
    assert log(temp_data_dir)["ops"] == [{"seq": 3, "op": "reset"}]

def test_seq_continues_past_what_the_kiosk_handled(client, temp_data_dir):
    write_status(temp_data_dir, {**OPEN, "handled_seq": 57})        # the file was wiped
    assert post(client, [HUM]).get_json()["data"]["seq"] == 58

def test_rapid_posts_keep_only_the_newest_op_per_row(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    seqs = [post(client, [{**HUM, "value": v}]).get_json()["data"]["seq"] for v in range(0, 101, 5)]
    assert seqs == list(range(1, 22))
    assert log(temp_data_dir)["ops"] == [{"seq": 21, "op": "row", "key": "hum", "value": 100}]
    assert not list(temp_data_dir.glob(".tube_request.*"))

def test_two_pages_ops_both_land_in_seq_order(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    post(client, [HUM])                                              # page A
    post(client, [{"op": "row", "key": "noise", "value": 30}])      # page B
    post(client, [{**HUM, "value": 25}])                             # page A again
    assert log(temp_data_dir)["ops"] == [
        {"seq": 2, "op": "row", "key": "noise", "value": 30},
        {"seq": 3, "op": "row", "key": "hum", "value": 25}]

@pytest.mark.parametrize("op,field", [({"op": "wobble"}, "op"), ({"op": "row", "key": "wobble", "value": 5}, "key"),
    ({"op": "row", "key": "hum", "value": 101}, "value"), ({"op": "row", "key": "hum", "value": "20"}, "value"),
    ({"op": "row", "key": "hum", "value": True}, "value"), ({"op": "row", "key": "hum", "value": 20.0}, "value"),
    ({"op": "mask", "mask": "Slot"}, "mask"), ({"op": "look", "look": "vhs"}, "look"),
    ({"op": "engine", "engine": "plasma"}, "engine")])
def test_post_refuses_a_bad_op_and_writes_nothing(client, temp_data_dir, op, field):
    write_status(temp_data_dir, OPEN)
    r = post(client, [HUM, op])
    assert r.status_code == 400 and r.get_json()["error"]["details"] == {"index": 1, "field": field}
    assert not (temp_data_dir / "tube_request.json").exists()

def test_post_refuses_an_empty_or_oversized_batch(client, temp_data_dir): ...   # [] and 65 ops → 400

def test_post_requires_csrf(client, temp_data_dir, monkeypatch):
    monkeypatch.delenv("MAGIC_DISABLE_CSRF")
    write_status(temp_data_dir, OPEN)
    r = client.post("/admin/display/tube", json={"ops": [HUM]})
    assert r.status_code == 403 and r.get_json()["error"]["code"] == "CSRF_ERROR"
```

- [ ] **Step 2: Run, see them fail** — `python -m pytest magic_dingus_box/web/tests/test_admin_display.py -q`.
- [ ] **Step 3: Implement** `admin_display.py` and the two `admin.py` edits.
- [ ] **Step 4: Run** the new file, then the whole web suite `python -m pytest magic_dingus_box/web/tests/ -q` — pass.
- [ ] **Step 5: Commit** — `tube: Content Manager routes for the CRT look`

---

### Task 9: The CRT Look tab

**Files:**
- Create: `magic_dingus_box/web/static/manager_display.js`
- Modify: `magic_dingus_box/web/static/index.html` (top tab `id="crtLookTabNavBtn"` and bottom-nav item `id="crtLookBottomNavBtn"`, both `style="display: none;"` like the Media Browser's; `<section id="crtlook" class="tab-content" style="display: none;">`; `<script src="/static/manager_display.js?v=1"></script>` after `vpn_settings.js`; bump `manager.js` to `?v=127`), `magic_dingus_box/web/static/manager.js` (`switchTab`; the `deviceChanged` handler beside `checkMediaBrowserVisibility()`)
- Test: `magic_dingus_box/web/tests/test_manager_display_js.py`

**Interfaces:**
- Consumes: Task 8's GET/POST and error codes; Task 5's status block; Task 3's row file (`looks`, `rows`, `groups`, `mask`, `engines`); `apiGet`, `apiPost`, `currentDevice.url`, `escapeHtml` from `manager.js`.
- Produces (top-level `function` declarations and nothing that runs while the file loads — no DOM access, no call; `function` rather than `const`/`let` so the node test finds them on its `vm` context):
  - `tubeAddOp(unsent, op) -> unsent` — a `row` op replaces an unsent `row` op with the same `key` (removed, the new one appended); a second unsent `degauss` is dropped (one is enough); any other op is appended.
  - `tubeShown(kiosk, ops, rows) -> {engine, look, mask, values}` — the kiosk's state with `ops` (sent-but-unconfirmed, then unsent, in order) on top: `row` sets a value; `mask`; `engine`; `look` sets the look, its mask and its 44 values from `rows.looks`; `reset` restores the current look's values and mask.
  - `tubeConfirm(sent, handledSeq, lastHandled) -> sent` — `sent` is `[{ops, seq}]`; keeps the batches with `seq > handledSeq`; when `handledSeq < lastHandled` (a fresh kiosk), returns `[]`.
  - `makeOpSender(post) -> {add(op), pump(), onStatus(handledSeq), unsent(), sent()}` — `post(ops)` resolves to `{seq}` or rejects with an error carrying `code` (`apiPost`'s errors). `pump()` sends every unsent op in one POST when nothing is in flight. Success records `{ops, seq}` in `sent` and pumps again if ops arrived meanwhile. A network error, a 5xx, `TUBE_UNAVAILABLE` or `TUBE_BACKLOG` puts the ops back in front of anything added meanwhile (through `tubeAddOp`, so a newer value for the same row still wins) and leaves them for the next `pump()`. A `VALIDATION_ERROR` drops them and shows the message. `onStatus` calls `tubeConfirm` and then `pump()`, so anything unsent is retried on every 1 s poll; a pending `degauss` is carried until a POST succeeds.
  - `checkCrtLookVisibility()` — `GET .../admin/display/tube?state_only=1`; shows the two tab buttons and the section only when `available`, else hides them (and leaves the tab if it was open). Called from `manager.js`'s `deviceChanged` handler.
  - `startCrtLookTab()` / `stopCrtLookTab()` — called by `switchTab` (`tabId === 'crtlook'` starts; any other tab stops). Polls `GET ...?state_only=1` every 1 s while the tab is shown and the document is visible; the first load fetches the rows too.
- Behaviour (styling is the builder's, within the Content Manager's look):
  - The tab is not shown where Tube is not offered (a Pi 4B, the renderer not ready, an older kiosk). If the kiosk stops answering while the tab is open (`kiosk_not_running`), the tab stays and shows "Waiting for the TV…" with controls disabled and unsent ops kept; another reason hides the tab.
  - Controls: engine picker (`engines`), look picker (words plus `hint`), Reset, Degauss (enabled only when `effective_engine` is `tube`; its note says the effect appears once the Tube renderer lands), then the ten groups from `rows.groups` with one control per row: a 0–100 range input stepping by 1 (the row file's `step` is the TV's step, not a limit here) and its number, or a select of the five mask words; both hint lines; a "moved" mark when the shown value differs from the look's. A `crt_native_neutral` row is disabled with "a real tube draws this itself" when `state.crt_native`. When the engine is not Tube, a note says the sliders apply when CRT Engine is Tube.
  - Every control change calls `sender.add(op)` then `sender.pump()`. Every control shows `tubeShown(kiosk, sent ops + unsent ops, rows)`, except a range input the pointer is holding, which is never repainted.

- [ ] **Step 1: Write the failing test** (`test_manager_display_js.py`; skipped when `shutil.which("node")` is None, as `test_escape_js.py` does; runs node scripts through `subprocess.run(["node", "-e", script])` that load the file with `vm.runInNewContext(src, ctx)` where `ctx = {setTimeout, clearTimeout, console}` — which also proves it touches nothing while loading — and print JSON results the test asserts on)

```python
def test_index_loads_manager_display_last_with_a_version():
    html = INDEX.read_text()
    scripts = re.findall(r'<script src="/static/([\w.]+)\?v=\d+"></script>', html)
    assert scripts[-1] == "manager_display.js" and scripts[-2] == "vpn_settings.js"

def test_tab_buttons_are_hidden_by_default():
    html = INDEX.read_text()
    for el_id in ("crtLookTabNavBtn", "crtLookBottomNavBtn"):
        assert re.search(rf'id="{el_id}"[^>]*style="display: none;"', html)

def test_newest_value_per_row_wins_in_the_unsent_list(): ...
    # tubeAddOp: hum 10, noise 5, hum 20 → [noise 5, hum 20]; degauss twice → one degauss.

def test_shown_is_the_kiosk_state_with_ops_on_top(): ...
    # kiosk tv with hum 20; ops [{op:'row',key:'noise',value:30}] → values.noise 30, values.hum 20;
    # then {op:'look',value:'pvm'} → pvm's values and mask from the row file.

def test_confirmation_drops_what_the_kiosk_has_handled(): ...
    # sent [{seq:7}, {seq:9}], handled 8 → [{seq:9}]; handled 3 with lastHandled 9 → [].

def test_a_handled_seq_that_goes_backwards_resets_the_page(): ...
    # makeOpSender with a resolving post: add + pump → sent has seq 5; onStatus(5) → sent []; then
    # add + pump (seq 6), onStatus(2) → sent [] (fresh kiosk), nothing re-sent.

def test_sender_keeps_one_post_in_flight_and_the_newest_value_per_row_wins(): ...
    # post resolves after a tick and records bodies; add hum 10, pump; add hum 15, pump; add hum 20, pump
    # → after settling, bodies == [[hum 10], [hum 20]].

def test_failed_post_is_retried_on_the_next_pump_with_degauss_carried(): ...
    # post rejects once (code 'NETWORK'), then resolves; add degauss + hum 10, pump → rejected;
    # add hum 12; pump → one body [degauss, hum 12]; sent holds it.
```

- [ ] **Step 2: Run, see it fail** — `python -m pytest magic_dingus_box/web/tests/test_manager_display_js.py -q`.
- [ ] **Step 3: Implement** `manager_display.js`, the hidden tab markup, the `switchTab` branch and the `deviceChanged` call.
- [ ] **Step 4: Run** the test, then the web suite — pass. By hand: `python3 -m magic_dingus_box.web.wsgi` against a data dir holding a hand-written fresh `kiosk_status.json` with an offered `tube` block; the tab appears; drag a slider and watch `tube_request.json` hold one `hum` op whose `seq` climbs and whose value matches the slider; change the block to `"gate": false` and reload: the tab is gone.
- [ ] **Step 5: Commit** — `tube: the Content Manager's CRT Look tab`

---

### Task 10: The op log's place on a box, and the notes

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/deploy_cpp.sh` (next to `--exclude 'data/seek_request.json'`), `magic_dingus_box_cpp/scripts/update.sh` (the three `seek_request.json` exclude lines and the comment block near line 1965), `scripts/golden_image/first_boot.sh` (`step_wipe_phone_remote`'s data-dir list and its build/data list), `scripts/golden_image/prepare_for_cloning.sh` (the `SECRET_PATHS` loop), `tests/ota_rehearsal/box/manifest.py` (`RUNTIME`), `tests/ota_rehearsal/hw_helpers.py` (`APPDATA_EXCLUDE`), `OTA_UPDATE_GUARANTEES.md` (the transient-files row), `CLAUDE.md`, `CHANGELOG.md`

**Interfaces:**
- Consumes: the file name `data/tube_request.json` (Tasks 4, 7, 8).
- Produces: the file is excluded from every deploy/OTA rsync and every OTA rehearsal comparison like `seek_request.json`, and removed by `first_boot.sh` (both lists) and by `prepare_for_cloning.sh` before an image is taken. It holds no secret.

- [ ] **Step 1: Make the edits above.** Add `tube_request.json` beside `seek_request.json` in each list, in each file's own syntax.
- [ ] **Step 2: Check them** — `git grep -n "seek_request.json" -- magic_dingus_box_cpp/scripts scripts/golden_image tests/ota_rehearsal OTA_UPDATE_GUARANTEES.md` and `git grep -n "tube_request.json"` over the same paths list the same places; `bats tests/local/` passes.
- [ ] **Step 3: Notes.**
  - `CLAUDE.md`, under `ui/` → `settings_menu`: the Display submenu's Tube rows, group submenus and edit mode (SELECT enters, knob/D-pad steps, SELECT or BTN4 leaves and saves once; BTN4 then leaves the group), decisions in `tube/tube_edit.h` and `app/tube_menu_logic.h`.
  - `CLAUDE.md`, under `app/`: `tube_ops` (the op log's shape; adopt at boot, apply above `handled_seq`) and `tube_live` (100 ms poll keyed on inode+mtime+size, no write on the loop, save 1 s after the last change and at shutdown).
  - `CLAUDE.md`: the renderer-ready gate — `kTubeRendererReady` (false until Plan 3), `AppState::tube_renderer_ready`, `app::tube_offered(state)` as the one Tube test for the menu, the poller, the status and drawing.
  - `CLAUDE.md`, the web module map: `admin_display` (GET/POST `/admin/display/tube`, `append_ops`, never writes settings); the static load order: `manager_display.js` last; under Phone Remote, a short "CRT look live tuning" paragraph naming `tube_request.json` next to `text_input_queue.jsonl`.
  - `CLAUDE.md`: `static/tube_rows.json` is generated (`MDB_WRITE_TUBE_ROWS_JSON=1 ./build/test_app_unit "[tube_rows_json]"`) and checked from C++ and Python; Degauss is only counted (`tube_live.degauss_requests`) until Plan 3; update the `tube/` entry's "no production caller yet" sentence (the menu now calls `effective_crt_engine` through `tube_offered`).
  - `CHANGELOG.md`: nothing user-facing ships with the gate closed, so no `### Added` line. Add one `### Changed` line only if the release notes list internal work; otherwise leave the file alone, and Plan 3 announces the feature.
- [ ] **Step 4: Full verification.** Both Mac configurations, every suite, then the web suite:
  ```bash
  ctest --test-dir build-tube2 --output-on-failure
  cmake -S magic_dingus_box_cpp -B build-tube2 -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=ON && cmake --build build-tube2 -j8 && ctest --test-dir build-tube2 --output-on-failure
  python -m pytest magic_dingus_box/web/tests/ -q
  ```
  Expected: all pass (9 C++ suites with Media Browser ON, 8 OFF). Then `rm -rf build-tube2`.
  On hardware:
  - Pi 5 and Pi 4B, as shipped: no visible change; the CRT Look tab is absent.
  - Owner's Pi 5, with a local build that sets `kTubeRendererReady = true`: every row is reachable; edit mode steps with the knob and the D-pad; BTN4 leaves edit mode, then the group; a slider on a phone moves the number on the TV inside a second.
  - Check one Tube sublabel in a `screenshot_request` capture: if the menu font lacks `·`, say so in the PR.
- [ ] **Step 5: Commit** — `tube: the op log's place on a box, and the notes`

---

## Spec coverage

| Requirement | Task |
|---|---|
| §6 one row table, JSON generated and checked in, a test on each side | 3 |
| §6 engine row Classic/Enhanced/Tube, Tube only when offered | 2, 6a |
| §6 Look row listing the six looks, "(n moved)", one press restores a known look | 2, 6a |
| §6 Reset to look | 2, 6a |
| §6 ten groups with row counts; value + hint sublabel inside | 1, 2, 6a, 6b |
| §6 edit mode: SELECT enters, knob/D-pad steps, SELECT/back leaves, saved on leaving | 2, 6b |
| §6 steps of 5, size rows 10, MASK cycles five words | 2 (core `step_row`), 6b |
| §6 seven Classic/Enhanced rows only for those engines, Tube rows only for Tube | 2, 6a |
| §6 Content Manager CRT Look tab: engine, look, Reset, ten groups of sliders, hints, moved mark, any whole number | 9 |
| §6 / §2 Tube not offered: the tab is not shown | 8, 9 |
| §6 `admin_display.py` via `register`, `manager_display.js` loaded last with `?v=`, CSRF | 8, 9 |
| §5 CRT Native rows shown as not applicable | 2, 6b, 9 |
| §7 Read: status block with engine, look, mask, 44 values, moved, gate, last op handled | 5 |
| §7 page polls about once a second, never repaints a held slider | 9 |
| §7 Write: POST validates against the row table, op log with increasing seq, atomic file | 8 |
| §7 Apply: about ten checks a second, a stat not a write, in memory at once | 7 |
| §7 save once changes stop for about a second (existing async save) | 7 |
| §7 no slider movement lost while dragging | 7, 8, 9 |
| §7 an applied op is never applied again after a restart | 4, 7 |
| §7 a TV edit shows up on the page | 5, 9 |
| §7 a kiosk older than this feature ignores the file | 8 (no old kiosk reads it; no `tube` block → refused) |
| §7 file excluded from deploy/OTA rsyncs, deleted on clones | 10 |
| §8 Degauss: a menu row and a web button that only record the request | 4, 6a, 9 |
| §2 a Pi 5 backup's Tube on a Pi 4B shows as Enhanced, not rewritten | 2, 6a |
| Addendum: hidden behind "renderer ready" until Plan 3; releases in between safe | 2, 5, 6a, 7, 8, 9 |
