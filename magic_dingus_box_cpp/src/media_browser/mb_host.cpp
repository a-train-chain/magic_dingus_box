#include "media_browser/mb_host.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "app/app_state.h"
#include "app/controller.h"
#include "app/movie_quiet_mode.h"
#include "app/playback_reset.h"
#include "app/redraw_gate.h"
#include "app/settings_persistence.h"
#include "media_browser/artwork/artwork_cache.h"
#include "media_browser/library/watch_store.h"
#include "media_browser/mb_entry_gate.h"
#include "media_browser/media_ref.h"
#include "media_browser/ui/episode_logic.h"
#include "media_browser/ui/mb_transition.h"
#include "platform/egl_context.h"
#include "platform/gpio_manager.h"
#include "ui/renderer.h"
#include "ui/toast.h"
#include "ui/virtual_keyboard.h"
#include "utils/config.h"
#include "utils/logger.h"

namespace media_browser {

namespace {

// Persist the in-flight playback position for the active watch identity
// (resume-on-next-play). ORDERING CONTRACT — called at BOTH in-UI Playback
// exits (exit_media_browser() and the sibling-screen transition in the
// dispatcher) and at shutdown, ALWAYS BEFORE active_mb_screen_->leave().
// Controller::update_state runs at the BOTTOM of the frame loop, and only
// every other frame, so the position read here is up to 2 frames stale —
// but it was captured while the pipeline was still playing, which is
// exactly what a resume point wants. The hazard is ordering, not
// staleness: leave() calls controller.stop(), which zeroes the pipeline's
// position/duration, and the next Controller::update_state() copies the
// zeros into AppState — a post-leave (or later-frame) call would upsert
// (0, 0) and CLOBBER the resume point (WatchStore's MAX() ratchet
// protects only `watched`, never position). Skips when the identity is disengaged (untracked
// playback) or the position reads 0: a 0 write is at best worthless and
// at worst — when a prior checkpoint holds a real position — destroys it.
void flush_watch_state(media_browser::ui::PlaybackScreen& playback,
                       media_browser::library::WatchStore& store,
                       const app::AppState& state) {
    const auto id = playback.watch_identity();
    if (!id.has_value()) return;
    // A session abandoned on a pipeline error is not a viewing: its final
    // position must not be written, because upsert_position marks
    // watched past the threshold and "watched" is a natural-end-only
    // decision. The last 30 s checkpoint (real progress) still stands.
    if (playback.ended_on_error()) return;
    const double pos = state.get_position();
    if (pos <= 0.0) return;
    store.upsert_position(id->ref, id->season, id->episode,
                          pos, state.get_duration());
}

}  // namespace

MediaBrowserHost::MediaBrowserHost(Deps d)
    : state_(d.state),
      watch_store_(d.watch_store),
      ui_renderer_(d.renderer),
      egl_(d.egl),
      // Six screens — the host owns one instance of each.
      // sonarr_configured is exactly main.cpp's fallback-to-SonarrMockClient
      // condition — BrowseScreen needs to know it so a box that never
      // had Sonarr set up isn't told "Sonarr service offline" (final review
      // Fix 1; see BrowseScreen's constructor doc comment).
      mb_browse_(d.radarr, d.sonarr, d.tmdb, d.state,
                 /*sonarr_configured=*/d.sonarr_configured),
      mb_search_(d.radarr),
      mb_detail_(d.radarr, d.tmdb, d.prowlarr, &d.qbit, &d.watch_store),
      mb_series_detail_(d.sonarr, d.tmdb, &d.qbit,
                        /*sonarr_configured=*/d.sonarr_configured,
                        &d.watch_store),
      // Queue shows Sonarr's TV downloads alongside Radarr's movies. Gated
      // on sonarr_configured for the same reason BrowseScreen and
      // SeriesDetailScreen are: without a key `sonarr` is a
      // SonarrMockClient, whose fixture queue is a 3-row season pack —
      // passing it here would render a fake download on every box that
      // never set Sonarr up. nullptr keeps the screen movie-only.
      mb_queue_(d.radarr, &d.qbit,
                d.sonarr_configured ? &d.sonarr : nullptr),
      // Library mixes Radarr movies with Sonarr TV (Phase 3 Task 7). The
      // Sonarr pointer is null-gated on sonarr_configured exactly like
      // QueueScreen above — with no key, `sonarr` is a SonarrMockClient
      // whose fixture library would render phantom TV tiles on every box
      // that never set Sonarr up. WatchStore feeds the real Unwatched
      // filter; the screen reads it only on the render thread
      // (apply_pending), per its main-thread-only contract.
      mb_library_(d.radarr, d.sonarr_configured ? &d.sonarr : nullptr,
                  &d.watch_store, d.state),
      mb_playback_(d.controller, d.state, d.tmdb, d.radarr),
      // Manual release-picker screen — opened from Detail's "Pick a source"
      // button (Task 13) when the user wants to override Radarr's auto-pick.
      mb_release_picker_(d.radarr),
      // Task 23: the Movies Settings screen's "Hide Movies feature" checkbox
      // flips media_browser_unlocked=false and persists settings. The screen
      // triggers this callback AND returns Screen::Exit, so the dispatcher
      // naturally transitions back to MainMenu on the next input tick.
      mb_mb_settings_(
          d.radarr,
          d.prowlarr,
          d.state,
          [&state = d.state]() {
              state.media_browser_unlocked = false;
              app::SettingsPersistence::save_settings(state);
              ::ui::Toast::show("Movie section hidden");
              LOG_INFO("Media Browser: feature re-locked via Movies Settings screen");
          }),
      // enter() is called the first time we actually transition into the
      // Media Browser, so entering always starts fresh on Browse.
      active_mb_screen_(&mb_browse_),
      // Background watchdog for stalled downloads. The watchdog's tick is
      // disabled (see handle_input); Detail / ReleasePicker still register
      // watches on grab. Task 16.
      mb_watchdog_(d.radarr, d.qbit),
      last_checkpoint_(std::chrono::steady_clock::now()) {
    mb_playback_.set_quiet_mode(&d.movie_quiet_mode);
    // Pipeline errors end MB playback with a toast (never as natural EOS).
    mb_playback_.set_error_probe(std::move(d.player_error_probe));
    // Detail->ReleasePicker handoff: Detail forwards the (tmdb_id,
    // radarr_movie_id, title) tuple via this callback; the picker spawns
    // its own worker thread for the slow Radarr /api/v3/release call so
    // the UI stays responsive. Was synchronous in v1.7.x — froze the
    // render loop for 14-25s and would trip systemd's watchdog at the
    // old WatchdogSec=10 (we bumped to 30s in commit b2b8c4c, then
    // reverted to 10s once this async path landed).
    mb_detail_.set_picker_callback(
        [this](int tmdb_id, int radarr_movie_id, std::string title) {
            // Set the tmdb_id first so the picker's SELECT branch can
            // register a watchdog watch keyed on it; then kick off the
            // worker thread. load_async() returns immediately — render()
            // shows the Loading state until update() drains the result.
            mb_release_picker_.set_movie_tmdb_id(tmdb_id);
            mb_release_picker_.load_async(radarr_movie_id, std::move(title));
        });
    // The stall modal traps input while active and emits Pick / Dismiss
    // via these handlers.
    mb_stall_modal_.set_handlers(
        [this](int tmdb_id, int radarr_movie_id, const std::string& title) {
            // Deep-link path: when the stall event carries the movie's
            // radarr id (every grab since the v1.7.x async picker
            // refactor does), open the picker directly with a fresh
            // load_async() — no Detail intermediate flash. The picker
            // shows its Loading state instantly and the user is one
            // click away from picking a different release.
            //
            // Fallback path: older watches (registered before the id
            // plumbing landed, or any future code path that calls
            // watch() with radarr_movie_id=0) route through Detail. The
            // user can press "Pick a source" from there to open the
            // picker manually — more clicks but the same end state.
            if (radarr_movie_id > 0) {
                mb_release_picker_.set_movie_tmdb_id(tmdb_id);
                mb_release_picker_.load_async(radarr_movie_id, title);
                active_mb_screen_->leave();
                current_mb_screen_ = media_browser::ui::Screen::ReleasePicker;
                active_mb_screen_ = &mb_release_picker_;
                active_mb_screen_->enter();
                return;
            }
            // Fallback: open Detail and let the user re-pick from there.
            if (tmdb_id > 0) {
                mb_detail_.set_tmdb_id(tmdb_id);
                mb_detail_.set_origin(media_browser::ui::Screen::Browse);
            }
            active_mb_screen_->leave();
            current_mb_screen_ = media_browser::ui::Screen::Detail;
            active_mb_screen_ = &mb_detail_;
            active_mb_screen_->enter();
        },
        [this](int tmdb_id) {
            mb_watchdog_.snooze(tmdb_id, std::chrono::minutes(10));
        });

    // Wire the watchdog into the screens that initiate downloads.
    mb_detail_.set_watchdog(&mb_watchdog_);
    mb_release_picker_.set_watchdog(&mb_watchdog_);

    // Wire the VPN-health probe into Detail so its render() can show a
    // banner ("VPN tunnel down") on Add/Re-search modes when the tunnel
    // is unhealthy. Captures the AppState by reference; the lambda lives
    // as long as mb_detail_ (which owns the std::function).
    mb_detail_.set_vpn_health_provider([&state = d.state]() {
        return state.media_browser_vpn_healthy.load(std::memory_order_acquire);
    });
}

void MediaBrowserHost::enter_from_settings() {
    state_.current_screen = app::AppScreen::MediaBrowser;
    // Always start on the Browse landing screen.
    current_mb_screen_ = media_browser::ui::Screen::Browse;
    active_mb_screen_ = &mb_browse_;
    active_mb_screen_->enter();
}

// Feed the Media Browser unlock sequence detector. Chord wins over
// single-button events; otherwise we translate the first matching
// GPIO/rotary press edge of this tick into a SeqInput.
//
// IMPORTANT: when the sequence detector matches an event (PROGRESS
// or UNLOCKED), that event is CONSUMED — removed from input_events
// so the normal dispatch loop does not also fire its action.
// Otherwise entering the sequence would double-fire playback
// controls (e.g. triple BTN2 toggles play/pause three times, and
// the final RCLICK fires SELECT). Unmatched events (NO_MATCH)
// pass through normally, so random button use is unaffected.
void MediaBrowserHost::feed_unlock_sequence(
        std::vector<platform::InputEvent>& input_events,
        platform::GpioManager& gpio) {
    using namespace std::chrono;
    auto seq_now = steady_clock::now();
    platform::SeqInput seq_ev = platform::SeqInput::NONE;
    std::optional<size_t> consumed_event_idx;

    // Edge-detect the chord: gpio.is_chord_btn1_btn3() is a level
    // check, so feeding the detector every frame while the chord is
    // held would reset the state machine (BTN1_BTN3_CHORD is only
    // expected at step 0, so step>=1 would get reset). Only fire on
    // the false -> true transition.
    bool chord_now = gpio.is_available() && gpio.is_chord_btn1_btn3();
    if (chord_now && !chord_was_active_) {
        seq_ev = platform::SeqInput::BTN1_BTN3_CHORD;
        // For the chord case we don't track a single index; if the
        // detector matches we'll sweep input_events and remove both
        // PREV and NEXT press events from this frame.
    } else if (!chord_now) {
        for (size_t i = 0; i < input_events.size(); ++i) {
            const auto& e = input_events[i];
            if (!e.pressed) continue;
            platform::SeqInput mapped = platform::SeqInput::NONE;
            switch (e.action) {
                case platform::InputAction::PREV:
                    mapped = platform::SeqInput::BTN1_PRESS;
                    break;
                case platform::InputAction::PLAY_PAUSE:
                    mapped = platform::SeqInput::BTN2_PRESS;
                    break;
                case platform::InputAction::NEXT:
                    mapped = platform::SeqInput::BTN3_PRESS;
                    break;
                case platform::InputAction::SETTINGS_MENU:
                    mapped = platform::SeqInput::BTN4_PRESS;
                    break;
                case platform::InputAction::SELECT:
                    // Only the rotary-encoder push switch counts as
                    // ROTARY_CLICK; the A button / keyboard Enter
                    // never set this flag.
                    if (e.is_from_rotary) {
                        mapped = platform::SeqInput::ROTARY_CLICK;
                    }
                    break;
                default:
                    break;
            }
            if (mapped != platform::SeqInput::NONE) {
                seq_ev = mapped;
                consumed_event_idx = i;
                break;  // only feed one sequence input per frame
            }
        }
    }

    if (seq_ev != platform::SeqInput::NONE) {
        auto seq_result = seq_detector_.feed(seq_ev, seq_now);
        if (seq_result != platform::SeqResult::NO_MATCH) {
            // Consume the matched input so the normal dispatch loop
            // below does not also fire its action (prev/next,
            // play/pause, select, etc.).
            if (seq_ev == platform::SeqInput::BTN1_BTN3_CHORD) {
                // The chord is the combination of PREV + NEXT
                // presses arriving this frame; remove both.
                input_events.erase(
                    std::remove_if(input_events.begin(), input_events.end(),
                        [](const platform::InputEvent& e) {
                            return e.pressed &&
                                   (e.action == platform::InputAction::PREV ||
                                    e.action == platform::InputAction::NEXT);
                        }),
                    input_events.end());
            } else if (consumed_event_idx.has_value() &&
                       *consumed_event_idx < input_events.size()) {
                input_events.erase(input_events.begin() + *consumed_event_idx);
            }
        }
        if (seq_result == platform::SeqResult::UNLOCKED) {
            state_.media_browser_unlocked = true;
            app::SettingsPersistence::save_settings(state_);
            ::ui::Toast::show("Movie section unlocked");
            LOG_INFO("Media Browser: secret sequence matched, feature unlocked");
        }
    }

    // Update chord edge-detector state for next frame
    chord_was_active_ = chord_now;
}

::ui::VirtualKeyboard* MediaBrowserHost::search_text_keyboard() {
    if (state_.current_screen == app::AppScreen::MediaBrowser
        && current_mb_screen_ == media_browser::ui::Screen::Search
        && mb_search_.is_keyboard_active()) {
        return &mb_search_.keyboard();
    }
    return nullptr;
}

// Media Browser screen dispatcher (Task 17). When the user is in
// the Media Browser, the active MbScreen owns all input: it may
// consume events, transition to a sibling screen, or return
// Screen::Exit to hand control back to the main kiosk UI.
// Input events are NOT forwarded to main.cpp's input-handling loop,
// which prevents stray Menu / DPad / Select events from
// leaking into the main UI while the Media Browser is active.
// Every way OUT goes through exit_media_browser().
void MediaBrowserHost::handle_input(std::vector<platform::InputEvent>& input_events,
                                    int fb_w, int fb_h) {
    // ── Display-mode gate (per-frame invariant): no live MB surface
    // on a canvas that can't host it. The MB screens are authored
    // for the 720p logical canvas; CRT_NATIVE runs 640x480, where
    // the Library/filter panels (x=740..1220) sit entirely off the
    // canvas. Entry is gated in the settings menu, but the mode can
    // still flip WHILE the Media Browser is open: the web-admin
    // backup-restore poke reloads settings.json mid-session
    // (main.cpp's settings_reload_request handler), and the
    // main loop's mode-change block then resizes the logical canvas to
    // 640x480 the same frame. Evicting here — before this frame's
    // MB input handling and render — means not a single MB frame is
    // ever drawn on the small canvas.
    if (state_.current_screen == app::AppScreen::MediaBrowser &&
        !media_browser::display_supports_media_browser(
            state_.display_settings.mode == app::DisplayMode::CRT_NATIVE)) {
        exit_media_browser(input_events);
        ::ui::Toast::show(media_browser::kMoviesClosedByDisplaySwitchToast);
        LOG_INFO("Media Browser: evicted to MainMenu — display mode no "
                 "longer provides the 720p logical canvas");
    }

    if (state_.current_screen == app::AppScreen::MediaBrowser) {
        // ONE SEMANTIC PER PHYSICAL INPUT. BTN1/BTN3 arrive here as
        // PREV/NEXT and every MB surface consumes them: the five tab
        // screens use them for Marquee tab navigation, the exit and
        // stall modals for focus, Playback for seeking. Do NOT
        // synthesize ROTATE_VERTICAL (or anything else) from them in
        // this dispatcher — a duplicate event in the same frame makes
        // one press fire two actions (tab-switch + row-move on
        // Browse; focus-set + focus-toggle in the modals). Vertical
        // nav comes only from inputs that are natively vertical:
        // D-pad hat/buttons, analog stick Y, keyboard arrows, and
        // the phone remote's UP/DOWN (ABS_HAT0Y) — all mapped in
        // InputManager. The rotary encoder's ROTATE walks every
        // cursor-bearing MB screen, so controller-free enclosures
        // stay fully navigable.

        // BTN4 (SETTINGS_MENU) long-press detection. Short press
        // (< 500ms) forwards a synthetic `pressed` event to the active
        // screen so the existing per-screen "back" logic runs unchanged.
        // Long press (>= 500ms) bypasses the screen entirely and exits
        // the Media Browser back to the kiosk MainMenu. Centralizing
        // this in the dispatcher avoids adding ~10 lines of boilerplate
        // to every screen.
        constexpr auto kLongPressThreshold =
            std::chrono::milliseconds(500);
        bool btn4_long_press_exit = false;
        {
            std::vector<platform::InputEvent> forwarded;
            forwarded.reserve(input_events.size());
            for (const auto& e : input_events) {
                if (e.action == platform::InputAction::SETTINGS_MENU) {
                    if (e.pressed) {
                        mb_btn4_pressed_at_ =
                            std::chrono::steady_clock::now();
                        // Don't forward the press yet — wait for the
                        // release to decide short vs long.
                    } else {
                        if (mb_btn4_pressed_at_.has_value()) {
                            auto held =
                                std::chrono::steady_clock::now() -
                                *mb_btn4_pressed_at_;
                            mb_btn4_pressed_at_.reset();
                            if (held >= kLongPressThreshold) {
                                btn4_long_press_exit = true;
                            } else {
                                // Synthesize the short-press "pressed"
                                // event expected by the screens' existing
                                // SETTINGS_MENU handlers.
                                platform::InputEvent synth = e;
                                synth.pressed = true;
                                forwarded.push_back(synth);
                            }
                        }
                        // If we saw a release with no prior press, drop
                        // it — the press happened before we entered the
                        // Media Browser and is no longer meaningful.
                    }
                } else {
                    forwarded.push_back(e);
                }
            }
            input_events = std::move(forwarded);
        }

        // === Download stall watchdog — Task 16 ===
        // Tick every frame; the watchdog rate-limits its own service
        // polls internally to ~once per 10s, so the per-frame cost is
        // negligible. Any returned stall events surface as a
        // StallPromptModal (one-at-a-time — additional events stay
        // queued in the watchdog's internal state and the next free
        // frame raises the next one).
        //
        // Modal placement note: this runs BEFORE the exit-modal
        // intercept so the exit modal can take precedence if both
        // happen to be active in the same frame (user pressing BTN2
        // to leave the Marquee shouldn't be hijacked by a late stall
        // event firing the same tick).
        // ── Stall-prompt modal disabled (per user request) ─────────────
        // The DownloadWatchdog used to tick here every frame, polling
        // Radarr + qBit for stalled downloads and surfacing a modal
        // asking the user to pick a different release or snooze. The
        // operator found the modal pesky — it was firing on completed
        // downloads (likely a stale-watch-cleanup bug, not investigated
        // since we're removing the surface anyway). The watchdog object
        // still exists (Detail / ReleasePicker hold references and
        // register watches on grab — those become no-ops without the
        // tick), and the input/render hooks below stay since modal.is_active()
        // returns false forever now, making them cheap no-ops. If you
        // ever want to re-enable: restore the tick + show() block here.

        // === Stall modal intercept ===
        // Traps ALL input while active and consumes the queue so no
        // MB screen sees those events. Mirrors the exit-modal
        // pattern below. Must run BEFORE the exit-modal block — if
        // the stall modal is up, any BTN2 should commit/dismiss the
        // stall prompt, not open a redundant exit-confirm on top.
        if (mb_stall_modal_.is_active()) {
            bool consumed = mb_stall_modal_.handle_input(input_events);
            if (consumed) {
                input_events.clear();
            }
        }

        // === Global BTN2 (PLAY_PAUSE) intercept + exit modal ===
        // The exit modal owns BTN2 entirely while the MB is active.
        // When the modal is closed, a BTN2 press opens it.
        // When the modal is open, all input is funnelled to the modal
        // and consumed so no MB screen sees it.
        // On modal commit the teardown mirrors the Screen::Exit path.
        // (Skipped when BTN4 long-press already fired — double-exit
        // would call leave() on the wrong screen pointer.)
        bool mb_modal_exited = false;
        if (!btn4_long_press_exit) {
            for (auto it = input_events.begin(); it != input_events.end(); ) {
                const auto& e = *it;
                bool consume = false;

                if (mb_exit_modal_.is_open()) {
                    // Modal owns all recognisable inputs while visible.
                    switch (e.action) {
                        case platform::InputAction::PLAY_PAUSE:
                            if (e.pressed) { mb_exit_modal_.on_btn2(); consume = true; }
                            break;
                        case platform::InputAction::PREV:
                            if (e.pressed) { mb_exit_modal_.on_btn1(); consume = true; }
                            break;
                        case platform::InputAction::NEXT:
                            if (e.pressed) { mb_exit_modal_.on_btn3(); consume = true; }
                            break;
                        case platform::InputAction::SETTINGS_MENU:
                            if (e.pressed) { mb_exit_modal_.on_btn4(); consume = true; }
                            break;
                        case platform::InputAction::SELECT:
                            if (e.pressed) { mb_exit_modal_.on_select(); consume = true; }
                            break;
                        case platform::InputAction::ROTATE_VERTICAL:
                        case platform::InputAction::ROTATE:
                            if (e.delta != 0) { mb_exit_modal_.on_rotate(e.delta); consume = true; }
                            break;
                        default:
                            break;
                    }
                } else if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed
                           && current_mb_screen_ != media_browser::ui::Screen::Playback) {
                    mb_exit_modal_.open();
                    consume = true;
                }

                if (consume) {
                    it = input_events.erase(it);
                } else {
                    ++it;
                }
            }

            // Handle modal commit result before dispatching remaining
            // events to the active screen.
            auto modal_result = mb_exit_modal_.last_result();
            if (modal_result == media_browser::ui::ExitModal::Result::Exit) {
                exit_media_browser(input_events);
                mb_modal_exited = true;
            } else if (modal_result == media_browser::ui::ExitModal::Result::Cancel) {
                mb_exit_modal_.clear_result();
            }
        }

        if (btn4_long_press_exit) {
            // Long-press exit bypasses the screen entirely; the rest of
            // the main loop still runs this frame (rendering, etc.) with
            // current_screen flipped to MainMenu.
            exit_media_browser(input_events);
        } else if (!mb_modal_exited) {
        auto next = active_mb_screen_->handle_input(input_events);
        if (next == media_browser::ui::Screen::Exit) {
            exit_media_browser(input_events);
        } else if (next != current_mb_screen_) {
            transition_to(next, fb_w, fb_h);
        }
        active_mb_screen_->update();
        // Consume all input events so the main UI's per-event loop
        // sees an empty queue this frame.
        input_events.clear();
        }  // else if (!mb_modal_exited && !btn4_long_press_exit)
    }
}

