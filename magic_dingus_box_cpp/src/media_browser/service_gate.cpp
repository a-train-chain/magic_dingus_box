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

    const auto deadline = now() + timing.deadline;
    const auto remaining = [&] {
        return std::chrono::duration_cast<Ms>(deadline - now());
    };
    const Ms slice = std::max(timing.slice, Ms(1));

    if (cancelled()) return GateResult::Cancelled;

    // Phase 1: the quiet-mode executor. Sliced at <= 1 s so cancellation
    // stays prompt even though wait_until_idle_for itself is not
    // cancellable.
    if (hooks.wait_quiet_idle) {
        for (;;) {
            const Ms left = remaining();
            if (left <= Ms::zero()) return GateResult::TimedOut;
            if (hooks.wait_quiet_idle(std::min(left, Ms(1000)))) break;
            if (cancelled()) return GateResult::Cancelled;
        }
    }

    // Phase 2: poll the service. A ping is attempted before the deadline
    // check on each round, so a service that answers on the very edge of
    // the window still counts.
    for (;;) {
        if (cancelled()) return GateResult::Cancelled;
        if (hooks.ping()) return GateResult::Ready;
        if (remaining() <= Ms::zero()) return GateResult::TimedOut;
        // Sleep out the poll interval in slices, re-checking cancellation
        // and the deadline between them.
        Ms waited{0};
        while (waited < timing.poll_interval) {
            if (cancelled()) return GateResult::Cancelled;
            const Ms left = remaining();
            if (left <= Ms::zero()) return GateResult::TimedOut;
            const Ms step =
                std::min({slice, timing.poll_interval - waited, left});
            sleep(step);
            waited += step;
        }
    }
}

}  // namespace media_browser
