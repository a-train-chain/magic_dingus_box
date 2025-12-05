#pragma once

#include "app_state.h"
#include <vector>

namespace app {

class SampleMode {
public:
    SampleMode();
    
    void enter();
    void exit();
    bool is_active() const { return active_; }
    
    void add_marker(double timestamp);
    void undo_marker();
    std::vector<double> get_markers() const { return markers_; }
    
    void update_state(AppState& state);

private:
    bool active_;
    std::vector<double> markers_;
};

} // namespace app

