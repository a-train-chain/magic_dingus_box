#pragma once

// The movie Detail screen's decisions, Renderer-free and I/O-free so
// test_media_browser_unit can assert on them
// (tests/media_browser/test_detail_logic.cpp): the page modes and their
// button rows, the quality-profile pick, the fetch / library-poll / worker
// drain verdicts, and every piece of text the page composes (meta line,
// availability readout, the in-library banners, header sub-info, footer).
// DetailScreen keeps the thread plumbing, the I/O (filesystem probes, the
// clients) and the drawing calls — the same split as
// series_detail_logic.h / series_detail_view.h for the TV page.
//
// Everything here was moved out of detail_screen.cpp verbatim in behaviour:
// same strings, same precedence, same float arithmetic order.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "media_browser/prowlarr/prowlarr_client.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_tone.h"
#include "media_browser/ui/mb_ui_utils.h"

namespace media_browser::ui {

// ---------- Modes and the action row ----------

// What the Detail screen is currently showing. Drives which action buttons
// are rendered and how SELECT resolves. (DetailScreen::Mode aliases this.)
enum class DetailMode {
    Loading,                // Fetching movie + profiles.
    Error,                  // Lookup failed — show Retry.
    NoTmdb,                 // tmdb_id == 0; no movie selected.
    NotInLibrary,           // [Add to Library]
    InLibraryNoFile,        // [Search Again] [Remove]
    InLibraryWithFile,      // [Play] [Remove]
};

// Abstract button id — one enum covers all modes. Only a subset is present
// in the button row at a time, determined by the current mode.
// (DetailScreen::Action aliases this.)
enum class DetailAction {
    AddToLibrary,
    SearchAgain,
    Remove,
    ConfirmRemove,   // Transient — Remove's second stage.
    Play,
    Retry,
    MoreInfo,        // Placeholder — future sub-screen with full trivia.
    PickSource,      // Manual release-picker (only when in library).
};

struct DetailButton {
    DetailAction action;
    std::string label;
};

// enter()'s cache test: a page that got past Loading/Error/NoTmdb holds
// real data, so re-entering the same movie reuses it instead of refetching.
inline bool detail_has_loaded_data(DetailMode mode) {
    return mode != DetailMode::Loading && mode != DetailMode::Error &&
           mode != DetailMode::NoTmdb;
}

// The button row for a mode. play_ready is the TRUE readiness predicate
// (file resolvable AND present on the host) — the screen evaluates it only
// for InLibraryWithFile, since it stats the disk. "Pick a source" is gated
// on the movie being in the library because Radarr's POST /api/v3/release
// needs an indexerId that only get_releases_for_movie(radarr_id) yields.
inline std::vector<DetailButton> decide_detail_buttons(DetailMode mode,
                                                       bool remove_pending,
                                                       bool play_ready) {
    std::vector<DetailButton> b;
    const DetailButton remove =
        remove_pending ? DetailButton{DetailAction::ConfirmRemove, "Confirm Remove"}
                       : DetailButton{DetailAction::Remove, "Remove"};
    switch (mode) {
        case DetailMode::Loading:
        case DetailMode::NoTmdb:
            break;  // No actions available.
        case DetailMode::Error:
            b.push_back({DetailAction::Retry, "Retry"});
            break;
        case DetailMode::NotInLibrary:
            b.push_back({DetailAction::AddToLibrary, "Add to Library"});
            b.push_back({DetailAction::MoreInfo, "More Info"});
            break;
        case DetailMode::InLibraryNoFile:
            b.push_back({DetailAction::SearchAgain, "Search Again"});
            b.push_back({DetailAction::PickSource, "Pick a source"});
            b.push_back(remove);
            break;
        case DetailMode::InLibraryWithFile:
            // Only offer Play when the file is TRULY ready — during the
            // brief import-copy window (or a mount hiccup) hasFile can be
            // true before the file is actually playable.
            if (play_ready) b.push_back({DetailAction::Play, "Play"});
            b.push_back({DetailAction::PickSource, "Pick a source"});
            b.push_back(remove);
            break;
    }
    return b;
}

// rebuild_buttons' focus clamp: never negative, never past the last button
// (an empty row leaves a non-negative focus alone).
inline int clamp_detail_focus(int focus, int button_count) {
    if (focus < 0) focus = 0;
    if (button_count > 0 && focus >= button_count) focus = button_count - 1;
    return focus;
}

// Color-coded bordered buttons: Play / Add → Ok (green); Search Again /
// Retry / Pick a source → Action (steel blue); Remove / Confirm → Warn
// (red); More Info → Neutral (dim).
inline chrome::ButtonKind detail_button_kind(DetailAction a) {
    switch (a) {
        case DetailAction::Play:
        case DetailAction::AddToLibrary:
            return chrome::ButtonKind::Ok;
        case DetailAction::SearchAgain:
        case DetailAction::Retry:
        case DetailAction::PickSource:
            return chrome::ButtonKind::Action;
        case DetailAction::Remove:
        case DetailAction::ConfirmRemove:
            return chrome::ButtonKind::Warn;
        case DetailAction::MoreInfo:
            break;
    }
    return chrome::ButtonKind::Neutral;
}

// SELECT's gate, ahead of the per-action dispatch. While a remove runs every
// action is a no-op (acting on a movie mid-deletion can only produce
// inconsistent state); likewise while an add runs.
enum class DetailActivateGate { Removing, Adding, Nothing, Dispatch };

inline DetailActivateGate decide_detail_activate(bool remove_in_flight,
                                                 bool add_in_flight,
                                                 int button_count, int focus) {
    if (remove_in_flight) return DetailActivateGate::Removing;
    if (add_in_flight) return DetailActivateGate::Adding;
    if (button_count == 0) return DetailActivateGate::Nothing;
    if (focus < 0 || focus >= button_count) return DetailActivateGate::Nothing;
    return DetailActivateGate::Dispatch;
}

// ---------- Profiles / preflight ----------

// Default to "Any" — most permissive profile, accepts whatever the indexer
// ships (popular older / public-domain titles only available as
// Bluray-720p on YTS otherwise silently fail to grab). Fallback order:
// "HD - 720p/1080p", "HD-1080p", any name containing "1080p", the first
// profile, else 0 (none available).
inline int pick_movie_quality_profile_id(const std::vector<QualityProfile>& profiles) {
    for (const auto& p : profiles) {
        if (p.name == "Any") return p.id;
    }
    for (const auto& p : profiles) {
        if (p.name == "HD - 720p/1080p") return p.id;
    }
    for (const auto& p : profiles) {
        if (p.name == "HD-1080p") return p.id;
    }
    for (const auto& p : profiles) {
        if (p.name.find("1080p") != std::string::npos) return p.id;
    }
    if (!profiles.empty()) return profiles.front().id;
    return 0;
}

// Add-to-Library disk preflight: WARN (never block) under 15 GB free on the
// library mount. available is nullopt when the stat failed (silently
// ignored — the kiosk doesn't own the mount lifecycle) and a 0 reading is
// ignored too, as before.
inline std::optional<std::string> low_space_warning(std::optional<std::uintmax_t> available) {
    constexpr int64_t kWarnFreeBytes = 15LL * 1024 * 1024 * 1024;  // 15 GB
    if (!available.has_value() || *available == 0) return std::nullopt;
    if (static_cast<int64_t>(*available) >= kWarnFreeBytes) return std::nullopt;
    const int gb_free = static_cast<int>(*available / (1024 * 1024 * 1024));
    return "Warning: only " + std::to_string(gb_free) +
           " GB free \xE2\x80\x94 large releases may fail to import";
}

// ---------- Fetch / poll / worker drains ----------

// The library record for this tmdb id, or nullptr.
inline const Movie* find_movie_by_tmdb(const std::vector<Movie>& library, int tmdb_id) {
    for (const auto& m : library) {
        if (m.tmdb_id == tmdb_id) return &m;
    }
    return nullptr;
}

// The page mode once TMDB answered: by the library match, if any.
inline DetailMode mode_for_library_match(const Movie* found) {
    if (found == nullptr) return DetailMode::NotInLibrary;
    return found->has_file ? DetailMode::InLibraryWithFile
                           : DetailMode::InLibraryNoFile;
}

// The library poll's import probe: the FIRST queue row for this movie
// decides — importing/importPending means the download finished and Radarr
// is copying it into the library.
inline bool queue_shows_import(const std::vector<QueueItem>& queue, int radarr_id) {
    for (const auto& qi : queue) {
        if (qi.movie_id != radarr_id) continue;
        return qi.tracked_download_state == "importing" ||
               qi.tracked_download_state == "importPending";
    }
    return false;
}

// Draining a quiet library poll. Only ever acts while still awaiting a file
// (a poll that landed after Retry / a different movie must not clobber the
// new state), and only on an answered poll with a record.
enum class LibraryPollStep { Ignore, FileLanded, StillWaiting };

inline LibraryPollStep decide_library_poll(DetailMode mode, bool ok, bool has_movie,
                                           bool movie_has_file) {
    if (mode != DetailMode::InLibraryNoFile) return LibraryPollStep::Ignore;
    if (!ok || !has_movie) return LibraryPollStep::Ignore;
    return movie_has_file ? LibraryPollStep::FileLanded
                          : LibraryPollStep::StillWaiting;
}

// Add worker drained: failed; succeeded for a movie no longer on screen
// (toast only — never register a watch or navigate over the new record);
// or succeeded here (toast, watch, refetch, go to Queue).
enum class AddDrain { Failed, OtherMovie, Success };

inline AddDrain decide_add_drain(bool ok, int current_tmdb_id, int add_tmdb_id) {
    if (!ok) return AddDrain::Failed;
    if (current_tmdb_id != add_tmdb_id) return AddDrain::OtherMovie;
    return AddDrain::Success;
}

// Remove worker drained: failed (banner the reason); succeeded for a movie
// no longer on screen (leave that page alone); or removed (invalidate and
// go to Library).
enum class RemoveDrain { Failed, OtherMovie, Removed };

inline RemoveDrain decide_remove_drain(bool ok, bool has_movie, int movie_radarr_id,
                                       int remove_radarr_id) {
    if (!ok) return RemoveDrain::Failed;
    if (has_movie && movie_radarr_id != remove_radarr_id) return RemoveDrain::OtherMovie;
    return RemoveDrain::Removed;
}

// Search-again worker drained: the in-page banner (same movie only) and
// whether to toast — a failure is toasted even after the user moved on:
// they asked for a search and none is running.
struct SearchDrainView {
    std::optional<std::string> banner;
    bool toast = false;
};

inline SearchDrainView decide_search_drain(bool ok, bool same_movie) {
    SearchDrainView v;
    if (ok) {
        if (same_movie) v.banner = "Search triggered";
        return v;
    }
    if (same_movie) v.banner = "Search failed";
    v.toast = true;
    return v;
}

inline const char* search_failed_toast() {
    return "Search didn't start \xE2\x80\x94 Radarr didn't answer; try again";
}

// ---------- Text ----------

// "2h 15m", "45m" (under an hour), "2h" (no minutes), or "N/A" when
// zero/missing.
inline std::string format_runtime(int minutes) {
    if (minutes <= 0) return "N/A";
    int h = minutes / 60;
    int m = minutes % 60;
    if (h == 0) return std::to_string(m) + "m";
    std::ostringstream os;
    os << h << "h";
    if (m > 0) os << " " << m << "m";
    return os.str();
}

// One decimal ("7.4"); empty when unrated.
inline std::string format_rating(double rating) {
    if (rating <= 0.0) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", rating);
    return buf;
}

// Compact vote count: 120 -> "120", 1234 -> "1.2k", 15000 -> "15k",
// 1500000 -> "1M".
inline std::string format_vote_count(int n) {
    if (n < 1000) return std::to_string(n);
    if (n < 10000) {
        int whole = n / 1000;
        int tenth = (n % 1000) / 100;
        std::ostringstream os;
        os << whole << "." << tenth << "k";
        return os.str();
    }
    if (n < 1000000) return std::to_string(n / 1000) + "k";
    return std::to_string(n / 1000000) + "M";
}

// Names joined with "  •  " (U+2022). Preserves order. Cast / directors.
inline std::string join_with_bullet(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += "  \xE2\x80\xA2  ";
        out += items[i];
    }
    return out;
}

