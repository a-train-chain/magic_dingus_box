// Proves mdb_core links on this host: one symbol from each subsystem the
// library claims. If a Linux-only include ever leaks into the list, this
// target stops building on the Mac before anything downstream notices.
#include <catch2/catch_test_macros.hpp>

#include "app/playlist_loader.h"
#include "app/settings_persistence.h"
#include "platform/platform_profile.h"
#include "retroarch/controller_mapping.h"
#include "utils/config.h"

TEST_CASE("mdb_core links its subsystems on this host", "[core]") {
    REQUIRE_FALSE(config::get_playlists_dir().empty());
    REQUIRE(platform::normalize_game_system("NES") == "nes");
    auto split = app::PlaylistLoader::split_for_ui({});
    REQUIRE(split.video.empty());
    REQUIRE(split.games.empty());
}
