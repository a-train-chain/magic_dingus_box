#pragma once
#include <chrono>
#include <string>
#include <thread>
#include "app_state.h"

namespace app {

class StatusWriter {
public:
    explicit StatusWriter(std::string path);

    /// Writes the current state to disk atomically (temp + rename).
    /// THREAD-SAFETY: must be called from the same thread that constructed
    /// the StatusWriter (typically the main loop). Reads several
    /// non-atomic AppState fields without a lock; calling from a second
    /// thread is a data race.
    void write_now(const AppState& state);

private:
    std::string path_;
    std::string tmp_path_;
    std::thread::id owning_thread_;
    // Change-detection + heartbeat (see write_now): the serialized body
    // (EXCLUDING the ts stamp) of the last successful write, and when it
    // happened. Both touched only on the owning thread.
    std::string last_body_;
    std::chrono::steady_clock::time_point last_write_{};
};

} // namespace app
