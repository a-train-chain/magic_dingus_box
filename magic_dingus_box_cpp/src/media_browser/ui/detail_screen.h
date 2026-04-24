#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "media_browser/radarr/radarr_types.h"
#include "media_browser/ui/mb_screen.h"

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Task 20: the Detail screen — real implementation.
//
// Layout:
//   - Top 35%: fanart/backdrop (dim + slightly-darkened colored poster-tint
//     placeholder until real image loading lands). Title + year + rating
//     overlaid at the bottom-left with the accent color. Runtime right-aligned.
//     "Press Menu to go back" hint in the top-right corner.
//   - Middle 40%: overview/synopsis text, wrapped at ~60% of screen width,
//     centered. Truncated to ~6 lines with trailing "...".
//   - Bottom 25%: context-sensitive action button row. Buttons depend on
//     whether the movie is in the library and whether the file is present:
//       - Not in library:              [Add to Library]
//       - In library, no file:         [Search Again] [Remove]
//       - In library, file present:    [Play]         [Remove]
//
// Interaction:
//   - LEFT/RIGHT (or ROTATE):      step between action buttons
//   - SELECT / ROTARY_CLICK:       activate focused button
//   - SETTINGS_MENU / BTN4:        back to Browse (MVP back-stack; a real
//                                  stack would track where we came from)
//
// Action behavior:
//   - [Add to Library]: picks HD-1080p quality profile (fallback to the
//     first profile) and calls radarr_.add_movie(tmdb_id, qp_id, true). On
//     success refreshes library lookup and re-derives button state so the
//     row flips to Search-Again / Remove immediately.
//   - [Search Again]: radarr_.trigger_search(radarr_id). Brief banner.
//   - [Remove]: two-stage confirmation. First SELECT changes button label
//     to "Confirm Remove" for 2 seconds. Second SELECT within that window
//     calls radarr_.remove_movie(radarr_id, false) and transitions back to
//     Screen::Library. Third-party press, or the 2s expiry, cancels.
//   - [Play]: for now just transitions to Screen::Library — real playback
//     hookup is Task 24.
//
// Error / loading states:
//   - Loading: "Loading..." centered while enter() is in flight.
//   - tmdb_id == 0 (shouldn't happen if the dispatcher handoff is correct):
//     centered "No movie selected" message, no actions.
//   - Lookup failure (Radarr offline): "Radarr service offline" with a
//     [Retry] button that re-runs enter().
class DetailScreen : public MbScreen {
public:
    explicit DetailScreen(RadarrClient& radarr);

    // Set the tmdb_id of the movie the detail screen should display. The
    // dispatcher in main.cpp calls this just before transitioning to this
    // screen, using the selected_tmdb_id() of whichever source screen (Browse
    // / Search) produced the selection. No-op if the id matches the current
    // one — avoids clobbering in-flight state on self-transitions.
    void set_tmdb_id(int tmdb_id) {
        if (tmdb_id != tmdb_id_) {
            tmdb_id_ = tmdb_id;
            needs_refresh_ = true;
        }
    }
    int tmdb_id() const { return tmdb_id_; }

    // Remember which screen we came from so BTN4 / SETTINGS_MENU returns
    // there instead of always dumping the user back to Browse. main.cpp
    // calls this on every transition INTO Detail, using the dispatcher's
    // current_mb_screen at the moment of transition. Library → Detail →
    // BTN4 should return to Library, Search → Detail → BTN4 should return
    // to Search (preserving query + results), etc.
    void set_origin(Screen s) { origin_ = s; }
    Screen origin() const { return origin_; }

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    // What the Detail screen is currently showing. Drives which action
    // buttons are rendered and how SELECT resolves.
    enum class Mode {
        Loading,                // Fetching movie + profiles.
        Error,                  // Lookup failed — show Retry.
        NoTmdb,                 // tmdb_id == 0; no movie selected.
        NotInLibrary,           // [Add to Library]
        InLibraryNoFile,        // [Search Again] [Remove]
        InLibraryWithFile,      // [Play] [Remove]
    };

    // Abstract button id — one enum covers all modes. Only a subset is
    // present in the button row at a time, determined by the current Mode.
    enum class Action {
        AddToLibrary,
        SearchAgain,
        Remove,
        ConfirmRemove,   // Transient — Remove's second stage.
        Play,
        Retry,
    };

    struct Button {
        Action action;
        std::string label;
    };

    // Rebuild buttons_ based on mode_ and remove_pending_. Resets focus_ if
    // it falls outside the new range.
    void rebuild_buttons();

    // Resolve tmdb_id_ against library and, if missing, lookup. Populates
    // movie_/hit_/mode_. Also fetches quality profiles (best-effort).
    void fetch();

    // Helpers that run on SELECT. Each returns the next Screen (often the
    // current one = Screen::Detail).
    Screen on_activate();
    Screen do_add_to_library();
    Screen do_search_again();
    Screen do_remove_stage1();
    Screen do_remove_confirm();
    Screen do_play();
    Screen do_retry();

    // Brief toast messages (e.g. "Search triggered", "Added to library").
    void show_banner(std::string text);

    // Pick the HD-1080p quality profile id. Falls back to the first profile
    // if none matches, or 0 if no profiles are available.
    int pick_quality_profile_id() const;

    RadarrClient& radarr_;
    int tmdb_id_ = 0;
    bool needs_refresh_ = false;

    // Screen to return to on BTN4 / SETTINGS_MENU. Default Browse preserves
    // legacy behavior for any callers that forget to set_origin().
    Screen origin_ = Screen::Browse;

    Mode mode_ = Mode::Loading;
    // If the movie is in the library we have the full Movie record (with
    // radarr_id, has_file, runtime, etc). Otherwise we only have the
    // MovieSearchHit from lookup().
    std::optional<Movie> movie_;
    std::optional<MovieSearchHit> hit_;
    std::vector<QualityProfile> profiles_;

    std::vector<Button> buttons_;
    int focus_ = 0;

    // Remove-button confirmation state. remove_pending_ is true between the
    // first SELECT on [Remove] and either the second SELECT (confirmation)
    // or the 2s expiry.
    bool remove_pending_ = false;
    std::chrono::steady_clock::time_point remove_pending_at_{};
    static constexpr int kRemovePendingMs = 2000;

    // Transient status banner (e.g. "Search triggered"). Cleared after a
    // short delay.
    std::string banner_;
    std::chrono::steady_clock::time_point banner_at_{};
    static constexpr int kBannerMs = 2000;
};

}  // namespace media_browser::ui
