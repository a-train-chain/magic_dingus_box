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
// suspended until `pactl suspend-sink <sink> 0` (which clears only that
// "user" suspend — PulseAudio's own idle suspend is separate): a new stream
// does not wake it, and a client cannot resume it. The kiosk's pulsesink
// WEDGES on such a sink — the video freezes and the playback stall watchdog
// loops play() with no way out (CHANGELOG, the stall watchdog entry) — and
// the box is silent, until the next game, a kiosk restart or a PulseAudio
// restart. So:
//   - resume() tries EVERY sink suspend() sent a suspend to, whatever
//     happened to the others;
//   - a session resume that did not complete (a call timed out or failed)
//     is retried once later in the post-game restore —
//     retry_resume_if_needed(), a no-op in the normal case;
//   - SinkSuspension's destructor resumes whatever was not explicitly
//     resumed, so an early return between suspend() and the end of the
//     session is covered by scope. NOT an exception on the kiosk's launch
//     chain: nothing there catches, so a throw ends in std::terminate
//     without unwinding — the backstop for that is systemd's restart plus:
//   - resume_all_sinks() at kiosk start-up, for a kiosk that died mid-game
//     and never reached its resume.
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
#include <optional>
#include <string>
#include <vector>

#include "../utils/subprocess.h"

namespace retroarch {

using PactlRunner =
    std::function<utils::subprocess::Result(const std::vector<std::string>& argv)>;

// One line of `pactl list short sinks`: "<index>\t<name>\t<driver>\t
// <sample spec>\t<state>". `state` is RUNNING / IDLE / SUSPENDED, or "" when
// the line has no state column.
struct ListedSink {
    std::string name;
    std::string state;
};

// The listed sinks, in order. Lines that do not start with a numeric index
// are ignored.
std::vector<ListedSink> parse_sinks(const std::string& pactl_short_sinks);
// Just their names.
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
    // resume_all_sinks() only: listed RUNNING or IDLE, so nothing to resume
    // (a user-suspended sink is always listed SUSPENDED).
    std::vector<std::string> not_suspended;
};

// One line for the journal / launcher log of a session's suspend() or
// resume(), e.g. "suspended 2 PulseAudio sink(s): a, b".
std::string describe_sink_report(const std::string& action, const SinkReport& report);

// The same for resume_all_sinks(), worded for what it can know: a sink
// listed SUSPENDED may be PulseAudio's own idle suspend, which
// `suspend-sink 0` leaves alone, so it never claims to have "resumed" one.
// e.g. "checked 2 PulseAudio sink(s), none suspended".
std::string describe_resume_all(const SinkReport& report);

class SinkSuspension {
public:
    // An empty runner makes every call a no-op (no PulseAudio to talk to).
    explicit SinkSuspension(PactlRunner run);
    // resume() for anything still held — an early return between suspend()
    // and the explicit resume(). Never throws: a throw out of a destructor
    // is std::terminate.
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

// `suspend-sink <sink> 0` for every sink PulseAudio lists as SUSPENDED (or
// with no state column) now; RUNNING/IDLE ones cannot be user-suspended and
// are only reported. Tries each one whatever happened to the others. Kiosk
// start-up (crash recovery) and the post-game retry below.
SinkReport resume_all_sinks(const PactlRunner& run);

// The post-game retry. `session_resume` is the session's resume() report:
// when any of its calls timed out (outcome unknown — PulseAudio may still
// hold the sink suspended) or failed (e.g. the device was not free yet, so
// the sink could not reopen), run resume_all_sinks() once more and return
// its report. Otherwise nullopt WITHOUT a single pactl call — the normal
// case costs nothing.
std::optional<SinkReport> retry_resume_if_needed(const SinkReport& session_resume,
                                                 const PactlRunner& run);

}  // namespace retroarch
