// platform::FlipSequencer + ScanoutBuffers — the order in which
// FramePresenter hands frames to the kernel, and which buffers it still
// owns. Pure logic; the kernel here is FakeKernel, which keeps the three
// rules of the real one that matter:
//   - one nonblocking flip per CRTC at a time: a flip requested while one
//     is pending is refused with EBUSY;
//   - a flip completes at a vblank, and its event then waits in a queue
//     until someone reads it — however late that is;
//   - SetCrtc blocks until the pending flip is done and the new frame is
//     on screen, and sends no event.
//
// The bug these pin (every box until 2026-11): after ONE flip wait timed
// out, the presenter requested the next flip on top of the pending one
// (EBUSY, shown by SetCrtc instead), and the late event then satisfied the
// following flip's wait at once — so that flip was still pending when the
// frame after it was requested. EBUSY on every other frame until the
// process restarted. "the old loop" below is that code against the same
// fake kernel, so the model is shown to reproduce the field failure before
// the new sequencer is shown not to.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "platform/flip_sequencer.h"

using platform::FlipBackend;
using platform::FlipSequencer;
using platform::FlipSummary;
using platform::FrameReport;
using platform::FrameResult;
using platform::ScanoutBuffers;

namespace {

struct FakeKernel : FlipBackend {
    static constexpr std::int64_t kVblank = 16;

    std::int64_t now = 1000;
    int frame = -1;   // the frame the current present() is showing
    int screen = -1;  // the frame on screen

    struct Pending {
        std::uint64_t token;
        int frame;
        std::int64_t done_at;
        bool lose_event;
    };
    std::optional<Pending> pending;
    std::deque<std::uint64_t> events;

    // Knobs, each applying to the next call it concerns.
    std::int64_t stall_next_flip_ms = 0;  // completes this much late
    bool lose_next_event = false;         // completes, but no event
    int fail_next_flip = 0;               // errno, once
    int fail_setcrtc = 0;                 // errno, while non-zero
    std::int64_t foreign_busy_until = 0;  // a commit that is not ours
    std::vector<std::uint64_t> queue_with_next_flip;  // stale events
    bool fd_broken = false;               // the DRM fd cannot be read
    int pump_calls = 0;

    int flips_accepted = 0;
    int ebusy = 0;
    int setcrtcs = 0;
    std::uint64_t last_token = 0;
    std::int64_t waited_ms = 0;  // time spent inside pump_events

    static std::int64_t next_vblank(std::int64_t t) {
        return (t / kVblank + 1) * kVblank;
    }

    void settle() {
        if (pending && pending->done_at <= now) {
            screen = pending->frame;
            if (!pending->lose_event) events.push_back(pending->token);
            pending.reset();
        }
    }

    void advance(std::int64_t ms) {
        now += ms;
        settle();
    }

    std::int64_t now_ms() override { return now; }

    bool pump_events(int timeout_ms, const EventFn& on_event) override {
        ++pump_calls;
        if (fd_broken) return false;  // at once, no time passes
        settle();
        if (events.empty() && timeout_ms > 0) {
            const std::int64_t before = now;
            if (pending && !pending->lose_event &&
                pending->done_at <= now + timeout_ms) {
                now = std::max(now, pending->done_at);
            } else {
                now += timeout_ms;
            }
            waited_ms += now - before;
            settle();
        }
        while (!events.empty()) {
            const std::uint64_t t = events.front();
            events.pop_front();
            on_event(t);
        }
        return true;
    }

    int page_flip(std::uint64_t token) override {
        settle();
        if (fail_next_flip != 0) {
            const int e = fail_next_flip;
            fail_next_flip = 0;
            return e;
        }
        if (pending || now < foreign_busy_until) {
            ++ebusy;
            return EBUSY;
        }
        pending = Pending{token, frame,
                          next_vblank(now) + stall_next_flip_ms,
                          lose_next_event};
        stall_next_flip_ms = 0;
        lose_next_event = false;
        for (std::uint64_t t : queue_with_next_flip) events.push_back(t);
        queue_with_next_flip.clear();
        last_token = token;
        ++flips_accepted;
        return 0;
    }

