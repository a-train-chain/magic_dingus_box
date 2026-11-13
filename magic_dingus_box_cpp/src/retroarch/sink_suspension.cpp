#include "sink_suspension.h"

#include <cstddef>
#include <iostream>
#include <sstream>
#include <utility>

namespace retroarch {

namespace {

std::vector<ListedSink> list_sinks(const PactlRunner& run, bool* listed) {
    const auto r = run({"pactl", "list", "short", "sinks"});
    *listed = r.ok();
    if (!r.ok()) return {};
    return parse_sinks(r.out);
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

void append_names(std::ostringstream& os, const std::vector<std::string>& sinks) {
    for (size_t i = 0; i < sinks.size(); ++i) os << (i ? ", " : "") << sinks[i];
}

void append_list(std::ostringstream& os, const char* label,
                 const std::vector<std::string>& sinks) {
    if (sinks.empty()) return;
    os << "; " << label << ": ";
    append_names(os, sinks);
}

// A sink listed RUNNING or IDLE is open, so it carries no suspend of any
// kind. Anything else — SUSPENDED, or no state column — may be ours.
bool may_be_suspended(const ListedSink& sink) {
    return sink.state != "RUNNING" && sink.state != "IDLE";
}

}  // namespace

std::vector<ListedSink> parse_sinks(const std::string& pactl_short_sinks) {
    std::vector<ListedSink> sinks;
    std::istringstream iss(pactl_short_sinks);
    std::string line;
    while (std::getline(iss, line)) {
        size_t i = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
        if (i == 0 || i >= line.size() || line[i] != '\t') continue;
        std::vector<std::string> fields;  // name, driver, sample spec, state
        std::istringstream rest(line.substr(i + 1));
        std::string field;
        while (std::getline(rest, field, '\t')) fields.push_back(field);
        if (fields.empty() || fields[0].empty()) continue;
        ListedSink sink;
        sink.name = fields[0];
        if (fields.size() >= 4) sink.state = fields[3];
        sinks.push_back(std::move(sink));
    }
    return sinks;
}

std::vector<std::string> parse_sink_names(const std::string& pactl_short_sinks) {
    std::vector<std::string> names;
    for (auto& sink : parse_sinks(pactl_short_sinks)) names.push_back(std::move(sink.name));
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
        append_names(os, report.done);
    }
    append_list(os, "FAILED", report.failed);
    append_list(os, "TIMED OUT", report.timed_out);
    append_list(os, "not tried", report.abandoned);
    return os.str();
}

std::string describe_resume_all(const SinkReport& report) {
    if (!report.listed) return "pactl list short sinks did not answer; nothing checked";
    const size_t suspended =
        report.done.size() + report.failed.size() + report.timed_out.size();
    const size_t total = suspended + report.not_suspended.size();
    std::ostringstream os;
    if (suspended == 0) {
        os << "checked " << total << " PulseAudio sink(s), none suspended";
        return os.str();
    }
    os << suspended << " of " << total
       << " PulseAudio sink(s) listed SUSPENDED (by a game session, or PulseAudio's "
          "idle suspend): cleared any user suspend on ";
    if (report.done.empty()) {
        os << "none";
    } else {
        append_names(os, report.done);
    }
    append_list(os, "FAILED", report.failed);
    append_list(os, "TIMED OUT", report.timed_out);
    return os.str();
}

SinkSuspension::SinkSuspension(PactlRunner run) : run_(std::move(run)) {}

SinkSuspension::~SinkSuspension() {
    if (held_.empty()) return;
    // Reached without an explicit resume(): an early return between the
    // suspend and the end of the session. A throw out of a destructor is
    // std::terminate, so nothing may escape — the start-up recovery covers
    // whatever this could not do.
    try {
        const SinkReport r = resume();
        std::cout << "[audio] session guard " << describe_sink_report("resumed", r)
                  << std::endl;
    } catch (...) {
        try {
            std::cerr << "[audio] session guard: resume threw; left to the start-up "
                         "recovery" << std::endl;
        } catch (...) {
        }
    }
}

SinkReport SinkSuspension::suspend() {
    SinkReport report;
    if (suspend_attempted_ || !run_) return report;
    suspend_attempted_ = true;

    const auto sinks = list_sinks(run_, &report.listed);
    for (size_t i = 0; i < sinks.size(); ++i) {
        // Held BEFORE the call: a timed-out suspend may still have landed.
        held_.push_back(sinks[i].name);
        const auto r = set_suspended(run_, sinks[i].name, true);
        record(report, sinks[i].name, r);
        if (r.timed_out) {
            // Wedged PulseAudio: every further call would cost the same
            // bound. The idle-timer hand-over is the fallback for the rest.
            for (size_t j = i + 1; j < sinks.size(); ++j) {
                report.abandoned.push_back(sinks[j].name);
            }
            break;
        }
    }
    return report;
}

SinkReport SinkSuspension::resume() {
    if (held_.empty() || !run_) return SinkReport{};
    // One pass over everything held, then release the list whatever the
    // outcome. A sink whose resume timed out or failed is the post-game
    // retry's (retry_resume_if_needed), then the start-up recovery's — not
    // the destructor's, which would only repeat the same bound at once.
    std::vector<std::string> sinks;
    sinks.swap(held_);
    return resume_each(run_, sinks);
}

SinkReport resume_all_sinks(const PactlRunner& run) {
    SinkReport report;
    if (!run) return report;
    const auto listed = list_sinks(run, &report.listed);
    if (!report.listed) return report;
    for (const auto& sink : listed) {
        if (may_be_suspended(sink)) {
            record(report, sink.name, set_suspended(run, sink.name, false));
        } else {
            report.not_suspended.push_back(sink.name);
        }
    }
    return report;
}

std::optional<SinkReport> retry_resume_if_needed(const SinkReport& session_resume,
                                                 const PactlRunner& run) {
    if (session_resume.timed_out.empty() && session_resume.failed.empty()) {
        return std::nullopt;
    }
    return resume_all_sinks(run);
}

}  // namespace retroarch
