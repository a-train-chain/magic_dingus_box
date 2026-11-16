#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "tube/tube_programs.h"

using namespace tube;

namespace {
constexpr unsigned kNeedBits = 13;
Needs needs_from_bits(unsigned b) {
    Needs n;
    bool* f[] = {&n.tone,      &n.displace, &n.sharp,  &n.guns, &n.chroma, &n.raster, &n.raster_only,
                 &n.glow,      &n.afterglow, &n.motion, &n.abl,  &n.bend,   &n.beam};
    for (unsigned i = 0; i < kNeedBits; ++i) *f[i] = (b >> i) & 1u;
    if (n.raster_only) n.raster = true;
    return n;
}
bool lean_has(const Needs& n, const LeanSet& l) {
    return (!n.bend || l.bend) && (!n.beam || l.beam) && (!n.tone || l.tone) && (!n.displace || l.displace) &&
           (!n.motion || l.motion) && (!n.abl || l.probe);
}
bool has_define(Program p, const std::string& name) {
    const std::string d = " " + program_defines(p) + " ";
    return d.find(" " + name + " ") != std::string::npos;
}
}  // namespace

TEST_CASE("tube programs: nineteen, named once, in enum order", "[tube]") {
    REQUIRE(kPrograms.size() == 19);
    for (std::size_t i = 0; i < kPrograms.size(); ++i) REQUIRE(static_cast<std::size_t>(kPrograms[i].id) == i);
    std::set<std::string_view> names;
    for (auto& p : kPrograms) names.insert(p.name);
    REQUIRE(names.size() == 19);
}

TEST_CASE("tube programs: every signal program has the sharpness path (the measured compiler hang)", "[tube]") {
    int signals = 0;
    for (const auto& p : kPrograms) {
        if (p.file != ShaderFile::signal) continue;
        ++signals;
        INFO(p.name);
        REQUIRE(has_define(p.id, "SIG_SHARP"));
        REQUIRE((program_caps(p.id) & kCapSharp));
    }
    REQUIRE(signals == 4);
}

TEST_CASE("tube programs: every reachable setting maps to a program with what it needs", "[tube]") {
    for (const LeanSet lean : {kLeanSet, LeanSet{true, false, true, true, false, true, false, true},
                               LeanSet{false, false, false, false, false, false, false, false}})
        for (unsigned b = 0; b < (1u << kNeedBits); ++b)
            for (bool big : {false, true}) {
                const Needs n = needs_from_bits(b);
                const PassPlan p = plan_for(n, big, lean);
                INFO(b << " big " << big);
                const bool rich_need = n.sharp || n.guns || n.chroma || n.raster || n.glow || n.afterglow;
                const bool rich = rich_need || big || !lean.usable || !lean_has(n, lean);
                REQUIRE((p.cls == FrameClass::Rich30) == rich);
                const std::uint32_t oc = program_caps(p.output);
                if (!rich) {
                    REQUIRE(p.output == ((n.motion || n.displace) ? Program::out_lean_moving : Program::out_lean_still));
                    REQUIRE_FALSE(p.signal.has_value());
                    REQUIRE_FALSE(p.glow);
                    REQUIRE_FALSE(p.afterglow);
                    REQUIRE(p.mean_probe == n.abl);
                    continue;
                }
                REQUIRE(p.signal.has_value() == (n.sharp || n.guns || n.chroma || n.raster));
                if (p.signal) {
                    const std::uint32_t sc = program_caps(*p.signal);
                    REQUIRE((sc & kCapTone));
                    REQUIRE((sc & kCapDisplace));
                    if (n.guns) REQUIRE((sc & kCapGuns));
                    if (n.chroma) REQUIRE((sc & kCapChroma));
                    REQUIRE((oc & kCapSceneSource) == 0);
                    REQUIRE(p.signal_half == (n.chroma || n.raster));
                } else {
                    REQUIRE((oc & kCapSceneSource));
                    REQUIRE((oc & kCapTone));
                    REQUIRE((oc & kCapDisplace));
                }
                REQUIRE((oc & kCapBend));
                REQUIRE((oc & kCapBeam));
                REQUIRE((oc & kCapMotion));
                REQUIRE((oc & kCapAbl));
                REQUIRE(p.lines == n.raster);
                REQUIRE(((oc & kCapRasterOnly) != 0) == n.raster_only);
                if (!big) {
                    REQUIRE(p.glow == n.glow);
                    REQUIRE(p.afterglow == n.afterglow);
                }
                if (p.glow) REQUIRE((oc & kCapGlow));
                REQUIRE(((oc & kCapHistory) != 0) == p.afterglow);
                REQUIRE(p.mean_probe == n.abl);
            }
}

