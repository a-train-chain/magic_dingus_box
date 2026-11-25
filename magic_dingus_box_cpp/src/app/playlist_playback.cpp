#include "playlist_playback.h"

#include "auto_advance.h"
#include "playback_reset.h"
#include "playlist_reload.h"
#include "tuner_snow.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>

namespace app {

namespace {

// The same airing of a channel's schedule: the same slot (index), still
// holding the same item, starting at the same time — half a second of
// slack for the rounding between two on_air() calls, as boundary_join
// allows (airings are >= kMinScheduledItemS apart).
bool same_airing(const LoadedAiring& a, const LoadedAiring& b) {
    return a.index == b.index && a.item.playlist == b.item.playlist &&
           a.item.item == b.item.item && std::fabs(a.starts_at - b.starts_at) <= 0.5;
}

}  // namespace

PlaylistPlayback::PlaylistPlayback(AppState& state, PlaylistTransport& transport,
                                   const std::string& playlist_directory)
    : state_(state), transport_(transport), playlist_directory_(playlist_directory) {}

// ── Failed-item handling (main playlist / Master Shuffle) ────────────────

bool PlaylistPlayback::owns_pipeline() const {
    bool owns = state_.current_playlist_item() != nullptr &&
                state_.intro_complete && !state_.showing_intro_video &&
                !state_.is_switching_playlist && !state_.is_loading_game;
#ifdef MEDIA_BROWSER_ENABLED
    // MB PlaybackScreen owns its own error path (toast + exit).
    owns = owns && state_.current_screen != app::AppScreen::MediaBrowser;
#endif
    return owns;
}

bool PlaylistPlayback::channel_load_live() const {
    bool live = state_.current_playlist_item() != nullptr &&
                state_.intro_complete && !state_.showing_intro_video &&
                !state_.is_loading_game;
#ifdef MEDIA_BROWSER_ENABLED
    live = live && state_.current_screen != app::AppScreen::MediaBrowser;
#endif
    return live;
}

void PlaylistPlayback::hold_load_deadlines_while_busy() {
    if (!transport_.pipeline_busy()) return;
    const auto now = transport_.now();
    if (channel_load_at_) channel_load_at_ = now;
    if (state_.is_switching_playlist) state_.playlist_switch_start_time = now;
}

void PlaylistPlayback::drop_settled_channel_load() {
    if (channel_load_at_ && (state_.playback_started_ || !channel_load_live())) {
        channel_load_at_.reset();
    }
}

int PlaylistPlayback::failure_budget() const {
    if (const Channel* ch = channel_at(state_.tuned_channel)) {
        // On a channel: one attempt per scheduled item, the 00 mix capped
        // like Master Shuffle — not the size of whichever source playlist
        // the failing item happens to come from.
        return video::PlaybackErrorPolicy::failure_budget(
            static_cast<int>(ch->items.size()), state_.tuned_channel == 0);
    }
    int size = 1;
    if (state_.current_playlist_index >= 0 &&
        state_.current_playlist_index <
            static_cast<int>(state_.playlists.size())) {
        size = static_cast<int>(
            state_.playlists[state_.current_playlist_index].items.size());
    }
    return video::PlaybackErrorPolicy::failure_budget(
        size, state_.master_shuffle_active);
}

void PlaylistPlayback::exclude_failed_channel() {
    const int channel = state_.tuned_channel;
    auto& gave_up = state_.guide_gave_up;
    if (channel >= 0 && std::find(gave_up.begin(), gave_up.end(), channel) == gave_up.end())
        gave_up.push_back(channel);
    const auto id = channel_identity(channel);
    if (!id.empty() && std::find(gave_up_identities_.begin(), gave_up_identities_.end(), id) == gave_up_identities_.end())
        gave_up_identities_.push_back(id);
    state_.tuned_channel = -1;
    state_.guide_wait_until_utc = 0.0;
}

void PlaylistPlayback::act_on_failed_item(video::PlaybackErrorPolicy::Decision d,
                                          const char* why) {
    using Decision = video::PlaybackErrorPolicy::Decision;
    if (d != Decision::None) state_.on_demand = false;
    // Counted (or handed on): a channel load's bound must not count it again.
    channel_load_at_.reset();
    const auto* failed = state_.current_playlist_item();
    const bool movie = failed && failed->movie_tmdb_id > 0;
    if (movie && d != Decision::None && movie_failure_hook_) movie_failure_hook_();
    if (movie && d == Decision::Advance && guide_style()) {
        const int channel = state_.tuned_channel;
        // A selected future film may finish/fail after today's close. Only
        // that chosen film has permission to run overnight, never its next
        // scheduled programme. Parking keeps the normal return identity and
        // gives 00 its own failure budget.
        if (park_off_air(channel)) return;
        const Channel* ch = channel_at(channel);
        const auto offset = [this](double utc) { return transport_.utc_offset_s(utc); };
        std::optional<OnAir> next;
        if (ch && loaded_airing_ && loaded_airing_->channel == channel) {
            const auto& held = loaded_airing_->airing;
            const auto still = on_air(*ch, held.starts_at, offset(held.starts_at), offset);
            // Rebuilds may re-phase or replace media. Only the original
            // stable airing may advance; stale numeric coordinates cannot.
            if (still && held.index >= 0 && still->index == held.index &&
                same_airing(held, LoadedAiring{still->index, ch->items[still->index],
                                              still->starts_at, still->ends_at, held.utc_offset_s}) &&
                std::fabs(still->ends_at - held.ends_at) <= 0.5 &&
                (!held.utc_offset_s || *held.utc_offset_s == offset(transport_.wall_now_utc()))) {
                next = airing_after(*ch, *still, offset(still->ends_at), offset);
            } else {
                next = join_point(*ch, transport_.wall_now_utc(), offset(transport_.wall_now_utc()), offset);
            }
        }
        transport_.stop();
        if (next) {
            next->offset_s = 0;
            load_on_air(channel, *next, true);
        } else {
            stop_to_menu(state_);
            // No later movie exists tonight: let the same-frame auto-tune
            // choose another live channel/00. Exclude this stable identity
            // so it cannot immediately retune the failed final airing.
            exclude_failed_channel();
        }
        return;
    }
    if (d == Decision::Advance && guide_style()) {
        // Live TV does not jump ahead to the next programme: the channel
        // stays dark until this item's slot ends, then main.cpp's
        // nothing-playing rule re-tunes it — joining the item after it
        // rather than reloading this one. The failure has been counted.
        // Never longer than the item itself from now: a wall clock that
        // jumped BACK since the load would otherwise stretch the wait by
        // the jump.
        const bool on_channel = channel_at(state_.tuned_channel) != nullptr;
        std::cerr << "Channel " << state_.tuned_channel << " item failed (" << why
                  << ") — the channel resumes with its next scheduled item"
                  << std::endl;
        transport_.stop();
        app::stop_to_menu(state_);
        state_.guide_wait_until_utc =
            on_channel ? std::min(guide_item_ends_at_,
                                  transport_.wall_now_utc() + guide_item_length_)
                       : 0.0;
        return;
    }
    if (d == Decision::Advance) {
        std::cerr << "Playlist item " << state_.current_item_index
                  << " failed (" << why << ") — skipping to next item"
                  << std::endl;
        if (state_.master_shuffle_active) {
            // Records the failed item in the PREV history (the virtual
            // row is filtered there), then picks the next random video.
            transport_.master_shuffle_advance(state_, playlist_directory_);
        } else {
            transport_.load_next_item(state_, playlist_directory_);
        }
    } else if (d == Decision::GiveUp) {
        std::cerr << "Playlist item failed (" << why << ") and too many "
                  << "consecutive items failed — stopping playback"
                  << std::endl;
        // Same end state as load_next_item's all-items-failed branch.
        transport_.stop();
        app::stop_to_menu(state_, "Couldn't play these videos");
        if (guide_style()) {
            // Re-tuning the channel that just failed repeatedly would loop:
            // note it, and leave nothing tuned for the nothing-playing rule
            // to move on from.
            exclude_failed_channel();
        }
    }
}

// ── Main-menu input ──────────────────────────────────────────────────────

void PlaylistPlayback::on_select() {
    // Don't allow playlist selection during intro video
    if (state_.showing_intro_video) {
        return;  // Ignore input during intro
    }

    // A switch is waiting for the old stream to settle: the pipeline is
    // stopped, and the old code froze through this window. Any SELECT
    // here would act on a stale picture (e.g. "same playlist" would fade
    // the menu out over the stopped video), so it is dropped. Likewise for
    // the settle after the stuck-switch timeout's recovery stop().
    if (switch_pending() || transport_.now() < timeout_settle_until_) {
        return;
    }

    // If video is playing and UI is hidden, just show UI
    if (state_.video_active && !state_.ui_visible_when_playing) {
        state_.ui_visible_when_playing = true;
        state_.ui_visibility_timer = 3.0; // Show for 3 seconds
        return; // Don't trigger selection yet
    }

    // The cursor is on the Picture control (below the last playlist row,
    // app/overlay_nav.h): toggle crop for the playing video. It never
    // switches playlist, never hides the overlay, never touches playback.
    // main.cpp drops the focus each frame once the control is gone; a press
    // that lands before that, after the pipeline changed hands, is dropped
    // too — it was aimed at a control, not at the row under it. The focus
    // rides through the load gap between items, where video_active is
    // briefly false and the reveal above does not fire: a press over the
    // hidden overlay there still only reveals it.
    if (state_.picture_focus) {
        if (!owns_pipeline()) return;
        if (!state_.ui_visible_when_playing) {
            state_.ui_visible_when_playing = true;
            state_.ui_visibility_timer = 3.0;
            return;
        }
        transport_.toggle_picture_crop();
        return;
    }

    // If video is already playing
    if (state_.video_active) {
        // Check if the selected playlist is the same as the one currently playing
        // Special case for Master Shuffle (index 0): current_playlist_index points to the source playlist,
        // so we check master_shuffle_active flag instead.
        bool is_same_playlist = (state_.current_playlist_index == state_.selected_index) ||
                                (state_.master_shuffle_active && state_.selected_index == 0);

        if (is_same_playlist) {
            // Same playlist: just toggle UI visibility with fade
            state_.ui_visible_when_playing = !state_.ui_visible_when_playing;

            // Start fade animation (synchronized UI and audio)
            state_.fade_start_time = std::chrono::steady_clock::now();
            state_.fade_target_ui_visible = state_.ui_visible_when_playing;
            state_.is_fading = true;
        } else {
            // Different playlist: stop current and start new playlist
            // Prevent overlapping playlist switches
            if (state_.is_switching_playlist) {
                return;  // Skip if already switching
            }

            state_.is_switching_playlist = true;  // Set flag to prevent overlapping operations
            switch_start_generation_ = transport_.stream_generation();
            state_.playlist_switch_start_time = transport_.now();  // Track when switch started

            // First, update the playlist index BEFORE stopping to prevent reset
            state_.current_playlist_index = state_.selected_index;
            state_.current_item_index = 0;

            // Reset advance flags when switching playlists to prevent issues
            state_.last_advanced_item_index = -1;
            state_.last_advanced_duration = 0.0;
            // No volume reset here: nothing dims the stream any more,
            // and forcing 100% blasted the outgoing video at full
            // level until stop() landed. load_file re-applies the
            // user's volume to the new stream.

            transport_.stop();
            // Let the stop complete and the old stream's buffers be released
            // before loading (the DRM driver needs time to release GEM
            // buffers from the previous video), then verify the player
            // really stopped. Both waits used to be sleeps right here, on
            // the render thread; they now run frame by frame from
            // tick_switch_timeout() -> advance_pending_switch(), which ends
            // in finish_switch(). The playlist to load is captured NOW: the
            // menu cursor may move while the switch waits.
            pending_.phase = SwitchPhase::Settling;
            pending_.target = state_.selected_index;
            pending_.next_check = transport_.now() + kSwitchSettle;
            pending_.repolls = 0;
        }
    } else {
        // No video playing: start the selected playlist
        // Prevent overlapping playlist switches
        if (state_.is_switching_playlist) {
            return;  // Skip if already switching
        }

        // Check for Master Shuffle (index 0)
        if (state_.selected_index == 0) {
            std::cout << "Master Shuffle selected (from stopped)!" << std::endl;
            state_.is_switching_playlist = true;
            switch_start_generation_ = transport_.stream_generation();
            state_.playlist_switch_start_time = transport_.now();
            state_.master_shuffle_active = true;

            transport_.play_random_global_video(state_, playlist_directory_);

            // When starting video, hide UI completely so video shows through fully
            state_.ui_visible_when_playing = false;

            // Note: play_random_global_video handles loading, but doesn't return success/fail
            // We assume it works or retries.
        } else if (!state_.playlists.empty() && state_.selected_index < static_cast<int>(state_.playlists.size())) {
            state_.master_shuffle_active = false; // Disable master shuffle for normal playlists
            const auto& pl = state_.playlists[state_.selected_index];
            if (!pl.items.empty() && pl.is_video_playlist()) {
                state_.is_switching_playlist = true;  // Set flag
                switch_start_generation_ = transport_.stream_generation();
                state_.playlist_switch_start_time = transport_.now();

                // Load first item of playlist
                auto load_result = transport_.load_playlist_item(state_, pl, 0, playlist_directory_);
                if (load_result) {
                    // Track which playlist and item is playing
                    state_.current_playlist_index = state_.selected_index;
                    state_.current_item_index = 0;
                    // When starting video, hide UI completely so video shows through fully
                    state_.ui_visible_when_playing = false;
                } else {
                    // Load failed - clear flag and show error
                    std::cerr << "Failed to load playlist item: " << load_result.error() << std::endl;
                    state_.set_error("Could not load: " + pl.title);
                    state_.is_switching_playlist = false;
                }
            } else if (!pl.items.empty() && pl.is_game_playlist()) {
                state_.set_error("Use Settings to launch games");
            } else {
                state_.set_error("No content in playlist");
            }
        }
    }
}

void PlaylistPlayback::on_play_pause() {
    // Only allow play/pause if intro is complete and we have an active video
    // (not a pending switch's stopped one: toggling the stopped pipeline
    // could restart the stream the switch just stopped).
    if (state_.intro_complete && state_.video_active && !switch_pending()) {
        transport_.toggle_pause();
    }
}

void PlaylistPlayback::on_next() {
    if (guide_style()) {
        on_channel_step(+1);  // channel up
        return;
    }
    // If video is playing, advance to next playlist item
    // In Master Shuffle mode, pick another random video
    // Otherwise, seek forward in current video
    // Don't allow if we're switching playlists (a pending switch always
    // has is_switching_playlist set; checked on its own for clarity).
    if (switch_pending()) return;
    if (!state_.is_switching_playlist && state_.video_active && state_.current_playlist_index >= 0) {
        if (state_.master_shuffle_active) {
            // In Master Shuffle, NEXT triggers another random video,
            // saving the current one to the shuffle history for
            // "Previous". Index 0 is the VIRTUAL Master Shuffle row
            // (one dummy item, no file), never a real source
            // playlist — the reload path parks
            // current_playlist_index there when a source playlist is
            // deleted mid-playback; record_history refuses it (see
            // app/shuffle_queue.h for why recording it stalled
            // auto-advance).
            transport_.master_shuffle_advance(state_, playlist_directory_);
        } else {
            transport_.load_next_item(state_, playlist_directory_);
        }
    } else if (!state_.is_switching_playlist && state_.video_active) {
        // Only seek if we have an active video (not intro)
        transport_.seek(10.0);
    }
}

void PlaylistPlayback::on_prev() {
    if (guide_style()) {
        on_channel_step(-1);  // channel down
        return;
    }
    // If video is playing, go to previous playlist item
    // In Master Shuffle mode, pick another random video
    // Otherwise, seek backward in current video
    // Don't allow if we're switching playlists
    if (switch_pending()) return;
    if (!state_.is_switching_playlist && state_.video_active && state_.current_playlist_index >= 0) {
        if (state_.master_shuffle_active) {
            // In Master Shuffle, PREV goes back through shuffle
            // history (random pick when it is empty).
            transport_.master_shuffle_back(state_, playlist_directory_);
        } else {
            transport_.load_previous_item(state_, playlist_directory_);
        }
    } else if (!state_.is_switching_playlist && state_.video_active) {
        // Only seek if we have an active video (not intro)
        transport_.seek(-10.0);
    }
}

// ── Channel Guide style ──────────────────────────────────────────────────
// The join rules (join_point for a tune, boundary_join at an item's end)
// are app/channel_schedule.h's: one copy, shared with the guide's view.

const Channel* PlaylistPlayback::channel_at(int c) const {
    if (!channels_ || c < 0 || c >= static_cast<int>(channels_->size())) return nullptr;
    return &(*channels_)[c];
}

bool PlaylistPlayback::channel_on_air(int channel) const {
    const auto* ch = channel_at(channel);
    if (!ch) return false;
    const double now = transport_.wall_now_utc();
    const auto offset = [this](double utc) { return transport_.utc_offset_s(utc); };
    return join_point(*ch, now, offset(now), offset).has_value();
}

std::string PlaylistPlayback::channel_identity(int c) const {
    const Channel* channel = channel_at(c);
    if (!channel) return {};
    if (!channel->identity.empty()) return channel->identity;
    // Keep hand-built channels useful; build_channels always fills identity.
    if (c == 0) return kShuffleChannelId;
    return c < static_cast<int>(state_.playlists.size())
        ? playlist_identity(state_.playlists[c]) : std::string{};
}

void PlaylistPlayback::reanchor_airing(std::optional<ChannelAiring>& held) {
    if (!held) return;
    held->channel = -1;
    held->airing.index = -1;  // absent/replaced media must not validate by stale coordinates
    if (!channels_) return;
    for (int row = 0; row < static_cast<int>(channels_->size()); ++row) {
        if (channel_identity(row) != held->identity) continue;
        held->channel = row;
        const auto& items = (*channels_)[row].items;
        for (int index = 0; index < static_cast<int>(items.size()); ++index) {
            const auto& scheduled = items[index];
            if (scheduled.playlist < 0 || scheduled.playlist >= static_cast<int>(state_.playlists.size())) continue;
            const auto& source = state_.playlists[scheduled.playlist];
            if (playlist_identity(source) != held->source_identity || scheduled.item < 0 ||
                scheduled.item >= static_cast<int>(source.items.size())) continue;
            const auto& item = source.items[scheduled.item];
            if (item.path != held->source_path || item.movie_tmdb_id != held->source_tmdb_id ||
                item.start != held->source_start) continue;
            held->airing.index = index;
            held->airing.item.playlist = scheduled.playlist;
            held->airing.item.item = scheduled.item;
            return;
        }
        return;
    }
}

void PlaylistPlayback::set_channels(const std::vector<Channel>* channels) {
    state_.channel_ident = {};
    channels_ = channels;
    // Reload can renumber a tune during its cooperative settle. Its target
    // is the channel chosen at the press, independent of the new row order.
    if (switch_pending() && pending_.tune && state_.is_switching_playlist &&
        !state_.is_loading_game && !state_.showing_intro_video) {
        int row = -1;
        if (channels_) {
            for (int c = 0; c < static_cast<int>(channels_->size()); ++c) {
                if (channel_identity(c) == pending_.identity) { row = c; break; }
            }
        }
        reanchor_airing(pending_.selected);
        if (row < 0 || (pending_.selected && pending_.selected->airing.index < 0)) {
            pending_ = PendingSwitch{};
            transport_.stop();
            stop_to_menu(state_);
        } else {
            pending_.target = row;
            state_.current_playlist_index = row;
        }
    }
    if (!guide_style()) {
        state_.on_demand = false;
        state_.off_air_return_after_utc = 0;
        if (state_.retained_playlist_item()) {
            transport_.stop();
            stop_to_menu(state_);
        }
        // List style: nothing is tuned, and nothing is owed a retry.
        state_.tuned_channel = -1;
        state_.off_air_return_id.clear();
        timezone_reload_pending_ = false;
        state_.guide_gave_up.clear();
        gave_up_identities_.clear();
        state_.guide_wait_until_utc = 0.0;
        channel_progress_ = {};
        channel_load_at_.reset();
        loaded_airing_.reset();
        stall_reloaded_.reset();
        return;
    }
    // Remap only coordinates. boundary_join still revalidates the original
    // start/end against the new schedule, so a re-phase invalidates its guard.
    reanchor_airing(loaded_airing_);
    reanchor_airing(stall_reloaded_);
    // Given-up rows stay until a channel proves it plays; only rows the new
    // list no longer has are dropped.
    auto& gave_up = state_.guide_gave_up;
    if (!gave_up_identities_.empty()) {
        gave_up.clear();
        for (int row = 0; row < static_cast<int>(channels_->size()); ++row) {
            if (std::find(gave_up_identities_.begin(), gave_up_identities_.end(), channel_identity(row)) != gave_up_identities_.end())
                gave_up.push_back(row);
        }
    }
    gave_up.erase(std::remove_if(gave_up.begin(), gave_up.end(),
                                 [this](int c) { return channel_at(c) == nullptr; }),
                  gave_up.end());
    if (state_.tuned_channel < 0 && !state_.retained_playlist_item()) return;
    // A reload renumbers the rows: re-find the tuned channel by the identity
    // it was tuned under rather than keep an index into the old list.
    const std::string id = state_.loaded_movie
        ? state_.loaded_movie->channel_identity
        : state_.loaded_on_demand ? state_.loaded_on_demand->channel_identity
        : (!state_.off_air_return_id.empty() && state_.tuned_channel == 0
               ? kShuffleChannelId : state_.last_channel_id);
    int row = -1;
    for (int c = 0; c < static_cast<int>(channels_->size()); ++c) {
        if (channel_identity(c) == id) { row = c; break; }
    }
    state_.tuned_channel = row;
}

void PlaylistPlayback::play_on_demand(int channel, const OnAir& oa) {
    const auto* ch = channel_at(channel);
    if (!ch || oa.index < 0 || oa.index >= static_cast<int>(ch->items.size())) return;
    const auto& si = ch->items[oa.index];
    if (si.playlist < 1 || si.playlist >= static_cast<int>(state_.playlists.size())) return;
    const auto& pl = state_.playlists[si.playlist];
    if (si.item < 0 || si.item >= static_cast<int>(pl.items.size())) return;
    const auto& item = pl.items[si.item];
    const ChannelAiring selected{channel,
        LoadedAiring{oa.index, si, oa.starts_at, oa.ends_at, std::nullopt},
        channel_identity(channel), playlist_identity(pl), item.path, item.movie_tmdb_id, item.start};
    // A rejected re-entrant press must not replace the pending selection.
    if (switch_pending() || state_.is_switching_playlist || state_.showing_intro_video ||
        transport_.now() < timeout_settle_until_) return;
    tune(channel, false, TuneOrigin::Viewer);
    if (switch_pending()) {
        pending_.selected = selected;
        state_.off_air_return_id.clear();
        state_.off_air_return_after_utc = 0;
    }
}

void PlaylistPlayback::tune(int channel, bool keep_overlay, TuneOrigin origin) {
    const Channel* ch = channel_at(channel);  // nullptr in List style
    if (!ch || ch->items.empty()) return;     // nothing to join
    const auto now = transport_.now();
    if (state_.showing_intro_video || state_.is_loading_game || now < timeout_settle_until_) return;
    const bool turning_dial = origin == TuneOrigin::Dial && !state_.ui_visible_when_playing;
    const bool continuing = turning_dial && state_.channel_ident.active();
    const bool reuse_stop = switch_pending();
    if (reuse_stop && !(continuing && pending_.origin == TuneOrigin::Dial &&
                        !pending_switch_obsolete_reason())) return;
    if (!reuse_stop && state_.is_switching_playlist &&
        !(continuing && state_.channel_ident.loaded)) return;

    state_.channel_ident = {};
    // SELECT's mid-playback switch, with the channel as its target.
    state_.is_switching_playlist = true;
    switch_start_generation_ = transport_.stream_generation();
    state_.playlist_switch_start_time = transport_.now();
    channel_load_at_.reset();  // the tune takes the pipeline over
    state_.loaded_movie.reset();
    state_.loaded_on_demand.reset();
    state_.on_demand = false;
    // Point at the channel's row before the stop, as SELECT does: it keeps
    // the indexes through the stop, and the pending switch's "re-pointed"
    // check covers a tune too. The on-air item replaces it at the load.
    state_.current_playlist_index = channel;
    state_.current_item_index = 0;
    state_.last_advanced_item_index = -1;
    state_.last_advanced_duration = 0.0;

    if (reuse_stop) {
        // The decoder is already stopping. Keep its release deadline and
        // give the last dial position time to settle before loading it.
        pending_.next_check = std::max(pending_.next_check, now + tuner_snow::duration);
    } else {
        transport_.stop();
        pending_.next_check = transport_.now() + kSwitchSettle;
    }
    pending_.phase = SwitchPhase::Settling;
    pending_.target = channel;
    pending_.repolls = 0;
    pending_.tune = true;
    pending_.identity = channel_identity(channel);
    pending_.keep_overlay = keep_overlay;
    pending_.origin = origin;
    if (origin != TuneOrigin::Automatic && !state_.ui_visible_when_playing) {
        state_.channel_ident.channel = channel;
        state_.channel_ident.tuned_at = transport_.now();
    }
}

void PlaylistPlayback::on_channel_step(int delta) {
    if (!guide_style() || delta == 0 || channels_->empty()) return;
    const int n = static_cast<int>(channels_->size());
    const int step = delta > 0 ? 1 : -1;
    const int from = pending_tune().value_or(state_.tuned_channel);
    // From nothing tuned, up starts at 00 and down at the last channel.
    int c = (from >= 0 && from < n) ? from : (step > 0 ? -1 : n);
    for (int i = 0; i < n; ++i) {
        c = ((c + step) % n + n) % n;
        if (c == from) return;  // no other channel has a schedule
        if (channel_on_air(c)) {
            // keep_overlay: the load leaves the overlay alone, so it is as
            // it stands THEN — `tune(c, ui_visible_when_playing)` froze it as
            // it stood at the press, and a guide opened during the ~200 ms
            // wait closed again at the load.
            tune(c, /*keep_overlay=*/true, TuneOrigin::Dial);
            return;
        }
    }
}

void PlaylistPlayback::land_on_channel(int channel) {
    // Whether or not its item then loads: a failed item is the channel's to
    // resume (act_on_failed_item), and a give-up must name this channel,
    // not the one tuned before it.
    state_.tuned_channel = channel;
    if (channel != 0 || state_.off_air_return_id.empty())
        state_.last_channel_id = channel_identity(channel);
    if (channel != 0) {
        state_.off_air_return_id.clear();
        state_.off_air_return_after_utc = 0;
    }
}

bool PlaylistPlayback::finish_tune(int channel, bool keep_overlay, bool reuse_tune_stop) {
    warn_if_still_playing();
    // On air NOW, not at the press: the wait must not put playback behind.
    const Channel* ch = channel_at(channel);
    const double now = transport_.wall_now_utc();
    const auto offset = [this](double utc) { return transport_.utc_offset_s(utc); };
    const auto oa = ch ? join_point(*ch, now, offset(now), offset) : std::nullopt;
    if (!oa) {
        if (park_off_air(channel)) return false;
        // The list was rebuilt during the wait and the channel lost its
        // schedule (or the style went back to List).
        std::cerr << "Tune to channel " << channel << " failed: nothing scheduled"
                  << std::endl;
        if (ch) land_on_channel(channel);  // not past a list that shrank
        app::stop_to_menu(state_);
        return false;
    }
    // Each channel gets its own failure run: failures on the channel being
    // left must not use up this one's budget.
    if (channel != state_.tuned_channel) {
        error_policy_ = video::PlaybackErrorPolicy{};
    }
    // A load that returns OK starts the channel's start bound
    // (kChannelStartTimeout, tick_pipeline_error).
    return load_on_air(channel, *oa, keep_overlay, /*on_demand=*/false, reuse_tune_stop);
}

double PlaylistPlayback::played_position(double player_pos) {
    ChannelProgress& p = channel_progress_;
    if (!p.loaded) return player_pos;  // not a channel load: as before
    if (!p.anchor) {
        // Not up yet: GstPlayer still reports the REQUESTED start here.
        if (!state_.video_active || transport_.has_error()) return 0.0;
        p.anchor = player_pos;
    }
    return player_pos - *p.anchor;
}

void PlaylistPlayback::prove_channel() {
    ChannelProgress& p = channel_progress_;
    if (!p.loaded || p.proven) return;
    p.proven = true;
    if (!state_.guide_gave_up.empty()) {
        std::cout << "Channel " << state_.tuned_channel
                  << " plays — the channels given up on may be tried again" << std::endl;
        state_.guide_gave_up.clear();
    }
    gave_up_identities_.clear();
}

bool PlaylistPlayback::park_off_air(int channel) {
    const auto* ch = channel_at(channel);
    const double now = transport_.wall_now_utc();
    const auto offset = [this](double utc) { return transport_.utc_offset_s(utc); };
    if (!ch || !back_on_air_at(*ch, now, offset(now), offset)) return false;
    park_on_mix(channel_identity(channel));
    return true;
}

bool PlaylistPlayback::park_on_mix(const std::string& return_id, double not_before) {
    if (state_.off_air_return_id.empty()) state_.off_air_return_id = return_id;
    state_.off_air_return_after_utc = not_before;
    const double now = transport_.wall_now_utc();
    const auto offset = [this](double utc) { return transport_.utc_offset_s(utc); };
    const auto* mix = channel_at(0);
    const auto live = mix ? join_point(*mix, now, offset(now), offset) : std::nullopt;
    transport_.stop();
    if (live) {
        if (state_.tuned_channel != 0) error_policy_ = video::PlaybackErrorPolicy{};
        load_on_air(0, *live, /*keep_overlay=*/true);
    } else {
        state_.on_demand = false;
        land_on_channel(0);
        stop_to_menu(state_);
    }
    return live.has_value();
}

void PlaylistPlayback::finish_on_demand() {
    prove_channel();
    state_.on_demand = false;
    const int channel = state_.tuned_channel;
    const auto* ch = channel_at(channel);
    if (park_off_air(channel)) return;
    const double now = transport_.wall_now_utc();
    const auto offset = [this](double utc) { return transport_.utc_offset_s(utc); };
    auto live = ch ? on_air(*ch, now, offset(now), offset) : std::nullopt;
    const auto watched = [&](const ScheduledItem& si) {
        if (!loaded_airing_ || si.playlist < 1 || si.playlist >= static_cast<int>(state_.playlists.size()))
            return false;
        const auto& pl = state_.playlists[si.playlist];
        if (si.item < 0 || si.item >= static_cast<int>(pl.items.size())) return false;
        const auto& item = pl.items[si.item];
        const auto& held = *loaded_airing_;
        return playlist_identity(pl) == held.source_identity && item.path == held.source_path &&
            item.movie_tmdb_id == held.source_tmdb_id && item.start == held.source_start;
    };
    const auto distinct = [&](const Channel& schedule, std::optional<OnAir> candidate) {
        return skip_matching_programmes(schedule, candidate, watched, offset(now), offset);
    };
    const bool watched_live = live && watched(ch->items[live->index]);
    const auto candidate = watched_live ? airing_after(*ch, *live, offset(live->ends_at), offset)
        : (ch ? join_point(*ch, now, offset(now), offset) : std::nullopt);
    const auto next = ch ? distinct(*ch, candidate) : std::nullopt;
    if (next) {
        auto oa = *next;
        if (watched_live) oa.offset_s = 0;
        load_on_air(channel, oa, true);
    } else if (state_.loaded_movie) {
        // Movie sources never enter 00's mix, so its candidate is distinct.
        // A final/sole film parks promptly for the rest of this broadcast
        // day. A removed movie row also gets a playable 00 fallback.
        if (!park_on_mix(state_.loaded_movie->channel_identity,
                         live && (watched_live || candidate) ? live->day_ends_at : 0)) {
            // No playable 00 exists: do not let auto-tune immediately return
            // to the exhausted film while its row is technically on air.
            state_.tuned_channel = channel;
            exclude_failed_channel();
            state_.off_air_return_id.clear();
            state_.off_air_return_after_utc = 0;
        }
    } else {
        // Exhaustion is not a decoder failure, but it uses the same stable
        // exclusion so the nothing-playing rule cannot immediately replay.
        exclude_failed_channel();
        const auto* mix = channel_at(0);
        const auto fallback = mix ? distinct(*mix, join_point(*mix, now, offset(now), offset)) : std::nullopt;
        if (fallback) {
            load_on_air(0, *fallback, true);
        } else {
            if (mix) {
                state_.tuned_channel = 0;
                exclude_failed_channel();
            }
            transport_.stop();
            stop_to_menu(state_);
        }
    }
}

void PlaylistPlayback::rejoin_schedule(bool programme_ended) {
    if (programme_ended) prove_channel();  // the item played to its natural end
    const int channel = state_.tuned_channel;
    if (park_off_air(channel)) return;
    const Channel* ch = channel_at(channel);
    // Never back to the airing that just ended (playback can be ahead of
    // the schedule here — boundary_join): only an airing loaded on THIS
    // channel counts.
    std::optional<LoadedAiring> last;
    if (programme_ended && loaded_airing_ && loaded_airing_->channel == channel) last = loaded_airing_->airing;
    const double now = transport_.wall_now_utc();
    const auto offset = [this](double utc) { return transport_.utc_offset_s(utc); };
    const auto oa = ch ? boundary_join(*ch, now, last, offset(now), offset) : std::nullopt;
    if (!oa) {
        // The EOF tolerance may fire a fraction before the civil close.
        // Keep the final picture until that deadline instead of exposing
        // a nothing-playing frame that could forget the movie identity.
        const auto ending = ch ? on_air(*ch, now, offset(now), offset) : std::nullopt;
        if (ending && ending->day_ends_at > now &&
            ending->day_ends_at - now <= 0.5 &&
            back_on_air_at(*ch, ending->day_ends_at, offset(ending->day_ends_at), offset)) {
            guide_day_ends_at_ = ending->day_ends_at;
            return;
        }
        // Not on a channel (a List-style item still playing when the style
        // changed) or the channel lost its schedule: end here, and the
        // nothing-playing rule tunes live from the menu.
        std::cout << "Item ended with no channel schedule to rejoin — stopping" << std::endl;
        transport_.stop();
        app::stop_to_menu(state_);
        return;
    }
    // No switch flag here: the load's own start bound is all that watches
    // it (kChannelStartTimeout, tick_pipeline_error).
    load_on_air(channel, *oa, /*keep_overlay=*/true);
}

bool PlaylistPlayback::load_on_air(int channel, const OnAir& oa, bool keep_overlay, bool on_demand, bool reuse_tune_stop) {
    state_.channel_ident = {};
    state_.on_demand = on_demand;
    timezone_reload_pending_ = false;  // this load uses the current zone rules
    land_on_channel(channel);
    channel_progress_ = {};
    const ScheduledItem& si = (*channels_)[channel].items[oa.index];
    if (si.playlist < 1 || si.playlist >= static_cast<int>(state_.playlists.size()) ||
        si.item < 0 ||
        si.item >= static_cast<int>(state_.playlists[si.playlist].items.size())) {
        // A channel list older than the playlists it points into.
        std::cerr << "Channel " << channel << " schedule is stale — stopping" << std::endl;
        transport_.stop();
        app::stop_to_menu(state_);
        // Not a failed item (nothing was tried), but the nothing-playing
        // rule would re-tune it at once: a missed rebuild would be a stop
        // and a tune every ~200 ms. Hold it off instead.
        state_.guide_wait_until_utc = transport_.wall_now_utc() + kStaleScheduleWaitS;
        return false;
    }
    const Playlist& pl = state_.playlists[si.playlist];
    state_.master_shuffle_active = false;
    state_.current_playlist_index = si.playlist;
    state_.current_item_index = si.item;
    guide_item_ends_at_ = oa.ends_at;
    guide_item_length_ = si.length_s;
    guide_item_end_position_ = on_demand ? 0 : pl.items[si.item].start + (oa.ends_at - oa.starts_at);
    guide_day_ends_at_ = on_demand ? 0 : oa.day_ends_at;
    LoadedAiring airing;
    airing.index = oa.index;
    airing.item = si;
    airing.starts_at = oa.starts_at;
    airing.ends_at = oa.ends_at;
    airing.utc_offset_s = transport_.utc_offset_s(transport_.wall_now_utc());
    loaded_airing_ = ChannelAiring{channel, airing, channel_identity(channel),
        playlist_identity(pl), pl.items[si.item].path, pl.items[si.item].movie_tmdb_id,
        pl.items[si.item].start};

    std::cout << "Channel " << channel << ": joining " << pl.title << " item " << si.item
              << " at " << oa.offset_s << "s" << std::endl;
    state_.loaded_movie.reset();
    if (is_movie_playlist(pl))
        state_.loaded_movie = AppState::LoadedMovie{channel_identity(channel), pl.items[si.item]};
    state_.loaded_on_demand.reset();
    if (on_demand && !is_movie_playlist(pl))
        state_.loaded_on_demand = AppState::LoadedOnDemand{channel_identity(channel), playlist_identity(pl), pl.items[si.item]};
    const auto selected = state_.loaded_on_demand;
    const auto movie = state_.loaded_movie;
    auto result = transport_.load_playlist_item_at(state_, pl, si.item,
                                                   playlist_directory_, oa.offset_s, reuse_tune_stop);
    // Controller clears the retained item when starting any new load. Both
    // success and synchronous failure still belong to this attempted programme.
    state_.loaded_movie = movie;
    state_.loaded_on_demand = selected;
    state_.on_demand = on_demand;
    if (!result) {
        std::cerr << "Failed to load channel item: " << result.error() << std::endl;
        act_on_failed_item(error_policy_.on_load_failure(failure_budget()), "load failed");
        return false;
    }
    // The start bound counts from HERE, the load's return (kChannelStartTimeout)
    // — or, while the pipeline has not carried the load out yet, from when
    // it does (hold_load_deadlines_while_busy).
    channel_load_at_ = transport_.now();
    // Loaded is not playing: guide_gave_up waits for prove_channel().
    channel_progress_.loaded = true;
    // As load_next_item does: Controller::update_state confirms the NEW
    // stream (position < its end). Until then auto-advance holds — the
    // state can still read the previous item's end for a frame (update_state
    // runs every other frame), which would load this boundary twice — and
    // the start bound (tick_pipeline_error) waits for it.
    state_.playback_started_ = false;
    state_.guide_wait_until_utc = 0.0;
    if (!keep_overlay && !state_.guide_opened_by_viewer) {
        // A tune closes the guide; channel up/down leaves it as it is. Never
        // one the viewer owns NOW (any input to it since it appeared — e.g.
        // SELECT during the power-on re-tune's settle): the guide never
        // closes by itself. The viewer's own Tune press drops that
        // ownership first (main.cpp), so it still closes the guide.
        state_.ui_visible_when_playing = false;
    }
    return true;
}

// ── Per-frame ticks ──────────────────────────────────────────────────────

const char* PlaylistPlayback::pending_switch_obsolete_reason() const {
    // Something else took the pipeline while the switch waited. In the old
    // blocking code none of these could happen before the load; now they
    // can, and loading a playlist video into them would be wrong.
    if (!state_.is_switching_playlist) {
        // Media Browser entry (reset_main_ui_for_media_browser) or a
        // stop_to_menu: whoever cleared the flag owns the state now.
        return "switch flag cleared";
    }
    if (state_.current_playlist_index != pending_.target) {
        // A game session returned (prepare_kiosk_state_after_game resets
        // the indexes), or something else re-pointed playback.
        return "playback re-pointed";
    }
    if (state_.is_loading_game) return "game launching";
    if (state_.showing_intro_video) return "intro playing";
#ifdef MEDIA_BROWSER_ENABLED
    if (state_.current_screen == app::AppScreen::MediaBrowser) {
        return "Media Browser open";
    }
#endif
    return nullptr;
}

void PlaylistPlayback::advance_pending_switch() {
    if (!switch_pending()) return;

    if (const char* why = pending_switch_obsolete_reason()) {
        std::cerr << "Playlist switch to row " << pending_.target
                  << " abandoned before its load (" << why << ")" << std::endl;
        pending_ = PendingSwitch{};
        state_.channel_ident = {};
        return;
    }

    const auto now = transport_.now();
    if (now < pending_.next_check) return;  // still settling / between polls

    // The old loop, one iteration per due tick:
    //   while (is_playing() && retry_count < max_retries) { sleep 50; ++retry; }
    // Same queries in the same order, so the same poll count and outcome.
    const bool still_playing = transport_.is_playing();
    if (still_playing && pending_.repolls < kSwitchMaxRepolls) {
        pending_.phase = SwitchPhase::Polling;
        ++pending_.repolls;
        pending_.next_check = now + kSwitchRepoll;
        return;
    }

    const PendingSwitch done = pending_;
    pending_ = PendingSwitch{};
    if (done.tune) {
        const auto tuning = state_.channel_ident;
        // A fullscreen dial turn already stopped the pipeline and waited the
        // release interval. A timed-out stop still needs normal load handling.
        const bool reuse_tune_stop = tuning.active() && !state_.ui_visible_when_playing && !still_playing;
        bool loaded;
        if (done.selected) {
            if (done.target != state_.tuned_channel) error_policy_ = video::PlaybackErrorPolicy{};
            const auto& a = done.selected->airing;
            loaded = load_on_air(done.target, OnAir{a.index, 0, a.starts_at, a.ends_at, 0},
                                 done.keep_overlay, /*on_demand=*/true, reuse_tune_stop);
        } else {
            loaded = finish_tune(done.target, done.keep_overlay, reuse_tune_stop);
        }
        if (loaded && done.origin != TuneOrigin::Automatic && !state_.ui_visible_when_playing) {
            // load_on_air clears the previous ident on every load. Carry the
            // accepted dial turn across this load, including its snow clock.
            state_.channel_ident.channel = done.target;
            state_.channel_ident.tuned_at = tuning.active() ? tuning.tuned_at : transport_.now();
            state_.channel_ident.loaded = true;
        } else {
            state_.channel_ident = {};
        }
    } else {
        finish_switch(done.target);
    }
}

// The end of a switch's bounded wait: a player still playing after it is
// logged, and the load goes ahead anyway.
void PlaylistPlayback::warn_if_still_playing() const {
    if (transport_.is_playing()) {
        std::cerr << "Warning: Video did not stop cleanly after "
                  << (kSwitchMaxRepolls * kSwitchRepoll.count())
                  << "ms, proceeding anyway" << std::endl;
    }
}

void PlaylistPlayback::finish_switch(int target) {
    warn_if_still_playing();
    channel_progress_ = {};  // not a channel load (see ChannelProgress)

    // Then start the new playlist
    bool load_success = false;

    // Check for Master Shuffle (index 0)
    if (target == 0) {
        std::cout << "Master Shuffle selected!" << std::endl;
        state_.master_shuffle_active = true;
        transport_.play_random_global_video(state_, playlist_directory_);
        load_success = true; // Assume success for now (play_random_global_video handles retries)

        // When starting video, hide UI completely so video shows through fully
        state_.ui_visible_when_playing = false;
    } else if (!state_.playlists.empty() && target < static_cast<int>(state_.playlists.size())) {
        state_.master_shuffle_active = false; // Disable master shuffle for normal playlists
        const auto& pl = state_.playlists[target];
        if (!pl.items.empty() && pl.is_video_playlist()) {
            // Load first item of new playlist
            auto load_result = transport_.load_playlist_item(state_, pl, 0, playlist_directory_);
            load_success = static_cast<bool>(load_result);
            if (!load_result) {
                std::cerr << "Failed to load playlist item: " << load_result.error() << std::endl;
                state_.set_error("Could not load: " + pl.title);
            }
            if (load_success) {
                // Only hide UI when video actually loaded
                state_.ui_visible_when_playing = false;
            }
        } else if (!pl.items.empty() && pl.is_game_playlist()) {
            // Game playlists are launched from Settings > Video Games
            state_.set_error("Use Settings to launch games");
            load_success = false;
        } else {
            state_.set_error("No content in playlist");
            load_success = false;
        }
    }

    // If load failed, the old video is already stopped:
    // land on the menu. stop_to_menu, not the three flags
    // this used to clear — current_playlist_index /
    // current_item_index were set to the NEW playlist above,
    // and indexes set with no video is the Renderer's
    // "between items" early-out, so the menu AND the error
    // banner just raised stayed blank until another press.
    // (An empty message leaves that banner in place.)
    if (!load_success) {
        app::stop_to_menu(state_);
        std::cerr << "Playlist switch failed - flag cleared, ready for retry" << std::endl;
    }
    // Otherwise, the flag will be cleared when the new video becomes active
    // If video doesn't become active within timeout, flag will be cleared by timeout mechanism
}

void PlaylistPlayback::tick_switch_timeout() {
    // A pending switch's load runs here, BEFORE the timeout check — the
    // old order (load inside on_select, timeout check later in the same
    // iteration). While it is still waiting, the timeout stands down: that
    // wait is bounded on its own (200 ms + 10 x 50 ms), and a slow frame
    // must not cut a switch short that the old code would have completed.
    advance_pending_switch();
    if (switch_pending()) return;
    hold_load_deadlines_while_busy();
    drop_settled_channel_load();

    // Completion. Controller::update_state clears the flag only when
    // video_active goes false -> true, and during a switch it holds
    // video_active true — so a switch made WHILE a video plays never saw
    // that edge: every one ended in the 2 s "CRITICAL" timeout below, with
    // NEXT/PREV ignored until then (seen on hardware 2026-10-05). A NEW
    // stream (generation bumped by load_file) that is playing with a known
    // duration is the switch's item up and running.
    if (state_.is_switching_playlist &&
        transport_.stream_generation() != switch_start_generation_ &&
        state_.get_duration() > 0.0 && transport_.is_playing()) {
        std::cout << "Playlist switch completed - new stream playing, clearing flag" << std::endl;
        state_.is_switching_playlist = false;
        return;
    }

    // Clear playlist switching flag if it's been stuck for too long (timeout safety)
    // This prevents the flag from getting stuck if video fails to load or gets into bad state
    if (state_.is_switching_playlist) {
        auto now = transport_.now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state_.playlist_switch_start_time);
        if (elapsed > kSwitchTimeout && channel_load_at_) {
            // Channel Guide: a tune whose load returned OK and whose stream
            // is not up yet (still within its start bound — a stalled card
            // can take seconds). The switch lets go, as a List switch does,
            // so NEXT/PREV and tunes work again; but nothing is stopped and
            // nothing is counted: kChannelStartTimeout from the load's
            // return decides that (tick_pipeline_error). Until then the
            // playing indexes stay set, which holds the nothing-playing
            // rule off.
            std::cerr << "Channel " << state_.tuned_channel << ": stream not up "
                      << elapsed.count() << "ms after the tune — waiting up to "
                      << kChannelStartTimeout.count() / 1000 << " s from its load"
                      << std::endl;
            state_.is_switching_playlist = false;
            return;
        }
        if (elapsed > kSwitchTimeout) {  // 2 second timeout
            std::cerr << "CRITICAL: Playlist switch timeout after " << elapsed.count() << "ms - clearing flag and resetting state" << std::endl;
            std::cerr << "  Debug info: video_active=" << state_.video_active
                      << ", is_playing=" << transport_.is_playing()
                      << ", current_playlist=" << state_.current_playlist_index
                      << ", current_item=" << state_.current_item_index << std::endl;
            state_.is_switching_playlist = false;

            // Also reset video state to ensure clean recovery
            if (!transport_.is_playing() && !state_.video_active) {
                std::cerr << "MPV appears stuck - attempting recovery by stopping and clearing state" << std::endl;
                transport_.stop();
                // Was a 200 ms sleep on the render thread; now a window in
                // which SELECT cannot start a load (see on_select).
                timeout_settle_until_ = now + kTimeoutSettle;
            }
        }
    }
}

