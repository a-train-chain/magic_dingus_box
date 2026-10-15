#pragma once

// PlaybackScreen's remaining pure decisions, Renderer-free and I/O-free so
// test_media_browser_unit can assert on them
// (tests/media_browser/test_playback_view.cpp): the phone-remote
// now-playing triple, HUD and title-marquee fades, the rotary seek curve,
// the finished-episode lookup, the end-overlay SELECT table and countdown,
// the end-card height, the quick-add profile pick and outcome copy, and the
// footer. episode_logic.h / still_watching.h keep the TV end-of-episode
// model; PlaybackScreen keeps the pipeline, the workers and the drawing.
//
// Everything here was moved out of playback_screen.cpp verbatim in
// behaviour: same strings, same thresholds, same arithmetic.

#include <string>
#include <vector>

#include "media_browser/radarr/radarr_types.h"
#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/ui/episode_logic.h"
#include "media_browser/ui/mb_chrome.h"

namespace media_browser::ui {

// ---------- Phone-remote now-playing ----------

struct NowPlayingStatus {
    std::string kind;      // "tv" / "movie"
    std::string title;
    std::string subtitle;
};

// TV = a Tv watch identity AND a series title; an identity-less TV file
// degrades to the movie shape (movie_title is the full display title there).
// The TV subtitle names the episode from the session's vector ("S2E5 · …"),
// a bare code when it is not found; the movie subtitle is the year, if known.
inline NowPlayingStatus now_playing_status(bool tv_identity, int season, int episode,
                                           const std::vector<EpisodeInfo>& episodes,
                                           const std::string& series_title,
                                           const std::string& movie_title, int year) {
    NowPlayingStatus s;
    if (tv_identity && !series_title.empty()) {
        s.kind = "tv";
        s.title = series_title;
        std::string ep_title;
        for (const auto& e : episodes) {
            if (e.season_number == season && e.episode_number == episode) {
                ep_title = e.title;
                break;
            }
        }
        s.subtitle = format_now_playing_episode(season, episode, ep_title);
        return s;
    }
    s.kind = "movie";
    s.title = movie_title;
    s.subtitle = year > 0 ? std::to_string(year) : std::string{};
    return s;
}

// ---------- Fades ----------

// HUD alpha: full while paused; otherwise full until the last `fade_ms` of
// its visibility window, then a linear fade to 0. expired = the window has
// passed; remaining_ms = whole milliseconds left otherwise.
inline float hud_alpha_for(bool paused, bool expired, long long remaining_ms,
                           int fade_ms) {
    if (paused) return 1.0f;
    if (expired) return 0.0f;
    if (remaining_ms > fade_ms) return 1.0f;
    return static_cast<float>(remaining_ms) / static_cast<float>(fade_ms);
}

// The "NOW PLAYING" marquee: linear fade over its last 500 ms.
inline float title_marquee_alpha(long long remaining_ms) {
    return remaining_ms < 500 ? static_cast<float>(remaining_ms) / 500.0f : 1.0f;
}

inline std::string now_playing_heading(const std::string& title) {
    std::string heading = "NOW PLAYING";
    if (!title.empty()) heading += " \xE2\x80\x94 " + title;
    return heading;
}

// ---------- Input ----------

// The rotary scrub curve: 5 s at low velocity up to 120 s at full speed
// (velocity²) — the playlist curve scaled for ~10x longer movies.
inline double rotary_seek_seconds(double velocity) {
    return 5.0 + 115.0 * (velocity * velocity);
}

// Where the episode that just ended sits in the session's vector: the
// cached index when it still matches the identity, else a linear search;
// -1 = not found (a stale context — movie-style exit).
inline int finished_episode_index(const std::vector<EpisodeInfo>& episodes,
                                  int cached_index, int season, int episode) {
    if (cached_index >= 0 && cached_index < static_cast<int>(episodes.size()) &&
        episodes[static_cast<size_t>(cached_index)].season_number == season &&
        episodes[static_cast<size_t>(cached_index)].episode_number == episode) {
        return cached_index;
    }
    for (size_t i = 0; i < episodes.size(); ++i) {
        if (episodes[i].season_number == season &&
            episodes[i].episode_number == episode) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// SELECT while an end-of-episode overlay is up. The still-watching prompt
// is checked BEFORE has_primary: its model carries has_primary for the
// button chrome, and reading Continue as a season intent would be wrong.
enum class EndOverlaySelect { PlayNext, Continue, StartSeason, Done };

inline EndOverlaySelect decide_end_overlay_select(EndOverlayKind kind, bool has_primary) {
    if (kind == EndOverlayKind::Countdown) return EndOverlaySelect::PlayNext;
    if (kind == EndOverlayKind::StillWatching) return EndOverlaySelect::Continue;
    if (has_primary) return EndOverlaySelect::StartSeason;
    return EndOverlaySelect::Done;
}

// "Starting in N…" / "Stopping in N…": whole seconds left of `total_s`,
// never below 1 (update() acts at expiry).
inline int countdown_remaining_seconds(int total_s, long long elapsed_ms) {
    int remaining = total_s - static_cast<int>(elapsed_ms / 1000);
    return remaining < 1 ? 1 : remaining;
}

// The centered end card's height: title, optional body, then the action
// area (button for the season card, body + "Stopping in" + button for the
// still-watching prompt, "Starting in" + hint row for the countdown).
inline int end_card_height(EndOverlayKind kind, bool has_body, int pad_y) {
    if (kind == EndOverlayKind::Card) {
        return pad_y + 30 + (has_body ? 28 : 0) + 16 + 44 + pad_y;
    }
    if (kind == EndOverlayKind::StillWatching) {
        return pad_y + 30 + (has_body ? 28 : 0) + 28 + 16 + 44 + pad_y;
    }
    return pad_y + 30 + 28 + 16 + 24 + pad_y;
}

// ---------- Quick-add ----------

// The overlay's quick-add profile: "Any", else "HD - 720p/1080p", else the
// first profile, else 0 (none). (Shorter than Detail's ladder, by design of
// the original.)
inline int pick_quick_add_profile_id(const std::vector<QualityProfile>& profiles) {
    int qp = 0;
    for (const auto& p : profiles) {
        if (p.name == "Any") { qp = p.id; break; }
    }
    if (qp == 0) {
        for (const auto& p : profiles) {
            if (p.name == "HD - 720p/1080p") { qp = p.id; break; }
        }
    }
    if (qp == 0 && !profiles.empty()) qp = profiles.front().id;
    return qp;
}

// A refused add: Radarr answers HTTP 400 "...already been added..." for a
// title already in the library.
inline std::string quick_add_failure_toast(const std::string& last_error) {
    if (last_error.find("already") != std::string::npos ||
        last_error.find("Already") != std::string::npos) {
        return "Already in library";
    }
    return "Couldn\xe2\x80\x99t add \xe2\x80\x94 try again";
}

// ---------- Footer ----------

inline std::vector<chrome::Hint> playback_footer_hints() {
    return {
        {chrome::HintIcon::Btn1Yellow,  "\xE2\x88\x92" "10s"},  // −10s
        {chrome::HintIcon::Btn2Red,     "Pause/Play"},
        {chrome::HintIcon::Btn3Green,   "+10s"},
        {chrome::HintIcon::Btn4Black,   "Back"},
        {chrome::HintIcon::RotaryNav,   "Scrub"},
        {chrome::HintIcon::RotaryPress, "Open Menu"},
    };
}

}  // namespace media_browser::ui