// The page's display metadata: TMDB first; a page whose TMDB detail has no
// title falls back to the Radarr record (title, year, rating, runtime,
// overview, and the poster when TMDB gave none); "Untitled" last.
struct DetailDisplay {
    std::string title, tagline, overview, language;
    int year = 0, runtime = 0, vote_count = 0;
    double rating = 0.0;
    std::vector<std::string> genres, cast_top, directors;
    std::string poster_url;
};

inline DetailDisplay resolve_detail_display(const std::optional<TmdbMovieDetail>& tmdb,
                                            const std::optional<Movie>& movie) {
    DetailDisplay d;
    if (tmdb.has_value()) {
        d.title      = tmdb->title;
        d.tagline    = tmdb->tagline;
        d.overview   = tmdb->overview;
        d.language   = tmdb->original_language;
        d.year       = tmdb->year;
        d.runtime    = tmdb->runtime_minutes;
        d.rating     = tmdb->rating;
        d.vote_count = tmdb->vote_count;
        d.genres     = tmdb->genres;
        d.cast_top   = tmdb->cast_top;
        d.directors  = tmdb->directors;
        d.poster_url = tmdb->poster_path;
    }
    if (d.title.empty() && movie.has_value()) {
        d.title    = movie->title;
        d.year     = movie->year;
        d.rating   = movie->rating;
        d.runtime  = movie->runtime_minutes;
        d.overview = movie->overview;
        if (d.poster_url.empty()) d.poster_url = movie->poster_url;
    }
    if (d.title.empty()) d.title = "Untitled";
    return d;
}