void PlaylistPlayback::tick_pipeline_error() {
    // Pipeline error on a playlist item -> skip it (or give up after a
    // run of failures). Not gated on video_active: an error before
    // preroll leaves duration 0, so video_active never turned on.
    //
    // Controller::load_next_item hands over a run of items that would not
    // load (its synchronous retries are bounded — see kMaxSyncLoadRetries)
    // by raising playlist_item_load_failed; it is consumed every frame, so
    // a hand-off that something else overtook (Media Browser entry, a
    // playlist switch) is dropped rather than acted on later.
    const bool load_failed = std::exchange(state_.playlist_item_load_failed, false);
    hold_load_deadlines_while_busy();
    // A channel load's start bound ends when its stream comes up, or when
    // anything else takes the pipeline (a tune, Media Browser entry, a game,
    // a stop) — whoever took it owns what happens to it.
    drop_settled_channel_load();
    if (owns_pipeline() && load_failed) {
        act_on_failed_item(error_policy_.on_load_failure(failure_budget()),
                           "load failed");
        return;
    }
    if (owns_pipeline()) {
        // On a channel, progress counts from where the playing stream
        // started (ChannelProgress); otherwise this is the player's position.
        const double played = played_position(transport_.player_position());
        const auto decision = error_policy_.on_frame(
            transport_.stream_generation(), transport_.has_error(),
            played, failure_budget());
        if (decision != video::PlaybackErrorPolicy::Decision::None) {
            act_on_failed_item(decision, "pipeline error");
        } else if (channel_load_at_ &&
                   transport_.now() - *channel_load_at_ > kChannelStartTimeout) {
            // Channel Guide: a tune's or a boundary's load returned OK but
            // its stream never came up and never errored. The stall watchdog
            // needs video_active, so nothing else would ever notice: the
            // indexes stay set and the screen stays dark. A failed item for
            // the channel — its budget, its wait, its give-up; the failure
            // path stops the pipeline — so the nothing-playing rule cannot
            // re-tune a dead channel forever. (act_on_failed_item drops the
            // bound.) A tune's switch flag was released at 2 s, so
            // owns_pipeline() holds here.
            act_on_failed_item(error_policy_.on_load_failure(failure_budget()),
                               "never started");
        } else if (channel_progress_.loaded && !channel_progress_.proven &&
                   played >= kChannelPlaysS && !transport_.has_error()) {
            prove_channel();
        }
    }
}

