// PipelineControl: GstPlayer's control thread (video/pipeline_control.h).
// The render thread queues every stop/load/play/pause/seek; one thread
// carries them out. These tests stand in a fake executor that can be held
// the way a GStreamer state change is held by a read stuck on the SD card.

#include <catch2/catch_test_macros.hpp>

#include "video/pipeline_control.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using video::PipelineCommand;
using video::PipelineControl;
using video::PipelineOp;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

PipelineCommand cmd(PipelineOp op, std::string uri = "") {
    PipelineCommand c;
    c.op = op;
    c.uri = std::move(uri);
    return c;
}

std::vector<PipelineOp> ops(const std::deque<PipelineCommand>& q) {
    std::vector<PipelineOp> out;
    for (const auto& c : q) out.push_back(c.op);
    return out;
}

// A gate the executor can be held at, like a set_state(NULL) joining a
// streaming thread that is inside a read().
struct Gate {
    std::mutex mu;
    std::condition_variable cv;
    bool open = true;
    void close() { std::lock_guard<std::mutex> lk(mu); open = false; }
    void release() {
        { std::lock_guard<std::mutex> lk(mu); open = true; }
        cv.notify_all();
    }
    void pass() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return open; });
    }
};

struct Recorder {
    std::mutex mu;
    std::vector<std::string> log;  // "<op> <uri>" in execution order
    std::vector<Clock::time_point> started;
    std::vector<Clock::time_point> finished;
    std::atomic<int> entered{0};
    Gate gate;
    std::atomic<bool> hold_stops{false};
    std::vector<double> volumes;
    std::vector<std::string> order;  // commands and volume applications

    PipelineControl::Executor executor() {
        return [this](const PipelineCommand& c) {
            const auto t = Clock::now();
            ++entered;
            if (c.op == PipelineOp::Stop && hold_stops) gate.pass();
            std::lock_guard<std::mutex> lk(mu);
            log.push_back(std::string(video::pipeline_op_name(c.op)) +
                          (c.uri.empty() ? "" : " " + c.uri));
            order.push_back(log.back());
            started.push_back(t);
            finished.push_back(Clock::now());
        };
    }
    PipelineControl::VolumeApplier volume() {
        return [this](double v, const std::shared_ptr<void>&) {
            std::lock_guard<std::mutex> lk(mu);
            volumes.push_back(v);
            order.push_back("volume");
        };
    }
    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return log;
    }
};

template <typename F>
double seconds_taken(F f) {
    const auto a = Clock::now();
    f();
    return std::chrono::duration<double>(Clock::now() - a).count();
}

void wait_for(const std::function<bool()>& pred) {
    const auto deadline = Clock::now() + 5s;
    while (!pred() && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
    REQUIRE(pred());
}

}  // namespace

TEST_CASE("coalesce: a stop makes everything not yet started moot", "[pipeline_control]") {
    std::deque<PipelineCommand> q;
    video::coalesce_into(q, cmd(PipelineOp::Play));
    video::coalesce_into(q, cmd(PipelineOp::Seek));
    video::coalesce_into(q, cmd(PipelineOp::Stop));
    REQUIRE(ops(q) == std::vector<PipelineOp>{PipelineOp::Stop});
    CHECK(q.back().uri.empty());  // a plain stop
    video::coalesce_into(q, cmd(PipelineOp::Stop));
    CHECK(ops(q) == std::vector<PipelineOp>{PipelineOp::Stop});
}

TEST_CASE("coalesce: a stop that drops a load ends where load-then-stop ended",
          "[pipeline_control]") {
    // The old synchronous load set the URI and flushed its stream before the
    // stop: a later play() restarted THAT stream. The stop carries it over.
    std::deque<PipelineCommand> q;
    PipelineCommand load = cmd(PipelineOp::Load, "b");
    load.generation = 7;
    video::coalesce_into(q, load);
    video::coalesce_into(q, cmd(PipelineOp::Play));
    video::coalesce_into(q, cmd(PipelineOp::Stop));
    REQUIRE(ops(q) == std::vector<PipelineOp>{PipelineOp::Stop});
    CHECK(q.back().uri == "b");
    CHECK(q.back().generation == 7);
    // ...and a further stop keeps it.
    video::coalesce_into(q, cmd(PipelineOp::Stop));
    CHECK(q.back().uri == "b");
    CHECK(q.back().generation == 7);
}

