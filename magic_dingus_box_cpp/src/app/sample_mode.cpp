#include "sample_mode.h"

namespace app {

SampleMode::SampleMode()
    : active_(false)
{
}

void SampleMode::enter() {
    active_ = true;
    markers_.clear();
}

void SampleMode::exit() {
    active_ = false;
    markers_.clear();
}

void SampleMode::add_marker(double timestamp) {
    markers_.push_back(timestamp);
}

void SampleMode::undo_marker() {
    if (!markers_.empty()) {
        markers_.pop_back();
    }
}

void SampleMode::update_state(AppState& state) {
    state.sample_mode_active = active_;
    state.markers = markers_;
}

} // namespace app

