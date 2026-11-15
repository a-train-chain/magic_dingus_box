// tube_programs.h - the Tube renderer's FIXED program table, which passes a setting
// and a picture need (the pass plan), the sizes of their targets and what they are
// predicted to cost (design section 3, "The pipeline" and "A fixed program table").
//
// Nineteen programs and nothing else is ever compiled: the Pi 5's shader compiler can
// hang on an arbitrary combination of features (see the DRIVER HAZARD below), so the
// set is closed, validated in CI and drawn through its real passes on the hardware
// before the engine is offered. Cheap blocks inside a program are switched by uniform
// tests; a program differs from another only where a measurement found a structural
// cost (an extra pass, multi-tap reads, a different target).
//
// Everything here is decided from the STILL uniforms (tube_uniforms_for) and the
// resolved setting, never from one frame's: a plan changes only when the setting or
// the picture does, so programs are built then, never per frame.
//
// Pure: standard library only, no I/O, no GL.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tube/tube_budget.h"
#include "tube/tube_frame_class.h"
#include "tube/tube_inputs.h"
#include "tube/tube_setting.h"

namespace tube {

enum class Program : std::uint8_t {
    glass,
    quarter,
    blur,
    copy_encode,
    sig_sharp,
    sig_sharp_chroma,
    sig_guns,
    sig_guns_chroma,
    lines,
    out_lean_still,
    out_lean_moving,
    out_rich_scene,
    out_rich_scene_ag,
    out_rich_signal,
    out_rich_signal_ag,
    out_rich_raster,
    out_rich_raster_ag,
    out_rich_raster_only,
    out_rich_raster_only_ag,
};
inline constexpr std::size_t kProgramCount = 19;

// The GLSL file a program is assembled from (Task 6).
enum class ShaderFile : std::uint8_t { glass, quarter, blur, copy_encode, signal, lines, output };

// What a program can draw.
inline constexpr std::uint32_t kCapTone = 1u << 0, kCapDisplace = 1u << 1, kCapSharp = 1u << 2,
                               kCapGuns = 1u << 3, kCapChroma = 1u << 4, kCapSceneSource = 1u << 5,
                               kCapSignalSource = 1u << 6, kCapRaster = 1u << 7, kCapRasterOnly = 1u << 8,
                               kCapGlow = 1u << 9, kCapHistory = 1u << 10, kCapMotion = 1u << 11,
                               kCapAbl = 1u << 12, kCapBend = 1u << 13, kCapBeam = 1u << 14;

struct ProgramInfo {
    Program id;
    std::string_view name;
    ShaderFile file;
    std::string_view defines;   // space-separated; for out_lean_* only the fixed part (program_defines adds the rest)
    bool reads_scene;
    std::uint32_t caps;         // for out_lean_* only the fixed part (program_caps adds the rest)
};

namespace detail {
inline constexpr std::uint32_t kCapsSignal = kCapTone | kCapDisplace | kCapSharp;
inline constexpr std::uint32_t kCapsRichCommon = kCapBend | kCapBeam | kCapMotion | kCapAbl | kCapGlow;
inline constexpr std::uint32_t kCapsRichScene = kCapSceneSource | kCapTone | kCapDisplace | kCapsRichCommon;
inline constexpr std::uint32_t kCapsRichSignal = kCapSignalSource | kCapsRichCommon;
inline constexpr std::uint32_t kCapsRichRaster = kCapsRichSignal | kCapRaster;
inline constexpr std::uint32_t kCapsRichRasterOnly = kCapRaster | kCapRasterOnly | kCapsRichCommon;
}  // namespace detail

// THE TABLE, one row per program in enum order.
//
// DRIVER HAZARD (measured, docs/engineering/measurements/2026-11-15-tube-hang-isolation.txt):
// the Pi 5's compiler (Mesa V3D) hung, 4 runs of 4, on a signal program with chroma
// bleed (SIG_CHROMA) AND convergence (SIG_GUNS) but WITHOUT the sharpness path
// (SIG_SHARP). Every signal program below therefore carries SIG_SHARP, switched off
// inside by `sharpness != 1.0`, and sig_guns_chroma is the one that draws chroma with
// the guns. Never add a signal row without SIG_SHARP (a test enforces it).
inline constexpr std::array<ProgramInfo, kProgramCount> kPrograms{{
    {Program::glass, "glass", ShaderFile::glass, "", false, 0},
    {Program::quarter, "quarter", ShaderFile::quarter, "", true, 0},
    {Program::blur, "blur", ShaderFile::blur, "", false, 0},
    {Program::copy_encode, "copy_encode", ShaderFile::copy_encode, "", false, 0},
    {Program::sig_sharp, "sig_sharp", ShaderFile::signal, "SIG_SHARP", true, detail::kCapsSignal},
    {Program::sig_sharp_chroma, "sig_sharp_chroma", ShaderFile::signal, "SIG_SHARP SIG_CHROMA", true,
     detail::kCapsSignal | kCapChroma},
    {Program::sig_guns, "sig_guns", ShaderFile::signal, "SIG_SHARP SIG_GUNS", true, detail::kCapsSignal | kCapGuns},
    {Program::sig_guns_chroma, "sig_guns_chroma", ShaderFile::signal, "SIG_SHARP SIG_GUNS SIG_CHROMA", true,
     detail::kCapsSignal | kCapGuns | kCapChroma},
    {Program::lines, "lines", ShaderFile::lines, "", false, kCapRaster},
    {Program::out_lean_still, "out_lean_still", ShaderFile::output, "OUT_SCENE", true, kCapSceneSource},
    {Program::out_lean_moving, "out_lean_moving", ShaderFile::output, "OUT_SCENE", true, kCapSceneSource},
    {Program::out_rich_scene, "out_rich_scene", ShaderFile::output,
     "OUT_SCENE OUT_BEND OUT_SCANLINES OUT_TONE OUT_DISPLACE OUT_MOTION OUT_ABL OUT_GLOW", true,
     detail::kCapsRichScene},
    {Program::out_rich_scene_ag, "out_rich_scene_ag", ShaderFile::output,
     "OUT_SCENE OUT_BEND OUT_SCANLINES OUT_TONE OUT_DISPLACE OUT_MOTION OUT_ABL OUT_GLOW OUT_HISTORY", true,
     detail::kCapsRichScene | kCapHistory},
    {Program::out_rich_signal, "out_rich_signal", ShaderFile::output,
     "OUT_SIGNAL OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW", false, detail::kCapsRichSignal},
    {Program::out_rich_signal_ag, "out_rich_signal_ag", ShaderFile::output,
     "OUT_SIGNAL OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW OUT_HISTORY", false,
     detail::kCapsRichSignal | kCapHistory},
    {Program::out_rich_raster, "out_rich_raster", ShaderFile::output,
     "OUT_SIGNAL OUT_RASTER OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW", false, detail::kCapsRichRaster},
    {Program::out_rich_raster_ag, "out_rich_raster_ag", ShaderFile::output,
     "OUT_SIGNAL OUT_RASTER OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW OUT_HISTORY", false,
     detail::kCapsRichRaster | kCapHistory},
    {Program::out_rich_raster_only, "out_rich_raster_only", ShaderFile::output,
     "OUT_RASTER OUT_RASTER_ONLY OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW", false,
     detail::kCapsRichRasterOnly},
    {Program::out_rich_raster_only_ag, "out_rich_raster_only_ag", ShaderFile::output,
     "OUT_RASTER OUT_RASTER_ONLY OUT_BEND OUT_SCANLINES OUT_MOTION OUT_ABL OUT_GLOW OUT_HISTORY", false,
     detail::kCapsRichRasterOnly | kCapHistory},
}};

constexpr const ProgramInfo& program_info(Program p) noexcept { return kPrograms[static_cast<std::size_t>(p)]; }

constexpr bool is_lean_output(Program p) noexcept {
    return p == Program::out_lean_still || p == Program::out_lean_moving;
}

// The program's defines, space-separated: the table's, plus for the two lean outputs
// the measured lean set's ingredients, plus the cheap beam where it is chosen.
inline std::string program_defines(Program p) {
    const ProgramInfo& info = program_info(p);
    std::string out{info.defines};
    const auto add = [&out](std::string_view d) {
        if (!out.empty()) out += ' ';
        out += d;
    };
    if (is_lean_output(p)) {
        if (kLeanSet.bend) add("OUT_BEND");
        if (kLeanSet.beam) add("OUT_SCANLINES");
        if (kLeanSet.beam && kLeanSet.cheap_beam) add("OUT_BEAM_CHEAP");
        if (kLeanSet.tone) add("OUT_TONE");
        if (p == Program::out_lean_moving && kLeanSet.displace) add("OUT_DISPLACE");
        if (p == Program::out_lean_moving && kLeanSet.motion) add("OUT_MOTION");
        if (kLeanSet.probe) add("OUT_ABL");
    } else if (info.file == ShaderFile::output && kRichBeamCheap) {
        add("OUT_BEAM_CHEAP");
    }
    return out;
}

// The program's capabilities: the table's, plus for the lean outputs the lean set's.
inline std::uint32_t program_caps(Program p) noexcept {
    std::uint32_t caps = program_info(p).caps;
    if (is_lean_output(p)) {
        if (kLeanSet.bend) caps |= kCapBend;
        if (kLeanSet.beam) caps |= kCapBeam;
        if (kLeanSet.tone) caps |= kCapTone;
        if (kLeanSet.probe) caps |= kCapAbl;
        if (p == Program::out_lean_moving && kLeanSet.displace) caps |= kCapDisplace;
        if (p == Program::out_lean_moving && kLeanSet.motion) caps |= kCapMotion;
    }
    return caps;
}

// What a setting asks the passes for. Each field is one ingredient, true when a row
// that feeds it is away from neutral.
struct Needs {
    bool tone = false;         // CONTRAST or SATURATION != 50
    bool displace = false;     // JITTER, TRACKING or FLAGGING
    bool sharp = false;        // SHARPNESS != 50
    bool guns = false;         // CONVERGENCE or COLOUR DRIFT
    bool chroma = false;       // CHROMA BLEED
    bool raster = false;       // TRUE RASTER
    bool raster_only = false;  // TRUE RASTER at 100: the picture is the lines alone
    bool glow = false;         // GLOW
    bool afterglow = false;    // AFTERGLOW
    bool motion = false;       // NOISE, FLICKER, HUM, ROLLING BAND or PURITY
    bool abl = false;          // ABL or BREATHING (the mean probe)
    bool bend = false;         // CURVATURE
    bool beam = false;         // SCANLINES
};

// Read from the STILL uniforms (so a CRT Native picture's own-tube rows already read
// as 0) and, for the character rows the still does not carry, from the resolved
// setting. Rows that map to one of forces_30fps()'s ingredients here map to the same
// one there, so plan_passes() and frame_class() agree (a test sweeps every row).
inline Needs needs_of(const TubeUniforms& still, const Resolved& resolved) noexcept {
    const auto on = [&resolved](Effect effect) { return resolved.effects[index_of(effect)] > 0; };
    Needs n;
    n.tone = still.contrast != 1.0f || still.saturation != 1.0f;
    n.displace = still.jitter > 0.0f || on(Effect::tracking) || on(Effect::flagging);
    n.sharp = still.sharpness != 1.0f;
    n.guns = still.convergence > 0.0f || on(Effect::colorDrift);
    n.chroma = still.chromaBleed > 0.0f;
    n.raster = still.raster > 0.0f;
    n.raster_only = still.raster >= 1.0f;
    n.glow = still.glow > 0.0f;
    n.afterglow = still.persistence > 0.0f;
    n.motion = still.noise > 0.0f || still.flicker > 0.0f || still.hum > 0.0f || on(Effect::rollBand) ||
               on(Effect::purity);
    n.abl = still.abl > 0.0f || still.breathe > 0.0f;
    n.bend = still.curvature > 0.0f;
    n.beam = still.scanlines > 0.0f;
    return n;
}

// Which passes run, and which program draws each.
struct PassPlan {
    FrameClass cls = FrameClass::Lean60;
    bool mean_probe = false, quarter = false, glow = false;
    std::optional<Program> signal;
    bool signal_half = false;
    bool lines = false;
    Program output = Program::out_lean_still;
    bool afterglow = false;

