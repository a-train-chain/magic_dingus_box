# Controller Coverage Invariant Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make it impossible to ship a pad × core mapping where a console control the core reads is unreachable, and fill the four gaps that already exist.

**Architecture:** A table declares, per core, which RetroPad slots that core actually reads. A table-driven test resolves `get_mapping(pad, core)` for both shipped pads and asserts every declared slot resolves to a non-empty bind token — and, in the other direction, that every declared exception really is unreachable, so the allow-list cannot rot into a list of things that used to be broken. Then four one-line fills in `semantic_n64_style()` turn the test green.

**Tech Stack:** C++17, Catch2 v3, CMake. Tests live in `magic_dingus_box_cpp/tests/retroarch/` and build into the `test_retroarch_unit` target.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-29-controller-coverage-invariant-design.md`.
- **Additive only.** Do not change any existing binding. Every fill assigns a `LogicalControl` that its branch does not already use — verified conflict-free (see Task 2).
- Do not touch `semantic_ps_style()`. The PS-style table is complete as of commit `64823cb`.
- Genesis **Mode** button is deliberately out of scope — it toggles 3/6-button compatibility and no game requires pressing it during play.
- The N64 pad on PS1 **cannot** have a right stick: after `l2 = Z` all ten of its buttons are consumed. This is a declared exception, not a gap.
- Golden snapshot regeneration is **required** and must be audited line by line. Anything changing outside the intended set is a regression — stop, do not accept it.
- Build with `nice -n 19 … -j2` on the Pi. Earlier `-j3` builds starved the live kiosk until it missed its 10-second systemd watchdog and restarted twice. Never touch `/opt`, never restart a service.
- Zero warnings is a requirement, not a nicety: `-Wall -Wextra -Wpedantic`.
- Repo path contains an emoji AND `magic_dingus_box ` has a **trailing space**. Quote every path.

---

## File Structure

| File | Responsibility |
|---|---|
| `magic_dingus_box_cpp/tests/retroarch/core_slot_coverage.h` (create) | The declaration: per-core RetroPad slot sets, the accessor that reads a slot off a resolved `ControllerMapping`, and the exception list. Data + one pure helper, no test macros, so the data is reusable and readable on its own. |
| `magic_dingus_box_cpp/tests/retroarch/test_core_slot_coverage.cpp` (create) | The two-direction invariant test. Globbed into `test_retroarch_unit` automatically — no CMake change needed. |
| `magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp` (modify) | Four additive fills in `semantic_n64_style()`. |
| `magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h` (regenerate) | Expected `get_mapping()` output; the four fills change it. |
| `CHANGELOG.md` (modify) | One entry under `## [Unreleased]` → `### Fixed`. |

Confirm the glob before assuming no CMake edit is needed:
`grep -n 'tests/retroarch' magic_dingus_box_cpp/CMakeLists.txt` — `RETROARCH_TEST_SOURCES` is a `file(GLOB …)`, so a new `.cpp` in that directory is picked up on the next `cmake` configure.

---

## Task 1: Declare the slots and enforce the invariant

**Files:**
- Create: `magic_dingus_box_cpp/tests/retroarch/core_slot_coverage.h`
- Create: `magic_dingus_box_cpp/tests/retroarch/test_core_slot_coverage.cpp`

**Interfaces:**
- Consumes: `retroarch::get_mapping(ControllerType, const std::string& core)` returning `ControllerMapping`; `retroarch::ControllerType::{N64_ADAPTER, PS_STYLE_DRAGONRISE}`.
- Produces: `mdb_cov::kCoreSlots` (vector of `CoreSlots`), `mdb_cov::slot_value(const ControllerMapping&, Slot)`, `mdb_cov::kExceptions`, `mdb_cov::slot_name(Slot)`. Task 2 does not consume these; it only makes the test pass.

This task deliberately ends **RED**. The test's first run documents the four real gaps. That is the deliverable: a failing test that names each unreachable control.

- [ ] **Step 1: Read the two functions the test depends on**

