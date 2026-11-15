#include "app/tube_build_guard.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "../utils/logger.h"

namespace app {

namespace {
constexpr std::size_t kMaxMarkerBytes = 4096;
constexpr const char* kUnknownProgram = "unknown";
}  // namespace

TubeBuildGuard::TubeBuildGuard(std::string marker_path) : path_(std::move(marker_path)) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path_, ec);
    // "Not found" is the clean case. Any other answer, including an error stat could
    // not explain, is treated as a marker: the cost of a false trip is one boot on the
    // old engine, the cost of a miss is a second hang.
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == std::filesystem::file_type::not_found))
        return;
    tripped_ = true;
    if (!ec && status.type() == std::filesystem::file_type::regular) {
        std::ifstream in(path_, std::ios::binary);
        if (in) {
            std::string text(kMaxMarkerBytes, '\0');
            in.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<std::size_t>(in.gcount()));
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            program_ = std::move(text);
        }
    }
    if (program_.empty()) program_ = kUnknownProgram;
}

void TubeBuildGuard::begin(std::string_view programs) {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (out) {
        out.write(programs.data(), static_cast<std::streamsize>(programs.size()));
        out.flush();
        out.close();
    }
    if (!out) LOG_WARN("tube build guard: could not write {}; building without a marker", path_);
}

void TubeBuildGuard::end() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

}  // namespace app
