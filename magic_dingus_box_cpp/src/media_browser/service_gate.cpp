#include "media_browser/service_gate.h"

#include <algorithm>
#include <thread>

namespace media_browser {

namespace {
using Ms = std::chrono::milliseconds;
}  // namespace

GateResult wait_for_service(const ServiceGateHooks& hooks,
                            ServiceGateTiming timing) {
    // No ping = nothing could ever prove readiness. Report it as a timeout
    // (the caller's failure toast) without spinning out the deadline.
    if (!hooks.ping) return GateResult::TimedOut;

    const auto now = [&hooks] {
        return hooks.now ? hooks.now() : std::chrono::steady_clock::now();
    };
    const auto sleep = [&hooks](Ms d) {
        if (d <= Ms::zero()) return;
        if (hooks.sleep) {
            hooks.sleep(d);
        } else {
            std::this_thread::sleep_for(d);
        }
    };
    const auto cancelled = [&hooks] {
        return hooks.cancelled && hooks.cancelled();
    };

    auto deadline = now() + timing.deadline;
    const auto remaining = [&] {
        return std::chrono::duration_cast<Ms>(deadline - now());
    };
    const Ms slice = std::max(timing.slice, Ms(1));

    if (cancelled()) return GateResult::Cancelled;

    const auto paused = [&hooks] { return hooks.paused && hooks.paused(); };
    bool need_quiet_wait = true;
    for (;;) {
        if (cancelled()) return GateResult::Cancelled;
        if (paused()) {
            // An owner can acquire the services after polling has already
            // begun. Keep this worker alive, without pinging stopped
            // services or charging that ownership time to their startup.
            do {
                if (cancelled()) return GateResult::Cancelled;
                sleep(slice);
            } while (paused());
            deadline = now() + timing.deadline;
            need_quiet_wait = true;
            continue;
        }

        // Quiet work is checked again after every ownership release: its
        // resume must finish before a ping can observe transient readiness.
        if (need_quiet_wait && hooks.wait_quiet_idle) {
            const Ms left = remaining();
            if (left <= Ms::zero()) return GateResult::TimedOut;
            if (hooks.wait_quiet_idle(std::min(left, Ms(1000))))
                need_quiet_wait = false;
            continue;
        }
        need_quiet_wait = false;

        // A claim beginning during the request wins over readiness or an
        // elapsed deadline. The next iteration waits for its eventual
        // release and gives service startup a fresh bound.
        const bool ready = hooks.ping();
        if (paused()) continue;
        if (ready) return GateResult::Ready;
        if (remaining() <= Ms::zero()) return GateResult::TimedOut;

        Ms waited{0};
        while (waited < timing.poll_interval) {
            if (cancelled()) return GateResult::Cancelled;
            if (paused()) break;
            const Ms left = remaining();
            if (left <= Ms::zero()) return GateResult::TimedOut;
            const Ms step = std::min({slice, timing.poll_interval - waited, left});
            sleep(step);
            waited += step;
        }
    }
}

DeferredBatchAction decide_deferred_batch(GateResult gate,
                                          bool full_pause_active,
                                          bool session_began_during_gate) {
    if (gate == GateResult::Cancelled) return DeferredBatchAction::Abandon;
    if (full_pause_active) return DeferredBatchAction::Regate;
    if (gate == GateResult::Ready) return DeferredBatchAction::Add;
    if (session_began_during_gate) return DeferredBatchAction::Regate;
    return DeferredBatchAction::Drop;
}

}  // namespace media_browser
