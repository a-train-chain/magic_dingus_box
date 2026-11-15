// Settings -> Display: the Menu Style row (List / Channel Guide).

#include <catch2/catch_test_macros.hpp>

#include "app/app_state.h"
#include "ui/settings_menu.h"
#include "ui_test_doubles.h"

using ui::MenuSection;
using ui::SettingsMenuManager;

TEST_CASE("Display menu: Menu Style sits right after Mode and flips on select",
          "[settings_menu][display]") {
    ui_test::fake_reset();
    app::AppState state{};
    SettingsMenuManager sm{&state};
    REQUIRE(state.display_settings.menu_style == app::MenuStyle::List);

    sm.enter_submenu(MenuSection::DISPLAY);
    const auto& items = sm.get_submenu_items();
    REQUIRE(items.size() > 2);
    CHECK(items[0].label.rfind("Mode: ", 0) == 0);
    CHECK(items[1].label == "Menu Style: List");
    CHECK(items[1].sublabel == "List / Channel guide");

    items[1].action();
    CHECK(state.display_settings.menu_style == app::MenuStyle::ChannelGuide);
    CHECK(ui_test::fake_settings_saves() == 1);
    CHECK(sm.get_submenu_items()[1].label == "Menu Style: Channel Guide");

    sm.get_submenu_items()[1].action();
    CHECK(state.display_settings.menu_style == app::MenuStyle::List);
    CHECK(ui_test::fake_settings_saves() == 2);
    CHECK(sm.get_submenu_items()[1].label == "Menu Style: List");
}
