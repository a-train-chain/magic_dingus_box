// retroarch/sink_suspension.h: the PulseAudio -> RetroArch audio hand-over.
// What is pinned: EVERY listed sink is suspended (both HDMI ports, the Pi 4B
// jack — never only the default sink), by name and verbatim argv; every
// sink sent a suspend gets a resume on every way out — explicit resume, an
// early return (and an exception where something catches it); a resume pass
// never stops early; a session resume that did not complete is retried once
// and a complete one costs no retry call; a wedged PulseAudio costs suspend()
// one bounded call; the recovery resumes sinks this process never suspended,
// only those listed SUSPENDED, and says so accurately.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "retroarch/sink_suspension.h"

using retroarch::PactlRunner;
using retroarch::SinkReport;
using retroarch::SinkSuspension;
using utils::subprocess::Result;
using Argv = std::vector<std::string>;

namespace {

// Real `pactl list short sinks` shapes (the sink names embed the SoC bus
// address, which differs per board and per HDMI port).
const char* kPi5Hdmi0 = "alsa_output.platform-107c701400.hdmi.hdmi-stereo";
const char* kPi5Hdmi1 = "alsa_output.platform-107c706400.hdmi.hdmi-stereo";
const char* kPi4Jack = "alsa_output.platform-fe00b840.mailbox.stereo-fallback";
const char* kPi4Hdmi0 = "alsa_output.platform-fef00700.hdmi.hdmi-stereo";
const char* kPi4Hdmi1 = "alsa_output.platform-fef05700.hdmi.hdmi-stereo";

const std::string kPi5Sinks =
    std::string("62\t") + kPi5Hdmi0 + "\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tRUNNING\n" +
    "63\t" + kPi5Hdmi1 + "\tmodule-alsa-card.c\ts16le 2ch 44100Hz\tSUSPENDED\n";
const std::string kPi4Sinks =
    std::string("0\t") + kPi4Jack + "\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tSUSPENDED\n" +
    "1\t" + kPi4Hdmi0 + "\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tIDLE\n" +
    "2\t" + kPi4Hdmi1 + "\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tSUSPENDED\n";

// What `pactl list short sinks` reads like after a kiosk died mid-game (a
// user-suspended sink is always listed SUSPENDED) and in normal play.
const std::string kPi5BothSuspended =
    std::string("62\t") + kPi5Hdmi0 + "\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tSUSPENDED\n" +
    "63\t" + kPi5Hdmi1 + "\tmodule-alsa-card.c\ts16le 2ch 44100Hz\tSUSPENDED\n";
const std::string kPi5NoneSuspended =
    std::string("62\t") + kPi5Hdmi0 + "\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tRUNNING\n" +
    "63\t" + kPi5Hdmi1 + "\tmodule-alsa-card.c\ts16le 2ch 44100Hz\tIDLE\n";

Result ok(std::string out = "") {
    Result r;
    r.exit_code = 0;
    r.out = std::move(out);
    return r;
}
Result fail(int code = 1) {
    Result r;
    r.exit_code = code;
    return r;
}
Result timed_out() {
    Result r;
    r.exit_code = 128 + 15;
    r.timed_out = true;
    return r;
}

Argv list_cmd() { return {"pactl", "list", "short", "sinks"}; }
Argv suspend_cmd(const std::string& sink) { return {"pactl", "suspend-sink", sink, "1"}; }
Argv resume_cmd(const std::string& sink) { return {"pactl", "suspend-sink", sink, "0"}; }

// Scripted pactl. Records every call; tracks which sinks are user-suspended
// the way PulseAudio would, so "nothing left suspended" is checked on the
// daemon's state, not just on the call log.
struct FakePactl {
    std::string sinks;
    bool list_fails = false;
    bool list_times_out = false;
    std::set<std::string> suspend_fails;      // suspend-sink X 1 -> exit 1
    std::set<std::string> suspend_times_out;  // ... -> timeout (but PA did it)
    std::set<std::string> resume_fails;
    std::set<std::string> resume_times_out;
    bool resume_throws = false;
    std::vector<Argv> calls;
    std::set<std::string> suspended;  // PulseAudio's view