// Screen-to-screen hand-off. WHAT moves between the two screens is
// decided by plan_transition() (ui/mb_transition.h, unit-tested); the
// ORDER below is the contract: every source screen is read before
// leave(), the watch-state flush runs before leave(), and the black clear
// runs before the new screen's enter().
void MediaBrowserHost::transition_to(media_browser::ui::Screen next,
                                     int fb_w, int fb_h) {
    using media_browser::ui::ArtworkAction;
    using media_browser::ui::DetailIdSource;
    using media_browser::ui::PlaybackHandoff;
    using media_browser::ui::SeriesIdSource;
    const media_browser::ui::TransitionPlan plan =
        media_browser::ui::plan_transition(current_mb_screen_, next);

    // When transitioning into Detail, forward the selected
    // tmdb_id from whichever source screen produced it so
    // Detail knows which movie to show.
    switch (plan.detail_id) {
        case DetailIdSource::Browse:
            mb_detail_.set_tmdb_id(mb_browse_.selected_tmdb_id());
            break;
        case DetailIdSource::Search:
            mb_detail_.set_tmdb_id(mb_search_.selected_tmdb_id());
            break;
        case DetailIdSource::LibraryMovie: {
            // Library is mixed-kind now; only movies reach
            // Detail (TV returns Screen::SeriesDetail, handled
            // below). Guard the kind anyway — the ids collide
            // across kinds, so feeding a TV id to the movie
            // Detail screen would show an unrelated film.
            const auto ref = mb_library_.selected_ref();
            if (ref.kind == media_browser::MediaKind::Movie) {
                mb_detail_.set_tmdb_id(ref.id);
            }
            break;
        }
        case DetailIdSource::None:
            break;
    }
    // Remember where we came from so BTN4 on Detail
    // returns the user to the screen that opened it
    // (preserves Search query/results, Library scroll,
    // etc.) instead of always landing on Browse.
    //
    // Skipped when returning from Playback or ReleasePicker
    // — both are sub-screens of Detail (user opened them
    // FROM Detail). Detail's origin should still point at
    // whatever screen opened Detail BEFORE the user
    // descended into the sub-screen, otherwise BTN4 on
    // Detail loops the user right back into the
    // sub-screen they just exited.
    if (plan.set_detail_origin) {
        mb_detail_.set_origin(current_mb_screen_);
    }

    // Browse and Library both produce the SeriesDetail transition, and
    // only for a TV hit — the kind lives in the transition
    // itself. The tmdb id comes from the respective
    // screen's selected accessor.
    if (plan.series_id != SeriesIdSource::None) {
        mb_series_detail_.set_tmdb_id(
            plan.series_id == SeriesIdSource::Browse
                ? mb_browse_.selected_tmdb_id()
                : mb_library_.selected_ref().id);
        mb_series_detail_.set_origin(current_mb_screen_);
    }
    // Playback -> SeriesDetail: drain the one-shot "Start
    // Season N" intent from the season-end card, PRE-leave.
    // Task 5's enter() clears an unconsumed intent, and this
    // is the only consumer — taking it here (before the
    // leave()/enter() pair below) is what makes the card's
    // primary actually start the season. SeriesDetail's tmdb
    // identity and origin are untouched: playback never
    // changes them, and pointing origin at Playback would
    // loop BTN4 straight back into the player.
    if (plan.take_next_season_intent) {
        if (auto s = mb_playback_.take_pending_next_season()) {
            mb_series_detail_.set_pending_intent_next_season(*s);
        }
    }

    // Detail -> Playback: copy resolved host path + title from
    // Detail to Playback so it knows what to load on enter().
    // Also pass the rich TMDB metadata so the PlaybackOverlay
    // can show a movie-detail header and pre-fetch similar films.
    if (plan.playback == PlaybackHandoff::FromDetail) {
        auto pt = mb_detail_.get_play_target();
        mb_playback_.set_movie(pt.host_path, pt.title);
        media_browser::ui::PlaybackOverlayMovieMeta meta;
        meta.tmdb_id     = pt.tmdb_id;
        meta.title       = pt.title;
        meta.year        = pt.year;
        meta.runtime_min = pt.runtime_min;
        meta.synopsis    = pt.synopsis;
        meta.genres      = pt.genres;
        meta.poster_url  = pt.poster_url;
        meta.cast        = pt.cast;
        meta.director    = pt.director;
        mb_playback_.set_movie_meta(std::move(meta));
        // Watch-state handoff (Task 4): one-shot resume offset
        // (cleared in PlaybackScreen::leave()), the identity
        // that checkpoint/EOS writes attribute to, and where
        // BTN4 / natural EOS should return.
        mb_playback_.set_start_position(pt.resume_position);
        mb_playback_.set_watch_identity(pt.watch_identity);
        mb_playback_.set_origin(media_browser::ui::Screen::Detail);
    }
    // SeriesDetail -> Playback (Task 6): the TV mirror of the
    // Detail handoff above, plus the episode context that arms
    // Task 5's end-of-episode overlay (countdown / season-end
    // card). SeriesDetail validated has_file + on-disk existence
    // before returning Screen::Playback, so host_path is
    // playable here.
    if (plan.playback == PlaybackHandoff::FromSeriesDetail) {
        auto pt = mb_series_detail_.get_play_target();
        mb_playback_.set_movie(pt.host_path, pt.display_title);
        media_browser::ui::PlaybackOverlayMovieMeta meta;
        // tmdb_id stays 0 ON PURPOSE: the overlay's similar-films
        // prefetch hits TMDB's MOVIE endpoints, and the movie/TV
        // id spaces overlap completely — a TV id here would fetch
        // an unrelated movie's "similar" rail. 0 = overlay
        // renders the meta header without similar films.
        meta.title       = pt.display_title;
        meta.year        = pt.year;
        meta.runtime_min = pt.runtime_min;
        meta.synopsis    = pt.synopsis;
        meta.genres      = pt.genres;
        meta.poster_url  = pt.poster_url;
        mb_playback_.set_movie_meta(std::move(meta));
        mb_playback_.set_start_position(pt.resume_position);
        mb_playback_.set_watch_identity(pt.identity);
        mb_playback_.set_origin(
            media_browser::ui::Screen::SeriesDetail);
        mb_playback_.set_episode_context(
            std::move(pt.episodes), std::move(pt.host_paths),
            std::move(pt.rows), std::move(pt.watch),
            std::move(pt.series_title));
    }
    // Artwork I/O contention guard: pause the artwork worker
    // when entering Playback so it doesn't compete with
    // GStreamer for read bandwidth on the USB SSD that holds
    // the library file. Resume on the way back out.
    //
    // Also trim the poster textures (the same release the game
    // launch does, but a TRIM so the playing film's and the
    // overlay's most-recent posters survive): GPU texture
    // memory is unswappable system RAM, and up to 256 MB of
    // posters competes with the decoder on a 1.5 GB Pi 4B.
    // Evicted posters rebuild lazily from the disk cache.
    if (plan.artwork == ArtworkAction::PauseAndTrim) {
        ui_renderer_.artwork_cache().pause();
        constexpr std::size_t kPlaybackArtworkBytes =
            32u * 1024u * 1024u;
        ui_renderer_.artwork_cache().trim_textures_to(
            kPlaybackArtworkBytes);
    } else if (plan.artwork == ArtworkAction::Resume) {
        ui_renderer_.artwork_cache().resume();
    }
    // Leaving Playback for a sibling screen (BTN4 back to
    // Detail, natural movie EOS, etc.): flush the watch
    // position BEFORE leave() — update_state runs at the
    // BOTTOM of the loop (every other frame), so the read is
    // up to 2 frames stale but pre-stop-valid; one line later
    // (post-leave, after stop() zeroes the pipeline) it
    // reads 0/0 and clobbers the resume point. The other
    // in-UI site is exit_media_browser(); see flush_watch_state.
    if (plan.flush_watch_state) {
        flush_watch_state(mb_playback_, watch_store_, state_);
    }
    active_mb_screen_->leave();
    current_mb_screen_ = next;
    switch (next) {
        case media_browser::ui::Screen::Browse:        active_mb_screen_ = &mb_browse_;      break;
        case media_browser::ui::Screen::Search:        active_mb_screen_ = &mb_search_;      break;
        case media_browser::ui::Screen::Detail:        active_mb_screen_ = &mb_detail_;      break;
        case media_browser::ui::Screen::SeriesDetail:
            active_mb_screen_ = &mb_series_detail_;
            break;
        case media_browser::ui::Screen::ReleasePicker: active_mb_screen_ = &mb_release_picker_; break;
        case media_browser::ui::Screen::Queue:         active_mb_screen_ = &mb_queue_;       break;
        case media_browser::ui::Screen::Library:       active_mb_screen_ = &mb_library_;     break;
        case media_browser::ui::Screen::Playback:      active_mb_screen_ = &mb_playback_;    break;
        case media_browser::ui::Screen::MovieSettings: active_mb_screen_ = &mb_mb_settings_; break;
        case media_browser::ui::Screen::Exit: break;  // handled above
    }

    // Clear the framebuffer to black before the new screen's
    // enter() runs. Some screens' enter() (notably Detail)
    // do synchronous HTTP work that blocks the render loop
    // for 200ms-2s. Without this clear, the user sees the
    // last-rendered frame frozen on screen — and if they
    // came from Playback, that frame can include garbage
    // texture data from the just-stopped GStreamer pipeline
    // (the "green rectangle in upper-right" the user
    // reported). A single black clear gives clean visual
    // continuity into the Loading state.
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    egl_.swap_buffers();

    active_mb_screen_->enter();
}