TEST_CASE("tube programs: a lean output compiles exactly the lean set", "[tube]") {
    const std::uint32_t still = program_caps(Program::out_lean_still);
    const std::uint32_t moving = program_caps(Program::out_lean_moving);
    REQUIRE(((still & kCapBend) != 0) == kLeanSet.bend);
    REQUIRE(((still & kCapTone) != 0) == kLeanSet.tone);
    REQUIRE(((still & kCapBeam) != 0) == kLeanSet.beam);
    REQUIRE(((still & kCapAbl) != 0) == kLeanSet.probe);
    REQUIRE((still & kCapMotion) == 0);
    REQUIRE((still & kCapDisplace) == 0);
    REQUIRE(((moving & kCapDisplace) != 0) == kLeanSet.displace);
    REQUIRE(((moving & kCapMotion) != 0) == kLeanSet.motion);
    REQUIRE(has_define(Program::out_lean_still, "OUT_BEAM_CHEAP") == kLeanSet.cheap_beam);
    REQUIRE(has_define(Program::out_lean_moving, "OUT_DISPLACE") == kLeanSet.displace);
    REQUIRE(has_define(Program::out_lean_moving, "OUT_MOTION") == kLeanSet.motion);
    REQUIRE(has_define(Program::out_lean_still, "OUT_ABL") == kLeanSet.probe);
    REQUIRE(has_define(Program::out_rich_scene, "OUT_BEAM_CHEAP") == kRichBeamCheap);
    for (const auto& p : kPrograms) {   // a define is named once
        std::set<std::string> seen;
        std::string word;
        const std::string d = program_defines(p.id);
        for (std::size_t i = 0; i <= d.size(); ++i) {
            if (i == d.size() || d[i] == ' ') {
                if (!word.empty()) REQUIRE(seen.insert(word).second);
                word.clear();
            } else {
                word += d[i];
            }
        }
    }
}

TEST_CASE("tube programs: the shipped looks", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    auto plan = [&](Look l) {
        TubeSetting s;
        select_look(s, l);
        return plan_passes(resolve(s), pic);
    };
    for (auto look : kLooks) {   // the two pure deciders agree
        TubeSetting s;
        select_look(s, look);
        REQUIRE(plan(look).cls == frame_class(resolve(s), pic));
    }
    const PassPlan tv = plan(Look::tv);
    REQUIRE(tv.signal == Program::sig_guns_chroma);
    REQUIRE(tv.signal_half);
    REQUIRE(tv.lines);
    REQUIRE(tv.output == Program::out_rich_raster);   // afterglow dropped: TV cannot afford it here
    REQUIRE(plan(Look::trinitron).output == Program::out_rich_raster_ag);
    REQUIRE(plan(Look::arcade).output == Program::out_rich_raster_only_ag);   // raster 100
}

TEST_CASE("tube programs: the pass plan's class is the frame class, row by row", "[tube]") {
    for (bool crt_native : {false, true})
        for (const Effect e : kEffects)
            for (int v : {0, 5, 50, 55, 100}) {
                const Picture pic{1440.0, 1080.0, crt_native};
                TubeSetting s;
                for (const Effect f : kEffects)
                    if (forces_30fps(f)) set_row(s, f, rate_neutral(f));
                set_row(s, e, v);
                const Resolved r = resolve(s);
                INFO(effect_key(e) << " = " << v << " crt_native " << crt_native);
                REQUIRE(plan_passes(r, pic).cls == frame_class(r, pic));
            }
}

TEST_CASE("tube programs: a cheap row change keeps the programs, a crossing changes them", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    TubeSetting s;   // a lean-able PVM: every row the measured lean set cannot draw, at neutral
    for (auto e : kEffects)
        if (forces_30fps(e)) set_row(s, e, rate_neutral(e));
    const PassPlan a = plan_passes(resolve(s), pic);
    REQUIRE(a.cls == FrameClass::Lean60);
    set_row(s, Effect::brightness, 80);
    set_row(s, Effect::maskStrength, 90);
    REQUIRE(plan_passes(resolve(s), pic) == a);
    set_row(s, Effect::glow, 5);
    REQUIRE(plan_passes(resolve(s), pic).cls == FrameClass::Rich30);
}

namespace {
Resolved look_resolved(Look look, int afterglow = -1) {
    TubeSetting s;
    select_look(s, look);
    if (afterglow >= 0) set_row(s, Effect::afterglow, afterglow);
    return resolve(s);
}
double plan_ms(const PassPlan& p, const Resolved& r, const Picture& pic) {
    return predicted_ms(p, target_sizes(p, pic, tube_uniforms_for(r, pic)), glow_sigma_px(r, pic));
}
// What each look leaves out at `pic`, as the status lists it.
std::string dropped(Look look, const Picture& pic) {
    const NotDrawn d = plan_passes_and_drops(look_resolved(look), pic).not_drawn;
    return std::string(d.afterglow ? "afterglow" : "") + (d.afterglow && d.glow ? "," : "") + (d.glow ? "glow" : "");
}
}  // namespace

