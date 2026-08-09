#pragma once

// "Still watching?" guard for TV auto-advance (field report 2026-08-09:
// viewer fell asleep mid-binge and the next-episode countdown carried
// playback all night). Auto-advance stays, but once kAutoAdvanceStreakLimit
// consecutive episodes have STARTED without any user input, the next
// episode end swaps the 8 s countdown for a "Still watching <series>?"
// card that waits kStillWatchingTimeoutSeconds for a press and otherwise
// stops playback through the screen's normal exit path (exit_pending_ ->
// origin_ -> the dispatcher's flush + leave()).
//
// Pure and header-only (the episode_logic.h / capture_session pattern):
// the streak accounting, the EOS decision, and every pinned string live
// here so test_media_browser_unit can pin the whole table on the Mac.
// PlaybackScreen owns WHEN the events fire; this owns WHAT they mean.

#include <string>

#include "media_browser/ui/episode_logic.h"  // EndOverlayModel / EndOverlayKind

namespace media_browser::ui {

// Consecutive auto-started episodes tolerated before asking. 3 means the
// countdown has driven ~2-3 hours of playback with no sign of life.
inline constexpr int kAutoAdvanceStreakLimit = 3;

// How long the prompt waits for a press before stopping playback. Long
// enough to find the remote across the room; short enough that a sleeping
// viewer gets one extra minute of audio, not a season.
inline constexpr int kStillWatchingTimeoutSeconds = 60;

// What the guard says should happen at a TV EOS whose end overlay would be
// the auto-advance countdown. Season-end cards never consult it — their
// flow already stops on its own.
enum class EosAdvance { AutoAdvance, Prompt };

// The streak state machine. streak = episodes in the CURRENT continuous
// playback session that started without the user proving presence.
// Transitions:
//   session start (manual play from the picker/detail) -> 0
//   auto-advance (countdown expiry)                    -> +1
//   any input handled during playback (pause, seek,
//   rotary, phone remote — same InputActions path)     -> 0
//   countdown "Play now" / prompt "Continue"           -> 0 (a press IS
//                                                          presence)
struct StillWatchingGuard {
    int streak = 0;

    void note_session_start()    { streak = 0; }
    void note_user_interaction() { streak = 0; }
    void note_auto_advance()     { ++streak; }

    // Consulted at the EOS edge BEFORE arming the countdown: has the
    // streak already exhausted the no-interaction allowance?
    EosAdvance decide() const {
        return streak >= kAutoAdvanceStreakLimit ? EosAdvance::Prompt
                                                 : EosAdvance::AutoAdvance;
    }

    // Timeout predicate for the visible prompt, pinned here so the screen
    // and the tests share one number.
    static bool prompt_timed_out(double shown_for_seconds) {
        return shown_for_seconds >=
               static_cast<double>(kStillWatchingTimeoutSeconds);
    }
};

// Pinned copy: "Still watching <series>?" — bare "Still watching?" when
// the series title is somehow empty (never render a dangling space).
inline std::string still_watching_title(const std::string& series_title) {
    if (series_title.empty()) return "Still watching?";
    return "Still watching " + series_title + "?";
}

// Converts the Countdown model decide_end_overlay produced into the
// prompt card, preserving next_index (what "Continue" plays) and the
// underlying card. The countdown's own title_line ("Next: SxEy · <title>")
// becomes the body, so the card still says what a Continue press starts.
// The "Stopping in N…" line is the screen's frame-timer's job, exactly
// like the countdown's "Starting in N…" — never static copy here.
inline EndOverlayModel make_still_watching_overlay(
        const EndOverlayModel& countdown, const std::string& series_title) {
    EndOverlayModel m = countdown;
    m.kind = EndOverlayKind::StillWatching;
    m.title_line = still_watching_title(series_title);
    m.body_line = countdown.title_line;
    m.primary_label = "Continue";
    m.has_primary = true;
    return m;
}

}  // namespace media_browser::ui