// THE way out of the Media Browser back to the kiosk MainMenu. Four
// exits share it — display-mode eviction, the exit modal's commit,
// BTN4 long-press, and a screen returning Screen::Exit — and each used
// to hand-write the same teardown, so its ordering rules lived in four
// places at once:
//
//   1. Watch-state flush BEFORE leave(). leave() stops the pipeline,
//      which zeroes position/duration; flushing after it writes (0, 0)
//      over the resume point. See flush_watch_state's contract.
//   2. Artwork worker resume. pause() is paired with resume() only in
//      the screen-transition branch, so exiting straight from Playback
//      would otherwise leave it paused forever (no posters next entry).
//      Idempotent — a no-op when not paused.
//   3. leave() on the active screen.
//   4. Exit modal closed and its result cleared, so a modal open at
//      exit cannot linger into the next session. (Only the eviction
//      path did this before; a BTN4 long-press with the modal up
//      carried it over.)
//   5. Main-menu renderable state restored — belt-and-braces companion
//      to the same reset at MB entry. Indexes or a stale UI fade
//      surviving into MainMenu make the Renderer early-return
//      (is_transitioning) or draw at alpha 0: a permanently blank menu.
//   6. Dispatcher back on Browse, so the next entry starts fresh, and
//      this frame's remaining input dropped so none of it leaks into
//      the main UI.
void MediaBrowserHost::exit_media_browser(
        std::vector<platform::InputEvent>& input_events) {
    if (current_mb_screen_ == media_browser::ui::Screen::Playback) {
        flush_watch_state(mb_playback_, watch_store_, state_);
    }
    ui_renderer_.artwork_cache().resume();
    active_mb_screen_->leave();
    mb_exit_modal_.close();
    mb_exit_modal_.clear_result();
    app::reset_main_ui_for_media_browser(state_);
    state_.current_screen = app::AppScreen::MainMenu;
    current_mb_screen_ = media_browser::ui::Screen::Browse;
    active_mb_screen_ = &mb_browse_;
    input_events.clear();
}

