#include "app/tube_build_guard.h"

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
        // there and still ours, so armed_ and the recorded file stay as they were and
        // end() still removes it. A file we never opened is not ours to remove.
        if (opened) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            armed_ = false;
        }
        return;
    }
    // Which file this is, so end() removes this file and never one that replaced it.
    struct stat st{};
    if (::stat(path_.c_str(), &st) == 0) {
        written_dev_ = static_cast<unsigned long long>(st.st_dev);
        written_ino_ = static_cast<unsigned long long>(st.st_ino);
        written_known_ = true;
    } else {
        written_known_ = false;
    }
    armed_ = true;
}

void TubeBuildGuard::end() {
    // Only a marker this instance wrote is ours to remove.
    if (!armed_) return;
    armed_ = false;
    // The path must still be a regular file and, when begin() could record it, the very
    // file begin() wrote: a directory or someone else's file put there since is left.
    struct stat st{};
    if (::lstat(path_.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;
    if (written_known_ && (static_cast<unsigned long long>(st.st_dev) != written_dev_ ||
                           static_cast<unsigned long long>(st.st_ino) != written_ino_))
        return;
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

}  // namespace app
