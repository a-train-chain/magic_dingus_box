# Tube CRT Engine — Controls (Plan 2 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every Tube row, the mask, the look, Reset and Degauss editable on the TV (Settings → Display) and in a new Content Manager **CRT Look** tab, with a change from the web on the TV within a fraction of a second — on top of the core, with no change to what any box draws.

**Architecture:** Decisions live in two pure headers, `src/tube/tube_edit.h` (edit mode) and `src/app/tube_menu_logic.h` (which rows, what they say); `settings_menu.cpp` only builds rows from them. The web side never writes `settings.json`: it writes a whole desired state with an increasing sequence number to `data/tube_request.json`; the kiosk stats it ten times a second (`app::TubeRequestPoller`), validates it (`app::parse_tube_request`), decides (`app::handle_tube_request`: a request built on a state some other writer has since changed is refused, so nobody's change is silently undone), applies it in memory and saves through the existing asynchronous save once changes stop for a second. The kiosk publishes the result in `kiosk_status.json`; the page reads it through `GET /admin/display/tube`. One generated `static/tube_rows.json` feeds the page, checked against the C++ tables from both sides. Until Plan 3, an engine saved as Tube draws as Enhanced, and Degauss is only counted.

**Tech Stack:** C++17, Catch2 (`test_app_unit`, `test_ui_unit`, `test_phone_remote_unit`), jsoncpp, Flask + pytest, plain classic-script JavaScript (helpers checked under node from pytest).

**Spec:** `docs/superpowers/specs/2026-11-15-tube-crt-engine-design.md` — §6 Controls, §7 Live tuning, the Degauss line of §8, §2 (gate closed), and the Addendum. Plan 1 (`docs/superpowers/plans/2026-11-15-tube-crt-core.md`) is built; its headers in `magic_dingus_box_cpp/src/tube/` are what this plan consumes.

**Open owner questions (Addendum):** this plan depends on neither. CRT Native's inset concerns picture fit, corner radius and margin, which are not rows; the menu and page mark "n/a" exactly the rows `tube::neutral_in_crt_native` names, so if the owner's answer adds a row there, both front ends follow with no edit. The speed-row jump is a per-frame renderer matter (Plan 3).

## Global Constraints

- The repository folder's name ends in a space: `.../📺 MDB/magic_dingus_box `. Work in the worktree `.worktrees/tube-crt-controls` (branch `tube-crt-controls`); use paths relative to its root.
- Dual-board contract: board differences only through `state.platform_profile.tube_crt_engine` and `app::effective_crt_engine(saved, gate)`. No `#ifdef` on a board.
- No new system dependency (kiosk or web). New C++ under `src/tube/` is header-only and standard-library-only.
- `config/settings.json` is written only by the kiosk, only through `app::SettingsPersistence::save_settings` (asynchronous). Tube rows are changed only through `tube::set_row` / `tube::set_mask` / `tube::select_look` / `tube::reset_to_look`; the engine only through `DisplaySettings::set_crt_engine`.
- No recurring file WRITE on the main loop. The request file is stat'ed at most every 100 ms and read only when its (mtime, size) changed. The kiosk never deletes or rewrites `tube_request.json`.
- Screens spawn, drain and paint: `settings_menu.cpp` and `settings_input.cpp` call the pure functions of Tasks 1–3 and contain no decision of their own beyond wiring.
- Web: route modules never import `admin`; mutating routes use `ctx.require_csrf`; error envelopes are `admin_common.error_response(code, message, status, details)`; new static script loads last, with `?v=`; any changed static file bumps its `?v=`.
- Exact values: request poll period **100 ms**; save after **1 s** with no applied change; kiosk status counts as fresh for **10 s** (`ts`); request file cap **16 KiB**; client id `^[0-9a-f]{8,32}$`; the kiosk's revision counter starts at a random value in **[2^40, 2^41)** each boot.
- Release order **(owner to confirm):** do not tag a release from `main` between this plan's merge and Plan 3's: on a Pi 5 this plan makes Tube selectable while it still draws as Enhanced, and Degauss does nothing visible.
- Mac build (one directory; the Mac's disk is nearly full — delete it at the end):
  ```bash
  cmake -S magic_dingus_box_cpp -B build-tube2 -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=OFF
  cmake --build build-tube2 --target test_app_unit test_ui_unit test_phone_remote_unit -j8
  ```
  Web: `python -m pytest magic_dingus_box/web/tests/<file> -q` from the repo root (as `.github/workflows/test-local.yml` runs it). `main.cpp` and `settings_input.cpp` are kiosk-only: they compile in CI's `kiosk-build` job, not on the Mac.
- One commit per task; messages end with the session's attribution line.

## Review Focus

1. **Two phones tuning at once.** Each phone's last released value must survive; neither silently undoes the other. The server refuses (409 `TUBE_BUSY`) to overwrite another page's request the kiosk has not handled yet, the kiosk refuses a request built on a state another writer has since changed, and the refused page re-sends only its own change on the fresh state. (Task 4 `"tube request: a request built before another writer's change is refused"`; Task 8 `test_post_does_not_overwrite_another_pages_unhandled_request`; Task 9 `refusal resends the page's own changes on the fresh state`)
2. **The page left open across a kiosk restart.** Nothing is re-applied at boot (a request found at start is adopted, never applied); the page shows "waiting for the TV" while the status is stale; a drag cut off by the restart is re-sent once the kiosk is back. (Task 4 `"tube request: the first file after boot is adopted, never applied"`; Task 7 `"tube live: a file left from before a restart is adopted, not applied"`; Task 8 `test_get_says_kiosk_not_running_when_status_is_stale`)
3. **A TV edit while a web request is in flight.** The TV's change wins and is never reverted by a request built before it; the page then shows the TV value within about a second. (Task 4 `"tube request: a request built before another writer's change is refused"`; Task 5 `"status_writer: a tube change is written at once"`)
4. **A slider dragged faster than the kiosk polls.** The TV ends exactly on the released value; skipped intermediate requests are never applied late or out of order. (Task 7 `"tube live: only the newest of several writes between polls lands"`; Task 8 `test_rapid_posts_leave_the_last_state_with_increasing_seq`; Task 9 `latest sender keeps one request in flight and ends on the newest`)
5. **A Pi 4B (gate closed).** The TV menu offers Classic/Enhanced and the seven old rows only, a Pi 5 backup's saved Tube shows as Enhanced and is not rewritten until the user presses; the CRT Look tab explains it needs a Pi 5, offers no sliders, and a POST writes nothing. (Task 6 `"Tube menu: a Pi 4B is offered Classic and Enhanced only"`; Task 8 `test_closed_gate_is_not_available_and_writes_nothing`)

---

### Task 1: The words the controls need

**Files:**
- Modify: `magic_dingus_box_cpp/src/tube/tube_groups.h`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_groups.cpp`

**Interfaces:**
- Consumes: `tube::RowInfo { std::string_view label, hint1, hint2; }`, `row_info(Effect)`, `kEffects`, `kMasks`, `kLooks`, `index_of`, `resolve`, `select_look`, and `moves_on_its_own(const Resolved&)` from `tube_inputs.h`.
- Produces (namespace `tube`):
  - `constexpr const RowInfo& mask_choice(Mask) noexcept;` — `label` is the word shown for the mask, hints describe it.
  - `constexpr const RowInfo& look_choice(Look) noexcept;` — `label` is the look's display word.
  - Six reworded hints (below); `kMaskRowInfo` is unchanged.

Words (Sound Factory's `CRT_MASK_WORD` / `CRT_MASK_DETAIL` / `CRT_LOOK_DETAIL` at `4ef74a6`; slot's second line reworded, look words in the menu's title case):

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

Millimetre hints become shares of the picture's width (a millimetre is `1/196.608` of it, `tube_inputs.h`'s `kTubeMmAcross`), replacing hint2 of: `rollWidth` → `0: 4 % · 100: 31 % of picture width`; `tracking` → `0 off · up to 1.8 % of picture width`; `colorDrift` → `0 off · up to 0.4 % of picture width`; `trackingWidth` → `0: 1 % · 100: 16 % of picture width`; `flagging` → `0 off · up to 2 % of picture width`; `puritySize` → `0: 5 % · 100: 36 % of picture width`.

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
- [ ] **Step 3: Implement** in `tube_groups.h`: two index-aligned `detail::` arrays and the two accessors; edit the six hint strings in `detail::kRowInfo`. Note in the header comment that the six millimetre hints were converted for a TV.
- [ ] **Step 4: Run** `./build-tube2/test_app_unit "[tube]"` — all pass.
- [ ] **Step 5: Commit** — `tube: mask and look words, and hints in shares of the picture`

---

### Task 2: One generated row file for the web page

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_rows_json.h`
- Create: `magic_dingus_box/web/static/tube_rows.json` (generated, checked in)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_rows_json.cpp`, `magic_dingus_box/web/tests/test_tube_rows_json.py`

**Interfaces:**
- Consumes: Task 1; `kEffects`, `effect_key`, `row_info`, `row_step`, `kGroups` (`key`, `label`, `has_mask_row`, `rows`), `kMasks`, `mask_name`, `kLooks`, `look_name`, `look_values`, `kLookDefault`, `kRowMin`, `kRowMax`, `neutral_in_crt_native`; `magic_dingus_box_cpp/scripts/dev/check_tube_parity.py`'s `parse_tables(rows_text, looks_text)`.
- Produces:
  - `std::string tube::tube_rows_json();` (inline, standard library only) — deterministic bytes, 2-space indentation, UTF-8 written as-is (escape only `"` and `\`), ending in `\n`.
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
    `title` is the label in sentence case (first letter upper, the rest lower: `COLOUR PURITY` → `Colour purity`).
  - Regeneration: `MDB_WRITE_TUBE_ROWS_JSON=1 ./build-tube2/test_app_unit "[tube_rows_json]"` rewrites the file and passes.

- [ ] **Step 1: Write the failing tests**

`test_tube_rows_json.cpp`:
```cpp
namespace fs = std::filesystem;
const fs::path kRowFile = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path()
                          / "magic_dingus_box" / "web" / "static" / "tube_rows.json";

TEST_CASE("tube rows json: the checked-in file is what the tables generate", "[tube][tube_rows_json]") {
    const std::string want = tube::tube_rows_json();
    if (std::getenv("MDB_WRITE_TUBE_ROWS_JSON")) { std::ofstream(kRowFile, std::ios::binary) << want; }
    std::ifstream f(kRowFile, std::ios::binary);
    REQUIRE(f.good());
    const std::string have((std::istreambuf_iterator<char>(f)), {});
    INFO("regenerate: MDB_WRITE_TUBE_ROWS_JSON=1 ./build-tube2/test_app_unit \"[tube_rows_json]\"");
    REQUIRE(have == want);
}
TEST_CASE("tube rows json: it parses and holds every row", "[tube][tube_rows_json]") {
    Json::Value root; std::istringstream in(tube::tube_rows_json());
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
}
```

`test_tube_rows_json.py` (loads `check_tube_parity.py` with `importlib.util.spec_from_file_location`; reads the three headers as text):
- `test_keys_and_looks_match_the_headers` — `[r["key"] for r in rows]` equals `parse_tables(...)["keys"]`; each look's `values` (in key order) and `mask` equal `parse_looks`.
- `test_labels_hints_and_groups_match_tube_groups_h` — the `{"LABEL", "hint1", "hint2"}` triples inside `kRowInfo{{ … }}` (regex `\{"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\}`, in order) equal each row's `label` + `hint`; each `kGroups` entry's `{"key", "LABEL", bool, detail::rows_of(Effect::a, …)}` equals the group's key, label and rows (with `"mask"` first when the bool is `true`).
- `test_steps` — `step` is 10 for `lineSize` and `maskSize`, 5 otherwise.
- Every regex must match at least once, or the test fails (never a silent pass on an unparsed header).

- [ ] **Step 2: Run, see them fail** — the C++ build fails (`tube_rows_json.h` missing); `python -m pytest magic_dingus_box/web/tests/test_tube_rows_json.py -q` fails (file missing).
- [ ] **Step 3: Implement** `tube_rows_json.h`, then generate the file with the command above.
- [ ] **Step 4: Run** both suites — pass. Change one hint locally, see both fail, revert.
- [ ] **Step 5: Commit** — `tube: one generated row file for the web page, checked from both sides`

---

### Task 3: Edit mode and the menu's decisions (pure)

**Files:**
- Create: `magic_dingus_box_cpp/src/tube/tube_edit.h`
- Create: `magic_dingus_box_cpp/src/app/tube_menu_logic.h` (header-only, so `test_ui_unit` needs no new source)
- Modify: `magic_dingus_box_cpp/src/app/app_state.h` (next to `CrtEngine`; `AppState` member), `magic_dingus_box_cpp/src/app/settings_persistence.cpp` (its private `crt_engine_to_string` / `crt_engine_from_string` call the new pair)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_edit.cpp`, `magic_dingus_box_cpp/tests/app/test_tube_menu_logic.cpp`

**Interfaces:**
- Consumes: `tube::TubeSetting`, `resolve`, `set_row`, `set_mask`, `moved_keys`, `step_row`, `row_info`, `kGroups`, `Group`, `kMasks`, `kLooks`, `look_choice`, `mask_choice`, `neutral_in_crt_native`; `app::CrtEngine`, `app::effective_crt_engine`.
- Produces in `app_state.h` (namespace `app`):
  - `constexpr std::string_view crt_engine_name(CrtEngine) noexcept;` (`"classic"|"enhanced"|"tube"`) and `constexpr std::optional<CrtEngine> parse_crt_engine(std::string_view) noexcept;`
  - `inline constexpr std::string_view kTubeWriterTv = "tv";` `inline constexpr std::string_view kTubeWriterRestore = "restore";`
  - ```cpp
    // Who changed the tube last, and the web requests seen. In memory only.
    struct TubeLiveState {
        std::uint64_t rev = 0;              // bumped by every change to crt_engine or tube, from anywhere
        std::string last_writer;            // kTubeWriterTv, kTubeWriterRestore or a web client id
        std::uint64_t handled_seq = 0;      // newest request seq read (applied, unchanged or refused)
        std::uint64_t refused_seq = 0;      // newest request seq refused, or adopted unapplied
        std::uint64_t degauss_requests = 0; // presses on the TV or the web; Plan 3 consumes the increase
        std::uint64_t web_degauss_seen = 0; // the request file's running degauss count last seen
        bool adopted = false;               // the request file has been looked at since boot
        void note_change(std::string_view writer) { ++rev; last_writer = std::string(writer); }
    };
    ```
    and `TubeLiveState tube_live;` in `AppState`.
- Produces in `tube_edit.h` (namespace `tube`):
  - `struct Edit { bool active = false; bool mask = false; Effect effect = Effect::brightness; bool changed = false; };`
  - `void begin_edit(Edit&, std::optional<Effect> effect) noexcept;` — `nullopt` is the MASK row.
  - `constexpr int edit_direction(int delta, bool vertical_axis) noexcept;` — knob and left/right: the sign of `delta`; up/down (`ROTATE_VERTICAL`, up is `-1`): minus the sign.
  - `constexpr Mask next_mask(Mask, int direction) noexcept;` — through `kMasks`, wrapping.
  - `bool edit_turn(Edit&, TubeSetting&, int direction) noexcept;` — a value row: `set_row(e, step_row(e, resolve(s).effects[i], direction))`; the mask row: `set_mask(next_mask(resolve(s).mask, direction))`. True when the resolved value changed (then `changed = true`); false when inactive or clamped.
  - `bool end_edit(Edit&) noexcept;` — returns `changed` (save once), always leaves the edit inactive and unchanged.
- Produces in `tube_menu_logic.h` (namespace `app`):
  - `struct DisplayCrtRows { bool classic; bool tube; };` `DisplayCrtRows display_crt_rows(CrtEngine saved, bool gate) noexcept;` — from the effective engine: Tube → tube rows only; otherwise the seven Classic/Enhanced rows only.
  - `CrtEngine next_crt_engine(CrtEngine saved, bool gate) noexcept;` — steps from the EFFECTIVE engine: Classic → Enhanced → Tube (gate open) or → Classic (gate closed); Tube → Classic.
  - `std::string crt_engine_label(CrtEngine saved, bool gate);` (`"CRT Engine: Tube"`, from the effective engine) and `std::string_view crt_engine_sublabel(bool gate);` (`"Classic / Enhanced / Tube"` / `"Classic / Enhanced"`).
  - `tube::Look next_look(tube::Look) noexcept;` (wraps); `std::string tube_look_label(const tube::TubeSetting&);` (`"Look: TV"`, `"Look: TV (2 moved)"`); `std::string_view tube_look_sublabel(tube::Look);` (= `look_choice(l).hint1`).
  - `std::string tube_reset_sublabel(const tube::TubeSetting&);` (`"nothing moved"`, `"1 row moved"`, `"3 rows moved"`).
  - `std::string tube_group_label(const tube::Group&);` (sentence case) and `std::string tube_group_sublabel(const tube::Group&, const tube::TubeSetting&);` — `"<n> rows"` counting the MASK row, plus `" · <m> moved"` when `m > 0`.
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
TEST_CASE("tube menu logic: the engine cycle offers Tube only with the gate open", "[tube]") {
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
}
TEST_CASE("tube menu logic: labels", "[tube]") {
    using namespace tube;
    TubeSetting s; select_look(s, Look::tv);
    REQUIRE(app::tube_look_label(s) == "Look: TV");
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
TEST_CASE("tube live state: every change moves the revision", "[tube]") {
    app::TubeLiveState live; live.rev = 41;
    live.note_change(app::kTubeWriterTv);
    REQUIRE(live.rev == 42); REQUIRE(live.last_writer == "tv");
}
```

- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement** the two headers and the `app_state.h` additions; point `settings_persistence.cpp`'s two private helpers at `crt_engine_name` / `parse_crt_engine` (one spelling of the three words).
- [ ] **Step 4: Run** `./build-tube2/test_app_unit` in full — all pass (the persistence tests prove the helper swap changed nothing).
- [ ] **Step 5: Commit** — `tube: edit mode and the menu's decisions, pure`

---

### Task 4: The live-tuning request — parsing and deciding

**Files:**
- Create: `magic_dingus_box_cpp/src/app/tube_request.h`, `magic_dingus_box_cpp/src/app/tube_request.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (`APP_SOURCES` and the `test_app_unit` source list: `src/app/tube_request.cpp`)
- Test: `magic_dingus_box_cpp/tests/app/test_tube_request.cpp`

**Interfaces:**
- Consumes: Task 3 (`TubeLiveState`, `parse_crt_engine`); `tube::parse_look`, `parse_mask`, `parse_effect`, `is_row_value`, `select_look`, `set_mask`, `set_row`; `AppState::DisplaySettings::{crt_engine, tube, set_crt_engine}`.
- Produces (namespace `app`):
  - The file `data/tube_request.json`, written only by the web admin (Task 8):
    ```json
    {"version": 1, "seq": 7, "client": "a1b2c3d4e5f60718", "base_rev": 42, "degauss": 2,
     "state": {"engine": "tube", "look": "tv", "mask": "dots", "rows": {"hum": 20, "noise": 30}}}
    ```
    `seq` ≥ 1 increases per write; `client` is the writing page's id; `base_rev` is the `tube.rev` of the kiosk status the page built the state on; `degauss` is the running count of web Degauss presses (carried forward by the server); `state` is the whole desired state (`mask` `null` = the look's own; `rows` = only moved rows) or `null`.
  - `inline constexpr std::size_t kTubeRequestMaxBytes = 16 * 1024;`
  - `struct TubeRequestState { CrtEngine engine; tube::TubeSetting setting; };`
  - `struct TubeRequest { std::uint64_t seq = 0; std::string client; std::uint64_t base_rev = 0; std::uint64_t degauss = 0; std::optional<TubeRequestState> state; };`
  - `struct TubeRequestParse { std::optional<TubeRequest> request; std::string error; };`
  - `TubeRequestParse parse_tube_request(std::string_view text);` — any bad field refuses the WHOLE request with an error naming it: not JSON or not an object, `version != 1`, `seq` not an unsigned integer ≥ 1, `client` not matching `^[0-9a-f]{8,32}$`, `base_rev` / `degauss` not unsigned integers, `state` neither object nor null, an unknown engine/look/mask word, `rows` not an object, an unknown row key, a value that is not an integer (booleans and reals refused) or fails `is_row_value`, text longer than `kTubeRequestMaxBytes`. The setting is built by `select_look`, then `set_mask` when `mask` is a word, then `set_row` per row.
  - `enum class TubeRequestOutcome { Stale, Applied, Unchanged, Refused };`
  - `struct TubeRequestResult { TubeRequestOutcome outcome; bool degauss = false; std::string_view reason; };`
  - `void adopt_tube_request(const TubeRequest* found, TubeLiveState& live) noexcept;` — the first look after boot (`found == nullptr`: no file or an unreadable one). Sets `adopted`; with a request: `handled_seq = seq`, `web_degauss_seen = degauss`, and, when it carries a state, `refused_seq = seq` (it was not applied; its page re-sends). Never changes the setting.
  - `TubeRequestResult handle_tube_request(const TubeRequest&, AppState::DisplaySettings&, TubeLiveState&, bool gate_open);` in this order:
    1. `seq <= handled_seq` → `Stale`, nothing else.
    2. `handled_seq = seq`. If `degauss > web_degauss_seen`: `++degauss_requests`, `degauss = true` (one press per request, however many it carries). `web_degauss_seen = degauss`.
    3. `state` null → `Unchanged`.
    4. `!gate_open` → `Refused` (reason `"gate closed"`), `refused_seq = seq`.
    5. `base_rev != rev && last_writer != client` → `Refused` (reason `"built on an older state"`), `refused_seq = seq`.
    6. Same engine and `setting == tube` → `Unchanged`. Otherwise `set_crt_engine(engine)`, `tube = setting`, `note_change(client)` → `Applied`.

- [ ] **Step 1: Write the failing test**

```cpp
namespace {
constexpr const char* kClient = "a1b2c3d4e5f60718";
std::string req(std::uint64_t seq, std::uint64_t base, const std::string& state,
                std::uint64_t degauss = 0, const std::string& client = kClient) {
    return R"({"version":1,"seq":)" + std::to_string(seq) + R"(,"client":")" + client +
           R"(","base_rev":)" + std::to_string(base) + R"(,"degauss":)" + std::to_string(degauss) +
           R"(,"state":)" + state + "}";
}
const std::string kTv = R"({"engine":"tube","look":"tv","mask":"dots","rows":{"hum":20,"noise":30}})";
app::TubeRequest parsed(const std::string& text) { auto p = app::parse_tube_request(text); REQUIRE(p.request); return *p.request; }
}
TEST_CASE("tube request: a whole state parses through the core's setters", "[tube]") {
    const auto r = parsed(req(7, 42, kTv, 2));
    REQUIRE(r.seq == 7); REQUIRE(r.client == kClient); REQUIRE(r.base_rev == 42); REQUIRE(r.degauss == 2);
    tube::TubeSetting want; tube::select_look(want, tube::Look::tv); tube::set_mask(want, tube::Mask::dots);
    tube::set_row(want, tube::Effect::hum, 20); tube::set_row(want, tube::Effect::noise, 30);
    REQUIRE(r.state->engine == app::CrtEngine::Tube);
    REQUIRE(r.state->setting == want);
    const auto same = parsed(req(8, 42, R"({"engine":"tube","look":"tv","mask":"slot","rows":{"hum":60}})"));
    REQUIRE(same.state->setting == tube::TubeSetting{tube::Look::tv, std::nullopt, {}});  // the look's own values
    REQUIRE_FALSE(parsed(req(9, 42, "null", 3)).state);
}
TEST_CASE("tube request: any bad value refuses the whole request", "[tube]") {
    const auto st = [](const std::string& rows) {
        return R"({"engine":"tube","look":"tv","mask":null,"rows":)" + rows + "}"; };
    const std::vector<std::string> bad = {
        "", "not json", "[]",
        R"({"version":2,"seq":7,"client":"a1b2c3d4e5f60718","base_rev":1,"degauss":0,"state":null})",
        req(0, 1, "null"), R"({"version":1,"seq":"7","client":"a1b2c3d4e5f60718","base_rev":1,"degauss":0,"state":null})",
        req(7, 1, "null", 0, ""), req(7, 1, "null", 0, "XYZ!"), req(7, 1, "5"),
        req(7, 1, R"({"engine":"plasma","look":"tv","mask":null,"rows":{}})"),
        req(7, 1, R"({"engine":"tube","look":"vhs","mask":null,"rows":{}})"),
        req(7, 1, R"({"engine":"tube","look":"tv","mask":"Slot","rows":{}})"),
        req(7, 1, st("[]")), req(7, 1, st(R"({"wobble":5})")), req(7, 1, st(R"({"hum":101})")),
        req(7, 1, st(R"({"hum":-1})")), req(7, 1, st(R"({"hum":"20"})")), req(7, 1, st(R"({"hum":20.5})")),
        req(7, 1, st(R"({"hum":true})")), std::string(app::kTubeRequestMaxBytes + 1, ' ')};
    for (const auto& text : bad) {
        INFO(text.substr(0, 120));
        const auto p = app::parse_tube_request(text);
        REQUIRE_FALSE(p.request); REQUIRE_FALSE(p.error.empty());
    }
}
TEST_CASE("tube request: the first file after boot is adopted, never applied", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; live.rev = 42;
    const auto r = parsed(req(7, 42, kTv, 2));
    app::adopt_tube_request(&r, live);
    REQUIRE(live.adopted); REQUIRE(live.handled_seq == 7); REQUIRE(live.refused_seq == 7);
    REQUIRE(live.web_degauss_seen == 2); REQUIRE(live.degauss_requests == 0);
    REQUIRE(d.tube == tube::TubeSetting{}); REQUIRE(live.rev == 42);
    REQUIRE(app::handle_tube_request(r, d, live, true).outcome == app::TubeRequestOutcome::Stale);
}
TEST_CASE("tube request: applied when built on the current state", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; live.rev = 42;
    app::adopt_tube_request(nullptr, live);
    const auto r = parsed(req(7, 42, kTv, 2));
    const auto res = app::handle_tube_request(r, d, live, true);
    REQUIRE(res.outcome == app::TubeRequestOutcome::Applied); REQUIRE(res.degauss);
    REQUIRE(d.crt_engine == app::CrtEngine::Tube); REQUIRE(d.enhanced_crt_enabled);
    REQUIRE(d.tube == r.state->setting);
    REQUIRE(live.rev == 43); REQUIRE(live.last_writer == kClient);
    REQUIRE(live.handled_seq == 7); REQUIRE(live.degauss_requests == 1);
    REQUIRE(app::handle_tube_request(r, d, live, true).outcome == app::TubeRequestOutcome::Stale);
}
TEST_CASE("tube request: a request built before another writer's change is refused", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; live.rev = 42;
    app::adopt_tube_request(nullptr, live);
    tube::set_row(d.tube, tube::Effect::hum, 35); live.note_change(app::kTubeWriterTv);  // the TV, rev 43
    const auto res = app::handle_tube_request(parsed(req(7, 42, kTv, 1)), d, live, true);
    REQUIRE(res.outcome == app::TubeRequestOutcome::Refused);
    REQUIRE(d.tube.rows[tube::index_of(tube::Effect::hum)] == 35);           // the TV's value stands
    REQUIRE(live.refused_seq == 7); REQUIRE(live.handled_seq == 7); REQUIRE(live.rev == 43);
    REQUIRE(res.degauss);                                                    // a press still counts
    // the same page again, after another page wrote: refused too
    app::AppState::DisplaySettings d2; app::TubeLiveState l2; l2.rev = 42; app::adopt_tube_request(nullptr, l2);
    REQUIRE(app::handle_tube_request(parsed(req(1, 42, kTv)), d2, l2, true).outcome == app::TubeRequestOutcome::Applied);
    REQUIRE(app::handle_tube_request(parsed(req(2, 42, kTv)), d2, l2, true).outcome
            == app::TubeRequestOutcome::Unchanged);                         // own later change: accepted
    REQUIRE(app::handle_tube_request(parsed(req(3, 42, kTv, 0, "ffffffffffffffff")), d2, l2, true).outcome
            == app::TubeRequestOutcome::Refused);
}
TEST_CASE("tube request: a closed gate refuses every state", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; live.rev = 42; app::adopt_tube_request(nullptr, live);
    REQUIRE(app::handle_tube_request(parsed(req(1, 42, kTv)), d, live, false).outcome == app::TubeRequestOutcome::Refused);
    REQUIRE(app::handle_tube_request(parsed(req(2, 42, R"({"engine":"enhanced","look":"pvm","mask":null,"rows":{}})")),
                                     d, live, false).outcome == app::TubeRequestOutcome::Refused);
    REQUIRE(d.crt_engine == app::CrtEngine::Classic);
}
```

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** with jsoncpp (`Json::CharReaderBuilder`, `strictRoot`). Check `isUInt64()` for counters and `isInt()` and not `isBool()`/`isDouble()`-only for row values (jsoncpp's `isInt()` is true for `20.0`: also require the token to be an integer, e.g. `v.isIntegral()`).
- [ ] **Step 4: Run** `./build-tube2/test_app_unit "[tube]"` — pass.
- [ ] **Step 5: Commit** — `tube: the web's request, parsed and decided without losing anyone's change`

---

### Task 5: The kiosk publishes the tube state

**Files:**
- Modify: `magic_dingus_box_cpp/src/app/status_writer.cpp` (`write_now`, before the `platform_policy_override` block)
- Test: `magic_dingus_box_cpp/tests/phone_remote/test_status_writer.cpp`

**Interfaces:**
- Consumes: Task 3 (`tube_live`, `crt_engine_name`); `tube::resolve`, `moved_keys`, `effect_key`, `mask_name`, `look_name`; `state.platform_profile.tube_crt_engine`; `state.display_settings.mode`.
- Produces: `root["tube"]` in `kiosk_status.json`.
  - Gate open: `{"gate": true, "engine", "effective_engine", "crt_native": bool, "look", "mask" (resolved), "custom", "moved": [keys, then "mask"], "values": {44 keys: int}, "rev", "handled_seq", "refused_seq", "degauss_requests"}` (counters as `Json::UInt64`).
  - Gate closed: exactly `{"gate": false, "engine", "effective_engine"}`.
  - No new write cadence: the existing change detection writes a changed body at once.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("status_writer publishes the tube state", "[remote][status][tube]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_status_tube.json"; fs::remove(tmp);
    app::AppState state;
    state.platform_profile.tube_crt_engine = true;
    state.display_settings.mode = app::DisplayMode::MODERN_TV;
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    tube::select_look(state.display_settings.tube, tube::Look::tv);
    tube::set_row(state.display_settings.tube, tube::Effect::hum, 20);
    tube::set_mask(state.display_settings.tube, tube::Mask::dots);
    state.tube_live.rev = (1ull << 40) + 7; state.tube_live.handled_seq = 12;
    state.tube_live.refused_seq = 11; state.tube_live.degauss_requests = 3;
    app::StatusWriter w(tmp.string()); w.write_now(state); w.flush();
    Json::Value root; std::ifstream(tmp) >> root;
    const Json::Value& t = root["tube"];
    REQUIRE(t["gate"].asBool());
    REQUIRE(t["engine"].asString() == "tube"); REQUIRE(t["effective_engine"].asString() == "tube");
    REQUIRE_FALSE(t["crt_native"].asBool());
    REQUIRE(t["look"].asString() == "tv"); REQUIRE(t["mask"].asString() == "dots");
    REQUIRE(t["custom"].asBool());
    REQUIRE(t["moved"].size() == 2);
    REQUIRE(t["moved"][0].asString() == "hum"); REQUIRE(t["moved"][1].asString() == "mask");
    REQUIRE(t["values"].size() == 44);
    REQUIRE(t["values"]["hum"].asInt() == 20); REQUIRE(t["values"]["brightness"].asInt() == 58);
    REQUIRE(t["rev"].asUInt64() == (1ull << 40) + 7);
    REQUIRE(t["handled_seq"].asUInt64() == 12); REQUIRE(t["refused_seq"].asUInt64() == 11);
    REQUIRE(t["degauss_requests"].asUInt64() == 3);
}
TEST_CASE("status_writer: a closed gate publishes the engine only", "[remote][status][tube]") {
    // same set-up with tube_crt_engine = false and engine Tube
    // REQUIRE(t.getMemberNames() == std::vector<std::string>{"effective_engine", "engine", "gate"});
    // REQUIRE(t["effective_engine"].asString() == "enhanced"); REQUIRE(t["engine"].asString() == "tube");
}
TEST_CASE("status_writer: a tube change is written at once", "[remote][status][tube]") {
    // StatusWriter(path, sink) with a sink counting writes; write_now; set_row(hum, 25); write_now;
    // flush → 2 writes, the second body has "hum":25 (no 2 s heartbeat wait).
}
```
(Write the two sketched cases out in full in the same shape as the first.)

