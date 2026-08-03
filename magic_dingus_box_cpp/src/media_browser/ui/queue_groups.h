#pragma once

// Pure grouping helper for the Queue screen's TV section (spec:
// 2026-07-31-marquee-personalization-and-tv-design.md, Phase 2c-3 pulled
// forward). Header-only and Renderer-free so test_media_browser_unit can
// assert on it — same rationale as browse_logic.h / series_detail_logic.h.
//
// The one structural fact this file exists for: Sonarr's /api/v3/queue is
// per EPISODE. A season pack is N rows sharing ONE downloadId (which IS the
// torrent hash), so a UI that rendered the raw rows would show a 13-episode
// pack as thirteen identical "downloading" lines with the same percentage.
// group_tv_queue() collapses those into one row per DOWNLOAD.
//
// Two consequences the field types encode:
//
//   1. SIZES ARE MAXED, NOT SUMMED. Every row of a pack repeats the WHOLE
//      download's size/sizeleft — verified against Sonarr's own payload in
//      tests/media_browser/fixtures/sonarr/queue.json, where three episode
//      rows each carry size=12884901888 / sizeleft=6442450944 for one 12 GB
//      pack. Summing would have reported 36 GB and a progress bar computed
//      from a tripled denominator. MAX (rather than "first row wins") also
//      survives a mid-flight row whose size hasn't been filled in yet.
//
//   2. ONE id CANCELS THE WHOLE DOWNLOAD. first_queue_id is any row's id
//      because DELETE /api/v3/queue/{id}?removeFromClient=true acts on the
//      download, not the episode — the siblings 404 BY DESIGN afterwards
//      (sonarr_client.h). Callers must issue exactly ONE cancel per group;
//      iterating the pack's ids turns a success into a pile of 404s. This
//      is the same hazard cancel_ids_for_series() in series_detail_logic.h
//      dedupes against.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "media_browser/sonarr/sonarr_types.h"

namespace media_browser::ui {

// One DOWNLOAD in Sonarr's queue — the collapsed form of the N episode rows
// that share a downloadId.
struct TvQueueGroup {
    std::string download_id;      // torrent hash; empty falls back to title key
    // Sonarr's queue carries no series title: SonarrQueueItem::title is the
    // RELEASE title and the embedded Episode only has an EPISODE title
    // (sonarr_types.h). group_tv_queue() itself stays lookup-free, so the
    // release title lands here as the fallback identity; enrich_tv_groups()
    // (below) REPLACES it with the clean "<series> — Season N" form when the
    // caller supplies a library snapshot that knows series_id.
    std::string series_title;
    // First row's seriesId. Every row of one download belongs to one series
    // (a download is one release grabbed for one series), so the first row
    // speaks for the pack — same first-row-wins rule status follows.
    // enrich_tv_groups() keys the library lookup on this.
    int series_id = 0;
    // Filled by enrich_tv_groups() from the library snapshot — Sonarr's
    // /queue payload itself carries no images. Empty means the row renders
    // the deterministic-tint placeholder, exactly like a poster-less movie.
    std::string poster_url;
    int season_number = -1;       // -1 = rows span multiple seasons
    int episode_count = 0;        // rows in this download
    int64_t size_bytes = 0;       // MAX across rows (rows repeat the pack size)
    int64_t sizeleft_bytes = 0;   // MAX across rows (same reason)
    std::string status;           // first row's status
    std::string tracked_download_state;  // first row's
    int first_queue_id = 0;       // any row's id — cancelling ONE cancels the
                                  // WHOLE download server-side (documented)
};

// Collapse per-episode queue rows into one entry per download, preserving
// first-appearance order (the screen renders them top-to-bottom and a
// reordering list under a 1.5s refresh would make the cursor jump).
//
// Key: download_id, falling back to title when empty — the precedent is
// cancel_ids_for_series(), and title is documented identical across a pack's
// rows. A row with NEITHER is taken as-is into its own group rather than
// collapsed onto a shared empty key, which would silently merge unrelated
// downloads into one row and one cancel.
inline std::vector<TvQueueGroup>
group_tv_queue(const std::vector<SonarrQueueItem>& rows) {
    std::vector<TvQueueGroup> out;
    out.reserve(rows.size());
    std::unordered_map<std::string, size_t> index;

    for (const auto& q : rows) {
        const std::string& key = q.download_id.empty() ? q.title : q.download_id;

        size_t slot;
        if (key.empty()) {
            slot = out.size();
            out.emplace_back();
        } else {
            auto it = index.find(key);
            if (it == index.end()) {
                slot = out.size();
                index.emplace(key, slot);
                out.emplace_back();
            } else {
                slot = it->second;
            }
        }

        TvQueueGroup& g = out[slot];
        if (g.episode_count == 0) {
            // First row of this download establishes identity + status.
            g.download_id            = q.download_id;
            g.series_title           = q.title;
            g.series_id              = q.series_id;
            g.season_number          = q.season_number;
            g.status                 = q.state;
            g.tracked_download_state = q.tracked_download_state;
            g.first_queue_id         = q.id;
        } else if (g.season_number != q.season_number) {
            // A multi-season pack has no single season to name.
            g.season_number = -1;
        }
        ++g.episode_count;
        if (q.size_bytes > g.size_bytes)         g.size_bytes = q.size_bytes;
        if (q.sizeleft_bytes > g.sizeleft_bytes) g.sizeleft_bytes = q.sizeleft_bytes;
    }
    return out;
}

// The two library facts enrichment needs, decoupled from the full Series
// struct so this header stays testable without constructing library rows
// (and so the caller decides which snapshot the map is built from).
struct SeriesRef {
    std::string title;       // clean series title, e.g. "Breaking Bad"
    std::string poster_url;  // same URL shape the movie rows feed the
                             // artwork cache; empty keeps the tint
};

// Second pass over group_tv_queue()'s output: resolve each group's
// series_id against a library snapshot (sonarr_id -> SeriesRef) to fill
// poster_url and replace the release-name fallback title with the clean
// "<series title> — Season N" form ("<series title>" alone when the pack
// spans seasons, season_number < 0). A group whose id the map does not
// know is left UNTOUCHED — the release title is still the best available
// identity when the library fetch failed or lagged, and an empty
// poster_url keeps the deterministic tint. Kept separate from
// group_tv_queue() so the collapse stays pure over queue rows and both
// halves unit-test independently.
//
// The season is baked in HERE, not appended at render time — the render's
// row-title builder must not add its own " — Season N" or every enriched
// row would read "Breaking Bad — Season 1 — Season 1 (10 eps)".
inline void enrich_tv_groups(
    std::vector<TvQueueGroup>& groups,
    const std::unordered_map<int, SeriesRef>& by_id) {
    for (auto& g : groups) {
        auto it = by_id.find(g.series_id);
        if (it == by_id.end()) continue;  // fallback identity stands
        const SeriesRef& ref = it->second;
        g.poster_url = ref.poster_url;
        // A library row with no title has nothing better than the release
        // name — take the poster but keep the fallback title.
        if (ref.title.empty()) continue;
        if (g.season_number >= 0) {
            g.series_title = ref.title + " \xE2\x80\x94 Season "
                           + std::to_string(g.season_number);
        } else {
            g.series_title = ref.title;
        }
    }
}

}  // namespace media_browser::ui
