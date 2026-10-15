#pragma once

// File-side half of the kiosk's debug screenshot hook: naming, atomic
// write, retention. No GL — the readback lives in debug/screenshot_capture.
// Unit-tested on the Mac (tests/utils/test_screenshot_store.cpp).
//
// Screenshots can show personal content (playlists, movie titles, Wi-Fi
// names), so <data>/screenshots is excluded from every deploy/OTA rsync and
// scrubbed from golden images (prepare_for_cloning.sh + first_boot.sh).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace utils {

// How many screenshots the box keeps; older ones are deleted after each save.
constexpr std::size_t kScreenshotsKept = 10;

// "20261004T153012_345Z.bmp" (UTC, millisecond resolution). Fixed width, so
// lexicographic order equals chronological order — prune_screenshots()
// relies on that. No colons: the files are meant to be scp'd off the box.
// Returns "" if the time cannot be converted.
std::string screenshot_filename(std::chrono::system_clock::time_point t);

// Writes `bytes` to `path` via `path + ".tmp"` and rename(), so a reader
// (or a crash) never sees a half-written file under the final name.
// Creates the parent directory. On failure removes the .tmp, fills
// `error` (if given) and returns false.
bool write_file_atomic(const std::string& path,
                       const std::vector<std::uint8_t>& bytes,
                       std::string* error = nullptr);

// Keeps the newest `keep` screenshots (*.bmp, newest = greatest name) in
// `dir` and deletes the rest, plus any leftover *.bmp.tmp from an
// interrupted write. Only regular files ending in .bmp/.bmp.tmp are
// touched. Returns how many files were deleted; a missing dir is 0.
std::size_t prune_screenshots(const std::string& dir, std::size_t keep);

}  // namespace utils
