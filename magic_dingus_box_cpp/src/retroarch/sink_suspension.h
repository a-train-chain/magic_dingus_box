#pragma once

// Handing the audio devices from PulseAudio to RetroArch, and back.
//
// Games play through ALSA directly (sysdefault:CARD=vc4hdmiN, or the Pi 4B's
// analog jack), and an ALSA hw device has one owner at a time. PulseAudio
// holds every sink's device open while it plays and for module-suspend-on-
// idle's timeout afterwards — 5 s, the stock /etc/pulse/default.pa line. So
// a RetroArch that opened its device less than 5 s after the kiosk's last
// stream closed got "Device or resource busy" and ran the whole game
// silent. A fast core does exactly that: FBNeo opened audio ~4-4.5 s after
// the kiosk's video teardown, and one launch in four had sound (Pi 5,
// 2026-11-13). The kiosk's fixed waits only moved the race around.
//
// `pactl suspend-sink <sink> 1` closes the device now, so the hand-over no
// longer depends on a timer. EVERY sink is suspended — both HDMI ports and
// the analog jack — because the game may open a card that is not the
// default sink (the TV may be on either HDMI port).
//
// The danger is the other direction. A sink suspended this way stays
// suspended until `pactl suspend-sink <sink> 0`: a new stream does not wake
// it (a client cannot resume an administratively suspended sink — the
// playback stall watchdog's CHANGELOG entry records the wedge that makes).
// One missed resume is a kiosk with no sound. So:
//   - SinkSuspension resumes, from its destructor, whatever it suspended and
//     was not explicitly resumed — every early return and exception between
//     suspend() and the end of the session is covered by scope;
//   - resume() tries EVERY sink suspend() sent a suspend to, whatever
//     happened to the others;
//   - resume_all_sinks() runs once at kiosk start-up, for a kiosk that was
//     killed mid-game and never reached its resume.
//
// Never fatal: a pactl that fails or times out is reported and the launch
// goes on (the old idle-timer hand-over is then the fallback). suspend()
// gives up after the first timed-out call — a wedged PulseAudio costs one
// bounded call there, not one per sink.
//
// Pure: every pactl call goes through the injected runner, so this is
// unit-tested on the Mac (tests/retroarch/test_sink_suspension.cpp). The
// kiosk passes app::audio::default_runner() (2 s bound, stdout captured).

#include <functional>
#include <string>
#include <vector>

#include "../utils/subprocess.h"

namespace retroarch {

using PactlRunner =
    std::function<utils::subprocess::Result(const std::vector<std::string>& argv)>;

// Sink names (second column) of `pactl list short sinks`, in listed order.
// Lines that do not start with a numeric index are ignored.
std::vector<std::string> parse_sink_names(const std::string& pactl_short_sinks);

// What one pass over the sinks did, for the log.
struct SinkReport {
    // `pactl list short sinks` answered. false = PulseAudio unreachable,
    // nothing else was sent. Always true for resume(), which does not list.
    bool listed = true;
    std::vector<std::string> done;       // pactl exited 0
    std::vector<std::string> failed;     // pactl exited non-zero (sink gone, ...)
    std::vector<std::string> timed_out;  // hit the bound: outcome unknown
    std::vector<std::string> abandoned;  // suspend() only: never sent after a timeout
};

// One line for the journal / launcher log, e.g.
// "suspended 2 PulseAudio sink(s): a, b".
std::string describe_sink_report(const std::string& action, const SinkReport& report);

class SinkSuspension {
public:
    // An empty runner makes every call a no-op (no PulseAudio to talk to).
    explicit SinkSuspension(PactlRunner run);
    // resume() for anything still held.
    ~SinkSuspension();

    SinkSuspension(const SinkSuspension&) = delete;
    SinkSuspension& operator=(const SinkSuspension&) = delete;

    // List the sinks and suspend each, in listed order. Once per object: a
    // second call does nothing.
    SinkReport suspend();

    // `suspend-sink <sink> 0` for every sink suspend() sent a suspend to —
    // including ones whose suspend failed or timed out (their state is
    // unknown; resuming a running sink is harmless). Idempotent.
    SinkReport resume();

    // Sinks still waiting for their resume.
    const std::vector<std::string>& held() const { return held_; }

private:
    PactlRunner run_;
    std::vector<std::string> held_;
    bool suspend_attempted_ = false;
};

// Start-up crash recovery: `suspend-sink <sink> 0` for every sink PulseAudio
// lists now. Tries each one whatever happened to the others.
SinkReport resume_all_sinks(const PactlRunner& run);

}  // namespace retroarch
