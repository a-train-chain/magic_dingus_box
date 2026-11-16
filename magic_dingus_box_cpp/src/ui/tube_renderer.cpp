// tube_renderer.cpp - TubeRenderer's resources: capabilities, the program cache built
// under the build guard, the render targets, the scene texture's storage, the mask and
// the glass bake. The frame's passes are tube_renderer_passes.cpp (Task 11).
//
// Ported from the Phase 0 spike (tube_spike.cpp: log_gl_info, build, first_use,
// ensure_tex, free_tex, begin_scene, scene_decode, ensure_glass, ensure_mask), which ran
// on the Pi 5. What differs, and why, is said where it happens.
//
// KIOSK-ONLY (GL): verified by the Pi-userspace container compile (pisim.sh check), CI's
// shader validation of the same assembled sources, and on the hardware.

#include "tube_renderer.h"
#include "tube_renderer_internal.h"

#include <EGL/egl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "../utils/logger.h"

namespace ui {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

const char* format_name(GLenum f) {
    switch (f) {
        case GL_SRGB8_ALPHA8: return "SRGB8_ALPHA8";
        case GL_RGBA8: return "RGBA8";
        case GL_RGBA16F: return "RGBA16F";
        default: return "?";
    }
}

const char* decode_name(tube::SceneDecode d) { return d == tube::SceneDecode::Hardware ? "hardware" : "shader"; }

// One stage from one assembled string (tube_shader_assembly.h already put the #line
// directives in). Zero on failure, the info log logged.
GLuint compile_stage(GLenum type, const std::string& source, std::string_view program, const char* stage) {
    const GLuint sh = glCreateShader(type);
    const char* text = source.c_str();
    const auto length = static_cast<GLint>(source.size());
    glShaderSource(sh, 1, &text, &length);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok) return sh;
    GLint len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    std::string log(len > 1 ? static_cast<std::size_t>(len) : 1, '\0');
    if (len > 1) glGetShaderInfoLog(sh, len, nullptr, &log[0]);
    LOG_ERROR("tube: program {} {} shader FAILED to compile (source string 1 = common.glsl, 2 = the program's "
              "file):\n{}",
              program, stage, log);
    glDeleteShader(sh);
    return 0;
}

template <std::size_t N>
std::optional<std::size_t> index_in(const std::array<std::string_view, N>& names, std::string_view name) {
    for (std::size_t i = 0; i < N; ++i)
        if (names[i] == name) return i;
    return std::nullopt;
}

}  // namespace

// ---- TubeGlState ---------------------------------------------------------------------

TubeGlState::TubeGlState(bool have_srgb_switch) : srgb_switch_(have_srgb_switch) {
    if (srgb_switch_) framebuffer_srgb_ = glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT) == GL_TRUE;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo_);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo_);
    glGetIntegerv(GL_VIEWPORT, viewport_);
    scissor_ = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box_);
    blend_ = glIsEnabled(GL_BLEND) == GL_TRUE;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb_);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb_);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a_);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a_);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb_);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_a_);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_unit_);
    for (int u = 0; u < kTubeTextureUnits; ++u) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + u));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d_[u]);
    }
    glActiveTexture(static_cast<GLenum>(active_unit_));
    glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer_);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer_);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer_);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment_);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length_);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows_);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels_);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment_);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length_);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows_);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels_);
}

void TubeGlState::decode_scene(GLuint scene_tex) {
    if (scene_tex == 0) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);
    decoded_scene_ = scene_tex;
}