void MediaBrowserHost::pump_artwork() {
    mb_artwork_uploads_ += ui_renderer_.pump_artwork();
}

// The active MB screen may opt out of continuous drawing
// (MbScreen::wants_continuous_redraw — Browse, Search,
// Library, Detail, SeriesDetail when idle); the dispatcher's
// modals keep it continuous while shown.
bool MediaBrowserHost::wants_static_frame() const {
    return !active_mb_screen_->wants_continuous_redraw() &&
           !mb_exit_modal_.is_open() &&
           !mb_stall_modal_.is_active();
}

// A static MB screen: which screen, what it shows, posters.
void MediaBrowserHost::add_redraw_signature(app::ContentSignature& sig,
                                            bool screen_static) const {
    sig.add(static_cast<uint64_t>(current_mb_screen_));
    sig.add(mb_artwork_uploads_);
    if (screen_static) {
        sig.add(active_mb_screen_->redraw_signature());
    }
}

bool MediaBrowserHost::in_playback() const {
    return state_.current_screen == app::AppScreen::MediaBrowser &&
           current_mb_screen_ == media_browser::ui::Screen::Playback;
}

void MediaBrowserHost::render(int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);

    // Bind the UI shader, enable blending, and set screen-size
    // uniform before any MB screen draws. This is normally done
    // inside ui_renderer.render(state), but that's skipped for
    // Media Browser — and on the Playback screen, gst_renderer
    // runs immediately before this block and leaves its YUV
    // sampler bound. Without resetting state here, all MB draw
    // calls silently target the wrong shader (invisible scrub
    // overlay; "green rect" garbage on Playback→Detail exit).
    ui_renderer_.mb_begin_2d_state();
    // Drawn-frame tick for the artwork cache's eviction guard
    // (this block only runs on frames the redraw gate draws).
    ui_renderer_.begin_artwork_frame();

    // LOGICAL canvas size, not the physical mode. mb_begin_2d_state
    // pins the shader's screenSize uniform to the renderer's
    // logical canvas, so the screens must lay out in that same
    // space. These matched only because the framebuffer used to
    // always be 1280x720; at 1920x1080 they diverge by 1.5x and
    // every right-/bottom-anchored element (tab strip, footer
    // hints, filter panel) would fly off-screen. This is also
    // what keeps library_screen.cpp's literal 1280 constants and
    // mb_filter_overlay.cpp's kScreenW correct with no edits.
    const int mb_w = static_cast<int>(ui_renderer_.get_width());
    const int mb_h = static_cast<int>(ui_renderer_.get_height());

    {
    // UI batching scope (MDB_BATCH_UI): the screen and its modals
    // only draw through Renderer primitives, so they accumulate
    // into as few draws as their textures allow; the scope's end
    // flushes before the CRT overlay below.
    ::ui::Renderer::BatchScope mb_batch_scope(ui_renderer_);

    active_mb_screen_->render(ui_renderer_, mb_w, mb_h);

    // "Exit Marquee?" confirm modal — rendered above the active MB
    // screen but below the wood frame and CRT effects so it reads
    // as a native UI element. No-op when the modal is not open.
    mb_exit_modal_.render(ui_renderer_, mb_w, mb_h);

    // Download-stall prompt modal — same compositing slot as the
    // exit modal. is_active() inside render() makes it a no-op
    // when not shown. Task 16.
    mb_stall_modal_.render(ui_renderer_, mb_w, mb_h);
    }  // mb_batch_scope — flushed here

    // Poster uploads (pump_artwork(), called from main.cpp's redraw
    // gate block): they must run on skipped frames too, and an
    // upload must count as "the picture changed". Same rule as
    // before — skipped during Playback (texture allocation + GPU
    // upload is the most expensive non-decode work in the frame
    // budget, and the operator can't see the posters; pending
    // fetches resume on the way back to a menu screen).

    // CRT effects overlay on Media Browser menu screens (Browse,
    // Library, Search, Detail, Queue, Settings). Same legacy
    // procedural overlay that wraps the kiosk main UI's playlist
    // grid — gives the Marquee section the same nostalgic CRT
    // look operators get on the home menu.
    //
    // SKIPPED during Playback so movies stay clean and unfiltered
    // (operator direction). Also skipped if all MB CRT intensity
    // values are 0 — render_crt_effects has its own any-active
    // early-out, and we don't pay for the call.
    //
    // Wood frame draws AFTER this (next block) so the cabinet
    // art always reads as foreground over the CRT haze.
    //
    // The Marquee CRT intensities live in
    // state_.display_settings.mb_* — independent from the kiosk
    // values that drive the home-menu look. We swap the mb_*
    // values into the kiosk fields just for this render call,
    // then restore — so render_crt_effects (which reads the
    // standard fields) renders the MB look without needing to
    // know about the dual-store. Restoration is unconditional
    // so the rest of the frame (and the home menu when the
    // user pops out of MB) sees the original kiosk values.
    if (current_mb_screen_ != media_browser::ui::Screen::Playback) {
        auto& s = state_.display_settings;
        const float k_scan  = s.scanline_intensity;
        const float k_warm  = s.warmth_intensity;
        const float k_glow  = s.glow_intensity;
        const float k_mask  = s.rgb_mask_intensity;
        const float k_bloom = s.bloom_intensity;
        const float k_inter = s.interlacing_intensity;
        const float k_flick = s.flicker_intensity;
        s.scanline_intensity    = s.mb_scanline_intensity;
        s.warmth_intensity      = s.mb_warmth_intensity;
        s.glow_intensity        = s.mb_glow_intensity;
        s.rgb_mask_intensity    = s.mb_rgb_mask_intensity;
        s.bloom_intensity       = s.mb_bloom_intensity;
        s.interlacing_intensity = s.mb_interlacing_intensity;
        s.flicker_intensity     = s.mb_flicker_intensity;

        ui_renderer_.render_crt_effects(state_, /*scanlines_enabled=*/true);

        s.scanline_intensity    = k_scan;
        s.warmth_intensity      = k_warm;
        s.glow_intensity        = k_glow;
        s.rgb_mask_intensity    = k_mask;
        s.bloom_intensity       = k_bloom;
        s.interlacing_intensity = k_inter;
        s.flicker_intensity     = k_flick;
    }

    // Marquee wood-frame overlay — the "TV cabinet" that gives the
    // Media Browser its distinct visual identity from the rest of
    // the kiosk. Drawn ON TOP of MB content (after pump_artwork)
    // so the frame visually covers the outer pixels of any
    // CRT-processed output, producing the design's inset-CRT
    // effect without needing a real CRT region change. Toasts
    // still render on top of the frame (intentional — toast text
    // must always be readable).
    //
    // load_marquee_frame() dedupes by path internally, like
    // load_bezel(). Calling it every frame is correct: it
    // re-uploads the texture after reset_gl() (e.g. RetroArch
    // return), and a static path tracker would defeat that.
    //
    // Gated on the MovieSettings "Wood frame during playback"
    // toggle for the Playback screen specifically. When that
    // toggle is OFF and we're playing a movie, the frame is
    // skipped so the video fills the whole framebuffer. Other
    // MB screens (Browse, Library, Search, Detail, Settings,
    // Queue) ALWAYS render the frame regardless of the toggle —
    // it's only the playback experience that operators can
    // choose to show full-screen.
    const bool show_frame =
        (current_mb_screen_ != media_browser::ui::Screen::Playback) ||
        state_.display_settings.mb_playback_show_frame;
    if (show_frame) {
        ui_renderer_.load_marquee_frame("marquee_frame.png");
        ui_renderer_.render_marquee_frame();
    }
}

