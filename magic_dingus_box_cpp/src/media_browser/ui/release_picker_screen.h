#pragma once

// ReleasePickerScreen — manual override of Radarr's auto-pick.
//
// Data flow: Detail screen (or the StallPromptModal "Pick another" branch)
// invokes `load_async()` with the Radarr movie id. The picker spawns a
// worker thread that calls RadarrClient::get_releases_for_movie (Radarr's
// /api/v3/release?movieId=X — a 14-25s indexer round-trip), then drains
// the parsed candidates into the rendered state on a future update()
// tick. While the load is in flight render() shows a "Loading releases…"
// state with a dot animation; SELECT/DPad are gated until results land
// so the user can't grab a non-existent row. The synchronous
// set_candidates() entry point is preserved for unit tests + any future
// caller that already has Json results in hand.

#include "media_browser/ui/mb_screen.h"
#include "platform/input_manager.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media_browser {
class RadarrClient;
class DownloadWatchdog;
}

namespace ui { class Renderer; }

namespace media_browser::ui {

class ReleasePickerScreen : public MbScreen {
public:
    // One row in the picker. Built from a Prowlarr ReleaseRecord (and
    // optionally enriched with Radarr's own scoring once we wire that in).
    // How well a release will DISPLAY on this kiosk's hardware. Distinct
    // from `score` (Radarr's quality preference) and `below_threshold`
    // (Radarr would reject) — this is specifically "will the picture
    // come out right on the Pi 4B + GL SDR renderer." See
    // classify_playability() for the rules.
    enum class Playability {
        Ideal,       // x264 / H.264 8-bit SDR — hardware-decoded, perfect
        Degraded,    // HEVC/x265 8-bit SDR — software-decoded, may stutter
        Unplayable,  // HDR/DV/10-bit/AV1/4K/Remux/3D — wrong colors, green
                     // screen, no decoder, or not a single playable file
    };

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
        // Display-compatibility verdict + a short human reason (e.g.
        // "HDR", "10-bit", "AV1 — no decoder"). Populated by
        // classify_playability() in set_candidates(). Unplayable rows
        // are sorted to the bottom and can't be SELECTed.
        Playability playability = Playability::Ideal;
        std::string playability_reason;
    };

    explicit ReleasePickerScreen(::media_browser::RadarrClient& radarr);
    ~ReleasePickerScreen();

    // Set the candidates to display. Caller passes raw rows; the screen
    // sorts (seeders desc, score desc) and decorates (would_auto_pick on
    // the highest-scoring above-threshold row, below_threshold on each
    // sub-floor row) before storing. Resets focus + scroll.
    //
    // Called by the load_async() worker drain in update(); also exposed
    // as a public entry point for unit tests + any future caller that
    // already has parsed candidates in hand.
    void set_candidates(std::string movie_title,
                        std::vector<ReleaseCandidate> rows);

    // Async-load state. The picker tracks whether a worker thread is
    // currently fetching releases from Radarr; render() branches on this
    // to show a "Loading…" state vs the populated row list, and
    // handle_input() gates SELECT/DPad while loading so the user can't
    // grab a non-existent row.
    enum class LoadState { Idle, Loading, Ready, Failed };

    // Kicks off a worker thread that calls
    // radarr_.get_releases_for_movie(radarr_movie_id) and parses the
    // JSON into ReleaseCandidate rows. Returns immediately. The picker
    // shows its Loading state until update() drains the worker's
    // result. Subsequent calls cancel any in-flight load via the
    // generation counter (the older worker runs to completion in the
    // background and silently discards its result).
    //
    // Required because Radarr's interactive search across the indexer
    // pool blocks for 14-25s. Running it on the UI thread froze the
    // kiosk render loop long enough to trip systemd's watchdog (was
    // 10s, bumped to 30s as a band-aid in v1.7.x — this commit reverts
    // that). Mirrors ProwlarrClient::search_async + BrowseScreen +
    // DetailScreen's worker patterns: detached worker, generation
    // counter, mutex-guarded result slot drained by update().
    void load_async(int radarr_movie_id, std::string movie_title);

