#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "media_browser/radarr/radarr_types.h"
#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/ui/series_detail_logic.h"

namespace media_browser {

class SonarrClient;
class QbittorrentClient;

// SeriesDetail's mutation-worker bodies (Download Season N, Add Season N,
// Whole series… press 1 + press 2, Remove), extracted from the screen so the
// Sonarr call ORDER, the abort rules and every toast's wording are unit-tested
// (tests/media_browser/test_series_mutations.cpp) rather than only compiled
// into the kiosk. Same arrangement as season_delete.h / movie_remove.h: each
// run_* is BLOCKING (sequential HTTP) and runs on the screen's one mutation
// worker; the screen copies the returned outcome into its mut_* block under
// mut_mtx_ and drain_mutation applies it on the render thread.
//
// None of these touch the screen, a Renderer or a Toast. A std::exception
// from the client propagates — spawn_mutation's catch turns it into the
// "something went wrong" toast, exactly as when these bodies were inline.

// Everything one worker publishes — field-for-field the screen's mut_*
// block (minus the per-season delete's two fields, which season_delete.h
// owns). Defaults are the drain's reset values, so an outcome that leaves a
// field alone publishes exactly what the inline worker did by not writing it.
struct SeriesMutationOutcome {
    std::string toast;
    // Fresh record to apply (series_), and whether Sonarr has refreshed it.
    std::optional<Series> series;
    bool settled = true;
    // The whole Sonarr record is gone — the page navigates back.
    bool removed = false;
    // Season > 1 add: the season to start (start_season_download) on the
    // render thread once `series` is applied, and the title the leave-mid-
    // add toast names.
    std::optional<int> start_season;
    std::string start_title;
    // Whole-series press 1: the disk verdict and the estimate it judged.
    bool have_verdict = false;
    ui::DiskVerdict verdict = ui::DiskVerdict::Block;
    int64_t estimate = 0;
};

// ---------- Pure helpers ----------

// Quality profile BY NAME ("Any" is this box's profile; the id is not
// portable) — the first profile when none is named "Any", 0 when there are
// none. DetailScreen::pick_quality_profile_id's policy minus its movie-only
// fallbacks.
int pick_series_quality_profile_id(const std::vector<QualityProfile>& profiles);

// The LOWEST NON-SPECIAL season number in a record, with its monitored flag
// and file count; season == 0 when the record has no ordinary season.
// addOptions.monitor = "firstSeason" monitors the first AIRED season, which
// is not always numbered 1 (and season 0 is specials), so the add's
// did-anything-happen check reads this, never the literal 1.
struct LowestSeason {
    int season = 0;
    bool monitored = false;
    int files = 0;
};
LowestSeason lowest_regular_season(const Series& s);

// The single-season download's outcome toast. eps_monitored is
// monitor_episodes_for_seasons' answer; suspiciously_empty is "it answered
// 0 for a season the user can see".
std::string compose_start_season_toast(const std::string& title, int season,
                                       const std::optional<int>& eps_monitored,
                                       bool suspiciously_empty, bool searched);

// The whole-series press-2 outcome toast, from BOTH signals — the PUT
// failure count AND the search outcome.
std::string compose_whole_series_toast(const std::string& title, int total,
                                       int failed,
                                       const std::optional<int>& eps_monitored,
                                       bool searched);

// The whole-series press-1 toast for a verdict; empty for Allow (the armed
// label IS the feedback).
std::string compose_preflight_toast(const std::string& title,
                                    ui::DiskVerdict v, int64_t estimate,
                                    const std::optional<int64_t>& free_bytes);

// ---------- Worker bodies (BLOCKING) ----------

// Quick Start: after a season's search/monitor has landed, ALSO fire a
// search scoped to that season's episode 1. STRICTLY BEST-EFFORT: every
// failure path logs at info and returns; callers never let it change their
// outcome. See the .cpp for the full contract.
void fire_episode1_search(SonarrClient& sonarr, int sid, int season,
                          const std::string& title);

// One get_episodes_checked + one bulk PUT re-monitoring every episode of
// `seasons` (probe P3: season->episode monitoring does NOT cascade and
// SeasonSearch skips unmonitored episodes). nullopt = a real failure (read
// failed or PUT refused); otherwise the count of episode ids monitored,
// which may legitimately be 0 — whether 0 is fine is the CALLER's call.
std::optional<int> monitor_episodes_for_seasons(SonarrClient& sonarr,
                                                int sonarr_id,
                                                const std::vector<int>& seasons);

// "Download Season N" on an in-library, settled series: monitor the season,
// re-monitor its episodes, search (unless the re-monitor came back
// suspiciously empty), Quick Start, re-read the record.
SeriesMutationOutcome run_start_season_download(SonarrClient& sonarr, int sid,
                                                int season,
                                                const std::string& title);

// "Add Season N" for a series not yet in the library. N == 1:
// add_series(monitor=true) and verify the add actually did something. N > 1:
// add_series(monitor=false) and hand `season` back in start_season for the
// render thread to start on the settled record.
SeriesMutationOutcome run_add_at_season(SonarrClient& sonarr, int tmdb_id,
                                        int season, const std::string& title);

// "Whole series…" press 2: (pre-add only) add_series(monitor=true), then
// monitor every season in to_monitor, re-monitor their episodes, series
// search, re-read.
SeriesMutationOutcome run_whole_series(SonarrClient& sonarr, bool pre_add,
                                       int tmdb_id, int sid,
                                       const std::vector<int>& to_monitor,
                                       const std::string& title);

// Free-space reading for a host path; nullopt when the stat fails (a
// returned 0 is a REAL full disk). std::filesystem::space by default;
// injectable so the preflight's two-source rule is testable.
using HostFreeSpaceFn =
    std::function<std::optional<int64_t>(const std::string& host_path)>;
std::optional<int64_t> host_free_space_bytes(const std::string& host_path);

// "Whole series…" press 1: free space from Sonarr's /tv root folder, falling
// back to the host path (resolved from that folder, or the compiled default
// when Sonarr named none), then whole_series_verdict. Always publishes a
// verdict (have_verdict == true).
SeriesMutationOutcome run_whole_series_preflight(
    SonarrClient& sonarr, int64_t estimate, const std::string& title,
    const HostFreeSpaceFn& host_free = host_free_space_bytes);

// Whole-series Remove: checked queue read, one cancel per download, checked
// history walk + qBit purge (when qbit is non-null), remove_series with its
// files. removed == true only when remove_series succeeded.
SeriesMutationOutcome run_remove_series(SonarrClient& sonarr,
                                        QbittorrentClient* qbit, int sid,
                                        const std::string& title);

}  // namespace media_browser
