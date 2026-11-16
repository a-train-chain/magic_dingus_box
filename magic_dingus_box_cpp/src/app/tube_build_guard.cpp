#include "app/tube_build_guard.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <sys/stat.h>

#include "../utils/logger.h"

namespace app {

namespace {
constexpr std::size_t kMaxMarkerBytes = 4096;
constexpr std::size_t kMaxProgramChars = 256;
constexpr const char* kUnknownProgram = "unknown";
}  // namespace

TubeBuildGuard::TubeBuildGuard(std::string marker_path) : path_(std::move(marker_path)) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path_, ec);
    // "Not found" is the clean case, with or without an error code (a parent that is
    // not a directory reports ENOTDIR and type not_found). Any other answer, including
    // an error stat could not explain, is treated as a marker. A false trip is safe by
    // design: nothing clears a marker this process did not arm, so Tube stays off until
    // the next REBOOT (/dev/shm survives a kiosk restart). A miss would be a second hang.
    if (status.type() == std::filesystem::file_type::not_found) return;
    tripped_ = true;
    if (!ec && status.type() == std::filesystem::file_type::regular) {
        std::ifstream in(path_, std::ios::binary);
        if (in) {
            std::string text(kMaxMarkerBytes, '\0');
            in.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<std::size_t>(in.gcount()));
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            // The name reaches the journal: printable ASCII only, bounded.
            if (text.size() > kMaxProgramChars) text.resize(kMaxProgramChars);
            for (char& c : text)
                if (c < 0x20 || c > 0x7E) c = '?';
            program_ = std::move(text);
        }
    }
    if (program_.empty()) program_ = kUnknownProgram;
}

void TubeBuildGuard::begin(std::string_view programs) {
    // A tripped guard's marker is the diagnosis of a hang: never overwritten.
    if (tripped_) {
        LOG_WARN("tube build guard: tripped by {}; not writing a new marker", program_);
        return;
    }
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    const bool opened = out.is_open();
    if (out) {
        out.write(programs.data(), static_cast<std::streamsize>(programs.size()));
        out.flush();
        out.close();
    }
    if (!out) {
        LOG_WARN("tube build guard: could not write {}; building without a marker", path_);
        // Opened (so created or truncated by us) but not fully written: a partial file
        // would read as a hang at the next start, and end() would not remove it (not
        // armed) - Tube off until a reboot for a write error. Remove it now, and with it
        // anything this instance wrote before (the open truncated it): disarm.
        // A failed OPEN touched nothing: whatever an earlier begin() wrote is still
        // there and still ours, so armed_ and the recorded text stay as they were and
        // end() still removes it. A file we never opened is not ours to remove.
        if (opened) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            armed_ = false;
        }
        return;
    }
    // What this instance last wrote with a SUCCESSFUL begin(): end() recognises its own
    // marker by this content (a failed open or write never reaches here).
    written_.assign(programs.data(), programs.size());
    armed_ = true;
}

void TubeBuildGuard::end() {
    // Only a marker this instance wrote is ours to remove.
    if (!armed_) return;
    armed_ = false;
    // Identity is by CONTENT, not device + inode: Linux filesystems (ext4, tmpfs,
    // overlayfs) hand a freed inode number to the very next file created, so a look-alike
    // written right after ours was removed has the same device and inode. The path must
    // be a regular file (never a directory) holding exactly what begin() last wrote.
    // A file with our exact content written by someone else is indistinguishable and
    // removing it is harmless: a marker with our content means "our build is in
    // progress", which is no longer true. The read is bounded (one byte past our length
    // is enough to tell "longer"); a short read on memory-backed /dev/shm is acceptable,
    // it only makes end() leave a file it could not confirm.
    struct stat st{};
    if (::lstat(path_.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return;
    const std::size_t want = std::min(written_.size(), kMaxMarkerBytes) + 1;
    std::string text(want, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    if (text != written_) return;
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

}  // namespace app