Run:
```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp"
sed -n '/^struct ControllerMapping/,/^};/p' src/retroarch/controller_mapping.h
grep -n "kSnapshotCores" -A 14 tests/retroarch/mapping_snapshot_util.h
```

Confirm the exact `ControllerMapping` member names used in Step 2 (`b_btn`, `a_btn`, `y_btn`, `x_btn`, `l_btn`, `r_btn`, `l2_btn`, `r2_btn`, `l3_btn`, `r3_btn`, `select_btn`, `start_btn`, `up_btn`, `down_btn`, `left_btn`, `right_btn`, `l_x_plus`, `l_y_plus`, `r_x_plus`, `r_y_plus`). If any differs, use the real name and say so in your report.

- [ ] **Step 2: Create the declaration header**

Create `magic_dingus_box_cpp/tests/retroarch/core_slot_coverage.h`:

```cpp
#pragma once
// WHICH RETROPAD SLOTS EACH CORE ACTUALLY READS.
//
// This is the data behind the coverage invariant. It is declared per CORE, not
// per console, because the RetroPad->console mapping is a property of the core:
// mupen64plus reads RetroPad L2 to mean the N64's Z button, so "which slots
// matter" cannot be derived from the console alone.
//
// An incorrect declaration makes the invariant either toothless (slot omitted
// that the core does read) or a source of false failures (slot listed that it
// does not). Every entry below was checked against the core's own input
// handling; when in doubt, prefer omitting a slot and note it, rather than
// asserting a binding that no game can use.
#include <string>
#include <vector>

#include "retroarch/controller_mapping.h"

namespace mdb_cov {

enum class Slot {
    B, A, Y, X, L, R, L2, R2, L3, R3, SELECT, START,
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    LSTICK, RSTICK,
};

inline const char* slot_name(Slot s) {
    switch (s) {
        case Slot::B: return "B"; case Slot::A: return "A";
        case Slot::Y: return "Y"; case Slot::X: return "X";
        case Slot::L: return "L"; case Slot::R: return "R";
        case Slot::L2: return "L2"; case Slot::R2: return "R2";
        case Slot::L3: return "L3"; case Slot::R3: return "R3";
        case Slot::SELECT: return "SELECT"; case Slot::START: return "START";
        case Slot::DPAD_UP: return "DPAD_UP"; case Slot::DPAD_DOWN: return "DPAD_DOWN";
        case Slot::DPAD_LEFT: return "DPAD_LEFT"; case Slot::DPAD_RIGHT: return "DPAD_RIGHT";
        case Slot::LSTICK: return "LSTICK"; case Slot::RSTICK: return "RSTICK";
    }
    return "?";
}

// The resolved bind token for one slot, or "" when nothing reaches it.
// Sticks report their +X/+Y token: a stick with a bound axis has a non-empty
// plus token, and a stick driven by buttons instead uses the *_btn fields, so
// both forms are accepted.
inline std::string slot_value(const retroarch::ControllerMapping& m, Slot s) {
    switch (s) {
        case Slot::B: return m.b_btn;
        case Slot::A: return m.a_btn;
        case Slot::Y: return m.y_btn;
        case Slot::X: return m.x_btn;
        case Slot::L: return m.l_btn;
        case Slot::R: return m.r_btn;
        case Slot::L2: return m.l2_btn;
        case Slot::R2: return m.r2_btn;
        case Slot::L3: return m.l3_btn;
        case Slot::R3: return m.r3_btn;
        case Slot::SELECT: return m.select_btn;
        case Slot::START: return m.start_btn;
        case Slot::DPAD_UP: return m.up_btn;
        case Slot::DPAD_DOWN: return m.down_btn;
        case Slot::DPAD_LEFT: return m.left_btn;
        case Slot::DPAD_RIGHT: return m.right_btn;
        case Slot::LSTICK:
            return m.l_x_plus.empty() ? m.l_y_plus : m.l_x_plus;
        case Slot::RSTICK: {
            if (!m.r_x_plus.empty()) return m.r_x_plus;
            if (!m.r_y_plus.empty()) return m.r_y_plus;
            if (!m.r_x_plus_btn.empty()) return m.r_x_plus_btn;
            return m.r_y_plus_btn;
        }
    }
    return "";
}

struct CoreSlots {
    const char* core;              // substring the mapping dispatch matches on
    std::vector<Slot> slots;       // slots this core resolves to a real control
};

// Every core the box ships, with the slots that core reads.
//
// Deliberate omissions:
//  - Genesis MODE: toggles 3/6-button compatibility, no game needs it pressed.
//  - PC Engine / NES turbo (X, Y): a kiosk convenience, not console hardware,
//    so not required for coverage even though both tables bind them.
inline const std::vector<CoreSlots>& kCoreSlots() {
    static const std::vector<CoreSlots> v = {
        {"nestopia", {Slot::B, Slot::A, Slot::SELECT, Slot::START,
                      Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT}},
        {"snes9x",   {Slot::B, Slot::A, Slot::Y, Slot::X, Slot::L, Slot::R,
                      Slot::SELECT, Slot::START,
                      Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT}},
        // Genesis 6-button: RetroPad Y/B/A = A/B/C, X/L/R = X/Y/Z.
        {"genesis_plus_gx", {Slot::B, Slot::A, Slot::Y, Slot::X, Slot::L, Slot::R,
                             Slot::START,
                             Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT}},
        {"pcsx_rearmed", {Slot::B, Slot::A, Slot::Y, Slot::X,
                          Slot::L, Slot::R, Slot::L2, Slot::R2, Slot::L3, Slot::R3,
                          Slot::SELECT, Slot::START,
                          Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT,
                          Slot::LSTICK, Slot::RSTICK}},
        {"mednafen_pce_fast", {Slot::B, Slot::A, Slot::SELECT, Slot::START,
                               Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT}},
        {"prosystem", {Slot::B, Slot::A, Slot::SELECT, Slot::START,
                       Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT}},
        // FBNeo 6-button + coin on SELECT.
        {"fbneo", {Slot::B, Slot::A, Slot::Y, Slot::X, Slot::L, Slot::R,
                   Slot::SELECT, Slot::START,
                   Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT}},
        // N64: RetroPad L2 == the N64 Z trigger; RSTICK == the C-button cluster.
        {"mupen64plus", {Slot::B, Slot::A, Slot::L, Slot::R, Slot::L2, Slot::START,
                         Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT,
                         Slot::LSTICK, Slot::RSTICK}},
        {"parallel_n64", {Slot::B, Slot::A, Slot::L, Slot::R, Slot::L2, Slot::START,
                          Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT,
                          Slot::LSTICK, Slot::RSTICK}},
        // Dreamcast: 4 face buttons, Start, two ANALOG triggers on L2/R2,
        // one analog stick. No second stick, no stick clicks.
        {"flycast", {Slot::B, Slot::A, Slot::Y, Slot::X, Slot::L2, Slot::R2, Slot::START,
                     Slot::DPAD_UP, Slot::DPAD_DOWN, Slot::DPAD_LEFT, Slot::DPAD_RIGHT,
                     Slot::LSTICK}},
    };
    return v;
}

// A slot a pad genuinely cannot reach, with the reason. Enforced in BOTH
// directions: if one of these turns out to be bound, the test fails too, so
// the list cannot decay into a record of things that used to be broken.
struct Exception {
    retroarch::ControllerType pad;
    const char* core;
    Slot slot;
    const char* reason;
};

inline const std::vector<Exception>& kExceptions() {
    static const std::vector<Exception> v = {
        {retroarch::ControllerType::N64_ADAPTER, "pcsx_rearmed", Slot::RSTICK,
         "The N64 pad has exactly ten buttons and PS1 needs ten (4 face, L1/R1/"
         "L2/R2, Select, Start). Nothing is left for a second stick. Freeing the "
         "C-cluster for a digital right stick would cost R2, and far more PS1 "
         "games use R2 than use the right stick."},
        {retroarch::ControllerType::N64_ADAPTER, "pcsx_rearmed", Slot::L3,
         "The N64 pad has no stick clicks and no spare button to stand in for "
         "one; see the RSTICK reason."},
        {retroarch::ControllerType::N64_ADAPTER, "pcsx_rearmed", Slot::R3,
         "The N64 pad has no stick clicks and no spare button to stand in for "
         "one; see the RSTICK reason."},
    };
    return v;
}

}  // namespace mdb_cov
```

