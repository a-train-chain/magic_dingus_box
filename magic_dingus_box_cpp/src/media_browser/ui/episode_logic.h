#pragma once

// Pure decision core for TV playback (spec: 2026-08-02-tv-playback-design.md,
// Phase 3). Everything PlaybackScreen and SeriesDetailScreen must AGREE on —
// watched/resumable thresholds, what plays next, what the season-end card
// says — lives here, header-only and Renderer-free, so test_media_browser_unit
// can pin it on the Mac. Same rationale as series_detail_logic.h.
//
// The episode functions template on the episode type and touch ONLY
// .season_number / .episode_number / .has_file / .title (title is consumed
// by decide_end_overlay's countdown line): EpisodeInfo is defined by the
// Sonarr layer (Task 2), and depending on it here would make this header
// uncompilable on its own and untestable without the client. The tests compile
// the templates against a four-field fake — the minimal contract, enforced.

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "media_browser/media_ref.h"
#include "media_browser/ui/series_detail_logic.h"  // SeasonRow / SeasonState

namespace media_browser::ui {

// ---------- Thresholds ----------

// 92% of duration counts as watched: credits routinely occupy the last 5-8%
// of an episode, so requiring 100% would leave every episode "unwatched"
// forever. Resume only from >= 60 s in — restarting is cheaper than a
// mis-seek for anything shorter. Countdown and checkpoint cadence are pinned
// here so the screens and the store can never drift apart.
inline constexpr double kWatchedFraction = 0.92;
inline constexpr double kResumableMinSeconds = 60.0;
inline constexpr int kNextUpCountdownSeconds = 8;
inline constexpr int kCheckpointIntervalMs = 30000;

// duration_s <= 0 is "duration unknown" (stream not yet prerolled, or a
// corrupt row) — never watched, rather than a division blowup or a
// spurious true that would mark an unplayed episode as seen.
inline bool is_watched_position(double position_s, double duration_s) {
    if (duration_s <= 0) return false;
    return position_s / duration_s >= kWatchedFraction;
}

// Past the watched threshold "resume" degrades to "replay", so resumable
// explicitly excludes it — the two states are mutually exclusive by
// construction and every UI branch can trust that.
inline bool is_resumable_position(double position_s, double duration_s) {
    return position_s >= kResumableMinSeconds &&
           !is_watched_position(position_s, duration_s);
}

// ---------- Watch map ----------

// The (season, episode) key SeriesDetail and Playback share. Per-series maps
// only — the series identity (MediaRef) lives OUTSIDE the key, in
// WatchIdentity below, so a map never mixes shows.
struct WatchKey {
    int season = 0;
    int episode = 0;
    bool operator==(const WatchKey& o) const {
        return season == o.season && episode == o.episode;
    }
};

// Injective: season occupies the high 32 bits, episode the low 32, so no
// two (season, episode) pairs can collide before std::hash mixes them.
// Both fields pass through unsigned: left-shifting a negative signed value
// is UB in C++17, so a (never-expected) negative season must not reach <<.
struct WatchKeyHash {
    size_t operator()(const WatchKey& k) const noexcept {
        return std::hash<unsigned long long>{}(
            (static_cast<unsigned long long>(static_cast<unsigned>(k.season)) << 32) |
            static_cast<unsigned long long>(static_cast<unsigned>(k.episode)));
    }
};

// The slice of a watch_state row the decision logic needs — deliberately
// not the DB row type, so this header never learns about sqlite.
struct WatchRowLite {
    double position_s = 0;
    double duration_s = 0;
    bool watched = false;
};

// The alias every later task uses; spelling the map out longhand in five
// screens is how two of them end up with different hashers.
using watch_map = std::unordered_map<WatchKey, WatchRowLite, WatchKeyHash>;

// Which piece of media a watch row belongs to. Declared HERE, once —
// PlaybackScreen's origin, DetailScreen's PlayTarget and SeriesDetail's
// SeriesPlayTarget all carry this one type rather than three per-screen
// redefinitions that drift. MediaRef (not a bare tmdb id) because the movie
// and TV id spaces overlap completely; season/episode stay 0 for movies.
struct WatchIdentity {
    MediaRef ref;
    int season = 0;
    int episode = 0;
};

// ---------- Time formatting ----------

// H:MM:SS when >= 1 hour, else M:SS — no leading zero on the leading unit,
// two digits on trailing units (7623 -> "2:07:03", 143 -> "2:23"). Task 4's
// resume toast and Task 6's episode glyph both use it. Renderer::format_time
// is NOT suitable: MM:SS-only with unbounded minutes ("127:03") and it needs
// a Renderer&, which this header must never see.
inline std::string format_position_hms(double position_s) {
    long long total = position_s > 0 ? static_cast<long long>(position_s) : 0;
    const long long h = total / 3600;
    const long long m = (total % 3600) / 60;
    const long long s = total % 60;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
    }
    return buf;
}

// ---------- Phone-remote now-playing subtitle ----------

