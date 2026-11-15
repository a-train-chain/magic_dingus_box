// Settings -> Display: the CRT Engine row and the Tube rows (look, reset, the
// ten groups, Degauss). Every Tube decision is app::tube_offered(state).

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "app/app_state.h"
#include "tube/tube_rows.h"
#include "tube/tube_setting.h"
#include "ui/settings_menu.h"
#include "ui_test_doubles.h"

using ui::MenuItem;
using ui::MenuSection;
using ui::SettingsMenuManager;

namespace {

void offer_tube(app::AppState& state) {
    state.platform_profile.tube_crt_engine = true;
    state.tube_renderer_ready = true;
    state.display_settings.mode = app::DisplayMode::MODERN_TV;
}

const MenuItem* row(const SettingsMenuManager& sm, const std::string& prefix) {
    for (const MenuItem& item : sm.get_submenu_items())
        if (item.label.rfind(prefix, 0) == 0) return &item;
    return nullptr;
}

// Copies the action first: it rebuilds submenu_items_, which would destroy a
// std::function called in place.
void press(const SettingsMenuManager& sm, const std::string& prefix) {
    const MenuItem* item = row(sm, prefix);
    REQUIRE(item);
    REQUIRE(item->action);
    const std::function<void()> action = item->action;
    action();
}

std::vector<std::string> labels(const SettingsMenuManager& sm) {
    std::vector<std::string> out;
    for (const MenuItem& item : sm.get_submenu_items()) out.push_back(item.label);
    return out;
}

std::vector<std::string> sublabels(const SettingsMenuManager& sm) {
    std::vector<std::string> out;
    for (const MenuItem& item : sm.get_submenu_items()) out.push_back(item.sublabel);
    return out;
}

// The Display page as it is today (before Tube), on a default AppState
// (CRT Native, List menu style, Classic engine, the effect defaults).
const std::vector<std::string> kTodaysLabels{
    "Mode: CRT Native", "Menu Style: List", "CRT Engine: Classic", "Scanlines: Low (10%)",
    "Color Warmth: OFF", "Phosphor Glow: OFF", "RGB Mask: OFF", "Screen Bloom: OFF",
    "Interlacing: OFF", "Flicker: OFF", "Back"};
const std::vector<std::string> kTodaysSublabels{
    "Cycle modes", "List / Channel guide", "Classic / Enhanced", "Horizontal CRT lines",
    "Warm tone + gamma", "Vignette + RGB convergence", "Subpixel R/G/B stripes",
    "Phosphor halation", "Alternate-line darken", "Brightness wobble", ""};

// The not-offered cases share one walk: Classic → Enhanced → Classic, no Tube rows.
void check_not_offered_cycle(app::AppState& state) {
    SettingsMenuManager sm{&state};
    sm.enter_submenu(MenuSection::DISPLAY);
    REQUIRE(row(sm, "CRT Engine: Classic"));
    CHECK(row(sm, "CRT Engine: ")->sublabel == "Classic / Enhanced");
    CHECK(row(sm, "CRT Engine: ")->section == MenuSection::CYCLE_CRT_ENGINE);
    for (const char* prefix : {"Scanlines: ", "Color Warmth: ", "Phosphor Glow: ", "RGB Mask: ",
                               "Screen Bloom: ", "Interlacing: ", "Flicker: "})
        CHECK(row(sm, prefix));
    CHECK_FALSE(row(sm, "Look: "));
    CHECK_FALSE(row(sm, "Degauss"));
    press(sm, "CRT Engine: ");
    CHECK(state.display_settings.crt_engine == app::CrtEngine::Enhanced);
    CHECK(row(sm, "CRT Engine: Enhanced"));
    CHECK_FALSE(row(sm, "Look: "));
    press(sm, "CRT Engine: ");
    CHECK(state.display_settings.crt_engine == app::CrtEngine::Classic);
    CHECK(row(sm, "CRT Engine: Classic"));
    CHECK(ui_test::fake_settings_saves() == 2);
}

}  // namespace