- [ ] **Step 3: Create the failing test**

Create `magic_dingus_box_cpp/tests/retroarch/test_core_slot_coverage.cpp`:

```cpp
// THE COVERAGE INVARIANT.
//
// Twenty hand-written pad x core mapping tables with nothing checking them for
// completeness produced five unreachable controls -- the PS pad could not use
// its right stick or L3/R3 on PS1, and the N64 pad could not reach three of
// Genesis's six buttons, Select on SNES or PC Engine, or L2 on PS1. In every
// case the pad had unused inputs sitting free and the table simply never
// referenced them. The PS1 gap surfaced only because someone launched a game
// and read the emitted binds by hand.
//
// This test makes that class of omission fail in CI instead.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "core_slot_coverage.h"
#include "retroarch/controller_mapping.h"

using retroarch::ControllerType;
using mdb_cov::Slot;

namespace {

struct PadUnderTest { ControllerType type; const char* label; };

const std::vector<PadUnderTest>& pads() {
    static const std::vector<PadUnderTest> v = {
        {ControllerType::N64_ADAPTER, "N64 adapter"},
        {ControllerType::PS_STYLE_DRAGONRISE, "PS-style pad"},
    };
    return v;
}

bool is_excepted(ControllerType pad, const std::string& core, Slot slot) {
    for (const auto& e : mdb_cov::kExceptions()) {
        if (e.pad == pad && core.find(e.core) != std::string::npos && e.slot == slot)
            return true;
    }
    return false;
}

}  // namespace

TEST_CASE("every slot a core reads is reachable on both pads",
          "[coverage][mapping]") {
    for (const auto& pad : pads()) {
        for (const auto& cs : mdb_cov::kCoreSlots()) {
            const std::string core = std::string(cs.core) + "_libretro";
            const auto m = retroarch::get_mapping(pad.type, core);
            for (const Slot slot : cs.slots) {
                if (is_excepted(pad.type, core, slot)) continue;
                const std::string v = mdb_cov::slot_value(m, slot);
                INFO(pad.label << " on " << core << ": RetroPad "
                     << mdb_cov::slot_name(slot)
                     << " resolves to \"" << v << "\"");
                CHECK_FALSE(v.empty());
            }
        }
    }
}

// The other direction. Without this the exception list rots: a slot that
// becomes reachable would keep its exemption forever, and nobody would notice
// that the reason attached to it had stopped being true.
TEST_CASE("every declared exception is genuinely unreachable",
          "[coverage][mapping]") {
    for (const auto& e : mdb_cov::kExceptions()) {
        const std::string core = std::string(e.core) + "_libretro";
        const auto m = retroarch::get_mapping(e.pad, core);
        const std::string v = mdb_cov::slot_value(m, e.slot);
        INFO("Exception claims " << mdb_cov::slot_name(e.slot) << " on " << core
             << " is unreachable, but it resolves to \"" << v << "\". "
             << "Either the exception is stale and should be deleted, or the "
             << "binding is wrong. Reason on file: " << e.reason);
        CHECK(v.empty());
    }
}
```

