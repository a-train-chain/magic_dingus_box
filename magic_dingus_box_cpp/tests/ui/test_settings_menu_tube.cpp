// Settings -> Display: the CRT Engine row and the Tube rows (look, reset, the
// ten groups, Degauss). Every Tube decision is app::tube_offered(state).

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "app/app_state.h"
#include "app/tube_menu_logic.h"
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

// Lets the open slide finish, then runs the one call that advances it (as
// test_settings_menu_redraw.cpp does).
void finish_slide(SettingsMenuManager& sm) {
    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    (void)sm.get_animation_progress();
}

// Offered, engine Tube, the Supply group open with HUM BAR in edit mode and
// stepped once (0 -> 5): one pending save.
void editing_hum_bar(app::AppState& state, SettingsMenuManager& sm) {
    sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Supply");
    press(sm, "HUM BAR");
    REQUIRE(sm.is_tube_editing());
    sm.step_tube_edit(+1);
    REQUIRE(*state.display_settings.tube.rows[tube::index_of(tube::Effect::hum)] == 5);
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
    CHECK(row(sm, "HUM BAR")->section == MenuSection::TUBE_VALUE);
    CHECK(row(sm, "Back")->section == MenuSection::TUBE_GROUP_BACK);
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

TEST_CASE("Tube menu: an edit that changes nothing saves nothing", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Supply"); press(sm, "HUM BAR");
    sm.step_tube_edit(-1);                                     // already at 0
    sm.end_tube_edit();
    CHECK_FALSE(sm.is_tube_editing());
    CHECK(ui_test::fake_settings_saves() == 0);
}

TEST_CASE("Tube menu: opening another row saves the edit that was open", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state};
    editing_hum_bar(state, sm);
    press(sm, "FLICKER");
    CHECK(ui_test::fake_settings_saves() == 1);
    CHECK(sm.is_tube_editing());
    CHECK(sm.get_submenu_items()[0].label == "FLICKER: < 0 >");
    CHECK(sm.get_submenu_items()[1].label == "HUM BAR: 5");
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

TEST_CASE("Tube menu: BTN4 in the existing submenus is unchanged", "[settings_menu][tube]") {
    for (const bool offered : {false, true}) {
        ui_test::fake_reset(); app::AppState state{};
        if (offered) offer_tube(state);
        SettingsMenuManager sm{&state}; sm.open();
        for (const MenuSection section : {MenuSection::VIDEO_GAMES, MenuSection::DISPLAY,
                                          MenuSection::AUDIO, MenuSection::SYSTEM, MenuSection::WIFI}) {
            sm.enter_submenu(section);
            sm.toggle();
            CHECK(sm.get_current_submenu() == MenuSection::BACK);
            CHECK(sm.is_active());
        }
        sm.enter_submenu(MenuSection::WIFI_NETWORKS);
        sm.toggle();
        CHECK(sm.get_current_submenu() == MenuSection::WIFI);
        sm.toggle();
        CHECK(sm.get_current_submenu() == MenuSection::BACK);
        sm.toggle();
        CHECK(sm.is_closing());
        CHECK(ui_test::fake_settings_saves() == 0);
    }
}

TEST_CASE("Tube menu: closing the menu mid-edit saves the edit", "[settings_menu][tube]") {
    SECTION("close()") {
        ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
        state.display_settings.set_crt_engine(app::CrtEngine::Tube);
        SettingsMenuManager sm{&state}; sm.open();
        editing_hum_bar(state, sm);
        sm.close();
        CHECK_FALSE(sm.is_tube_editing());
        CHECK(ui_test::fake_settings_saves() == 1);
        CHECK(sm.get_submenu_items()[1].label == "HUM BAR: 5");
    }
    SECTION("force_close() (the Media Browser or a game takes the screen)") {
        ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
        state.display_settings.set_crt_engine(app::CrtEngine::Tube);
        SettingsMenuManager sm{&state}; sm.open();
        editing_hum_bar(state, sm);
        sm.force_close();
        CHECK_FALSE(sm.is_tube_editing());
        CHECK(ui_test::fake_settings_saves() == 1);
    }
    SECTION("exit_submenu()") {
        ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
        state.display_settings.set_crt_engine(app::CrtEngine::Tube);
        SettingsMenuManager sm{&state}; sm.open();
        editing_hum_bar(state, sm);
        sm.exit_submenu();
        CHECK_FALSE(sm.is_tube_editing());
        CHECK(ui_test::fake_settings_saves() == 1);
    }
    SECTION("a fresh enter_submenu()") {
        ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
        state.display_settings.set_crt_engine(app::CrtEngine::Tube);
        SettingsMenuManager sm{&state}; sm.open();
        editing_hum_bar(state, sm);
        sm.enter_submenu(MenuSection::AUDIO);
        CHECK_FALSE(sm.is_tube_editing());
        CHECK(ui_test::fake_settings_saves() == 1);
    }
    SECTION("the group's Back row") {
        ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
        state.display_settings.set_crt_engine(app::CrtEngine::Tube);
        SettingsMenuManager sm{&state}; sm.open();
        editing_hum_bar(state, sm);
        press(sm, "Back");
        CHECK_FALSE(sm.is_tube_editing());
        CHECK(sm.get_current_submenu() == MenuSection::DISPLAY);
        CHECK(ui_test::fake_settings_saves() == 1);
        CHECK(row(sm, "Supply")->sublabel == "4 rows · 1 moved");
    }
}

TEST_CASE("Tube menu: an engine change from outside leaves the open group", "[settings_menu][tube]") {
    SECTION("the engine is set to Enhanced") {
        ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
        state.display_settings.set_crt_engine(app::CrtEngine::Tube);
        SettingsMenuManager sm{&state}; sm.open();
        editing_hum_bar(state, sm);
        state.display_settings.set_crt_engine(app::CrtEngine::Enhanced);   // as the op log would
        sm.rebuild_current_submenu();
        CHECK(sm.get_current_submenu() == MenuSection::DISPLAY);
        CHECK_FALSE(sm.is_tube_editing());
        CHECK(ui_test::fake_settings_saves() == 1);
        CHECK(row(sm, "Scanlines: "));
        CHECK_FALSE(row(sm, "Look: "));
    }
    SECTION("Tube stops being offered") {
        ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
        state.display_settings.set_crt_engine(app::CrtEngine::Tube);
        SettingsMenuManager sm{&state}; sm.open();
        sm.enter_submenu(MenuSection::DISPLAY);
        press(sm, "Supply");
        state.tube_renderer_ready = false;
        sm.rebuild_current_submenu();
        CHECK(sm.get_current_submenu() == MenuSection::DISPLAY);
        CHECK(row(sm, "CRT Engine: Enhanced"));
        CHECK(ui_test::fake_settings_saves() == 0);
    }
}

TEST_CASE("Tube menu: CRT Native shows the tube's own rows as n/a", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.mode = app::DisplayMode::CRT_NATIVE;
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Scanlines");
    REQUIRE(sm.get_current_submenu() == MenuSection::TUBE_GROUP);
    CHECK(labels(sm) == std::vector<std::string>{
        "SCANLINES: n/a", "SCANLINE SIZE: 10", "BEAM SWELL: 80", "TRUE RASTER: n/a", "Back"});
    for (const char* na : {"SCANLINES: n/a", "TRUE RASTER: n/a"}) {
        const MenuItem* item = row(sm, na);
        REQUIRE(item);
        CHECK(item->section == MenuSection::INFO);
        CHECK_FALSE(item->action);
        CHECK(item->sublabel == app::kTubeNaSublabel);
    }
    press(sm, "Back");
    press(sm, "Mask");
    CHECK(labels(sm) == std::vector<std::string>{
        "MASK: n/a", "MASK STRENGTH: n/a", "MASK SIZE: n/a", "Back"});
    CHECK(row(sm, "MASK: n/a")->section == MenuSection::INFO);
    CHECK_FALSE(row(sm, "MASK: n/a")->action);
}

TEST_CASE("Tube menu: the MASK row cycles in edit mode", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Mask");
    CHECK(sm.submenu_title() == "Mask");
    CHECK(row(sm, "MASK")->section == MenuSection::TUBE_MASK);
    press(sm, "MASK");
    REQUIRE(sm.is_tube_editing());
    CHECK(sm.get_submenu_items()[0].label == "MASK: < GRILLE >");
    CHECK(sm.get_submenu_items()[0].sublabel == "aperture grille · default");
    sm.step_tube_edit(+1);
    CHECK(sm.get_submenu_items()[0].label == "MASK: < MAGENTA-GREEN >");
    CHECK(ui_test::fake_settings_saves() == 0);
    sm.end_tube_edit();
    CHECK(ui_test::fake_settings_saves() == 1);
    CHECK(tube::resolve(state.display_settings.tube).mask == tube::Mask::magentaGreen);
    CHECK(sm.get_submenu_items()[0].label == "MASK: MAGENTA-GREEN");
}

TEST_CASE("Tube menu: the header names the page", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state};
    CHECK(sm.submenu_title() == "Settings");
    const std::vector<std::pair<MenuSection, std::string>> pages{
        {MenuSection::VIDEO_GAMES, "Video games"}, {MenuSection::DISPLAY, "Display"},
        {MenuSection::AUDIO, "Audio"}, {MenuSection::SYSTEM, "System info"},
        {MenuSection::WIFI, "Wi-Fi"}, {MenuSection::WIFI_NETWORKS, "Wi-Fi Networks"}};
    for (const auto& [section, title] : pages) {
        sm.enter_submenu(section);
        CHECK(sm.submenu_title() == title);
    }
    sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Colour purity");
    CHECK(sm.tube_group_index() == 9);
    CHECK(sm.submenu_title() == "Colour purity");
}