    PactlRunner runner() {
        return [this](const Argv& argv) -> Result {
            calls.push_back(argv);
            if (argv == list_cmd()) {
                if (list_times_out) return timed_out();
                if (list_fails) return fail();
                return ok(sinks);
            }
            if (argv.size() == 4 && argv[0] == "pactl" && argv[1] == "suspend-sink") {
                const std::string& sink = argv[2];
                if (argv[3] == "1") {
                    if (suspend_fails.count(sink)) return fail();
                    suspended.insert(sink);
                    if (suspend_times_out.count(sink)) return timed_out();
                    return ok();
                }
                if (argv[3] == "0") {
                    if (resume_throws) throw std::runtime_error("runner blew up");
                    if (resume_fails.count(sink)) return fail();
                    if (resume_times_out.count(sink)) return timed_out();
                    suspended.erase(sink);
                    return ok();
                }
            }
            return fail(99);  // anything else is a bug in the unit
        };
    }
};

}  // namespace

TEST_CASE("sink names come from the second column of pactl list short sinks",
          "[sink_suspension]") {
    CHECK(retroarch::parse_sink_names(kPi5Sinks) == Argv{kPi5Hdmi0, kPi5Hdmi1});
    CHECK(retroarch::parse_sink_names(kPi4Sinks) == Argv{kPi4Jack, kPi4Hdmi0, kPi4Hdmi1});
    CHECK(retroarch::parse_sink_names("").empty());
    // A trailing line without a newline still counts.
    CHECK(retroarch::parse_sink_names(std::string("3\t") + kPi5Hdmi0 + "\tx") ==
          Argv{kPi5Hdmi0});
    // Errors and junk (pactl prints these on stdout) contribute nothing.
    CHECK(retroarch::parse_sink_names("Connection failure: Connection refused\n"
                                      "\n"
                                      "x\tnot-a-sink\n"
                                      "7\t\tmodule-null-sink.c\n")
              .empty());
}

TEST_CASE("parse_sinks keeps each sink's state column", "[sink_suspension]") {
    const auto pi4 = retroarch::parse_sinks(kPi4Sinks);
    REQUIRE(pi4.size() == 3);
    CHECK(pi4[0].name == kPi4Jack);
    CHECK(pi4[0].state == "SUSPENDED");
    CHECK(pi4[1].name == kPi4Hdmi0);
    CHECK(pi4[1].state == "IDLE");
    CHECK(pi4[2].state == "SUSPENDED");
    // An older pactl without the state column: the name, an empty state.
    const auto old = retroarch::parse_sinks(std::string("5\t") + kPi5Hdmi0 +
                                            "\tmodule-alsa-card.c\ts16le 2ch 48000Hz\n");
    REQUIRE(old.size() == 1);
    CHECK(old[0].name == kPi5Hdmi0);
    CHECK(old[0].state.empty());
}

TEST_CASE("suspend() suspends every sink PulseAudio lists, not just the default",
          "[sink_suspension]") {
    SECTION("Pi 5: both HDMI ports — the TV may be on either") {
        FakePactl pa;
        pa.sinks = kPi5Sinks;
        SinkSuspension s(pa.runner());
        const SinkReport r = s.suspend();
        CHECK(pa.calls == std::vector<Argv>{list_cmd(), suspend_cmd(kPi5Hdmi0),
                                            suspend_cmd(kPi5Hdmi1)});
        CHECK(r.listed);
        CHECK(r.done == Argv{kPi5Hdmi0, kPi5Hdmi1});
        CHECK(r.failed.empty());
        CHECK(r.timed_out.empty());
        CHECK(r.abandoned.empty());
        CHECK(pa.suspended == std::set<std::string>{kPi5Hdmi0, kPi5Hdmi1});
        CHECK(s.held() == Argv{kPi5Hdmi0, kPi5Hdmi1});
        s.resume();
    }
    SECTION("Pi 4B: the analog jack and both HDMI ports") {
        FakePactl pa;
        pa.sinks = kPi4Sinks;
        SinkSuspension s(pa.runner());
        s.suspend();
        CHECK(pa.calls == std::vector<Argv>{list_cmd(), suspend_cmd(kPi4Jack),
                                            suspend_cmd(kPi4Hdmi0),
                                            suspend_cmd(kPi4Hdmi1)});
        CHECK(pa.suspended.size() == 3);
        s.resume();
    }
}

