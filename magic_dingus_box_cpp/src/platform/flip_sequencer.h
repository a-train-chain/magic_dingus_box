#pragma once

// The order in which FramePresenter hands frames to the kernel: SetCrtc or
// page flip, when to wait, and when to drop a frame. Pure, header-only, no
// DRM types — the kernel is reached through FlipBackend, so the whole
// sequence runs on the Mac against a fake kernel
// (tests/platform/test_flip_sequencer.cpp).
//
// THE BUG THIS REPLACES (on every box until 2026-11): the presenter passed
// one bool as the page flip's user data, waited up to 100 ms for "an
// event", and on a timeout carried on with the flip still pending. Two
// things then went wrong, for good:
//   - the next frame's flip was refused with EBUSY (the kernel takes one
//     nonblocking commit per CRTC at a time) and was shown with a blocking
//     SetCrtc instead;
//   - the late event stayed in the DRM fd's queue, and the next flip's wait
//     took it for its own completion and returned at once — with THAT flip
//     still pending, so the frame after it was EBUSY again.
// One timed-out wait therefore left the loop one event behind until the
// process restarted: every other frame EBUSY + SetCrtc (owner's Pi 5,
// 2026-11-14/15: about 110,000 EBUSY an hour for ten hours after one
// timeout; the trigger was the Media Browser containers starting again
// after a game).
//
// THE RULES NOW:
//   1. Every flip carries its own token. An event completes a flip only
//      when it carries that flip's token; any other event is stale and is
//      counted, never acted on. Tokens are never reused, so an event left
//      over from before a game (reset()) cannot complete a later flip.
//   2. No flip is requested while one of ours is outstanding. Queued
//      events are taken in first (without waiting); if the flip is still
//      out, the frame waits for it, and if it is still out when the
//      frame's wait budget ends the frame is DROPPED: the picture on
//      screen stays, and the caller releases the buffer it just rendered.
//   3. One budget per frame: waiting for an earlier flip and waiting for
//      this frame's flip together never exceed kFrameWaitBudgetMs — the
//      main loop pings a 10 s systemd watchdog, and this is the same
//      100 ms bound the old single wait had.
//   4. EBUSY from the kernel (a commit we do not know about is pending)
//      drops the frame. It is not answered with SetCrtc.
//   5. Any other flip error is answered with a blocking SetCrtc in the
//      same frame. This is the path a game's return depends on: the first
//      flip after RetroArch hands the display back fails with EINVAL, and
//      the SetCrtc puts the kiosk's picture back.
//   6. SetCrtc is also used for the first frame, for
//      kSetCrtcFramesAfterReset frames after reset(), and as the last
//      resort when a flip has been outstanding (or the kernel has said
//      EBUSY) for kLostFlipMs — a lost event must not freeze the picture.
//      A SetCrtc that succeeds is on screen when it returns, so it
//      supersedes an outstanding flip: that flip's event, when it comes,
//      is stale.
//
// Trouble is reported as ONE line per episode (take_summary + describe),
// plus one line every kOngoingSummaryMs while an episode lasts. Nothing
// here prints.

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>

namespace platform {

// What the sequencer needs from the kernel, for ONE frame (the framebuffer
// to show is the backend's business). Errors are errno values, 0 = ok.
class FlipBackend {
public:
    using EventFn = std::function<void(std::uint64_t token)>;

    virtual ~FlipBackend() = default;

    // Monotonic milliseconds.
    virtual std::int64_t now_ms() = 0;

    // Calls on_event(token) for every page-flip event queued on the DRM
    // fd. With none queued, waits up to timeout_ms for the first one
    // (0 = do not wait). Returns false when the fd cannot be waited on or
    // read — the caller then stops waiting instead of spinning.
    virtual bool pump_events(int timeout_ms, const EventFn& on_event) = 0;

    // Nonblocking page flip to this frame, with an event carrying `token`.
    virtual int page_flip(std::uint64_t token) = 0;

    // Blocking SetCrtc to this frame; on screen when it returns 0.
    virtual int set_crtc() = 0;
};

enum class FrameResult {
    ShownBySetCrtc,  // on screen now; every buffer held before is free
    FlipCompleted,   // flipped and on screen
    FlipPending,     // flip requested, not completed inside the budget
    Dropped,         // never reached the kernel; release this frame's buffer
};

struct FrameReport {
    FrameResult result = FrameResult::Dropped;
    // The flip a PREVIOUS frame left pending completed during this call:
    // its buffer is on screen now. Apply this before `result`.
    bool earlier_flip_completed = false;
};

struct FlipTrouble {
    int wait_timeouts = 0;      // a flip did not complete inside its frame
    int dropped_frames = 0;     // frames that never reached the screen
    int busy_rejections = 0;    // EBUSY from the kernel
    int flip_errors = 0;        // any other page-flip error
    int setcrtc_fallbacks = 0;  // SetCrtc that showed a frame after one
    int resyncs = 0;            // SetCrtc after a lost flip / lasting EBUSY
    int setcrtc_failures = 0;
    int stale_events = 0;       // events that were not the outstanding flip's
    int last_errno = 0;         // of the last flip error or SetCrtc failure
};

struct FlipSummary {
    bool recovered = false;      // false: the episode is still going
    std::int64_t duration_ms = 0;
    FlipTrouble trouble;
};

class FlipSequencer {
public:
    // Total time one present() may spend waiting for flip events.
    static constexpr std::int64_t kFrameWaitBudgetMs = 100;
    // A flip outstanding (or EBUSY answered) this long is given up on and
    // the display is re-set with SetCrtc. Far above the frame budget (a
    // flip that is merely late is waited for, not fought), far below the
    // 10 s watchdog.
    static constexpr std::int64_t kLostFlipMs = 2000;
    // RetroArch owned the CRTC in between; a bare flip onto it is not
    // trustworthy for the first frames.
    static constexpr int kSetCrtcFramesAfterReset = 10;
    static constexpr std::int64_t kOngoingSummaryMs = 10000;