TEST_CASE("Tube menu: edit mode stays static and redraws on each step", "[settings_menu][redraw][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state};
    sm.open();
    finish_slide(sm);
    REQUIRE(sm.is_static_for_redraw());
    sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Supply");
    const uint64_t s0 = sm.redraw_signature();
    press(sm, "HUM BAR");
    CHECK(sm.redraw_signature() != s0);
    CHECK(sm.is_static_for_redraw());
    const uint64_t s1 = sm.redraw_signature();
    sm.step_tube_edit(+1);
    CHECK(sm.redraw_signature() != s1);
    const uint64_t s2 = sm.redraw_signature();
    // A value changed from elsewhere reaches the signature through the labels.
    tube::set_row(state.display_settings.tube, tube::Effect::flicker, 40);
    sm.rebuild_current_submenu();
    CHECK(sm.redraw_signature() != s2);
}

TEST_CASE("Tube menu: a row that turns n/a mid-edit ends its edit", "[settings_menu][tube]") {
    ui_test::fake_reset(); app::AppState state{}; offer_tube(state);
    state.display_settings.set_crt_engine(app::CrtEngine::Tube);
    SettingsMenuManager sm{&state}; sm.enter_submenu(MenuSection::DISPLAY);
    press(sm, "Glass"); press(sm, "RIM");
    sm.step_tube_edit(+1);
    state.display_settings.mode = app::DisplayMode::CRT_NATIVE;   // changed from elsewhere
    sm.rebuild_current_submenu();
    CHECK_FALSE(sm.is_tube_editing());
    CHECK(ui_test::fake_settings_saves() == 1);
    CHECK(row(sm, "RIM: n/a"));
    const int rim = tube::resolve(state.display_settings.tube).effects[tube::index_of(tube::Effect::rim)];
    sm.step_tube_edit(+1);
    CHECK(tube::resolve(state.display_settings.tube).effects[tube::index_of(tube::Effect::rim)] == rim);
}