TEST_CASE("coalesce: a load keeps the stop before it and replaces the rest",
          "[pipeline_control]") {
    std::deque<PipelineCommand> q;
    video::coalesce_into(q, cmd(PipelineOp::Stop));
    video::coalesce_into(q, cmd(PipelineOp::Load, "a"));
    video::coalesce_into(q, cmd(PipelineOp::Play));
    video::coalesce_into(q, cmd(PipelineOp::Seek));
    video::coalesce_into(q, cmd(PipelineOp::Load, "b"));
    REQUIRE(ops(q) == std::vector<PipelineOp>{PipelineOp::Stop, PipelineOp::Load});
    CHECK(q.back().uri == "b");

    std::deque<PipelineCommand> no_stop;
    video::coalesce_into(no_stop, cmd(PipelineOp::Pause));
    video::coalesce_into(no_stop, cmd(PipelineOp::Load, "c"));
    CHECK(ops(no_stop) == std::vector<PipelineOp>{PipelineOp::Load});
}

TEST_CASE("coalesce: play/pause and seeks replace their own kind at the back",
          "[pipeline_control]") {
    std::deque<PipelineCommand> q;
    video::coalesce_into(q, cmd(PipelineOp::Load, "a"));
    video::coalesce_into(q, cmd(PipelineOp::Play));
    video::coalesce_into(q, cmd(PipelineOp::Pause));
    CHECK(ops(q) == std::vector<PipelineOp>{PipelineOp::Load, PipelineOp::Pause});
    PipelineCommand s1 = cmd(PipelineOp::Seek);
    s1.seek_target_ns = 1;
    PipelineCommand s2 = cmd(PipelineOp::Seek);
    s2.seek_target_ns = 2;
    video::coalesce_into(q, s1);
    video::coalesce_into(q, s2);
    REQUIRE(ops(q) == std::vector<PipelineOp>{PipelineOp::Load, PipelineOp::Pause,
                                              PipelineOp::Seek});
    CHECK(q.back().seek_target_ns == 2);
    video::coalesce_into(q, cmd(PipelineOp::Play));  // a seek between: kept
    CHECK(ops(q) == std::vector<PipelineOp>{PipelineOp::Load, PipelineOp::Pause,
                                            PipelineOp::Seek, PipelineOp::Play});
}

TEST_CASE("load_not_before: the caller's gap, counted from the stop's completion",
          "[pipeline_control]") {
    const Clock::time_point t0{std::chrono::hours(1)};
    // Prompt stop: the old timing exactly (200 ms after stop() returned).
    CHECK(video::load_not_before(t0, t0 + 20ms, t0 + 200ms) == t0 + 220ms);
    // A stop held 10 s by the card: still 200 ms after it finished.
    CHECK(video::load_not_before(t0, t0 + 10s, t0 + 200ms) == t0 + 10200ms);
    // A stop long before the load imposes at most kMaxStopSettle.
    CHECK(video::load_not_before(t0, t0 + 10ms, t0 + 60s) ==
          t0 + 10ms + video::kMaxStopSettle);
    // A load issued with no gap (GstPlayer::load_file's own NULL): no settle.
    CHECK(video::load_not_before(t0, t0 + 5ms, t0) == t0 + 5ms);
}

