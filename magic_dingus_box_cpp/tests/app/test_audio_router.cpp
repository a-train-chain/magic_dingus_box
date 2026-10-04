// app::audio — PulseAudio routing through an injected runner. What is
// pinned: the sink chosen per AudioOutput on both boards, the exact pactl
// argv sequence (no shell, sink name verbatim), the stream moves, and that
// a wedged or absent PulseAudio costs at most ONE bounded call before the
// routing gives up — apply_output() runs on the render thread.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "app/audio_router.h"

using app::AudioOutput;
using utils::subprocess::Result;
namespace audio = app::audio;
using Argv = std::vector<std::string>;

namespace {

const char* kPi4Sinks =
    "0\talsa_output.platform-fe00b840.mailbox.stereo-fallback\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tSUSPENDED\n"
    "1\talsa_output.platform-fef00700.hdmi.hdmi-stereo\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tRUNNING\n";
const char* kPi5Sinks =
    "0\talsa_output.platform-107c701400.hdmi.hdmi-stereo\tmodule-alsa-card.c\ts16le 2ch 48000Hz\tIDLE\n";
const char* kPi4Hdmi = "alsa_output.platform-fef00700.hdmi.hdmi-stereo";
const char* kPi4Jack = "alsa_output.platform-fe00b840.mailbox.stereo-fallback";
const char* kPi5Hdmi = "alsa_output.platform-107c701400.hdmi.hdmi-stereo";

Result ok(std::string out = "") {
    Result r;
    r.exit_code = 0;
    r.out = std::move(out);
    return r;
}
Result timed_out() {
    Result r;
    r.exit_code = 128 + 15;
    r.timed_out = true;
    return r;
}

// Scripted pactl: answers by argv[1..3], records every call.
struct FakePactl {
    std::string sinks;
    std::string sink_inputs;
    bool sinks_time_out = false;
    bool set_default_times_out = false;
    std::vector<Argv> calls;

    audio::Runner runner() {
        return [this](const Argv& argv) -> Result {
            calls.push_back(argv);
            if (argv.size() >= 4 && argv[1] == "list" && argv[3] == "sinks") {
                return sinks_time_out ? timed_out() : ok(sinks);
            }
            if (argv.size() >= 4 && argv[1] == "list" && argv[3] == "sink-inputs") {
                return ok(sink_inputs);
            }
            if (argv.size() >= 2 && argv[1] == "set-default-sink") {
                return set_default_times_out ? timed_out() : ok();
            }
            return ok();
        };
    }
};

}  // namespace

TEST_CASE("audio: sink-input id parsing", "[audio_router]") {
    CHECK(audio::parse_sink_input_ids("").empty());
    CHECK(audio::parse_sink_input_ids(
              "12\t1\tprotocol-native.c\tfloat32le 2ch 44100Hz\n"
              "7\t0\tmodule-loopback.c\ts16le 2ch 48000Hz\n") == Argv{"12", "7"});
    // Junk lines (warnings, blank) contribute nothing.
    CHECK(audio::parse_sink_input_ids("\nWarning: foo\n3\tx\n") == Argv{"3"});
}

TEST_CASE("audio: sink resolution per output and board", "[audio_router]") {
    FakePactl pa;
    pa.sinks = kPi4Sinks;
    CHECK(audio::resolve_output_sink(AudioOutput::AUTO, pa.runner()) == kPi4Hdmi);
    CHECK(audio::resolve_output_sink(AudioOutput::HDMI, pa.runner()) == kPi4Hdmi);
    CHECK(audio::resolve_output_sink(AudioOutput::HEADPHONE, pa.runner()) == kPi4Jack);

    // Pi 5 has no jack: a stale HEADPHONE degrades to HDMI, not silence.
    pa.sinks = kPi5Sinks;
    CHECK(audio::resolve_output_sink(AudioOutput::HEADPHONE, pa.runner()) == kPi5Hdmi);

    pa.sinks = "";
    CHECK(audio::resolve_output_sink(AudioOutput::AUTO, pa.runner()).empty());
}

TEST_CASE("audio: apply sets the default and moves every live stream", "[audio_router]") {
    FakePactl pa;
    pa.sinks = kPi4Sinks;
    pa.sink_inputs = "12\t1\tprotocol-native.c\n7\t0\tprotocol-native.c\n";
    CHECK(audio::apply_output(AudioOutput::HEADPHONE, pa.runner()) == kPi4Jack);
    CHECK(pa.calls == std::vector<Argv>{
        {"pactl", "list", "short", "sinks"},
        {"pactl", "set-default-sink", kPi4Jack},
        {"pactl", "list", "short", "sink-inputs"},
        {"pactl", "move-sink-input", "12", kPi4Jack},
        {"pactl", "move-sink-input", "7", kPi4Jack},
    });
}

TEST_CASE("audio: no usable sink leaves the PulseAudio default alone", "[audio_router]") {
    FakePactl pa;
    pa.sinks = "";
    CHECK(audio::apply_output(AudioOutput::AUTO, pa.runner()).empty());
    CHECK(pa.calls.size() == 1);   // only the listing; no set-default-sink
}

TEST_CASE("audio: a wedged PulseAudio costs one bounded call", "[audio_router]") {
    FakePactl pa;
    pa.sinks = kPi4Sinks;
    pa.sinks_time_out = true;
    CHECK(audio::apply_output(AudioOutput::AUTO, pa.runner()).empty());
    CHECK(pa.calls.size() == 1);

    FakePactl pb;
    pb.sinks = kPi4Sinks;
    pb.sink_inputs = "1\tx\n2\tx\n";
    pb.set_default_times_out = true;
    // The sink was resolved, so it is reported, but no stream moves are
    // attempted against a daemon that just timed out.
    CHECK(audio::apply_output(AudioOutput::AUTO, pb.runner()) == kPi4Hdmi);
    CHECK(pb.calls.size() == 2);
}

TEST_CASE("audio: AudioSettings members route through the default runner",
          "[audio_router]") {
    FakePactl pa;
    pa.sinks = kPi4Sinks;
    audio::set_default_runner_for_tests(pa.runner());
    app::AppState::AudioSettings s;
    s.output = AudioOutput::AUTO;
    s.analog_audio_available = true;
    CHECK(s.resolve_output_sink() == kPi4Hdmi);
    s.cycle_output();   // AUTO -> HDMI, applied
    CHECK(s.output == AudioOutput::HDMI);
    CHECK(pa.calls.back() == Argv{"pactl", "list", "short", "sink-inputs"});
    audio::set_default_runner_for_tests({});
}