    int set_crtc() override {
        settle();
        if (fail_setcrtc != 0) return fail_setcrtc;
        if (pending) {  // a blocking commit waits behind the pending one
            now = std::max(now, pending->done_at);
            settle();
        }
        foreign_busy_until = 0;
        now = next_vblank(now);
        screen = frame;
        ++setcrtcs;
        return 0;
    }
};

// One kiosk: kernel + sequencer + the presenter's buffer bookkeeping, with
// the invariants checked on every frame.
struct Rig {
    FakeKernel k;
    FlipSequencer seq;
    ScanoutBuffers<int> held;
    int next_frame = 0;
    int released = 0;
    std::vector<std::string> log;
    std::vector<FrameResult> results;

    ScanoutBuffers<int>::ReleaseFn release() {
        return [this](const int& f) {
            // Never give back a buffer the kernel is showing or about to.
            CHECK(f != k.screen);
            if (k.pending) CHECK(f != k.pending->frame);
            ++released;
        };
    }

    FrameReport frame(std::int64_t render_ms = 4) {
        k.advance(render_ms);
        k.frame = next_frame++;
        k.waited_ms = 0;
        const FrameReport r = seq.present(k);
        // The wait for an earlier flip and for this one share one budget.
        CHECK(k.waited_ms <= FlipSequencer::kFrameWaitBudgetMs);
        held.apply(r, k.frame, release());
        // No leak: every frame ever rendered is either held or released.
        const int holding = (held.scanout() ? 1 : 0) + (held.pending() ? 1 : 0);
        CHECK(released + holding == next_frame);
        // The books match the sequencer's.
        CHECK(held.pending().has_value() == seq.flip_outstanding());
        if (auto s = seq.take_summary(k.now)) log.push_back(describe(*s));
        results.push_back(r.result);
        return r;
    }

    void frames(int n, std::int64_t render_ms = 4) {
        for (int i = 0; i < n; ++i) frame(render_ms);
    }

    // A game took the display and gave it back. A flip still pending when
    // it started completed long ago; its event sat in our queue meanwhile.
    void game_handoff() {
        if (k.pending) k.advance(k.pending->done_at - k.now);
        k.advance(60 * 1000);
        k.screen = -1;  // RetroArch's picture, none of ours
        seq.reset();
        held.release_all(release());
    }

    bool last_n_completed(int n) const {
        if (static_cast<int>(results.size()) < n) return false;
        return std::all_of(results.end() - n, results.end(), [](FrameResult r) {
            return r == FrameResult::FlipCompleted;
        });
    }
};

// The presenter's flip path before the fix: one bool as the event's user
// data, any event ends the wait, a timed-out wait is simply left behind,
// and a refused flip is shown with SetCrtc.
struct OldLoop {
    bool waiting = false;
    void present(FakeKernel& k) {
        waiting = true;
        if (k.page_flip(1) != 0) {
            k.set_crtc();
            return;
        }
        k.pump_events(100, [this](std::uint64_t) { waiting = false; });
    }
};

}  // namespace

TEST_CASE("the old loop: one timed-out wait leaves it EBUSY every other frame",
          "[flip_sequencer]") {
    FakeKernel k;
    OldLoop old;
    const auto frame = [&] {
        k.advance(4);
        ++k.frame;
        old.present(k);
    };
    for (int i = 0; i < 50; ++i) frame();
    REQUIRE(k.ebusy == 0);

    k.stall_next_flip_ms = 150;  // one flip lands late: the wait times out
    frame();
    const int before = k.ebusy;
    for (int i = 0; i < 200; ++i) frame();
    // Not a burst: it never recovers. One EBUSY for each accepted flip,
    // which is the "98 failures in last 100 frames" line from the field.
    CHECK(k.ebusy - before >= 95);
    const int late = k.ebusy;
    for (int i = 0; i < 200; ++i) frame();
    CHECK(k.ebusy - late >= 95);
}

