// Audio output option gating — on boards without an analog jack
// (Pi 5) the Settings menu must not offer Headphone, and a stale
// "headphone" value loaded from settings.json must degrade to Auto.

#include <catch2/catch_test_macros.hpp>

#include "app/app_state.h"

using app::AppState;
using app::AudioOutput;

using AudioSettings = decltype(AppState{}.audio_settings);

TEST_CASE("cycle order with analog audio: Auto -> HDMI -> Headphone -> Auto") {
    REQUIRE(AudioSettings::next_output(AudioOutput::AUTO, true) == AudioOutput::HDMI);
    REQUIRE(AudioSettings::next_output(AudioOutput::HDMI, true) == AudioOutput::HEADPHONE);
    REQUIRE(AudioSettings::next_output(AudioOutput::HEADPHONE, true) == AudioOutput::AUTO);
}

TEST_CASE("cycle skips Headphone when board has no analog jack") {
    REQUIRE(AudioSettings::next_output(AudioOutput::AUTO, false) == AudioOutput::HDMI);
    REQUIRE(AudioSettings::next_output(AudioOutput::HDMI, false) == AudioOutput::AUTO);
    // Even from a (stale) Headphone state, cycling leaves it.
    REQUIRE(AudioSettings::next_output(AudioOutput::HEADPHONE, false) == AudioOutput::AUTO);
}

TEST_CASE("sanitize_for_platform coerces stale headphone setting to Auto") {
    AppState state;
    state.audio_settings.output = AudioOutput::HEADPHONE;
    state.audio_settings.sanitize_for_platform(false);
    REQUIRE(state.audio_settings.output == AudioOutput::AUTO);
    REQUIRE_FALSE(state.audio_settings.analog_audio_available);
}

TEST_CASE("sanitize_for_platform keeps headphone setting when jack exists") {
    AppState state;
    state.audio_settings.output = AudioOutput::HEADPHONE;
    state.audio_settings.sanitize_for_platform(true);
    REQUIRE(state.audio_settings.output == AudioOutput::HEADPHONE);
    REQUIRE(state.audio_settings.analog_audio_available);
}
