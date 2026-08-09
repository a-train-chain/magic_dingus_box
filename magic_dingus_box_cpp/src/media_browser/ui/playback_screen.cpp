#include "media_browser/ui/playback_screen.h"

#include <cstdlib>  // std::system — used to call playback_services_pause.sh
#include <filesystem>  // exists() guard before the in-place episode advance
#include <spdlog/spdlog.h>

#include "app/app_state.h"
#include "app/controller.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "platform/input_manager.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "utils/result.h"

namespace media_browser::ui {

PlaybackScreen::PlaybackScreen(app::Controller& controller, app::AppState& state,
                                ::media_browser::TmdbClient& tmdb,
                                ::media_browser::RadarrClient& radarr,
                                QbittorrentClient* qbit)
    : controller_(controller), state_(state), tmdb_(tmdb), radarr_(radarr), qbit_(qbit) {}

void PlaybackScreen::bump_hud_visibility() {
    hud_visible_until_ = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(kHudShowMs);
}

float PlaybackScreen::hud_alpha(bool paused) const {
    if (paused) return 1.0f;
    auto now = std::chrono::steady_clock::now();
    if (now >= hud_visible_until_) return 0.0f;
    auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        hud_visible_until_ - now).count();
    if (remaining_ms > kHudFadeMs) return 1.0f;
    return static_cast<float>(remaining_ms) / static_cast<float>(kHudFadeMs);
}

void PlaybackScreen::set_movie(std::string host_path, std::string title) {
    movie_path_ = std::move(host_path);
    movie_title_ = std::move(title);
    // Reset overlay meta so a stale entry from a previous movie doesn't leak.
    overlay_meta_ = {};
    overlay_meta_.title = movie_title_;
}

void PlaybackScreen::set_movie_meta(PlaybackOverlayMovieMeta meta) {
    overlay_meta_ = std::move(meta);
}

void PlaybackScreen::publish_now_playing_status() {
    // TV = a watch identity of kind Tv plus a series title to show. An
    // identity-less TV file (shouldn't happen via SeriesDetail, which
    // always sets both) degrades to the movie shape below — movie_title_
    // is the full display_title there, so nothing is lost.
    const bool tv = watch_identity_.has_value() &&
                    watch_identity_->ref.kind == MediaKind::Tv &&
                    !series_title_.empty();
    if (tv) {
        state_.now_playing_kind  = "tv";
        state_.now_playing_title = series_title_;
        // Episode title looked up from the session's episode vector; an
        // absent match still yields the bare "SxEy" code.
        std::string ep_title;
        for (const auto& e : episodes_) {
            if (e.season_number == watch_identity_->season &&
                e.episode_number == watch_identity_->episode) {
                ep_title = e.title;
                break;
            }
        }
        state_.now_playing_subtitle = format_now_playing_episode(
            watch_identity_->season, watch_identity_->episode, ep_title);
    } else {
        state_.now_playing_kind  = "movie";
        state_.now_playing_title = movie_title_;
        state_.now_playing_subtitle =
            overlay_meta_.year > 0 ? std::to_string(overlay_meta_.year)
                                   : std::string{};
    }
    // MB playback has no playlist context; a stale name/count from the
    // last main-menu playlist would otherwise ride along in the JSON.
    // (current_item_index is already -1 — main.cpp resets it at MB entry —
    // which is also what keeps verify_box's playlist-scoped now_playing
    // check from firing here.)
    state_.current_playlist_name.clear();
    state_.current_item_count = 0;
}

void PlaybackScreen::notify_external_seek() {
    // Mirrors the local seek handlers in handle_input(): suppress the
    // FLUSH-seek video_active flicker so update()'s EOS edge detector
    // doesn't bail out of playback, and flash the on-TV scrub bar so the
    // phone tap has visible feedback.
    eos_suppress_frames_ = kSeekSuppressFrames;
    state_.show_seek_bar = true;
    state_.seek_bar_timer = kSeekBarVisibleSec;
    bump_hud_visibility();
}

void PlaybackScreen::set_episode_context(std::vector<EpisodeInfo> episodes,
                                         std::vector<std::string> host_paths,
                                         std::vector<SeasonRow> rows,
                                         watch_map watch,
                                         std::string series_title) {
    episodes_ = std::move(episodes);
    episode_host_paths_ = std::move(host_paths);
    season_rows_ = std::move(rows);
    watch_ = std::move(watch);
    series_title_ = std::move(series_title);
    // The cached index belongs to the PREVIOUS context; begin_end_overlay()
    // re-derives it from the watch identity at the first EOS.
    current_index_ = -1;
}

