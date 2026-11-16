#pragma once

// The kiosk's side of live Tube tuning: watches the web admin's op log
// (data/tube_request.json, app/tube_ops.h), applies new ops to AppState and
// saves settings.json once the changes stop.
//
// It runs on the render loop, which pings a 10 s systemd watchdog, and an SD
// card saturated by an upload can make one file operation take seconds. So:
//   - where Tube is not offered (app::tube_offered: every Pi 4B, and a Pi 5
//     under MDB_PLATFORM_POLICY_OVERRIDE=pi4) poll() returns at once and
//     touches nothing;
//   - otherwise the steady state is ONE ::stat every kPollPeriod and nothing else;
//   - the file is read only when its version (inode, mtime, size) changed.
//     The web replaces it with os.replace, whose new inode differs from the
//     file it REPLACED — but the poller compares with the version it last READ,
//     which can be two replaces back, so an inode can in theory come round
//     again. With the size and the nanosecond mtime also in the key a miss is
//     negligible, and the next write recovers it;
//   - anything at the path that is not a regular file (a directory, a FIFO) is
//     never opened: warned once per version and treated as no file. The open
//     is O_NONBLOCK as well, so a FIFO slipped in after the stat cannot block;
//   - the read is bounded by kTubeOpsMaxBytes before anything is allocated;
//   - the op log is never written, renamed, truncated or deleted here;
//   - settings are saved only through SettingsPersistence::save_settings
//     (serialize here, write on its worker), kSaveQuiet after the last change;
//   - nothing throws: POSIX calls and std::error_code overloads only.
//
// A read can race the web's replace (the new bytes under the old version key,
// or ENOENT for an instant): neither loses an op — the next stat sees a new key
// and reads again, and ops at or below the handled seq are skipped.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "app/app_state.h"

namespace app {

class ContentSignature;

class TubeRequestPoller {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kPollPeriod{100};
    static constexpr std::chrono::seconds kSaveQuiet{1};

    explicit TubeRequestPoller(std::string path);

    // Not offered (app::tube_offered false): returns false and does nothing.
    // Otherwise every call: saves when a debounced save is due. At most every
    // kPollPeriod: stats the log, and on the first look or a new version reads
    // and parses it. The first look since boot adopts the file (marks its ops
    // handled, applies none); later looks apply the new ops. Returns true when
    // the engine or the tube setting changed (the open Settings menu should
    // rebuild its rows).
    bool poll(AppState& state, Clock::time_point now);

    // Saves now if a save is armed (shutdown).
    void flush(AppState& state);

    // settings.json was just replaced and reloaded (the web admin's restore):
    // a save armed before it must not follow it.
    void forget_pending_save() noexcept { save_due_.reset(); }

    // How many times the log was opened for reading (tests: proves a look
    // that must not read, did not).
    std::uint64_t reads() const noexcept { return reads_; }

private:
    // What identifies one version of the file. `exists` false: no file.
    struct Version {
        bool exists = false;
        std::uint64_t inode = 0;
        std::int64_t mtime_ns = 0;
        std::int64_t size = 0;
        bool operator==(const Version& o) const {
            return exists == o.exists && inode == o.inode && mtime_ns == o.mtime_ns && size == o.size;
        }
        bool operator!=(const Version& o) const { return !(*this == o); }
    };

    bool look(AppState& state, Clock::time_point now);

    std::string path_;
    std::optional<Clock::time_point> last_look_;
    std::optional<Version> seen_;            // the version last read (or found absent)
    std::optional<Clock::time_point> save_due_;
    std::uint64_t reads_ = 0;
};

// The tube state the screen shows (engine, look, mask and row overrides,
// Degauss presses) for the main loop's redraw gate.
void add_tube_redraw_signature(ContentSignature& sig, const AppState& state);

}  // namespace app
