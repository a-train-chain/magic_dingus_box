// The Tube renderer's GLSL: each program of the fixed table assembled from the
// embedded files (tube/tube_shader_sources.h, generated at configure time) and
// checked for what the renderer and the hardware rely on. CI writes the same
// assembled sources out (MDB_WRITE_TUBE_GLSL) and runs glslangValidator on them.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tube/tube_derived.h"
#include "tube/tube_driver.h"
#include "tube/tube_programs.h"
#include "tube/tube_shader_assembly.h"

using namespace tube;

namespace {

// A GLSL source as the compiler sees it after the preprocessor, for the checks
// below. HANDLES exactly the forms the Tube files use: `#ifdef NAME`,
// `#ifndef NAME`, `#if EXPR` with EXPR made of `defined(NAME)`, `!`, `&&`, `||`
// and parentheses, `#else`, `#endif`, `#define NAME ...` (records NAME),
// `#error ...` (recorded with its nesting), `#version`, `#line`, and `//`
// comments. Anything else (`#elif`, `/* */`, macro values in an `#if`, macro
// expansion in code) makes it return nullopt, so a new form fails the test
// instead of being misread.
struct Preprocessed {
    struct Error {
        std::size_t depth;  // conditional blocks around it, its own included
        bool active;        // would fire
        std::string text;
    };
    std::string code;       // the enabled lines, comments and directives removed
    std::vector<Error> errors;
};

class IfExpr {
   public:
    IfExpr(const std::string& text, const std::set<std::string>& defined) : defined_(defined) {
        for (std::size_t i = 0; i < text.size();) {
            const char ch = text[i];
            if (std::isspace(static_cast<unsigned char>(ch))) { ++i; continue; }
            if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
                std::size_t j = i;
                while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) ++j;
                tokens_.push_back(text.substr(i, j - i));
                i = j;
            } else if (text.compare(i, 2, "&&") == 0 || text.compare(i, 2, "||") == 0) {
                tokens_.push_back(text.substr(i, 2));
                i += 2;
            } else if (ch == '!' || ch == '(' || ch == ')') {
                tokens_.push_back(std::string(1, ch));
                ++i;
            } else {
                ok_ = false;
                return;
            }
        }
    }
    std::optional<bool> value() {
        if (!ok_) return std::nullopt;
        const bool v = or_expr();
        if (!ok_ || pos_ != tokens_.size()) return std::nullopt;
        return v;
    }

   private:
    bool accept(const std::string& t) {
        if (pos_ < tokens_.size() && tokens_[pos_] == t) { ++pos_; return true; }
        return false;
    }
    bool or_expr() {
        bool v = and_expr();
        while (accept("||")) v = and_expr() || v;
        return v;
    }
    bool and_expr() {
        bool v = unary();
        while (accept("&&")) v = unary() && v;
        return v;
    }
    bool unary() {
        if (accept("!")) return !unary();
        if (accept("(")) {
            const bool v = or_expr();
            if (!accept(")")) ok_ = false;
            return v;
        }
        if (accept("defined") && accept("(") && pos_ < tokens_.size()) {
            const bool v = defined_.count(tokens_[pos_++]) > 0;
            if (!accept(")")) ok_ = false;
            return v;
        }
        ok_ = false;
        return false;
    }
    const std::set<std::string>& defined_;
    std::vector<std::string> tokens_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

std::optional<Preprocessed> preprocess(const std::string& src) {
    if (src.find("/*") != std::string::npos) return std::nullopt;
    struct Block { bool parent, cond; };
    std::vector<Block> stack;
    const auto active = [&stack] { return stack.empty() || (stack.back().parent && stack.back().cond); };
    std::set<std::string> defined;
    Preprocessed out;
    std::istringstream lines{src};
    for (std::string line; std::getline(lines, line);) {
        if (const auto c = line.find("//"); c != std::string::npos) line.erase(c);
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (line[first] != '#') {
            if (active()) out.code += line + "\n";
            continue;
        }
        std::istringstream words{line.substr(first + 1)};
        std::string directive, rest;
        words >> directive;
        std::getline(words, rest);
        std::string name;
        std::istringstream{rest} >> name;
        if (directive == "ifdef" || directive == "ifndef") {
            const bool d = defined.count(name) > 0;
            stack.push_back({active(), directive == "ifdef" ? d : !d});
        } else if (directive == "if") {
            const std::optional<bool> v = IfExpr(rest, defined).value();
            if (!v) return std::nullopt;
            stack.push_back({active(), *v});
        } else if (directive == "else") {
            if (stack.empty()) return std::nullopt;
            stack.back().cond = !stack.back().cond;
        } else if (directive == "endif") {
            if (stack.empty()) return std::nullopt;
            stack.pop_back();
        } else if (directive == "define") {
            if (active()) defined.insert(name);
        } else if (directive == "error") {
            out.errors.push_back({stack.size(), active(), rest});
        } else if (directive != "version" && directive != "line") {
            return std::nullopt;
        }
    }
    if (!stack.empty()) return std::nullopt;
    return out;
}