void PlaybackScreen::enter() {
    exit_pending_ = false;
    deferred_toast_.clear();
    qbit_was_paused_by_us_ = false;
    qbit_alt_limited_by_us_ = false;
    // EOS latch pair resets TOGETHER here and in advance_to_next_episode()
    // — the two session starts — so each playback session gets exactly one
    // consume-once report from take_eos_watched().
    eos_latched_ = false;
    eos_reported_ = false;
    // Fresh session: no end-of-episode overlay, and any never-consumed
    // "Start Season N" intent from a PREVIOUS session dies here (leave()
    // deliberately does not clear it — the dispatcher drains it around the
    // Playback->SeriesDetail transition, possibly after leave()).
    end_overlay_ = {};
    pending_next_season_.reset();
    // Tracks state.video_active across update() ticks for end-of-stream
    // edge detection. Starts false; update() needs to see false→true (play
    // started) before a later true→false transition reads as natural EOS.
    was_video_active_ = false;

    if (movie_path_.empty()) {
        deferred_toast_ = "No movie file path";
        exit_pending_ = true;
        return;
    }

    // Quiet the torrent stack BEFORE loading the movie so the GStreamer
    // demuxer's initial reads don't have to fight qBit's piece-write
    // bursts for disk bandwidth. Per-board (platform profile):
    //
    //   Trickle (Pi 5): engage qBit's alternative speed limits
    //   (~1.5 MB/s, configured at kiosk startup) instead of stopping the
    //   swarm — the SSD library and spare CPU absorb a trickle without
    //   playback impact, so downloads keep progressing through a movie.
    //
    //   Full pause (Pi 4B / Unknown): on USB-flash media (the typical
    //   Pi 4 setup), concurrent random read+write tanks throughput to
    //   single-digit MB/s and makes scrubbing feel frozen. Pausing
    //   gives the playback reader exclusive disk access.
    //
    // Best-effort either way: a qBit failure here doesn't abort playback
    // — we just log and continue with whatever performance the disk can
    // give us. Same rationale as the controller_.load_file fallback
    // path below.
    if (qbit_ != nullptr) {
        if (state_.platform_profile.trickle_torrents_during_video) {
            if (qbit_->set_alt_speed_limits_enabled(true)) {
                qbit_alt_limited_by_us_ = true;
            } else {
                spdlog::warn("[playback] qbit trickle cap failed; "
                             "downloads run uncapped during playback");
            }
        } else {
            if (qbit_->pause_all()) {
                qbit_was_paused_by_us_ = true;
            } else {
                spdlog::warn("[playback] qbit pause_all failed; "
                             "playback may stutter on USB-flash media");
            }
        }
    }

    // Also pause the Radarr/Prowlarr/Byparr containers — frees ~300 MB
    // RAM and ~6% CPU for the duration of the movie so the kiosk's
    // video pipeline isn't competing with metadata syncs / indexer
    // queries / Cloudflare-challenge solving. The helper is best-effort
    // (no-op if Docker isn't installed, the user isn't in the docker
    // group, or the containers don't exist on this Pi). Resumed on
    // leave() via the symmetric "unpause" call. Output is logged via
    // shell, not spdlog, so we discard the return code here.
    //
    // std::system() is acceptable for this fire-and-forget shell call
    // because (a) the script is fixed-path / not derived from any
    // user-controllable input, (b) we don't care about the exit code
    // beyond a debug log line, (c) the script itself is idempotent.
    //
    // Platform-gated: only boards that actually need the memory pay the
    // cost. On Pi 5 there is 1122MB free of 2006MB during 1080p playback
    // with the whole stack running (measured 2026-07-26), so pausing
    // reclaims memory we don't need while the 20-40s restart on exit
    // shows the user a false "tunnel down" toast and a blank library
    // grid. Pi 4B still pauses — it genuinely needed it.
    if (state_.platform_profile.pause_services_during_movie) {
        (void)std::system(
            "/usr/local/bin/playback_services_pause.sh pause >/dev/null 2>&1");
    } else {
        spdlog::info("[playback] skipping service pause "
                     "(platform has memory headroom)");
    }

    // Empty playlist_dir disables the playlist-dir-relative resolution
    // strategy in path_resolver. The path is already host-absolute (passed
    // through RadarrClient::resolve_host_path by DetailScreen), so the
    // resolver's absolute-path branch returns it unchanged.
    //
    // start_position_ is the one-shot resume offset the dispatcher set via
    // set_start_position() (0.0 = play from the beginning); leave() clears
    // it so it can never leak into an unrelated later session.
    auto load_result = controller_.load_file_with_resolution(
        movie_path_, /*playlist_dir=*/"", /*start=*/start_position_,
        /*end=*/0.0, /*loop=*/false);

    // Result<> exposes operator bool — false on failure. .error() carries
    // the message.
    if (!load_result) {
        deferred_toast_ = "Playback failed: " + load_result.error();
        spdlog::error("[playback] load failed for '{}': {}",
                      movie_path_, load_result.error());
        exit_pending_ = true;
        return;
    }

    // load_file_with_resolution -> GstPlayer::load_file -> play() internally,
    // so the pipeline is already transitioning to PLAYING here. Calling
    // controller_.play() again would issue a second set_state(PLAYING) on
    // the same pipeline back-to-back, which trips the state machine and
    // logs spurious "state change to PLAYING failed" errors. One play() per
    // load is the contract.

    // Pinned resume toast. format_position_hms is the pure Task 1 helper —
    // Task 6's episode glyph uses the same formatter, so the two surfaces
    // cannot drift. Copy is pinned; do not reword.
    if (start_position_ > 0.0) {
        ::ui::Toast::show("Resuming from " + format_position_hms(start_position_)
                          + " \xE2\x80\x94 seek back to restart");
    }

    // Title marquee for 3 seconds on entry.
    title_marquee_until_ = std::chrono::steady_clock::now()
                          + std::chrono::seconds(3);

    // Show the HUD briefly on entry so the user sees the controls.
    bump_hud_visibility();

    // Initial warmup: suppress EOS detection for ~1 second so an
    // immediate user-initiated scrub right after pressing Play doesn't
    // get its post-seek state-flicker mistaken for end-of-stream. The
    // pipeline takes a moment to reach steady PLAYING state after load,
    // and seeks issued during that window cause extra-long flicker.
    eos_suppress_frames_ = 60;

    spdlog::info("[playback] playing '{}' (path='{}')",
                 movie_title_, movie_path_);

    // Tell the phone remote what is actually playing. Without this the
    // StatusWriter kept serializing the LAST main-menu playlist item's
    // now_playing fields (only Controller::load_playlist_item ever wrote
    // them) under the movie's live position/duration. leave() clears.
    publish_now_playing_status();

    // Start pre-fetching similar films in the background so they are ready
    // by the time the user presses the rotary to open the overlay.
    // Idempotent for the same tmdb_id; no-op when tmdb_id == 0.
    // The fetch runs on a detached std::thread inside PlaybackOverlay; the
    // overlay's destructor joins it, so lifetime is safe.
    overlay_.start_prefetch(tmdb_, overlay_meta_);
}