TEST_CASE("steady state: first frame by SetCrtc, then clean flips, no log",
          "[flip_sequencer]") {
    Rig r;
    CHECK(r.seq.awaiting_first_frame());
    CHECK(r.frame().result == FrameResult::ShownBySetCrtc);
    CHECK_FALSE(r.seq.awaiting_first_frame());
    r.frames(300);
    CHECK(r.last_n_completed(300));
    CHECK(r.k.setcrtcs == 1);
    CHECK(r.k.ebusy == 0);
    CHECK(r.log.empty());
    // One buffer held between frames: the one on screen.
    CHECK(r.held.scanout().has_value());
    CHECK_FALSE(r.held.pending().has_value());
}

TEST_CASE("a wait that times out, then the late event: recovered next frame",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    r.k.stall_next_flip_ms = 150;
    const FrameReport slow = r.frame();
    CHECK(slow.result == FrameResult::FlipPending);
    CHECK(r.seq.flip_outstanding());
    // Two buffers are the kernel's now: on screen, and the flip's target.
    CHECK(r.held.scanout().has_value());
    CHECK(r.held.pending().has_value());

    // The next frame waits for the late flip, then flips itself — and its
    // own wait ends on ITS event, not the late one.
    const FrameReport next = r.frame();
    CHECK(next.earlier_flip_completed);
    CHECK(next.result == FrameResult::FlipCompleted);
    CHECK_FALSE(r.k.pending.has_value());  // really completed in the kernel
    CHECK(r.k.screen == r.k.frame);

    r.frames(200);
    CHECK(r.last_n_completed(201));
    CHECK(r.k.ebusy == 0);     // never a flip on top of a pending one
    CHECK(r.k.setcrtcs == 1);  // only the first frame; no SetCrtc fallback
    REQUIRE(r.log.size() == 1);
    CHECK(r.log[0].find("recovered") != std::string::npos);
    CHECK(r.log[0].find("1 flip wait(s) timed out") != std::string::npos);
}

TEST_CASE("a flip that stays out past a whole frame's budget drops frames, "
          "then recovers",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    r.k.stall_next_flip_ms = 450;
    CHECK(r.frame().result == FrameResult::FlipPending);
    const int shown = r.k.screen;

    int dropped = 0;
    while (r.frame().result == FrameResult::Dropped) {
        ++dropped;
        // The picture on screen stays until the late flip lands.
        CHECK((r.k.screen == shown || r.k.screen == shown + 1));
        REQUIRE(dropped < 20);
    }
    CHECK(dropped >= 2);
    CHECK(r.results.back() == FrameResult::FlipCompleted);
    r.frames(100);
    CHECK(r.last_n_completed(101));
    CHECK(r.k.ebusy == 0);
    CHECK(r.k.setcrtcs == 1);
    REQUIRE(r.log.size() == 1);
    CHECK(r.log[0].find("frame(s) dropped") != std::string::npos);
}

TEST_CASE("any stall length: bounded waits, no EBUSY, no SetCrtc, one log line",
          "[flip_sequencer]") {
    // Below kLostFlipMs. Rig::frame() checks the wait budget, the buffer
    // books and the release rules on every frame.
    for (std::int64_t stall = 0; stall <= 1800; stall += 7) {
        for (std::int64_t render : {1, 4, 15, 30}) {
            Rig r;
            r.frames(5, render);
            r.k.stall_next_flip_ms = stall;
            r.frames(60, render);
            INFO("stall " << stall << " ms, render " << render << " ms");
            CHECK(r.last_n_completed(10));
            CHECK(r.k.ebusy == 0);
            CHECK(r.k.setcrtcs == 1);
            CHECK(r.log.size() <= 1);
        }
    }
}

