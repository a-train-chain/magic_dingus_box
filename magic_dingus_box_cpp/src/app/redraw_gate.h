#pragma once

// Redraw-only-when-needed for the kiosk main loop. Pure logic, time
// injected, unit-tested on the Mac (tests/app/test_redraw_gate.cpp).
//
// WHY: the loop rendered, swapped and page-flipped every vblank even when
// nothing on screen could change. On the idle main menu that is a full
// 720p/1080p frame of CPU + GPU work and ~0.5 GB/s of memory bandwidth,
// 60 times a second, for an identical picture — on a box that mostly sits
// on that menu.
//
// HOW (deliberately conservative): the loop still runs every iteration —
// input, GPIO, phone-remote queues, status file, systemd watchdog — and
// only the GL draw + eglSwapBuffers + page flip are skipped. Skipping is
// opt-in per screen (is_static_main_menu): the main menu, the Settings
// menu when open and idle (SettingsMenuManager::is_static_for_redraw), and
// the Media Browser screens that opt out of continuous drawing
// (MbScreen::wants_continuous_redraw: Browse, Search, Library, Detail,
// SeriesDetail when nothing on them animates). Every other screen — video,
// games, MB Playback/Queue, the wizard, pairing, keyboard — asks for
// continuous drawing exactly as before. Within that state a frame is still
// drawn when:
//   - any input arrived this iteration (no added input latency);
//   - the drawn content's signature changed (selection, blink phase,
//     status text ... — see ContentSignature);
//   - activity just ENDED (one settle frame: the last active frame can be
//     an in-between state, e.g. a menu 95% closed);
//   - max_idle has passed since the last draw. This is the safety net: a
//     dirty source nobody wired up shows as slight latency, never as a
//     frozen screen.
// The bare main menu with CRT flicker/interlacing on is a half-way case
// (is_crt_field_rate_main_menu): those shaders only change the picture once
// per interlace field, so it is drawn every other vblank (30 fps at 60 Hz)
// rather than every vblank — see RedrawInputs::crt_field_rate,
// utils::field_rate_skip_sleep and ui/crt_time.h.
// Every drawn frame is a full clear + redraw (nothing reads back the
// previous back buffer), so a frame drawn after a skip streak is correct
// whatever EGL buffer it lands in.
//
// Kill switch: MDB_REDRAW_GATE=0 in the kiosk's environment, read once at
// startup — disabled, every iteration draws (pre-gate behavior).

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace app {

// What is on screen this iteration, reduced to the facts that decide
// whether the picture can change without input.
struct MainMenuActivity {
    bool intro = false;             // intro video showing / fading / not complete
    bool video = false;             // playback, playlist switch, pipeline playing
    bool media_browser = false;     // MB owns the screen (not opted in yet)
    bool settings_menu = false;     // open/opening/closing (incl. wizard, pairing)
    bool keyboard = false;          // on-screen keyboard (blinking cursor)
    bool ui_fade = false;           // menu fade or the post-game fade-up
    bool transient_overlay = false; // toast shown/queued, error banner, volume
                                    // slider, seek bar, BTN4 held, game loading
    bool crt_time_effects = false;  // flicker / interlacing: time-animated shaders
};

// The one state the gate may skip in. Anything not listed above is, by
// construction, static between inputs except what ContentSignature covers.
bool is_static_main_menu(const MainMenuActivity& a);

// The bare main menu whose ONLY animation is the CRT time effects
// (flicker / interlacing). Those shaders change the picture once per
// interlace field — 30 times a second — so the gate draws it at field rate
// (RedrawInputs::crt_field_rate) instead of every vblank. Everything that
// makes the menu non-static besides CRT still means continuous drawing.
bool is_crt_field_rate_main_menu(const MainMenuActivity& a);

struct RedrawInputs {
    bool input_event = false;
    bool video_frame = false;
    bool animation_active = false;
    bool screen_requests_continuous = false;
    bool forced = false;
    // Hash of everything the static screen draws that can change without
    // an input or activity flag (e.g. the selection blink phase).
    uint64_t content_signature = 0;
    // CRT time effects on an otherwise static screen: draw every OTHER
    // iteration (the one after a skipped one) — with the loop's
    // field-rate pacing (utils::field_rate_skip_sleep) that is every other
    // vblank, 30 fps at 60 Hz. Any other reason to draw (input, signature,
    // forced, activity) still draws on the iteration it happens. The
    // renderer pairs this with ui::crt_render_field so each drawn frame
    // shows the opposite interlace field to the one before.
    bool crt_field_rate = false;
};

class RedrawGate {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds kDefaultMaxIdle{250};
    static constexpr std::chrono::seconds kReportInterval{60};

    explicit RedrawGate(bool enabled,
                        std::chrono::milliseconds max_idle = kDefaultMaxIdle);

    // Call once per loop iteration; true = render, swap and present.
    bool should_draw(const RedrawInputs& in, Clock::time_point now);

    bool enabled() const { return enabled_; }

    struct Report {
        uint64_t drawn = 0;
        uint64_t skipped = 0;
        // Iterations spent in CRT field-rate mode (subset of drawn+skipped).
        uint64_t crt_field_rate = 0;
    };
    // Counts for the window since the previous report, once per
    // kReportInterval; nullopt otherwise. The first call starts the window.
    std::optional<Report> take_report(Clock::time_point now);

private:
    bool enabled_;
    std::chrono::milliseconds max_idle_;
    bool has_drawn_ = false;
    bool active_last_ = false;
    Clock::time_point last_draw_{};
    uint64_t last_signature_ = 0;
    bool drew_last_ = false;

    bool report_started_ = false;
    Clock::time_point report_start_{};
    Report window_{};
};

// MDB_REDRAW_GATE value -> enabled. Unset/empty/anything else = ON;
// "0", "off", "false", "no" (any case) = OFF.
bool redraw_gate_enabled_from_env(const char* value);

// Order-sensitive 64-bit FNV-1a accumulator for RedrawInputs::content_signature.
class ContentSignature {
public:
    void add(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            mix(static_cast<unsigned char>(v >> (i * 8)));
        }
    }
    void add(const std::string& s) {
        add(static_cast<uint64_t>(s.size()));
        for (unsigned char c : s) mix(c);
    }
    uint64_t value() const { return h_; }

private:
    void mix(unsigned char c) {
        h_ ^= c;
        h_ *= 1099511628211ull;
    }
    uint64_t h_ = 1469598103934665603ull;
};

}  // namespace app