// The Playback hand-off title: the rich TMDB title when there is one, else
// the Radarr record's.
inline std::string detail_play_title(const std::optional<TmdbMovieDetail>& tmdb,
                                     const Movie& movie) {
    if (tmdb.has_value() && !tmdb->title.empty()) return tmdb->title;
    return movie.title;
}

// Header sub-info: "1999  ·  BTN4 back", or just the back hint.
inline std::string detail_header_sub_info(int year) {
    if (year > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d  \xC2\xB7  BTN4 back", year);
        return buf;
    }
    return "BTN4 back";
}

// Meta line: "1999  •  2h 16m  •  EN" — each part only when known; the
// language code upper-cased for retro flair.
inline std::string detail_meta_line(int year, int runtime_minutes,
                                    const std::string& language) {
    std::ostringstream meta_os;
    bool first = true;
    if (year > 0) {
        meta_os << year;
        first = false;
    }
    std::string runtime_str = format_runtime(runtime_minutes);
    if (runtime_str != "N/A") {
        if (!first) meta_os << "  \xE2\x80\xA2  ";
        meta_os << runtime_str;
        first = false;
    }
    if (!language.empty()) {
        if (!first) meta_os << "  \xE2\x80\xA2  ";
        std::string lang_upper = language;
        for (auto& c : lang_upper) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        }
        meta_os << lang_upper;
    }
    return meta_os.str();
}

