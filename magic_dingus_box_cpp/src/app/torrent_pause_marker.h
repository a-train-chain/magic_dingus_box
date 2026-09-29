#pragma once

// Crash-recovery record for torrents the KIOSK paused.
//
// Movie (Pi 4B / low-memory FullPause) and game quiet modes call qBit's
// pause_all() and resume on the way out. If the kiosk dies or is stopped in
// between — crash, watchdog, OTA install, a slow shutdown past the 3 s
// quiet-mode drain — nothing resumed them: containers are restarted by the
// startup unpause, but torrents stayed paused until someone opened qBit.
// A plain file survives the restart; startup resumes only when it exists,
// so a pause the operator made by hand is never undone.

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace app {

inline void mark_torrents_paused_by_kiosk(const std::string& marker_path,
                                          bool paused) {
    std::error_code ec;
    if (paused) {
        std::ofstream(marker_path) << "1\n";
    } else {
        std::filesystem::remove(marker_path, ec);
    }
}

inline bool torrents_paused_by_kiosk(const std::string& marker_path) {
    std::error_code ec;
    return std::filesystem::exists(marker_path, ec);
}

}  // namespace app
