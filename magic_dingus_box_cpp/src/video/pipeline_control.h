#pragma once

// The shared GStreamer pipeline's control thread.
//
// WHY: every GStreamer state change used to run on the render loop, and a
// state change can wait on the SD card. Setting a playing pipeline to NULL
// deactivates the demuxer's pad, which joins its streaming thread — and
// that thread is inside a read() of the media file. Starting one opens the
// file (three open()s on playbin's path) on the calling thread. With the
// card saturated by writes a read waits seconds behind write-back, so on
// magicpi5 (2026-11-15 23:37, a 2.57 GB unpaced upload) the stall
// watchdog's channel reload sat in its stop() and the systemd watchdog
// (WatchdogSec=10) SIGABRTed the kiosk. Reproduced off the box by delaying
// the media file's reads (dev/sd_stall_repro): stop() held the
// caller for the rest of the read in flight (10.8 s with 15 s reads),
// load_file() for its open()s. And a stall is exactly the moment a read is
// stuck: the watchdog fires BECAUSE the streaming thread is waiting on
// the card.
//
// So the render thread only QUEUES pipeline operations here; one thread
// carries them out, one at a time, in order (GstPlayer's execute()). The
// queue is coalesced (coalesce_into) so a stalled card never builds a
// backlog: a stop or a load supersedes everything not yet started.
//
// The settle: Controller::load_item_media stops, waits 200 ms, then loads
// — the time the DRM driver needs to release the previous video's buffers
// before the next stream starts. That gap used to begin when stop()
// RETURNED, i.e. when the pipeline had reached NULL. A queued stop returns
// at once, so the worker re-creates it: a load that follows a stop does
// not start before the stop has finished plus the gap its caller left
// between the two calls (load_not_before, capped at kMaxStopSettle).
//
// Volume is state, not a step: the latest value is applied between
// commands and never makes the pipeline "busy".
//
// No GStreamer here: the pipeline is an opaque reference the executor
// casts back, so the queue rules and the thread are unit-tested on the Mac
// (tests/video/test_pipeline_control.cpp).
//
// THREADING: submit(), set_volume(), busy(), status() and the wait_*()
// calls belong to ONE owner thread (the render thread). The executor and
// the volume applier run on the control thread only.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace video {

enum class PipelineOp { Stop, Load, Play, Pause, Seek };

inline const char* pipeline_op_name(PipelineOp op) {
    switch (op) {
        case PipelineOp::Stop: return "stop";
        case PipelineOp::Load: return "load";
        case PipelineOp::Play: return "play";
        case PipelineOp::Pause: return "pause";
        case PipelineOp::Seek: return "seek";
    }
    return "?";
}

struct PipelineCommand {
    using Clock = std::chrono::steady_clock;
    PipelineOp op = PipelineOp::Stop;
    // Load: the stream it starts (uri + generation). Stop: the load it
    // superseded, if any — set after NULL, as the old synchronous
    // load-then-stop left the pipeline (coalesce_into); empty for a plain
    // stop. Seek: `generation` is the owner's serial for the seek.
    std::string uri;
    std::uint64_t generation = 0;
    std::int64_t seek_target_ns = 0;  // Seek
    int seek_flags = 0;               // Seek (GstSeekFlags)
    Clock::time_point issued_at{};
    // The pipeline the command acts on — a reference the owner took, so a
    // dropped command releases it. Opaque here (no GStreamer in this file).
    std::shared_ptr<void> target;
};

// A command in hand this long is wedged, not slow (a decoder deadlock, a
// dead card): the owner stops pinging the systemd watchdog so the kiosk is
// restarted, the last resort the blocking calls used to get for free. A
// read behind a saturated card's write-back was measured at 4-7 s.
inline constexpr std::chrono::seconds kWedgedAfter{30};

// The longest settle a stop can impose on the load after it. A stop issued
// long before the load (the Media Browser's leave(), minutes earlier) must
// not delay it.
inline constexpr std::chrono::milliseconds kMaxStopSettle{1000};