TubeGlState::~TubeGlState() {
    // The scene back to raw bytes first (it needs a unit; the bindings are restored after).
    if (decoded_scene_ != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, decoded_scene_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
    }
    for (int u = 0; u < kTubeTextureUnits; ++u) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + u));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex2d_[u]));
    }
    glActiveTexture(static_cast<GLenum>(active_unit_));
    glUseProgram(static_cast<GLuint>(program_));
    glBindVertexArray(static_cast<GLuint>(vao_));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer_));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer_));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_buffer_));
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length_);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows_);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels_);
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment_);
    glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length_);
    glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows_);
    glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo_));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo_));
    glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
    glScissor(scissor_box_[0], scissor_box_[1], scissor_box_[2], scissor_box_[3]);
    if (scissor_) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glBlendFuncSeparate(static_cast<GLenum>(blend_src_rgb_), static_cast<GLenum>(blend_dst_rgb_),
                        static_cast<GLenum>(blend_src_a_), static_cast<GLenum>(blend_dst_a_));
    glBlendEquationSeparate(static_cast<GLenum>(blend_eq_rgb_), static_cast<GLenum>(blend_eq_a_));
    if (blend_) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glClearColor(clear_[0], clear_[1], clear_[2], clear_[3]);
    if (srgb_switch_) {
        if (framebuffer_srgb_) glEnable(GL_FRAMEBUFFER_SRGB_EXT); else glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }
}

// ---- Impl ------------------------------------------------------------------------------

TubeRenderer::Impl::Impl(std::string guard_path) : guard(std::move(guard_path)) {
    if (guard.tripped())
        LOG_ERROR("tube: the last shader build never finished (marker names: {}); Tube stays off until the box "
                  "reboots",
                  guard.tripped_program());
    first_draw_ms.fill(-1.0);
    if (const char* v = std::getenv("MDB_TUBE_GPU_TIMING")) gpu_timing = std::string_view(v) == "1";
    if (gpu_timing)
        LOG_INFO("tube: MDB_TUBE_GPU_TIMING=1 - per-pass GPU timers on (debug; they cost frames: never in production)");
#ifdef MDB_TUBE_PREVIEW
    if (const char* v = std::getenv("MDB_TUBE_FORCE_HANG")) force_hang = v;
    if (!force_hang.empty()) LOG_WARN("tube: MDB_TUBE_FORCE_HANG={} (preview build): that build will hang", force_hang);
#endif
}

// Spike log_gl_info: glGetStringi over GL_NUM_EXTENSIONS (ES 3.0's way; exact names, no
// substring matches inside the one long GL_EXTENSIONS string).
void TubeRenderer::Impl::probe_caps() {
    caps = Caps{};
    caps_probed = true;
    begin_query = nullptr;
    end_query = nullptr;
    get_query_ui64 = nullptr;
    const auto str = [](GLenum e) {
        const GLubyte* s = glGetString(e);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string("?");
    };
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    bool timer_ext = false;
    for (GLint i = 0; i < n; ++i) {
        const auto* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (!e) continue;
        const std::string_view ext(e);
        if (ext == "GL_EXT_sRGB_write_control") caps.srgb_write_control = true;
        if (ext == "GL_EXT_texture_sRGB_decode") caps.srgb_decode = true;
        if (ext == "GL_EXT_color_buffer_half_float" || ext == "GL_EXT_color_buffer_float")
            caps.half_float_targets = true;
        if (ext == "GL_EXT_disjoint_timer_query") timer_ext = true;
    }
    if (timer_ext) {
        begin_query = reinterpret_cast<PfnBeginQuery>(eglGetProcAddress("glBeginQueryEXT"));
        end_query = reinterpret_cast<PfnEndQuery>(eglGetProcAddress("glEndQueryEXT"));
        get_query_ui64 = reinterpret_cast<PfnGetQueryObjectui64>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
        caps.timer_query = begin_query && end_query && get_query_ui64;
    }
    LOG_INFO("tube: GL_RENDERER={} GL_VERSION={}; sRGB write control {} / sRGB decode {} / half-float targets {} / "
             "GPU timers {} -> scene decode {}",
             str(GL_RENDERER), str(GL_VERSION), caps.srgb_write_control, caps.srgb_decode, caps.half_float_targets,
             caps.timer_query, decode_name(scene_decode_for(caps)));
}

void TubeRenderer::Impl::free_program(tube::Program p) {
    ProgramSlot& s = slot(p);
    if (s.id) glDeleteProgram(s.id);
    s = ProgramSlot{};
}