TEST_CASE("resume() resumes exactly what suspend() suspended, once",
          "[sink_suspension]") {
    FakePactl pa;
    pa.sinks = kPi5Sinks;
    SinkSuspension s(pa.runner());
    s.suspend();
    pa.calls.clear();

    const SinkReport r = s.resume();
    CHECK(pa.calls == std::vector<Argv>{resume_cmd(kPi5Hdmi0), resume_cmd(kPi5Hdmi1)});
    CHECK(r.done == Argv{kPi5Hdmi0, kPi5Hdmi1});
    CHECK(pa.suspended.empty());
    CHECK(s.held().empty());

    // Idempotent: the destructor (or a second explicit call) sends nothing.
    pa.calls.clear();
    const SinkReport again = s.resume();
    CHECK(pa.calls.empty());
    CHECK(again.done.empty());
}

TEST_CASE("the destructor resumes the sinks on every way out of the session",
          "[sink_suspension]") {
    FakePactl pa;
    pa.sinks = kPi5Sinks;

    SECTION("an early return (launch failure: spawn failed, stop requested)") {
        {
            SinkSuspension s(pa.runner());
            s.suspend();
            REQUIRE(pa.suspended.size() == 2);
        }  // no explicit resume()
        CHECK(pa.suspended.empty());
        CHECK(pa.calls.back() == resume_cmd(kPi5Hdmi1));
    }
    // Unwinding only happens where something catches: on the kiosk's launch
    // chain nothing does, so a throw there is std::terminate and the
    // start-up recovery is the backstop. This pins the guard's own half.
    SECTION("scope exit by an exception that something catches") {
        try {
            SinkSuspension s(pa.runner());
            s.suspend();
            REQUIRE(pa.suspended.size() == 2);
            throw std::runtime_error("launch blew up");
        } catch (const std::runtime_error&) {
        }
        CHECK(pa.suspended.empty());
    }
    SECTION("after an explicit resume it sends nothing more") {
        {
            SinkSuspension s(pa.runner());
            s.suspend();
            s.resume();
            pa.calls.clear();
        }
        CHECK(pa.calls.empty());
    }
    SECTION("an object that never suspended sends nothing") {
        { SinkSuspension s(pa.runner()); }
        CHECK(pa.calls.empty());
    }
}

TEST_CASE("the destructor never throws, even when the runner does",
          "[sink_suspension][throws]") {
    FakePactl pa;
    pa.sinks = kPi5Sinks;
    pa.resume_throws = true;
    CHECK_NOTHROW([&] {
        SinkSuspension s(pa.runner());
        s.suspend();
    }());  // ~SinkSuspension -> resume() -> runner throws
    CHECK(pa.calls.back() == resume_cmd(kPi5Hdmi0));
}

TEST_CASE("suspend() runs once per session", "[sink_suspension]") {
    FakePactl pa;
    pa.sinks = kPi5Sinks;
    SinkSuspension s(pa.runner());
    s.suspend();
    pa.calls.clear();
    const SinkReport r = s.suspend();
    CHECK(pa.calls.empty());
    CHECK(r.done.empty());
    CHECK(s.held() == Argv{kPi5Hdmi0, kPi5Hdmi1});
}

TEST_CASE("PulseAudio unreachable: nothing is suspended, the launch goes on",
          "[sink_suspension]") {
    SECTION("list fails (daemon down)") {
        FakePactl pa;
        pa.sinks = kPi5Sinks;
        pa.list_fails = true;
        SinkSuspension s(pa.runner());
        const SinkReport r = s.suspend();
        CHECK_FALSE(r.listed);
        CHECK(pa.calls == std::vector<Argv>{list_cmd()});
        CHECK(s.held().empty());
        pa.calls.clear();
        s.resume();
        CHECK(pa.calls.empty());
    }
    SECTION("list times out (daemon wedged): one bounded call, no more") {
        FakePactl pa;
        pa.sinks = kPi5Sinks;
        pa.list_times_out = true;
        SinkSuspension s(pa.runner());
        const SinkReport r = s.suspend();
        CHECK_FALSE(r.listed);
        CHECK(pa.calls == std::vector<Argv>{list_cmd()});
        CHECK(s.held().empty());
    }
    SECTION("no sinks at all") {
        FakePactl pa;
        pa.sinks = "";
        SinkSuspension s(pa.runner());
        const SinkReport r = s.suspend();
        CHECK(r.listed);
        CHECK(r.done.empty());
        CHECK(pa.calls == std::vector<Argv>{list_cmd()});
    }
    SECTION("an empty runner is a no-op, not a crash") {
        SinkSuspension s(PactlRunner{});
        const SinkReport r = s.suspend();
        CHECK(r.done.empty());
        CHECK(s.held().empty());
        CHECK(s.resume().done.empty());
        CHECK(retroarch::resume_all_sinks(PactlRunner{}).done.empty());
    }
}

