// controller_text_input.cpp
// Implements Controller::poll_text_input_queue and set_text_input_queue_path.
// Lives in a separate TU so the phone-remote unit-test target can link it
// without dragging in GStreamer / DRM / RetroArch dependencies.

#include "controller.h"
#include "../ui/virtual_keyboard.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <sstream>
#include <json/json.h>

namespace fs = std::filesystem;

namespace app {

namespace {

// Lock, read and truncate the queue file. Returns the bytes taken, or ""
// when there were none or any step failed.
//
// WORKER THREAD ONLY (TextQueueDrainer). Every call in here can block on
// the SD card or on the web side: flock waits for the web admin's writer,
// which holds the lock across its own write + flush, and ftruncate is a
// journaled file write. None of it may run on the render loop, which pings
// the systemd watchdog.
std::string drain_text_queue_file(const std::string& path) {
    // O_CLOEXEC: this runs beside the main thread, which fork/execs
    // RetroArch and the helper commands; none of them should inherit a
    // descriptor that may be holding the queue lock.
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return {};
    // Blocking on purpose: on this thread waiting is free, and it means the
    // web side's writer and this reader simply take turns — no polling, no
    // starvation in either direction.
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return {};
    }

    // Read entire file into memory via the locked fd.
    // flock(2) is per-open-file-description on Linux: opening a second fd
    // (e.g. via std::ifstream) would have no lock, so we read from the same
    // fd we already locked.
    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        // Already empty (an earlier drain took it): nothing to read, and
        // nothing to truncate — no file write at all.
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return {};
    }
    ::lseek(fd, 0, SEEK_SET);

    std::string contents(static_cast<size_t>(size), '\0');
    ssize_t total = 0;
    while (total < size) {
        ssize_t n = ::read(fd, contents.data() + total,
                           static_cast<size_t>(size - total));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::flock(fd, LOCK_UN);
            ::close(fd);
            return {};  // read failed — skip this batch entirely
        }
        if (n == 0) break;  // EOF earlier than expected; truncate buffer below
        total += n;
    }
    contents.resize(static_cast<size_t>(total));

    // Truncate the file (we own the exclusive lock). If truncate fails,
    // return NOTHING — the bytes are still in the file and the next drain
    // would deliver them a second time. Better to skip a batch than to
    // deliver duplicate keystrokes.
    if (::ftruncate(fd, 0) != 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return {};
    }
    ::flock(fd, LOCK_UN);
    ::close(fd);
    return contents;
}

// Parse and dispatch each JSON line to the active keyboard.
void dispatch_text_events(const std::string& contents, ui::VirtualKeyboard* kb) {
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

}  // namespace

void Controller::set_text_input_queue_path(std::string path) {
    text_input_queue_path_ = std::move(path);
}

void Controller::poll_text_input_queue(AppState& state) {
    if (text_input_queue_path_.empty()) return;

    // 1. Deliver what the worker has drained since the last frame. One
    //    uncontended mutex lock; no allocation when nothing arrived.
    const std::string drained = text_drainer_.take();
    if (!drained.empty()) {
        dispatch_text_events(drained, state.active_text_keyboard);
    }

    // 2. Anything (more) queued? A stat only — the idle fast path.
    std::error_code ec;
    auto sz = fs::file_size(text_input_queue_path_, ec);
    if (ec || sz == 0) return;

    // 3. Hand the lock/read/truncate to the worker (a no-op while a drain
    //    is already under way; the file is still non-empty next frame if
    //    that drain missed something).
    text_drainer_.request(
        text_input_queue_path_,
        text_drain_fn_ ? text_drain_fn_
                       : TextQueueDrainer::DrainFn(&drain_text_queue_file));
}

} // namespace app