bool is_ident_start(char ch) { return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_'; }
bool is_ident(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

// The identifiers in `text`, skipping numbers and what follows a '.' (swizzles).
std::set<std::string> identifiers(const std::string& text) {
    std::set<std::string> ids;
    for (std::size_t i = 0; i < text.size();) {
        if (is_ident_start(text[i])) {
            std::size_t j = i;
            while (j < text.size() && is_ident(text[j])) ++j;
            if (i == 0 || text[i - 1] != '.') ids.insert(text.substr(i, j - i));
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(text[i]))) {
            while (i < text.size() && (is_ident(text[i]) || text[i] == '.')) ++i;
        } else {
            ++i;
        }
    }
    return ids;
}

// The uniforms the enabled code of `pre` declares, and those of them it READS:
// the identifiers in main() and in every function main() reaches (a function
// common.glsl defines but the program never calls reads nothing). Uniform
// declarations are top-level `uniform <type> a, b, c;` statements.
struct UniformUse { std::set<std::string> declared, read; };
UniformUse uniform_use(const Preprocessed& pre) {
    UniformUse use;
    std::map<std::string, std::set<std::string>> fn_ids;
    const std::string& code = pre.code;
    std::string statement;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char ch = code[i];
        if (ch == ';') {
            std::istringstream words{statement};
            std::string kw, type;
            words >> kw >> type;
            if (kw == "uniform") {
                std::string names;
                std::getline(words, names);
                std::istringstream list{names};
                for (std::string n; std::getline(list, n, ',');) {
                    const auto ids = identifiers(n);
                    if (ids.size() == 1) use.declared.insert(*ids.begin());
                }
            }
            statement.clear();
        } else if (ch == '{') {
            const std::string name = statement.substr(0, statement.find('('));
            const auto ids = identifiers(name);
            std::size_t depth = 1, j = i + 1;
            for (; j < code.size() && depth > 0; ++j) depth += code[j] == '{' ? 1 : code[j] == '}' ? -1 : 0;
            std::string fn;
            for (std::size_t k = name.size(); k > 0 && is_ident(name[k - 1]); --k) fn.insert(fn.begin(), name[k - 1]);
            fn_ids[fn] = identifiers(code.substr(i + 1, j - i - 2));
            i = j - 1;
            statement.clear();
        } else {
            statement += ch;
        }
    }
    std::set<std::string> reached{"main"};
    std::deque<std::string> todo{"main"};
    while (!todo.empty()) {
        const std::string f = todo.front();
        todo.pop_front();
        for (const std::string& id : fn_ids[f]) {
            if (use.declared.count(id)) use.read.insert(id);
            if (fn_ids.count(id) && reached.insert(id).second) todo.push_back(id);
        }
    }
    return use;
}

bool listed(const std::vector<std::string_view>& names, const std::string& n) {
    for (std::string_view u : names)
        if (u == n) return true;
    return false;
}

std::string without_define(std::string src, const std::string& d) {
    const std::string line = "#define " + d + " 1\n";
    const auto at = src.find(line);
    if (at != std::string::npos) src.erase(at, line.size());
    return src;
}

}  // namespace