// Spike build(), minus its draw-time watchdog (the guard's marker covers the first draw)
// and with every uniform location resolved once (the uniform tables).
bool TubeRenderer::Impl::build(tube::Program p, tube::SceneDecode decode) {
    const tube::ProgramInfo& info = tube::program_info(p);
    const auto t0 = Clock::now();
    const GLuint vs = compile_stage(GL_VERTEX_SHADER, tube::assemble_vertex(), info.name, "vertex");
    const GLuint fs = compile_stage(GL_FRAGMENT_SHADER, tube::assemble_fragment(p, decode), info.name, "fragment");
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 1 ? static_cast<std::size_t>(len) : 1, '\0');
        if (len > 1) glGetProgramInfoLog(prog, len, nullptr, &log[0]);
        LOG_ERROR("tube: program {} FAILED to link:\n{}", info.name, log);
        glDeleteProgram(prog);
        return false;
    }
    const double ms = ms_since(t0);

    ProgramSlot s;
    s.id = prog;
    s.built_for = decode;
    s.own.fill(-1);
    glUseProgram(prog);   // for the sampler units; the caller's TubeGlState restores the program
    for (const std::string_view name : tube::uniforms_of(p)) {
        const std::string cname(name);
        const GLint loc = glGetUniformLocation(prog, cname.c_str());
        bool known = false;
        for (std::size_t f = 0; f < kTubeFieldCount && !known; ++f)
            if (name == kTubeFields[f].name) {
                known = true;
                if (loc >= 0) s.uploads.push_back({static_cast<std::uint8_t>(f), loc});
            }
        if (!known && name == "tint") {
            known = true;
            s.tint = loc;
        }
        if (!known)
            if (const auto i = index_in(kOwnUniformNames, name)) {
                known = true;
                s.own[*i] = loc;
            }
        for (const SamplerUnit& su : kSamplerUnits)
            if (!known && name == su.name) {
                known = true;
                if (loc >= 0) glUniform1i(loc, su.unit);
            }
        if (!known) LOG_WARN("tube: program {} lists uniform {} that no table here knows (not uploaded)", info.name, name);
    }
    programs[static_cast<std::size_t>(p)] = std::move(s);
    first_draw_ms[static_cast<std::size_t>(p)] = -1.0;
    LOG_INFO("tube: program {} compiled+linked in {:.1f} ms (scene decode {})", info.name, ms, decode_name(decode));
    return true;
}

std::string TubeRenderer::Impl::undrawn_of(const tube::PassPlan& plan) const {
    std::string names;
    for (const tube::Program p : tube::programs_of(plan)) {
        const ProgramSlot& s = programs[static_cast<std::size_t>(p)];
        if (s.id != 0 && s.drawn) continue;
        if (!names.empty()) names += ' ';
        names += tube::program_info(p).name;
    }
    return names;
}

void TubeRenderer::Impl::write_marker(const std::string& names) {
    // A tripped guard's marker is evidence of a hang: never rewritten or removed here.
    if (guard.tripped() || names == marker_text) return;
    if (names.empty())
        guard.end();
    else
        guard.begin(names);
    marker_text = names;
}

// Never leaves a marker once a failure is recorded: record_failure() removed it (a
// failure is not a hang), and prepare() refuses the failed plan, so no later frame would
// draw the programs and remove it again - a marker left behind would read as a hang at
// the next unrelated restart and lock Tube off until reboot. The same when the frame stood
// down without a failure (prepared_ok cleared: the caller handed composite() something it
// cannot draw): the next prepare() takes its full path and writes the marker again.
void TubeRenderer::Impl::settle_marker() {
    if (failed || !current_plan || !prepared_ok) {
        write_marker("");
        return;
    }
    write_marker(undrawn_of(*current_plan));
}

void TubeRenderer::Impl::record_failure(const char* what) {
    failed = true;
    prepared_ok = false;
    failed_plan = current_plan;
    failed_decode = current_decode;
    write_marker("");   // a failure is not a hang
    LOG_ERROR("tube: {}; Tube is unavailable until the plan changes or the engine is chosen again", what);
}