TEST_CASE("an event for an older flip never completes a newer one",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    const std::uint64_t old_token = r.k.last_token;

    // A stale copy of an old event and an event nobody asked for are in
    // the queue when the next flip is requested, and that flip is slow.
    r.k.stall_next_flip_ms = 40;
    r.k.events.push_back(old_token);
    r.k.events.push_back(987654321);
    const std::int64_t before = r.k.now;
    const FrameReport rep = r.frame(0);
    CHECK(rep.result == FrameResult::FlipCompleted);
    CHECK(r.k.now - before >= 40);         // it waited for its own event
    CHECK_FALSE(r.k.pending.has_value());  // which had really arrived
    CHECK(r.k.screen == r.k.frame);

    // Stale events arriving DURING the wait do not end it either.
    r.k.stall_next_flip_ms = 150;
    r.k.queue_with_next_flip = {old_token, 987654321};
    CHECK(r.frame(0).result == FrameResult::FlipPending);
    CHECK(r.seq.flip_outstanding());

    r.frames(50);
    CHECK(r.last_n_completed(49));
    CHECK(r.k.ebusy == 0);
}

TEST_CASE("an event left over from before a game cannot complete a flip "
          "after it",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    // The last flip before the game never completed inside its frame.
    r.k.stall_next_flip_ms = 300;
    CHECK(r.frame().result == FrameResult::FlipPending);
    const std::uint64_t before_game = r.k.last_token;

    r.game_handoff();
    CHECK_FALSE(r.seq.flip_outstanding());
    CHECK_FALSE(r.held.scanout().has_value());
    CHECK_FALSE(r.held.pending().has_value());

    // SetCrtc for the first frames back, as before.
    for (int i = 0; i < FlipSequencer::kSetCrtcFramesAfterReset; ++i) {
        CHECK(r.frame().result == FrameResult::ShownBySetCrtc);
    }
    // The old flip's event turns up again just as the first real flip
    // after the game is requested — and that flip is slow.
    r.k.events.push_back(before_game);
    r.k.stall_next_flip_ms = 150;
    CHECK(r.frame().result == FrameResult::FlipPending);
    CHECK(r.seq.flip_outstanding());
    CHECK(r.k.pending.has_value());

    r.frames(50);
    CHECK(r.last_n_completed(49));
    CHECK(r.k.ebusy == 0);
}

TEST_CASE("EBUSY from the kernel drops the frame; it is not answered with "
          "SetCrtc",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    const int shown = r.k.screen;
    r.k.foreign_busy_until = r.k.now + 60;  // a commit that is not ours

    int dropped = 0;
    while (r.frame().result == FrameResult::Dropped) {
        ++dropped;
        CHECK(r.k.screen == shown);
        CHECK_FALSE(r.seq.flip_outstanding());
        REQUIRE(dropped < 100);
    }
    CHECK(dropped >= 1);
    CHECK(r.k.ebusy == dropped);
    CHECK(r.k.setcrtcs == 1);
    r.frames(50);
    CHECK(r.last_n_completed(51));
    REQUIRE(r.log.size() == 1);
    CHECK(r.log[0].find("EBUSY") != std::string::npos);
}

TEST_CASE("EBUSY that never clears is ended with one SetCrtc",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    r.k.foreign_busy_until = r.k.now + 3600 * 1000;
    const std::int64_t start = r.k.now;
    while (r.frame().result == FrameResult::Dropped) {
        REQUIRE(r.k.now - start < 2 * FlipSequencer::kLostFlipMs);
    }
    CHECK(r.results.back() == FrameResult::ShownBySetCrtc);
    CHECK(r.k.now - start >= FlipSequencer::kLostFlipMs);
    CHECK(r.k.setcrtcs == 2);
    r.frames(50);
    CHECK(r.last_n_completed(50));
}