// The proof the automatic rule fires for the right looks: the prediction reproduces the
// kiosk's own window-4 totals (docs/engineering/measurements/2026-11-15-tube-budgets-pi5.txt,
// section 7, binary b5f67d2, 1440x1080). The tolerance is 0.20 ms, not 0.15: Trinitron and
// TV run the same lines, raster output and history programs and TV measured 0.11 ms more on
// each (it drew at 20 fps), so no per-pass prices fit both closer than ~0.18 (tube_budget.h).
TEST_CASE("tube programs: the prediction reproduces the kiosk's measured totals", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    struct Case {
        Look look;
        int afterglow;   // -1: the look's own
        double measured;
    };
    const Case cases[] = {
        {Look::pvm, -1, 19.11},     {Look::monitor, -1, 17.08}, {Look::trinitron, -1, 22.62},
        {Look::tv, -1, 24.47},      {Look::arcade, -1, 22.94},  {Look::everything, -1, 24.41},
        {Look::tv, 0, 22.56},       {Look::everything, 0, 22.52},
    };
    for (const Case& c : cases) {
        const Resolved r = look_resolved(c.look, c.afterglow);
        // The plan the look WANTS (nothing dropped): what the kiosk measured.
        const PassPlan wanted = plan_for(needs_of(tube_uniforms_for(r, pic), r), false);
        PassPlan p = wanted;
        p.glow_half = p.glow && glow_is_wide(glow_sigma_px(r, pic));
        const double ms = plan_ms(p, r, pic);
        INFO(look_name(c.look) << " afterglow " << c.afterglow << " predicted " << ms << " measured " << c.measured);
        CHECK(std::abs(ms - c.measured) <= 0.20);
    }
}

TEST_CASE("tube programs: at 1440x1080 TV and Everything drop afterglow, the others nothing", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    CHECK(dropped(Look::pvm, pic).empty());
    CHECK(dropped(Look::monitor, pic).empty());
    CHECK(dropped(Look::trinitron, pic).empty());
    CHECK(dropped(Look::arcade, pic).empty());
    CHECK(dropped(Look::tv, pic) == "afterglow");
    CHECK(dropped(Look::everything, pic) == "afterglow");
    for (Look look : kLooks) {
        const Resolved r = look_resolved(look);
        const PlannedPasses pp = plan_passes_and_drops(r, pic);
        INFO(look_name(look));
        REQUIRE(pp.plan == plan_passes(r, pic));                  // one decision, two spellings
        REQUIRE(pp.plan.cls == FrameClass::Rich30);               // still a rich, 30 fps look
        REQUIRE(pp.plan.cls == frame_class(r, pic));
        const Needs wanted = needs_of(tube_uniforms_for(r, pic), r);
        REQUIRE(pp.not_drawn.afterglow == (wanted.afterglow && !pp.plan.afterglow));
        REQUIRE(pp.not_drawn.glow == (wanted.glow && !pp.plan.glow));
        REQUIRE(plan_ms(pp.plan, r, pic) <= kRichBudgetMs);       // and what is drawn fits
    }
}

TEST_CASE("tube programs: the 720p Modern TV and CRT Native pictures drop nothing", "[tube]") {
    // Costs scale with the picture's area: the 960x720 picture of a 720p Modern TV, and
    // the CRT Native picture (the whole 1280x720 framebuffer: CRT Native always runs 720p,
    // config::display::target_drm_mode; the rows a real tube draws read as 0 there).
    for (const Picture pic : {Picture{960.0, 720.0, false}, Picture{1280.0, 720.0, true}})
        for (Look look : kLooks) {
            INFO(look_name(look) << " at " << pic.w << "x" << pic.h << " crt_native " << pic.crt_native);
            CHECK(dropped(look, pic).empty());
        }
    // Only until a restart: switching a 1080p Modern TV to CRT Native defers the mode
    // change, so the tube draws CRT Native into the 1920x1080 framebuffer meanwhile (with
    // the rows a real tube draws at 0, so it is lighter than a film).
    const Picture deferred{1920.0, 1080.0, true};
    CHECK(dropped(Look::pvm, deferred) == "afterglow");
    CHECK(dropped(Look::monitor, deferred).empty());
    CHECK(dropped(Look::trinitron, deferred).empty());   // ~28.8 ms whatever it drops: kept as asked
    CHECK(dropped(Look::tv, deferred) == "afterglow,glow");
    CHECK(dropped(Look::arcade, deferred) == "afterglow,glow");
    CHECK(dropped(Look::everything, deferred) == "afterglow,glow");
}

TEST_CASE("tube programs: the rule applies below the reference size too", "[tube]") {
    // A picture a little SMALLER than the reference: TV still cannot afford afterglow.
    const Picture pic{1440.0, 1060.0, false};
    REQUIRE(pic.w * pic.h < kReferencePicturePixels);
    CHECK(dropped(Look::tv, pic) == "afterglow");
    CHECK(dropped(Look::arcade, pic).empty());
    // A lean plan has neither effect, so nothing is ever left out of it.
    TubeSetting lean;
    for (auto e : kEffects)
        if (forces_30fps(e)) set_row(lean, e, rate_neutral(e));
    const PlannedPasses pp = plan_passes_and_drops(resolve(lean), Picture{1440.0, 1080.0, false});
    REQUIRE(pp.plan.cls == FrameClass::Lean60);
    REQUIRE_FALSE(pp.not_drawn.any());
}