void PlaybackScreen::leave() {
    // Idempotent — safe to call from any exit path. Catches the case where
    // the user long-presses BTN4 and the dispatcher hard-exits to MainMenu.
    controller_.stop();

    // Cancel any in-flight similar-films prefetch so its thread doesn't
    // outlive the screen's use of tmdb_ after we've left. The thread checks
    // cancel_requested_ and exits early; join happens in PlaybackOverlay's
    // destructor (or on the next start_prefetch call).
    overlay_.cancel_prefetch();
    overlay_.close();

    // Force state.video_active false immediately. controller_.stop() tears
    // down the pipeline, but state.video_active is normally only updated
    // by Controller::update_state(), which runs every other frame in the
    // main loop. Without this explicit reset, the next render tick sees
    // stale video_active=true and runs gst_renderer.render() over a
    // torn-down pipeline, painting a stale texture fragment (the
    // upper-right green rectangle the user reports). Same pattern as
    // main.cpp's main-UI exit path.
    state_.video_active = false;
    state_.show_seek_bar = false;

    // Clear the published now-playing info (the counterpart of enter()'s
    // publish_now_playing_status) so the phone remote doesn't keep showing
    // the movie/episode after playback ends. Mirrors what the playlist
    // path's stop does via Controller::update_state — which deliberately
    // does NOT clear during Media Browser sessions (see the guard there:
    // an episode-end countdown must keep its now_playing up). The next
    // playlist item reclaims all of these via load_playlist_item.
    state_.now_playing_title.clear();
    state_.now_playing_subtitle.clear();
    state_.now_playing_kind.clear();

    if (!deferred_toast_.empty()) {
        ::ui::Toast::show(deferred_toast_);
        deferred_toast_.clear();
    }

    // Undo whatever torrent quieting enter() did — and ONLY what enter()
    // did. The two flags are the consent records: never resume a torrent
    // the operator had manually stopped before entering playback, and
    // never clear an alt-limits cap the operator engaged for their own
    // reasons. (A cap WE set that fails to clear here is additionally
    // covered by the unconditional clear at kiosk startup — crash
    // recovery in main.cpp's qBit init.)
    if (qbit_ != nullptr && qbit_alt_limited_by_us_) {
        if (!qbit_->set_alt_speed_limits_enabled(false)) {
            spdlog::warn("[playback] qbit trickle cap clear failed; "
                         "downloads stay capped until the next kiosk start");
        }
        qbit_alt_limited_by_us_ = false;
    }
    if (qbit_ != nullptr && qbit_was_paused_by_us_) {
        if (!qbit_->resume_all()) {
            spdlog::warn("[playback] qbit resume_all failed; "
                         "operator may need to manually resume from web UI");
        }
        qbit_was_paused_by_us_ = false;
    }

    // One-shot watch-state carriers die with the session. start_position_
    // must not leak into a later playback that never called
    // set_start_position(); watch_identity_ must not attribute a later,
    // unrelated file's checkpoints to this title. main.cpp's
    // flush_watch_state runs BEFORE leave() at every exit site, so by the
    // time these clear, the final position write has already happened —
    // and any (buggy) post-leave flush reads a disengaged identity and
    // becomes a safe no-op instead of an upsert of zeros.
    start_position_ = 0.0;
    watch_identity_.reset();

    // Episode context dies with the session too — a stale vector must never
    // feed a later session's end-of-episode overlay. pending_next_season_
    // survives on purpose (drained by the dispatcher around this very
    // transition; cleared in the next enter() instead — see the header).
    episodes_.clear();
    episode_host_paths_.clear();
    season_rows_.clear();
    watch_.clear();
    series_title_.clear();
    current_index_ = -1;
    end_overlay_ = {};

    // Symmetric un-pause for the Radarr/Prowlarr/Byparr containers we
    // froze in enter(). Deliberately left UNCONDITIONAL even though
    // enter() is now platform-gated: the helper is idempotent (a no-op
    // for un-paused or missing containers), and keeping it unconditional
    // means a box that was paused by an older build — or by the game
    // quiet-mode path — always gets recovered. The worst case
    // (un-pausing something already running) is harmless.
    (void)std::system(
        "/usr/local/bin/playback_services_pause.sh unpause >/dev/null 2>&1");

    spdlog::info("[playback] left playback screen");
}

