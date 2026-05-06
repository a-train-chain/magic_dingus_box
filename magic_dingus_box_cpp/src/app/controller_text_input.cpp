// controller_text_input.cpp
// Implements Controller::poll_text_input_queue and set_text_input_queue_path.
// Lives in a separate TU so the phone-remote unit-test target can link it
// without dragging in GStreamer / DRM / RetroArch dependencies.

#include "controller.h"
#include "../ui/virtual_keyboard.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <json/json.h>

namespace fs = std::filesystem;

namespace app {

void Controller::set_text_input_queue_path(std::string path) {
    text_input_queue_path_ = std::move(path);
}

void Controller::poll_text_input_queue(AppState& state) {
    if (text_input_queue_path_.empty()) return;

    std::error_code ec;
    auto sz = fs::file_size(text_input_queue_path_, ec);
    if (ec || sz == 0) return;  // idle fast path — no allocation

    // Acquire exclusive lock for the read+truncate window.
    int fd = ::open(text_input_queue_path_.c_str(), O_RDWR);
    if (fd < 0) return;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return;
    }

    // Read entire file into memory under lock.
    std::string contents;
    {
        std::ifstream f(text_input_queue_path_);
        std::ostringstream ss;
        ss << f.rdbuf();
        contents = ss.str();
    }

    // Truncate the file (we own the exclusive lock).
    if (::ftruncate(fd, 0) != 0) {
        // best-effort — events still parsed below even if truncate fails
    }
    ::flock(fd, LOCK_UN);
    ::close(fd);

    // Parse and dispatch each JSON-line.
    auto* kb = state.active_text_keyboard;
    std::istringstream ss(contents);
    std::string line;
    Json::CharReaderBuilder reader_builder;
    auto reader = std::unique_ptr<Json::CharReader>(reader_builder.newCharReader());

    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        Json::Value ev;
        std::string err;
        if (!reader->parse(line.data(), line.data() + line.size(), &ev, &err)) {
            continue;  // malformed line — skip, queue already truncated
        }
        if (kb == nullptr) continue;  // no receiver — drop silently

        const std::string t = ev.get("t", "").asString();
        if (t == "type_char") {
            const std::string c = ev.get("c", "").asString();
            if (c.size() == 1) kb->type_char(c[0]);
        } else if (t == "key_special") {
            const std::string k = ev.get("k", "").asString();
            if      (k == "backspace") kb->backspace();
            else if (k == "enter")     kb->commit();
            else if (k == "cancel")    kb->close();
        } else if (t == "clear") {
            kb->clear_buffer();
        }
        // Unknown event types are silently ignored (forward-compatible).
    }
}

} // namespace app