// "(25k votes)".
inline std::string detail_votes_text(int vote_count) {
    return "(" + format_vote_count(vote_count) + " votes)";
}

// The AVAILABILITY readout (Add path only). Border and text share the tone.
struct AvailabilityView {
    std::string body;
    MbTone tone = MbTone::Dim;
    float alpha = 0.85f;
};

// error is only read for Failed, summary only for Ready — the screen passes
// what it peeked for that state.
inline AvailabilityView availability_view(ProwlarrClient::State state,
                                          const std::string& error,
                                          const std::optional<ReleaseSummary>& summary) {
    AvailabilityView v;
    switch (state) {
        case ProwlarrClient::State::Idle:
        case ProwlarrClient::State::Searching:
            v.body = availability_searching_message();
            break;
        case ProwlarrClient::State::Failed:
            v.body = "Sources unavailable: " + error;
            v.tone = MbTone::Highlight2;
            v.alpha = 0.95f;
            break;
        case ProwlarrClient::State::Ready:
            if (!summary || summary->total_releases == 0) {
                v.body = "No sources found  \xE2\x80\xA2  "
                         "Add anyway and Radarr will keep watching";
                v.tone = MbTone::Highlight2;
            } else {
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "%d seeders (best)  \xE2\x80\xA2  "
                         "%d releases  \xE2\x80\xA2  "
                         "%d total seeders",
                         summary->best_seeders, summary->total_releases,
                         summary->total_seeders);
                v.body = buf;
                v.tone = MbTone::Highlight1;  // green
            }
            v.alpha = 0.95f;
            break;
    }
    return v;
}

// The VPN-down banner rides every mode with a download path (Add,
// re-search); a page without a health provider treats the VPN as healthy.
inline bool vpn_banner_applies(bool has_provider, bool healthy, DetailMode mode) {
    return has_provider && !healthy &&
           (mode == DetailMode::NotInLibrary || mode == DetailMode::InLibraryNoFile);
}