TEST_CASE("the caller never waits for a command held by the card", "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    r.hold_stops = true;
    r.gate.close();
    pc.submit(cmd(PipelineOp::Stop));
    wait_for([&] { return r.entered.load() == 1; });  // the stop is in hand, held

    const double took = seconds_taken([&] {
        pc.submit(cmd(PipelineOp::Load, "next"));
        pc.submit(cmd(PipelineOp::Play));
        PipelineCommand seek = cmd(PipelineOp::Seek);
        pc.submit(seek);
        pc.set_volume(55.0, nullptr);
        (void)pc.busy();
        (void)pc.status();
    });
    CHECK(took < 0.05);
    const auto st = pc.status();
    CHECK(st.busy);
    REQUIRE(st.running.has_value());
    CHECK(*st.running == PipelineOp::Stop);
    CHECK(st.queued == 3);

    r.gate.release();
    pc.wait_idle();
    CHECK_FALSE(pc.busy());
    CHECK(r.snapshot() == std::vector<std::string>{"stop", "load next", "play", "seek"});
    CHECK(r.volumes == std::vector<double>{55.0});
}

TEST_CASE("cancel_pending also drops a load waiting out its settle", "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    pc.submit(cmd(PipelineOp::Stop));
    pc.wait_idle();
    PipelineCommand load = cmd(PipelineOp::Load, "late");
    {
        std::lock_guard<std::mutex> lk(r.mu);
        load.issued_at = r.finished[0] + 800ms;
    }
    pc.submit(load);
    wait_for([&] { return pc.status().running.has_value(); });  // in its settle
    pc.cancel_pending();
    pc.wait_idle();
    CHECK(r.snapshot() == std::vector<std::string>{"stop"});
}

TEST_CASE("a queued stop or load is not held back by volume changes", "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    r.hold_stops = true;
    r.gate.close();
    pc.submit(cmd(PipelineOp::Stop));
    wait_for([&] { return r.entered.load() == 1; });
    pc.submit(cmd(PipelineOp::Load, "next"));
    pc.set_volume(30.0, nullptr);
    r.gate.release();
    pc.wait_idle();
    REQUIRE(r.order.size() == 3);
    CHECK(r.order == std::vector<std::string>{"stop", "load next", "volume"});
}

TEST_CASE("wedged: a command in hand past the limit", "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    CHECK_FALSE(pc.wedged(10ms));
    r.hold_stops = true;
    r.gate.close();
    pc.submit(cmd(PipelineOp::Stop));
    wait_for([&] { return r.entered.load() == 1; });
    CHECK_FALSE(pc.wedged());  // the real limit is 30 s
    std::this_thread::sleep_for(30ms);
    CHECK(pc.wedged(20ms));
    r.gate.release();
    pc.wait_idle();
    CHECK_FALSE(pc.wedged(0ms));
}

TEST_CASE("a stalled queue never builds a backlog", "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    r.hold_stops = true;
    r.gate.close();
    pc.submit(cmd(PipelineOp::Stop));
    wait_for([&] { return r.entered.load() == 1; });
    // A channel tuned, a boundary, a reload and a retune while the card is
    // stuck: only the stop before them and the latest load survive.
    for (const char* uri : {"a", "b", "c", "d"}) {
        pc.submit(cmd(PipelineOp::Stop));
        pc.submit(cmd(PipelineOp::Load, uri));
        pc.submit(cmd(PipelineOp::Play));
    }
    CHECK(pc.status().queued == 3);
    r.hold_stops = false;
    r.gate.release();
    pc.wait_idle();
    // The surviving stop carries "c", the last load it dropped: the pipeline
    // passes through exactly what load c, stop, load d left it in before.
    CHECK(r.snapshot() == std::vector<std::string>{"stop", "stop c", "load d", "play"});
}

TEST_CASE("a load after a stop keeps the caller's settle after the stop finished",
          "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    r.hold_stops = true;
    r.gate.close();
    pc.submit(cmd(PipelineOp::Stop));
    std::this_thread::sleep_for(150ms);  // load_item_media's settle (shortened)
    pc.submit(cmd(PipelineOp::Load, "x"));
    std::this_thread::sleep_for(100ms);  // the card holds the stop longer
    r.gate.release();
    pc.wait_idle();
    REQUIRE(r.snapshot() == std::vector<std::string>{"stop", "load x"});
    const auto gap = r.started[1] - r.finished[0];
    CHECK(gap >= 140ms);   // the settle, after the stop really finished
    CHECK(gap < 1000ms);
}

