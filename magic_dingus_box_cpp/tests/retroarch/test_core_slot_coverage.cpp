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
// This test makes that class of omission fail in CI instead. The mapping
// snapshot golden cannot: it pins whatever is emitted, gaps included.
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

bool is_excepted(ControllerType pad, const std::string& core, const mdb_cov::Need& need) {
    for (const auto& e : mdb_cov::kExceptions()) {
        if (e.pad != pad || core.find(e.core) == std::string::npos) continue;
        for (Slot s : need) {
            if (s == e.slot) return true;
        }
    }
    return false;
}

std::string describe(const mdb_cov::Need& need) {
    std::string out;
    for (Slot s : need) {
        if (!out.empty()) out += " or ";
        out += mdb_cov::slot_name(s);
    }
    return out;
}

}  // namespace

TEST_CASE("every slot a core reads is reachable on both pads",
          "[coverage][mapping]") {
    for (const auto& pad : pads()) {
        for (const auto& cs : mdb_cov::kCoreSlots()) {
            const std::string core = std::string(cs.core) + "_libretro";
            const auto r = mdb_cov::resolve(pad.type, core);
            for (const auto& need : cs.needs) {
                if (is_excepted(pad.type, core, need)) continue;
                bool met = false;
                for (Slot s : need) {
                    if (!mdb_cov::slot_value(r, s).empty()) met = true;
                }
                INFO(pad.label << " on " << core << ": nothing reaches RetroPad "
                     << describe(need));
                CHECK(met);
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
        const auto r = mdb_cov::resolve(e.pad, core);
        const std::string v = mdb_cov::slot_value(r, e.slot);
        INFO("Exception claims " << mdb_cov::slot_name(e.slot) << " on " << core
             << " is unreachable, but it resolves to \"" << v << "\". "
             << "Either the exception is stale and should be deleted, or the "
             << "binding is wrong. Reason on file: " << e.reason);
        CHECK(v.empty());
    }
}

// The guard in slot_value() is what keeps a ghost default from counting as
// coverage. Pin that it bites: on a table that does not clear unassigned
// buttons, an unassigned face-button slot still EMITS a token, and
// slot_value() must still report it unreachable.
TEST_CASE("a ghost default does not count as a binding", "[coverage][mapping]") {
    // NES on the N64 pad never assigns RetroPad L (no NES control uses it),
    // and its table keeps the legacy struct defaults.
    const auto r = mdb_cov::resolve(ControllerType::N64_ADAPTER, "nestopia_libretro");
    REQUIRE_FALSE(r.sem.clear_unassigned_buttons);
    REQUIRE_FALSE(r.sem.l.has_value());
    REQUIRE_FALSE(r.m.l_btn.empty());
    CHECK(mdb_cov::slot_value(r, Slot::L).empty());
}