TEST_CASE("a flip whose event never comes: frames drop, then one SetCrtc "
          "puts the picture back",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    r.k.lose_next_event = true;
    CHECK(r.frame().result == FrameResult::FlipPending);
    const std::int64_t start = r.k.now;

    while (r.frame().result == FrameResult::Dropped) {
        REQUIRE(r.k.now - start < 2 * FlipSequencer::kLostFlipMs);
    }
    CHECK(r.results.back() == FrameResult::ShownBySetCrtc);
    CHECK(r.k.setcrtcs == 2);
    CHECK_FALSE(r.seq.flip_outstanding());
    CHECK(r.k.screen == r.k.frame);

    r.frames(100);
    CHECK(r.last_n_completed(100));
    CHECK(r.k.ebusy == 0);
    REQUIRE(r.log.size() == 1);
    CHECK(r.log[0].find("1 SetCrtc resync(s)") != std::string::npos);
}

TEST_CASE("a DRM fd that cannot be read: no spinning, and SetCrtc takes over",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    r.k.fd_broken = true;
    const std::int64_t start = r.k.now;
    while (r.k.now - start < 3 * FlipSequencer::kLostFlipMs) {
        r.k.pump_calls = 0;
        r.frame(16);
        // The wait gives up the moment the fd fails; it does not loop on
        // it until the budget ends.
        CHECK(r.k.pump_calls <= 3);
    }
    // Each flip that gets through is never seen to complete; after
    // kLostFlipMs a SetCrtc shows a frame, and it goes round again. The
    // picture keeps moving and the kernel is never asked for a flip on
    // top of one we still think is out.
    CHECK(r.k.setcrtcs >= 3);
    CHECK(r.k.ebusy == 0);
    CHECK(r.log.size() <= 2);

    r.k.fd_broken = false;
    r.frames(50);
    CHECK(r.last_n_completed(45));
}

TEST_CASE("after a game: the first flip fails with EINVAL and SetCrtc shows "
          "the frame",
          "[flip_sequencer]") {
    Rig r;
    r.frames(60);

    // RetroArch has exited and the mode is restored; the loading plate's
    // next frame is flipped onto a CRTC that still shows another fb.
    r.k.fail_next_flip = EINVAL;
    const FrameReport rep = r.frame();
    CHECK(rep.result == FrameResult::ShownBySetCrtc);
    CHECK(r.k.screen == r.k.frame);  // shown in the SAME frame
    CHECK(r.k.setcrtcs == 2);
    CHECK_FALSE(r.seq.flip_outstanding());
    CHECK(r.held.scanout() == std::optional<int>(r.k.frame));

    // restore_display_after_game(): reset, then SetCrtc for ten frames.
    r.game_handoff();
    CHECK(r.seq.awaiting_first_frame());
    for (int i = 0; i < FlipSequencer::kSetCrtcFramesAfterReset; ++i) {
        CHECK(r.frame().result == FrameResult::ShownBySetCrtc);
    }
    CHECK(r.k.setcrtcs == 2 + FlipSequencer::kSetCrtcFramesAfterReset);
    r.frames(100);
    CHECK(r.last_n_completed(100));
    CHECK(r.k.ebusy == 0);

    // One line for the whole hand-off, naming the errno.
    REQUIRE(r.log.size() == 1);
    CHECK(r.log[0].find("1 flip error(s)") != std::string::npos);
    CHECK(r.log[0].find("errno 22") != std::string::npos);
}

TEST_CASE("a flip still pending when a game starts: its event is ignored "
          "after the game",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    r.k.stall_next_flip_ms = 150;
    CHECK(r.frame().result == FrameResult::FlipPending);
    r.game_handoff();
    CHECK(r.k.events.size() == 1);  // waiting in the queue since the game

    r.frames(FlipSequencer::kSetCrtcFramesAfterReset);
    CHECK(r.k.events.empty());
    r.frames(50);
    CHECK(r.last_n_completed(50));
    CHECK(r.k.ebusy == 0);
    REQUIRE(r.log.size() == 1);
    CHECK(r.log[0].find("1 stale event(s) ignored") != std::string::npos);
}

