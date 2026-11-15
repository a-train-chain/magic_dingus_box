#include <catch2/catch_test_macros.hpp>
#include <array>
#include "tube/tube_frame_class.h"
#include "tube/tube_looks.h"
#include "tube/tube_setting.h"
using namespace tube;

namespace {
struct Pinned {
    Look look;
    std::array<int, 44> effects;
    Mask mask;
};

// Typed out by hand on purpose: nothing reads this but this file, so a change to a
// look has to be made twice (header and here) and shows up in review as a decision.
// Sound Factory's kCrtLookTable (4ef74a6) with lineSize and maskSize changed for a TV.
constexpr std::array<Pinned, 6> kPinned{{
    // clang-format off
    { Look::pvm,        { { 55, 60, 58, 50, 63,   35, 10, 80,   0,   60, 10,    8, 20, 10,    70, 20, 45, 30, 15,    0,  0,   0,  0,    0,  0, 20, 15,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::grille },
    { Look::monitor,    { { 52, 55, 54, 50, 60,   15, 10, 10,   0,   38, 10,    5, 10,  0,    60, 12, 35, 20, 15,    0,  0,   0,  0,    0,  0, 15,  0,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::magentaGreen },
    { Look::trinitron,  { { 55, 58, 58, 58, 55,   35, 10, 70,  30,   60, 10,   14, 30, 20,    80, 25, 45, 40, 30,   17,  0,  50, 10,   20,  0, 30, 30,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::grille },
    { Look::tv,         { { 58, 55, 52, 45, 40,   45, 10, 60,  60,   56, 10,   22, 40, 30,   100, 30, 50, 50, 40,   33, 50, 100, 30,   30, 60, 50, 45,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::slot },
    { Look::arcade,     { { 60, 58, 60, 50, 45,   65, 10, 90, 100,   62, 40,   28, 45, 30,   100, 30, 50, 50, 30,   27, 30, 100, 20,   20, 20, 45, 40,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::dots },
    { Look::everything, { { 58, 58, 56, 48, 43,   50, 10, 70,  60,   56, 10,   25, 40, 40,   100, 32, 50, 50, 40,   33, 50, 100, 20,   40, 80, 50, 60,    0, 35, 35, 50, 0, 15, 0, 25,    35, 43, 0, 35, 25, 0, 40, 75, 0 } }, Mask::slot },
    // clang-format on
}};
}  // namespace

TEST_CASE("tube looks: the table is pinned in full", "[tube]") {
    for (std::size_t i = 0; i < kLooks.size(); ++i) {
        REQUIRE(kPinned[i].look == kLooks[i]);
        const auto& v = look_values(kLooks[i]);
        REQUIRE(v.effects == kPinned[i].effects);
        REQUIRE(v.mask == kPinned[i].mask);
    }
}
TEST_CASE("tube looks: an out-of-enum look reads as the default", "[tube]") {
    const auto& bad = look_values(static_cast<Look>(99));
    REQUIRE(bad.effects == look_values(kLookDefault).effects);
    REQUIRE(bad.mask == look_values(kLookDefault).mask);
}
TEST_CASE("tube looks: pvm as shipped", "[tube]") {
    const auto& v = look_values(Look::pvm);
    REQUIRE(v.mask == Mask::grille);
    REQUIRE(v.effects[index_of(Effect::brightness)] == 55);
    REQUIRE(v.effects[index_of(Effect::lineSize)] == 10);
    REQUIRE(v.effects[index_of(Effect::maskSize)] == 10);
    REQUIRE(v.effects[index_of(Effect::hum)] == 0);
    REQUIRE(v.effects[index_of(Effect::purityPosition)] == 0);
    REQUIRE(look_values(Look::arcade).effects[index_of(Effect::maskSize)] == 40);
    REQUIRE(look_values(Look::tv).mask == Mask::slot);
}
TEST_CASE("tube looks: every value is a legal row", "[tube]") {
    for (auto look : kLooks)
        for (int v : look_values(look).effects) REQUIRE(is_row_value(v));
}
TEST_CASE("tube looks: whole-pixel sizes", "[tube]") {
    REQUIRE(mask_pitch_row_px(0) == 2);
    REQUIRE(mask_pitch_row_px(10) == 3);
    REQUIRE(mask_pitch_row_px(100) == 12);
    REQUIRE(line_pitch_row_px(10) == 4);
    REQUIRE(whole_px(4, 1080.0) == 4);
    REQUIRE(whole_px(4, 720.0) == 3);
    REQUIRE(whole_px(3, 720.0) == 2);
    REQUIRE(whole_px(2, 480.0) == 2);   // never under two
}

TEST_CASE("tube looks: the classes Task 1b decided", "[tube]") {
    constexpr bool kPvmLean = false;  // from Task 1b's table: pvm_lean (not established without the probe; Task 15's soak decides)
    for (auto look : kLooks) {
        TubeSetting s;
        select_look(s, look);
        const bool lean = kPvmLean && (look == Look::pvm || look == Look::monitor);
        INFO(look_name(look));
        REQUIRE((setting_frame_class(resolve(s), false) == FrameClass::Lean60) == lean);
    }
    const auto& pvm = look_values(Look::pvm).effects;
    REQUIRE(pvm[index_of(Effect::sharpness)] == (kPvmLean ? 50 : 63));
    REQUIRE(pvm[index_of(Effect::glow)] == (kPvmLean ? 0 : 8));
    REQUIRE(pvm[index_of(Effect::abl)] == 20);
}
