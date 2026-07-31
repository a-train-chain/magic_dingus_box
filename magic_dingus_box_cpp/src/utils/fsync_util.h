#pragma once

// Durability helpers for the tmp-write → rename save pattern.
//
// fs::rename makes a save ATOMIC (a reader sees the old file or the new
// file, never a torn one) but not DURABLE: on a power cut the journal
// can commit the rename (metadata) before the temp file's DATA reaches
// flash, leaving a zero-length or truncated file under the final name.
// ext4's auto_da_alloc narrows that window for rename-over-existing but
// does not close it (and does nothing for a first-ever save). On a
// kiosk that customers power-cut routinely, the consequences are silent
// and nasty — SettingsPersistence::peek_is_crt_native() treats an
// unreadable settings.json as CRT_NATIVE, so a MODERN_TV 1080p unit
// reboots into 720p CRT mode with no error anywhere.
//
// Usage, in order:
//   write tmp  →  fsync_file(tmp)  →  rename(tmp, final)  →
//   fsync_parent_dir(final)
//
// Both helpers are best-effort: callers on rare-write paths treat a
// false return as a warning, never a save failure — the atomic rename
// alone already equals the old behavior.
//
// NOT for hot paths. fsync on an SD card costs tens of milliseconds and
// real flash wear; StatusWriter (5 writes/second, forever) must never
// call these. Reserve them for operator-action saves: settings,
// controller profiles.

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <string>

namespace utils {

inline bool fsync_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
}

inline bool fsync_parent_dir(const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    std::string dir = parent.empty() ? std::string(".") : parent.string();
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int fd = ::open(dir.c_str(), flags);
    if (fd < 0) return false;
    bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
}

}  // namespace utils