// "S2E5" / "S2E5 · <episode title>" — the now_playing.subtitle shape the
// kiosk publishes to kiosk_status.json during TV playback (the phone remote
// renders it under the series title). Pure so the shape is pinned by
// test_episode_logic on the Mac; the middle dot matches the display_title
// separator SeriesDetail and advance_to_next_episode already use.
inline std::string format_now_playing_episode(int season, int episode,
                                              const std::string& episode_title) {
    std::string s = "S" + std::to_string(season) + "E" + std::to_string(episode);
    if (!episode_title.empty()) s += " \xC2\xB7 " + episode_title;
    return s;
}

// ---------- Next up ----------

// What plays after `current` — or, with current == nullptr, what a fresh
// "watch next" from the series screen should pick. episodes are assumed
// sorted (season asc, episode asc); season 0 (specials) is ignored, matching
// merge_season_rows.
//
// The two forms consult the watch map DIFFERENTLY, and that is the point:
// - current != nullptr (auto-play at episode end): the watch map is IGNORED.
//   Sequential playback is the natural order of television — a mid-binge
//   rewatch must not silently skip ahead over previously-watched episodes.
// - current == nullptr ("what should I watch next"): skips episodes marked
//   watched, returning the first unwatched episode with a file.
// Returns a pointer into `episodes`, or nullptr when nothing qualifies.
template <class Ep>
const Ep* next_up(const std::vector<Ep>& episodes,
                  const std::unordered_map<WatchKey, WatchRowLite, WatchKeyHash>& watch,
                  const Ep* current) {
    if (current != nullptr) {
        for (const auto& e : episodes) {
            if (e.season_number == 0) continue;
            const bool after =
                e.season_number > current->season_number ||
                (e.season_number == current->season_number &&
                 e.episode_number > current->episode_number);
            if (!after) continue;
            if (e.has_file) return &e;
        }
        return nullptr;
    }
    for (const auto& e : episodes) {
        if (e.season_number == 0) continue;
        if (!e.has_file) continue;
        auto it = watch.find(WatchKey{e.season_number, e.episode_number});
        if (it != watch.end() && it->second.watched) continue;
        return &e;
    }
    return nullptr;
}

// ---------- Season end ----------

enum class SeasonEndKind { NextEpisode, OfferNextSeason, Downloading, SeriesDone };

// BOTH season fields are set in every non-NextEpisode outcome (next_season
// is 0 only when no next row exists). The pinned copy needs both: "Season N
// finished" uses the FINISHED season, "Season N is on its way" uses the NEXT
// (downloading) season — deriving one from the other as finished+1 breaks on
// non-contiguous season numbering.
struct SeasonEndCard {
    SeasonEndKind kind;
    int finished_season = 0;
    int next_season = 0;
};

// What the end-of-episode overlay offers once `finished` completes.
// Precedence, in order:
//   1. next_up says another episode is playable -> NextEpisode (the
//      countdown overlay; no card copy involved).
//   2. No next row at all -> SeriesDone.
//   3. Next row is actively Downloading -> Downloading (freshest signal).
//   4. Next row unmonitored -> OfferNextSeason (the upsell).
//   5. Monitored with no files and not downloading -> Downloading: a search
//      is in flight or pending, and "on its way" is the honest copy.
//   6. episode_file_count > 0 yet next_up found nothing -> the rows and the
//      episode list disagree (stale statistics). SeriesDone is the honest
//      fallback: never promise an episode we cannot actually start.
template <class Ep>
SeasonEndCard season_end_card(
        const std::vector<SeasonRow>& rows, const std::vector<Ep>& episodes,
        const std::unordered_map<WatchKey, WatchRowLite, WatchKeyHash>& watch,
        const Ep& finished) {
    SeasonEndCard card;
    card.finished_season = finished.season_number;
    if (next_up(episodes, watch, &finished) != nullptr) {
        card.kind = SeasonEndKind::NextEpisode;
        return card;
    }
    const SeasonRow* next_row = nullptr;
    for (const auto& r : rows) {
        if (r.season_number <= finished.season_number) continue;
        if (next_row == nullptr || r.season_number < next_row->season_number)
            next_row = &r;
    }
    if (next_row == nullptr) {
        card.kind = SeasonEndKind::SeriesDone;
        card.next_season = 0;
        return card;
    }
    card.next_season = next_row->season_number;
    if (next_row->state == SeasonState::Downloading) {
        card.kind = SeasonEndKind::Downloading;
    } else if (!next_row->monitored) {
        card.kind = SeasonEndKind::OfferNextSeason;
    } else if (next_row->episode_file_count == 0) {
        card.kind = SeasonEndKind::Downloading;
    } else {
        card.kind = SeasonEndKind::SeriesDone;
    }
    return card;
}

