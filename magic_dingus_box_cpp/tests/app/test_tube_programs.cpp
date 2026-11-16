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
    REQUIRE(tv.output == Program::out_rich_raster_ag);
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

TEST_CASE("tube programs: a full-screen film drops afterglow, then glow, to fit", "[tube]") {
    const Picture film{1920.0, 1080.0, false};
    for (Look look : kLooks) {
        TubeSetting s;
        select_look(s, look);
        const Resolved r = resolve(s);
        const PassPlan full = plan_for(needs_of(tube_uniforms_for(r, film), r), true);
        const PassPlan p = plan_passes(r, film);
        const TargetSizes t = target_sizes(p, film, tube_uniforms_for(r, film));
        INFO(look_name(look));
        REQUIRE(p.cls == FrameClass::Rich30);
        if (predicted_ms(full, target_sizes(full, film, tube_uniforms_for(r, film))) <= kRichBudgetMs) REQUIRE(p == full);
        if (full.glow && !p.glow) REQUIRE_FALSE(p.afterglow);   // glow goes only after afterglow
        if (full.afterglow && !p.afterglow && p.glow) REQUIRE(predicted_ms(p, t) <= kRichBudgetMs);
    }
    TubeSetting s;
    select_look(s, Look::tv);
    REQUIRE(plan_passes(resolve(s), Picture{1440.0, 1080.0, false}).afterglow);   // the 4:3 picture keeps it
}

TEST_CASE("tube programs: the prediction adds the plan's passes from the measured prices", "[tube]") {
    TubeSetting s;
    select_look(s, Look::tv);
    const Resolved r = resolve(s);
    const PassPlan tv = plan_passes(r, Picture{1440.0, 1080.0, false});
    const TargetSizes t = target_sizes(tv, Picture{1440.0, 1080.0, false}, tube_uniforms_for(r, {1440.0, 1080.0, false}));
    const PassCosts& c = kPassCosts;
    // TV: mean probe, quarter + glow, guns+chroma at half width, lines, the raster output, afterglow.
    REQUIRE(tv.glow_half);   // TV's glow is wide: blurred on the half-size targets
    const double expect = c.probe + c.quarter + c.glow_blur_half + c.sig_guns_chroma * c.half_width_factor + c.lines_half +
                          c.out_rich_raster + c.history;
    REQUIRE(tv.mean_probe);
    REQUIRE(predicted_ms(tv, t) == Catch::Approx(expect));
    // The same plan on a picture a third larger costs a third more.
    TargetSizes wide = t;
    wide.pic_w = 1920;
    REQUIRE(predicted_ms(tv, wide) == Catch::Approx(expect * (1920.0 * 1080.0) / kReferencePicturePixels));
    // A lean plan is the lean output alone.
    REQUIRE(predicted_ms(PassPlan{}, t) == Catch::Approx(c.out_lean));
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

TEST_CASE("tube programs: a wide glow is blurred at half the quarter's size", "[tube]") {
    const Picture pic{1440.0, 1080.0, false};
    // The threshold (measured: see kGlowHalfSigma) against GLOW SIZE.
    for (int row = 0; row <= 100; ++row) {
        TubeSetting s;
        select_look(s, Look::tv);
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
