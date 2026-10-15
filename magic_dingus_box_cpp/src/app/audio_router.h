#pragma once

// PulseAudio output routing: pick the sink for the user's AudioOutput
// setting, make it the default, and move every live stream onto it.
// Moved out of app_state.h, where it was ~100 lines of inline fork/exec in
// a header every TU includes. All process I/O goes through an injected
// Runner, so the routing decisions are unit-tested on the Mac
// (tests/app/test_audio_router.cpp) without PulseAudio.
//
// WHY the bounds matter: apply_output() runs on the RENDER thread (game
// return, settings restore, Settings -> Audio Output). The old inline
// pactl capture read stdout and waited with no deadline, so a wedged
// PulseAudio froze the picture until the systemd watchdog killed the
// kiosk. Every pactl call is now bounded, and the first one that times
// out abandons the rest — a wedged daemon costs one timeout, not four.

#include <functional>
#include <string>
#include <vector>

#include "app_state.h"
#include "../utils/subprocess.h"

namespace app::audio {

using Runner = std::function<utils::subprocess::Result(const std::vector<std::string>& argv)>;

// The production runner: utils::subprocess, stdout captured, 2 s bound.
// Returns the test override when one is installed.
Runner default_runner();

// Process-wide override for tests that reach apply_output() indirectly
// (SettingsPersistence::load_settings applies the loaded output). Pass an
// empty Runner to restore the real one.
void set_default_runner_for_tests(Runner runner);

// "<id>\t<sink>\t..." lines of `pactl list short sink-inputs` -> the ids.
std::vector<std::string> parse_sink_input_ids(const std::string& pactl_short_sink_inputs);

// The sink AudioOutput resolves to among those `pactl list short sinks`
// reports: HEADPHONE -> analog (HDMI fallback), AUTO/HDMI -> HDMI (analog
// fallback). "" when PulseAudio is unreachable or has no sinks.
std::string resolve_output_sink(AudioOutput output, const Runner& run);

// Route to that sink: set-default-sink, then move-sink-input for every
// live stream. Returns the sink applied, "" when there was none (the
// PulseAudio default is then left alone rather than pointed at a sink
// that does not exist).
std::string apply_output(AudioOutput output, const Runner& run);

}  // namespace app::audio