// What a newly submitted command does to the commands not yet started:
//   - stop:  everything queued is moot; the queue becomes [stop]. A load it
//            drops is not forgotten entirely: the stop carries its URI and
//            generation, so the pipeline ends as the old synchronous
//            load-then-stop left it (that URI set, that stream flushed) and a
//            later play() restarts that stream, not the one before it.
//   - load:  queued loads, plays, pauses and seeks are moot (the load starts
//            from NULL itself). A queued stop STAYS, so the settle after it
//            is kept; the queue becomes [stop?, load].
//   - play / pause: replaces a play or pause at the back.
//   - seek:  replaces a seek at the back (the coalescer in GstPlayer keeps
//            one in flight; this only bounds a stalled queue).
inline void coalesce_into(std::deque<PipelineCommand>& queued, PipelineCommand cmd) {
    switch (cmd.op) {
        case PipelineOp::Stop:
            for (auto it = queued.rbegin(); it != queued.rend(); ++it) {
                if (it->op == PipelineOp::Load ||
                    (it->op == PipelineOp::Stop && !it->uri.empty())) {
                    if (cmd.uri.empty()) {
                        cmd.uri = it->uri;
                        cmd.generation = it->generation;
                    }
                    break;
                }
            }
            queued.clear();
            break;
        case PipelineOp::Load:
            queued.erase(std::remove_if(queued.begin(), queued.end(),
                                        [](const PipelineCommand& c) {
                                            return c.op != PipelineOp::Stop;
                                        }),
                         queued.end());
            break;
        case PipelineOp::Play:
        case PipelineOp::Pause:
            if (!queued.empty() && (queued.back().op == PipelineOp::Play ||
                                    queued.back().op == PipelineOp::Pause)) {
                queued.pop_back();
            }
            break;
        case PipelineOp::Seek:
            if (!queued.empty() && queued.back().op == PipelineOp::Seek) {
                queued.pop_back();
            }
            break;
    }
    queued.push_back(std::move(cmd));
}

// The earliest a load may start after the stop that ran before it: the
// stop's completion plus the gap its caller left between issuing the stop
// and the load (at most kMaxStopSettle). With a prompt stop this is the
// old timing exactly — the caller's 200 ms counted from stop()'s return.
inline PipelineCommand::Clock::time_point load_not_before(
    PipelineCommand::Clock::time_point stop_issued,
    PipelineCommand::Clock::time_point stop_done,
    PipelineCommand::Clock::time_point load_issued) {
    auto gap = load_issued - stop_issued;
    if (gap < PipelineCommand::Clock::duration::zero()) gap = {};
    if (gap > kMaxStopSettle) gap = kMaxStopSettle;
    return stop_done + gap;
}

class PipelineControl {
public:
    using Clock = PipelineCommand::Clock;
    using Executor = std::function<void(const PipelineCommand&)>;
    using VolumeApplier = std::function<void(double, const std::shared_ptr<void>&)>;

    PipelineControl(Executor execute, VolumeApplier apply_volume)
        : execute_(std::move(execute)), apply_volume_(std::move(apply_volume)) {}

    // Drops what was not started, lets the command in hand finish, stops.
    // The owner tears the pipeline down only after wait_idle().
    ~PipelineControl() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            queue_.clear();
            volume_dirty_ = false;
            volume_target_.reset();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    PipelineControl(const PipelineControl&) = delete;
    PipelineControl& operator=(const PipelineControl&) = delete;

    // Queues `cmd` (coalesce_into). Never runs it on the caller's thread and
    // never waits for the command in flight. The thread starts on the first
    // submit. If it cannot be started the command runs inline — the old,
    // blocking behavior — rather than being dropped.
    void submit(PipelineCommand cmd) {
        if (cmd.issued_at == Clock::time_point{}) cmd.issued_at = Clock::now();
        std::unique_lock<std::mutex> lk(mu_);
        if (!start_thread_locked()) {
            lk.unlock();
            execute_(cmd);
            return;
        }
        coalesce_into(queue_, std::move(cmd));
        lk.unlock();
        cv_.notify_all();
    }

    // The latest volume wins; applied on the control thread between
    // commands. Inline if the thread cannot be started.
    void set_volume(double volume, std::shared_ptr<void> target) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!start_thread_locked()) {
            lk.unlock();
            if (apply_volume_) apply_volume_(volume, target);
            return;
        }
        volume_ = volume;
        volume_target_ = std::move(target);
        volume_dirty_ = true;
        lk.unlock();
        cv_.notify_all();
    }

    // A command is queued or running (including a load waiting out its
    // settle). The owner leaves the pipeline alone meanwhile: its state,
    // bus and queries describe a stream the queue is replacing.
    bool busy() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !queue_.empty() || running_;
    }

    struct Status {
        bool busy = false;
        std::optional<PipelineOp> running;  // set while one is in hand
        double running_for_s = 0.0;
        std::size_t queued = 0;
    };
    Status status() const {
        std::lock_guard<std::mutex> lk(mu_);
        Status s;
        s.busy = !queue_.empty() || running_;
        s.queued = queue_.size();
        if (running_) {
            s.running = running_op_;
            s.running_for_s =
                std::chrono::duration<double>(Clock::now() - running_since_).count();
        }
        return s;
    }

    // Drops every command and volume change not yet started (their
    // pipeline references with them), including a load still waiting out
    // its settle. A command already executing finishes. For a
    // teardown: a queued load would otherwise start a stream just to have
    // it torn down.
    void cancel_pending() {
        std::deque<PipelineCommand> dropped;
        std::shared_ptr<void> volume_target;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++cancel_epoch_;  // a load waiting out its settle is dropped too
            dropped.swap(queue_);
            volume_target = std::move(volume_target_);
            volume_target_.reset();
            volume_dirty_ = false;
        }
        cv_.notify_all();
    }

    // The command in hand has run for `after` (kWedgedAfter) or more.
    bool wedged(std::chrono::duration<double> after = kWedgedAfter) const {
        const Status s = status();
        return s.running && s.running_for_s >= after.count();
    }

    // Blocks until nothing is queued, running or waiting to be applied.
    // Teardown and tests only — NEVER the render loop.
    void wait_idle() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return idle_locked(); });
    }
    bool wait_idle_until(Clock::time_point deadline) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_until(lk, deadline, [this] { return idle_locked(); });
    }