Screen PlaybackScreen::handle_input(
        const std::vector<platform::InputEvent>& events) {
    // First: if an armed exit is pending, honor it. Fires every frame even
    // with empty events because the dispatcher always calls handle_input.
    // Movies arm this at natural EOS; TV sessions only via the end-overlay's
    // own outcomes (missing file / load failure) — the countdown/card branch
    // below is what runs while eos_latched_ && !exit_pending_.
    if (exit_pending_) {
        return origin_;
    }

    // End-of-episode overlay (TV only — begin_end_overlay() armed it at the
    // EOS edge). It owns ALL input while active: the movie has ended, so
    // seeks/pause/similar-films are dead controls until the next episode
    // loads or the session exits.
    if (end_overlay_.kind != EndOverlayKind::None) {
        for (const auto& e : events) {
            if (!e.pressed) continue;
            // Rotary press: countdown -> play the next episode now;
            // card primary -> fire the Start-Season intent; plain "Done"
            // card -> dismiss back to where we came from.
            if (e.action == platform::InputAction::SELECT) {
                if (end_overlay_.kind == EndOverlayKind::Countdown) {
                    advance_to_next_episode();
                    // On failure the advance armed exit_pending_ + deferred
                    // toast; the fast-return above exits next frame.
                    return Screen::Playback;
                }
                if (end_overlay_.has_primary) {
                    // "Start Season N" — the dispatcher consumes the intent
                    // on the Playback->SeriesDetail transition (Task 6).
                    pending_next_season_ = end_overlay_.card.next_season;
                    return Screen::SeriesDetail;
                }
                return origin_;  // "Done"
            }
            // BTN2 (red) = Stop / close; BTN4 = Back. Both leave playback.
            // origin_ is SeriesDetail for every TV handoff (Task 6 sets it),
            // and returning it (rather than a hardcoded SeriesDetail) keeps
            // the exit robust if a future entry path passes a different
            // origin.
            if (e.action == platform::InputAction::PLAY_PAUSE ||
                e.action == platform::InputAction::SETTINGS_MENU) {
                return origin_;
            }
        }
        return Screen::Playback;  // swallow everything else while it's up
    }

    for (const auto& e : events) {
        // BTN4 short-press:
        //   - When overlay is open → close the overlay (stay in Playback).
        //   - When overlay is closed → return to origin_ (Detail by default).
        // The dispatcher's long-press handler (held >500ms) intercepts before
        // we see it, so reaching here means it's a short press. We don't call
        // controller_.stop() because leave() will (idempotently).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            bump_hud_visibility();
            if (overlay_.is_open()) {
                overlay_.close();
                return Screen::Playback;
            }
            return origin_;
        }

        // Rotary press (SELECT):
        //   - When overlay is closed → open it.
        //   - When overlay is open → quick-add the focused similar film via Radarr,
        //     then show a 2s toast confirming the result. The overlay stays open and
        //     the movie keeps playing.
        // Note: only on key-down (pressed == true); ignore key-up.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            bump_hud_visibility();
            if (!overlay_.is_open()) {
                overlay_.open();
                return Screen::Playback;
            }
            // Overlay is open — attempt quick-add for the focused film.
            auto film = overlay_.focused_film();
            if (!film) {
                // No film focused (list still loading or empty) — just ignore.
                return Screen::Playback;
            }

            // Quick-add runs on a worker — get_quality_profiles +
            // add_movie are two 5s-timeout HTTP calls, and this handler
            // runs while a MOVIE IS PLAYING (see the header note on
            // quickadd_worker_). The worker composes the outcome toast;
            // update() shows it when it lands.
            if (quickadd_in_flight_.load(std::memory_order_acquire)) {
                overlay_.show_toast("Adding\xe2\x80\xa6");
                return Screen::Playback;
            }
            if (quickadd_worker_.joinable()) quickadd_worker_.join();
            quickadd_in_flight_.store(true, std::memory_order_release);
            overlay_.show_toast("Adding\xe2\x80\xa6");
            const int add_tmdb_id = film->tmdb_id;
            try {
                quickadd_worker_ = std::thread([this, add_tmdb_id]() {
                    std::string toast;
                    // Profile pick mirrors detail_screen.cpp's
                    // pick_quality_profile_id heuristic: prefer "Any" so
                    // we don't block on a profile mismatch.
                    int qp = 0;
                    auto profiles = radarr_.get_quality_profiles();
                    for (const auto& p : profiles) {
                        if (p.name == "Any") { qp = p.id; break; }
                    }
                    if (qp == 0) {
                        for (const auto& p : profiles) {
                            if (p.name == "HD - 720p/1080p") { qp = p.id; break; }
                        }
                    }
                    if (qp == 0 && !profiles.empty()) qp = profiles.front().id;

                    if (qp == 0) {
                        toast = "No quality profile";
                    } else if (radarr_.add_movie(add_tmdb_id, qp,
                                                 /*monitor=*/true)) {
                        toast = "Added \xe2\x80\x94 searching";
                    } else {
                        const std::string err = radarr_.last_error();
                        // Radarr returns HTTP 400 with "This movie has
                        // already been added" in the body when the title is
                        // already in the library; the error string is
                        // "HTTP 400: <json body>".
                        if (err.find("already") != std::string::npos ||
                            err.find("Already") != std::string::npos) {
                            toast = "Already in library";
                        } else {
                            toast = "Couldn\xe2\x80\x99t add \xe2\x80\x94 try again";
                        }
                    }
                    quickadd_toast_ = std::move(toast);
                    quickadd_done_.store(true, std::memory_order_release);
                    quickadd_in_flight_.store(false, std::memory_order_release);
                });
            } catch (const std::system_error&) {
                quickadd_in_flight_.store(false, std::memory_order_release);
                overlay_.show_toast("Couldn\xe2\x80\x99t add \xe2\x80\x94 try again");
            }
            return Screen::Playback;
        }

        // BTN2 (PLAY_PAUSE, red) = toggle play/pause during MB playback.
        // main.cpp's global exit-modal intercept exempts Screen::Playback
        // so this handler stays reachable while a movie is playing.
        // NOTE: BTN2 is intentionally NOT gated by overlay_.is_open() —
        // the user explicitly asked for BTN2 = pause/play to work even
        // when the overlay is open.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            controller_.toggle_pause();
            bump_hud_visibility();  // Pause/resume always shows HUD.
            continue;
        }

        // Every seek bumps the EOS-suppression counter (see the class
        // constants kSeekSuppressFrames / kSeekBarVisibleSec — shared with
        // notify_external_seek() so phone-remote tap-to-seek gets the same
        // FLUSH-seek flicker margin and scrub-bar flash as local seeks).

        // ±10s with PREV/NEXT — same as main UI when video is playing.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            controller_.seek(10.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            bump_hud_visibility();
            continue;
        }
        if (e.action == platform::InputAction::PREV && e.pressed) {
            controller_.seek(-10.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            bump_hud_visibility();
            continue;
        }

        // ±5s with C-stick.
        if (e.action == platform::InputAction::SEEK_RIGHT) {
            controller_.seek(5.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            bump_hud_visibility();
            continue;
        }
        if (e.action == platform::InputAction::SEEK_LEFT) {
            controller_.seek(-5.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            bump_hud_visibility();
            continue;
        }

        // Rotary twist:
        //   - When overlay is open → scroll the similar-films carousel.
        //   - When overlay is closed → velocity-curve seek (existing behavior).
        //
        // Velocity-curve seek: same shape as the main UI's playlist scrub
        // (main.cpp:1903), but with the max-seek constant scaled up because
        // movies are ~10x longer than the typical playlist video. The
        // playlist formula gives 5s slow → 30s fast (a visible 4% jump on a
        // 12-min video); on a 2hr movie that same 30s is only 0.4% of the
        // runtime, which feels like nothing. Bumping the curve to 5s slow
        // → 120s (~2 min) fast restores the same proportional travel: about
        // 1-2% of total runtime per fast click, suitable for chapter-
        // skipping while preserving precise small scrubs at slow velocities
        // (the velocity² factor means low-velocity ticks still produce
        // ~5-10s seeks, same as the playlist).
        if (e.action == platform::InputAction::ROTATE && e.delta != 0) {
            if (overlay_.is_open()) {
                overlay_.on_rotate(e.delta);
                continue;
            }
            double velocity = static_cast<double>(e.velocity);
            double seek_seconds = 5.0 + 115.0 * (velocity * velocity);
            controller_.seek(seek_seconds * e.delta);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            bump_hud_visibility();
            continue;
        }
    }

    return Screen::Playback;
}

void PlaybackScreen::update() {
    // Quick-add outcome from the worker (see handle_input's SELECT
    // branch). Toast state is render-thread-only, hence the drain here.
    if (quickadd_done_.exchange(false, std::memory_order_acq_rel)) {
        overlay_.show_toast(quickadd_toast_);
    }

    // Decay the post-seek / post-enter EOS suppression counter. While
    // it's positive, we ignore the state.video_active true→false
    // transition that GStreamer's FLUSH-seek state-machine produces;
    // without this guard, the edge below would mistake "pipeline
    // briefly went PAUSED for the seek" as "movie ended."
    if (eos_suppress_frames_ > 0) {
        --eos_suppress_frames_;
    }

    // Edge-detect natural end-of-stream: state.video_active flips
    // true→false when the GStreamer pipeline reaches EOS. We only
    // treat it as end-of-stream if WE didn't trigger the stop AND the
    // suppression counter has fully decayed (no recent seek that could
    // be causing the flicker) AND this session hasn't already latched
    // (an idling countdown/card must not re-arm off pipeline noise).
    //
    // The latch/exit split is the whole TV feature: a TV session latches
    // eos_latched_ ONLY and hands control to the end-of-episode overlay —
    // arming exit_pending_ here too would make handle_input's fast-return
    // bail to origin_ on the very next frame and the countdown could never
    // render. Movies and identity-less sessions keep the old both-flags
    // behavior exactly.
    bool video_active_now = state_.video_active;
    if (was_video_active_ && !video_active_now && !exit_pending_
        && !eos_latched_ && eos_suppress_frames_ <= 0) {
        eos_latched_ = true;
        const bool tv_session =
            watch_identity_.has_value() &&
            watch_identity_->ref.kind == MediaKind::Tv;
        if (tv_session) {
            // May itself arm exit_pending_ (S0 identity / stale vector /
            // no episode context) — the movie-style exit for TV edge cases.
            begin_end_overlay();
        } else {
            exit_pending_ = true;
        }
        spdlog::info("[playback] natural end-of-stream detected (tv={})",
                     tv_session);
    }
    was_video_active_ = video_active_now;

    // Countdown expiry -> in-place advance. Frame-clock timer, same idiom
    // as SeriesDetail's confirm timers; render() derives its "Starting in
    // N…" line from the same start point so the two can't drift.
    if (end_overlay_.kind == EndOverlayKind::Countdown && !exit_pending_) {
        const auto elapsed = std::chrono::steady_clock::now()
                           - countdown_started_at_;
        if (elapsed >= std::chrono::seconds(kNextUpCountdownSeconds)) {
            advance_to_next_episode();
        }
    }
}

// Arms the end-of-episode overlay at a TV EOS edge. Every skip path here
// is the movie-style exit (exit_pending_ -> origin_): specials (S0) are
// never wired into the overlay, and a finished episode we cannot find in
// the vector means the context is stale — never promise a "next" computed
// from data that disagrees with what just played.
void PlaybackScreen::begin_end_overlay() {
    const WatchIdentity& id = *watch_identity_;
    if (id.season == 0) {
        exit_pending_ = true;
        return;
    }

    // Locate the finished episode: trust current_index_ only after
    // re-validating it against the identity; otherwise linear-search for
    // (season, episode). No match -> movie-style exit.
    int idx = -1;
    if (current_index_ >= 0 &&
        current_index_ < static_cast<int>(episodes_.size()) &&
        episodes_[current_index_].season_number == id.season &&
        episodes_[current_index_].episode_number == id.episode) {
        idx = current_index_;
    } else {
        for (size_t i = 0; i < episodes_.size(); ++i) {
            if (episodes_[i].season_number == id.season &&
                episodes_[i].episode_number == id.episode) {
                idx = static_cast<int>(i);
                break;
            }
        }
    }
    if (idx < 0) {
        exit_pending_ = true;
        return;
    }
    current_index_ = idx;

    // In-memory watched sync BEFORE deciding the overlay. main.cpp owns
    // the persistent write (it drains take_eos_watched() and calls
    // WatchStore::mark_watched); this keeps OUR copy consistent with what
    // that drain is about to persist, so the decision below and any later
    // decision in this session see the same world.
    watch_[WatchKey{id.season, id.episode}].watched = true;

    // The similar-films overlay must not float above the end overlay —
    // and its SELECT semantics would fight the countdown's "Play now".
    overlay_.close();

    end_overlay_ = decide_end_overlay(season_rows_, episodes_, watch_,
                                      episodes_[idx], series_title_);
    if (end_overlay_.kind == EndOverlayKind::Countdown) {
        countdown_started_at_ = std::chrono::steady_clock::now();
    }
    spdlog::info("[playback] end overlay armed (kind={}, next_index={})",
                 static_cast<int>(end_overlay_.kind), end_overlay_.next_index);
}

// In-place advance to episodes_[end_overlay_.next_index]. enter() is NOT
// re-run, so its side effects are re-armed explicitly here; leave() is NOT
// run either, so the episode context and watch identity survive into the
// next episode's session (the setters overwrite what must change).
void PlaybackScreen::advance_to_next_episode() {
    const int idx = end_overlay_.next_index;

    // Belt-and-braces on the model's index, then the brief's existence
    // guard: an empty host path (no file) or a file deleted since the
    // handoff both exit with the pinned toast rather than feeding
    // GStreamer a path that cannot play.
    std::string next_host_path;
    if (idx >= 0 && idx < static_cast<int>(episodes_.size()) &&
        idx < static_cast<int>(episode_host_paths_.size())) {
        next_host_path = episode_host_paths_[idx];
    }
    if (next_host_path.empty() ||
        !std::filesystem::exists(next_host_path)) {
        spdlog::warn("[playback] next episode file missing (index={}, "
                     "path='{}')", idx, next_host_path);
        deferred_toast_ = "File missing on disk";  // surfaced by leave()
        end_overlay_ = {};
        exit_pending_ = true;
        return;
    }

    const EpisodeInfo& next = episodes_[idx];
    controller_.stop();

    // "<series> — S<season>E<episode> · <title>" (em dash / middle dot).
    const std::string new_title =
        series_title_ + " \xE2\x80\x94 S" +
        std::to_string(next.season_number) + "E" +
        std::to_string(next.episode_number) + " \xC2\xB7 " + next.title;

    // set_movie() RESETS overlay_meta_ (see its definition) — save the
    // series meta across the call and restore it with the new title so
    // poster/synopsis/cast survive the whole binge.
    auto meta = overlay_meta_;
    set_movie(next_host_path, new_title);
    meta.title = new_title;
    set_movie_meta(std::move(meta));

    // Re-attribute watch state to the next episode; the MediaRef (series)
    // part is unchanged. current_index_ is the cache begin_end_overlay()
    // re-validates at the next EOS.
    set_watch_identity(WatchIdentity{watch_identity_->ref,
                                     next.season_number,
                                     next.episode_number});
    current_index_ = idx;
    set_start_position(0.0);

    // Session restart bookkeeping — the enter() side effects, re-armed
    // explicitly. EOS latch pair resets TOGETHER (the take_eos_watched
    // contract), and the edge detector needs a fresh false→true→false.
    eos_latched_ = false;
    eos_reported_ = false;
    was_video_active_ = false;
    eos_suppress_frames_ = 60;
    end_overlay_ = {};
    title_marquee_until_ = std::chrono::steady_clock::now()
                         + std::chrono::seconds(3);
    bump_hud_visibility();

    // One play per load (load_file_with_resolution plays internally —
    // see enter()). Empty playlist_dir: the path is host-absolute.
    auto load_result = controller_.load_file_with_resolution(
        movie_path_, /*playlist_dir=*/"", /*start=*/0.0,
        /*end=*/0.0, /*loop=*/false);
    if (!load_result) {
        deferred_toast_ = "Playback failed: " + load_result.error();
        spdlog::error("[playback] in-place advance load failed for '{}': {}",
                      movie_path_, load_result.error());
        exit_pending_ = true;
        return;
    }

    // Re-publish for the phone remote: watch_identity_ now names the next
    // episode, so the subtitle advances from e.g. "S2E5 · …" to "S2E6 · …"
    // (enter() is not re-run on an in-place advance, so its publish isn't).
    publish_now_playing_status();

    spdlog::info("[playback] advanced to '{}' (path='{}')",
                 movie_title_, movie_path_);
}

void PlaybackScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // Title marquee — top-left, 3 seconds with linear fade-out over the
    // last 500 ms. Sits BELOW the 40 px wood-frame top band (which would
    // otherwise hide the heading), with breathing room above the inset
    // movie. Pre-v1.6.x this was at y=38 / rule y=58 — those put both
    // baseline and rule UNDER the wood frame.
    //   y_baseline = 40 (frame inner edge) + ~30 (heading height + pad) = 70
    //   y_rule     = baseline + 20
    // kPaddingX matches mb_chrome::kSafeInset_px so the heading lines up
    // horizontally with every other Marquee screen's title.
    auto now = std::chrono::steady_clock::now();
    if (now < title_marquee_until_) {
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            title_marquee_until_ - now).count();
        float alpha = 1.0f;
        if (remaining_ms < 500) {
            alpha = static_cast<float>(remaining_ms) / 500.0f;
        }
        std::string heading = "NOW PLAYING";
        if (!movie_title_.empty()) {
            heading += " — " + movie_title_;
        }
        const float kPaddingX  = 60.0f;   // matches chrome::kSafeInset_px
        const float kBaselineY = 70.0f;   // below the 40 px wood top band
        r.mb_draw_title_text(heading, kPaddingX, kBaselineY,
                             th.font_heading_size, th.dim, alpha);
        // Underline beneath, just like Detail's header rule.
        const float rule_y = 90.0f;       // 20 px under heading baseline
        r.mb_draw_line(kPaddingX, rule_y, w - kPaddingX, rule_y,
                       2.0f, th.dim, 0.95f * alpha);
    }

    // HUD auto-hide: scrub bar + footer hints are only shown while paused,
    // within 3s of any input, or during the 300ms fade-out window.
    // When the overlay is open, the overlay has its own footer hints — the
    // no-overlay HUD does NOT render in that case anyway, so the auto-hide
    // only ever applies to the no-overlay playback state.
    // The end-of-episode overlay suppresses the HUD entirely: the stream
    // has ended (pipeline reads "paused", which would pin the HUD at full
    // alpha) and every HUD hint is a dead control while it's up.
    if (!overlay_.is_open() && end_overlay_.kind == EndOverlayKind::None) {
        // Determine whether the movie is currently paused. video_active is
        // false when paused (GStreamer pipeline in PAUSED state).
        const bool paused = !state_.video_active;
        const float alpha = hud_alpha(paused);

        if (alpha > 0.0f) {
            // Scrub bar: moved up so the footer hints can sit below it with
            // breathing room above the bezel. New position: ~110px from
            // screen bottom (was 60px). The renderer's seek bar uses
            // show_seek_bar + seek_bar_timer; we pass it through as before,
            // but also force-show when HUD is visible (paused or input bump)
            // and alpha > 0.
            //
            // The seek bar render path in renderer.cpp uses its own state-
            // gated draw; we apply the hud alpha by temporarily scaling the
            // seek bar timer to ensure it draws, and restoring afterwards.
            // A cleaner path: call mb_render_seek_bar_alpha once that exists.
            // For now, since mb_render_seek_bar already handles its own alpha
            // via seek_bar_timer, we just ensure show_seek_bar is true and
            // then draw the footer hints with our alpha applied.
            //
            // Force the seek bar visible whenever the HUD is showing.
            const bool saved_show = state_.show_seek_bar;
            const double saved_timer = state_.seek_bar_timer;
            if (alpha > 0.0f && !state_.show_seek_bar) {
                // Temporarily inject a timer so the render path draws it.
                state_.show_seek_bar = true;
                state_.seek_bar_timer = static_cast<double>(alpha) * 0.5;
            }
            r.mb_render_seek_bar(state_);
            // Restore original state (don't corrupt the seek-bar timer logic).
            state_.show_seek_bar = saved_show;
            state_.seek_bar_timer = saved_timer;

            // Footer hints: draw with hud_alpha applied.
            // draw_footer_hints doesn't accept an alpha parameter, so we
            // only draw the hints when alpha is above the visibility threshold.
            // The hints are positioned by the chrome helper at the screen's
            // bezel-inset bottom — their position is already correct.
            if (alpha > 0.05f) {
                namespace mc = ::media_browser::ui::chrome;
                mc::draw_footer_hints(r, screen_w, screen_h, {
                    {mc::HintIcon::Btn1Yellow,  "\xE2\x88\x92" "10s"},  // −10s
                    {mc::HintIcon::Btn2Red,     "Pause/Play"},
                    {mc::HintIcon::Btn3Green,   "+10s"},
                    {mc::HintIcon::Btn4Black,   "Back"},
                    {mc::HintIcon::RotaryNav,   "Scrub"},
                    {mc::HintIcon::RotaryPress, "Open Menu"},
                });
            }
        }
    }

    // Overlay: bottom-1/3 panel with movie meta + similar-films carousel.
    // Drawn above all other HUD elements.
    overlay_.render(r, screen_w, screen_h);

    // End-of-episode countdown / season-end card — drawn LAST: its scrim
    // must dim everything, including any HUD remnants.
    render_end_overlay(r, screen_w, screen_h);

    (void)w;
    (void)h;
}

