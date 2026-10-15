#include "status_writer.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <json/json.h>
#include "utils/logger.h"
#include "status_snapshot.h"

namespace app {

StatusWriter::StatusWriter(std::string path)
    : path_(std::move(path)),
      tmp_path_(path_ + ".tmp"),
      owning_thread_(std::this_thread::get_id()) {}

void StatusWriter::write_now(const AppState& state) {
    assert(std::this_thread::get_id() == owning_thread_ &&
           "StatusWriter::write_now called from non-owning thread");
    Json::Value root = build_status_json(state);

    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    // Serialize WITHOUT ts first: this is the change-detection key. The
    // writer used to run at 5Hz unconditionally — ~432k tmp+rename
    // cycles/day of SD metadata churn on a box that idles at a menu for
    // most of its life, with ts guaranteeing every body differed. Now:
    // skip when nothing but the timestamp would change, EXCEPT every 2s
    // as a heartbeat. Both freshness consumers stay honest — verify_box
    // asserts ts < 5s old, and the phone remote's broadcaster polls
    // mtime (it also gets fewer no-op parse wakeups). Real content
    // changes (position ticks during playback, screen transitions, game
    // session begin/end) still write immediately.
    const std::string body = Json::writeString(b, root);
    const auto mono_now = std::chrono::steady_clock::now();
    if (body == last_body_ &&
        mono_now - last_write_ < std::chrono::seconds(2)) {
        return;
    }

    using namespace std::chrono;
    root["ts"] = duration_cast<duration<double>>(
        system_clock::now().time_since_epoch()).count();
    std::string out = Json::writeString(b, root);

    {
        std::ofstream f(tmp_path_, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f.good()) {
            LOG_WARN("StatusWriter: failed to write tmp file {}", tmp_path_);
            std::remove(tmp_path_.c_str());
            return;
        }
    }
    if (std::rename(tmp_path_.c_str(), path_.c_str()) != 0) {
        LOG_WARN("StatusWriter: rename {} -> {} failed (errno {})",
                 tmp_path_, path_, errno);
        std::remove(tmp_path_.c_str());
        return;  // failed write: leave last_body_ stale so the next tick retries
    }
    last_body_ = body;
    last_write_ = mono_now;
}

} // namespace app
