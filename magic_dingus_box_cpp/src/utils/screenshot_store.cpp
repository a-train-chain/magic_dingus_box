#include "utils/screenshot_store.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace utils {

namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string screenshot_filename(std::chrono::system_clock::time_point t) {
    using namespace std::chrono;
    const auto since_epoch = t.time_since_epoch();
    auto secs = duration_cast<seconds>(since_epoch);
    auto ms = duration_cast<milliseconds>(since_epoch - secs).count();
    if (ms < 0) {  // pre-epoch: floor, keep ms in [0, 999]
        secs -= seconds(1);
        ms += 1000;
    }
    const std::time_t tt = static_cast<std::time_t>(secs.count());
    std::tm tm{};
    if (gmtime_r(&tt, &tm) == nullptr) return "";
    char date[32];
    if (std::strftime(date, sizeof(date), "%Y%m%dT%H%M%S", &tm) == 0) return "";
    char out[48];
    std::snprintf(out, sizeof(out), "%s_%03dZ.bmp", date, static_cast<int>(ms));
    return out;
}

bool write_file_atomic(const std::string& path,
                       const std::vector<std::uint8_t>& bytes,
                       std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) return fail("create_directories: " + ec.message());
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return fail("cannot open " + tmp);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return fail("write failed: " + tmp);
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        return fail("rename: " + ec.message());
    }
    return true;
}

std::size_t prune_screenshots(const std::string& dir, std::size_t keep) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;

    std::vector<fs::path> shots;
    std::vector<fs::path> leftovers;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec)) continue;
        const std::string name = it->path().filename().string();
        if (ends_with(name, ".bmp")) {
            shots.push_back(it->path());
        } else if (ends_with(name, ".bmp.tmp")) {
            leftovers.push_back(it->path());
        }
    }

    std::sort(shots.begin(), shots.end(),
              [](const fs::path& a, const fs::path& b) {
                  return a.filename().string() > b.filename().string();
              });
    std::size_t removed = 0;
    for (std::size_t i = keep; i < shots.size(); ++i) {
        std::error_code rm_ec;
        if (fs::remove(shots[i], rm_ec)) ++removed;
    }
    for (const auto& p : leftovers) {
        std::error_code rm_ec;
        if (fs::remove(p, rm_ec)) ++removed;
    }
    return removed;
}

}  // namespace utils
