#include "sink_suspension.h"

#include <cstddef>
#include <iostream>
#include <sstream>
#include <utility>

namespace retroarch {

namespace {

std::vector<std::string> list_sinks(const PactlRunner& run, bool* listed) {
    const auto r = run({"pactl", "list", "short", "sinks"});
    *listed = r.ok();
    if (!r.ok()) return {};
    return parse_sink_names(r.out);
}

// argv all the way down: the sink name comes from pactl's own output and is
// never seen by a shell.
utils::subprocess::Result set_suspended(const PactlRunner& run, const std::string& sink,
                                        bool suspended) {
    return run({"pactl", "suspend-sink", sink, suspended ? "1" : "0"});
}

void record(SinkReport& report, const std::string& sink,
            const utils::subprocess::Result& r) {
    if (r.timed_out) {
        report.timed_out.push_back(sink);
    } else if (r.exit_code == 0) {
        report.done.push_back(sink);
    } else {
        report.failed.push_back(sink);
    }
}

// Every sink in `sinks` gets its resume, whatever happened to the others.
SinkReport resume_each(const PactlRunner& run, const std::vector<std::string>& sinks) {
    SinkReport report;
    for (const auto& sink : sinks) record(report, sink, set_suspended(run, sink, false));
    return report;
}

void append_list(std::ostringstream& os, const char* label,
                 const std::vector<std::string>& sinks) {
    if (sinks.empty()) return;
    os << "; " << label << ": ";
    for (size_t i = 0; i < sinks.size(); ++i) os << (i ? ", " : "") << sinks[i];
}

}  // namespace

std::vector<std::string> parse_sink_names(const std::string& pactl_short_sinks) {
    std::vector<std::string> names;
    std::istringstream iss(pactl_short_sinks);
    std::string line;
    while (std::getline(iss, line)) {
        size_t i = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
        if (i == 0 || i >= line.size() || line[i] != '\t') continue;
        const size_t start = i + 1;
        const size_t end = line.find('\t', start);
        std::string name = line.substr(start, end == std::string::npos ? std::string::npos
                                                                       : end - start);
        if (!name.empty()) names.push_back(std::move(name));
    }
    return names;
}

std::string describe_sink_report(const std::string& action, const SinkReport& report) {
    std::ostringstream os;
    os << action << " " << report.done.size() << " PulseAudio sink(s)";
    if (!report.listed) {
        os << " (pactl list short sinks did not answer)";
        return os.str();
    }
    if (!report.done.empty()) {
        os << ": ";
        for (size_t i = 0; i < report.done.size(); ++i) {
            os << (i ? ", " : "") << report.done[i];
        }
    }
    append_list(os, "FAILED", report.failed);
    append_list(os, "TIMED OUT", report.timed_out);
    append_list(os, "not tried", report.abandoned);
    return os.str();
}

SinkSuspension::SinkSuspension(PactlRunner run) : run_(std::move(run)) {}

SinkSuspension::~SinkSuspension() {
    if (held_.empty()) return;
    // Reached without an explicit resume(): an early return or an exception
    // between the suspend and the end of the session.
    const SinkReport r = resume();
    std::cout << "[audio] session guard " << describe_sink_report("resumed", r)
              << std::endl;
}

SinkReport SinkSuspension::suspend() {
    SinkReport report;
    if (suspend_attempted_ || !run_) return report;
    suspend_attempted_ = true;

    const auto sinks = list_sinks(run_, &report.listed);
    for (size_t i = 0; i < sinks.size(); ++i) {
        // Held BEFORE the call: a timed-out suspend may still have landed.
        held_.push_back(sinks[i]);
        const auto r = set_suspended(run_, sinks[i], true);
        record(report, sinks[i], r);
        if (r.timed_out) {
            // Wedged PulseAudio: every further call would cost the same
            // bound. The idle-timer hand-over is the fallback for the rest.
            report.abandoned.assign(sinks.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                    sinks.end());
            break;
        }
    }
    return report;
}

SinkReport SinkSuspension::resume() {
    if (held_.empty() || !run_) return SinkReport{};
    // One pass over everything held, then release the list whatever the
    // outcome: a sink whose resume failed here is the start-up recovery's
    // to retry, not the destructor's (it would only repeat the bound).
    std::vector<std::string> sinks;
    sinks.swap(held_);
    return resume_each(run_, sinks);
}

SinkReport resume_all_sinks(const PactlRunner& run) {
    SinkReport report;
    if (!run) return report;
    const auto sinks = list_sinks(run, &report.listed);
    if (!report.listed) return report;
    SinkReport resumed = resume_each(run, sinks);
    resumed.listed = true;
    return resumed;
}

}  // namespace retroarch
