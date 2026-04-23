// Smoke test: proves Catch2 is wired up correctly.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

TEST_CASE("Catch2 is wired up", "[smoke]") {
    REQUIRE(1 + 1 == 2);
}

TEST_CASE("C++17 filesystem works", "[smoke]") {
    namespace fs = std::filesystem;
    fs::path p = "/tmp";
    REQUIRE(fs::exists(p));
}