TEST_CASE("tube programs: a full-screen film drops only what gets a look under the budget", "[tube]") {
    // A third more pixels than the reference; none of these figures was measured (they are
    // the 1440x1080 prices scaled by area).
    const Picture film{1920.0, 1080.0, false};
    const auto as_asked_ms = [&](Look look) {
        const Resolved r = look_resolved(look);
        PassPlan p = plan_for(needs_of(tube_uniforms_for(r, film), r), true);
        p.glow_half = p.glow && glow_is_wide(glow_sigma_px(r, film));
        return plan_ms(p, r, film);
    };
    for (Look look : kLooks) {
        const Resolved r = look_resolved(look);
        const PlannedPasses pp = plan_passes_and_drops(r, film);
        INFO(look_name(look));
        REQUIRE(pp.plan.cls == FrameClass::Rich30);
        // Either what is drawn fits, or nothing was dropped (it would not have helped).
        if (pp.not_drawn.any()) REQUIRE(plan_ms(pp.plan, r, film) <= kRichBudgetMs);
        else if (plan_ms(pp.plan, r, film) > kRichBudgetMs) REQUIRE(pp.plan == plan_passes(r, film));
    }
    CHECK(dropped(Look::pvm, film) == "afterglow");        // 25.5 as asked -> 22.9
    CHECK(dropped(Look::monitor, film).empty());           // 22.8: fits as asked
    // Over the budget whatever is dropped: kept as asked, and the pacing monitor's
    // "too heavy" notice tells the truth.
    for (Look heavy : {Look::trinitron, Look::tv, Look::arcade, Look::everything}) {
        INFO(look_name(heavy));
        CHECK(dropped(heavy, film).empty());
        CHECK(as_asked_ms(heavy) > kRichBudgetMs);
    }
}

TEST_CASE("tube programs: the ladder takes the first step that fits, else nothing", "[tube]") {
    // TV at the reference picture predicts 24.28 as asked, 22.66 without afterglow and
    // 22.04 without afterglow and glow; the budget is moved to land in each gap.
    const Picture pic{1440.0, 1080.0, false};
    const Resolved tv = look_resolved(Look::tv);
    const auto drops = [&](double budget) { return plan_passes_and_drops(tv, pic, budget).not_drawn; };
    CHECK(drops(25.0) == NotDrawn{false, false});   // fits as asked
    CHECK(drops(23.0) == NotDrawn{true, false});    // afterglow alone is enough
    CHECK(drops(22.3) == NotDrawn{true, true});     // both are needed, and fit
    const PlannedPasses none = plan_passes_and_drops(tv, pic, 21.0);   // nothing fits
    CHECK(none.not_drawn == NotDrawn{false, false});
    CHECK(none.plan.afterglow);
    CHECK(none.plan.glow);
    CHECK(none.plan == plan_passes_and_drops(tv, pic, 1000.0).plan);   // the plan as asked
}

TEST_CASE("tube programs: dropping an effect never changes the frame class", "[tube]") {
    // A setting whose ONLY rich needs are afterglow and glow would plan Lean60 without them;
    // a plan that dropped them must stay Rich30. Swept over every budget step and the
    // three picture sizes, and over every row one at a time.
    TubeSetting only;
    for (auto e : kEffects)
        if (forces_30fps(e)) set_row(only, e, rate_neutral(e));
    set_row(only, Effect::glow, 60);
    set_row(only, Effect::afterglow, 60);
    std::vector<TubeSetting> settings{only};
    for (const Effect e : kEffects) {
        TubeSetting s = only;
        set_row(s, e, e == Effect::glow || e == Effect::afterglow ? 0 : 70);
        settings.push_back(s);
    }
    int dropped_any = 0;
    for (const TubeSetting& s : settings)
        for (const Picture pic : {Picture{960.0, 720.0, false}, Picture{1440.0, 1080.0, false},
                                  Picture{1920.0, 1080.0, false}})
            for (int tenth = 0; tenth <= 400; tenth += 5) {
                const Resolved r = resolve(s);
                const PlannedPasses pp = plan_passes_and_drops(r, pic, tenth / 10.0);
                INFO("budget " << tenth / 10.0 << " at " << pic.w);
                REQUIRE(pp.plan.cls == frame_class(r, pic));
                if (pp.not_drawn.any()) {
                    ++dropped_any;
                    REQUIRE(pp.plan.cls == FrameClass::Rich30);
                    REQUIRE_FALSE(is_lean_output(pp.plan.output));
                }
            }
    REQUIRE(dropped_any > 0);   // the sweep did exercise the ladder
}

