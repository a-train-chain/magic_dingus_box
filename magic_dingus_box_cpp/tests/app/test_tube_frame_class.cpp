#include <catch2/catch_test_macros.hpp>

#include "tube/tube_frame_class.h"

using namespace tube;

namespace {
Resolved neutral_rates() {  // every row that can force 30 fps, at neutral
    Resolved r = resolve(TubeSetting{});
    for (auto e : kEffects)
        if (forces_30fps(e, LeanSet{true, false, false, false, false, false, false, false}))
            r.effects[index_of(e)] = rate_neutral(e);
    return r;
}
constexpr LeanSet kAllLean{true, true, true, true, true, true, true, true};
}  // namespace

TEST_CASE("tube frame class: with every ingredient lean, the seven always-rich rows force 30 fps", "[tube]") {
    int n = 0;
    for (auto e : kEffects) n += forces_30fps(e, kAllLean);
    REQUIRE(n == 7);
    for (auto e : kAlwaysRich) REQUIRE(forces_30fps(e, kAllLean));
    REQUIRE_FALSE(forces_30fps(Effect::glowSize, kAllLean));
    REQUIRE_FALSE(forces_30fps(Effect::brightness, kAllLean));
    for (auto e : kAlwaysRich) {
        Resolved r = neutral_rates();
        r.effects[index_of(e)] = (e == Effect::sharpness) ? 55 : 5;
        INFO(effect_key(e));
        REQUIRE(setting_frame_class(r, false, kAllLean) == FrameClass::Rich30);
    }
    REQUIRE(setting_frame_class(neutral_rates(), false, kAllLean) == FrameClass::Lean60);
}

TEST_CASE("tube frame class: a row whose ingredient is not lean forces 30 fps", "[tube]") {
    LeanSet no_bend = kAllLean;
    no_bend.bend = false;
    REQUIRE(forces_30fps(Effect::curvature, no_bend));
    REQUIRE_FALSE(forces_30fps(Effect::curvature, kAllLean));
    Resolved r = neutral_rates();
    r.effects[index_of(Effect::curvature)] = 70;
    REQUIRE(setting_frame_class(r, false, kAllLean) == FrameClass::Lean60);
    REQUIRE(setting_frame_class(r, false, no_bend) == FrameClass::Rich30);
    LeanSet no_motion = kAllLean;
    no_motion.motion = false;
    for (auto e : {Effect::noise, Effect::flicker, Effect::hum, Effect::rollBand, Effect::purity})
        REQUIRE(forces_30fps(e, no_motion));
}

TEST_CASE("tube frame class: no usable lean set makes everything rich", "[tube]") {
    LeanSet none = kAllLean;
    none.usable = false;
    REQUIRE(setting_frame_class(neutral_rates(), false, none) == FrameClass::Rich30);
}

TEST_CASE("tube frame class: CRT Native's neutral rows do not force 30 fps", "[tube]") {
    Resolved r = neutral_rates();
    r.effects[index_of(Effect::raster)] = 60;
    REQUIRE(setting_frame_class(r, false, kAllLean) == FrameClass::Rich30);
    REQUIRE(setting_frame_class(r, true, kAllLean) == FrameClass::Lean60);
}

TEST_CASE("tube frame class: a picture larger than the reference is rich", "[tube]") {
    const Resolved r = neutral_rates();
    REQUIRE(frame_class(r, Picture{1440.0, 1080.0, false}, kAllLean) == FrameClass::Lean60);
    REQUIRE(frame_class(r, Picture{960.0, 720.0, false}, kAllLean) == FrameClass::Lean60);
    REQUIRE(frame_class(r, Picture{1920.0, 1080.0, false}, kAllLean) == FrameClass::Rich30);
    REQUIRE(frames_per_second(FrameClass::Rich30) == 30);
    REQUIRE(frames_per_second(FrameClass::Lean60) == 60);
}

TEST_CASE("tube frame class: the shipped lean set forces 30 fps on exactly fourteen rows", "[tube]") {
    const std::array<Effect, 14> expected{
        Effect::glow,        Effect::afterglow, Effect::sharpness, Effect::convergence, Effect::chromaBleed,
        Effect::raster,      Effect::colorDrift, Effect::noise,    Effect::flicker,     Effect::hum,
        Effect::rollBand,    Effect::purity,    Effect::abl,       Effect::breathing};
    int n = 0;
    for (auto e : kEffects) n += forces_30fps(e);
    REQUIRE(n == 14);
    for (auto e : expected) {
        INFO(effect_key(e));
        REQUIRE(forces_30fps(e));
    }
}

TEST_CASE("tube budget: the constants are Task 1b's", "[tube]") {
    // Pinned to the DECISION block; update with it, in the same commit.
    REQUIRE(kLeanBudgetMs == 11.8);
    REQUIRE(kRichBudgetMs == 23.1);
    REQUIRE(kLeanSet.usable);
    REQUIRE(kLeanSet.cheap_beam);
    REQUIRE_FALSE(kLeanSet.motion);
    REQUIRE_FALSE(kLeanSet.probe);
}