private:
    bool idle_locked() const {
        return queue_.empty() && !running_ && !volume_dirty_ && !applying_volume_;
    }

    bool start_thread_locked() {
        if (thread_.joinable()) return true;
        try {
            thread_ = std::thread([this] { loop(); });
            return true;
        } catch (const std::system_error&) {
            return false;
        }
    }

    static bool supersedes(const std::deque<PipelineCommand>& q) {
        return std::any_of(q.begin(), q.end(), [](const PipelineCommand& c) {
            return c.op == PipelineOp::Stop || c.op == PipelineOp::Load;
        });
    }

    void loop() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait(lk, [this] { return stop_ || volume_dirty_ || !queue_.empty(); });
            if (stop_) break;
            // Commands first: a knob spin sets the volume every frame, and a
            // stop or load must not wait behind it.
            if (queue_.empty() && volume_dirty_) {
                const double v = volume_;
                auto target = std::move(volume_target_);
                volume_target_.reset();
                volume_dirty_ = false;
                applying_volume_ = true;
                lk.unlock();
                try {
                    if (apply_volume_) apply_volume_(v, target);
                } catch (...) {
                }
                target.reset();
                lk.lock();
                applying_volume_ = false;
                cv_.notify_all();
                continue;
            }
            PipelineCommand cmd = std::move(queue_.front());
            queue_.pop_front();
            running_ = true;
            running_op_ = cmd.op;
            running_since_ = Clock::now();
            if (cmd.op == PipelineOp::Load && stop_since_load_) {
                // The settle after the stop before it. A newer stop or load
                // arriving meanwhile supersedes this load.
                const auto not_before =
                    load_not_before(last_stop_issued_, last_stop_done_, cmd.issued_at);
                const std::uint64_t epoch = cancel_epoch_;
                auto superseded = [this, epoch] {
                    return stop_ || cancel_epoch_ != epoch || supersedes(queue_);
                };
                cv_.wait_until(lk, not_before, superseded);
                if (superseded()) {
                    // A newer stop takes this load's URI over, as if it had
                    // been dropped from the queue (coalesce_into).
                    for (auto& q : queue_) {
                        if (q.op == PipelineOp::Stop && q.uri.empty()) {
                            q.uri = cmd.uri;
                            q.generation = cmd.generation;
                        }
                        if (q.op == PipelineOp::Stop || q.op == PipelineOp::Load) break;
                    }
                    running_ = false;
                    lk.unlock();
                    cmd.target.reset();
                    lk.lock();
                    cv_.notify_all();
                    if (stop_) break;
                    continue;
                }
                running_since_ = Clock::now();
            }
            // The command runs WITHOUT the lock: it is the part that can wait
            // on the card for seconds, and submit() must stay instant.
            lk.unlock();
            try {
                execute_(cmd);
            } catch (...) {
                // A GStreamer call does not throw; an executor bug must not
                // take the kiosk down from this thread.
            }
            const auto done = Clock::now();
            cmd.target.reset();
            lk.lock();
            running_ = false;
            if (cmd.op == PipelineOp::Stop) {
                stop_since_load_ = true;
                last_stop_issued_ = cmd.issued_at;
                last_stop_done_ = done;
            } else if (cmd.op == PipelineOp::Load) {
                stop_since_load_ = false;
            }
            cv_.notify_all();
        }
    }

    Executor execute_;
    VolumeApplier apply_volume_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<PipelineCommand> queue_;
    bool running_ = false;
    PipelineOp running_op_ = PipelineOp::Stop;
    Clock::time_point running_since_{};
    bool stop_since_load_ = false;
    Clock::time_point last_stop_issued_{};
    Clock::time_point last_stop_done_{};
    double volume_ = 0.0;
    std::shared_ptr<void> volume_target_;
    bool volume_dirty_ = false;
    bool applying_volume_ = false;
    std::uint64_t cancel_epoch_ = 0;
    bool stop_ = false;
    std::thread thread_;  // last: started after every field above exists
};

}  // namespace video
