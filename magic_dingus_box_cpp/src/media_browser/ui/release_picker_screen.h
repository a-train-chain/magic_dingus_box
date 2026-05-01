#pragma once

// ReleasePickerScreen — manual override of Radarr's auto-pick.
//
// Data flow: Detail screen invokes `set_candidates()` with releases sourced
// from ProwlarrClient::get_last_releases() (already cached on Detail screen
// when its availability search completed). The picker sorts, decorates, and
// displays them. SELECT calls RadarrClient::grab_release(); BACK returns
// to Detail.

#include "media_browser/ui/mb_screen.h"
#include "platform/input_manager.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace media_browser {
class RadarrClient;
}

namespace ui { class Renderer; }

namespace media_browser::ui {

class ReleasePickerScreen : public MbScreen {
public:
    // One row in the picker. Built from a Prowlarr ReleaseRecord (and
    // optionally enriched with Radarr's own scoring once we wire that in).
    struct ReleaseCandidate {
        std::string title;
        std::string indexer;
        int         indexer_id = 0;   // Radarr's indexerId — required to grab
        std::string guid;
        std::string download_url;
        int         seeders   = 0;
        int         leechers  = 0;
        long long   size_bytes = 0;
        std::string codec;       // "x264" / "x265" / "AV1" / "" (parsed from title)
        std::string resolution;  // "720p" / "1080p" / "2160p" / ""
        std::string source;      // "BluRay" / "WEB-DL" / "WEBRip" / "HDTV" / ""
        int         score        = 0;     // Radarr custom-format score, if known
        bool        would_auto_pick = false;  // gold-border highlight
        bool        below_threshold = false;  // dim red — score < minFormatScore
    };

    explicit ReleasePickerScreen(::media_browser::RadarrClient& radarr);

    // Set the candidates to display. Caller passes raw rows; the screen
    // sorts (seeders desc, score desc) and decorates (would_auto_pick on
    // the highest-scoring above-threshold row, below_threshold on each
    // sub-floor row) before storing. Resets focus + scroll.
    void set_candidates(std::string movie_title,
                        std::vector<ReleaseCandidate> rows);

    // MbScreen overrides.
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void   render(::ui::Renderer& r, int screen_w, int screen_h) override;

    // Static helpers — defined inline so the unit-test binary (which
    // does NOT link release_picker_screen.cpp because that would pull
    // in Renderer + Toast symbols) can exercise them directly.
    static void sort_candidates(std::vector<ReleaseCandidate>& rows) {
        std::sort(rows.begin(), rows.end(),
                  [](const ReleaseCandidate& a, const ReleaseCandidate& b) {
                      if (a.seeders != b.seeders) return a.seeders > b.seeders;
                      return a.score > b.score;
                  });
    }

    static void flag_auto_pick_and_threshold(
        std::vector<ReleaseCandidate>& rows, int min_format_score) {
        int best_idx   = -1;
        int best_score = std::numeric_limits<int>::min();
        for (size_t i = 0; i < rows.size(); ++i) {
            rows[i].below_threshold = (rows[i].score < min_format_score);
            rows[i].would_auto_pick = false;
            if (!rows[i].below_threshold && rows[i].score > best_score) {
                best_score = rows[i].score;
                best_idx   = static_cast<int>(i);
            }
        }
        if (best_idx >= 0) rows[best_idx].would_auto_pick = true;
    }

private:
    ::media_browser::RadarrClient& radarr_;
    std::string                    movie_title_;
    std::vector<ReleaseCandidate>  rows_;
    int                            focus_ = 0;
    int                            scroll_top_ = 0;
    static constexpr int           kVisibleRows = 6;
};

}  // namespace media_browser::ui
