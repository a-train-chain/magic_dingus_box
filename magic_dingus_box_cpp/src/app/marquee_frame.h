#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace app {

struct MarqueeFrame {
    const char* id;
    const char* name;
    std::string file;
};

inline const std::array<MarqueeFrame, 4> kMarqueeFrames{{
    {"simple_walnut", "Simple Walnut", "simple_walnut.png"},
    {"shaker_cherry", "Shaker Cherry", "shaker_cherry.png"},
    {"pegged_oak", "Pegged Oak", "pegged_oak.png"},
    {"walnut_brass", "Walnut & Brass", "walnut_brass.png"},
}};

inline std::size_t marquee_frame_index(std::string_view id) {
    for (std::size_t i = 0; i < kMarqueeFrames.size(); ++i) {
        if (id == kMarqueeFrames[i].id) return i;
    }
    return 0;
}

} // namespace app
