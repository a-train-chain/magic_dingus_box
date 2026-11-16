// app/tube_ops.h - the web's op log (data/tube_request.json): its text parsed
// into validated ops, adopted at boot, applied in order through the core's setters.
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>
#include "app/app_state.h"
#include "app/tube_ops.h"
#include "tube/tube_setting.h"

namespace {
std::string log_of(const std::string& ops) { return R"({"version":1,"ops":[)" + ops + "]}"; }
std::vector<app::TubeOp> parsed(const std::string& text) {
    auto p = app::parse_tube_ops(text); INFO(p.error); REQUIRE(p.ops); return *p.ops; }
const std::string kOps = log_of(
    R"({"seq":12,"op":"look","look":"tv"},{"seq":13,"op":"row","key":"hum","value":20},)"
    R"({"seq":14,"op":"mask","mask":"dots"},{"seq":15,"op":"engine","engine":"tube"},{"seq":16,"op":"degauss"})");
}
TEST_CASE("tube ops: applied in order, each through the core's setters", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; app::adopt_tube_ops(nullptr, live);
    const auto r = app::apply_tube_ops(parsed(kOps), d, live, true);
    REQUIRE(r.changed); REQUIRE(r.applied == 5); REQUIRE(r.skipped == 0);
    tube::TubeSetting want; tube::select_look(want, tube::Look::tv);
    tube::set_row(want, tube::Effect::hum, 20); tube::set_mask(want, tube::Mask::dots);
    REQUIRE(d.tube == want);
    REQUIRE(d.crt_engine == app::CrtEngine::Tube); REQUIRE(d.enhanced_crt_enabled);
    REQUIRE(live.handled_seq == 16); REQUIRE(live.degauss_requests == 1);
    const auto again = app::apply_tube_ops(parsed(kOps), d, live, true);     // nothing twice
    REQUIRE_FALSE(again.changed); REQUIRE(again.applied == 0); REQUIRE(live.degauss_requests == 1);
}
TEST_CASE("tube ops: a web op touches only its own row", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; app::adopt_tube_ops(nullptr, live);
    tube::set_row(d.tube, tube::Effect::noise, 35);                          // the TV, just now
    app::apply_tube_ops(parsed(log_of(R"({"seq":1,"op":"row","key":"hum","value":20})")), d, live, true);
    REQUIRE(*d.tube.rows[tube::index_of(tube::Effect::noise)] == 35);
    REQUIRE(*d.tube.rows[tube::index_of(tube::Effect::hum)] == 20);
    app::apply_tube_ops(parsed(log_of(R"({"seq":2,"op":"row","key":"noise","value":10})")), d, live, true);
    REQUIRE(*d.tube.rows[tube::index_of(tube::Effect::noise)] == 10);        // same row: the later op wins
}
TEST_CASE("tube ops: the file found at boot is adopted, never applied", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live;
    const auto ops = parsed(kOps);
    app::adopt_tube_ops(&ops, live);
    REQUIRE(live.adopted); REQUIRE(live.handled_seq == 16); REQUIRE(live.degauss_requests == 0);
    REQUIRE(d.tube == tube::TubeSetting{});
    REQUIRE(app::apply_tube_ops(ops, d, live, true).applied == 0);
    app::TubeLiveState empty; app::adopt_tube_ops(nullptr, empty);
    REQUIRE(empty.adopted); REQUIRE(empty.handled_seq == 0);
}
TEST_CASE("tube ops: a box where Tube is not offered consumes ops without applying them", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; app::adopt_tube_ops(nullptr, live);
    const auto r = app::apply_tube_ops(parsed(kOps), d, live, false);
    REQUIRE_FALSE(r.changed); REQUIRE(r.skipped == 5); REQUIRE(live.handled_seq == 16);
    REQUIRE(d.crt_engine == app::CrtEngine::Classic); REQUIRE(live.degauss_requests == 0);
}
TEST_CASE("tube ops: any bad op refuses the whole file", "[tube]") {
    const std::vector<std::string> bad = {
        "", "not json", "[]", R"({"version":2,"ops":[]})", R"({"version":1,"ops":{}})",
        log_of("5"), log_of(R"({"seq":0,"op":"reset"})"), log_of(R"({"seq":"3","op":"reset"})"),
        log_of(R"({"seq":3,"op":"reset"},{"seq":3,"op":"reset"})"),
        log_of(R"({"seq":4,"op":"reset"},{"seq":3,"op":"reset"})"),
        log_of(R"({"seq":1,"op":"wobble"})"),
        log_of(R"({"seq":1,"op":"row","key":"wobble","value":5})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":101})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":-1})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":"20"})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":20.0})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":true})"),
        log_of(R"({"seq":1,"op":"mask","mask":"Slot"})"), log_of(R"({"seq":1,"op":"look","look":"vhs"})"),
        log_of(R"({"seq":1,"op":"engine","engine":"plasma"})"),
        std::string(app::kTubeOpsMaxBytes + 1, ' ')};
    for (const auto& text : bad) {
        INFO(text.substr(0, 120));
        const auto p = app::parse_tube_ops(text);
        REQUIRE_FALSE(p.ops); REQUIRE_FALSE(p.error.empty());
    }
    REQUIRE(app::parse_tube_ops(log_of("")).ops->empty());
}
TEST_CASE("tube ops: hostile text is refused, never thrown", "[tube]") {
    const std::vector<std::string> bad = {
        std::string(5000, '[') + std::string(5000, ']'),                   // nesting past jsoncpp's limit
        R"({"version":1,"ops":[)",                                         // truncated
        log_of("") + "trailing",
        R"({"version":1.0,"ops":[]})", R"({"version":"1","ops":[]})", R"({"ops":[]})",
        log_of(R"({"seq":1e400,"op":"reset"})"),
        log_of(R"({"seq":-1,"op":"reset"})"),
        log_of(R"({"seq":3.0,"op":"reset"})"),
        log_of(R"({"seq":99999999999999999999999,"op":"reset"})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":18446744073709551615})"),
        log_of(R"({"seq":1,"op":"row","key":"hum","value":-9223372036854775808})"),
        log_of(R"({"seq":1,"op":"row","key":"hum"})"), log_of(R"({"seq":1,"op":"row","value":5})"),
        log_of(R"({"seq":1,"op":["row"]})"), log_of(R"({"op":"reset"})"),
        log_of(R"({"seq":1,"op":"mask","mask":3})"), log_of(R"({"seq":1,"op":"look"})"),
        log_of(R"({"seq":1,"op":"engine","engine":"Tube"})")};
    for (const auto& text : bad) {
        INFO(text.substr(0, 120));
        app::TubeOpsParse p;
        REQUIRE_NOTHROW(p = app::parse_tube_ops(text));
        REQUIRE_FALSE(p.ops); REQUIRE_FALSE(p.error.empty());
    }
}
TEST_CASE("tube ops: extra fields are ignored and the widest seq is kept", "[tube]") {
    const auto ops = parsed(R"({"version":1,"from":"web","ops":[)"
                            R"({"seq":7,"op":"row","key":"hum","value":0,"note":"x"},)"
                            R"({"seq":18446744073709551615,"op":"degauss"}]})");
    REQUIRE(ops.size() == 2);
    REQUIRE(ops[0].kind == app::TubeOpKind::Row); REQUIRE(ops[0].effect == tube::Effect::hum);
    REQUIRE(ops[0].value == 0);
    REQUIRE(ops[1].seq == 18446744073709551615ULL); REQUIRE(ops[1].kind == app::TubeOpKind::Degauss);
}
TEST_CASE("tube ops: only ops past the handled seq apply, and a moved-back row counts as no change", "[tube]") {
    app::AppState::DisplaySettings d; app::TubeLiveState live; app::adopt_tube_ops(nullptr, live);
    live.handled_seq = 13;
    const auto r = app::apply_tube_ops(parsed(kOps), d, live, true);          // 14, 15, 16 only
    REQUIRE(r.applied == 3); REQUIRE(d.tube.look == tube::kLookDefault);
    REQUIRE_FALSE(d.tube.rows[tube::index_of(tube::Effect::hum)]);
    app::AppState::DisplaySettings same; app::TubeLiveState l2; app::adopt_tube_ops(nullptr, l2);
    const auto r2 = app::apply_tube_ops(parsed(log_of(R"({"seq":1,"op":"reset"},{"seq":2,"op":"degauss"})")),
                                        same, l2, true);
    REQUIRE(r2.applied == 2); REQUIRE_FALSE(r2.changed); REQUIRE(l2.degauss_requests == 1);
}