- [ ] **Step 4: Configure and build, then run the test to see it FAIL**

Run:
```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target test_retroarch_unit -j8
./build/test_retroarch_unit "[coverage]"
```

Expected: **FAIL**, with `CHECK_FALSE(v.empty())` failing for exactly these — record the actual list in your report:
- N64 adapter on `snes9x2010_libretro`: `SELECT`
- N64 adapter on `genesis_plus_gx_libretro`: `X`, `L`, `R`
- N64 adapter on `pcsx_rearmed_libretro`: `L2`
- N64 adapter on `mednafen_pce_fast_libretro`: `SELECT`

That is 7 failing assertions across 6 (core, slot) gaps. The second test case should **pass** already.

If any *other* slot fails, stop and report it — that is a gap the spec's audit missed, and it needs a decision before you fill anything.

- [ ] **Step 5: Commit the failing test**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box "
git add magic_dingus_box_cpp/tests/retroarch/core_slot_coverage.h \
        magic_dingus_box_cpp/tests/retroarch/test_core_slot_coverage.cpp
git commit -m "test(retroarch): assert every core's RetroPad slots are reachable

Declares, per core, which RetroPad slots that core actually reads, then asserts
in both directions that each resolves for both shipped pads: an unreachable
control fails, and so does a declared exception that turns out to be bound --
otherwise the allow-list decays into a record of things that used to be broken.

