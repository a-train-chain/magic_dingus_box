#pragma once
#include <optional>
namespace app {
class MovieChannelQuiet {
public:
    static constexpr double kReleaseGraceS = 120.0;
    enum class Cmd { None, Claim, Release };
    Cmd tick(bool movie_playing, double now_s) {
        if (movie_playing) {
            stopped_at_.reset();
            if (!claimed_) { claimed_ = true; return Cmd::Claim; }
        } else if (claimed_) {
            if (!stopped_at_) stopped_at_ = now_s;
            if (now_s - *stopped_at_ >= kReleaseGraceS) {
                claimed_ = false; stopped_at_.reset(); return Cmd::Release;
            }
        }
        return Cmd::None;
    }
    bool claimed() const { return claimed_; }
private:
    bool claimed_ = false;
    std::optional<double> stopped_at_;
};
}