inline const char* vpn_down_banner_text() {
    return "VPN TUNNEL DOWN  \xE2\x80\xA2  Adds will queue but torrents "
           "won't transfer until the tunnel comes back";
}

// The in-library-no-file banner's three sub-states: importing (green,
// imminent), pre-release (red — anything indexers surface now is a scam
// upload), plain monitored (dim, informational).
struct BannerView {
    std::string text;
    MbTone tone = MbTone::Dim;
};

inline BannerView awaiting_file_banner(bool import_in_progress, bool prerelease) {
    if (import_in_progress) {
        return {"DOWNLOADED  \xE2\x80\xA2  Importing to library \xE2\x80\x94 "
                "ready to play in a few seconds",
                MbTone::Highlight1};
    }
    if (prerelease) {
        return {"IN THEATERS  \xE2\x80\xA2  No digital release "
                "exists yet \xE2\x80\x94 downloads found now are almost "
                "always fakes; the real one lands automatically",
                MbTone::Highlight2};
    }
    return {"MONITORED  \xE2\x80\xA2  Radarr re-checks indexers "
            "every 30 minutes and will auto-download when "
            "seeders appear",
            MbTone::Dim};
}

// The imported file's duration is wildly off the expected runtime.
inline std::string runtime_mismatch_text(int file_runtime_minutes, int runtime_minutes) {
    return "FILE LOOKS WRONG  \xE2\x80\xA2  " + std::to_string(file_runtime_minutes) +
           " min file vs " + std::to_string(runtime_minutes) +
           " min expected \xE2\x80\x94 probably not the real movie (use Remove, then "
           "re-add)";
}

// Error page copy. A keyless box reaches Detail through Search (Radarr's
// SkyHook proxy works without a TMDB key) and then fails here because
// get_movie() is TMDB-direct — name the real cause.
inline const char* detail_error_message(bool has_tmdb_key) {
    return has_tmdb_key ? "Couldn't fetch movie info from TMDB."
                        : "No TMDB key \xE2\x80\x94 add one in the Content Manager, "
                          "Media Browser tab.";
}

inline const char* directors_label(size_t count) {
    return count == 1 ? "DIRECTED BY" : "DIRECTORS";
}

// Smart-quoted tagline.
inline std::string quoted_tagline(const std::string& tagline) {
    return std::string("\xE2\x80\x9C") + tagline + "\xE2\x80\x9D";
}

inline std::vector<chrome::Hint> detail_footer_hints() {
    return {
        {chrome::HintIcon::Btn1Yellow,  "\xE2\x80\x94"},
        {chrome::HintIcon::Btn2Red,     "Exit"},
        {chrome::HintIcon::Btn3Green,   "\xE2\x80\x94"},
        {chrome::HintIcon::Btn4Black,   "Back"},
        {chrome::HintIcon::RotaryNav,   "Action"},
        {chrome::HintIcon::RotaryPress, "Confirm"},
    };
}

// ---------- Flex column (synopsis / cast / directors) line budgets ----------
//
// The right column's three wrapped blocks share the space down to the
// section divider. Synopsis expands into whatever remains after reserving a
// minimum (label + one line) for each cast/directors block that has content;
// cast then takes what is left minus the directors minimum; directors takes
// the final remainder. Each budget is at least one line. `space` is
// content_bottom - cursor_y at the moment the block is laid out; the
// subtraction order is the original's (float rounding included).

inline int flex_line_budget(float space, float line_h) {
    return std::max(1, static_cast<int>(space / line_h));
}

inline int synopsis_line_budget(float space, bool has_cast, bool has_directors,
                                float min_section_h, float top_pad, float line_h) {
    if (has_cast) space -= min_section_h;
    if (has_directors) space -= min_section_h;
    space -= top_pad;  // synopsis's own top pad
    return flex_line_budget(space, line_h);
}

inline int cast_line_budget(float space, bool has_directors, float min_section_h,
                            float top_pad, float label_h, float line_h) {
    if (has_directors) space -= min_section_h;
    space -= top_pad + label_h;  // own header overhead
    return flex_line_budget(space, line_h);
}

inline int directors_line_budget(float space, float top_pad, float label_h,
                                 float line_h) {
    space -= top_pad + label_h;
    return flex_line_budget(space, line_h);
}

}  // namespace media_browser::ui