TEST_CASE("a newer stop arriving during the settle supersedes the waiting load",
          "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    pc.submit(cmd(PipelineOp::Stop));
    pc.wait_idle();
    std::this_thread::sleep_for(50ms);
    // Stamped 600 ms after the stop finished, as if the caller had left
    // that gap: it waits out the settle until then.
    PipelineCommand load = cmd(PipelineOp::Load, "y");
    {
        std::lock_guard<std::mutex> lk(r.mu);
        load.issued_at = r.finished[0] + 600ms;
    }
    pc.submit(load);
    wait_for([&] { return pc.status().running.has_value(); });  // waiting out the settle
    pc.submit(cmd(PipelineOp::Stop));
    pc.wait_idle();
    // The superseding stop takes the dropped load's URI over.
    CHECK(r.snapshot() == std::vector<std::string>{"stop", "stop y"});
}

TEST_CASE("volume: latest wins, and it never makes the pipeline busy", "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    r.hold_stops = true;
    r.gate.close();
    pc.submit(cmd(PipelineOp::Stop));
    wait_for([&] { return r.entered.load() == 1; });
    pc.set_volume(10.0, nullptr);
    pc.set_volume(20.0, nullptr);
    pc.set_volume(30.0, nullptr);
    r.gate.release();
    pc.wait_idle();
    CHECK(r.volumes == std::vector<double>{30.0});

    pc.set_volume(40.0, nullptr);
    CHECK_FALSE(pc.busy());
    pc.wait_idle();
    CHECK(r.volumes.back() == 40.0);
}

TEST_CASE("an executor that throws does not stop the control thread", "[pipeline_control]") {
    std::vector<PipelineOp> ran;
    std::mutex mu;
    PipelineControl pc(
        [&](const PipelineCommand& c) {
            {
                std::lock_guard<std::mutex> lk(mu);
                ran.push_back(c.op);
            }
            if (c.op == PipelineOp::Stop) throw std::runtime_error("bug");
        },
        nullptr);
    pc.submit(cmd(PipelineOp::Stop));
    pc.wait_idle();
    pc.submit(cmd(PipelineOp::Play));
    pc.wait_idle();
    CHECK(ran == std::vector<PipelineOp>{PipelineOp::Stop, PipelineOp::Play});
}

TEST_CASE("a dropped command releases its pipeline reference", "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    r.hold_stops = true;
    r.gate.close();
    pc.submit(cmd(PipelineOp::Stop));
    wait_for([&] { return r.entered.load() == 1; });
    auto pipeline = std::make_shared<int>(7);
    std::weak_ptr<int> watch = pipeline;
    PipelineCommand load = cmd(PipelineOp::Load, "gone");
    load.target = pipeline;
    pipeline.reset();
    pc.submit(std::move(load));
    CHECK_FALSE(watch.expired());          // queued: held
    pc.submit(cmd(PipelineOp::Stop));      // supersedes it
    CHECK(watch.expired());
    r.gate.release();
    pc.wait_idle();
}

TEST_CASE("cancel_pending drops what has not started; the command in hand finishes",
          "[pipeline_control]") {
    Recorder r;
    PipelineControl pc(r.executor(), r.volume());
    r.hold_stops = true;
    r.gate.close();
    pc.submit(cmd(PipelineOp::Stop));
    wait_for([&] { return r.entered.load() == 1; });
    pc.submit(cmd(PipelineOp::Load, "never"));
    pc.set_volume(70.0, nullptr);
    pc.cancel_pending();
    CHECK(pc.status().queued == 0);
    CHECK(pc.busy());  // the stop is still in hand
    r.gate.release();
    pc.wait_idle();
    CHECK(r.snapshot() == std::vector<std::string>{"stop"});
    CHECK(r.volumes.empty());
}