TEST_CASE("tube programs: the rows a dropped effect silences", "[tube]") {
    const NotDrawnRows none = not_drawn_rows({});
    CHECK(none.count == 0);
    const NotDrawnRows ag = not_drawn_rows({true, false});
    REQUIRE(ag.count == 1);
    CHECK(ag.rows[0] == Effect::afterglow);
    const NotDrawnRows both = not_drawn_rows({true, true});
    REQUIRE(both.count == 3);
    CHECK(both.rows[0] == Effect::afterglow);
    CHECK(both.rows[1] == Effect::glow);
    CHECK(both.rows[2] == Effect::glowSize);   // it sizes a glow that is not drawn
    CHECK(both.contains(Effect::glowSize));
    CHECK_FALSE(both.contains(Effect::hum));
    const NotDrawnRows glow_only = not_drawn_rows({false, true});   // a look with no afterglow
    REQUIRE(glow_only.count == 2);
    CHECK(glow_only.rows[0] == Effect::glow);
}

TEST_CASE("tube programs: the prediction adds the plan's passes from the measured prices", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    const Resolved r = look_resolved(Look::trinitron);
    const PassPlan p = plan_passes(r, pic);
    const TargetSizes t = target_sizes(p, pic, tube_uniforms_for(r, pic));
    const float sigma = glow_sigma_px(r, pic);
    const PassCosts& c = kPassCosts;
    // Trinitron: mean probe, quarter + a wide glow, guns at half width, lines, the raster
    // output into the history plus the copy pass.
    REQUIRE(p.glow_half);
    REQUIRE(p.mean_probe);
    REQUIRE(p.afterglow);
    REQUIRE(p.signal == Program::sig_guns);
    REQUIRE(blur_params(p, sigma).radius == 6);
    const double glow = c.glow_blur_half_base + 6 * c.glow_blur_half_per_radius;
    const double expect = c.probe + c.quarter + glow + c.sig_guns.half + c.lines + c.out_rich_raster.into_history +
                          c.history;
    REQUIRE(predicted_ms(p, t, sigma) == Catch::Approx(expect));
    // Without afterglow: the output to the screen and no copy pass.
    PassPlan no_ag = p;
    no_ag.afterglow = false;
    no_ag.output = Program::out_rich_raster;
    REQUIRE(predicted_ms(no_ag, t, sigma) ==
            Catch::Approx(expect - c.out_rich_raster.into_history - c.history + c.out_rich_raster.to_screen));
    // ... which saves the copy pass LESS the to-screen output's extra cost.
    REQUIRE(c.out_rich_raster.to_screen > c.out_rich_raster.into_history);
    REQUIRE(c.out_rich.to_screen > c.out_rich.into_history);
    REQUIRE(c.out_rich_raster_only.to_screen > c.out_rich_raster_only.into_history);
    // A narrow glow is the full-size chain's price.
    PassPlan narrow = p;
    narrow.glow_half = false;
    REQUIRE(predicted_ms(narrow, t, sigma) == Catch::Approx(expect - glow + c.glow_blur));
    // A signal at full width costs its full-width price.
    PassPlan sharp = p;
    sharp.signal = Program::sig_sharp;
    sharp.signal_half = false;
    REQUIRE(predicted_ms(sharp, t, sigma) == Catch::Approx(expect - c.sig_guns.half + c.sig_sharp.full));
    // The same plan on a picture a third larger: every picture-sized pass a third more; the
    // lines pass follows its own target and the probe's 64x64 target does not grow.
    TargetSizes wide = t;
    wide.pic_w = 1920;
    REQUIRE(predicted_ms(p, wide, sigma) ==
            Catch::Approx((expect - c.lines - c.probe) * (1920.0 * 1080.0) / kReferencePicturePixels + c.lines + c.probe));
    TargetSizes wide_lines = wide;
    wide_lines.lines_w = 960;
    REQUIRE(predicted_ms(p, wide_lines, sigma) - predicted_ms(p, wide, sigma) == Catch::Approx(c.lines / 3.0));
    // A lean plan is the lean output alone.
    REQUIRE(predicted_ms(PassPlan{}, t, 0.0f) == Catch::Approx(c.out_lean));
}

TEST_CASE("tube programs: the validation plans draw every program", "[tube]") {
    std::set<Program> drawn;
    for (const PassPlan& p : validation_plans())
        for (Program g : programs_of(p)) drawn.insert(g);
    if (!(kLeanSet.motion || kLeanSet.displace)) drawn.insert(Program::out_lean_moving);   // unreachable, still validated by name
    REQUIRE(drawn.size() == kProgramCount);
}

TEST_CASE("tube programs: a plan's programs start with the glass and name each once", "[tube]") {
    for (unsigned b = 0; b < (1u << kNeedBits); ++b) {
        const PassPlan p = plan_for(needs_from_bits(b), false);
        const std::vector<Program> g = programs_of(p);
        REQUIRE(g.front() == Program::glass);
        REQUIRE(std::set<Program>(g.begin(), g.end()).size() == g.size());
        REQUIRE(g.back() == (p.afterglow ? Program::copy_encode : p.output));
    }
}

