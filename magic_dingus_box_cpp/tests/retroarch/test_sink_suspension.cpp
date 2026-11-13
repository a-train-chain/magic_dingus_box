// retroarch/sink_suspension.h: the PulseAudio -> RetroArch audio hand-over.
// What is pinned: EVERY listed sink is suspended (both HDMI ports, the Pi 4B
// jack — never only the default sink), by name and verbatim argv; every
// sink sent a suspend gets a resume on every way out — explicit resume, an
// early return, an exception; a resume pass never stops early; a wedged
// PulseAudio costs suspend() one bounded call; the start-up recovery resumes
// sinks this process never suspended.

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
    SECTION("an exception between suspend and resume") {
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

TEST_CASE("resume() never stops early: a missed resume is a silent kiosk",
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

TEST_CASE("start-up recovery resumes every listed sink, whoever suspended it",
          "[sink_suspension]") {
    SECTION("a kiosk killed mid-game left both HDMI sinks suspended") {
        FakePactl pa;
        pa.sinks = kPi5Sinks;
        pa.suspended = {kPi5Hdmi0, kPi5Hdmi1};
        const SinkReport r = retroarch::resume_all_sinks(pa.runner());
        CHECK(pa.calls == std::vector<Argv>{list_cmd(), resume_cmd(kPi5Hdmi0),
                                            resume_cmd(kPi5Hdmi1)});
        CHECK(r.listed);
        CHECK(r.done == Argv{kPi5Hdmi0, kPi5Hdmi1});
        CHECK(pa.suspended.empty());
    }
    SECTION("tries every sink even after one times out") {
        FakePactl pa;
        pa.sinks = kPi4Sinks;
        pa.resume_times_out = {kPi4Jack};
        const SinkReport r = retroarch::resume_all_sinks(pa.runner());
        CHECK(pa.calls.size() == 4);
        CHECK(r.timed_out == Argv{kPi4Jack});
        CHECK(r.done == Argv{kPi4Hdmi0, kPi4Hdmi1});
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