    bool operator==(const PassPlan& o) const {
        return cls == o.cls && mean_probe == o.mean_probe && quarter == o.quarter && glow == o.glow &&
               signal == o.signal && signal_half == o.signal_half && lines == o.lines && output == o.output &&
               afterglow == o.afterglow;
    }
    bool operator!=(const PassPlan& o) const { return !(*this == o); }
};

// The plan for these needs. Lean60 (one output pass) only when the lean path exists,
// nothing needs a pass of its own, the picture is no larger than the reference and
// every ingredient asked for is in the lean set; otherwise Rich30.
inline PassPlan plan_for(const Needs& n, bool larger_than_reference, const LeanSet& lean = kLeanSet) noexcept {
    const bool raster = n.raster || n.raster_only;
    const bool rich_need = n.sharp || n.guns || n.chroma || raster || n.glow || n.afterglow;
    const bool lean_has = (!n.bend || lean.bend) && (!n.beam || lean.beam) && (!n.tone || lean.tone) &&
                          (!n.displace || lean.displace) && (!n.motion || lean.motion) && (!n.abl || lean.probe);
    PassPlan p;
    p.mean_probe = n.abl;
    if (lean.usable && !rich_need && !larger_than_reference && lean_has) {
        p.cls = FrameClass::Lean60;
        p.output = (n.motion || n.displace) ? Program::out_lean_moving : Program::out_lean_still;
        return p;
    }
    p.cls = FrameClass::Rich30;
    if (n.sharp || n.guns || n.chroma || raster) {
        p.signal = n.guns ? (n.chroma ? Program::sig_guns_chroma : Program::sig_guns)
                          : (n.chroma ? Program::sig_sharp_chroma : Program::sig_sharp);
        p.signal_half = n.chroma || raster;
    }
    p.lines = raster;
    p.quarter = n.glow || n.chroma;
    p.glow = n.glow;
    p.afterglow = n.afterglow;
    const bool ag = n.afterglow;
    if (n.raster_only)
        p.output = ag ? Program::out_rich_raster_only_ag : Program::out_rich_raster_only;
    else if (raster)
        p.output = ag ? Program::out_rich_raster_ag : Program::out_rich_raster;
    else if (p.signal)
        p.output = ag ? Program::out_rich_signal_ag : Program::out_rich_signal;
    else
        p.output = ag ? Program::out_rich_scene_ag : Program::out_rich_scene;
    return p;
}

// The render targets' sizes, px. Each is what its pass would use; target_bytes()
// counts only the passes the plan runs.
struct TargetSizes {
    int pic_w, pic_h, quarter_w, quarter_h, signal_w, signal_h, lines_w, lines_h;
    static constexpr int kMeanSize = 64;   // the mean probe, mipped to 1x1
};

inline TargetSizes target_sizes(const PassPlan& plan, const Picture& picture, const TubeUniforms& still) noexcept {
    TargetSizes t{};
    t.pic_w = std::max(1, static_cast<int>(std::lround(picture.w)));
    t.pic_h = std::max(1, static_cast<int>(std::lround(picture.h)));
    t.quarter_w = std::max(1, (t.pic_w + kTubeQuarterRes - 1) / kTubeQuarterRes);
    t.quarter_h = std::max(1, (t.pic_h + kTubeQuarterRes - 1) / kTubeQuarterRes);
    t.signal_w = plan.signal_half ? std::max(1, (t.pic_w + 1) / 2) : t.pic_w;
    t.signal_h = t.pic_h;
    t.lines_w = t.signal_w;
    t.lines_h = std::max(1, static_cast<int>(std::ceil(still.lines)));
    return t;
}

// Bytes of GPU memory the plan's targets hold: the glass (RGBA8), the afterglow
// history pair, the signal and lines (RGBA16F when the driver renders half floats,
// else 4 bytes), the quarter picture and the two glow targets, the mean probe's mips.
inline std::size_t target_bytes(const PassPlan& plan, const TargetSizes& t, bool half_float) noexcept {
    const auto area = [](int w, int h) { return static_cast<std::size_t>(w) * static_cast<std::size_t>(h); };
    const std::size_t signal_px = half_float ? 8 : 4;
    std::size_t bytes = area(t.pic_w, t.pic_h) * 4;
    if (plan.afterglow) bytes += 2 * area(t.pic_w, t.pic_h) * 4;
    if (plan.signal) bytes += area(t.signal_w, t.signal_h) * signal_px;
    if (plan.lines) bytes += area(t.lines_w, t.lines_h) * signal_px;
    if (plan.quarter) bytes += area(t.quarter_w, t.quarter_h) * 4;
    if (plan.glow) bytes += 2 * area(t.quarter_w, t.quarter_h) * 4;
    if (plan.mean_probe) bytes += area(TargetSizes::kMeanSize, TargetSizes::kMeanSize) * 4 * 4 / 3;
    return bytes;
}

// The plan's predicted GPU time a frame, ms: the measured price of each pass it runs
// (tube_budget.h; the signal at half width costs half_width_factor of its full-width
// price, afterglow is the net cost on top of the output) scaled by the picture's pixels
// against the reference it was measured at. The glass is rebaked only when it changes,
// so it is not a per-frame cost.
inline double predicted_ms(const PassPlan& plan, const TargetSizes& t, const PassCosts& c = kPassCosts) noexcept {
    double ms = 0.0;
    if (plan.mean_probe) ms += c.probe;
    if (plan.quarter) ms += c.quarter;
    if (plan.glow) ms += c.glow_blur;
    if (plan.signal) {
        double sig = c.sig_sharp;
        switch (*plan.signal) {
            case Program::sig_sharp_chroma: sig = c.sig_sharp_chroma; break;
            case Program::sig_guns: sig = c.sig_guns; break;
            case Program::sig_guns_chroma: sig = c.sig_guns_chroma; break;
            default: break;
        }
        ms += plan.signal_half ? sig * c.half_width_factor : sig;
    }
    if (plan.lines) ms += plan.signal_half ? c.lines_half : c.lines_full;
    const std::uint32_t caps = program_caps(plan.output);
    if (is_lean_output(plan.output))
        ms += c.out_lean;
    else if (caps & kCapRasterOnly)
        ms += c.out_rich_raster_only;
    else if (caps & kCapRaster)
        ms += c.out_rich_raster;
    else
        ms += c.out_rich;
    if (plan.afterglow) ms += c.history;
    const double pixels = static_cast<double>(t.pic_w) * static_cast<double>(t.pic_h);
    return ms * (pixels / kReferencePicturePixels);
}

// The plan for a resolved setting drawn into `picture`. A picture larger than the
// reference (a full-screen film) is always Rich30; when its predicted cost exceeds the
// rich budget it drops the afterglow pass, then (if still over) the glow passes, and
// stops there (design section 3). The driver must then draw with those rows at 0.
inline PassPlan plan_passes(const Resolved& resolved, const Picture& picture) noexcept {
    const TubeUniforms still = tube_uniforms_for(resolved, picture);
    const bool larger = picture.w * picture.h > kReferencePicturePixels;
    Needs n = needs_of(still, resolved);
    PassPlan p = plan_for(n, larger);
    if (!larger) return p;
    const auto over = [&] { return predicted_ms(p, target_sizes(p, picture, still)) > kRichBudgetMs; };
    if (n.afterglow && over()) {
        n.afterglow = false;
        p = plan_for(n, larger);
    }
    if (n.glow && over()) {
        n.glow = false;
        p = plan_for(n, larger);
    }
    return p;
}

// The still uniforms as the plan draws them: a row whose pass the plan dropped reads 0.
// Every rich output program compiles OUT_GLOW, so a GLOW left above 0 after
// plan_passes() dropped the glow pass would sample a stale glow texture. A dropped
// afterglow selects a program without OUT_HISTORY, so PERSISTENCE is not read; it is
// zeroed too, so the uniforms never claim a pass the plan does not draw (and
// settle_frames() owes no afterglow frames for it).
inline void apply_plan(TubeUniforms& u, const PassPlan& plan) noexcept {
    if (!plan.glow) u.glow = 0.0f;
    if (!plan.afterglow) u.persistence = 0.0f;
}

// The programs a plan draws with: the glass first (the bake), then in pass order, each once.
inline std::vector<Program> programs_of(const PassPlan& plan) {
    std::vector<Program> out{Program::glass};
    if (plan.quarter || plan.mean_probe) out.push_back(Program::quarter);
    if (plan.glow) out.push_back(Program::blur);
    if (plan.signal) out.push_back(*plan.signal);
    if (plan.lines) out.push_back(Program::lines);
    out.push_back(plan.output);
    if (plan.afterglow) out.push_back(Program::copy_encode);
    return out;
}

// Plans that together draw every program at least once, each through its real passes
// (Task 14's validation on the hardware). The rich ones come from plan_for, so each is
// a plan a setting can reach; both lean outputs are named even if the lean set leaves
// one unreachable, because both are in the table.
inline std::vector<PassPlan> validation_plans() {
    std::vector<PassPlan> plans;
    PassPlan lean_still;
    plans.push_back(lean_still);
    PassPlan lean_moving;
    lean_moving.output = Program::out_lean_moving;
    plans.push_back(lean_moving);
    struct Rich {
        bool sharp, guns, chroma, raster, raster_only, glow, afterglow, abl;
    };
    constexpr std::array<Rich, 8> kRich{{
        {false, false, false, false, false, true, false, true},   // out_rich_scene, quarter + blur, the mean probe
        {false, false, false, false, false, true, true, false},   // out_rich_scene_ag, copy_encode
        {true, false, false, false, false, false, false, false},  // sig_sharp, out_rich_signal
        {true, false, true, false, false, false, true, false},    // sig_sharp_chroma, out_rich_signal_ag
        {false, true, false, true, false, false, false, false},   // sig_guns, lines, out_rich_raster
        {false, true, true, true, false, true, true, false},      // sig_guns_chroma, out_rich_raster_ag
        {true, false, false, true, true, false, false, false},    // out_rich_raster_only
        {false, true, true, true, true, true, true, true},        // out_rich_raster_only_ag
    }};
    for (const Rich& r : kRich) {
        Needs n;
        n.sharp = r.sharp;
        n.guns = r.guns;
        n.chroma = r.chroma;
        n.raster = r.raster;
        n.raster_only = r.raster_only;
        n.glow = r.glow;
        n.afterglow = r.afterglow;
        n.abl = r.abl;
        plans.push_back(plan_for(n, false));
    }
    return plans;
}

// The uniforms a program declares: TubeUniforms fields by their member names (the
// renderer uploads these from the frame's uniforms) and the program's own, prefixed
// `u` or a sampler's name (the renderer sets these itself). Follows the defines, so
// the lean outputs list what the lean set compiles in. Task 6 checks every name is
// declared in the program's assembled source.
inline std::vector<std::string_view> uniforms_of(Program p) {
    const ProgramInfo& info = program_info(p);
    const std::string defines = " " + program_defines(p) + " ";
    const auto has = [&defines](std::string_view d) {
        return defines.find(" " + std::string(d) + " ") != std::string::npos;
    };
    std::vector<std::string_view> out;
    const auto add = [&out](std::initializer_list<std::string_view> names) {
        for (std::string_view name : names)
            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    };
    if (info.reads_scene) add({"ui", "uSceneMap", "uSceneClamp"});
    switch (info.file) {
        case ShaderFile::glass: add({"outW", "outH", "cornerPx", "rim", "vignette", "glassBlack", "reflection"}); break;
        case ShaderFile::quarter: add({"uPicSize", "uDstSize"}); break;
        case ShaderFile::blur: add({"src", "uDir", "uSigma", "uRadius"}); break;
        case ShaderFile::copy_encode: add({"src", "uOrigin", "uSrcOffset"}); break;
        case ShaderFile::signal:
            add({"outW", "outH", "pxPerMm", "lines", "sharpness", "contrast", "saturation", "jitter", "jitterSeed",
                 "jitterBias", "trackingOffset", "trackingPosition", "trackingHalfWidth", "flagOffset", "flagExtent",
                 "uSigSize"});
            if (has("SIG_GUNS")) add({"convergence", "driftOffset"});
            if (has("SIG_CHROMA")) add({"chromaBleed", "quarter"});
            break;
        case ShaderFile::lines: add({"outW", "outH", "lines", "signalTex"}); break;
        case ShaderFile::output:
            add({"outW", "outH", "marginPx", "flatFit", "pxPerMm", "maskStrength", "maskGain", "maskPitch",
                 "brightness", "tint", "power", "degauss", "degaussPhaseA", "degaussPhaseB", "spot", "warmupGain",
                 "uOrigin", "uMaskSize", "glassTex", "maskTex"});
            if (has("OUT_SIGNAL")) add({"signalTex"});
            if (has("OUT_BEND")) add({"curvature"});
            if (has("OUT_SCANLINES")) add({"scanlines", "lines", "beamMin", "beamMax"});
            if (has("OUT_RASTER")) add({"raster", "lines", "linesTex"});
            if (has("OUT_TONE")) add({"contrast", "saturation"});
            if (has("OUT_DISPLACE"))
                add({"jitter", "jitterSeed", "jitterBias", "trackingOffset", "trackingPosition", "trackingHalfWidth",
                     "flagOffset", "flagExtent"});
            if (has("OUT_MOTION"))
                add({"noise", "noiseSeed", "flicker", "flickerWave", "hum", "humPhase", "rollAmount", "rollPosition",
                     "rollHalfWidth", "rollTint", "purityAmount", "purityX", "purityY", "purityRadius", "purityR",
                     "purityG", "purityB"});
            if (has("OUT_ABL")) add({"abl", "breathe", "ablLevel"});
            if (has("OUT_GLOW")) add({"glow", "glowTex"});
            if (has("OUT_HISTORY")) add({"persistence", "prev"});
            break;
    }
    return out;
}

}  // namespace tube