TEST_CASE("a timed-out suspend abandons the rest but is still resumed",
          "[sink_suspension]") {
    FakePactl pa;
    pa.sinks = kPi4Sinks;
    pa.suspend_times_out = {kPi4Hdmi0};  // PulseAudio did suspend it, late
    SinkSuspension s(pa.runner());
    const SinkReport r = s.suspend();
    CHECK(pa.calls == std::vector<Argv>{list_cmd(), suspend_cmd(kPi4Jack),
                                        suspend_cmd(kPi4Hdmi0)});
    CHECK(r.done == Argv{kPi4Jack});
    CHECK(r.timed_out == Argv{kPi4Hdmi0});
    CHECK(r.abandoned == Argv{kPi4Hdmi1});
    // Outcome unknown -> it must get its resume; the never-sent one must not.
    CHECK(s.held() == Argv{kPi4Jack, kPi4Hdmi0});

    pa.calls.clear();
    s.resume();
    CHECK(pa.calls == std::vector<Argv>{resume_cmd(kPi4Jack), resume_cmd(kPi4Hdmi0)});
    CHECK(pa.suspended.empty());
}

TEST_CASE("a failed suspend does not stop the others and is still resumed",
          "[sink_suspension]") {
    FakePactl pa;
    pa.sinks = kPi5Sinks;
    pa.suspend_fails = {kPi5Hdmi0};  // e.g. the sink vanished (HDMI unplugged)
    SinkSuspension s(pa.runner());
    const SinkReport r = s.suspend();
    CHECK(r.failed == Argv{kPi5Hdmi0});
    CHECK(r.done == Argv{kPi5Hdmi1});
    CHECK(s.held() == Argv{kPi5Hdmi0, kPi5Hdmi1});
    pa.calls.clear();
    s.resume();
    CHECK(pa.calls == std::vector<Argv>{resume_cmd(kPi5Hdmi0), resume_cmd(kPi5Hdmi1)});
    CHECK(pa.suspended.empty());
}

TEST_CASE("resume() never stops early: a missed resume freezes the kiosk's video",
          "[sink_suspension]") {
    FakePactl pa;
    pa.sinks = kPi4Sinks;
    SinkSuspension s(pa.runner());
    s.suspend();
    pa.resume_times_out = {kPi4Jack};
    pa.resume_fails = {kPi4Hdmi0};
    pa.calls.clear();

    const SinkReport r = s.resume();
    CHECK(pa.calls == std::vector<Argv>{resume_cmd(kPi4Jack), resume_cmd(kPi4Hdmi0),
                                        resume_cmd(kPi4Hdmi1)});
    CHECK(r.timed_out == Argv{kPi4Jack});
    CHECK(r.failed == Argv{kPi4Hdmi0});
    CHECK(r.done == Argv{kPi4Hdmi1});
    CHECK(r.abandoned.empty());
    CHECK(pa.suspended.count(kPi4Hdmi1) == 0);
    // One pass, then it is done: the destructor does not repeat the bounded
    // calls (start-up recovery is the retry).
    CHECK(s.held().empty());
}

