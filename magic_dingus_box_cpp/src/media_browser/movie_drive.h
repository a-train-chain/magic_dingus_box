#pragma once

#include <string>

namespace media_browser {

// True iff `mountpoint` is a real mount in the given /proc/mounts content.
//
// This answers the one question the kiosk's Storage-path row cannot: is the
// movie drive actually there? With nothing mounted at STORAGE_ROOT, that path
// is a plain directory on the SD card, so Radarr reports a valid root folder
// and the free-space readout shows the SD's free space — downloads quietly
// fill the partition the OS runs from, and every existing indicator says fine.
//
// Takes the file CONTENT rather than reading it, so the interesting cases
// (prefix collisions, escaped paths, ragged lines) are testable without a real
// block device. See tests/media_browser/test_movie_drive.cpp.
bool movie_drive_mounted(const std::string& proc_mounts,
                         const std::string& mountpoint);

// Reads /proc/mounts and applies the above. Returns false if it cannot be
// read — failing toward the warning, since a false alarm makes someone check a
// cable while the opposite hides the failure this exists to surface.
bool movie_drive_mounted_now(const std::string& mountpoint);

}  // namespace media_browser