void PlaybackScreen::render_end_overlay(::ui::Renderer& r,
                                        int screen_w, int screen_h) {
    if (end_overlay_.kind == EndOverlayKind::None) return;

    namespace mc = ::media_browser::ui::chrome;
    const ::ui::Theme& th = r.mb_theme();

    // Dim scrim over the whole frame (the exit-modal idiom, a touch
    // stronger — the backdrop here is the last decoded video frame).
    r.mb_fill_rect(0.0f, 0.0f,
                   static_cast<float>(screen_w), static_cast<float>(screen_h),
                   th.bg, 0.65f);

    // Centered card — same chrome as the exit modal / overlay panels:
    // bg_lift fill, 2 px gold border on all four sides.
    const bool is_card = (end_overlay_.kind == EndOverlayKind::Card);
    const bool has_body = !end_overlay_.body_line.empty();
    constexpr int kCardW = 640;
    constexpr int kPadX = 32;
    constexpr int kPadY = 26;
    // Rows: title (~30), body/countdown line (~28 when present), action
    // area (button ~44 for cards, hint row ~24 for the countdown).
    const int card_h = kPadY + 30 + (is_card ? (has_body ? 28 : 0) + 16 + 44
                                             : 28 + 16 + 24) + kPadY;
    const int cx = (screen_w - kCardW) / 2;
    const int cy = (screen_h - card_h) / 2;
    const float fcx = static_cast<float>(cx);
    const float fcy = static_cast<float>(cy);
    const float fcw = static_cast<float>(kCardW);
    const float fch = static_cast<float>(card_h);
    r.mb_fill_rect(fcx, fcy, fcw, fch, th.bg_lift, 1.0f);
    r.mb_fill_rect(fcx, fcy, fcw, 2.0f, th.accent, 1.0f);
    r.mb_fill_rect(fcx, fcy + fch - 2.0f, fcw, 2.0f, th.accent, 1.0f);
    r.mb_fill_rect(fcx, fcy, 2.0f, fch, th.accent, 1.0f);
    r.mb_fill_rect(fcx + fcw - 2.0f, fcy, 2.0f, fch, th.accent, 1.0f);

    const float text_x = fcx + static_cast<float>(kPadX);
    const float max_text_w = static_cast<float>(kCardW - 2 * kPadX);
    float y = fcy + static_cast<float>(kPadY) + 22.0f;  // title baseline

    if (end_overlay_.kind == EndOverlayKind::Countdown) {
        // Title: "Next: SxEy · <episode title>" — body font (episode titles
        // are unbounded; the title font has no truncation helper), gold.
        const std::string title =
            truncate_to_width(r, end_overlay_.title_line, 20, max_text_w);
        r.mb_draw_text(title, text_x, y, 20, th.accent, 1.0f);

        // "Starting in N…" — N derived from the same frame clock update()
        // expires on, so the displayed count and the actual advance agree.
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - countdown_started_at_)
                .count();
        int remaining = kNextUpCountdownSeconds
                      - static_cast<int>(elapsed_ms / 1000);
        if (remaining < 1) remaining = 1;  // update() advances at expiry
        y += 28.0f + 16.0f;
        r.mb_draw_text("Starting in " + std::to_string(remaining)
                           + "\xE2\x80\xA6",
                       text_x, y, 16, th.fg, 1.0f);

        // Pinned hint row (the brief's copy): rotary press plays now,
        // BTN2 (red) stops.
        mc::draw_hint_row(r, cx + kPadX,
                          cy + card_h - kPadY,
                          {
                              {mc::HintIcon::RotaryPress, "Play now"},
                              {mc::HintIcon::Btn2Red, "Stop"},
                          });
        return;
    }

    // Season-end card. Title in the title font (short pinned strings),
    // optional body line, then the single action button — its label is the
    // model's primary_label ("Start Season N" or "Done"); Ok styling only
    // when it fires a real intent.
    r.mb_draw_title_text(end_overlay_.title_line, text_x, y, 22, th.accent,
                         1.0f);
    if (has_body) {
        y += 28.0f + 8.0f;
        const std::string body =
            truncate_to_width(r, end_overlay_.body_line, 16, max_text_w);
        r.mb_draw_text(body, text_x, y, 16, th.fg, 1.0f);
    }
    const int btn_y = cy + card_h - kPadY - 44;
    mc::draw_button(r, cx + kPadX, btn_y, end_overlay_.primary_label,
                    end_overlay_.has_primary ? mc::ButtonKind::Ok
                                             : mc::ButtonKind::Neutral,
                    /*focused=*/true);
}

}  // namespace media_browser::ui
