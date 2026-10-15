#include <catch2/catch_test_macros.hpp>

#include "app/torrent_pause_marker.h"

#include <filesystem>

TEST_CASE("torrent pause marker records only kiosk-made pauses", "[quiet]") {
    const auto path = (std::filesystem::temp_directory_path() /
                       "mdb_test_qbit_paused_by_kiosk").string();
    std::filesystem::remove(path);

    CHECK_FALSE(app::torrents_paused_by_kiosk(path));   // clean boot: no-op
    app::mark_torrents_paused_by_kiosk(path, true);
    CHECK(app::torrents_paused_by_kiosk(path));         // survives a restart
    app::mark_torrents_paused_by_kiosk(path, false);
    CHECK_FALSE(app::torrents_paused_by_kiosk(path));
    app::mark_torrents_paused_by_kiosk(path, false);     // idempotent
    CHECK_FALSE(app::torrents_paused_by_kiosk(path));
}
