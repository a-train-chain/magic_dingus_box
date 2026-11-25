#include "app/cable_tv_look.h"
#include "app/tuner_snow.h"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>

TEST_CASE("Tuner hiss follows the snow envelope and the master volume") {
    using namespace app::tuner_snow;
    constexpr int length = duration.count() * sample_rate / 1000;
    std::array<int16_t, length> full{}, half{}, muted{};
    write_audio(full.data(), length, 0, 42, 100);
    write_audio(half.data(), length, 0, 42, 50);
    write_audio(muted.data(), length, 0, 42, 0);
    double energy = 0.0;
    int peak = 0, half_error = 0;
    for (int i = 0; i < length; ++i) {
        peak = std::max(peak, std::abs(int(full[i])));
        half_error = std::max(half_error, std::abs(2 * half[i] - full[i]));
        energy += double(full[i]) * full[i];
    }
    const double rms = std::sqrt(energy / length) / 32768;
    // Include attack and decay: the whole burst is between -32 and -29 dBFS.
    CHECK(rms > 0.025);
    CHECK(rms < 0.0355);
    CHECK(peak < 5200);
    CHECK(full.front() == 0);
    CHECK(std::abs(full.back()) <= 1);
    CHECK(half_error <= 1);
    CHECK(std::all_of(muted.begin(), muted.end(), [](auto v) { return v == 0; }));
    CHECK(strength(0.150f) == 0.0f);
    CHECK(strength(0.125f) < strength(0.095f));
}

TEST_CASE("Cable TV selects the low-cost Enhanced mix and preserves movie and bezel controls") {
    app::AppState::DisplaySettings display;
    display.glow_intensity = display.bloom_intensity = display.flicker_intensity = 0.5f;
    display.interlacing_intensity = display.mb_bloom_intensity = 0.75f;
    display.bezel_index = 3;
    app::apply_cable_tv_look(display);
    CHECK(display.crt_engine == app::CrtEngine::Enhanced);
    CHECK(display.enhanced_crt_enabled);
    CHECK(display.scanline_intensity == 0.25f);
    CHECK(display.rgb_mask_intensity == 0.16f);
    CHECK(display.warmth_intensity == 0.12f);
    CHECK(display.bloom_intensity == 0);
    CHECK(display.glow_intensity == 0);
    CHECK(display.interlacing_intensity == 0);
    CHECK(display.flicker_intensity == 0);
    CHECK(display.mb_bloom_intensity == 0.75f);
    CHECK(display.bezel_index == 3);
}
