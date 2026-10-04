#include "audio_router.h"

#include <iostream>
#include <mutex>
#include <sstream>

#include "../platform/platform_profile.h"

namespace app::audio {

namespace {

std::mutex g_override_mutex;
Runner g_override;   // empty = the real pactl runner

utils::subprocess::Result run_pactl_for_real(const std::vector<std::string>& argv) {
    return utils::subprocess::run(argv, std::chrono::milliseconds(2000), /*capture_stdout=*/true);
}

}  // namespace

Runner default_runner() {
    std::lock_guard<std::mutex> lock(g_override_mutex);
    if (g_override) return g_override;
    return run_pactl_for_real;
}

void set_default_runner_for_tests(Runner runner) {
    std::lock_guard<std::mutex> lock(g_override_mutex);
    g_override = std::move(runner);
}

std::vector<std::string> parse_sink_input_ids(const std::string& pactl_short_sink_inputs) {
    std::vector<std::string> ids;
    std::istringstream iss(pactl_short_sink_inputs);
    std::string line;
    while (std::getline(iss, line)) {
        std::string id;
        for (char c : line) {
            if (c >= '0' && c <= '9') id += c;
            else break;
        }
        if (!id.empty()) ids.push_back(id);
    }
    return ids;
}

std::string resolve_output_sink(AudioOutput output, const Runner& run) {
    const auto sinks = run({"pactl", "list", "short", "sinks"});
    if (!sinks.ok()) return "";
    const auto want = (output == AudioOutput::HEADPHONE) ? platform::SinkChoice::Analog
                                                         : platform::SinkChoice::Hdmi;
    return platform::resolve_sink(sinks.out, want).value_or("");
}

std::string apply_output(AudioOutput output, const Runner& run) {
    const std::string sink = resolve_output_sink(output, run);
    if (sink.empty()) return "";

    // argv all the way down — the sink name comes from pactl's own output
    // and is never interpreted by a shell.
    const auto set = run({"pactl", "set-default-sink", sink});
    if (set.timed_out) {
        std::cerr << "audio: pactl set-default-sink timed out; skipping stream moves"
                  << std::endl;
        return sink;
    }

    const auto inputs = run({"pactl", "list", "short", "sink-inputs"});
    if (!inputs.ok()) return sink;
    for (const auto& id : parse_sink_input_ids(inputs.out)) {
        if (run({"pactl", "move-sink-input", id, sink}).timed_out) break;
    }
    return sink;
}

}  // namespace app::audio

// ---- AudioSettings members (declared in app_state.h) -------------------

namespace app {

std::string AppState::AudioSettings::resolve_output_sink() const {
    return audio::resolve_output_sink(output, audio::default_runner());
}

std::string AppState::AudioSettings::apply_output() const {
    return audio::apply_output(output, audio::default_runner());
}

void AppState::AudioSettings::cycle_output() {
    output = next_output(output, analog_audio_available);
    apply_output();
}

}  // namespace app
