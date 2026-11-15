// tube/tube_gpu_timing.h - turns per-pass GPU times into one journal line every 10 s.
// Pure: the caller supplies the clock (seconds) and the per-frame milliseconds.
#pragma once

#include <array>
#include <cstdio>
#include <optional>
#include <string>

namespace tube {

class PassTimingAverager {
public:
    enum class Pass { quarter, glow, signal, lines, output, history, probe };

    void add(Pass pass, double ms) {
        const auto i = static_cast<std::size_t>(pass);
        frame_ms_[i] += ms;
        frame_ran_[i] = true;
    }

    // Folds the passes added since the last call into the window as one frame.
    void end_frame() {
        for (std::size_t i = 0; i < kPasses; ++i) {
            if (!frame_ran_[i]) continue;
            sum_[i] += frame_ms_[i];
            ++ran_[i];
        }
        frame_ms_.fill(0.0);
        frame_ran_.fill(false);
        ++frames_;
    }

    // The first call starts the window. Every 10 s after, one line; the average of a
    // pass is over the frames it ran in ("-" when none), the total is their sum.
    std::optional<std::string> take_line(double now_seconds) {
        if (!started_) { started_ = true; window_start_ = now_seconds; return std::nullopt; }
        if (now_seconds - window_start_ < 10.0) return std::nullopt;
        window_start_ = now_seconds;
        const unsigned long frames = frames_;
        std::array<double, kPasses> avg{};
        std::array<bool, kPasses> ran{};
        double total = 0.0;
        for (std::size_t i = 0; i < kPasses; ++i) {
            ran[i] = ran_[i] > 0;
            if (ran[i]) { avg[i] = sum_[i] / static_cast<double>(ran_[i]); total += avg[i]; }
        }
        sum_.fill(0.0);
        ran_.fill(0);
        frames_ = 0;
        if (frames == 0) return std::nullopt;
        static constexpr const char* names[kPasses] = {"quarter", "glow", "signal", "lines",
                                                       "output", "history", "probe"};
        std::string line = "tube gpu: " + std::to_string(frames) + " frames:";
        char buf[32];
        for (std::size_t i = 0; i < kPasses; ++i) {
            line += ' ';
            line += names[i];
            line += ' ';
            if (ran[i]) { std::snprintf(buf, sizeof buf, "%.2f", avg[i]); line += buf; }
            else line += '-';
        }
        std::snprintf(buf, sizeof buf, " = %.2f ms", total);
        line += buf;
        return line;
    }

private:
    static constexpr std::size_t kPasses = 7;
    std::array<double, kPasses> frame_ms_{}, sum_{};
    std::array<bool, kPasses> frame_ran_{};
    std::array<unsigned long, kPasses> ran_{};
    unsigned long frames_ = 0;
    double window_start_ = 0.0;
    bool started_ = false;
};

}  // namespace tube
