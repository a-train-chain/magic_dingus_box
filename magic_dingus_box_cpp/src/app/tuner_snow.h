#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace app::tuner_snow {

inline constexpr std::chrono::milliseconds duration{150};
inline constexpr int sample_rate = 48000;

inline float strength(float age_seconds) {
    const float end = std::chrono::duration<float>(duration).count();
    return std::clamp((end - age_seconds) / 0.055f, 0.0f, 1.0f);
}

// A little mid-band receiver hiss. Limit the level in the samples themselves,
// so opening a new Pulse stream at full volume cannot produce a loud pop.
inline void write_audio(int16_t* samples, int count, int first, uint32_t seed, int volume) {
    // At full volume the body of the burst is about -29 dBFS RMS.
    constexpr float gain_per_volume_percent = 42.5f;
    float low = 0.0f;
    float mid = 0.0f;
    for (int i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float white = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
        low += 0.05f * (white - low);
        mid += 0.45f * (white - mid);
        const float age = static_cast<float>(first + i) / sample_rate;
        const float attack = std::min(1.0f, static_cast<float>(i) / (0.004f * sample_rate));
        samples[i] = static_cast<int16_t>((mid - low) * (gain_per_volume_percent * volume) * attack * strength(age));
    }
}

}  // namespace app::tuner_snow