GLenum TubeRenderer::Impl::signal_format() const { return caps.half_float_targets ? GL_RGBA16F : GL_SRGB8_ALPHA8; }

void TubeRenderer::Impl::free_target(Target& t) {
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    t = Target{};
}

void TubeRenderer::Impl::free_targets() {
    for (Target* t : {&quarter, &glow_tmp, &glow_out, &history[0], &history[1], &signal, &lines, &glass, &mean})
        free_target(*t);
    history_index = 0;
    glass_valid = false;
}

// Spike ensure_tex: immutable storage, LINEAR + CLAMP_TO_EDGE (the mean probe's mips:
// LINEAR_MIPMAP_NEAREST), an FBO on level 0, cleared to black (the history is read before
// it is written). Binds on the ACTIVE unit and the FBO: the caller's TubeGlState restores.
bool TubeRenderer::Impl::make_target(Target& t, const char* name, int w, int h, GLenum fmt, int levels) {
    const auto t0 = Clock::now();
    free_target(t);
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexStorage2D(GL_TEXTURE_2D, levels, fmt, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("tube: target {} {}x{} {} incomplete (0x{:x})", name, w, h, format_name(fmt), status);
        free_target(t);
        return false;
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    t.w = w;
    t.h = h;
    t.levels = levels;
    t.fmt = fmt;
    LOG_INFO("tube: target {} {}x{} {} made in {:.2f} ms", name, w, h, format_name(fmt), ms_since(t0));
    return true;
}

bool TubeRenderer::Impl::ensure_targets(const tube::PassPlan& plan, const tube::TargetSizes& t) {
    const GLenum sig = signal_format();
    constexpr int kMeanLevels = 7;   // 64, 32, ... 1
    struct Need {
        Target& target;
        const char* name;
        bool need;
        int w, h;
        GLenum fmt;
        int levels;
    };
    // An incomplete target - an RGBA16F one included - is a build failure for this plan, as
    // the brief has it; it is not retried with the SRGB8_ALPHA8 fallback format.
    // Formats (colour space, binding): the quarter, glow, history and mean targets are
    // SRGB8_ALPHA8 holding LINEAR light - the passes drawing into them run with
    // FRAMEBUFFER_SRGB on, so the hardware encodes on write and decodes on read. Signal and
    // lines are RGBA16F (linear, no encode) or, without half-float targets, SRGB8_ALPHA8
    // like the others (signal.frag: "an sRGB target that encodes"). The glass is RGBA8
    // holding linear light x kGlassLightScale (glass.frag), never encoded.
    Need needs[] = {
        {glass, "glass", true, t.pic_w, t.pic_h, GL_RGBA8, 1},
        {history[0], "history0", plan.afterglow, t.pic_w, t.pic_h, GL_SRGB8_ALPHA8, 1},
        {history[1], "history1", plan.afterglow, t.pic_w, t.pic_h, GL_SRGB8_ALPHA8, 1},
        {signal, "signal", plan.signal.has_value(), t.signal_w, t.signal_h, sig, 1},
        {lines, "lines", plan.lines, t.lines_w, t.lines_h, sig, 1},
        {quarter, "quarter", plan.quarter, t.quarter_w, t.quarter_h, GL_SRGB8_ALPHA8, 1},
        {glow_tmp, "glow_tmp", plan.glow, t.quarter_w, t.quarter_h, GL_SRGB8_ALPHA8, 1},
        {glow_out, "glow_out", plan.glow, t.quarter_w, t.quarter_h, GL_SRGB8_ALPHA8, 1},
        {mean, "mean", plan.mean_probe, tube::TargetSizes::kMeanSize, tube::TargetSizes::kMeanSize, GL_SRGB8_ALPHA8,
         kMeanLevels},
    };
    for (Need& n : needs) {
        Target& tg = n.target;
        if (!n.need) {
            free_target(tg);
            continue;
        }
        if (tg.made() && tg.w == n.w && tg.h == n.h && tg.fmt == n.fmt && tg.levels == n.levels) continue;
        if (&tg == &glass) glass_valid = false;
        if (&tg == &history[0] || &tg == &history[1]) history_index = 0;
        if (!make_target(tg, n.name, n.w, n.h, n.fmt, n.levels)) {
            record_failure("a render target could not be made (incomplete framebuffer)");
            return false;
        }
    }
    return true;
}

// Spike ensure_mask, from the driver's tile. The tile is RGB, three bytes a texel, rows
// counted DOWN from the top; it is widened to RGBA here, because ES 3.0 has no RGB-data
// path into an RGBA8 texture. Rows go up in the order the tile stores them, top row first:
// GL's row 0 is then the tile's top row (v = 0..1/h), which is what output.frag reads -
// `texture(maskTex, (floor(p) + 0.5) / uMaskSize)` with p TOP-DOWN (p.y = outH -
// gl_FragCoord.y), so p.y in [0, 1) lands on row 0, the top. No flip.
void TubeRenderer::Impl::upload_mask(const tube::MaskTile& tile) {
    const std::size_t texels = static_cast<std::size_t>(std::max(tile.w, 0)) * static_cast<std::size_t>(std::max(tile.h, 0));
    if (tile.w <= 0 || tile.h <= 0 || tile.rgb.size() != texels * 3) {
        // A missing or stale mask samples black or wrong: not a picture to show.
        LOG_ERROR("tube: mask tile {}x{} with {} bytes - not uploaded", tile.w, tile.h, tile.rgb.size());
        record_failure("the mask tile was malformed");
        return;
    }
    std::vector<std::uint8_t> rgba(texels * 4);
    for (std::size_t i = 0; i < texels; ++i) {
        rgba[i * 4 + 0] = tile.rgb[i * 3 + 0];
        rgba[i * 4 + 1] = tile.rgb[i * 3 + 1];
        rgba[i * 4 + 2] = tile.rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    if (!mask_tex) glGenTextures(1, &mask_tex);
    glBindTexture(GL_TEXTURE_2D, mask_tex);
    // Tightly packed rows from client memory, whatever the kiosk's unpack state was.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tile.w, tile.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    mask_w = tile.w;
    mask_h = tile.h;
    LOG_INFO("tube: mask uploaded {}x{} RGBA8", tile.w, tile.h);
}

void TubeRenderer::Impl::upload_tube_uniforms(const ProgramSlot& s, const tube::TubeUniforms& u) {
    for (const ProgramSlot::Upload& up : s.uploads) glUniform1f(up.location, u.*(kTubeFields[up.field].member));
    if (s.tint >= 0) glUniform3f(s.tint, u.tint[0], u.tint[1], u.tint[2]);
}

void TubeRenderer::Impl::bind_quad(const GlEnv& env) {
    static constexpr float kQuad[] = {
        -1.f, -1.f, 0.f, 0.f, 1.f, -1.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f,
        -1.f, -1.f, 0.f, 0.f, 1.f, 1.f,  1.f, 1.f, -1.f, 1.f, 0.f, 1.f,
    };
    glBindVertexArray(env.vao);
    glBindBuffer(GL_ARRAY_BUFFER, env.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_DYNAMIC_DRAW);
}

// Spike ensure_glass, into its own RGBA8 target at the picture's size - without the
// spike's two glFinish() calls: the glass re-bakes on every frame a glass row changes
// (live tuning), and each finish drained the queued frame before the bake. Mesa compiles a
// program's real variant synchronously inside its first draw call, so the guard's marker
// still covers that compile without a finish.
bool TubeRenderer::Impl::bake_glass(const GlEnv& env, const tube::TubeUniforms& u) {
    ProgramSlot& g = slot(tube::Program::glass);
    if (!g.id || !glass.made()) return false;
    const bool first_draw = !g.drawn;
    glBindFramebuffer(GL_FRAMEBUFFER, glass.fbo);
    glViewport(0, 0, glass.w, glass.h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(g.id);
    upload_tube_uniforms(g, u);
    bind_quad(env);
    // A query still in flight from an earlier bake is not reused (that would wait).
    const bool timed = caps.timer_query && !glass_query_pending;
    if (timed) {
        if (!glass_query) glGenQueries(1, &glass_query);
        begin_query(GL_TIME_ELAPSED_EXT, glass_query);
    }
    const auto t0 = Clock::now();
    glDrawArrays(GL_TRIANGLES, 0, 6);
    const double submit_ms = ms_since(t0);
    if (timed) {
        end_query(GL_TIME_ELAPSED_EXT);
        glass_query_pending = true;
        glass_query_first_draw = first_draw;
    }
    mark_drawn(tube::Program::glass);
    glass_valid = true;
    // The first bake after a build at INFO (it carries the compile); later ones - one a
    // frame while a glass row is being dragged - at DEBUG, so live tuning does not fill
    // the journal.
    if (first_draw)
        LOG_INFO("tube: glass bake submitted in {:.2f} ms CPU, including the glass program's first-draw compile "
                 "({}x{}, rim {:.2f} vignette {:.2f} glassBlack {:.2f} reflection {:.2f}){}",
                 submit_ms, glass.w, glass.h, u.rim, u.vignette, u.glassBlack, u.reflection,
                 timed ? "; GPU time follows" : "");
    else
        LOG_DEBUG("tube: glass bake submitted in {:.2f} ms CPU ({}x{}, rim {:.2f} vignette {:.2f} glassBlack {:.2f} "
                  "reflection {:.2f})",
                  submit_ms, glass.w, glass.h, u.rim, u.vignette, u.glassBlack, u.reflection);
    return true;
}

void TubeRenderer::Impl::poll_glass_timing() {
    if (!glass_query_pending) return;
    GLuint available = 0;
    glGetQueryObjectuiv(glass_query, GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) return;   // a later frame
    glass_query_pending = false;
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (disjoint) return;   // the GPU's clock jumped: the result means nothing
    GLuint64 ns = 0;
    get_query_ui64(glass_query, GL_QUERY_RESULT, &ns);
    // GPU time only: Mesa compiles a program's variant on the CPU before the draw is
    // recorded, so no compile is in this figure (the first bake's CPU submit line has it).
    if (glass_query_first_draw)
        LOG_INFO("tube: glass baked in {:.2f} ms GPU (first bake after a build)", static_cast<double>(ns) / 1.0e6);
    else
        LOG_DEBUG("tube: glass baked in {:.2f} ms GPU", static_cast<double>(ns) / 1.0e6);
}

void TubeRenderer::Impl::release() {
    for (std::size_t i = 0; i < programs.size(); ++i) free_program(static_cast<tube::Program>(i));
    free_targets();
    if (mask_tex) glDeleteTextures(1, &mask_tex);
    mask_tex = 0;
    mask_w = mask_h = 0;
    if (black_tex) glDeleteTextures(1, &black_tex);
    black_tex = 0;
    abandon_mean();   // the fence; a reading in flight is dropped
    mean_requested = false;
    if (mean_read_fbo) glDeleteFramebuffers(1, &mean_read_fbo);
    if (mean_pbo) glDeleteBuffers(1, &mean_pbo);
    mean_read_fbo = mean_pbo = 0;
    free_timers();
    if (glass_query) glDeleteQueries(1, &glass_query);
    glass_query = 0;
    glass_query_pending = false;
    prepared_ok = false;
    // A new context gets one more try at the scene texture: the stuck flag AND the failure
    // it recorded (while scene_srgb_failed is set prepare() refuses everything, so the
    // recorded failure can only be the scene's). Other failures - a program that failed
    // to compile or link, an incomplete target, a malformed mask tile, a GL error during
    // a frame - stay until a different plan or retry_builds(); a hang is never cleared.
    if (scene_srgb_failed) {
        failed = false;
        failed_plan.reset();
    }
    scene_srgb_failed = false;
    scene_tex = 0;
    scene_w = scene_h = 0;
    scene_format = 0;
    caps_probed = false;   // the next context is probed again
    // The deleted programs were never going to be drawn: nothing is left to hang in.
    write_marker("");
}

// ---- TubeRenderer ---------------------------------------------------------------------

TubeRenderer::TubeRenderer(std::string guard_path) : impl_(std::make_unique<Impl>(std::move(guard_path))) {}

// No GL. Removes a marker this instance wrote (a file, not GL): a kiosk shutting down
// mid-window is not a hang. A watchdog SIGABRT runs no destructor, so a real hang keeps it.
TubeRenderer::~TubeRenderer() {
    if (impl_) impl_->write_marker("");
}

bool TubeRenderer::available() const { return !impl_->guard.tripped() && !impl_->failed; }

std::string_view TubeRenderer::unavailable_reason() const {
    if (impl_->guard.tripped()) return "build_hung";
    if (impl_->failed) return "build_failed";
    return "";
}

const TubeRenderer::Caps& TubeRenderer::caps() {
    if (!impl_->caps_probed) impl_->probe_caps();
    return impl_->caps;
}

void TubeRenderer::allow_builds() { impl_->builds_allowed = true; }

void TubeRenderer::retry_builds() {
    Impl& m = *impl_;
    if (!m.failed && !m.scene_srgb_failed) return;
    m.failed = false;
    m.failed_plan.reset();
    m.scene_srgb_failed = false;
    LOG_INFO("tube: build failure cleared; the next prepare tries again");
}

// Spike begin_scene. The scene texture is mutable storage (ensure_scene_fbo uses
// glTexImage2D), so the same name is re-specified in place and its FBO attachment stays.
// Colour-space requirement (1): with FRAMEBUFFER_SRGB off, an SRGB8_ALPHA8 attachment is
// written and blended exactly as UNORM8 - the UI's and video's already-encoded bytes are
// stored unchanged - and the composite reads them with DECODE (TubeGlState::decode_scene)
// when the decode is the hardware's. With the shader decode the scene stays RGBA8.
void TubeRenderer::begin_scene(GLuint scene_tex, int w, int h) {
    Impl& m = *impl_;
    const Caps& c = caps();
    if (scene_tex == 0 || w <= 0 || h <= 0) return;
    if (scene_tex != m.scene_tex || w != m.scene_w || h != m.scene_h) {
        // A new or resized texture (or a forgotten one, after release_gl):
        // ensure_scene_fbo made it RGBA8.
        m.scene_tex = scene_tex;
        m.scene_w = w;
        m.scene_h = h;
        m.scene_format = GL_RGBA8;
    }
    const bool hw = scene_decode_for(c) == tube::SceneDecode::Hardware;
    if (hw && m.scene_format == GL_SRGB8_ALPHA8) {
        // A texture this class specified carries SKIP_DECODE; one the Renderer recreated
        // under the same name and size has the default (DECODE). One client-side query a
        // frame catches a recreation nobody told us about (no release_gl()). On the shader
        // path there is nothing to read: the caller's release_gl() is the only defence there.
        GLint bound = 0, decode = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, &decode);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
        if (decode != GL_SKIP_DECODE_EXT) {
            LOG_WARN("tube: scene texture {} was recreated without release_gl(); specifying it again", scene_tex);
            m.scene_format = GL_RGBA8;
        }
    }
    if (hw && !m.scene_srgb_failed && m.scene_format != GL_SRGB8_ALPHA8) {
        TubeGlState state(c.srgb_write_control);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status == GL_FRAMEBUFFER_COMPLETE) {
            m.scene_format = GL_SRGB8_ALPHA8;
            LOG_INFO("tube: scene texture re-specified as SRGB8_ALPHA8 (written unencoded) {}x{}", w, h);
        } else {
            // Back to what the old engine expects, and Tube off until retry_builds() /
            // release_gl(): a hardware-decode program reading RGBA8 would draw wrong colours.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);
            m.scene_format = GL_RGBA8;
            m.scene_srgb_failed = true;
            LOG_ERROR("tube: scene FBO incomplete with an SRGB8_ALPHA8 texture (0x{:x}); scene kept RGBA8", status);
            // Nothing more can be done if RGBA8 is broken too, but it must not be silent:
            // the old engine would then draw into an incomplete framebuffer.
            const GLenum back = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (back != GL_FRAMEBUFFER_COMPLETE)
                LOG_ERROR("tube: the scene FBO is STILL incomplete (0x{:x}) after going back to RGBA8 {}x{}; the "
                          "Enhanced engine will draw into a broken framebuffer",
                          back, w, h);
            m.record_failure("the scene texture could not be made SRGB8_ALPHA8");
        }
    }
    // Deliberately NOT restored: the scene is drawn after this with encoding off.
    if (c.srgb_write_control) glDisable(GL_FRAMEBUFFER_SRGB_EXT);
}

void TubeRenderer::end_tube_scene(GLuint scene_tex, int w, int h) {
    Impl& m = *impl_;
    const Caps& c = caps();
    if (scene_tex != 0 && scene_tex == m.scene_tex && m.scene_format == GL_SRGB8_ALPHA8 && w > 0 && h > 0) {
        TubeGlState state(c.srgb_write_control);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);   // the default again
        LOG_INFO("tube: scene texture back to RGBA8 {}x{}", w, h);
    }
    // Tube stopped drawing: undrawn programs cannot hang anything now, and a marker left
    // behind would read as a hang at the next unrelated restart. The next prepare() writes
    // it again for whatever is still undrawn.
    m.write_marker("");
    m.current_plan.reset();
    m.prepared_ok = false;
    m.scene_tex = 0;
    m.scene_w = m.scene_h = 0;
    m.scene_format = 0;
    // FRAMEBUFFER_SRGB_EXT's initial state is enabled: put back what a kiosk that never
    // chose Tube has.
    if (c.srgb_write_control) glEnable(GL_FRAMEBUFFER_SRGB_EXT);
}