TEST_CASE("tube shaders: every program assembles with its defines first", "[tube][tube_shaders]") {
    for (const auto& info : kPrograms) for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
        const std::string src = assemble_fragment(info.id, decode); INFO(info.name);
        REQUIRE(src.rfind("#version 300 es\n", 0) == 0);
        REQUIRE(src.find("#version", 1) == std::string::npos);
        std::istringstream words{program_defines(info.id)}; std::string d;
        while (words >> d) REQUIRE(src.find("#define " + d + " 1\n") != std::string::npos);
        const bool hw = decode == SceneDecode::Hardware && info.reads_scene;
        REQUIRE((src.find("#define SCENE_HW_DECODE 1\n") != std::string::npos) == hw);
        REQUIRE(src.find("TUBE_MASK_TYPE") == std::string::npos);      // the mask is data
        REQUIRE(src.find("OUT_MESH") == std::string::npos);            // the bend is per pixel
        REQUIRE(src.find("TUBE_GLSL_TABLE_V1") != std::string::npos);  // the binary's marker
    }
    REQUIRE(assemble_vertex().rfind("#version 300 es\n", 0) == 0);
}
TEST_CASE("tube shaders: crt.metal's flash-safety caps are in the GLSL", "[tube][tube_shaders]") {
    const std::string out = assemble_fragment(Program::out_rich_scene, SceneDecode::Hardware);
    REQUIRE(out.find("min(noise, 0.2)") != std::string::npos);                 // NOISE: +-1 % + grain
    REQUIRE(out.find("min(flicker, 1.0) * 0.025") != std::string::npos);       // FLICKER: +-2.5 %
    // crt.metal computes the flicker wave in the shader (|wave| <= 1); here it is a
    // uniform, so the shader bounds it itself or the cap would trust the upload.
    REQUIRE(out.find("min(flicker, 1.0) * 0.025 * clamp(flickerWave, -1.0, 1.0)") != std::string::npos);
    REQUIRE(out.find("min(hum, 1.0) * 0.25") != std::string::npos);            // HUM: 25 % darker at most
    REQUIRE(out.find("min(jitter, 1.0) * 0.50 * mm") != std::string::npos);    // JITTER: +-0.25 mm a line
    // POWER-OFF's brightening. Its factor 1 + 2.5 * (1 - max(smoothstep(...), 0.003)) is
    // at most 3.4925 by construction, but it is worked out on the CPU (tube_derived.h
    // dPowerGain), so the shader bounds it itself: 3.5 is just above the largest real
    // value. Without it a bad upload could brighten the whole picture without limit.
    REQUIRE(out.find("col *= min(dPowerGain, 3.5);") != std::string::npos);
    REQUIRE(assemble_fragment(Program::sig_sharp, SceneDecode::Hardware).find("min(jitter, 1.0) * 0.50 * mm") != std::string::npos);
}
TEST_CASE("tube shaders: each program declares the uniforms the renderer uploads", "[tube][tube_shaders]") {
    for (const auto& info : kPrograms) {
        const std::string src = assemble_fragment(info.id, SceneDecode::Hardware);
        for (std::string_view u : uniforms_of(info.id)) {
            INFO(info.name << " " << u);
            const std::regex decl("uniform\\s+\\w+\\s+[^;]*\\b" + std::string(u) + "\\b");
            REQUIRE(std::regex_search(src, decl));
        }
    }
}
// Every uniform the enabled code of a program READS is in uniforms_of (the
// renderer uploads only what that lists, so a missing name is a silently wrong
// picture). Through preprocess() above: covers the forms it handles, the
// reachability from main(), and both directions of a block only as far as
// "read implies listed" (a listed name the code never reads is allowed:
// maskStrength and maskPitch in the outputs, outW in lines, read by nothing but
// uploaded harmlessly). Does NOT cover the vertex shader (no uniforms).
TEST_CASE("tube shaders: every uniform a program reads is one the renderer uploads", "[tube][tube_shaders]") {
    for (const auto& info : kPrograms) {
        for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
            INFO(info.name);
            const auto pre = preprocess(assemble_fragment(info.id, decode));
            REQUIRE(pre.has_value());
            for (const auto& e : pre->errors) {
                INFO(e.text);
                REQUIRE_FALSE(e.active);
            }
            const UniformUse use = uniform_use(*pre);
            REQUIRE_FALSE(use.read.empty());
            const auto names = uniforms_of(info.id);
            for (const std::string& u : use.read) {
                INFO(u);
                REQUIRE(listed(names, u));
            }
        }
    }
}
// The same with each of a program's defines taken away in turn (skipping the
// sets the files' own #error guards refuse): a name one define's block reads
// must be listed by that block, not only by another block every table row
// happens to carry with it (OUT_DISPLACE reads `lines`, which OUT_SCANLINES
// also lists).
TEST_CASE("tube shaders: each define's block lists the uniforms it reads", "[tube][tube_shaders]") {
    int checked = 0;
    for (const auto& info : kPrograms) {
        const std::string all = program_defines(info.id);
        std::istringstream words{all};
        for (std::string drop; words >> drop;) {
            INFO(info.name << " without " << drop);
            const auto pre = preprocess(without_define(assemble_fragment(info.id, SceneDecode::Hardware), drop));
            REQUIRE(pre.has_value());
            bool refused = false;
            for (const auto& e : pre->errors) refused = refused || e.active;
            if (refused) continue;
            std::string rest;
            std::istringstream again{all};
            for (std::string d; again >> d;)
                if (d != drop) rest += (rest.empty() ? "" : " ") + d;
            const auto names = detail::uniforms_for(info.file, info.reads_scene, rest);
            for (const std::string& u : uniform_use(*pre).read) {
                INFO(u);
                REQUIRE(listed(names, u));
            }
            ++checked;
        }
    }
    REQUIRE(checked > 50);
}
// DRIVER HAZARD (docs/engineering/measurements/2026-11-15-tube-hang-isolation.txt):
// the Pi 5's compiler hung on the signal program with chroma bleed and convergence
// but without the sharpness path. Checks, for every signal program and both scene
// decodes: SIG_SHARP is defined in the assembled source; the sharpness blend is in
// the code the preprocessor keeps (not merely in the file's text); and the two
// #error guards stand at the top level of the file (each inside only its own #if,
// not in a block that could be switched off) and do not fire.
TEST_CASE("tube shaders: every signal program compiles the sharpness path", "[tube][tube_shaders]") {
    int signal_programs = 0;
    for (const auto& info : kPrograms) {
        if (info.file != ShaderFile::signal) continue;
        ++signal_programs;
        for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
            INFO(info.name);
            const std::string src = assemble_fragment(info.id, decode);
            REQUIRE(src.find("#define SIG_SHARP 1\n") != std::string::npos);
            const auto pre = preprocess(src);
            REQUIRE(pre.has_value());
            REQUIRE(pre->code.find("(l + 2.0 * c + r) * 0.25") != std::string::npos);
            int guards = 0;
            for (const auto& e : pre->errors)
                if (e.text.find("SIG_SHARP") != std::string::npos && e.depth == 1 && !e.active) ++guards;
            REQUIRE(guards == 2);
        }
    }
    REQUIRE(signal_programs == 4);
}
// GLSL ES limits the source character set; a stray non-ASCII character (a dash
// or a plus-minus pasted into a comment) is a compile error on some drivers.
TEST_CASE("tube shaders: every file is plain ASCII", "[tube][tube_shaders]") {
    for (std::string_view file : {glsl::common, glsl::fullscreen_vert, glsl::glass, glsl::quarter, glsl::blur,
                                  glsl::copy_encode, glsl::signal, glsl::lines, glsl::output}) {
        REQUIRE_FALSE(file.empty());
        for (char ch : file) {
            const auto byte = static_cast<unsigned char>(ch);
            REQUIRE((byte == '\n' || (byte >= 0x20 && byte < 0x7f)));
        }
    }
}
TEST_CASE("tube shaders: written out for the validator when asked", "[tube][tube_shaders]") {
    const char* dir = std::getenv("MDB_WRITE_TUBE_GLSL");
    if (dir == nullptr || *dir == '\0') return;
    std::filesystem::create_directories(dir); int n = 0;
    for (const auto& info : kPrograms) for (auto decode : {SceneDecode::Hardware, SceneDecode::Shader}) {
        if (decode == SceneDecode::Shader && !info.reads_scene) continue;
        std::ofstream(std::string(dir) + "/" + std::string(info.name) + (decode == SceneDecode::Hardware ? ".hw" : ".sw") + ".frag")
            << assemble_fragment(info.id, decode);
        ++n;
    }
    std::ofstream(std::string(dir) + "/fullscreen.vert") << assemble_vertex();
    REQUIRE(n == 28);
}

