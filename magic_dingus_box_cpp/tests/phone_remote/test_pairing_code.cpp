#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include "ui/pairing_screen.h"

namespace fs = std::filesystem;

TEST_CASE("pairing code is 6-digit decimal", "[remote][pairing]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_pairing_test.json";
    fs::remove(tmp);

    ui::PairingScreen p(tmp.string());
    p.regenerate();

    REQUIRE(fs::exists(tmp));
    std::ifstream f(tmp);
    Json::Value root;
    f >> root;

    std::string code = root["code"].asString();
    REQUIRE(code.size() == 6);
    for (char c : code) REQUIRE(c >= '0');
    for (char c : code) REQUIRE(c <= '9');
    REQUIRE(root["attempts_remaining"].asInt() == 5);
    REQUIRE(root["expires_at"].asInt64() > root["issued_at"].asInt64());
    REQUIRE(root["nonce"].asString().size() >= 32);
}

TEST_CASE("two regenerations produce different codes", "[remote][pairing]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_pairing_test2.json";
    fs::remove(tmp);

    ui::PairingScreen p(tmp.string());
    p.regenerate();
    auto code1 = p.current_code();
    p.regenerate();
    auto code2 = p.current_code();
    REQUIRE(code1 != code2);  // entropy sanity check
}

TEST_CASE("close() deletes the session file", "[remote][pairing]") {
    fs::path tmp = fs::temp_directory_path() / "mdb_pairing_test3.json";
    fs::remove(tmp);

    ui::PairingScreen p(tmp.string());
    p.regenerate();
    REQUIRE(fs::exists(tmp));
    p.close();
    REQUIRE_FALSE(fs::exists(tmp));
}