    FrameReport present(FlipBackend& be) {
        FrameReport rep;
        const std::int64_t start = be.now_ms();
        const std::int64_t deadline = start + kFrameWaitBudgetMs;

        // Whatever already completed, before any decision (never waits).
        const bool was_outstanding = outstanding_.has_value();
        be.pump_events(0, [this](std::uint64_t t) { take_event(t); });
        rep.earlier_flip_completed = was_outstanding && !outstanding_;

        if (first_frame_ || setcrtc_frames_left_ > 0) {
            rep.result = set_crtc(be, SetCrtcWhy::Planned);
            return rep;
        }

        if (outstanding_) {
            wait_for_outstanding(be, deadline);
            rep.earlier_flip_completed = !outstanding_;
        }

        if (outstanding_) {
            const std::int64_t now = be.now_ms();
            if (now - outstanding_since_ < kLostFlipMs) {
                open_episode(now);
                ++counts_.dropped_frames;
                rep.result = FrameResult::Dropped;
                return rep;
            }
            rep.result = set_crtc(be, SetCrtcWhy::Resync);
            return rep;
        }

        if (busy_since_ && be.now_ms() - *busy_since_ >= kLostFlipMs) {
            rep.result = set_crtc(be, SetCrtcWhy::Resync);
            return rep;
        }

        const std::uint64_t token = next_token_++;
        const int err = be.page_flip(token);
        const std::int64_t flipped_at = be.now_ms();
        if (err == EBUSY) {
            open_episode(flipped_at);
            ++counts_.busy_rejections;
            ++counts_.dropped_frames;
            if (!busy_since_) busy_since_ = flipped_at;
            rep.result = FrameResult::Dropped;
            return rep;
        }
        if (err != 0) {
            open_episode(flipped_at);
            ++counts_.flip_errors;
            counts_.last_errno = err;
            rep.result = set_crtc(be, SetCrtcWhy::Fallback);
            return rep;
        }

        busy_since_.reset();
        outstanding_ = token;
        outstanding_since_ = flipped_at;
        wait_for_outstanding(be, deadline);

        const std::int64_t now = be.now_ms();
        if (outstanding_) {
            open_episode(now);
            ++counts_.wait_timeouts;
            rep.result = FrameResult::FlipPending;
        } else {
            close_episode(now);
            rep.result = FrameResult::FlipCompleted;
        }
        return rep;
    }

    // After a game returned the display. Anything outstanding from before
    // is void (its event, if it still comes, is stale); the next frames
    // are SetCrtc'd. An open trouble episode stays open.
    void reset() {
        outstanding_.reset();
        busy_since_.reset();
        first_frame_ = true;
        setcrtc_frames_left_ = kSetCrtcFramesAfterReset;
    }

    bool flip_outstanding() const { return outstanding_.has_value(); }
    bool awaiting_first_frame() const { return first_frame_; }

    // The line to log, when there is one: once when an episode ends, and
    // every kOngoingSummaryMs while one lasts.
    std::optional<FlipSummary> take_summary(std::int64_t now) {
        if (ready_) {
            FlipSummary s = *ready_;
            ready_.reset();
            return s;
        }
        if (episode_since_ && now - last_report_ >= kOngoingSummaryMs) {
            FlipSummary s{false, now - *episode_since_, counts_};
            counts_ = {};
            last_report_ = now;
            return s;
        }
        return std::nullopt;
    }

private:
    enum class SetCrtcWhy { Planned, Fallback, Resync };

    void take_event(std::uint64_t token) {
        if (outstanding_ && token == *outstanding_) {
            outstanding_.reset();
        } else {
            ++counts_.stale_events;
        }
    }

    void wait_for_outstanding(FlipBackend& be, std::int64_t deadline) {
        while (outstanding_) {
            const std::int64_t left = deadline - be.now_ms();
            if (left <= 0) break;
            if (!be.pump_events(static_cast<int>(left),
                                [this](std::uint64_t t) { take_event(t); })) {
                break;
            }
        }
    }

