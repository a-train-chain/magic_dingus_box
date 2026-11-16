// app/tube_menu_logic.h - the Settings menu's Tube decisions (which rows show,
// the engine cycle, every row's words) and the two gates on offering Tube at all.
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include "app/app_state.h"
#include "app/tube_menu_logic.h"
#include "platform/platform_profile.h"
#include "tube/tube_budget.h"
#include "tube/tube_frame_class.h"
#include "tube/tube_groups.h"
#include "tube/tube_setting.h"
using app::CrtEngine;

TEST_CASE("tube menu logic: Tube is offered only on a Pi 5 with the renderer ready", "[tube]") {
    // The two gates, each closed in turn. The renderer flag is a test seam since the
    // switch-on (every build sets it from kTubeRendererReady, now true); closing it here
    // pins what "not ready" still means to the status file and the web.
    app::AppState s;
    s.tube_renderer_ready = false;
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
namespace {
// Whether a default AppState (the renderer flag as every build sets it) on
// `profile` is offered Tube, and the reason it is not.
bool offered_on(const platform::PlatformProfile& profile) {
    app::AppState s;
    s.platform_profile = profile;
    return app::tube_offered(s);
}
std::string reason_on(const platform::PlatformProfile& profile) {
    app::AppState s;
    s.platform_profile = profile;
    return std::string(app::tube_not_offered_reason(s));
}
}  // namespace

TEST_CASE("tube menu logic: the renderer is switched on in every build", "[tube]") {
    REQUIRE(app::kTubeRendererReady);
    REQUIRE(app::AppState{}.tube_renderer_ready);
}

TEST_CASE("tube menu logic: a default AppState is offered Tube on a Pi 5 only", "[tube]") {
    using platform::PiModel;
    REQUIRE(offered_on(platform::profile_for(PiModel::Pi5)));
    REQUIRE(reason_on(platform::profile_for(PiModel::Pi5)).empty());

    REQUIRE_FALSE(offered_on(platform::profile_for(PiModel::Pi4)));
    REQUIRE(reason_on(platform::profile_for(PiModel::Pi4)) == "needs_pi5");

    // A dev machine: whatever its profile says (today: hide nothing).
    REQUIRE(offered_on(platform::profile_for(PiModel::Unknown)) ==
            platform::profile_for(PiModel::Unknown).tube_crt_engine);
}

TEST_CASE("tube menu logic: a Pi 5 rehearsing Pi 4B policies is not offered Tube", "[tube]") {
    platform::PlatformProfile real = platform::profile_for(platform::PiModel::Pi5);
    real.model_string = "Raspberry Pi 5 Model B Rev 1.1";
    app::AppState s;
    s.platform_profile = platform::apply_policy_override(real, "pi4");
    REQUIRE(s.platform_profile.model == platform::PiModel::Pi5);   // the hardware stays real
    REQUIRE_FALSE(app::tube_offered(s));
    REQUIRE(app::tube_not_offered_reason(s) == "needs_pi5");
    s.display_settings.set_crt_engine(CrtEngine::Tube);              // a saved Tube draws Enhanced
    REQUIRE(app::effective_crt_engine(s.display_settings.crt_engine, app::tube_offered(s)) ==
            CrtEngine::Enhanced);
    // Any other value is ignored: the real Pi 5 is offered Tube.
    REQUIRE(offered_on(platform::apply_policy_override(real, "pi5")));
}

TEST_CASE("tube menu logic: switching on changes no box's engine; a saved Tube draws only on a Pi 5",
          "[tube]") {
    using platform::PiModel;
    for (const PiModel model : {PiModel::Pi5, PiModel::Pi4}) {
        app::AppState s;
        s.platform_profile = platform::profile_for(model);
        const bool offered = app::tube_offered(s);
        // No saved engine: a default box draws Classic, never Tube.
        REQUIRE(app::effective_crt_engine(s.display_settings.crt_engine, offered) == CrtEngine::Classic);
        s.display_settings.set_crt_engine(CrtEngine::Classic);
        REQUIRE(app::effective_crt_engine(s.display_settings.crt_engine, offered) == CrtEngine::Classic);
        s.display_settings.set_crt_engine(CrtEngine::Enhanced);
        REQUIRE(app::effective_crt_engine(s.display_settings.crt_engine, offered) == CrtEngine::Enhanced);
        s.display_settings.set_crt_engine(CrtEngine::Tube);
        REQUIRE(app::effective_crt_engine(s.display_settings.crt_engine, offered) ==
                (model == PiModel::Pi5 ? CrtEngine::Tube : CrtEngine::Enhanced));
    }
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

namespace {
void drawing_tube(app::AppState& state, tube::Look look, tube::Picture picture) {
    state.platform_profile.tube_crt_engine = true;
    state.tube_renderer_ready = true;
    state.display_settings.set_crt_engine(CrtEngine::Tube);
    tube::select_look(state.display_settings.tube, look);
    state.tube_render.drawing = true;
    state.tube_render.picture = picture;
}
}  // namespace

TEST_CASE("tube menu logic: what is not drawn is the renderer's plan for the real picture", "[tube]") {
    const tube::Picture ref{1440.0, 1080.0, false};
    const tube::Picture film{1440.0, 1110.0, false};   // TV fits there only without both
    app::AppState tv, trinitron, tv_film;
    drawing_tube(tv, tube::Look::tv, ref);
    drawing_tube(trinitron, tube::Look::trinitron, ref);
    drawing_tube(tv_film, tube::Look::tv, film);
    CHECK(app::tube_not_drawn(tv) == tube::NotDrawn{true, false});
    CHECK(app::tube_not_drawn(tv) ==
          tube::plan_passes_and_drops(tube::resolve(tv.display_settings.tube), ref).not_drawn);
    CHECK_FALSE(app::tube_not_drawn(trinitron).any());
    CHECK(app::tube_not_drawn(tv_film) == tube::NotDrawn{true, true});
    // Not drawing (the Media Browser, another engine, before the first frame): nothing.
    tv.tube_render.drawing = false;
    CHECK_FALSE(app::tube_not_drawn(tv).any());
    CHECK_FALSE(app::tube_not_drawn(app::AppState{}).any());
}

TEST_CASE("tube menu logic: a row whose effect is not drawn says so", "[tube]") {
    using tube::Effect;
    const tube::NotDrawn none{}, ag{true, false}, both{true, true};
    CHECK_FALSE(app::tube_row_not_drawn(Effect::afterglow, none));
    CHECK(app::tube_row_not_drawn(Effect::afterglow, ag));
    CHECK_FALSE(app::tube_row_not_drawn(Effect::glow, ag));
    CHECK_FALSE(app::tube_row_not_drawn(Effect::glowSize, ag));
    CHECK(app::tube_row_not_drawn(Effect::glow, both));
    CHECK(app::tube_row_not_drawn(Effect::glowSize, both));   // it sizes a glow that is not drawn
    for (const Effect e : tube::kEffects)
        if (e != Effect::afterglow && e != Effect::glow && e != Effect::glowSize) CHECK_FALSE(app::tube_row_not_drawn(e, both));
    // The sublabel: the hint, the rate tag, then the note.
    CHECK(app::tube_value_sublabel(Effect::afterglow, none) == "the phosphor lingers \xC2\xB7 30 fps");
    CHECK(app::tube_value_sublabel(Effect::afterglow, ag) == "the phosphor lingers \xC2\xB7 30 fps \xC2\xB7 not drawn");
    CHECK(app::tube_value_sublabel(Effect::glowSize, both) == "how far the light scatters \xC2\xB7 not drawn");
    CHECK(app::tube_value_sublabel(Effect::brightness, both) == "the picture's overall light");
}

TEST_CASE("tube menu logic: which engine a full-screen film gets, and which draws the screen now", "[tube]") {
    app::AppState state;
    drawing_tube(state, tube::Look::tv, tube::Picture{1440.0, 1080.0, false});
    state.display_settings.menu_style = app::MenuStyle::ChannelGuide;
    // Before the main loop has run, the film picture is unknown.
    CHECK_FALSE(app::tube_film_engine(state).has_value());
    state.tube_render.film_picture = app::full_screen_film_picture(1920, 1080, app::DisplayMode::MODERN_TV);
    CHECK(app::tube_film_engine(state) == CrtEngine::Enhanced);   // TV is too heavy for a film
    tube::select_look(state.display_settings.tube, tube::Look::pvm);
    CHECK(app::tube_film_engine(state) == CrtEngine::Tube);       // PVM fits without afterglow
    tube::select_look(state.display_settings.tube, tube::Look::everything);
    // CRT Native at 720p: a film is the 1280x720 framebuffer, never yields.
    state.tube_render.film_picture = app::full_screen_film_picture(1280, 720, app::DisplayMode::CRT_NATIVE);
    CHECK(app::tube_film_engine(state) == CrtEngine::Tube);
    state.tube_render.film_picture = app::full_screen_film_picture(1920, 1080, app::DisplayMode::MODERN_TV);
    CHECK(app::tube_film_engine(state) == CrtEngine::Enhanced);
    // Not applicable (no note anywhere): the List style shows no film full-screen ...
    state.display_settings.menu_style = app::MenuStyle::List;
    CHECK_FALSE(app::tube_film_engine(state).has_value());
    state.display_settings.menu_style = app::MenuStyle::ChannelGuide;
    // ... and with Classic or Enhanced in effect the tube draws nothing at all.
    state.display_settings.set_crt_engine(CrtEngine::Enhanced);
    CHECK_FALSE(app::tube_film_engine(state).has_value());
    state.display_settings.set_crt_engine(CrtEngine::Classic);
    CHECK_FALSE(app::tube_film_engine(state).has_value());
    state.display_settings.set_crt_engine(CrtEngine::Tube);
    state.tube_renderer_ready = false;   // not offered: a saved Tube draws as Enhanced
    CHECK_FALSE(app::tube_film_engine(state).has_value());
    state.tube_renderer_ready = true;
    // The engine drawing the picture on screen right now.
    CHECK(app::tube_engine_on_screen(state) == CrtEngine::Tube);
    state.tube_render.yielding = true;
    CHECK(app::tube_engine_on_screen(state) == CrtEngine::Enhanced);
    state.tube_render.yielding = false;
    state.tube_render.unavailable = "build_hung";   // the guard tripped: Enhanced draws
    CHECK(app::tube_engine_on_screen(state) == CrtEngine::Enhanced);
    state.tube_render.unavailable = "build_failed";
    CHECK(app::tube_engine_on_screen(state) == CrtEngine::Enhanced);
    state.tube_render.unavailable.clear();
    state.tube_renderer_ready = false;
    CHECK(app::tube_engine_on_screen(state) == CrtEngine::Enhanced);
}

TEST_CASE("tube menu logic: the picture a full-screen film gets is the whole framebuffer", "[tube]") {
    const tube::Picture modern = app::full_screen_film_picture(1920, 1080, app::DisplayMode::MODERN_TV);
    CHECK(modern.w == 1920.0);
    CHECK(modern.h == 1080.0);
    CHECK_FALSE(modern.crt_native);
    const tube::Picture modern720 = app::full_screen_film_picture(1280, 720, app::DisplayMode::MODERN_TV);
    CHECK(modern720.w == 1280.0);
    const tube::Picture native = app::full_screen_film_picture(1280, 720, app::DisplayMode::CRT_NATIVE);
    CHECK(native.w == 1280.0);
    CHECK(native.crt_native);
}

TEST_CASE("tube menu logic: the Look row says when films use Enhanced", "[tube]") {
    using namespace tube;
    TubeSetting s; select_look(s, Look::tv);
    CHECK(app::tube_look_label(s, false, CrtEngine::Enhanced) == "Look: TV \xC2\xB7 30 fps \xC2\xB7 films: Enhanced");
    CHECK(app::tube_look_label(s, false, CrtEngine::Tube) == "Look: TV \xC2\xB7 30 fps");
    CHECK(app::tube_look_label(s, false, std::nullopt) == "Look: TV \xC2\xB7 30 fps");
    CHECK(app::tube_look_label(s, false) == "Look: TV \xC2\xB7 30 fps");
}

namespace {
// A setting with `n` rows moved off the look (each to a value one away from the look's).
tube::TubeSetting moved_rows(tube::Look look, int n) {
    tube::TubeSetting s;
    tube::select_look(s, look);
    const tube::LookValues& base = tube::look_values(look);
    for (int i = 0; i < n; ++i) {
        const int v = base.effects[static_cast<std::size_t>(i)];
        tube::set_row(s, tube::kEffects[static_cast<std::size_t>(i)], v < 100 ? v + 1 : v - 1);
    }
    return s;
}
}  // namespace

TEST_CASE("tube menu logic: the Look label always fits its row", "[tube]") {
    for (const tube::Look look : tube::kLooks)
        for (const int moved : {0, 3, 12, 45})
            for (const bool note : {false, true})
                for (const bool crt_native : {false, true}) {
                    const tube::TubeSetting s = moved_rows(look, moved == 45 ? 44 : moved);
                    const std::string label = app::tube_look_label(
                        s, crt_native, note ? std::optional<CrtEngine>(CrtEngine::Enhanced) : std::nullopt);
                    INFO(label);
                    CHECK(app::utf8_length(label) <= app::look_label_max_chars(crt_native));
                    CHECK(label.rfind("Look: " + std::string(tube::look_choice(look).label), 0) == 0);
                }
    // The ladder, step by step (Modern TV, 47 characters).
    using tube::Look;
    const auto films = std::optional<CrtEngine>(CrtEngine::Enhanced);
    CHECK(app::tube_look_label(moved_rows(Look::tv, 3), false, films) ==
          "Look: TV (3 moved) \xC2\xB7 30 fps \xC2\xB7 films: Enhanced");          // 45: fits whole
    CHECK(app::tube_look_label(moved_rows(Look::arcade, 3), false, films) ==
          "Look: Arcade (+3) \xC2\xB7 30 fps \xC2\xB7 films: Enhanced");          // "(n moved)" -> "(+n)"
    CHECK(app::tube_look_label(moved_rows(Look::everything, 12), false, films) ==
          "Look: Everything \xC2\xB7 30 fps \xC2\xB7 films: Enhanced");           // then dropped
    // CRT Native (30 characters).
    CHECK(app::tube_look_label(moved_rows(Look::tv, 0), true, std::nullopt) == "Look: TV \xC2\xB7 30 fps");
    CHECK(app::tube_look_label(moved_rows(Look::everything, 12), true, std::nullopt) ==
          "Look: Everything \xC2\xB7 30 fps");   // the overflow that existed before
    // (A films note in CRT Native exists only in a 1080p framebuffer before the restart.)
    CHECK(app::tube_look_label(moved_rows(Look::tv, 0), true, films) ==
          "Look: TV \xC2\xB7 films: Enh.");          // abbreviated (31 with the rate), then the rate goes
    CHECK(app::tube_look_label(moved_rows(Look::everything, 0), true, films) ==
          "Look: Everything \xC2\xB7 films: Enh.");   // exactly 30
}

TEST_CASE("tube menu logic: Degauss counts only while the tube draws the screen", "[tube]") {
    app::AppState state;
    drawing_tube(state, tube::Look::tv, tube::Picture{1440.0, 1080.0, false});
    CHECK(app::tube_degauss_live(state));
    state.tube_render.yielding = true;   // a film handed to Enhanced
    CHECK_FALSE(app::tube_degauss_live(state));
    state.tube_render.yielding = false;
    state.tube_render.unavailable = "build_hung";
    CHECK_FALSE(app::tube_degauss_live(state));
}
