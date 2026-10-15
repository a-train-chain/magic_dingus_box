#include "media_browser/series_mutations.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "spdlog/spdlog.h"

// Moved verbatim from series_detail_screen.cpp's mutation workers. The
// "[SeriesDetail]" log prefix is kept on purpose: these lines are read in
// the journal as the screen's, and grepping for them must keep working.

namespace media_browser {

int pick_series_quality_profile_id(const std::vector<QualityProfile>& profiles) {
    int qp_id = 0;
    for (const auto& qp : profiles) {
        if (qp_id == 0) qp_id = qp.id;
        if (qp.name == "Any") { qp_id = qp.id; break; }
    }
    return qp_id;
}

LowestSeason lowest_regular_season(const Series& s) {
    LowestSeason out;
    for (const auto& ss : s.seasons) {
        if (ss.season_number <= 0) continue;
        if (out.season != 0 && ss.season_number >= out.season) continue;
        out.season = ss.season_number;
        out.monitored = ss.monitored;
        out.files = ss.episode_file_count;
    }
    return out;
}

std::string compose_start_season_toast(const std::string& title, int season,
                                       const std::optional<int>& eps_monitored,
                                       bool suspiciously_empty, bool searched) {
    // The episode re-monitor is reported FIRST when it failed OR came back
    // suspiciously empty: for a previously deleted season it is the step
    // that decides whether anything can arrive at all, so "search started"
    // would be the silent dead end this amendment exists to close.
    return (!eps_monitored.has_value() || suspiciously_empty)
        ? title + ": Season " + std::to_string(season) +
              " monitored, but its episodes couldn't be re-enabled "
              "\xE2\x80\x94 a previously deleted season may not "
              "re-download; try again"
        : searched
        ? title + ": Season " + std::to_string(season) + " search started"
        : title + ": Season " + std::to_string(season) +
              " monitored, but the search didn't start "
              "\xE2\x80\x94 Sonarr will pick it up on RSS";
}

std::string compose_whole_series_toast(const std::string& title, int total,
                                       int failed,
                                       const std::optional<int>& eps_monitored,
                                       bool searched) {
    // Composed from BOTH signals — the failure count AND the search outcome.
    // Never claim "Monitored" or promise RSS when every PUT failed (Sonarr
    // stopped mid-flow): the series is in the library with nothing monitored
    // and nothing will ever arrive.
    if (total > 0 && failed == total) {
        return title + ": couldn't monitor seasons "
                       "\xE2\x80\x94 is Sonarr running?";
    } else if (!eps_monitored.has_value()) {
        // Reported ahead of the search outcome, exactly as the single-season
        // path reports it: for a previously deleted season this is the step
        // that decides whether anything can arrive at all, so "search
        // started" would be the silent dead end. A zero count is NOT routed
        // here — see the split contract in monitor_episodes_for_seasons.
        return title + ": seasons monitored, but their "
                       "episodes couldn't be re-enabled "
                       "\xE2\x80\x94 a previously deleted season "
                       "may not re-download; try again";
    } else if (!searched) {
        return title + ": seasons monitored, but the search "
                       "didn't start \xE2\x80\x94 Sonarr will pick "
                       "them up on RSS";
    } else if (failed > 0) {
        return title + ": search started (" + std::to_string(failed) + " of " +
               std::to_string(total) + " seasons couldn't be monitored)";
    }
    return title + ": whole-series search started";
}

std::string compose_preflight_toast(const std::string& title,
                                    ui::DiskVerdict v, int64_t estimate,
                                    const std::optional<int64_t>& free_bytes) {
    const auto gb = [](int64_t b) {
        return std::to_string(b / (1024LL * 1024 * 1024)) + " GB";
    };
    switch (v) {
        case ui::DiskVerdict::Block:
            // The codebase's FIRST blocking preflight: show both numbers plus
            // the floor.
            return title + ": not enough space \xE2\x80\x94 needs ~" +
                   gb(estimate) + " (est), " + gb(free_bytes.value_or(0)) +
                   " free (20 GB floor)";
        case ui::DiskVerdict::WarnOnly:
            return title + ": couldn't check free space "
                           "\xE2\x80\x94 confirm to proceed anyway";
        case ui::DiskVerdict::Allow:
            break;  // the armed label IS the feedback
    }
    return std::string();
}

// Quick Start: after a season's search/monitor has landed, ALSO fire a
// search scoped to that season's episode 1. A season pack is typically
// 10-30 GB and takes an hour+; a single-episode release (~1-2 GB) lands in
// minutes and imports hardlink-instant, so E1 is watchable while the pack
// fills in behind it. Sonarr itself dedupes the overlap: whichever grab
// covers E1 first wins and the loser's release is rejected in-queue.
//
// STRICTLY BEST-EFFORT, by contract with the mutation workers that call
// it: runs INSIDE the worker (worker-thread HTTP is correct there — the
// mutation lane serializes), but only after the user-visible outcome is
// already decided. Every failure path logs at info and returns — it never
// touches toasts, mut_* publish state, or the drain. Callers must never
// let its result change theirs.
//
// The episode fetch right after an add/settle is expected to be usable
// (metadata refresh is done at settle, so ids are real), but nullopt
// (transport blip) or an empty list (refresh drift, garbage body) are
// both quietly acceptable: the season search already queued covers the
// content either way.
void fire_episode1_search(SonarrClient& sonarr, int sid, int season,
                          const std::string& title) {
    if (sid <= 0 || season <= 0) return;
    const auto eps = sonarr.get_episodes_checked(sid);
    if (!eps.has_value() || eps->empty()) {
        spdlog::info("[SeriesDetail] quick-start: no episode list for '{}' "
                     "S{} — skipping E1 search", title, season);
        return;
    }
    for (const auto& ep : *eps) {
        if (ep.season_number != season || ep.episode_number != 1) continue;
        if (ep.has_file) {
            // Already on disk — nothing to fast-lane.
            return;
        }
        if (ep.id <= 0) return;  // unusable id: never POST a guess
        if (sonarr.trigger_episode_search(ep.id)) {
            spdlog::info("[SeriesDetail] quick-start: E1 search fired for "
                         "'{}' S{}E1 (episode id {})", title, season, ep.id);
        } else {
            spdlog::info("[SeriesDetail] quick-start: E1 search for '{}' S{} "
                         "didn't start — the season search still covers it",
                         title, season);
        }
        return;
    }
    spdlog::info("[SeriesDetail] quick-start: '{}' S{} has no episode 1 — "
                 "skipping E1 search", title, season);
}

std::optional<int> monitor_episodes_for_seasons(SonarrClient& sonarr,
                                                int sonarr_id,
                                                const std::vector<int>& seasons) {
    // WORKER-THREAD helper: one get_episodes_checked + one bulk PUT for
    // every season being (re-)monitored. Touches nothing but `sonarr` and
    // its arguments, so it is safe off the render thread.
    //
    // Probe P3 (live-verified): season->episode monitoring does NOT cascade,
    // and Sonarr's SeasonSearch skips unmonitored episodes. A season that
    // went through the per-season delete has its episodes INDIVIDUALLY
    // unmonitored — that is precisely what stops autoRedownloadFailed from
    // re-grabbing behind the delete — so ANY path that re-monitors a season
    // in order to download it must re-monitor its episodes too, or the
    // search fires, finds nothing to want, and says nothing. It exists as a
    // shared helper because it originally lived inline in ONE of the two
    // such paths: "Whole series…" silently downloaded nothing for a deleted
    // season while reporting success.
    //
    // Split contract on the return value — nullopt is a REAL failure (the
    // read failed transport-wise, or the PUT was refused); otherwise the
    // count of episode ids actually monitored, which may legitimately be
    // zero. Whether zero is fine is the CALLER's call, not this helper's:
    //   - whole-series worker: zero is tolerated — an ANNOUNCED season
    //     legitimately has no episode records yet.
    //   - start_season_download: zero is suspicious. get_episodes_checked
    //     can read engaged-but-empty on a malformed body or a series id
    //     Sonarr no longer knows (see its doc comment) — collapsing that
    //     into a bare "success" would let the caller fire a search that
    //     can never find anything, under a toast that says it started.
    //
    // Idempotent (and one wasted GET) for seasons that were never deleted.
    if (seasons.empty()) return 0;
    const auto eps = sonarr.get_episodes_checked(sonarr_id);
    if (!eps.has_value()) return std::nullopt;
    std::vector<int> ids;
    for (const auto& e : *eps) {
        // id <= 0 is an unusable record — never PUT a guess
        // (fire_episode1_search's rule).
        if (e.id <= 0) continue;
        if (std::find(seasons.begin(), seasons.end(), e.season_number) ==
            seasons.end())
            continue;
        ids.push_back(e.id);
    }
    if (!sonarr.set_episodes_monitored(ids, true)) return std::nullopt;
    return static_cast<int>(ids.size());
}

SeriesMutationOutcome run_start_season_download(SonarrClient& sonarr, int sid,
                                                int season,
                                                const std::string& title) {
    SeriesMutationOutcome out;
    if (!sonarr.set_season_monitored(sid, season, true)) {
        const std::string err = sonarr.last_error();
        out.toast = title + ": couldn't monitor season " +
                    std::to_string(season) + " \xE2\x80\x94 " + err;
        return out;
    }
    // Episodes before the search — see monitor_episodes_for_seasons.
    const std::optional<int> eps_monitored =
        monitor_episodes_for_seasons(sonarr, sid, {season});
    // Zero is suspicious HERE (unlike the whole-series worker): this
    // season came from a row the user can see in rows_, so an
    // engaged-but-empty read is more likely get_episodes_checked's
    // documented misclassification (malformed body, or a series id
    // Sonarr no longer knows) than a real absence of episodes. Firing
    // the search anyway would run it against still-unmonitored
    // episodes under a "search started" toast — the exact silent dead
    // end this helper exists to close, reached through this call site
    // instead. A genuine read/PUT failure (nullopt) still fires the
    // search exactly as before — it costs nothing and it is correct
    // for every season that was never deleted.
    const bool suspiciously_empty = eps_monitored.value_or(-1) == 0;
    const bool searched = suspiciously_empty
        ? false
        : sonarr.trigger_season_search(sid, season);
    // Quick Start: fast E1 single alongside the season pack. Only when
    // the season search actually started, and strictly best-effort —
    // see fire_episode1_search.
    if (searched) fire_episode1_search(sonarr, sid, season, title);
    auto fresh = sonarr.get_series(sid);
    out.toast = compose_start_season_toast(title, season, eps_monitored,
                                           suspiciously_empty, searched);
    if (fresh.has_value()) {
        out.settled = record_refreshed(*fresh);
        out.series = std::move(fresh);
    }
    return out;
}

SeriesMutationOutcome run_add_at_season(SonarrClient& sonarr, int tmdb_id,
                                        int season, const std::string& title) {
    SeriesMutationOutcome out;
    // Quality profile BY NAME ("Any" is this box's profile; the id is not
    // portable).
    const int qp_id = pick_series_quality_profile_id(sonarr.get_quality_profiles());
    if (qp_id == 0) {
        // Distinguish "Sonarr answered, no profiles" from "we never reached
        // Sonarr" — last_error() is set on every transport failure, so an
        // empty vector alone is ambiguous and blaming the config would be
        // wrong.
        const std::string err = sonarr.last_error();
        out.toast = err.empty()
            ? title + ": Sonarr has no quality profile \xE2\x80\x94 not added"
            : title + ": couldn't reach Sonarr \xE2\x80\x94 " + err;
        return out;
    }
    if (season > 1) {
        // ---- chosen season > 1: add with NOTHING monitored ----
        // monitor=false => addOptions.monitor="none", no search. Never
        // "firstSeason": the user chose another season, and monitoring
        // Season 1 behind their back would download a season they did not
        // ask for. The chosen season is started on the RENDER thread by
        // drain_mutation via start_season_download (start_season), on the
        // record published here — the same monitor + episode re-monitor +
        // search every other single-season download goes through.
        // (set_season_monitored also turns the series-level flag on:
        // monitor=false wrote series.monitored=false, and Sonarr grabs
        // nothing for an unmonitored series.)
        //
        // The find-existing branch (a library record this screen did not
        // recognise by tmdbId) lands here too, settled by record_refreshed:
        // starting the season explicitly is exactly right for it, since that
        // branch applies no addOptions.
        auto res = sonarr.add_series(tmdb_id, qp_id, /*monitor=*/false, title);
        if (!res.ok) {
            const std::string err = sonarr.last_error();
            out.toast = title + ": add failed \xE2\x80\x94 " + err;
            return out;
        }
        if (!res.settled || res.series.sonarr_id <= 0) {
            // Record kept (the settle poll brings the controls back once
            // Sonarr finishes), nothing monitored, nothing searched — and NO
            // fallback to Season 1. The user picks the season again once the
            // page offers it.
            out.series = res.series;
            out.settled = false;
            out.toast = title + ": added \xE2\x80\x94 choose the "
                                "season again in a moment";
            return out;
        }
        out.series = res.series;
        out.settled = true;
        out.start_season = season;
        out.start_title = title;
        return out;
    }
    // monitor=true => addOptions.monitor="firstSeason" +
    // searchForMissingEpisodes=true: exactly the spec's season-at-a-time
    // default, applied by Sonarr itself.
    auto res = sonarr.add_series(tmdb_id, qp_id, /*monitor=*/true, title);
    if (!res.ok) {
        const std::string err = sonarr.last_error();
        out.toast = title + ": add failed \xE2\x80\x94 " + err;
        return out;
    }
    if (!res.settled) {
        // settled==false: seasons[] is EMPTY by contract. The page keeps
        // rendering TMDB rows and hides the add controls until the Task-8
        // poll settles the record.
        out.series = res.series;
        out.settled = false;
        out.toast = title + ": added \xE2\x80\x94 syncing seasons\xE2\x80\xA6";
        return out;
    }
    // ---- settled: did the add actually DO anything? ----
    // add_series returns ok=true from its find-existing branch without
    // applying addOptions, monitoring anything or searching. That branch
    // dedupes by tvdbId while this screen detects in-library by tmdbId, so
    // a library record with tmdb_id == 0 still offers "Add Season 1" — and
    // the press would toast "Season 1 search started" having done nothing at
    // all. Read the outcome off the returned record instead of trusting
    // ok=true. See lowest_regular_season for why it is never the literal 1.
    const int sid = res.series.sonarr_id;
    const LowestSeason first = lowest_regular_season(res.series);
    std::string toast;
    std::optional<Series> fresh;
    if (first.season == 0 || sid <= 0) {
        // A settled record with no ordinary season, or with no id to act
        // on: nothing to verify, so claim nothing.
        toast = title + ": added to your TV library";
    } else if (!first.monitored) {
        // The idempotent branch (or any drift): the add did NOT apply
        // monitoring, so do it explicitly and report THOSE outcomes — same
        // shape as the NextSeason flow.
        if (!sonarr.set_season_monitored(sid, first.season, true)) {
            const std::string err = sonarr.last_error();
            out.toast = title + ": couldn't monitor season " +
                        std::to_string(first.season) + " \xE2\x80\x94 " + err;
            return out;
        }
        const bool searched = sonarr.trigger_season_search(sid, first.season);
        // Quick Start rides only on a search that actually started; on the
        // RSS fallback there is no pack grab to outrun. Best-effort — see
        // fire_episode1_search.
        if (searched) fire_episode1_search(sonarr, sid, first.season, title);
        fresh = sonarr.get_series(sid);
        toast = searched
            ? title + ": Season " + std::to_string(first.season) +
                  " search started"
            : title + ": Season " + std::to_string(first.season) +
                  " monitored, but the search didn't start "
                  "\xE2\x80\x94 Sonarr will pick it up on RSS";
    } else if (first.files > 0) {
        // Monitored AND already has files: the box already had this series
        // and nothing was started, so say that rather than promising a
        // search.
        toast = title + ": already in your TV library";
    } else {
        // Monitored with nothing on disk yet — the add path genuinely acted
        // (or a prior add did) and searchForMissingEpisodes rode in with
        // monitor=true.
        toast = title + ": Season " + std::to_string(first.season) +
                " search started";
        // Quick Start: the add-time search is already hunting the season
        // pack; pair it with the fast E1 single. Settled (this whole branch
        // is behind res.settled) means the metadata refresh is done, so
        // episode ids are real. Best-effort — see fire_episode1_search.
        fire_episode1_search(sonarr, sid, first.season, title);
    }
    if (fresh.has_value()) {
        out.settled = record_refreshed(*fresh);
        out.series = std::move(fresh);
    } else {
        out.series = res.series;
        out.settled = true;
    }
    out.toast = std::move(toast);
    return out;
}

SeriesMutationOutcome run_whole_series(SonarrClient& sonarr, bool pre_add,
                                       int tmdb_id, int sid,
                                       const std::vector<int>& to_monitor,
                                       const std::string& title) {
    SeriesMutationOutcome out;
    int series_id = sid;
    std::optional<Series> added;
    if (pre_add) {
        const int qp_id =
            pick_series_quality_profile_id(sonarr.get_quality_profiles());
        if (qp_id == 0) {
            const std::string err = sonarr.last_error();
            out.toast = err.empty()
                ? title + ": Sonarr has no quality profile "
                          "\xE2\x80\x94 not added"
                : title + ": couldn't reach Sonarr \xE2\x80\x94 " + err;
            return out;
        }
        // *** monitor=true is REQUIRED here. *** add_series writes the
        // SERIES-LEVEL monitored flag from this same parameter
        // (series["monitored"] = monitor). (The chosen-season add, N>1, uses
        // monitor=false and relies on set_season_monitored(true) turning the
        // series flag on afterwards; THIS whole-series path has no such
        // follow-up.) "none" would leave the series unmonitored, and Sonarr's
        // MonitoredEpisodeSpecification rejects every release for an
        // unmonitored series: seasons monitored, search runs, NOTHING ever
        // downloads — invisibly.
        //
        // The firstSeason-vs-our-PUTs race this used to fear is provably
        // over when settled==true: add_settled (monitor=true) requires the
        // applied monitoring state to have been OBSERVED. The race exists
        // only on the timeout path — which is why the season PUTs below are
        // gated on res.settled.
        auto res = sonarr.add_series(tmdb_id, qp_id, /*monitor=*/true, title);
        if (!res.ok) {
            const std::string err = sonarr.last_error();
            out.toast = title + ": add failed \xE2\x80\x94 " + err;
            return out;
        }
        if (!res.settled) {
            // firstSeason has NOT been applied yet; seasons PUT now would be
            // unmonitored behind us when it lands. Stop honestly: the row
            // shows [Remove] only until the poll settles, then "Whole
            // series…" reappears and the retry takes the in-library path —
            // idempotent by design.
            out.series = res.series;
            out.settled = false;
            out.toast = title + ": added \xE2\x80\x94 syncing seasons; "
                                "the whole-series option returns when "
                                "Sonarr finishes";
            return out;
        }
        // Settled: S1 is already monitored and the add-time search already
        // covers it (searchForMissingEpisodes rode in with monitor=true);
        // S1's entry in to_monitor is a harmless idempotent PUT, and the
        // series search below may re-query S1 — redundant, not harmful.
        series_id = res.series.sonarr_id;
        added = res.series;
    }
    if (series_id <= 0) {
        if (added.has_value()) {
            out.series = std::move(added);
            out.settled = false;
        }
        out.toast = title + ": added, but Sonarr hasn't assigned "
                            "an id yet \xE2\x80\x94 try Whole series again "
                            "in a moment";
        return out;
    }
    // For an ANNOUNCED series Sonarr may not know every season yet, so some
    // of these PUTs legitimately fail. They are counted, not hidden, and the
    // toast is composed from the real count.
    int failed = 0;
    std::vector<int> monitored_seasons;
    for (int season : to_monitor) {
        if (!sonarr.set_season_monitored(series_id, season, true))
            ++failed;
        else
            monitored_seasons.push_back(season);
    }
    // The SAME probe-P3 re-monitor "Download Season N" does, for every
    // season this press just monitored. Without it "Whole series…" silently
    // downloaded nothing for a season that had been through the per-season
    // delete (its episodes stay individually unmonitored, and season->
    // episode does NOT cascade) while reporting success. Scoped to the
    // seasons whose PUT actually took — a season Sonarr refused to monitor
    // is not one we are searching for. Unlike start_season_download, a ZERO
    // count here is tolerated, not suspicious — an announced-but-unaired
    // season legitimately has no episode records — so only nullopt (a real
    // read/PUT failure) counts against it.
    const std::optional<int> eps_monitored =
        monitor_episodes_for_seasons(sonarr, series_id, monitored_seasons);
    const bool searched = sonarr.trigger_series_search(series_id);
    auto fresh = sonarr.get_series(series_id);
    const int total = static_cast<int>(to_monitor.size());
    if (fresh.has_value()) {
        out.settled = record_refreshed(*fresh);
        out.series = std::move(fresh);
    } else if (added.has_value()) {
        out.series = std::move(added);
        out.settled = false;
    }
    out.toast =
        compose_whole_series_toast(title, total, failed, eps_monitored, searched);
    return out;
}

std::optional<int64_t> host_free_space_bytes(const std::string& host_path) {
    std::error_code ec;
    auto info = std::filesystem::space(host_path, ec);
    if (ec) return std::nullopt;
    return static_cast<int64_t>(info.available);
}

SeriesMutationOutcome run_whole_series_preflight(SonarrClient& sonarr,
                                                 int64_t estimate,
                                                 const std::string& title,
                                                 const HostFreeSpaceFn& host_free) {
    // Free space, two sources with DIFFERENT zero semantics:
    //
    //  - Sonarr's root folder: freeSpace is parsed with a 0 default, so an
    //    ABSENT/null field (Sonarr can't stat the folder — the stale-
    //    container-bind state that magic-dingus-storage-attach.service
    //    exists for) is indistinguishable from a genuinely full disk. A 0
    //    here is therefore AMBIGUOUS and must fall through, or a healthy box
    //    with 400 GB free gets a false "0 GB free" Block with a wrong
    //    diagnosis.
    //  - std::filesystem::space on the host path: failure is a distinct
    //    error code, so a returned 0 is a REAL full disk and
    //    whole_series_verdict correctly Blocks on it.
    //
    // The host path is the MATCHED root folder's container path put through
    // resolve_host_path, not a literal: main.cpp resolves this client's
    // prefixes in three tiers (explicit TV var → parent movie var + "tv" →
    // compiled default), so a box whose STORAGE_ROOT moved would otherwise
    // stat a path that does not exist, set `ec` forever, and pin the second
    // source to WarnOnly — turning the blocking preflight into no preflight
    // at all, exactly where the ambiguous Sonarr zero above needs the
    // backup. resolve_host_path is a PURE, non-virtual string mapping over
    // cfg_.container_library_prefix / cfg_.host_library_prefix — no HTTP, no
    // virtual dispatch, safe to call from this worker.
    //
    // Do not "simplify" the two guards into one.
    std::optional<int64_t> free_bytes;
    std::string tv_root;  // container path of the matched /tv folder
    for (const auto& rf : sonarr.get_root_folders()) {
        if (rf.path.find("/tv") == std::string::npos) continue;
        // Captured even when the reading is 0/ambiguous — that IS the case
        // the host-path probe exists to answer.
        if (tv_root.empty()) tv_root = rf.path;
        if (rf.free_space_bytes > 0) {
            free_bytes = rf.free_space_bytes;
            break;
        }
    }
    if (!free_bytes.has_value()) {
        // Literal fallback ONLY when Sonarr named no /tv folder at all (it
        // never answered, or the box is misconfigured): there is no
        // container path to resolve, so the compiled default is the best
        // guess available.
        const std::string host_path =
            tv_root.empty()
                ? std::string("/mnt/ssd/library/tv")
                : sonarr.resolve_host_path(SonarrClient::normalize_prefix(tv_root));
        free_bytes = host_free(host_path);
    }
    const ui::DiskVerdict v = ui::whole_series_verdict(estimate, free_bytes);
    SeriesMutationOutcome out;
    out.have_verdict = true;
    out.verdict = v;
    out.estimate = estimate;
    // A Block verdict is published so drain_mutation does NOT arm.
    out.toast = compose_preflight_toast(title, v, estimate, free_bytes);
    return out;
}

SeriesMutationOutcome run_remove_series(SonarrClient& sonarr,
                                        QbittorrentClient* qbit, int sid,
                                        const std::string& title) {
    // Sonarr-shaped mirror of DetailScreen::run_remove:
    //  1. Cancel in-flight downloads — ONCE PER DOWNLOAD, not once per queue
    //     row (cancel_ids_for_series does that dedupe; it is pure and
    //     Mac-tested).
    //  2. Purge every torrent the series' history knows about (catches
    //     finished+seeding ones step 1 misses).
    //  3. remove_series(delete_files=true).
    //  4. Back to origin (drained on the render thread).
    // get_queue_CHECKED, not the bare wrapper — the same rule the per-season
    // worker's stage (c) follows, and it matters MORE here: get_queue()
    // collapses a Sonarr outage to an empty vector, which reads as "nothing
    // in flight", fires no cancels at all, and walks straight into
    // remove_series(deleteFiles=true) with a download still live in
    // qBittorrent. That is the orphaned-torrent failure this whole flow
    // exists to prevent, reached through the one answer the bare form cannot
    // give. Aborting here is retry-safe: nothing destructive has run.
    SeriesMutationOutcome out;
    const auto queue = sonarr.get_queue_checked();
    if (!queue.has_value()) {
        out.toast = title + ": couldn't check for in-flight "
                            "downloads \xE2\x80\x94 series NOT removed; "
                            "retry is safe";
        return out;
    }
    const std::vector<int> cancel_ids = ui::cancel_ids_for_series(*queue, sid);
    int cancel_failed = 0;
    int cancel_ok = 0;
    std::string cancel_err;
    for (int qid : cancel_ids) {
        if (sonarr.cancel_queue_item(qid)) {
            ++cancel_ok;
            continue;
        }
        ++cancel_failed;
        // Captured AT the failing iteration. last_error() is cleared on
        // entry to every client call, so a LATER success wipes the diagnosis
        // and the abort toast below degrades to "NOT removed" with no reason
        // attached.
        if (cancel_err.empty()) cancel_err = sonarr.last_error();
    }
    if (cancel_failed > 0) {
        // A genuinely failed cancel still aborts before anything is deleted
        // — the house rule that keeps a half-removed series from orphaning a
        // torrent. But cancels use removeFromClient=true, so any that
        // SUCCEEDED before the failure have already taken their downloads
        // with them: saying "couldn't cancel" flat would imply nothing
        // happened, and the user would not know data is gone.
        out.toast =
            (cancel_ok > 0
                 ? title + ": cancelled " + std::to_string(cancel_ok) +
                       " download(s), then failed \xE2\x80\x94 series "
                       "NOT removed; retry is safe"
                 : title + ": couldn't cancel " + std::to_string(cancel_failed) +
                       " download(s) \xE2\x80\x94 NOT removed") +
            (cancel_err.empty() ? std::string() : " (" + cancel_err + ")");
        return out;
    }
    if (qbit != nullptr) {
        // The history walk runs BEFORE the decision to proceed, because a
        // FAILED walk is indistinguishable from "no history" by an unchecked
        // return alone. Removing on a failed walk is the exact production
        // failure DetailScreen documents — every seeding torrent orphaned
        // forever, under a toast that said "removed".
        //
        // CHECKED variant, deliberately not the raw one + a follow-up
        // last_error() read: the ~9 s background re-poll runs on its own
        // thread and shares this SonarrClient (and its one last_error_
        // member). A decision split across two calls could read whatever the
        // poll thread set or cleared in the gap, not this call's own
        // outcome. nullopt IS the whole answer here.
        //
        // Aborting here is retry-safe: the cancel stage above is idempotent —
        // an already-cancelled download leaves no queue row, so its ids
        // simply dedupe to nothing next time.
        //
        // Still gated on qbit: a box with no qBittorrent client cannot purge
        // anything at all, so it keeps the old semantics and accepts the
        // orphan risk BY CONSTRUCTION.
        const std::optional<std::vector<std::string>> hashes =
            sonarr.get_series_download_hashes_checked(sid);
        if (!hashes.has_value()) {
            // Cancel-stage context: any downloads cancelled before this abort
            // ARE gone (removeFromClient=true in the cancel loop above), so a
            // flat "couldn't check, NOT removed" would understate what
            // already happened — same honesty rule as the cancel-failure
            // toast just above.
            out.toast =
                (cancel_ok > 0
                     ? title + ": cancelled " + std::to_string(cancel_ok) +
                           " download(s), then couldn't check "
                           "for seeding torrents \xE2\x80\x94 "
                           "series NOT removed"
                     : title + ": couldn't check for seeding "
                               "torrents \xE2\x80\x94 series NOT "
                               "removed") +
                " \xE2\x80\x94 Sonarr didn't answer";
            return out;
        }
        for (const auto& h : *hashes) {
            if (!qbit->delete_torrent(h, /*delete_files=*/true)) {
                spdlog::warn("[SeriesDetail] qbit delete failed for {}", h);
            }
        }
    }
    if (!sonarr.remove_series(sid, /*delete_files=*/true)) {
        const std::string err = sonarr.last_error();
        out.toast = title + ": remove failed \xE2\x80\x94 " + err;
        return out;
    }
    out.removed = true;
    out.toast = title + ": removed from the TV library";
    return out;
}

}  // namespace media_browser
