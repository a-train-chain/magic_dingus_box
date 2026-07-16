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
void write_core_options(std::ostream& out, const std::string& core_name);

enum class StartupStatus {
    Ready,
    Exited,
    TimedOut,
    WaitError,
};

struct ReadyWatchOptions {
    std::string ready_file = "/tmp/retroarch_mdb.ready";
    std::string drm_card_pattern = "/dev/dri/card*";
};

std::string build_kms_ready_watch_block(const std::string& command,
                                        const ReadyWatchOptions& options);

StartupStatus wait_for_startup(
    pid_t launcher_pid,
    const std::string& ready_file,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(50));

bool terminate_process_group(pid_t launcher_pid,
                             std::chrono::milliseconds grace);

}  // namespace retroarch