void PlaylistPlayback::tick_auto_advance() {
    if (!state_.on_demand && timezone_reload_pending_ && guide_style() && loaded_airing_ && owns_pipeline()) {
        timezone_reload_pending_ = false;
        rejoin_schedule(/*programme_ended=*/false);
        return;
    }
    const bool owns_cut = !state_.on_demand && guide_style() && loaded_airing_ &&
        !loaded_airing_->identity.empty() &&
        (loaded_airing_->identity == channel_identity(state_.tuned_channel) ||
         (state_.loaded_movie && loaded_airing_->identity == state_.loaded_movie->channel_identity));
    // Live programmes cut at the civil day boundary even if a keyframe seek
    // or pause has left the file position behind. A load records a new day
    // deadline and clears playback_started_, so this cannot fire twice.
    // Whole selected programmes bypass both this deadline and the file cut.
    if (owns_cut && owns_pipeline() && state_.video_active &&
        state_.playback_started_ &&
        guide_day_ends_at_ > 0 && transport_.wall_now_utc() >= guide_day_ends_at_) {
        rejoin_schedule();
        return;
    }
    // Auto-advance to next item in playlist when current video ends.
    // "Ends" honors the item's `end:` trim (decide_auto_advance) — the
    // old full-file comparison played every trimmed item to EOF.
    //
    // `holding`: this frame is a logged hold. Any other frame re-arms the
    // hold log, so the next hold (a later item, or this one again after a
    // seek back) is logged once more.
    bool holding = false;
    // A pending switch has stopped the old stream and already points the
    // indexes at the new playlist: there is no item to advance (and the old
    // code never ran this inside the switch's wait).
    if (!switch_pending() && state_.video_active && state_.current_playlist_item()) {
        // Snapshot the (position, duration) pair so the whole advance
        // decision sees a consistent view rather than reading the
        // mutex-protected fields multiple times.
        app::AutoAdvanceInput adv;
        adv.position = state_.get_position();
        adv.duration = state_.get_duration();
        adv.item_end = state_.current_playlist_item()->end;
        if (owns_cut && guide_item_end_position_ > 0) {
            adv.item_end = adv.item_end > 0 ? std::min(adv.item_end, guide_item_end_position_)
                                          : guide_item_end_position_;
        }
        adv.playback_started = state_.playback_started_;
        adv.master_shuffle = state_.master_shuffle_active;
        adv.current_item = state_.retained_playlist_item() ? 0 : state_.current_item_index;
        adv.last_advanced_item = state_.last_advanced_item_index;

        switch (app::decide_auto_advance(adv)) {
            case app::AutoAdvance::Advance:
                std::cout << "Auto-advancing from item " << state_.current_item_index
                          << " at position " << adv.position << "/"
                          << app::effective_end(adv.duration, adv.item_end) << std::endl;
                // Set flag BEFORE calling load_next_item to prevent race
                // conditions. The REAL duration: update_state compares it
                // to the new file's duration to detect the next item loaded.
                state_.last_advanced_item_index = state_.current_item_index;
                state_.last_advanced_duration = adv.duration;
                // Note: load_next_item handles errors internally (skips broken files)
                if (guide_style()) {
                    // Live: whatever is on the tuned channel now, at its
                    // scheduled offset — drift is corrected here.
                    if (state_.on_demand) finish_on_demand();
                    else rejoin_schedule();
                } else if (state_.master_shuffle_active) {
                    // Saves the current item to the shuffle history first
                    // (virtual row excluded — see on_next()).
                    transport_.master_shuffle_advance(state_, playlist_directory_);
                } else {
                    transport_.load_next_item(state_, playlist_directory_);
                }
                break;
            case app::AutoAdvance::Held:
                if (!state_.master_shuffle_active) {
                    // Held lasts for every frame between reaching the end
                    // and the next item's load landing. This line used to
                    // print once per FRAME (dozens of journald writes a
                    // second to the SD card); now it prints on entering the
                    // hold and whenever the reason changes.
                    const HeldReason reason{state_.current_item_index,
                                            state_.last_advanced_item_index,
                                            static_cast<bool>(state_.playback_started_)};
                    if (!held_logged_ || !(reason == held_reason_)) {
                        std::cout << "NOT auto-advancing: item=" << reason.item
                                  << ", last_advanced=" << reason.last_advanced
                                  << ", playback_started=" << reason.playback_started
                                  << std::endl;
                        held_reason_ = reason;
                    }
                    holding = true;
                }
                break;
            case app::AutoAdvance::ResetGuard:
                // Well away from the end: re-arm the once-per-item guard,
                // so auto-advance works even after manual navigation.
                // Master Shuffle stays active - only exits when user selects a different playlist
                state_.last_advanced_item_index = -1;
                state_.last_advanced_duration = 0.0;
                break;
            case app::AutoAdvance::None:
                break;
        }
    }
    held_logged_ = holding;
}

