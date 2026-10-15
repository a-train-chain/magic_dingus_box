#include "app/redraw_gate.h"

#include <algorithm>
#include <cctype>

namespace app {

bool is_static_main_menu(const MainMenuActivity& a) {
    return !(a.intro || a.video || a.media_browser || a.settings_menu ||
             a.keyboard || a.ui_fade || a.transient_overlay ||
             a.crt_time_effects);
}

bool is_crt_field_rate_main_menu(const MainMenuActivity& a) {
    MainMenuActivity without_crt = a;
    without_crt.crt_time_effects = false;
    return a.crt_time_effects && is_static_main_menu(without_crt);
}

RedrawGate::RedrawGate(bool enabled, std::chrono::milliseconds max_idle)
    : enabled_(enabled), max_idle_(max_idle) {}

bool RedrawGate::should_draw(const RedrawInputs& in, Clock::time_point now) {
    const bool active = in.input_event || in.video_frame ||
                        in.animation_active || in.screen_requests_continuous ||
                        in.forced;

    bool draw = true;
    if (enabled_ && has_drawn_) {
        draw = active ||
               active_last_ ||                      // settle frame
               in.content_signature != last_signature_ ||
               (in.crt_field_rate && !drew_last_) ||
               now - last_draw_ >= max_idle_;
    }
    active_last_ = active;

    if (draw) {
        has_drawn_ = true;
        last_draw_ = now;
        last_signature_ = in.content_signature;
        ++window_.drawn;
    } else {
        ++window_.skipped;
    }
    if (in.crt_field_rate) ++window_.crt_field_rate;
    drew_last_ = draw;
    return draw;
}

std::optional<RedrawGate::Report> RedrawGate::take_report(Clock::time_point now) {
    if (!report_started_) {
        report_started_ = true;
        report_start_ = now;
        window_ = Report{};
        return std::nullopt;
    }
    if (now - report_start_ < kReportInterval) return std::nullopt;
    const Report r = window_;
    window_ = Report{};
    report_start_ = now;
    return r;
}

bool redraw_gate_enabled_from_env(const char* value) {
    if (value == nullptr) return true;
    std::string v(value);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return !(v == "0" || v == "off" || v == "false" || v == "no");
}

}  // namespace app
