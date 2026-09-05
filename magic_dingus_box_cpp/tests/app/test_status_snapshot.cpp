#include <catch2/catch_test_macros.hpp>
#include <json/json.h>

#include "app/app_state.h"
#include "app/status_snapshot.h"
#include "ui/virtual_keyboard.h"

TEST_CASE("status snapshot carries schema 1 and the phone remote's keys", "[app][status]") {
    app::AppState state;
    state.current_playlist_name = "Sacred Steel";
    state.current_item_index = 2;
    state.current_item_count = 7;
    state.now_playing_title = "Song";
    state.ui_visible_when_playing = false;
    state.set_duration(120.0);
    state.set_position(30.5);

    Json::Value j = app::build_status_json(state);

    REQUIRE(j["schema"].asInt() == 1);
    REQUIRE(j["screen"].asString() == "playlist");
    REQUIRE(j["playlist"]["name"].asString() == "Sacred Steel");
    REQUIRE(j["playlist"]["item_index"].asInt() == 2);
    REQUIRE(j["playlist"]["item_count"].asInt() == 7);
    REQUIRE(j["now_playing"]["title"].asString() == "Song");
    REQUIRE(j["playback"]["position_sec"].asDouble() == 30.5);
    REQUIRE(j["playback"]["duration_sec"].asDouble() == 120.0);
    REQUIRE(j["playback"]["is_paused"].asBool() == false);
    REQUIRE(j["overlay_visible"].asBool() == false);
    REQUIRE(j["retroarch"].isNull());
    REQUIRE(j["settings"]["active"].asBool() == false);
    REQUIRE(j["settings"]["game_playlist_count"].asInt() == 0);
    REQUIRE(j["text_input"]["active"].asBool() == false);
    REQUIRE_FALSE(j.isMember("ts"));
}

TEST_CASE("status snapshot reports the active text field", "[app][status]") {
    app::AppState state;
    ui::VirtualKeyboard kb;
    // type_char() is a no-op while the keyboard is inactive (see
    // ui/virtual_keyboard.cpp) — real callers only ever reach it through
    // an already-open() keyboard (see controller_text_input.cpp), so the
    // test opens it first to reach the same state.
    kb.open("", "Search", nullptr, nullptr);
    kb.type_char('a');
    kb.type_char('b');
    state.active_text_keyboard = &kb;
    state.active_text_title = "Search";
    Json::Value j = app::build_status_json(state);
    REQUIRE(j["text_input"]["active"].asBool());
    REQUIRE(j["text_input"]["title"].asString() == "Search");
    REQUIRE(j["text_input"]["buffer"].asString() == "ab");
}

TEST_CASE("status snapshot reports a RetroArch session", "[app][status]") {
    app::AppState state;
    state.screen_mode = app::ScreenMode::RetroArch;
    state.retroarch_rom_name = "Zelda";
    state.retroarch_core = "nestopia_libretro";
    Json::Value j = app::build_status_json(state);
    REQUIRE(j["screen"].asString() == "retroarch");
    REQUIRE(j["retroarch"]["rom_name"].asString() == "Zelda");
    REQUIRE(j["retroarch"]["core"].asString() == "nestopia_libretro");
}