// ── Watch-state checkpoint + EOS watched marking (Task 4) ────────
void MediaBrowserHost::tick_watch_state() {
    // 30 s position checkpoints while a tracked file plays on the
    // MB Playback screen — last_checkpoint_ plays the role
    // last_status_write does in main.cpp. Outside Playback the timer is
    // continually re-armed, so every session waits a FULL interval
    // before its first write (a stale/0 position read right after
    // enter() can never be checkpointed — doubly guarded by the
    // pos > 0 skip, which also keeps a paused-at-EOS or mid-seek 0
    // read from clobbering the resume point).
    const bool in_mb_playback =
        state_.current_screen == app::AppScreen::MediaBrowser &&
        current_mb_screen_ == media_browser::ui::Screen::Playback;
    const auto cp_now = std::chrono::steady_clock::now();
    if (!in_mb_playback) {
        last_checkpoint_ = cp_now;
    } else if (cp_now - last_checkpoint_ >=
               std::chrono::milliseconds(
                   media_browser::ui::kCheckpointIntervalMs)) {
        last_checkpoint_ = cp_now;
        const auto wid = mb_playback_.watch_identity();
        const double wpos = state_.get_position();
        if (wid.has_value() && wpos > 0.0) {
            watch_store_.upsert_position(wid->ref, wid->season,
                                        wid->episode, wpos,
                                        state_.get_duration());
        }
    }

    // EOS → watched. take_eos_watched() is consume-once per latch,
    // so this per-frame poll costs exactly ONE SQLite write per
    // end-of-stream — never a per-frame write while Task 5's
    // countdown / season-end card idles on screen. Kind-agnostic
    // on purpose: watched marking applies to movies and TV alike
    // (only the exit_pending_ EOS behavior stays movie-only, and
    // that lives inside PlaybackScreen, unchanged this task).
    if (auto eos_id = mb_playback_.take_eos_watched()) {
        watch_store_.mark_watched(eos_id->ref, eos_id->season,
                                 eos_id->episode);
    }
}

