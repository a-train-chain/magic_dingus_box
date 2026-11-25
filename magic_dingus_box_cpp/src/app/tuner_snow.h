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
class ReceiverNoise {
public:
    explicit ReceiverNoise(uint32_t seed) : seed_(seed) {}

    // first is the sample offset from the latest dial press. Retuning
    // extends the envelope while the noise generator keeps running.
    void write(int16_t* samples, int count, int first, int volume) {
        constexpr float gain_per_volume_percent = 42.5f;
        constexpr float attack_step = 1.0f / (0.004f * sample_rate);
        for (int i = 0; i < count; ++i) {
            seed_ = seed_ * 1664525u + 1013904223u;
            const float white = static_cast<float>(seed_ >> 8) / 8388608.0f - 1.0f;
            low_ += 0.05f * (white - low_);
            mid_ += 0.45f * (white - mid_);
            const float age = static_cast<float>(first + i) / sample_rate;
            const float gain = std::min(envelope_, strength(age));
            samples[i] = static_cast<int16_t>((mid_ - low_) * (gain_per_volume_percent * volume) * gain);
            envelope_ = std::min(1.0f, gain + attack_step);
        }
    }

private:
    uint32_t seed_;
    float low_ = 0.0f;
    float mid_ = 0.0f;
    float envelope_ = 0.0f;
};

}  // namespace app::tuner_snow