TEST_CASE("start-up recovery resumes every SUSPENDED sink, whoever suspended it",
          "[sink_suspension]") {
    SECTION("a kiosk killed mid-game left both HDMI sinks suspended") {
        FakePactl pa;
        pa.sinks = kPi5BothSuspended;
        pa.suspended = {kPi5Hdmi0, kPi5Hdmi1};
        const SinkReport r = retroarch::resume_all_sinks(pa.runner());
        CHECK(pa.calls == std::vector<Argv>{list_cmd(), resume_cmd(kPi5Hdmi0),
                                            resume_cmd(kPi5Hdmi1)});
        CHECK(r.listed);
        CHECK(r.done == Argv{kPi5Hdmi0, kPi5Hdmi1});
        CHECK(r.not_suspended.empty());
        CHECK(pa.suspended.empty());
    }
    SECTION("RUNNING and IDLE sinks cannot be user-suspended: no call for them") {
        FakePactl pa;
        pa.sinks = kPi5Sinks;  // HDMI 0 RUNNING, HDMI 1 SUSPENDED
        const SinkReport r = retroarch::resume_all_sinks(pa.runner());
        CHECK(pa.calls == std::vector<Argv>{list_cmd(), resume_cmd(kPi5Hdmi1)});
        CHECK(r.done == Argv{kPi5Hdmi1});
        CHECK(r.not_suspended == Argv{kPi5Hdmi0});
    }
    SECTION("an ordinary start: nothing suspended, only the list call") {
        FakePactl pa;
        pa.sinks = kPi5NoneSuspended;
        const SinkReport r = retroarch::resume_all_sinks(pa.runner());
        CHECK(pa.calls == std::vector<Argv>{list_cmd()});
        CHECK(r.done.empty());
        CHECK(r.not_suspended == Argv{kPi5Hdmi0, kPi5Hdmi1});
    }
    SECTION("no state column (older pactl): resumed, since it may be suspended") {
        FakePactl pa;
        pa.sinks = std::string("5\t") + kPi5Hdmi0 + "\tmodule-alsa-card.c\ts16le 2ch 48000Hz\n";
        const SinkReport r = retroarch::resume_all_sinks(pa.runner());
        CHECK(pa.calls == std::vector<Argv>{list_cmd(), resume_cmd(kPi5Hdmi0)});
        CHECK(r.done == Argv{kPi5Hdmi0});
    }
    SECTION("tries every suspended sink even after one times out") {
        FakePactl pa;
        pa.sinks = kPi4Sinks;  // jack SUSPENDED, HDMI 0 IDLE, HDMI 1 SUSPENDED
        pa.resume_times_out = {kPi4Jack};
        const SinkReport r = retroarch::resume_all_sinks(pa.runner());
        CHECK(pa.calls == std::vector<Argv>{list_cmd(), resume_cmd(kPi4Jack),
                                            resume_cmd(kPi4Hdmi1)});
        CHECK(r.timed_out == Argv{kPi4Jack});
        CHECK(r.done == Argv{kPi4Hdmi1});
        CHECK(r.not_suspended == Argv{kPi4Hdmi0});
    }
    SECTION("PulseAudio not answering: one call, nothing else") {
        FakePactl pa;
        pa.sinks = kPi5Sinks;
        pa.list_fails = true;
        const SinkReport r = retroarch::resume_all_sinks(pa.runner());
        CHECK_FALSE(r.listed);
        CHECK(pa.calls == std::vector<Argv>{list_cmd()});
    }
}