void MediaBrowserHost::on_external_seek() {
    // The seek landed on the SHARED pipeline — during MB playback
    // that is the movie/episode. PlaybackScreen's own seek handlers
    // pump its EOS-flicker suppression on every seek; an external
    // phone seek must do the same or the FLUSH-seek video_active
    // flicker reads as "movie ended" and playback bails to Detail.
    if (in_playback()) {
        mb_playback_.notify_external_seek();
    }
}

// SHUTDOWN watch-state flush site. The others (exit_media_browser()
// and the dispatcher's sibling-screen transition) cover deliberate
// in-UI exits from Playback; this one covers the process being told
// to stop while a movie or episode is still on screen.
//
// It stopped being an edge case on 2026-08-03, when the front panel
// gained a standby switch and a restart button: both stop this
// service via SIGTERM, so "flip to standby partway through an
// episode" is now an ordinary thing an owner does, many times a day.
// Without this, the resume point falls back to the last 30-second
// checkpoint and the box appears to forget where you were.
//
// Ordering is the same contract the other sites document: main.cpp
// MUST call this before its cleanup, because player.cleanup() stops
// the pipeline and zeroes position — flushing after it would write
// (0, 0) over a real resume point. flush_watch_state itself no-ops
// when nothing is playing or the position reads 0, so the menu-idle
// case costs one function call.
void MediaBrowserHost::flush_before_shutdown() {
    if (in_playback()) {
        flush_watch_state(mb_playback_, watch_store_, state_);
        LOG_INFO("Flushed watch position before shutdown");
        // Stopping mid-movie never ran leave(), so the contention guard
        // stayed engaged (torrents paused / capped, containers stopped)
        // until some later session. Queue the resume now (after the flush,
        // which must read the live position); main.cpp drains it.
        mb_playback_.leave();
    }
}

}  // namespace media_browser