// Real uniform sets for scripts/dev/tube_shaderstat (the on-Pi compile/compare tool),
// written only when asked: for each look at a 1440x1080 Modern TV picture (1920x1080
// framebuffer, picture at x 240), what FrameDriver::frame uploads at a mid-run moment
// (warm-up over, phases non-zero, the limiter fed a mean) - `<look>.txt` - and the same
// with a degauss 0.4 s in - `<look>.degauss.txt`. `<look>.synthetic.txt` is NOT what
// the driver uploads (it has no power-off animation yet, and no look moves the band,
// purity, tracking or flagging rows): the mid-run set with power 0.6, spot 0.3,
// warmupGain 0.7 and those character blocks on, so a comparison reaches every branch.
// Each file: every TubeUniforms field (`tint` as three values), every TubeDerived
// field (tube_derived.h, as the renderer works them out this frame), the renderer's
// own uniforms at this picture, including those the programs read before the derived
// values (so a file drives the old source and the new one alike): uSceneMap,
// uSceneClamp, uMaskSize, uSigSize, uPicSize/uDstSize (the quarter pass), uDir, uSigma
// and uRadius (the horizontal glow blur); uOrigin depends on the program's target and
// the tool derives it. Directives: @fb, @pic, @mask (the look's mask tile), and
// @sig, @lines, @quarter (the plan's signal, lines and quarter target sizes).
TEST_CASE("tube shaders: real uniform sets written out when asked", "[tube][tube_shaders]") {
    const char* dir = std::getenv("MDB_WRITE_TUBE_UNIFORMS");
    if (dir == nullptr || *dir == '\0') return;
    std::filesystem::create_directories(dir);
    using TU = TubeUniforms;
    static constexpr std::pair<const char*, float TU::*> kFields[] = {
        {"outW", &TU::outW}, {"outH", &TU::outH}, {"pxPerMm", &TU::pxPerMm}, {"curvature", &TU::curvature},
        {"cornerPx", &TU::cornerPx}, {"marginPx", &TU::marginPx}, {"flatFit", &TU::flatFit},
        {"scanlines", &TU::scanlines}, {"lines", &TU::lines}, {"raster", &TU::raster}, {"beamMin", &TU::beamMin},
        {"beamMax", &TU::beamMax}, {"maskType", &TU::maskType}, {"maskStrength", &TU::maskStrength},
        {"maskGain", &TU::maskGain}, {"maskPitch", &TU::maskPitch}, {"glow", &TU::glow},
        {"sharpness", &TU::sharpness}, {"brightness", &TU::brightness}, {"contrast", &TU::contrast},
        {"saturation", &TU::saturation}, {"convergence", &TU::convergence}, {"persistence", &TU::persistence},
        {"flicker", &TU::flicker}, {"hum", &TU::hum}, {"vignette", &TU::vignette}, {"rim", &TU::rim},
        {"noise", &TU::noise}, {"jitter", &TU::jitter}, {"glassBlack", &TU::glassBlack},
        {"reflection", &TU::reflection}, {"abl", &TU::abl}, {"breathe", &TU::breathe},
        {"chromaBleed", &TU::chromaBleed}, {"humPhase", &TU::humPhase}, {"flickerWave", &TU::flickerWave},
        {"noiseSeed", &TU::noiseSeed}, {"jitterSeed", &TU::jitterSeed}, {"degaussPhaseA", &TU::degaussPhaseA},
        {"degaussPhaseB", &TU::degaussPhaseB}, {"jitterBias", &TU::jitterBias}, {"ablLevel", &TU::ablLevel},
        {"power", &TU::power}, {"degauss", &TU::degauss}, {"spot", &TU::spot}, {"warmupGain", &TU::warmupGain},
        {"rollAmount", &TU::rollAmount}, {"rollPosition", &TU::rollPosition},
        {"rollHalfWidth", &TU::rollHalfWidth}, {"rollTint", &TU::rollTint},
        {"trackingOffset", &TU::trackingOffset}, {"trackingPosition", &TU::trackingPosition},
        {"trackingHalfWidth", &TU::trackingHalfWidth}, {"driftOffset", &TU::driftOffset},
        {"flagOffset", &TU::flagOffset}, {"flagExtent", &TU::flagExtent}, {"purityAmount", &TU::purityAmount},
        {"purityX", &TU::purityX}, {"purityY", &TU::purityY}, {"purityRadius", &TU::purityRadius},
        {"purityR", &TU::purityR}, {"purityG", &TU::purityG}, {"purityB", &TU::purityB},
    };
    static_assert(sizeof(TU) == (std::size(kFields) + 3) * sizeof(float), "a TubeUniforms field is missing here");
    constexpr int kFbW = 1920, kFbH = 1080, kPicX = 240, kPicY = 0, kPicW = 1440, kPicH = 1080;
    const Picture picture{kPicW, kPicH, false};
    const auto write = [&](const std::string& path, const TU& u, const DriverFrame& first) {
        const TargetSizes& t = first.sizes;
        std::ofstream out(path);
        out.precision(9);
        out << "# " << path.substr(path.find_last_of('/') + 1) << ": output " << program_info(first.plan.output).name
            << "\n@fb=" << kFbW << "," << kFbH << "\n@pic=" << kPicX << "," << kPicY << "," << kPicW << "," << kPicH
            << "\n@mask=" << first.mask.w << "," << first.mask.h << ",";
        for (std::uint8_t b : first.mask.rgb) {
            static constexpr char kHex[] = "0123456789abcdef";
            out << kHex[b >> 4] << kHex[b & 15];
        }
        out << "\n@sig=" << t.signal_w << "," << t.signal_h << "\n@lines=" << t.lines_w << "," << t.lines_h
            << "\n@quarter=" << t.quarter_w << "," << t.quarter_h << "\n";
        for (const auto& [name, member] : kFields) out << name << "=" << u.*member << "\n";
        const TubeDerived d = tube_derived(u, DerivedSizes{first.mask.w, first.mask.h, t.signal_w, t.signal_h});
        for (const DerivedField& f : kDerivedFields) out << f.name << "=" << d.*(f.member) << "\n";
        const float sigma = std::max(first.glow_sigma, 0.1f);   // tube_renderer_passes.cpp's blur uniforms
        out << "uSigSize=" << t.signal_w << "," << t.signal_h << "\nuPicSize=" << kPicW << "," << kPicH
            << "\nuDstSize=" << t.quarter_w << "," << t.quarter_h << "\nuDir=1,0\nuSigma=" << sigma
            << "\nuRadius=" << std::min(static_cast<int>(std::ceil(3.0f * sigma)), 24) << "\n";
        out << "tint=" << u.tint[0] << "," << u.tint[1] << "," << u.tint[2] << "\n";
        const float W = kFbW, H = kFbH, x = kPicX, y = kPicY, w = kPicW, h = kPicH;   // scene_uniforms()
        out << "uMaskSize=" << first.mask.w << "," << first.mask.h << "\nuSceneMap=" << x / W << "," << (H - y) / H
            << "," << w / W << "," << h / H << "\nuSceneClamp=" << (x + 0.5f) / W << "," << (H - y - h + 0.5f) / H
            << "," << (x + w - 0.5f) / W << "," << (H - y - 0.5f) / H << "\n";
    };
    int n = 0;
    for (const Look look : kLooks) {
        TubeSetting setting;
        setting.look = look;
        // From t = 100 s at 30 fps; a mean of 0.35 fed while the plan has the probe.
        const auto run = [&](double until, double degauss_at, DriverFrame& first) {
            FrameDriver d;
            DriverFrame f;
            bool have_first = false;
            std::uint64_t requests = 0;
            for (double t = 100.0; t <= until + 1e-9; t += 1.0 / 30.0) {
                if (t >= degauss_at) requests = 1;
                f = d.frame(setting, picture, t, requests);
                if (!have_first) first = f, have_first = true;
                d.on_mean(0.35);
            }
            return f;
        };
        DriverFrame first;
        const DriverFrame mid = run(106.4, 1e9, first);
        REQUIRE(mid.u.warmupGain == 1.0f);
        REQUIRE(mid.u.degauss == 0.0f);
        REQUIRE(first.mask_changed);
        const std::string base = std::string(dir) + "/" + std::string(look_name(look));
        write(base + ".txt", mid.u, first);
        DriverFrame first_d;
        const DriverFrame deg = run(106.4, 106.0, first_d);
        REQUIRE(deg.u.degauss > 0.5f);
        REQUIRE(deg.u.degauss < 1.0f);
        write(base + ".degauss.txt", deg.u, first_d);
        TU syn = mid.u;
        syn.power = 0.6f;
        syn.spot = 0.3f;
        syn.warmupGain = 0.7f;
        syn.rollAmount = 1.2f, syn.rollPosition = 0.4f, syn.rollHalfWidth = 0.1f, syn.rollTint = 0.5f;
        syn.purityAmount = 0.8f, syn.purityX = 0.3f, syn.purityY = 0.6f, syn.purityRadius = 40.0f;
        syn.purityR = 0.9f, syn.purityG = 0.2f, syn.purityB = 0.5f;
        syn.trackingOffset = 5.0f, syn.trackingPosition = 0.5f, syn.trackingHalfWidth = 0.05f;
        syn.flagOffset = 6.0f, syn.flagExtent = 0.2f;
        write(base + ".synthetic.txt", syn, first);
        n += 3;
    }
    REQUIRE(n == 18);
}
