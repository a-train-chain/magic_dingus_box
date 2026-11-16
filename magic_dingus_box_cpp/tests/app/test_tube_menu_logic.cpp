// app/tube_menu_logic.h - the Settings menu's Tube decisions (which rows show,
// the engine cycle, every row's words) and the two gates on offering Tube at all.
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include "app/app_state.h"
#include "app/tube_menu_logic.h"
#include "tube/tube_budget.h"
#include "tube/tube_frame_class.h"
#include "tube/tube_groups.h"
#include "tube/tube_setting.h"
using app::CrtEngine;

TEST_CASE("tube menu logic: Tube is offered only on a Pi 5 with the renderer ready", "[tube]") {
    app::AppState s;
    s.tube_renderer_ready = false;   // the production default (pinned by "a production build is not renderer-ready")
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
    REQUIRE(app::tube_look_label(s, false) == "Look: TV · 30 fps");
    REQUIRE(app::tube_look_sublabel() == "PVM · Monitor · Trinitron · TV · Arcade · Everything");
    REQUIRE(app::tube_reset_sublabel(s) == "nothing moved");
    set_row(s, Effect::hum, 20); set_mask(s, Mask::dots);
    REQUIRE(app::tube_look_label(s, false) == "Look: TV (2 moved) · 30 fps");
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
TEST_CASE("tube menu logic: a production build is not renderer-ready", "[tube]") {
#ifndef MDB_TUBE_PREVIEW
    REQUIRE_FALSE(app::kTubeRendererReady);
    app::AppState s; s.platform_profile.tube_crt_engine = true;
    s.display_settings.set_crt_engine(app::CrtEngine::Tube);
    REQUIRE(app::effective_crt_engine(s.display_settings.crt_engine, app::tube_offered(s)) == app::CrtEngine::Enhanced);
#endif
}
TEST_CASE("tube menu logic: the Look row says its rate; forcing rows are tagged", "[tube]") {
    tube::TubeSetting s; for (auto e : tube::kAlwaysRich) tube::set_row(s, e, tube::rate_neutral(e));
    const bool lean = tube::setting_frame_class(tube::resolve(s), false) == tube::FrameClass::Lean60;
    REQUIRE(app::tube_look_label(s, false).find(lean ? " · 60 fps" : " · 30 fps") != std::string::npos);
    tube::set_row(s, tube::Effect::glow, 10);
    REQUIRE(app::tube_look_label(s, false).find(" · 30 fps") != std::string::npos);
    REQUIRE(app::tube_rate_tag(tube::Effect::glow) == "30 fps");
    REQUIRE(app::tube_rate_tag(tube::Effect::brightness).empty());
}