TEST_CASE("tube programs: each program names its uniforms once", "[tube]") {
    for (const auto& info : kPrograms) {
        const std::vector<std::string_view> u = uniforms_of(info.id);
        INFO(info.name);
        REQUIRE_FALSE(u.empty());
        REQUIRE(std::set<std::string_view>(u.begin(), u.end()).size() == u.size());
    }
    const auto has = [](Program p, std::string_view name) {
        for (std::string_view u : uniforms_of(p))
            if (u == name) return true;
        return false;
    };
    REQUIRE(has(Program::out_rich_scene_ag, "dDecayR"));   // persistence * 0.68, once a frame
    REQUIRE_FALSE(has(Program::out_rich_scene, "dDecayR"));
    REQUIRE(has(Program::sig_guns, "convergence"));
    REQUIRE_FALSE(has(Program::sig_sharp, "convergence"));
    REQUIRE(has(Program::out_lean_still, "uSceneMap"));
    REQUIRE_FALSE(has(Program::out_rich_signal, "uSceneMap"));
}

TEST_CASE("tube programs: target sizes and memory", "[tube]") {
    TubeSetting s;
    select_look(s, Look::tv);
    const Picture pic{1440.0, 1080.0, false};
    const Resolved r = resolve(s);
    const PassPlan p = plan_passes(r, pic);
    const TargetSizes t = target_sizes(p, pic, tube_uniforms_for(r, pic));
    REQUIRE(t.quarter_w == 360);
    REQUIRE(t.quarter_h == 270);
    REQUIRE(t.signal_w == 720);
    REQUIRE(t.signal_h == 1080);
    REQUIRE(t.lines_w == 720);
    REQUIRE(t.lines_h == 270);
    REQUIRE(p.glow_half);
    REQUIRE(t.glow_w == 180);   // half the quarter: blur.frag uDown's (size + 1) / 2
    REQUIRE(t.glow_h == 135);
    for (unsigned b = 0; b < (1u << kNeedBits); ++b) {
        const PassPlan q = plan_for(needs_from_bits(b), false);
        const TargetSizes ts{1440, 1080, 360, 270, q.signal_half ? 720 : 1440, 1080, q.signal_half ? 720 : 1440, 270,
                             360, 270};
        REQUIRE(target_bytes(q, ts, true) < 32u * 1024u * 1024u);
    }
}

// The glow's sigma is in quarter-resolution pixels and grows with the picture's width, so
// the threshold falls on different sides of the looks at the real picture sizes: Modern TV
// at 1080p draws a 1440x1080 picture (the test above), at 720p a 960x720 one
// (video::content_rects: four_by_three_rect), and CRT Native fills the framebuffer.
TEST_CASE("tube programs: the glow threshold at the CRT Native and 720p Modern TV pictures", "[tube]") {
    struct Case {
        Picture pic;
        int wide_looks;   // pinned: how many of the six looks blur at half size
        int quarter_w, quarter_h;
    };
    const Case cases[] = {
        {{1920.0, 1080.0, true}, 6, 480, 270},    // CRT Native at 1080p
        {{1280.0, 720.0, true}, 5, 320, 180},     // CRT Native at 720p
        {{960.0, 720.0, false}, 5, 240, 180},     // Modern TV at 720p
    };
    for (const Case& c : cases) {
        int wide = 0;
        for (Look look : kLooks) {
            TubeSetting s;
            select_look(s, look);
            const Resolved r = resolve(s);
            const float sigma = glow_sigma_px(r, c.pic);
            const PlannedPasses pp = plan_passes_and_drops(r, c.pic);
            const PassPlan& p = pp.plan;
            INFO(look_name(look) << " at " << c.pic.w << "x" << c.pic.h << " sigma " << sigma);
            REQUIRE(p.glow != pp.not_drawn.glow);   // every look asks for a glow
            if (!p.glow) {   // a look that cannot afford it there: no glow targets to size
                if (sigma >= kGlowHalfSigma) ++wide;
                continue;
            }
            REQUIRE(p.glow_half == (sigma >= kGlowHalfSigma));   // from the sigma, not hard-coded
            const TargetSizes t = target_sizes(p, c.pic, tube_uniforms_for(r, c.pic));
            REQUIRE(t.quarter_w == c.quarter_w);
            REQUIRE(t.quarter_h == c.quarter_h);
            REQUIRE(t.glow_w == (p.glow_half ? c.quarter_w / 2 : c.quarter_w));
            REQUIRE(t.glow_h == (p.glow_half ? c.quarter_h / 2 : c.quarter_h));
            if (p.glow_half) ++wide;
        }
        INFO(c.pic.w << "x" << c.pic.h);
        CHECK(wide == c.wide_looks);   // a change to the threshold or the sizes shows here
    }
    // The pinned cases: Monitor's glow (GLOW SIZE 10) is the narrow one at both 720p sizes
    // (1.28 and 1.71 quarter px) and stays at full size; PVM is only just wide at 960x720
    // (1.95); at 1440 and 1080p Native every look is wide.
    const auto sigma_of = [](Look look, const Picture& pic) {
        TubeSetting s;
        select_look(s, look);
        return glow_sigma_px(resolve(s), pic);
    };
    CHECK(sigma_of(Look::monitor, Picture{960.0, 720.0, false}) < kGlowHalfSigma);
    CHECK(sigma_of(Look::monitor, Picture{1280.0, 720.0, true}) < kGlowHalfSigma);
    CHECK(sigma_of(Look::pvm, Picture{960.0, 720.0, false}) >= kGlowHalfSigma);
    CHECK(sigma_of(Look::monitor, Picture{1440.0, 1080.0, false}) >= kGlowHalfSigma);
    CHECK(sigma_of(Look::monitor, Picture{1920.0, 1080.0, true}) >= kGlowHalfSigma);
}

