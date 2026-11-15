#include "app/tube_live.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <optional>
#include <string>
#include <vector>
#include <utility>

#include "../utils/logger.h"
#include "app/redraw_gate.h"
#include "app/settings_persistence.h"
#include "app/tube_ops.h"
#include "tube/tube_setting.h"

namespace app {
namespace {

// The file's text, at most kTubeOpsMaxBytes + 1 bytes (one more than the cap,
// so an over-long file reaches the parser as over-long and is refused there).
// nullopt when it cannot be opened or read — the web's replace can leave the
// path missing for an instant; the caller tries again on a later look.
std::optional<std::string> read_capped(const std::string& path) noexcept {
    try {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return std::nullopt;
        std::string text(kTubeOpsMaxBytes + 1, '\0');
        std::size_t got = 0;
        bool ok = true;
        while (got < text.size()) {
            const ssize_t n = ::read(fd, &text[got], text.size() - got);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (n == 0) break;
            got += static_cast<std::size_t>(n);
        }
        ::close(fd);
        if (!ok) return std::nullopt;
        text.resize(got);
        return text;
    } catch (...) {  // the one allocation
        return std::nullopt;
    }
}

}  // namespace

TubeRequestPoller::TubeRequestPoller(std::string path) : path_(std::move(path)) {}

bool TubeRequestPoller::poll(AppState& state, Clock::time_point now) {
    bool changed = false;
    if (!last_look_ || now - *last_look_ >= kPollPeriod) {
        last_look_ = now;
        changed = look(state, now);
    }
    if (save_due_ && now >= *save_due_) {
        save_due_.reset();
        (void)SettingsPersistence::save_settings(state);
    }
    return changed;
}

void TubeRequestPoller::flush(AppState& state) {
    if (!save_due_) return;
    save_due_.reset();
    (void)SettingsPersistence::save_settings(state);
}

bool TubeRequestPoller::look(AppState& state, Clock::time_point now) {
    // One stat: the whole steady-state cost.
    Version version;
    struct stat st {};
    if (::stat(path_.c_str(), &st) == 0) {
        version.exists = true;
        version.inode = static_cast<std::uint64_t>(st.st_ino);
#if defined(__APPLE__)
        const struct timespec& mtime = st.st_mtimespec;  // the Mac's name (tests)
#else
        const struct timespec& mtime = st.st_mtim;
#endif
        version.mtime_ns = static_cast<std::int64_t>(mtime.tv_sec) * 1000000000 + mtime.tv_nsec;
        version.size = static_cast<std::int64_t>(st.st_size);
    } else if (errno != ENOENT && errno != ENOTDIR) {
        return false;  // cannot tell (permissions, I/O): look again later
    }
    if (seen_ && *seen_ == version) return false;

    std::optional<std::vector<TubeOp>> ops;
    if (version.exists) {
        const std::optional<std::string> text = read_capped(path_);
        if (!text) return false;  // gone or unreadable just now: next look retries
        TubeOpsParse parsed = parse_tube_ops(*text);
        if (parsed.ops) {
            ops = std::move(parsed.ops);
        } else {
            // Once per version (a version is read once). Log only: the reason
            // carries jsoncpp's wording, never for the TV.
            LOG_WARN("tube_request.json ignored: {}", parsed.error);
        }
    }
    seen_ = version;

    if (!state.tube_live.adopted) {
        adopt_tube_ops(ops ? &*ops : nullptr, state.tube_live);
        return false;
    }
    if (!ops) return false;
    const TubeOpsResult result =
        apply_tube_ops(*ops, state.display_settings, state.tube_live, tube_offered(state));
    if (result.changed) save_due_ = now + kSaveQuiet;
    return result.changed;
}

void add_tube_redraw_signature(ContentSignature& sig, const AppState& state) {
    // Outside 0..100 and every enum's range: "not overridden".
    constexpr std::uint64_t kUnset = ~std::uint64_t{0};
    const tube::TubeSetting& t = state.display_settings.tube;
    sig.add(static_cast<std::uint64_t>(state.display_settings.crt_engine));
    sig.add(static_cast<std::uint64_t>(t.look));
    sig.add(t.mask ? static_cast<std::uint64_t>(*t.mask) : kUnset);
    for (const std::optional<int>& row : t.rows) {
        sig.add(row ? static_cast<std::uint64_t>(*row) : kUnset);
    }
    sig.add(state.tube_live.degauss_requests);
}

}  // namespace app