void PlaylistPlayback::tick_stall_watchdog(double now_sec) {
    // Catches the pipeline silently stalling while the kiosk still
    // believes it is playing. Seen live 2026-07-29: a playlist-switch
    // timeout restored the "playing" flags but left GStreamer PAUSED
    // (its PulseAudio stream read `Corked: yes` against the kiosk's
    // `is_paused = false`), position sat at 0.00 for seven hours, and
    // nothing detected it. Logic and thresholds live in
    // app::PlaybackStallWatchdog — see tests/app/test_playback_stall.cpp.
    //
    // A new stream (every load_file bumps the generation) starts at
    // 0.0, which must not read as "frozen at 0.0" from the previous
    // item's baseline — and its escalation count starts fresh.
    if (transport_.stream_generation() != watchdog_generation_) {
        watchdog_generation_ = transport_.stream_generation();
        stall_watchdog_.reset();
    }
    // Not while the pipeline's control thread has a command in hand: the
    // player does not query a pipeline it is changing, so the position reads
    // frozen by construction (a seek waiting on the card is not a stall).
    const bool expect_playing =
        state_.video_active && transport_.is_playing() &&
        !transport_.is_paused() && !state_.is_loading_game &&
        !state_.showing_intro_video && !state_.is_switching_playlist &&
        !transport_.pipeline_busy();
    const auto wd_action = stall_watchdog_.update(
        expect_playing, state_.get_position(), now_sec);
    if (wd_action == app::PlaybackStallWatchdog::Action::None) return;
    // What the pipeline itself says (zero-timeout state read): the frozen
    // picture seen on a Pi 5 reported playing throughout.
    const std::string pipeline = transport_.pipeline_state();
    const std::string pipeline_note = pipeline.empty() ? "" : " (pipeline " + pipeline + ")";
    if (wd_action == app::PlaybackStallWatchdog::Action::Recover && stall_is_channels()) {
        on_channel_stall(pipeline_note);
    } else if (wd_action == app::PlaybackStallWatchdog::Action::Advance &&
               owns_pipeline()) {
        // Repeated play() never got position moving: skip the item,
        // counted in the same failure run as pipeline errors.
        std::cerr << "Playback still stalled at "
                  << state_.get_position() << "s after "
                  << app::PlaybackStallWatchdog::kMaxRecoveriesBeforeAdvance
                  << " restarts" << pipeline_note << " — giving up on this item" << std::endl;
        act_on_failed_item(
            error_policy_.on_failure(
                transport_.stream_generation(), failure_budget()),
            "stalled");
    } else {
        // Recover — or Advance where there is no playlist to
        // advance (Media Browser playback): one more restart.
        std::cerr << "Playback stalled at " << state_.get_position()
                  << "s while reported playing" << pipeline_note
                  << " — restarting playback" << std::endl;
        transport_.play();
    }
}