bool TubeRenderer::prepare(const tube::PassPlan& plan, tube::SceneDecode decode) {
    Impl& m = *impl_;
    if (!m.builds_allowed || m.guard.tripped() || m.scene_srgb_failed) return false;
    // The fast path: nothing changed since a prepare() that succeeded (see Impl::prepared_ok).
    if (m.prepared_ok && m.current_decode == decode && m.current_plan && *m.current_plan == plan) return true;
    m.prepared_ok = false;
    if (m.failed) {
        if (m.failed_plan == plan && m.failed_decode == decode) return false;
        m.failed = false;   // a different plan: try again
        m.failed_plan.reset();
    }
    const Caps& c = caps();
    TubeGlState state(c.srgb_write_control);
    m.current_plan = plan;
    m.current_decode = decode;
    const std::vector<tube::Program> wanted = tube::programs_of(plan);

    // A scene-reading program built for the other decode is a different program.
    for (const tube::Program p : wanted) {
        const ProgramSlot& s = m.slot(p);
        if (s.id && tube::program_info(p).reads_scene && s.built_for != decode) m.free_program(p);
    }

    // The marker first: every program of this plan not yet drawn (built here or before).
    m.write_marker(m.undrawn_of(plan));

    for (const tube::Program p : wanted) {
        if (m.slot(p).id) continue;
#ifdef MDB_TUBE_PREVIEW
        if (!m.force_hang.empty() && m.force_hang == tube::program_info(p).name) {
            LOG_WARN("tube: MDB_TUBE_FORCE_HANG - hanging before building {} (preview test)", m.force_hang);
            for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
        }
#endif
        if (!m.build(p, decode)) {
            m.record_failure("a program failed to compile or link");
            return false;
        }
    }
    m.prepared_ok = true;
    return true;
}

void TubeRenderer::release_gl() {
    impl_->release();
    LOG_INFO("tube: GL objects released; everything is made again on the next frame");
}

}  // namespace ui
