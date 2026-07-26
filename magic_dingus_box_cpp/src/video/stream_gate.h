#pragma once

#include <cstdint>

namespace video {

// Decides whether GstRenderer may draw its current textures this tick.
//
// The renderer's GL textures always hold the last uploaded frame, and
// render() draws them even when no new sample is pending — that is how
// a 24/30fps video repeats frames on a 60Hz loop. But the same behavior
// across a STREAM BOUNDARY paints the previous video's last frame while
// the new pipeline is still decoding its first one (observed on
// hardware: the boot intro's final frame flashing before a Media
// Browser movie starts).
//
// GstPlayer bumps a generation counter on every load_file(); the gate
// suppresses drawing from the moment the generation changes until the
// new stream's first sample is uploaded. Pure logic so it can be unit
// tested off-Pi; GstRenderer owns one and consults it in render().
struct StreamGate {
    // Call once per render tick, before drawing. Returns true when the
    // textures may be drawn (fresh frame, or a repeat frame of the SAME
    // stream), false when drawing must be suppressed (paint black).
    bool on_render_tick(uint64_t current_generation, bool got_new_sample) {
        if (current_generation != last_seen_generation_) {
            last_seen_generation_ = current_generation;
            awaiting_first_frame_ = true;
        }
        if (got_new_sample) {
            awaiting_first_frame_ = false;
        }
        return !awaiting_first_frame_;
    }

    // After an external GL context takeover (RetroArch), the recreated
    // textures hold undefined contents even though the generation is
    // unchanged — suppress until the next real frame.
    void on_gl_reset() { awaiting_first_frame_ = true; }

private:
    // Starts at "awaiting" with generation 0 meaning "nothing has ever
    // loaded": textures that were never uploaded must not be drawn.
    uint64_t last_seen_generation_ = 0;
    bool awaiting_first_frame_ = true;
};

} // namespace video