Lands RED on purpose. It currently names six real gaps: the N64 pad cannot reach
Select on SNES or PC Engine, L2 on PS1, or three of Genesis's six buttons, while
having unused inputs free in every case. Filling them is the next commit.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: Fill the four gaps and regenerate the snapshot

**Files:**
- Modify: `magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp` (four branches in `semantic_n64_style()`)
- Regenerate: `magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: the test from Task 1.
- Produces: no new API. Behaviour change only.

**Conflict check, already done — each fill uses an input its branch does not claim:**

| branch | already uses | free | fill |
|---|---|---|---|
| `PS1 (N64 Controller)` | A, B, C_DOWN, C_LEFT, C_UP, C_RIGHT, L, R, START | **Z** | `l2 = N64_Z` |
| `Sega Genesis` | A, B, C_DOWN, START | Z, L, R, C_LEFT, C_UP, C_RIGHT | `x = C_LEFT`, `l = N64_L`, `r = N64_R` |
| `Super Nintendo` | B, A, C_DOWN, C_LEFT, L, R, START | Z, C_UP, C_RIGHT | `select = C_UP` |
| `PC Engine` | B, A, C_LEFT, C_DOWN, START | Z, L, R, C_UP, C_RIGHT | `select = C_UP` |

- [ ] **Step 1: Fill PS1 L2**

In `src/retroarch/controller_mapping.cpp`, in `semantic_n64_style()`, find the line
`s.l = L::N64_L; s.r = L::N64_R; s.r2 = L::N64_C_RIGHT;`
inside the `s.name = "PS1 (N64 Controller)"` branch, and replace it with:

```cpp
        s.l = L::N64_L; s.r = L::N64_R; s.r2 = L::N64_C_RIGHT;
        // Z was the pad's only unused input and L2 is the only unbound PS1
        // button: trigger to trigger. With this the N64 pad covers all ten PS1
        // buttons and has nothing left, which is why the right stick and L3/R3
        // are declared exceptions rather than gaps (see core_slot_coverage.h).
        s.l2 = L::N64_Z;
```

- [ ] **Step 2: Fill Genesis X/Y/Z**

Find, inside the `s.name = "Sega Genesis"` branch:

```cpp
        s.y = L::N64_C_DOWN;  // A
        s.start = L::N64_START;
```

Replace with:

```cpp
        s.y = L::N64_C_DOWN;  // A
        // 6-button top row. RetroPad X/L/R are Genesis X/Y/Z for
        // genesis_plus_gx. Only three of the six were bound before, so
        // 6-button games were unplayable on this pad.
        //
        // C_LEFT is the most reachable C button after C_DOWN (which is
        // Genesis A), and Y/Z go on the physical shoulders. The alternative --
        // all six under the thumb on the C-cluster, like real 6-button
        // hardware -- was rejected because it puts Genesis Y on C_UP, the
        // least reachable input on the pad, and Y is medium punch in Street
        // Fighter II: pressed constantly. Frequency beats thumb-locality here.
        // Revisit with a 6-button fighter in hand if it feels wrong.
        s.x = L::N64_C_LEFT;  // X
        s.l = L::N64_L;       // Y
        s.r = L::N64_R;       // Z
        s.start = L::N64_START;