TEST_CASE("Tube menu: with the renderer not ready the Display page is exactly today's",
          "[settings_menu][tube]") {
    SECTION("default AppState") {
        ui_test::fake_reset();
        app::AppState state{};
        SettingsMenuManager sm{&state};
        sm.enter_submenu(MenuSection::DISPLAY);
        CHECK(labels(sm) == kTodaysLabels);
        CHECK(sublabels(sm) == kTodaysSublabels);
    }
    SECTION("a Pi 5 profile, renderer flag false") {
        ui_test::fake_reset();
        app::AppState state{};
        state.platform_profile.tube_crt_engine = true;
        REQUIRE_FALSE(state.tube_renderer_ready);
        SettingsMenuManager sm{&state};
        sm.enter_submenu(MenuSection::DISPLAY);
        CHECK(labels(sm) == kTodaysLabels);
        CHECK(sublabels(sm) == kTodaysSublabels);
    }
}

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
    REQUIRE(at != all.end());
    CHECK(std::vector<std::string>(at + 1, all.end()) == std::vector<std::string>{
        "Look: PVM", "Reset to look", "Picture", "Scanlines", "Mask", "Light", "Glass", "Signal",
        "Supply", "Character", "Sync", "Colour purity", "Degauss", "Back"});
    CHECK(row(sm, "Picture")->section == MenuSection::TUBE_GROUP);
    CHECK(row(sm, "Picture")->sublabel == "5 rows");
    CHECK(row(sm, "Degauss")->section == MenuSection::TUBE_DEGAUSS);
    CHECK(row(sm, "Degauss")->sublabel == "demagnetize the tube");
    press(sm, "CRT Engine: ");
    CHECK(state.display_settings.crt_engine == app::CrtEngine::Classic);
    CHECK(row(sm, "Scanlines: "));
    CHECK_FALSE(row(sm, "Look: "));
    CHECK(ui_test::fake_settings_saves() == 3);
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
    ui_test::fake_reset(); app::AppState state{};
    state.platform_profile.tube_crt_engine = true;
    REQUIRE_FALSE(state.tube_renderer_ready);
    check_not_offered_cycle(state);
}

TEST_CASE("Tube menu: picking a look clears what was moved; Reset keeps the look", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    auto& tube_setting = state.display_settings.tube;
    tube::set_row(tube_setting, tube::Effect::hum, 20);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    REQUIRE(row(sm, "Look: PVM (1 moved)"));
    CHECK(row(sm, "Reset to look")->sublabel == "1 row moved");
    CHECK(row(sm, "Look: ")->section == MenuSection::CYCLE_TUBE_LOOK);
    CHECK(row(sm, "Reset to look")->section == MenuSection::TUBE_RESET_LOOK);

    press(sm, "Look: ");
    CHECK(tube_setting.look == tube::Look::monitor);
    CHECK(tube::moved_keys(tube_setting).empty());
    CHECK(row(sm, "Look: Monitor"));
    CHECK(ui_test::fake_settings_saves() == 1);

    tube::set_row(tube_setting, tube::Effect::noise, 30);
    sm.rebuild_current_submenu();
    REQUIRE(row(sm, "Look: Monitor (1 moved)"));
    press(sm, "Reset to look");
    CHECK(tube_setting.look == tube::Look::monitor);
    CHECK(tube::moved_keys(tube_setting).empty());
    CHECK(row(sm, "Reset to look")->sublabel == "nothing moved");
    CHECK(row(sm, "Look: Monitor"));
    CHECK(ui_test::fake_settings_saves() == 2);

    press(sm, "Reset to look");
    CHECK(ui_test::fake_settings_saves() == 2);   // nothing moved: nothing to save
}

TEST_CASE("Tube menu: Degauss records a request and saves nothing", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Degauss");
    CHECK(state.tube_live.degauss_requests == 1);
    CHECK(ui_test::fake_settings_saves() == 0);
    CHECK(ui_test::fake_last_toast().empty());
}

TEST_CASE("Tube menu: a group row opens that group and its sublabel counts moved rows",
          "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    tube::set_row(state.display_settings.tube, tube::Effect::hum, 20);   // a SUPPLY row
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    CHECK(row(sm, "Supply")->sublabel == "4 rows · 1 moved");
    const uint64_t on_display = sm.redraw_signature();
    press(sm, "Supply");
    CHECK(sm.get_current_submenu() == MenuSection::TUBE_GROUP);
    CHECK(sm.redraw_signature() != on_display);
    CHECK(ui_test::fake_settings_saves() == 0);
}