// Pinned copy per kind. NextEpisode renders the countdown overlay, never a
// titled card, so it has no copy here. No default: — -Wswitch catches a new
// enumerator.
inline std::string season_end_title(const SeasonEndCard& card,
                                    const std::string& /*series_title*/) {
    switch (card.kind) {
        case SeasonEndKind::NextEpisode:
            return "";
        case SeasonEndKind::OfferNextSeason:
            return "Season " + std::to_string(card.finished_season) + " finished";
        case SeasonEndKind::Downloading:
            // The NEXT (downloading) season, not the finished one.
            return "Season " + std::to_string(card.next_season) + " is on its way";
        case SeasonEndKind::SeriesDone:
            return "That's everything!";
    }
    return "";  // unreachable; keeps -Wreturn-type quiet without a default:
}

inline std::string season_end_button_label(const SeasonEndCard& card) {
    if (card.kind == SeasonEndKind::OfferNextSeason)
        return "Start Season " + std::to_string(card.next_season);
    return "Done";
}

// ---------- End-of-episode overlay model ----------

// What PlaybackScreen shows when a TV episode reaches EOS. None is the
// screen's idle state; Countdown is the 8-second next-episode overlay;
// Card is the season-end card (offer / downloading / series done);
// StillWatching is the auto-advance guard prompt (still_watching.h) that
// replaces the Countdown once too many episodes have started without any
// user input. decide_end_overlay never produces StillWatching — the
// screen swaps a Countdown for it via make_still_watching_overlay when
// the streak guard says to ask.
enum class EndOverlayKind { None, Countdown, Card, StillWatching };

// Fully-resolved overlay content: every pinned string the screen renders
// is composed HERE (Mac-tested), never inline in the kiosk TU. next_index
// is the POSITION of the next episode in the episodes vector handed to
// decide_end_overlay (-1 when there is no next episode) — an index, not a
// pointer, so the model stays typed, copyable and testable, and the screen
// pairs it with its index-aligned host-path vector.
//
// has_primary marks a primary ACTION beyond dismissal: the countdown's
// "Play now" and the season-end card's "Start Season N" upsell. The
// Downloading / SeriesDone cards carry only the dismissing "Done" label
// (primary_label still holds it — the card renders one button either way;
// has_primary tells the screen whether SELECT fires an intent or just
// closes).
struct EndOverlayModel {
    EndOverlayKind kind = EndOverlayKind::None;
    std::string title_line;
    std::string body_line;
    std::string primary_label;
    bool has_primary = false;
    SeasonEndCard card{SeasonEndKind::SeriesDone, 0, 0};
    int next_index = -1;
};

// Resolves the whole end-of-episode overlay for `finished`:
//   - next_up finds a playable next episode -> Countdown.
//     title_line = "Next: S<season>E<episode> · <title>" (the middle dot is
//     UTF-8 \xC2\xB7); body_line stays empty — the "Starting in N…" line is
//     the screen's frame-timer's job, not static copy.
//   - otherwise -> Card, with the season_end_title / season_end_button_label
//     strings and per-kind body copy:
//       OfferNextSeason -> "Start the Season <next_season> download?"
//       Downloading     -> "Check the Queue for progress."
//       SeriesDone      -> ""
// Touches ep.title in addition to the three next_up fields — the template
// contract grows to four names here, enforced by the test fake.
template <class Ep>
EndOverlayModel decide_end_overlay(
        const std::vector<SeasonRow>& rows, const std::vector<Ep>& episodes,
        const std::unordered_map<WatchKey, WatchRowLite, WatchKeyHash>& watch,
        const Ep& finished, const std::string& series_title) {
    EndOverlayModel m;
    m.card = season_end_card(rows, episodes, watch, finished);
    if (m.card.kind == SeasonEndKind::NextEpisode) {
        // season_end_card only returns NextEpisode when next_up found one,
        // so `next` is non-null by construction; the guard keeps a future
        // regression from dereferencing null instead of degrading to Card.
        const Ep* next = next_up(episodes, watch, &finished);
        if (next != nullptr) {
            m.kind = EndOverlayKind::Countdown;
            m.next_index = static_cast<int>(next - episodes.data());
            m.title_line = "Next: S" + std::to_string(next->season_number) +
                           "E" + std::to_string(next->episode_number) +
                           " \xC2\xB7 " + next->title;
            m.primary_label = "Play now";
            m.has_primary = true;
            return m;
        }
        m.card.kind = SeasonEndKind::SeriesDone;  // honest fallback
    }
    m.kind = EndOverlayKind::Card;
    m.title_line = season_end_title(m.card, series_title);
    switch (m.card.kind) {
        case SeasonEndKind::NextEpisode:
            break;  // unreachable: handled above
        case SeasonEndKind::OfferNextSeason:
            m.body_line = "Start the Season " +
                          std::to_string(m.card.next_season) + " download?";
            break;
        case SeasonEndKind::Downloading:
            m.body_line = "Check the Queue for progress.";
            break;
        case SeasonEndKind::SeriesDone:
            m.body_line = "";
            break;
    }
    m.primary_label = season_end_button_label(m.card);
    m.has_primary = (m.card.kind == SeasonEndKind::OfferNextSeason);
    return m;
}

}  // namespace media_browser::ui