bool PlaylistPlayback::stall_is_channels() const {
    const Channel* ch = channel_at(state_.tuned_channel);
    return !state_.on_demand && ch && !ch->items.empty() && channel_progress_.loaded && owns_pipeline();
}

void PlaylistPlayback::on_channel_stall(const std::string& pipeline_note) {
    const int channel = state_.tuned_channel;
    const bool airing_known = loaded_airing_ && loaded_airing_->channel == channel;
    if (airing_known && stall_reloaded_ && stall_reloaded_->channel == channel &&
        same_airing(stall_reloaded_->airing, loaded_airing_->airing)) {
        // This airing had its free reload: a failed item, like any other —
        // the channel waits for the slot's end, or is given up. Bounded the
        // way the List's kMaxRecoveriesBeforeAdvance bounds play().
        std::cerr << "Channel " << channel << " stalled again at "
                  << state_.get_position() << "s on the airing it was reloaded for"
                  << pipeline_note << " — counting a failed item" << std::endl;
        act_on_failed_item(
            error_policy_.on_failure(transport_.stream_generation(), failure_budget()),
            "stalled");
        return;
    }
    const double now = transport_.wall_now_utc();
    const auto offset = [this](double utc) { return transport_.utc_offset_s(utc); };
    const auto oa = join_point(*channel_at(channel), now, offset(now), offset);
    if (!oa) return;  // off air: the live day-boundary tick handles stopping
    std::cerr << "Playback stalled at " << state_.get_position()
              << "s while reported playing" << pipeline_note << " — reloading channel "
              << channel << " at its live point" << std::endl;
    // The airing that froze has had its free reload (a stall reload from its
    // last ~2 s joins the next airing, which keeps one of its own).
    if (airing_known) stall_reloaded_ = loaded_airing_;
    // A fresh join, as a tune's is: the once-per-item guard re-armed.
    state_.last_advanced_item_index = -1;
    state_.last_advanced_duration = 0.0;
    // The boundary's load (no stop/settle of its own: load_item_media stops
    // and settles the old pipeline itself), so the start bound watches it
    // (kChannelStartTimeout) and the overlay stays as it is.
    load_on_air(channel, *oa, /*keep_overlay=*/true);
}

}  // namespace app