    FrameResult set_crtc(FlipBackend& be, SetCrtcWhy why) {
        const int err = be.set_crtc();
        const std::int64_t now = be.now_ms();
        if (err != 0) {
            open_episode(now);
            ++counts_.setcrtc_failures;
            ++counts_.dropped_frames;
            counts_.last_errno = err;
            return FrameResult::Dropped;
        }
        first_frame_ = false;
        if (setcrtc_frames_left_ > 0) --setcrtc_frames_left_;
        // On screen now, so it replaced whatever flip was pending.
        outstanding_.reset();
        busy_since_.reset();
        switch (why) {
            case SetCrtcWhy::Planned:
                close_episode(now);
                break;
            case SetCrtcWhy::Fallback:
                ++counts_.setcrtc_fallbacks;
                break;
            case SetCrtcWhy::Resync:
                open_episode(now);
                ++counts_.resyncs;
                break;
        }
        return FrameResult::ShownBySetCrtc;
    }

    void open_episode(std::int64_t now) {
        if (!episode_since_) {
            episode_since_ = now;
            last_report_ = now;
        }
    }

    void close_episode(std::int64_t now) {
        if (!episode_since_) return;
        ready_ = FlipSummary{true, now - *episode_since_, counts_};
        counts_ = {};
        episode_since_.reset();
    }

    std::optional<std::uint64_t> outstanding_;  // token of our pending flip
    std::int64_t outstanding_since_ = 0;
    std::optional<std::int64_t> busy_since_;    // first EBUSY of a streak
    std::uint64_t next_token_ = 1;
    bool first_frame_ = true;
    int setcrtc_frames_left_ = 0;

    FlipTrouble counts_;
    std::optional<std::int64_t> episode_since_;
    std::int64_t last_report_ = 0;
    std::optional<FlipSummary> ready_;
};

// Which rendered buffers the presenter still owns, driven by the
// FrameReport of each present(): `scanout` is on screen, `pending` is the
// target of a flip the kernel has not completed. Both are in the kernel's
// hands and must not be given back to the GBM surface — a released buffer
// is the next thing EGL renders into, on screen or not. Everything else is
// released the moment it is known to be off screen:
//   - a dropped frame at once (it never reached the kernel);
//   - the old scanout when the flip that replaces it COMPLETES (not when
//     it is requested);
//   - both, when a blocking SetCrtc put a new frame on screen.
// So at most two buffers are held between frames, three while one is
// being presented (a GBM surface has four).
template <typename Buffer>
class ScanoutBuffers {
public:
    using ReleaseFn = std::function<void(const Buffer&)>;

    void apply(const FrameReport& rep, const Buffer& frame,
               const ReleaseFn& release) {
        if (rep.earlier_flip_completed) promote(release);
        switch (rep.result) {
            case FrameResult::ShownBySetCrtc:
                release_all(release);
                scanout_ = frame;
                break;
            case FrameResult::FlipCompleted:
                set_pending(frame, release);
                promote(release);
                break;
            case FrameResult::FlipPending:
                set_pending(frame, release);
                break;
            case FrameResult::Dropped:
                release(frame);
                break;
        }
    }

    // Display reset / shutdown: nothing of ours is (or stays) on screen.
    void release_all(const ReleaseFn& release) {
        if (pending_) release(*pending_);
        if (scanout_) release(*scanout_);
        pending_.reset();
        scanout_.reset();
    }

    const std::optional<Buffer>& scanout() const { return scanout_; }
    const std::optional<Buffer>& pending() const { return pending_; }

private:
    void set_pending(const Buffer& frame, const ReleaseFn& release) {
        // The sequencer never flips on top of an outstanding flip, so this
        // slot is empty here; if it ever is not, do not leak the buffer.
        if (pending_) release(*pending_);
        pending_ = frame;
    }

    void promote(const ReleaseFn& release) {
        if (!pending_) return;
        if (scanout_) release(*scanout_);
        scanout_ = pending_;
        pending_.reset();
    }

    std::optional<Buffer> scanout_;
    std::optional<Buffer> pending_;
};

// The journal line for a summary. Only what happened is listed.
inline std::string describe(const FlipSummary& s) {
    std::string out = s.recovered ? "Page flip recovered after "
                                  : "Page flip trouble continuing, ";
    out += std::to_string(s.duration_ms) + " ms";
    if (!s.recovered) out += " so far";
    out += ":";

    const FlipTrouble& t = s.trouble;
    bool any = false;
    const auto item = [&](int n, const char* what) {
        if (n <= 0) return;
        out += any ? ", " : " ";
        out += std::to_string(n) + " " + what;
        any = true;
    };
    item(t.wait_timeouts, "flip wait(s) timed out");
    item(t.dropped_frames, "frame(s) dropped");
    item(t.busy_rejections, "EBUSY");
    item(t.flip_errors, "flip error(s)");
    item(t.setcrtc_fallbacks, "shown by SetCrtc instead");
    item(t.resyncs, "SetCrtc resync(s)");
    item(t.setcrtc_failures, "SetCrtc failure(s)");
    item(t.stale_events, "stale event(s) ignored");
    if (!any) out += " nothing counted";
    if (t.last_errno != 0) {
        out += " (last errno " + std::to_string(t.last_errno) + ": " +
               std::strerror(t.last_errno) + ")";
    }
    return out;
}

}  // namespace platform
