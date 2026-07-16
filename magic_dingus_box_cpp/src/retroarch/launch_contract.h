#pragma once

#include <chrono>
#include <iosfwd>
#include <string>
#include <sys/types.h>

#include "app/app_state.h"

namespace retroarch {

struct LaunchOptions {
    app::DisplayMode display_mode = app::DisplayMode::CRT_NATIVE;
    std::string bezel_file;
};

void write_video_config(std::ostream& out, const LaunchOptions& options);

}  // namespace retroarch