    LoadState load_state() const { return load_state_.load(); }

    // Optional download-stall watchdog. Set by main.cpp at startup; if
    // null the screen skips watchdog registration (e.g. unit-test builds
    // that don't construct one). The SELECT branch of handle_input()
    // registers a watch on a successful grab so a stalled torrent can
    // later surface the StallPromptModal.
    void set_watchdog(::media_browser::DownloadWatchdog* w) { watchdog_ = w; }

    // The tmdb_id of the movie whose releases are being picked. Set by
    // main.cpp's picker callback (which fires from DetailScreen) just
    // before the dispatcher transitions us in. Required so a successful
    // grab can register a watch keyed on the movie.
    void set_movie_tmdb_id(int tmdb_id) { tmdb_id_ = tmdb_id; }

    // MbScreen overrides.
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void   render(::ui::Renderer& r, int screen_w, int screen_h) override;
    // Drains a finished worker's result into rows_ when load_state_ ==
    // Ready. Cheap atomic load most frames. Defined out-of-line in the
    // .cpp — the test binary doesn't link release_picker_screen.cpp.
    void   update() override;

    // Static helpers — defined inline so the unit-test binary (which
    // does NOT link release_picker_screen.cpp because that would pull
    // in Renderer + Toast symbols) can exercise them directly.
    // Classify how a release will display on the Pi 4B + GL SDR renderer
    // from its title. Pure string logic — no I/O — so the unit-test
    // binary can exercise every rule without a Renderer. Returns the
    // verdict; on a non-Ideal verdict, `reason_out` gets a short tag for
    // the UI badge (e.g. "HDR", "10-bit", "4K").
    //
    // Ordering matters: we check the hard-block signals first (they win
    // over codec), then the degraded (HEVC-8bit) case, else Ideal.
    // Case-insensitive substring matching on a lowercased copy.
    static Playability classify_playability(const std::string& title,
                                            std::string* reason_out = nullptr) {
        std::string t;
        t.reserve(title.size());
        for (char ch : title) {
            t.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        }
        auto has = [&t](const char* needle) {
            return t.find(needle) != std::string::npos;
        };
        auto set_reason = [reason_out](const char* r) {
            if (reason_out) *reason_out = r;
        };

        // --- Hard blocks: wrong colors / no decoder / not-a-file ---------
        // HDR + Dolby Vision: the GL renderer is SDR-only, so HDR content
        // renders washed-out / grey. DV adds a metadata layer nothing in
        // our pipeline reads.
        if (has("dolby vision") || has("dolby.vision")
            || has("dovi") || has(".dv.") || has(" dv ")
            || has("hdr10") || has("hdr")) {
            set_reason("HDR/DV — washed-out colour"); return Playability::Unplayable;
        }
        // 10-bit (incl. Hi10P): the rpivid HEVC path negotiates the SAND
        // tiled format GStreamer can't consume → green/no picture. Even
        // 10-bit H.264 (Hi10P) isn't hardware-decodable on the Pi.
        if (has("10bit") || has("10 bit") || has("10-bit")
            || has("hi10p") || has("main 10") || has("main10")) {
            set_reason("10-bit — green screen"); return Playability::Unplayable;
        }
        // AV1: no decoder at all on the Pi 4B (hardware or practical
        // software at 1080p).
        if (has("av1")) {
            set_reason("AV1 — no decoder"); return Playability::Unplayable;
        }
        // 4K / 2160p: downscaling overwhelms the Pi and these are almost
        // always HDR anyway.
        if (has("2160p") || has("4k") || has("uhd")) {
            set_reason("4K — too heavy"); return Playability::Unplayable;
        }
        // Disc images / remuxes: not a single directly-playable file.
        if (has("remux") || has("bdmv") || has("video_ts")
            || has("m2ts") || has(".iso") || has(" iso ")
            || has("bdremux")) {
            set_reason("Disc image — not a file"); return Playability::Unplayable;
        }
        // 3D side-by-side / over-under: renders doubled on a 2D screen.
        if (has("hsbs") || has("h-sbs") || has(" sbs ") || has(".sbs.")
            || has(" hou ") || has(".hou.") || has("half-sbs")
            || has(" 3d ") || has(".3d.")) {
            set_reason("3D — doubled image"); return Playability::Unplayable;
        }

        // --- Degraded: HEVC/x265 8-bit — software-decoded, may stutter ---
        if (has("x265") || has("h265") || has("h.265") || has("hevc")) {
            set_reason("HEVC — may stutter"); return Playability::Degraded;
        }

        // --- Ideal: everything else (x264 / H.264 8-bit SDR) ------------
        set_reason("");
        return Playability::Ideal;
    }

