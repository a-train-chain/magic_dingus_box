#pragma once

// At most one log line per source per interval, for GstPlayer's GStreamer
// WARNING messages: a misbehaving element (a decoder that warns on every
// frame) must not flood the journal — the SD card the render loop's
// watchdog is sensitive to — while the first warning of each element stays
// visible, so the next frozen picture can be diagnosed. The number of lines
// a source was refused comes back with its next admitted one.
//
// Bounded: at most `max_sources` sources are remembered; a new one evicts
// the source quiet the longest. Pure logic with an injected clock, used on
// the render thread only — tests/video/test_log_rate_limit.cpp.

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace video {

class PerSourceLogLimit {
public:
    using Clock = std::chrono::steady_clock;

    struct Verdict {
        bool log = false;    // write this line
        int suppressed = 0;  // lines refused since this source's last one
    };

    explicit PerSourceLogLimit(std::chrono::milliseconds interval,
                               std::size_t max_sources = 32)
        : interval_(interval), max_sources_(max_sources < 1 ? 1 : max_sources) {}

    Verdict admit(const std::string& source, Clock::time_point now) {
        for (Entry& e : entries_) {
            if (e.source != source) continue;
            if (now - e.last < interval_) {
                ++e.suppressed;
                return {};
            }
            Verdict v{true, e.suppressed};
            e.last = now;
            e.suppressed = 0;
            return v;
        }
        if (entries_.size() >= max_sources_) {
            std::size_t oldest = 0;
            for (std::size_t i = 1; i < entries_.size(); ++i) {
                if (entries_[i].last < entries_[oldest].last) oldest = i;
            }
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(oldest));
        }
        entries_.push_back(Entry{source, now, 0});
        return Verdict{true, 0};
    }

    // A new stream: every source may log its first warning again.
    void clear() { entries_.clear(); }
    std::size_t sources() const { return entries_.size(); }

private:
    struct Entry {
        std::string source;
        Clock::time_point last;
        int suppressed = 0;
    };
    std::chrono::milliseconds interval_;
    std::size_t max_sources_;
    std::vector<Entry> entries_;
};

}  // namespace video