TEST_CASE("tube programs: an odd quarter's glow targets are the rounded-up half", "[tube]") {
    // 1004x700: a quarter of 251x175, odd both ways; PVM's glow is wide there (2.04).
    const Picture pic{1004.0, 700.0, false};
    TubeSetting s;
    select_look(s, Look::pvm);
    const Resolved r = resolve(s);
    REQUIRE(glow_sigma_px(r, pic) >= kGlowHalfSigma);
    const PassPlan p = plan_passes(r, pic);
    REQUIRE(p.glow);
    REQUIRE(p.glow_half);
    const TargetSizes t = target_sizes(p, pic, tube_uniforms_for(r, pic));
    REQUIRE(t.quarter_w == 251);
    REQUIRE(t.quarter_h == 175);
    CHECK(t.glow_w == 126);   // ceil(251 / 2), blur.frag uDown's (size + 1) / 2
    CHECK(t.glow_h == 88);    // ceil(175 / 2)
    PassPlan without = p;
    without.glow = false;
    // The two glow targets are RGBA8 at the rounded-up half size, nothing more.
    CHECK(target_bytes(p, t, true) - target_bytes(without, t, true) == 2u * 126u * 88u * 4u);
}

TEST_CASE("tube programs: a wide glow is blurred at half the quarter's size", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    // The threshold (measured: see kGlowHalfSigma) against GLOW SIZE.
    for (int row = 0; row <= 100; ++row) {
        TubeSetting s;
        select_look(s, Look::pvm);   // a look that affords its glow at every GLOW SIZE here
        set_row(s, Effect::glowSize, row);
        const Resolved r = resolve(s);
        const float sigma = glow_sigma_px(r, pic);
        const PassPlan p = plan_passes(r, pic);
        INFO(row << " sigma " << sigma);
        REQUIRE(p.glow);
        REQUIRE(p.glow_half == (sigma >= kGlowHalfSigma));
        const TargetSizes t = target_sizes(p, pic, tube_uniforms_for(r, pic));
        REQUIRE(t.glow_w == (p.glow_half ? 180 : 360));
        REQUIRE(t.glow_h == (p.glow_half ? 135 : 270));
        REQUIRE(t.quarter_w == 360);   // the quarter itself is unchanged: chroma and the probe read it
        const BlurParams b = blur_params(p, sigma);
        REQUIRE(b.down == p.glow_half);
        REQUIRE(b.sigma == std::max(sigma, 0.1f) * (p.glow_half ? 0.5f : 1.0f));
        REQUIRE(b.radius == std::min(static_cast<int>(std::ceil(3.0f * b.sigma)), 24));
    }
    // Every shipped look's glow is wide (all at GLOW SIZE 10 or more).
    for (Look look : kLooks) {
        TubeSetting s;
        select_look(s, look);
        INFO(look_name(look));
        REQUIRE(plan_passes(resolve(s), pic).glow_half);
    }
    // Without a glow pass there is nothing to halve; an odd quarter rounds up.
    TubeSetting off;
    set_row(off, Effect::glow, 0);
    REQUIRE_FALSE(plan_passes(resolve(off), pic).glow_half);
    PassPlan odd;
    odd.glow = odd.glow_half = true;
    const TargetSizes t = target_sizes(odd, Picture{1441.0, 1081.0, false}, tube_uniforms_for(resolve(TubeSetting{}), pic));
    REQUIRE(t.quarter_w == 361);
    REQUIRE(t.glow_w == 181);
    REQUIRE(t.quarter_h == 271);
    REQUIRE(t.glow_h == 136);
    // The half-size targets hold a quarter of the bytes.
    PassPlan full = odd;
    full.glow_half = false;
    const TargetSizes tf = target_sizes(full, Picture{1440.0, 1080.0, false}, tube_uniforms_for(resolve(TubeSetting{}), pic));
    const TargetSizes th = target_sizes(odd, Picture{1440.0, 1080.0, false}, tube_uniforms_for(resolve(TubeSetting{}), pic));
    REQUIRE(target_bytes(full, tf, true) - target_bytes(odd, th, true) == 2u * (360u * 270u - 180u * 135u) * 4u);
}