- [ ] **Step 2: Run, see them fail** — `cmake --build build-tube2 --target test_phone_remote_unit -j8 && ./build-tube2/test_phone_remote_unit "[tube]"`.
- [ ] **Step 3: Implement** a file-local `Json::Value tube_status_json(const AppState&)` in `status_writer.cpp` and one line in `write_now`.
- [ ] **Step 4: Run** `./build-tube2/test_phone_remote_unit` in full — pass.
- [ ] **Step 5: Commit** — `tube: the kiosk publishes the tube state for the web page`

---

### Task 6: The TV menu

**Files:**
- Modify: `magic_dingus_box_cpp/src/ui/settings_menu.h`, `magic_dingus_box_cpp/src/ui/settings_menu.cpp`, `magic_dingus_box_cpp/src/app/settings_input.cpp`
- Test: `magic_dingus_box_cpp/tests/ui/test_settings_menu_tube.cpp` (globbed into `test_ui_unit`)

**Interfaces:**
- Consumes: Tasks 1 and 3; `app::SettingsPersistence::save_settings`; `app::ContentSignature`; `state.platform_profile.tube_crt_engine`; `ui_test::fake_settings_saves()`.
- Produces:
  - `MenuSection`: `TOGGLE_ENHANCED_CRT` renamed `CYCLE_CRT_ENGINE`; new `CYCLE_TUBE_LOOK`, `TUBE_RESET_LOOK`, `TUBE_GROUP` (a group row, and the submenu it opens), `TUBE_GROUP_BACK`, `TUBE_VALUE`, `TUBE_MASK`, `TUBE_DEGAUSS`. Not-applicable rows use the existing `INFO` (no action).
  - `SettingsMenuManager` public: `void enter_tube_group(std::size_t group);` `void exit_tube_group();` `std::size_t tube_group_index() const;` `bool is_tube_editing() const;` `void begin_tube_edit(std::optional<tube::Effect> effect);` `void step_tube_edit(int direction);` `void end_tube_edit();`
  - Private: `std::vector<MenuItem> build_tube_group_submenu();` `tube::Edit tube_edit_;` `std::size_t tube_group_index_ = 0;` `int tube_group_return_index_ = 0;`. `settings_menu.h` includes `../tube/tube_edit.h` (standard library only).

