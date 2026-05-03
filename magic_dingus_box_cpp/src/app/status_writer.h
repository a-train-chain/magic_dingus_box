#pragma once
#include <string>
#include "app_state.h"

namespace app {

class StatusWriter {
public:
    explicit StatusWriter(std::string path);
    // Writes the current state to disk atomically (temp + rename).
    void write_now(const AppState& state);

private:
    std::string path_;
    std::string tmp_path_;
};

} // namespace app