// Owner's decision (2026-11-15): an ACTUAL full-screen film (a picture larger than the
// reference) whose plan does not fit even after the ladder yields to the Enhanced engine.
// Nothing else ever yields, however large.
TEST_CASE("tube programs: a look too heavy for a full-screen film yields it to Enhanced", "[tube]") {
    const Picture film{1920.0, 1080.0, false};
    CHECK_FALSE(tube_yields_to_enhanced(look_resolved(Look::pvm), film, true));       // fits without afterglow
    CHECK_FALSE(tube_yields_to_enhanced(look_resolved(Look::monitor), film, true));   // fits as asked
    for (Look heavy : {Look::trinitron, Look::arcade, Look::tv, Look::everything}) {
        INFO(look_name(heavy));
        const PlannedPasses pp = plan_passes_and_drops(look_resolved(heavy), film);
        CHECK(pp.over_budget);
        CHECK_FALSE(pp.not_drawn.any());   // kept as asked: nothing is "not drawn"
        CHECK(tube_yields_to_enhanced(look_resolved(heavy), film, true));
        // The same size when it is NOT a film (CRT Native into a 1080p framebuffer until the
        // restart, a menu, a normal channel there): never yields; drawn as asked, late.
        CHECK_FALSE(tube_yields_to_enhanced(look_resolved(heavy), film, false));
        CHECK_FALSE(tube_yields_to_enhanced(look_resolved(heavy), Picture{1920.0, 1080.0, true}, false));
    }
    // A film where the setting fits only after dropping afterglow: Tube draws it.
    const PlannedPasses pvm = plan_passes_and_drops(look_resolved(Look::pvm), film);
    CHECK_FALSE(pvm.over_budget);
    CHECK(pvm.not_drawn == NotDrawn{true, false});
}

TEST_CASE("tube programs: nothing yields at or below the reference picture, whatever the cost", "[tube]") {
    for (const Picture pic : {Picture{1440.0, 1080.0, false}, Picture{960.0, 720.0, false},
                              Picture{1280.0, 720.0, true}, Picture{1440.0, 1060.0, false}})
        for (Look look : kLooks) {
            INFO(look_name(look) << " at " << pic.w << "x" << pic.h);
            CHECK_FALSE(tube_yields_to_enhanced(look_resolved(look), pic, true));
            CHECK_FALSE(tube_yields_to_enhanced(look_resolved(look), pic, true, 1.0));   // even over budget
        }
    // A setting over the budget at 1440x1080 even after the ladder (no shipped price set
    // makes one: the heaviest, TV without both, predicts 22.04, so the budget is lowered to
    // 21 ms to stand for it): drawn by Tube as asked, late, with the pacing monitor's
    // notice; never yielded.
    const Picture ref{1440.0, 1080.0, false};
    const Resolved r = look_resolved(Look::tv);
    const PlannedPasses pp = plan_passes_and_drops(r, ref, 21.0);
    REQUIRE(plan_ms(pp.plan, r, ref) > 21.0);
    CHECK(pp.over_budget);
    CHECK_FALSE(pp.not_drawn.any());
    CHECK_FALSE(tube_yields_to_enhanced(r, ref, true, 21.0));
    // The yield follows the plan AS DRAWN on the film.
    const Picture film{1920.0, 1080.0, false};
    CHECK(tube_yields_to_enhanced(look_resolved(Look::monitor), film, true, 20.0));
    CHECK_FALSE(tube_yields_to_enhanced(look_resolved(Look::monitor), film, true, 25.0));
}

TEST_CASE("tube programs: the renderer's yield decision is the pure one, evaluated once per change", "[tube]") {
    YieldDecision decide;
    TubeSetting tv;
    select_look(tv, Look::tv);
    const Picture film{1920.0, 1080.0, false}, normal{1440.0, 1080.0, false};
    for (int frame = 0; frame < 5; ++frame) CHECK(decide(tv, film, true));
    CHECK(decide.evaluations() == 1);
    CHECK_FALSE(decide(tv, normal, false));   // a channel flip: one evaluation
    CHECK_FALSE(decide(tv, film, false));     // the size alone, without the film fact: no
    CHECK(decide(tv, film, true));
    CHECK(decide.evaluations() == 4);
    TubeSetting monitor;
    select_look(monitor, Look::monitor);
    CHECK_FALSE(decide(monitor, film, true));
    CHECK(decide.evaluations() == 5);
    for (const TubeSetting& s : {tv, monitor})
        for (const Picture pic : {film, normal})
            for (bool is_film : {false, true})
                CHECK(decide(s, pic, is_film) == tube_yields_to_enhanced(resolve(s), pic, is_film));
}