TEST_CASE("post-game retry: only when the session's resume did not complete",
          "[sink_suspension]") {
    SECTION("a complete resume costs not a single pactl call") {
        FakePactl pa;
        pa.sinks = kPi5BothSuspended;
        SinkReport session;
        session.done = {kPi5Hdmi0, kPi5Hdmi1};
        CHECK_FALSE(retroarch::retry_resume_if_needed(session, pa.runner()).has_value());
        CHECK(pa.calls.empty());
    }
    SECTION("no session resume at all (no runner, nothing suspended): no retry") {
        FakePactl pa;
        pa.sinks = kPi5BothSuspended;
        CHECK_FALSE(retroarch::retry_resume_if_needed(SinkReport{}, pa.runner()).has_value());
        CHECK(pa.calls.empty());
    }
    SECTION("a resume that timed out at game exit is retried, and lands") {
        // The game-exit stall: PulseAudio missed the 2 s bound, so the sink
        // is still user-suspended — the kiosk's pulsesink would wedge on it.
        FakePactl pa;
        pa.sinks = kPi5BothSuspended;
        pa.suspended = {kPi5Hdmi0, kPi5Hdmi1};
        SinkReport timed;
        timed.done = {kPi5Hdmi1};
        timed.timed_out = {kPi5Hdmi0};
        const auto retry = retroarch::retry_resume_if_needed(timed, pa.runner());
        REQUIRE(retry.has_value());
        CHECK(pa.calls.front() == list_cmd());
        CHECK(retry->done == Argv{kPi5Hdmi0, kPi5Hdmi1});
        CHECK(pa.suspended.empty());
    }
    SECTION("a resume that failed (device not free yet) is retried") {
        FakePactl pa;
        pa.sinks = kPi5Sinks;  // HDMI 1 still SUSPENDED
        pa.suspended = {kPi5Hdmi1};
        SinkReport failed;
        failed.done = {kPi5Hdmi0};
        failed.failed = {kPi5Hdmi1};
        const auto retry = retroarch::retry_resume_if_needed(failed, pa.runner());
        REQUIRE(retry.has_value());
        CHECK(pa.calls == std::vector<Argv>{list_cmd(), resume_cmd(kPi5Hdmi1)});
        CHECK(retry->done == Argv{kPi5Hdmi1});
        CHECK(pa.suspended.empty());
    }
    SECTION("end to end: suspend, a resume that times out, then the retry") {
        FakePactl pa;
        pa.sinks = kPi5BothSuspended;
        SinkSuspension s(pa.runner());
        s.suspend();
        pa.resume_times_out = {kPi5Hdmi1};  // stalls, and does not land
        const SinkReport session = s.resume();
        CHECK(session.timed_out == Argv{kPi5Hdmi1});
        CHECK(pa.suspended == std::set<std::string>{kPi5Hdmi1});
        pa.resume_times_out.clear();
        const auto retry = retroarch::retry_resume_if_needed(session, pa.runner());
        REQUIRE(retry.has_value());
        CHECK(pa.suspended.empty());
    }
}

TEST_CASE("the log line names the sinks and every non-success",
          "[sink_suspension]") {
    SinkReport r;
    r.done = {"a", "b"};
    CHECK(retroarch::describe_sink_report("suspended", r) ==
          "suspended 2 PulseAudio sink(s): a, b");

    SinkReport bad;
    bad.done = {"a"};
    bad.failed = {"b"};
    bad.timed_out = {"c"};
    bad.abandoned = {"d"};
    CHECK(retroarch::describe_sink_report("suspended", bad) ==
          "suspended 1 PulseAudio sink(s): a; FAILED: b; TIMED OUT: c; not tried: d");

    SinkReport none;
    CHECK(retroarch::describe_sink_report("resumed", none) ==
          "resumed 0 PulseAudio sink(s)");

    SinkReport unreachable;
    unreachable.listed = false;
    CHECK(retroarch::describe_sink_report("suspended", unreachable) ==
          "suspended 0 PulseAudio sink(s) (pactl list short sinks did not answer)");
}

TEST_CASE("the recovery's log line never claims a resume it cannot know about",
          "[sink_suspension]") {
    SinkReport none;
    none.not_suspended = {"a", "b"};
    CHECK(retroarch::describe_resume_all(none) ==
          "checked 2 PulseAudio sink(s), none suspended");

    SinkReport some;
    some.done = {"a"};
    some.not_suspended = {"b"};
    CHECK(retroarch::describe_resume_all(some) ==
          "1 of 2 PulseAudio sink(s) listed SUSPENDED (by a game session, or "
          "PulseAudio's idle suspend): cleared any user suspend on a");

    SinkReport bad;
    bad.timed_out = {"a"};
    bad.failed = {"b"};
    CHECK(retroarch::describe_resume_all(bad) ==
          "2 of 2 PulseAudio sink(s) listed SUSPENDED (by a game session, or "
          "PulseAudio's idle suspend): cleared any user suspend on none; "
          "FAILED: b; TIMED OUT: a");

    SinkReport unreachable;
    unreachable.listed = false;
    CHECK(retroarch::describe_resume_all(unreachable) ==
          "pactl list short sinks did not answer; nothing checked");

    SinkReport empty;
    CHECK(retroarch::describe_resume_all(empty) ==
          "checked 0 PulseAudio sink(s), none suspended");
}
