#include <catch2/catch_test_macros.hpp>

#include <string>

#include "app/rom_title.h"

using app::title_from_rom_path;

TEST_CASE("region tags are stripped from the displayed title",
          "[app][title]") {
    REQUIRE(title_from_rom_path("data/roms/n64/Super Mario 64 (USA).z64") ==
            "Super Mario 64");
    REQUIRE(title_from_rom_path("data/roms/n64/Banjo-Tooie (Europe).z64") ==
            "Banjo-Tooie");
    REQUIRE(title_from_rom_path("data/roms/n64/Sin and Punishment (Japan).z64") ==
            "Sin and Punishment");
}

TEST_CASE("stacked No-Intro tags are all stripped", "[app][title]") {
    // Real filenames from the box's own library.
    REQUIRE(title_from_rom_path(
                "data/roms/n64/Diddy Kong Racing (USA) (En,Fr) (Rev 1).z64") ==
            "Diddy Kong Racing");
    REQUIRE(title_from_rom_path(
                "data/roms/dreamcast/Sonic Adventure 2 (USA) (En,Ja,Fr,De,Es).chd") ==
            "Sonic Adventure 2");
    REQUIRE(title_from_rom_path("data/roms/ps1/Silent Hill (USA) (v1.1).chd") ==
            "Silent Hill");
    REQUIRE(title_from_rom_path("data/roms/n64/Star Fox 64 (USA) (Rev 1).z64") ==
            "Star Fox 64");
}

TEST_CASE("article suffix is moved to the front", "[app][title]") {
    REQUIRE(title_from_rom_path(
                "data/roms/n64/Legend of Zelda, The - Ocarina of Time (USA).z64") ==
            "The Legend of Zelda: Ocarina of Time");
    REQUIRE(title_from_rom_path("data/roms/nes/Guardian Legend, The (USA).nes") ==
            "The Guardian Legend");
}

TEST_CASE("subtitles read as subtitles", "[app][title]") {
    REQUIRE(title_from_rom_path(
                "data/roms/n64/Kirby 64 - The Crystal Shards (USA).z64") ==
            "Kirby 64: The Crystal Shards");
    REQUIRE(title_from_rom_path(
                "data/roms/n64/Wave Race 64 - Kawasaki Jet Ski (USA).z64") ==
            "Wave Race 64: Kawasaki Jet Ski");
}

TEST_CASE("parentheses that belong to the title survive", "[app][title]") {
    // Only KNOWN dump/region tags may be stripped. A parenthetical that
    // isn't one is part of the name and removing it would corrupt the
    // title -- the failure mode is silent and permanent-looking.
    REQUIRE(title_from_rom_path("data/roms/arcade/Donkey Kong (Original).zip") ==
            "Donkey Kong (Original)");
    REQUIRE(title_from_rom_path("data/roms/n64/Bangai-O.z64") == "Bangai-O");
}

TEST_CASE("a year in parentheses is not mistaken for a revision",
          "[app][title]") {
    // "(1.1)" is a version and goes; "(1981)" is part of the name and
    // stays. Both are digits-only, so the rule has to be narrower than
    // "looks numeric" or it silently eats real titles.
    REQUIRE(title_from_rom_path("data/roms/arcade/Donkey Kong (1981).zip") ==
            "Donkey Kong (1981)");
    REQUIRE(title_from_rom_path("data/roms/ps1/Metal Gear Solid (1.1).chd") ==
            "Metal Gear Solid");
}

TEST_CASE("bracket dump flags are stripped", "[app][title]") {
    REQUIRE(title_from_rom_path("data/roms/nes/Contra (USA) [!].nes") ==
            "Contra");
}

TEST_CASE("titles that are already clean are left alone", "[app][title]") {
    REQUIRE(title_from_rom_path("Super Metroid") == "Super Metroid");
    REQUIRE(title_from_rom_path("") == "");
}

TEST_CASE("apostrophes and hyphens inside a word are untouched",
          "[app][title]") {
    REQUIRE(title_from_rom_path(
                "data/roms/n64/Conker's Bad Fur Day (USA).z64") ==
            "Conker's Bad Fur Day");
    REQUIRE(title_from_rom_path("data/roms/n64/F-Zero X (USA).z64") ==
            "F-Zero X");
}