    static void sort_candidates(std::vector<ReleaseCandidate>& rows) {
        std::sort(rows.begin(), rows.end(),
                  [](const ReleaseCandidate& a, const ReleaseCandidate& b) {
                      // Unplayable always sinks below playable. Within the
                      // playable set, Ideal (hardware H.264) outranks
                      // Degraded (HEVC) so the smooth options float up
                      // even when an HEVC release has more seeders.
                      auto tier = [](const ReleaseCandidate& c) {
                          switch (c.playability) {
                              case Playability::Ideal:      return 0;
                              case Playability::Degraded:   return 1;
                              case Playability::Unplayable: return 2;
                          }
                          return 1;
                      };
                      int ta = tier(a), tb = tier(b);
                      if (ta != tb) return ta < tb;
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
            // The "Radarr's pick" gold highlight must only ever land on a
            // release the kiosk can actually display. An Unplayable row
            // (HDR/4K/AV1/etc.) is never a valid auto-pick even if its
            // raw score is highest — skip it here so the highlight goes
            // to the best PLAYABLE above-threshold row instead.
            if (rows[i].playability == Playability::Unplayable) continue;
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

    // See setters above. Both default to "not wired" so unit tests that
    // construct the screen with just a RadarrClient still compile + run.
    ::media_browser::DownloadWatchdog* watchdog_ = nullptr;
    int                                tmdb_id_ = 0;

    // --- Async load state ------------------------------------------------
    // Mirrors the pattern in ProwlarrClient::search_async + DetailScreen
    // run_fetch: generation counter for cancellable loads, atomic state
    // flag for the UI's branch, mutex-protected pending result slot,
    // and a vector of every spawned worker for clean teardown.
    std::atomic<LoadState>            load_state_{LoadState::Idle};
    std::atomic<int>                  load_generation_{0};
    // The Radarr movie id of the in-flight (or most-recently-completed)
    // load. Stored separately from rows_/movie_title_ so the SELECT
    // grab path can also feed it to watchdog_->watch() — the watchdog
    // needs the radarr_movie_id to power the deep-link from the stall
    // modal "Pick another" button back into the picker.
    int                               loading_movie_id_ = 0;
    mutable std::mutex                load_mu_;
    std::vector<ReleaseCandidate>     pending_rows_;       // guarded by load_mu_
    std::string                       loading_title_;      // guarded by load_mu_
    std::chrono::steady_clock::time_point loading_started_at_{};

    // All worker threads spawned during this screen's lifetime. Joined
    // in the destructor so a worker mid-CURL doesn't outlive the
    // ReleasePickerScreen and segfault on result publication. Same
    // tracking pattern DetailScreen uses for its TMDB workers.
    std::vector<std::thread>          load_workers_;

    // Worker entry — runs on a background thread spawned by load_async.
    // Captures gen at spawn time and compares against load_generation_
    // before publishing so a stale worker (load_async called again
    // before this one returned) silently drops its result rather than
    // overwriting the new load's pending state.
    void run_load(int gen, int radarr_movie_id);
};

}  // namespace media_browser::ui