Rows, decided here:
- **Display submenu.** The engine row (label `crt_engine_label`, sublabel `crt_engine_sublabel`, action `set_crt_engine(next_crt_engine(...))`) stays where the toggle is. After it, by `display_crt_rows`: either the seven existing rows, unchanged; or `tube_look_label` / `tube_look_sublabel` (action `select_look(next_look(...))`), `"Reset to look"` / `tube_reset_sublabel` (action `reset_to_look`; no save when nothing was moved), the ten `kGroups` rows (`tube_group_label` / `tube_group_sublabel`, action `enter_tube_group(i)`), and `"Degauss"` / `"demagnetize the tube"` (action `++tube_live.degauss_requests`; no save, no toast: nothing is drawn until Plan 3). Then `"Back"`.
- Every action that changes the engine or the tube calls `app_state_->tube_live.note_change(app::kTubeWriterTv)`, then `rebuild_current_submenu()`, then `save_settings` — as the Menu Style and Guide Mode rows do.
- **A group submenu** (`current_submenu_ == TUBE_GROUP`): when `has_mask_row`, the MASK row first; then each row of the group; then `"Back"` (`TUBE_GROUP_BACK`, action `exit_tube_group()`). A value row: label `tube_value_label(e, resolve(tube).effects[i], editing this row)`, sublabel `row_info(e).hint1`, action `begin_tube_edit(e)`. The MASK row: `tube_mask_label`, sublabel `mask_choice(mask).hint1`, action `begin_tube_edit(std::nullopt)`. A row whose `tube_row_kind` is `NotApplicable` (display mode `CRT_NATIVE`): `tube_na_label(label)`, sublabel `kTubeNaSublabel`, section `INFO`, no action.
- `enter_tube_group` remembers the cursor in `tube_group_return_index_`; `exit_tube_group` returns to `DISPLAY` with the cursor back on that row (clamped). `enter_submenu(TUBE_GROUP)` builds `build_tube_group_submenu()`, so `rebuild_current_submenu()` works inside a group.
- **Edit mode:** `begin_tube_edit` → `tube::begin_edit` + rebuild. `step_tube_edit(direction)` → `tube::edit_turn`; when it returns true, `note_change(tv)` and rebuild; it never saves. `end_tube_edit` → `tube::end_edit`; rebuild; `save_settings` once when it returned true. `close()`, `force_close()`, `exit_submenu()`, `exit_tube_group()` and a fresh `enter_submenu` call `end_tube_edit()` first.
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
              if (ev.pressed) settings_menu.end_tube_edit();   // QUIT leaves edit mode, not the menu
              break;
          default: break;
      }
      return true;
  }
  ```
  A short BTN4 press still closes the menu (through `toggle()` → `close()`, which saves the edit); BTN4 held + turn is still master volume (`handle_menu_button` runs first).

- [ ] **Step 1: Write the failing test** (`test_settings_menu_tube.cpp`; helpers: `open_gate(state)` sets `platform_profile.tube_crt_engine = true` and `mode = MODERN_TV`; `row(sm, prefix)` returns the first submenu item whose label starts with `prefix`, or `nullptr`; `press(sm, prefix)` COPIES that row's `action` and then calls the copy — an action rebuilds `submenu_items_`, which would destroy a `std::function` called in place; `labels(sm)`)

```cpp
TEST_CASE("Tube menu: the engine cycles through Tube on a Pi 5 and the rows follow", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; open_gate(state);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    REQUIRE(row(sm, "CRT Engine: Classic"));
    CHECK(row(sm, "CRT Engine: ")->sublabel == "Classic / Enhanced / Tube");
    CHECK(row(sm, "Scanlines: ")); CHECK_FALSE(row(sm, "Look: "));
    press(sm, "CRT Engine: "); press(sm, "CRT Engine: ");
    CHECK(state.display_settings.crt_engine == app::CrtEngine::Tube);
    CHECK(ui_test::fake_settings_saves() == 2);
    CHECK(state.tube_live.last_writer == "tv");
    CHECK_FALSE(row(sm, "Scanlines: "));
    const auto all = labels(sm);
    const auto at = std::find(all.begin(), all.end(), "CRT Engine: Tube");
    CHECK(std::vector<std::string>(at + 1, all.end()) == std::vector<std::string>{
        "Look: PVM", "Reset to look", "Picture", "Scanlines", "Mask", "Light", "Glass", "Signal",
        "Supply", "Character", "Sync", "Colour purity", "Degauss", "Back"});
}
TEST_CASE("Tube menu: a Pi 4B is offered Classic and Enhanced only", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{};             // gate closed
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);   // a Pi 5 backup
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
TEST_CASE("Tube menu: picking a look clears what was moved; Reset keeps the look", "[settings_menu][tube]") {
    // engine Tube, gate open, set_row(hum, 20): row "Look: PVM (1 moved)"; its action → "Look: Monitor",
    // moved_keys empty, one save; set_row(noise, 30); "Reset to look" action → look still monitor,
    // nothing moved, sublabel "nothing moved", one more save; Reset again → no further save.
}
TEST_CASE("Tube menu: a group opens its rows and Back returns to it", "[settings_menu][tube]") {
    // engine Tube; row "Supply" action → get_current_submenu() == TUBE_GROUP, labels ==
    // {"FLICKER: 0", "HUM BAR: 0", "BRIGHTNESS LIMIT: 20", "BREATHING: 15", "Back"};
    // the HUM BAR sublabel == "soft at low · pronounced at 100"; "Back" action →
    // get_current_submenu() == DISPLAY and get_current_highlighted_label() == "Supply".
}
TEST_CASE("Tube menu: edit mode steps the value and saves once on leaving", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; open_gate(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Supply");
    press(sm, "HUM BAR");
    REQUIRE(sm.is_tube_editing());
    CHECK(sm.get_submenu_items()[1].label == "HUM BAR: < 0 >");
    const auto saves = ui_test::fake_settings_saves(); const auto rev = state.tube_live.rev;
    sm.step_tube_edit(+1); sm.step_tube_edit(+1);
    CHECK(sm.get_submenu_items()[1].label == "HUM BAR: < 10 >");
    CHECK(ui_test::fake_settings_saves() == saves);           // not per step
    CHECK(state.tube_live.rev == rev + 2);
    sm.end_tube_edit();
    CHECK(ui_test::fake_settings_saves() == saves + 1);
    CHECK(sm.get_submenu_items()[1].label == "HUM BAR: 10");
    sm.end_tube_edit(); CHECK(ui_test::fake_settings_saves() == saves + 1);
}
TEST_CASE("Tube menu: closing the menu mid-edit saves the edit", "[settings_menu][tube]") {
    // begin edit on HUM BAR, step +1, sm.close() → !is_tube_editing(), one save.
}
TEST_CASE("Tube menu: CRT Native shows the tube's own rows as n/a", "[settings_menu][tube]") {
    // mode CRT_NATIVE, engine Tube: "Scanlines" group rows == {"SCANLINES: n/a", "SCANLINE SIZE: 10",
    // "BEAM SWELL: 80", "TRUE RASTER: n/a", "Back"}; the n/a rows have section INFO, no action and
    // sublabel kTubeNaSublabel; "Mask" group == {"MASK: n/a", "MASK STRENGTH: n/a", "MASK SIZE: n/a", "Back"}.
}
TEST_CASE("Tube menu: Degauss records a request and saves nothing", "[settings_menu][tube]") {
    // engine Tube: row "Degauss" action → tube_live.degauss_requests == 1, saves unchanged.
}
TEST_CASE("Tube menu: edit mode stays static and redraws on each step", "[settings_menu][redraw][tube]") {
    // open() and finish the slide (as test_settings_menu_redraw.cpp does); enter Supply; s0 = signature;
    // begin edit on HUM BAR → signature != s0, is_static_for_redraw() still true; s1 = signature;
    // step +1 → signature != s1.
}
```
(Write the sketched cases out in full in the shape of the first.)

- [ ] **Step 2: Run, see it fail** — `cmake --build build-tube2 --target test_ui_unit -j8`.
- [ ] **Step 3: Implement** the rows, the group submenu, edit mode and the signature in `settings_menu.{h,cpp}`; the input block in `settings_input.cpp`. Update the existing Display tests only if they relied on the old engine label.
- [ ] **Step 4: Run** `./build-tube2/test_ui_unit` and `./build-tube2/test_app_unit` in full — pass. `settings_input.cpp` is checked by CI's `kiosk-build` job.
- [ ] **Step 5: Commit** — `tube: the TV menu — engine, look, reset, ten groups, edit mode, degauss`

---

### Task 7: The kiosk polls, applies and saves

**Files:**
- Create: `magic_dingus_box_cpp/src/app/tube_live.h`, `magic_dingus_box_cpp/src/app/tube_live.cpp`
- Modify: `magic_dingus_box_cpp/CMakeLists.txt` (`APP_SOURCES` and `test_app_unit`: `src/app/tube_live.cpp`), `magic_dingus_box_cpp/src/main.cpp`
- Test: `magic_dingus_box_cpp/tests/app/test_tube_live.cpp`

**Interfaces:**
- Consumes: Task 4 (`parse_tube_request`, `adopt_tube_request`, `handle_tube_request`, `TubeRequestOutcome`, `kTubeRequestMaxBytes`); Task 3 (`TubeLiveState`, `kTubeWriterRestore`); `SettingsPersistence::save_settings`, `set_file_sink_for_tests`, `flush_pending_saves`; `app::ContentSignature`; `config::get_data_path()`.
- Produces (namespace `app`):
  - `class TubeRequestPoller` —
    `using Clock = std::chrono::steady_clock;`
    `static constexpr std::chrono::milliseconds kPollPeriod{100};` `static constexpr std::chrono::seconds kSaveQuiet{1};`
    `explicit TubeRequestPoller(std::string path);`
    `bool poll(AppState& state, Clock::time_point now);` — every call: saves when a debounced save is due. At most every `kPollPeriod`: `std::filesystem` (last_write_time, file_size) of the file; only on the first look or when that pair changed, read it (≤ `kTubeRequestMaxBytes`; larger is a parse error) and parse. First look since boot → `adopt_tube_request` (with `nullptr` when absent or unparsable). Later → `handle_tube_request(..., state.platform_profile.tube_crt_engine)`; `Applied` arms the save at `now + kSaveQuiet` (re-armed by each later apply). A parse error or a refusal logs one `LOG_WARN` per file version. Returns true when the engine or the tube setting changed.
    `void flush(AppState& state);` — saves now if a save is armed.
  - `std::uint64_t random_tube_rev_start();` — `std::random_device`, uniform in [2^40, 2^41).
  - `void add_tube_redraw_signature(ContentSignature&, const AppState&);` — adds `crt_engine`, `tube_live.rev`, `tube_live.degauss_requests`.
- main.cpp wiring (decided here):
  - After `AppState state` is set up: `state.tube_live.rev = app::random_tube_rev_start();` and `app::TubeRequestPoller tube_requests(config::get_data_path() + "/tube_request.json");`
  - In the loop, right after `controller.poll_text_input_queue(state);`:
    `if (tube_requests.poll(state, std::chrono::steady_clock::now()) && settings_menu.is_active()) settings_menu.rebuild_current_submenu();`
  - Redraw signature block: `app::add_tube_redraw_signature(sig, state);` next to the `display_settings.mode` line.
  - Settings-restore block (`settings_reload_request`): keep `crt_engine` and `tube` before `reload_from_disk`; when either differs after it, `state.tube_live.note_change(app::kTubeWriterRestore);` (a request built before the restore is then refused).
  - Shutdown, before `flush_pending_saves`: `tube_requests.flush(state);`

- [ ] **Step 1: Write the failing test** (helpers: a fresh temp dir per test; `write_request(path, seq, base, hum)` writes `<path>.tmp` then `std::filesystem::rename`s it, with `kClient` and state `{"engine":"tube","look":"pvm","mask":null,"rows":{"hum":<hum>}}`; `SaveCounter` installs a counting `set_file_sink_for_tests` sink and clears it in its destructor, `count()` calls `flush_pending_saves(1s)` first; `T0` a fixed time point)

```cpp
TEST_CASE("tube live: no file at start, then a request applies", "[tube]") {
    app::AppState state; state.platform_profile.tube_crt_engine = true; state.tube_live.rev = 42;
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0)); REQUIRE(state.tube_live.adopted);
    write_request(path, 1, 42, 20);
    REQUIRE_FALSE(poller.poll(state, T0 + 50ms));          // inside the 100 ms period
    REQUIRE(poller.poll(state, T0 + 100ms));
    REQUIRE(state.display_settings.tube.rows[tube::index_of(tube::Effect::hum)] == 20);
    REQUIRE(state.display_settings.crt_engine == app::CrtEngine::Tube);
}
TEST_CASE("tube live: a file left from before a restart is adopted, not applied", "[tube]") {
    write_request(path, 9, 42, 20);
    app::AppState state; state.platform_profile.tube_crt_engine = true; state.tube_live.rev = 42;
    app::TubeRequestPoller poller(path.string());
    REQUIRE_FALSE(poller.poll(state, T0));
    REQUIRE(state.display_settings.tube == tube::TubeSetting{});
    REQUIRE(state.tube_live.handled_seq == 9); REQUIRE(state.tube_live.refused_seq == 9);
    REQUIRE_FALSE(poller.poll(state, T0 + 1s));            // same file: not read again
}
TEST_CASE("tube live: only the newest of several writes between polls lands", "[tube]") {
    // adopt with no file at T0; write seq 1 (hum 10), seq 2 (hum 15), seq 3 (hum 20); poll at T0+100ms →
    // hum 20, handled_seq 3; write seq 2 (hum 5) again → poll at T0+200ms returns false, hum still 20.
}
TEST_CASE("tube live: saves once, a second after the last change", "[tube]") {
    SaveCounter saves;
    // adopt at T0; write seq 1, poll T0+100ms (applied); write seq 2, poll T0+600ms (applied);
    // poll T0+1500ms → saves.count() == 0; poll T0+1600ms → 1; poll T0+3s → still 1.
}
TEST_CASE("tube live: flush saves a pending change at once", "[tube]") {
    // apply one request, flush(state) → saves.count() == 1; poll after T0+5s → still 1.
}
TEST_CASE("tube live: an unreadable file is skipped and the next good one applies", "[tube]") {
    // adopt at T0; write "not json" (tmp + rename); poll T0+100ms → false, nothing changed;
    // write_request seq 1; poll T0+200ms → applied.
}
TEST_CASE("tube live: the revision starts where no old page can name it", "[tube]") {
    for (int i = 0; i < 100; ++i) { const auto r = app::random_tube_rev_start();
        REQUIRE(r >= (1ull << 40)); REQUIRE(r < (1ull << 41)); }
}
TEST_CASE("tube live: any tube change moves the redraw signature", "[tube]") {
    app::AppState state; app::ContentSignature a, b, c;
    app::add_tube_redraw_signature(a, state);
    state.tube_live.note_change(app::kTubeWriterTv); app::add_tube_redraw_signature(b, state);
    ++state.tube_live.degauss_requests;            app::add_tube_redraw_signature(c, state);
    REQUIRE(a.value() != b.value()); REQUIRE(b.value() != c.value());
}
```
(Write the sketched cases out in full.)

- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** `tube_live.{h,cpp}` and the five main.cpp edits.
- [ ] **Step 4: Run** `./build-tube2/test_app_unit` in full — pass.
- [ ] **Step 5: Commit** — `tube: the kiosk applies web requests at once and saves when they stop`

---

### Task 8: The web route module

**Files:**
- Create: `magic_dingus_box/web/admin_display.py`
- Modify: `magic_dingus_box/web/admin.py` (import `admin_display` in both import forms; `admin_display.register(app, ctx)` after `admin_media_browser_ops.register(app, ctx)`, before `ctx.sock = sock`)
- Test: `magic_dingus_box/web/tests/test_admin_display.py`

**Interfaces:**
- Consumes: Task 2 (`static/tube_rows.json`); Task 4 (the request file's shape); Task 5 (the status `tube` block); `ctx.data_dir`, `ctx.require_csrf`; `admin_common.error_response`, `success_response`.
- Produces (`admin_display.py`, no import of `admin`):
  - Constants: `REQUEST_NAME = "tube_request.json"`, `STATUS_NAME = "kiosk_status.json"`, `STATUS_MAX_AGE_S = 10.0`, `CLIENT_RE = re.compile(r"^[0-9a-f]{8,32}$")`, `MAX_JS_INT = 2**53 - 1`.
  - `load_rows() -> dict` — `static/tube_rows.json`, read once in `register`.
  - `kiosk_tube(data_dir: Path, now: float | None = None) -> tuple[dict | None, str | None]` — `(tube block, None)` or `(None, reason)`; reason `"kiosk_not_running"` (status missing, unparsable, or `ts` older than `STATUS_MAX_AGE_S`), `"kiosk_too_old"` (no `tube` key), `"needs_pi5"` (`gate` false).
  - `validate_request(body, rows: dict) -> tuple[dict | None, str | None]` — `(clean, None)` or `(None, field)`: body a dict; `engine` in the row file's engine keys; `look` a look key; `mask` `None` or a mask key; `rows` a dict whose keys are row keys and values `int` (not `bool`) within `row_min..row_max`; `base_rev` int `0..MAX_JS_INT`; `client` matches `CLIENT_RE`; `degauss` absent or `bool`.
  - `register(app, ctx) -> None` with:
    - `GET /admin/display/tube` → `success_response({"available": True, "rows": ROWS, "state": tube})`, or `{"available": False, "reason": reason, "rows": ROWS}`; with `?state_only=1` the `rows` key is left out. Always 200.
    - `POST /admin/display/tube` (`@require_csrf`) → in order: `validate_request` → 400 `VALIDATION_ERROR` with `details={"field": field}`; `kiosk_tube` → 409 `TUBE_UNAVAILABLE` with `details={"reason": reason}`; then under a module `threading.Lock`: read the previous request file (missing or unparsable = none); if it exists with `seq > tube["handled_seq"]` and a different `client` → 409 `TUBE_BUSY` (header `Retry-After: 1`); else `seq = max(prev seq, handled_seq) + 1`, `degauss = prev degauss + (1 if body degauss else 0)`, write `{"version": 1, "seq", "client", "base_rev", "degauss", "state": {"engine", "look", "mask", "rows"}}` with `tempfile.mkstemp(dir=data_dir, prefix=".tube_request.", suffix=".tmp")` + `os.replace` (no fsync: transient; a write failure removes the temp file and returns 500 `INTERNAL_ERROR`) → `success_response({"seq": seq})`.
  - Never touches `settings.json`, `crt_engine` or `enhanced_crt_enabled`.

- [ ] **Step 1: Write the failing tests** (helpers: `write_status(data_dir, tube, ts=None)` writes `{"schema": 1, "ts": ..., "tube": tube}`; `OPEN` a gate-open block with `rev` 42, `handled_seq` 0, `refused_seq` 0 and 44 `values`; `BODY = {"engine": "tube", "look": "tv", "mask": None, "rows": {"hum": 20}, "base_rev": 42, "client": "a1b2c3d4e5f60718"}`; `post(client, body)` posts JSON to `/admin/display/tube`; `request_file(data_dir)` loads it)

```python
def test_get_returns_rows_and_state(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    data = client.get("/admin/display/tube").get_json()["data"]
    assert data["available"] is True
    assert len(data["rows"]["rows"]) == 44 and data["state"] == OPEN
    data = client.get("/admin/display/tube?state_only=1").get_json()["data"]
    assert "rows" not in data and data["state"]["rev"] == 42

def test_get_says_kiosk_not_running_when_status_is_stale(client, temp_data_dir):
    assert client.get("/admin/display/tube").get_json()["data"]["reason"] == "kiosk_not_running"
    write_status(temp_data_dir, OPEN, ts=time.time() - 30)
    data = client.get("/admin/display/tube").get_json()["data"]
    assert data["available"] is False and data["reason"] == "kiosk_not_running"

def test_get_says_kiosk_too_old_without_a_tube_block(client, temp_data_dir): ...  # status without "tube"

def test_closed_gate_is_not_available_and_writes_nothing(client, temp_data_dir):
    write_status(temp_data_dir, {"gate": False, "engine": "enhanced", "effective_engine": "enhanced"})
    assert client.get("/admin/display/tube").get_json()["data"]["reason"] == "needs_pi5"
    r = post(client, BODY)
    assert r.status_code == 409 and r.get_json()["error"]["details"]["reason"] == "needs_pi5"
    assert not (temp_data_dir / "tube_request.json").exists()

def test_post_writes_the_whole_state_with_increasing_seq(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    assert post(client, BODY).get_json()["data"]["seq"] == 1
    assert request_file(temp_data_dir) == {"version": 1, "seq": 1, "client": BODY["client"],
        "base_rev": 42, "degauss": 0,
        "state": {"engine": "tube", "look": "tv", "mask": None, "rows": {"hum": 20}}}
    post(client, {**BODY, "degauss": True}); post(client, BODY)
    assert request_file(temp_data_dir)["seq"] == 3 and request_file(temp_data_dir)["degauss"] == 1

def test_seq_continues_past_what_the_kiosk_handled(client, temp_data_dir):
    write_status(temp_data_dir, {**OPEN, "handled_seq": 57})
    assert post(client, BODY).get_json()["data"]["seq"] == 58

@pytest.mark.parametrize("field,value", [("engine", "plasma"), ("look", "vhs"), ("mask", "Slot"),
    ("rows", []), ("rows", {"wobble": 5}), ("rows", {"hum": 101}), ("rows", {"hum": "20"}),
    ("rows", {"hum": True}), ("base_rev", -1), ("client", "XYZ"), ("degauss", "yes")])
def test_post_refuses_a_bad_value_and_writes_nothing(client, temp_data_dir, field, value):
    write_status(temp_data_dir, OPEN)
    r = post(client, {**BODY, field: value})
    assert r.status_code == 400 and r.get_json()["error"]["details"]["field"] == field
    assert not (temp_data_dir / "tube_request.json").exists()

def test_post_does_not_overwrite_another_pages_unhandled_request(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)                         # handled_seq 0
    post(client, BODY)                                        # seq 1, unhandled
    r = post(client, {**BODY, "client": "ffffffffffffffff"})
    assert r.status_code == 409 and r.get_json()["error"]["code"] == "TUBE_BUSY"
    assert post(client, {**BODY, "rows": {"hum": 25}}).status_code == 200   # the same page may
    write_status(temp_data_dir, {**OPEN, "handled_seq": 2})
    assert post(client, {**BODY, "client": "ffffffffffffffff"}).status_code == 200

def test_rapid_posts_leave_the_last_state_with_increasing_seq(client, temp_data_dir):
    write_status(temp_data_dir, OPEN)
    seqs = [post(client, {**BODY, "rows": {"hum": v}}).get_json()["data"]["seq"] for v in range(0, 101, 5)]
    assert seqs == list(range(1, 22))
    assert request_file(temp_data_dir)["state"]["rows"] == {"hum": 100}
    assert not list(temp_data_dir.glob(".tube_request.*"))

def test_post_requires_csrf(client, temp_data_dir, monkeypatch):
    monkeypatch.delenv("MAGIC_DISABLE_CSRF")
    write_status(temp_data_dir, OPEN)
    r = client.post("/admin/display/tube", json=BODY)
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
- Modify: `magic_dingus_box/web/static/index.html` (top tab, bottom-nav item, `<section id="crtlook" class="tab-content">`, `<script src="/static/manager_display.js?v=1"></script>` after `vpn_settings.js`; bump `manager.js` to `?v=127`), `magic_dingus_box/web/static/manager.js` (`switchTab`)
- Test: `magic_dingus_box/web/tests/test_manager_display_js.py`

**Interfaces:**
- Consumes: Task 8's GET/POST and error codes; Task 5's status block; `apiGet`, `apiPost`, `currentDevice.url`, `escapeHtml` from `manager.js`.
- Produces (top-level `function` declarations and nothing that runs while the file loads — no DOM access, no call; `function` rather than `const`/`let` so the node test finds them on its `vm` context):
  - `tubeWant(kiosk, pending) -> {engine, look, mask, rows}` — start from the kiosk: `mask` is `kiosk.mask` when `kiosk.moved` includes `"mask"`, else `null`; `rows` holds `kiosk.values[k]` for each other moved key. Then apply `pending` in order: `{op:'row', key, value}` sets a row; `{op:'mask', value}`; `{op:'engine', value}`; `{op:'look', value}` sets the look and clears rows and mask; `{op:'reset'}` clears rows and mask.
  - `tubeOnStatus(pending, kiosk, lastSentSeq) -> {pending, resend}` — `kiosk.handled_seq < lastSentSeq` (or nothing sent): unchanged, `resend: false`. `kiosk.refused_seq === lastSentSeq`: every op's `sentIn` becomes `null`, `resend: pending.length > 0`. Otherwise drop the ops with `sentIn !== null && sentIn <= lastSentSeq`.
  - `makeLatestSender(post) -> {send(build), busy()}` — one POST in flight; a `send` while busy only remembers that another is due, and when the POST settles, the newest `build()` is sent; a `TUBE_BUSY` error retries the newest after 250 ms. `post(body)` resolves to `{seq}`.
  - `startCrtLookTab()` / `stopCrtLookTab()` — called by `switchTab` (`tabId === 'crtlook'` starts; any other tab stops). Polls `GET .../admin/display/tube?state_only=1` every 1 s while the tab is shown and the document is visible (the first load fetches the rows too).
- Behaviour (styling is the builder's, within the Content Manager's look):
  - Not available: `needs_pi5` → "The CRT look needs a Pi 5. This box keeps its Classic and Enhanced effects, set on the TV under Settings → Display." `kiosk_not_running` → "Waiting for the TV…" (controls disabled, pending changes kept). `kiosk_too_old` → "Update this box to tune the CRT look."
  - Available: engine picker (three words), look picker (words plus `hint`), Reset, Degauss (enabled only when `effective_engine` is `tube`; the button's note says the effect appears once the Tube renderer lands), then ten groups from `rows.groups` with one control per row: a 0–100 range input stepping by 1 (the row file's `step` is the TV's step, not a limit here) and its number, or a select of the five mask words; both hint lines; a "moved" mark when the key is in `tubeWant`'s result as moved. A row that is `crt_native_neutral` is disabled with "a real tube draws this itself" when `state.crt_native`. When the engine is not Tube, a note says the sliders apply when CRT Engine is Tube.
  - Every control change pushes an op with `sentIn: null` and calls `sender.send(() => …)`, which builds `{...tubeWant(kiosk, pending), base_rev: kiosk.rev, client, degauss?}`, POSTs, and on success sets `sentIn` on the ops it carried and `lastSentSeq`. `client` is 16 random hex digits per page load (`crypto.getRandomValues`).
  - Each poll result runs `tubeOnStatus`; on `resend` it sends again. Every control shows `tubeWant(kiosk, pending)`, except a range input the pointer is holding, which is never repainted.

- [ ] **Step 1: Write the failing test** (`test_manager_display_js.py`; skipped when `shutil.which("node")` is None, as `test_escape_js.py` does; runs one node script through `subprocess.run(["node", "-e", script])` that loads the file with `vm.runInNewContext(src, ctx)` where `ctx = {setTimeout, clearTimeout, console}` — which also proves it touches nothing while loading)

```python
def test_index_loads_manager_display_last_with_a_version():
    html = INDEX.read_text()
    scripts = re.findall(r'<script src="/static/([\w.]+)\?v=\d+"></script>', html)
    assert scripts[-1] == "manager_display.js" and scripts[-2] == "vpn_settings.js"

def test_want_is_the_kiosk_state_with_pending_changes_on_top(): ...
    # kiosk {engine:'tube', look:'tv', mask:'slot', moved:['hum'], values:{hum:20,...}};
    # pending [{op:'row',key:'noise',value:30}] → {engine:'tube', look:'tv', mask:null, rows:{hum:20, noise:30}};
    # then {op:'look', value:'pvm'} → rows {}, mask null.

def test_refusal_resends_the_pages_own_changes_on_the_fresh_state(): ...
    # pending [{op:'row',key:'hum',value:25,sentIn:7}], kiosk {handled_seq:7, refused_seq:7, ...}
    # → resend true, sentIn null; tubeWant on the new kiosk (TV moved noise to 40) keeps noise 40 and hum 25.

def test_confirmation_drops_only_what_was_sent(): ...
    # ops sentIn 7 and null, kiosk {handled_seq:7, refused_seq:0} → only the null op stays, resend false;
    # handled_seq 6 → nothing dropped.

def test_latest_sender_keeps_one_request_in_flight_and_ends_on_the_newest(): ...
    # post resolves after a tick and records bodies; send(()=>1), send(()=>2), send(()=>3) synchronously
    # → after settling, bodies == [1, 3].
```

- [ ] **Step 2: Run, see it fail** — `python -m pytest magic_dingus_box/web/tests/test_manager_display_js.py -q`.
- [ ] **Step 3: Implement** `manager_display.js`, the tab markup and the `switchTab` branch.
- [ ] **Step 4: Run** the test, then the web suite — pass. By hand: `python3 -m magic_dingus_box.web.wsgi` against a data dir holding a hand-written fresh `kiosk_status.json`; drag a slider, watch `tube_request.json`'s `seq` climb and its last state match the slider.
- [ ] **Step 5: Commit** — `tube: the Content Manager's CRT Look tab`

---

### Task 10: The request file's place on a box, and the notes

**Files:**
- Modify: `magic_dingus_box_cpp/scripts/deploy_cpp.sh` (next to `--exclude 'data/seek_request.json'`), `magic_dingus_box_cpp/scripts/update.sh` (the three `seek_request.json` exclude lines and the comment block near line 1965), `scripts/golden_image/first_boot.sh` (`step_wipe_phone_remote`'s data-dir list and its build/data list), `scripts/golden_image/prepare_for_cloning.sh` (the `SECRET_PATHS` loop), `tests/ota_rehearsal/box/manifest.py` (`RUNTIME`), `tests/ota_rehearsal/hw_helpers.py` (`APPDATA_EXCLUDE`), `OTA_UPDATE_GUARANTEES.md` (the transient-files row), `CLAUDE.md`, `CHANGELOG.md`

**Interfaces:**
- Consumes: the file name `data/tube_request.json` (Tasks 4, 7, 8).
- Produces: the file is excluded from every deploy/OTA rsync and every OTA rehearsal comparison like `seek_request.json`, and removed by `first_boot.sh` (both lists) and by `prepare_for_cloning.sh` before an image is taken. It holds no secret; a clone that kept it would only make its first request look like a restart.

- [ ] **Step 1: Make the edits above.** Add `tube_request.json` beside `seek_request.json` in each list, in each file's own syntax.
- [ ] **Step 2: Check them** — `git grep -n "seek_request.json" -- magic_dingus_box_cpp/scripts scripts/golden_image tests/ota_rehearsal OTA_UPDATE_GUARANTEES.md` and `git grep -n "tube_request.json"` over the same paths list the same places; `bats tests/local/` passes.
- [ ] **Step 3: Notes.** `CLAUDE.md`: under `ui/` → `settings_menu`, the Display submenu's Tube rows, group submenus and edit mode (SELECT enters, knob/D-pad steps, SELECT/QUIT leaves and saves once; decisions in `tube/tube_edit.h` and `app/tube_menu_logic.h`); under `app/`, `tube_request` (the file's shape, adopt/stale/refuse rules) and `tube_live` (100 ms poll, no write on the loop, save 1 s after the last change, the random revision start); in the web module map, `admin_display` (GET/POST `/admin/display/tube`, `TUBE_BUSY`, never writes settings); in the static load order, `manager_display.js` last; under Phone Remote, a short "CRT look live tuning" paragraph naming `tube_request.json` next to `text_input_queue.jsonl`; that `static/tube_rows.json` is generated (regenerate with `MDB_WRITE_TUBE_ROWS_JSON=1 ./build/test_app_unit "[tube_rows_json]"`) and checked from C++ and Python; that Degauss is only counted (`tube_live.degauss_requests`) until Plan 3; update the `tube/` entry's "no production caller yet" sentence (the menu now calls `effective_crt_engine`). `CHANGELOG.md` `## [Unreleased]` → `### Added`: one line for the Pi 5 CRT look controls on the TV and in the Content Manager.
- [ ] **Step 4: Full verification.** Both Mac configurations, every suite, then the web suite:
  ```bash
  ctest --test-dir build-tube2 --output-on-failure
  cmake -S magic_dingus_box_cpp -B build-tube2 -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER=ON && cmake --build build-tube2 -j8 && ctest --test-dir build-tube2 --output-on-failure
  python -m pytest magic_dingus_box/web/tests/ -q
  ```
  Expected: all pass (9 C++ suites with Media Browser ON, 8 OFF). Then `rm -rf build-tube2`. On hardware (owner's Pi 5, and one Pi 4B): the Pi 5 menu reaches every row and edit mode steps with the knob and the D-pad; a slider on a phone moves the number on the TV inside a second; the Pi 4B shows Classic/Enhanced only and the tab's Pi 5 message. Check one Tube sublabel in a `screenshot_request` capture: if the menu font lacks `·`, say so in the PR (the hints are Sound Factory's words).
- [ ] **Step 5: Commit** — `tube: the request file's place on a box, and the notes`

---

## Spec coverage

| Requirement | Task |
|---|---|
| §6 one row table, JSON generated and checked in, a test on each side | 2 |
| §6 engine row Classic/Enhanced/Tube, Tube only with the gate open | 3, 6 |
| §6 Look row, "(n moved)", one press restores a known look | 3, 6 |
| §6 Reset to look | 3, 6 |
| §6 ten groups with row counts, value + hint sublabel | 1, 3, 6 |
| §6 edit mode: SELECT enters, knob/D-pad steps, SELECT/back leaves, saved on leaving | 3, 6 |
| §6 steps of 5, size rows 10, MASK cycles five words | 3 (core `step_row`) |
| §6 seven Classic/Enhanced rows only for those engines, Tube rows only for Tube | 3, 6 |
| §6 Content Manager CRT Look tab: engine, look, Reset, ten groups of sliders, hints, moved mark, any whole number | 9 |
| §6 gate closed: the tab explains a Pi 5 is needed | 8, 9 |
| §6 `admin_display.py` via `register`, `manager_display.js` loaded last with `?v=`, CSRF | 8, 9 |
| §5 CRT Native rows shown as not applicable | 3, 6, 9 |
| §7 Read: status block with engine, look, mask, 44 values, moved, gate, last request seq | 5 |
| §7 page polls about once a second, never repaints a held slider | 9 |
| §7 Write: POST validates against the row table, whole state, increasing seq, atomic file | 8 |
| §7 Apply: about ten checks a second, a stat not a write, in memory at once | 7 |
| §7 save once changes stop for about a second (existing async save) | 7 |
| §7 no slider movement lost while dragging | 4, 7, 8, 9 |
| §7 an applied request is never applied again after a restart | 4, 7 |
| §7 a TV edit shows up on the page | 5, 9 |
| §7 a kiosk older than this feature ignores the file | (no old kiosk reads it; the web refuses without a status `tube` block) 8 |
| §7 file excluded from deploy/OTA rsyncs, deleted on clones | 10 |
| §8 Degauss: a menu row and a web button that only record the request | 4, 6, 9 |
| §2 a Pi 5 backup's Tube on a Pi 4B shows as Enhanced, not rewritten | 3, 6 |