```

- [ ] **Step 3: Fill SNES Select**

Find, inside the `s.name = "Super Nintendo"` branch:

```cpp
        s.l = L::N64_L; s.r = L::N64_R; s.start = L::N64_START;
```

Replace with:

```cpp
        s.l = L::N64_L; s.r = L::N64_R; s.start = L::N64_START;
        // SNES has a Select button and this pad had no way to press it.
        // C_UP is already Select for this pad on NES, PS1 and FBNeo -- the
        // convention existed, SNES just missed it. C_UP being the least
        // reachable C button suits a control pressed between lives, not
        // during them.
        s.select = L::N64_C_UP;
```

- [ ] **Step 4: Fill PC Engine Select**

Find, inside the `s.name = "PC Engine / TurboGrafx-16"` branch:

```cpp
        s.x = L::N64_C_DOWN;  // Turbo I
        dpad();
```

Replace with:

```cpp
        s.x = L::N64_C_DOWN;  // Turbo I
        // PC Engine has a Select button; same missed convention as SNES.
        s.select = L::N64_C_UP;
        dpad();
```

- [ ] **Step 5: Run the coverage test — expect GREEN**

Run:
```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp"
cmake --build build --target test_retroarch_unit -j8
./build/test_retroarch_unit "[coverage]"
```

Expected: **PASS**, both test cases, zero failures.

- [ ] **Step 6: Snapshot the golden file, then regenerate it**

The four fills change `get_mapping()` output, so the snapshot must move. Save the old one first so the diff can be audited:

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp"
cp tests/retroarch/mapping_snapshot_golden.h /tmp/golden_before.h
./build/test_retroarch_unit "[mapping_snapshot_gen]" --success > /tmp/golden_new.txt
head -3 /tmp/golden_new.txt
```

The generator is a maintenance tool hidden behind Catch2's `[.]` tag; it asserts nothing and prints fresh entries to stdout. Paste its entries into the `kGolden` table in `tests/retroarch/mapping_snapshot_golden.h`, preserving the file's existing header comment and table syntax.

- [ ] **Step 7: Audit the snapshot diff line by line**

Run:
```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box "
git diff --numstat -- magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h
git diff -- magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h | grep '^-' | grep -v '^---'
git diff -- magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h | grep '^+' | grep -v '^+++'
```

**Expected changes, and nothing else.** Only the `N64|…` and `UNKNOWN|…` entries may move — `UNKNOWN` falls back to the N64 table, so it moves identically. No `PS|…` entry may change at all.

| entry | line | change |
|---|---|---|
| `N64\|snes9x2010_libretro` | `btn=` | Select field gains the C_UP token |
| `N64\|genesis_plus_gx_libretro` | `btn=` | X, L, R fields gain tokens |
| `N64\|pcsx_rearmed_libretro` | `btn=` | L2 field gains the Z token |
| `N64\|mednafen_pce_fast_libretro` | `btn=` | Select field gains the C_UP token |
| `UNKNOWN\|…` (same four) | `btn=` | identical, via the N64 fallback |

Anything else — a d-pad, left stick, right stick, hotkey, `pad=`, `adm=` or `extra=` field, or **any `PS|` entry** — is a regression. **Stop and report it. Do not commit a snapshot you cannot fully account for.**

State the exact changed-line count in your report and attribute every line.

- [ ] **Step 8: Full suite, both configs, zero warnings**

Run:
```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box /magic_dingus_box_cpp"
cmake --build build -j8 2>&1 | grep -icE "error:|warning:"
for t in test_retroarch_unit test_utils_unit test_ui_unit test_app_unit \
         test_platform_unit test_display_unit test_video_unit test_phone_remote_unit; do
  printf "%-24s " "$t"; ./build/$t 2>&1 | tail -3 | grep -E "All tests passed|failed"
done
cmake -S . -B build-mb -DCMAKE_BUILD_TYPE=Debug -DENABLE_MEDIA_BROWSER=ON
cmake --build build-mb -j8 2>&1 | grep -icE "error:|warning:"
./build-mb/test_media_browser_unit 2>&1 | tail -3
```

