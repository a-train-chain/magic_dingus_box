// app/tube_rows_json.h - the checked-in web row file is what the C++ tables generate.
#include <catch2/catch_test_macros.hpp>
#include <json/json.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "app/tube_rows_json.h"
#include "tube/tube_frame_class.h"

namespace fs = std::filesystem;
const fs::path kRowFile = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path()
                          / "magic_dingus_box" / "web" / "static" / "tube_rows.json";

TEST_CASE("tube rows json: the checked-in file is what the tables generate", "[tube][tube_rows_json]") {
    const std::string want = app::tube_rows_json();
    if (std::getenv("MDB_WRITE_TUBE_ROWS_JSON")) { std::ofstream(kRowFile, std::ios::binary) << want; }
    std::ifstream f(kRowFile, std::ios::binary);
    REQUIRE(f.good());
    const std::string have((std::istreambuf_iterator<char>(f)), {});
    INFO("regenerate: MDB_WRITE_TUBE_ROWS_JSON=1 ./build-tube2/test_app_unit \"[tube_rows_json]\"");
    REQUIRE(have == want);
}
TEST_CASE("tube rows json: it parses and holds every row", "[tube][tube_rows_json]") {
    Json::Value root; std::istringstream in(app::tube_rows_json());
    REQUIRE(Json::parseFromStream(Json::CharReaderBuilder{}, in, &root, nullptr));
    REQUIRE(root["rows"].size() == 44);
    REQUIRE(root["rows"][24]["key"].asString() == "hum");
    REQUIRE(root["rows"][24]["hint"][0].asString() == "soft at low · pronounced at 100");
    REQUIRE(root["rows"][10]["step"].asInt() == 10);
    REQUIRE(root["rows"][5]["crt_native_neutral"].asBool());          // scanlines
    REQUIRE(root["groups"][2]["rows"][0].asString() == "mask");
    REQUIRE(root["groups"][9]["title"].asString() == "Colour purity");
    REQUIRE(root["looks"][3]["values"]["hum"].asInt() == 60);         // tv
    REQUIRE(root["looks"][3]["mask"].asString() == "slot");
    REQUIRE(root["mask"]["choices"][4]["word"].asString() == "OFF");
    REQUIRE(root["engines"][2]["key"].asString() == "tube");
}

TEST_CASE("tube rows json: the 30 fps rows and each look's rate", "[tube][tube_rows_json]") {
    Json::Value root; std::istringstream in(app::tube_rows_json()); in >> root;
    for (const auto& r : root["rows"])
        REQUIRE(r["forces_30fps"].asBool() == tube::forces_30fps(*tube::parse_effect(r["key"].asString())));
    for (const auto& l : root["looks"]) {
        tube::TubeSetting s; tube::select_look(s, *tube::parse_look(l["key"].asString()));
        REQUIRE(l["fps"].asInt() == tube::frames_per_second(tube::setting_frame_class(tube::resolve(s), false)));
    }
    REQUIRE(root["looks"][3]["fps"].asInt() == 30);                  // tv is rich whatever Task 1b decided
}

// Today's measured facts, pinned so a change to the lean set is a visible diff here.
// tube_budget.h (kLeanSet, kAlwaysRich) is what decides them.
TEST_CASE("tube rows json: with the shipped lean set 14 rows force 30 fps and every look is 30 fps",
          "[tube][tube_rows_json]") {
    Json::Value root; std::istringstream in(app::tube_rows_json()); in >> root;
    int forcing = 0;
    for (const auto& r : root["rows"]) forcing += r["forces_30fps"].asBool() ? 1 : 0;
    REQUIRE(forcing == 14);   // the seven kAlwaysRich + noise, flicker, hum, rollBand, purity, abl, breathing
    REQUIRE(root["looks"].size() == 6);
    for (const auto& l : root["looks"]) REQUIRE(l["fps"].asInt() == 30);
}