TEST_CASE("SetCrtc failing on the first frame: retried every frame, frame "
          "dropped, one line per 10 s",
          "[flip_sequencer]") {
    Rig r;
    r.k.fail_setcrtc = EACCES;
    const std::int64_t start = r.k.now;
    while (r.k.now - start < 25000) {
        CHECK(r.frame(16).result == FrameResult::Dropped);
        CHECK(r.seq.awaiting_first_frame());
    }
    CHECK_FALSE(r.held.scanout().has_value());
    REQUIRE(r.log.size() == 2);  // at 10 s and 20 s, not one per frame
    CHECK(r.log[0].find("continuing") != std::string::npos);
    CHECK(r.log[0].find("errno 13") != std::string::npos);

    r.k.fail_setcrtc = 0;
    CHECK(r.frame().result == FrameResult::ShownBySetCrtc);
    REQUIRE(r.log.size() == 3);
    CHECK(r.log[2].find("recovered") != std::string::npos);
    r.frames(20);
    CHECK(r.last_n_completed(20));
}

TEST_CASE("flip errors that keep coming: every frame still shown, log stays "
          "at one line per 10 s",
          "[flip_sequencer]") {
    Rig r;
    r.frames(20);
    const std::int64_t start = r.k.now;
    int frames = 0;
    while (r.k.now - start < 35000) {
        r.k.fail_next_flip = EINVAL;
        CHECK(r.frame().result == FrameResult::ShownBySetCrtc);
        ++frames;
    }
    CHECK(frames > 1000);
    CHECK(r.log.size() == 3);
}

TEST_CASE("describe: one line, only what happened", "[flip_sequencer]") {
    FlipSummary s;
    s.recovered = true;
    s.duration_ms = 212;
    s.trouble.wait_timeouts = 1;
    s.trouble.dropped_frames = 2;
    CHECK(describe(s) ==
          "Page flip recovered after 212 ms: 1 flip wait(s) timed out, "
          "2 frame(s) dropped");

    FlipSummary going;
    going.duration_ms = 10004;
    going.trouble.busy_rejections = 600;
    going.trouble.dropped_frames = 600;
    const std::string line = describe(going);
    CHECK(line.find("Page flip trouble continuing, 10004 ms so far: "
                    "600 frame(s) dropped, 600 EBUSY") == 0);
    CHECK(line.find('\n') == std::string::npos);
}

TEST_CASE("ScanoutBuffers: the old scanout is released when the flip "
          "completes, not when it is requested",
          "[flip_sequencer]") {
    ScanoutBuffers<int> held;
    std::vector<int> released;
    const auto rel = [&](const int& b) { released.push_back(b); };

    held.apply({FrameResult::ShownBySetCrtc, false}, 1, rel);
    CHECK(released.empty());
    held.apply({FrameResult::FlipPending, false}, 2, rel);
    CHECK(released.empty());  // 1 is still on screen
    held.apply({FrameResult::Dropped, false}, 3, rel);
    CHECK(released == std::vector<int>{3});
    held.apply({FrameResult::FlipCompleted, true}, 4, rel);
    CHECK(released == std::vector<int>{3, 1, 2});
    CHECK(held.scanout() == std::optional<int>(4));
    CHECK_FALSE(held.pending().has_value());

    held.apply({FrameResult::FlipPending, false}, 5, rel);
    held.apply({FrameResult::ShownBySetCrtc, false}, 6, rel);
    CHECK(released == std::vector<int>{3, 1, 2, 5, 4});
    held.release_all(rel);
    CHECK(released.size() == 6);
    CHECK_FALSE(held.scanout().has_value());
}