Expected: `0` for both warning counts, every suite passing. Report per-target counts.

- [ ] **Step 9: Verify on the Pi (the kiosk target macOS cannot build)**

Run:
```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box "
rsync -a --delete --exclude 'build*/' --exclude '.git' --exclude 'data/' --exclude 'docs/' \
  magic_dingus_box_cpp/ magic@magicpi5.local:~/coverage-verify/
ssh magic@magicpi5.local 'cd ~/coverage-verify && mkdir -p data && \
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_MEDIA_BROWSER=ON && \
  nice -n 19 cmake --build build --target magic_dingus_box_cpp test_retroarch_unit -j2'
ssh magic@magicpi5.local 'cd ~/coverage-verify && ./build/test_retroarch_unit "[coverage]"'
ssh magic@magicpi5.local 'rm -rf ~/coverage-verify'
```

`mkdir -p data` matters — CMake does a `file COPY` of `data/` and fails without it. Allow 2400000 ms; a timeout is not a failure. **Never touch `/opt`, never restart a service.** Report the exit code, error count, and any warning in a file you changed.

- [ ] **Step 10: CHANGELOG entry**

Add under `## [Unreleased]` → `### Fixed`, matching the surrounding discursive style (explain the failure mode, not just the change). Cover: the four gaps and that the pad had free inputs in each case; that the invariant is enforced in both directions so the exception list cannot rot; the Genesis ergonomic tradeoff and that it is revisable; and the one accepted exception with its R2-versus-right-stick reasoning.

- [ ] **Step 11: Commit**

```bash
cd "/Users/alexanderchaney/Documents/🧠 Projects/📺 MDB/magic_dingus_box "
git add magic_dingus_box_cpp/src/retroarch/controller_mapping.cpp \
        magic_dingus_box_cpp/tests/retroarch/mapping_snapshot_golden.h \
        CHANGELOG.md
git commit -m "fix(retroarch): the N64 pad can now reach every control on every core

Four console controls were unreachable on the N64 pad while the pad had unused
inputs sitting free: Select on SNES and PC Engine, L2 on PS1, and three of
Genesis's six buttons -- so 6-button Genesis games were simply unplayable on it.

None were design compromises. Three of the four follow a convention already in
the file (C_UP is Select for this pad on NES, PS1 and FBNeo) and the fourth is
trigger-to-trigger (Z -> L2, Z being the pad's last free input). All four are
additive; no existing binding changed.

Genesis X/Y/Z go to C_LEFT plus both shoulders rather than filling the C-cluster.
Thumb-locality argues for the cluster, but that puts Genesis Y on C_UP -- the
least reachable input -- and Y is medium punch in Street Fighter II. Frequency
wins. Recorded in the source as revisable with a controller in hand.

Golden snapshot regenerated and audited line by line: only the N64 and UNKNOWN
entries move (UNKNOWN falls back to the N64 table), only in btn= fields, and no
PS entry changes at all.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage.** Every spec section maps to a task: the per-core declaration and both-direction enforcement to Task 1; the five fills to Task 2 (the PS→PS1 fill already landed in `64823cb`, which the spec's Sequencing section anticipated); the accepted exception to Task 1 Step 2's `kExceptions()`; the ergonomic principles to the comments in Task 2 Steps 2–4; snapshot auditing to Task 2 Step 7; verification to Steps 8–9.

**Placeholder scan.** No TBD/TODO. Every code step carries complete code; every command has expected output.

**Type consistency.** `Slot`, `slot_name`, `slot_value`, `CoreSlots`, `kCoreSlots()`, `Exception`, `kExceptions()` are defined once in Task 1 Step 2 and used with those exact names in Step 3. `ControllerMapping` member names are confirmed against the header in Task 1 Step 1 before use, with an instruction to correct and report if any differs.

**Known open risk.** `kCoreSlots()` asserts which slots each core reads. If a declaration is wrong the invariant is either toothless or produces false failures — hence Task 1 Step 4's instruction to stop and report if any slot fails beyond the six audited gaps.
