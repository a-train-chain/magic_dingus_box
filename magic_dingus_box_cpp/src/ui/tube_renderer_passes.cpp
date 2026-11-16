// tube_renderer_passes.cpp - TubeRenderer's frame: the passes a tube::PassPlan draws
// (composite), the asynchronous mean probe, the debug GPU timers and the validation of
// every program through its real pass (validate_all_programs). KIOSK-ONLY (GL): verified
// by the Pi-userspace container compile and on the hardware (Task 14).
//
// Ported from the Phase 0 spike (tube_spike.cpp: TubeSpike::composite, Impl::draw_v2,
// run_prepasses, copy_history, run_probe, the timer ring), which ran on the Pi 5. What
// differs, and why, is said where it happens.
//
// What this file builds on (tube_renderer_internal.h): the program slots and their uniform
// tables, the drawn-program bookkeeping (mark_drawn, settle_marker), the render targets
// (ensure_targets), the mask and glass (upload_mask, bake_glass) and the GL state
// save/restore every path uses (TubeGlState).
//
// COLOUR SPACE, PASS BY PASS (the binding rule is the "COLOUR SPACE FOR TASK 11's PASSES"
// note in tube_renderer_internal.h). FRAMEBUFFER_SRGB (when the context has the switch):
//   ON  for every pass into a target: quarter, glow_tmp, glow_out, history and mean
//       (SRGB8_ALPHA8: linear light encoded on write, decoded on read), signal and lines
//       (RGBA16F: no effect; the SRGB8_ALPHA8 fallback: encoded like the others);
//   OFF for every draw into the default framebuffer (output without afterglow,
//       copy_encode): output.frag and copy_encode.frag encode themselves. The kiosk's EGL
//       surface is not sRGB, so ON would change nothing there today; OFF keeps that true
//       if a surface ever is.
// The scene is read with GL_DECODE_EXT on the hardware path (TubeGlState::decode_scene;
// its destructor puts SKIP_DECODE back) and raw on the shader path (the shader decodes).

#include "tube_renderer.h"
#include "tube_renderer_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "../utils/logger.h"

namespace ui {

namespace {

using Clock = std::chrono::steady_clock;
using Pass = tube::PassTimingAverager::Pass;

// Every glGetError drain stops here: a lost context returns errors forever, and the main
// loop must not spin on it.
constexpr int kMaxErrorDrain = 16;

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

double seconds_now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

const char* program_name(tube::Program p) { return tube::program_info(p).name.data(); }

void bind_unit(int unit, GLuint tex) {
    glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
    glBindTexture(GL_TEXTURE_2D, tex);
}

void target(GLuint fbo, int x, int y, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(x, y, w, h);
}

// Encode on write into the sRGB targets; never into the default framebuffer.
void framebuffer_srgb(bool have_switch, bool on) {
    if (!have_switch) return;
    if (on)
        glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    else
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
}

// The state the kiosk's 2D renderer draws the next UI in, as the Enhanced composite
// (renderer_crt.cpp end_scene_fbo_and_composite) leaves it. Declared BEFORE the
// TubeGlState in composite() so it is applied AFTER that restores everything else (the
// scene's decode, pixel-store state, scissor, clear colour): the caller's entry state had
// the scene FBO bound, which the UI after the composite must not draw into.
class CompositeExitState {
public:
    CompositeExitState(const TubeRenderer::GlEnv& env, bool have_srgb_switch)
        : fb_w_(env.fb_w), fb_h_(env.fb_h), ui_program_(env.ui_program), srgb_switch_(have_srgb_switch) {}
    ~CompositeExitState() {
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fb_w_, fb_h_);
        for (int u = kTubeTextureUnits - 1; u >= 0; --u) bind_unit(u, 0);   // ends on unit 0
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(ui_program_);
        framebuffer_srgb(srgb_switch_, false);
    }
    CompositeExitState(const CompositeExitState&) = delete;
    CompositeExitState& operator=(const CompositeExitState&) = delete;

private:
    int fb_w_, fb_h_;
    GLuint ui_program_;
    bool srgb_switch_;
};

// The scene map: the picture rectangle inside the scene texture (which covers the whole
// framebuffer, drawn Y-flipped by the 2D renderer), as the spike's upload_scene.
void scene_uniforms(const ProgramSlot& s, const TubeRenderer::GlEnv& env, video::IRect pic) {
    const float W = static_cast<float>(env.scene_w), H = static_cast<float>(env.scene_h);
    const float x = static_cast<float>(pic.x), y = static_cast<float>(pic.y), w = static_cast<float>(pic.w),
                h = static_cast<float>(pic.h);
    if (s.loc(OwnUniform::uSceneMap) >= 0) glUniform4f(s.loc(OwnUniform::uSceneMap), x / W, (H - y) / H, w / W, h / H);
    if (s.loc(OwnUniform::uSceneClamp) >= 0)
        glUniform4f(s.loc(OwnUniform::uSceneClamp), (x + 0.5f) / W, (H - y - h + 0.5f) / H, (x + w - 0.5f) / W,
                    (H - y - 0.5f) / H);
}

void uniform2f(const ProgramSlot& s, OwnUniform u, float a, float b) {
    if (s.loc(u) >= 0) glUniform2f(s.loc(u), a, b);
}

// The sRGB transfer function's inverse, for the probe's one texel.
double srgb_to_linear(std::uint8_t v) {
    const double c = static_cast<double>(v) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

}  // namespace

// ---- Small helpers ------------------------------------------------------------------------

GLuint TubeRenderer::Impl::black() {
    if (black_tex) return black_tex;
    static constexpr std::uint8_t kBlack[4] = {0, 0, 0, 255};
    glGenTextures(1, &black_tex);
    glBindTexture(GL_TEXTURE_2D, black_tex);   // the active unit; the caller's TubeGlState restores it
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kBlack);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return black_tex;
}

void TubeRenderer::Impl::draw_pass(tube::Program p) {
    const std::size_t i = static_cast<std::size_t>(p);
    const bool first = !slot(p).drawn && first_draw_ms[i] < 0.0;
    const auto t0 = first ? Clock::now() : Clock::time_point{};
    glDrawArrays(GL_TRIANGLES, 0, 6);
    // Mesa compiles the program's real variant synchronously inside this call: once it
    // has returned, that compile is over (no glFinish needed).
    if (first) {
        first_draw_ms[i] = ms_since(t0);
        LOG_INFO("tube: program {} first real draw {:.1f} ms CPU", program_name(p), first_draw_ms[i]);
    }
    if (validating && !validate_failed_in) {
        bool error = false;
        int n = 0;
        for (GLenum e = glGetError(); e != GL_NO_ERROR && n < kMaxErrorDrain; e = glGetError(), ++n) {
            LOG_ERROR("tube validate: GL error 0x{:x} drawing {}", e, program_name(p));
            error = true;
        }
        if (error) validate_failed_in = p;
    }
    for (std::size_t k = 0; k < drawn_now_count; ++k)
        if (drawn_now[k] == p) return;
    if (drawn_now_count < drawn_now.size()) drawn_now[drawn_now_count++] = p;
}

// ---- The mean probe ------------------------------------------------------------------------

void TubeRenderer::Impl::abandon_mean() {
    if (mean_fence) glDeleteSync(mean_fence);
    mean_fence = nullptr;
    mean_stage = MeanStage::idle;
}

// Advances the probe one stage when its fence has signalled; never waits.
void TubeRenderer::Impl::poll_mean() {
    if (mean_stage == MeanStage::idle || !mean_fence) return;
    const GLenum r = glClientWaitSync(mean_fence, 0, 0);
    if (r == GL_TIMEOUT_EXPIRED) return;   // a later frame
    glDeleteSync(mean_fence);
    mean_fence = nullptr;
    if (r == GL_WAIT_FAILED) {
        mean_stage = MeanStage::idle;
        LOG_WARN("tube: the mean probe's fence failed; reading dropped");
        return;
    }
    if (mean_stage == MeanStage::mipped) {
        // The mips are finished: read the 1x1 level into the PBO. Attached afresh each time
        // (a cheap state change every 0.2 s at most): a deleted and re-made `mean` may reuse
        // the old name while this FBO still holds the old object.
        if (!mean.made()) {
            mean_stage = MeanStage::idle;
            return;
        }
        if (!mean_read_fbo) glGenFramebuffers(1, &mean_read_fbo);
        if (!mean_pbo) {
            glGenBuffers(1, &mean_pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, mean_pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, 4, nullptr, GL_STREAM_READ);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, mean_read_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mean.tex, mean.levels - 1);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_WARN("tube: the mean probe's 1x1 level is not readable; reading dropped");
            mean_stage = MeanStage::idle;
            return;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, mean_pbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        mean_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        mean_stage = mean_fence ? MeanStage::reading : MeanStage::idle;
        return;
    }
    // reading: fence B signalled, the PBO holds the texel.
    mean_stage = MeanStage::idle;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, mean_pbo);
    const void* ptr = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4, GL_MAP_READ_BIT);
    if (!ptr) {
        LOG_WARN("tube: the mean probe's buffer could not be mapped; reading dropped");
        return;
    }
    // The 1x1 level's stored bytes, sRGB-encoded (ES 3.0 ReadPixels returns an sRGB
    // attachment's values unconverted, as the spike assumed; Task 14 checks it on the
    // hardware with a known grey).
    const auto* b = static_cast<const std::uint8_t*>(ptr);
    const double light = 0.2126 * srgb_to_linear(b[0]) + 0.7152 * srgb_to_linear(b[1]) + 0.0722 * srgb_to_linear(b[2]);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    if (mean_sink) mean_sink(light);
}

// The picture into the 64x64 mean target and mipped down to 1x1; the read-back follows
// on a later frame, once the GPU has finished (poll_mean).
void TubeRenderer::Impl::run_mean_probe(const GlEnv& env, video::IRect pic) {
    const ProgramSlot& q = slot(tube::Program::quarter);
    timer_begin(Pass::probe);
    target(mean.fbo, 0, 0, mean.w, mean.h);
    framebuffer_srgb(caps.srgb_write_control, true);
    glUseProgram(q.id);
    uniform2f(q, OwnUniform::uPicSize, static_cast<float>(pic.w), static_cast<float>(pic.h));
    uniform2f(q, OwnUniform::uDstSize, static_cast<float>(mean.w), static_cast<float>(mean.h));
    scene_uniforms(q, env, pic);
    bind_unit(0, env.scene_tex);
    draw_pass(tube::Program::quarter);
    bind_unit(0, mean.tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    timer_end();
    mean_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    mean_stage = mean_fence ? MeanStage::mipped : MeanStage::idle;
}

// ---- The debug GPU timers ------------------------------------------------------------------

void TubeRenderer::Impl::free_timers() {
    for (TimerSet& set : timer_ring) {
        for (GLuint& q : set.q)
            if (q) glDeleteQueries(1, &q);
        set = TimerSet{};
    }
    timer_open = false;
}

void TubeRenderer::Impl::timer_begin(Pass pass) {
    if (!gpu_timing || !caps.timer_query || timer_open) return;
    TimerSet& set = timer_ring[timer_slot];
    const std::size_t i = static_cast<std::size_t>(pass);
    if (!set.q[i]) glGenQueries(1, &set.q[i]);
    begin_query(GL_TIME_ELAPSED_EXT, set.q[i]);
    set.pending[i] = true;
    timer_open = true;
}

void TubeRenderer::Impl::timer_end() {
    if (!timer_open) return;
    end_query(GL_TIME_ELAPSED_EXT);
    timer_open = false;
}

// Reads every set whose results are all available (oldest first), each as one frame of
// the averager; drops what is still pending in the set this frame is about to reuse.
void TubeRenderer::Impl::collect_timers() {
    if (!gpu_timing || !caps.timer_query) return;
    for (std::size_t k = 1; k <= kTimerSets; ++k) {
        TimerSet& set = timer_ring[(timer_slot + k) % kTimerSets];
        if (!set.any_pending()) continue;
        bool all = true;
        for (std::size_t i = 0; i < kTimedPasses && all; ++i) {
            if (!set.pending[i]) continue;
            GLuint available = 0;
            glGetQueryObjectuiv(set.q[i], GL_QUERY_RESULT_AVAILABLE, &available);
            all = available != 0;
        }
        if (!all) continue;
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        for (std::size_t i = 0; i < kTimedPasses; ++i) {
            if (!set.pending[i]) continue;
            set.pending[i] = false;
            if (disjoint) continue;   // the GPU's clock jumped: the results mean nothing
            GLuint64 ns = 0;
            get_query_ui64(set.q[i], GL_QUERY_RESULT, &ns);
            timing.add(static_cast<Pass>(i), static_cast<double>(ns) / 1.0e6);
        }
        if (!disjoint) timing.end_frame();
    }
    // The slot this frame writes: a result still not available is dropped, never waited for.
    timer_slot = (timer_slot + 1) % kTimerSets;
    TimerSet& next = timer_ring[timer_slot];
    if (next.any_pending()) {
        next.pending.fill(false);
        ++timer_dropped;
    }
    if (const auto line = timing.take_line(seconds_now()))
        LOG_INFO("{} ({} frame sets dropped unread)", *line, timer_dropped);
}

// ---- One frame -----------------------------------------------------------------------------

bool TubeRenderer::Impl::draw_frame(const GlEnv& env, video::IRect pic, const tube::DriverFrame& f) {
    const tube::PassPlan& plan = f.plan;
    drawn_now_count = 0;
    // Only the plan prepare() succeeded with: never draw undrawn programs with no marker
    // in place (end_tube_scene() or release_gl() since, or a failure recorded).
    if (!prepared_ok || failed || !current_plan || *current_plan != plan) return false;

    // A GL error left by someone else must not be blamed on (and fail) this plan.
    int drained = 0;
    for (GLenum e = glGetError(); e != GL_NO_ERROR && drained < kMaxErrorDrain; e = glGetError(), ++drained)
        if (!foreign_error_logged) {
            foreign_error_logged = true;
            LOG_WARN("tube: GL error 0x{:x} was pending before the tube composite (not the tube's; logged once)", e);
        }

    poll_glass_timing();
    collect_timers();
    // A probe in flight is dropped only when the plan no longer has one (the mean target
    // goes) - not on still_changed: the 64x64 target never resizes, the request is spent,
    // and dropping it would stall ABL readings for as long as a row is dragged.
    if (!plan.mean_probe) {
        abandon_mean();
        mean_requested = false;
    }
    poll_mean();

    // GL in: the clip-space quad in the renderer's VAO/VBO, no blending, no scissor,
    // encode on for the targets, the scene decoded by the texture unit on the hardware path.
    bind_quad(env);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    framebuffer_srgb(caps.srgb_write_control, true);
    black();   // made now, before any unit is bound (it binds on the active unit once)

    // Targets: no GL in the steady state (sizes and formats compared only).
    if (!ensure_targets(plan, f.sizes)) return false;
    if (mask_pending) {
        mask_pending = false;
        upload_mask(mask_tile);   // the newest tile handed over (take_one_shots)
        if (failed) return false;
    } else if (!mask_tex) {
        // composite() refuses this before prepare(); unreachable from there, kept so a
        // future caller of draw_frame cannot sample a missing mask.
        prepared_ok = false;
        return false;
    }
    if ((f.glass_changed || !glass_valid) && !bake_glass(env, f.u)) {
        record_failure("the glass could not be baked (no program or target)");
        return false;
    }

    // The values the shaders would otherwise work out per pixel from uniforms alone, once
    // for the frame (on the stack: no allocation), now that the mask and signal sizes are known.
    const tube::TubeDerived derived = tube::tube_derived(f.u, tube::DerivedSizes{std::max(mask_w, 1), std::max(mask_h, 1),
                                                                                 std::max(signal.w, 1), std::max(signal.h, 1)});
    const auto use = [&](tube::Program p) -> const ProgramSlot& {
        const ProgramSlot& s = slot(p);
        glUseProgram(s.id);
        upload_tube_uniforms(s, f.u, derived);
        return s;
    };
    const auto read_scene = [&] { bind_unit(0, env.scene_tex); };

    // 1. The mean probe, when asked for (one in flight at a time).
    if (plan.mean_probe && mean_requested && mean_stage == MeanStage::idle && mean.made()) {
        mean_requested = false;
        run_mean_probe(env, pic);
    }

    // 2. Quarter -> blur H -> blur V (glow), the quarter alone for chroma bleed.
    if (plan.quarter) {
        timer_begin(Pass::quarter);
        target(quarter.fbo, 0, 0, quarter.w, quarter.h);
        const ProgramSlot& q = use(tube::Program::quarter);
        uniform2f(q, OwnUniform::uPicSize, static_cast<float>(pic.w), static_cast<float>(pic.h));
        uniform2f(q, OwnUniform::uDstSize, static_cast<float>(quarter.w), static_cast<float>(quarter.h));
        scene_uniforms(q, env, pic);
        read_scene();
        draw_pass(tube::Program::quarter);
        timer_end();
    }
    if (plan.glow) {
        timer_begin(Pass::glow);
        const ProgramSlot& b = use(tube::Program::blur);
        const float sigma = std::max(f.glow_sigma, 0.1f);
        const int radius = std::min(static_cast<int>(std::ceil(3.0f * sigma)), 24);   // blur.frag MAX_RADIUS
        if (b.loc(OwnUniform::uSigma) >= 0) glUniform1f(b.loc(OwnUniform::uSigma), sigma);
        if (b.loc(OwnUniform::uRadius) >= 0) glUniform1i(b.loc(OwnUniform::uRadius), radius);
        target(glow_tmp.fbo, 0, 0, glow_tmp.w, glow_tmp.h);
        uniform2f(b, OwnUniform::uDir, 1.0f, 0.0f);
        bind_unit(0, quarter.tex);
        draw_pass(tube::Program::blur);
        target(glow_out.fbo, 0, 0, glow_out.w, glow_out.h);
        uniform2f(b, OwnUniform::uDir, 0.0f, 1.0f);
        bind_unit(0, glow_tmp.tex);
        draw_pass(tube::Program::blur);
        timer_end();
    }

    // 3. The signal (source space, signal_w x signal_h) and the raster's lines.
    if (plan.signal) {
        timer_begin(Pass::signal);
        target(signal.fbo, 0, 0, signal.w, signal.h);
        const ProgramSlot& s = use(*plan.signal);
        scene_uniforms(s, env, pic);
        bind_unit(4, quarter.made() ? quarter.tex : black());
        read_scene();
        draw_pass(*plan.signal);
        timer_end();
    }
    if (plan.lines) {
        timer_begin(Pass::lines);
        target(lines.fbo, 0, 0, lines.w, lines.h);
        use(tube::Program::lines);
        bind_unit(5, signal.tex);
        draw_pass(tube::Program::lines);
        timer_end();
    }

    // 4. The output: into the history (afterglow) or straight into the picture rectangle
    //    on the default framebuffer, whose bars are cleared black first. GL's viewport is
    //    bottom-left: y = fb_h - y - h. The clear is issued right before the default
    //    framebuffer's first draw (the spike cleared it before every pass): on a tiler the
    //    clear then folds into the same job as the draw instead of storing a cleared
    //    framebuffer and loading it back.
    const int yb = env.fb_h - pic.y - pic.h;
    const auto clear_bars = [&] {
        framebuffer_srgb(caps.srgb_write_control, false);
        target(0, 0, 0, env.fb_w, env.fb_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glViewport(pic.x, yb, pic.w, pic.h);
    };
    const int write = history_index, read = history_index ^ 1;
    bind_unit(1, plan.glow ? glow_out.tex : black());
    bind_unit(2, plan.afterglow ? history[read].tex : black());
    bind_unit(3, plan.lines ? lines.tex : black());
    bind_unit(5, plan.signal ? signal.tex : black());
    bind_unit(6, glass.tex);
    bind_unit(7, mask_tex);
    read_scene();
    timer_begin(Pass::output);
    if (plan.afterglow) {
        target(history[write].fbo, 0, 0, history[write].w, history[write].h);
    } else {
        clear_bars();
    }
    const ProgramSlot& o = use(plan.output);
    if (plan.afterglow)
        uniform2f(o, OwnUniform::uOrigin, 0.0f, 0.0f);
    else
        uniform2f(o, OwnUniform::uOrigin, static_cast<float>(pic.x), static_cast<float>(yb));
    scene_uniforms(o, env, pic);
    draw_pass(plan.output);
    timer_end();
    if (plan.afterglow) {
        bind_unit(2, black());   // the history just written must not stay bound as `prev`
        timer_begin(Pass::history);
        clear_bars();
        const ProgramSlot& c = use(tube::Program::copy_encode);
        uniform2f(c, OwnUniform::uOrigin, static_cast<float>(pic.x), static_cast<float>(yb));
        if (c.loc(OwnUniform::uSrcOffset) >= 0) glUniform2i(c.loc(OwnUniform::uSrcOffset), 0, 0);
        bind_unit(0, history[write].tex);
        draw_pass(tube::Program::copy_encode);
        timer_end();
    }

    // 5. One drain of GL errors for the whole frame.
    bool error = false;
    int errors = 0;
    for (GLenum e = glGetError(); e != GL_NO_ERROR && errors < kMaxErrorDrain; e = glGetError(), ++errors) {
        if (!error && gl_error_logged_plan != plan) {
            LOG_ERROR("tube: GL error 0x{:x} during the frame (output {}); drawing Enhanced",
                      e, program_name(plan.output));
            gl_error_logged_plan = plan;
        }
        error = true;
    }
    if (error || validate_failed_in) {
        record_failure("a GL error during the tube's passes");
        return false;
    }
    // Only now, with every draw submitted cleanly, are this frame's programs drawn.
    for (std::size_t k = 0; k < drawn_now_count; ++k) mark_drawn(drawn_now[k]);
    if (plan.afterglow) history_index ^= 1;
    return true;
}

void TubeRenderer::Impl::take_one_shots(const tube::DriverFrame& f) {
    if (f.mask_changed && !f.mask.rgb.empty()) {
        mask_tile.w = f.mask.w;
        mask_tile.h = f.mask.h;
        mask_tile.rgb.assign(f.mask.rgb.begin(), f.mask.rgb.end());   // reuses the capacity when it can
        mask_pending = true;
    }
    if (f.glass_changed) glass_valid = false;
}

// After validation: nothing synthetic may reach a real frame. The programs stay built and
// drawn (that was the point); everything the synthetic frames filled is invalidated.
void TubeRenderer::Impl::forget_synthetic_state(bool have_srgb_switch) {
    TubeGlState state(have_srgb_switch);
    glass_valid = false;   // the next frame re-bakes from ITS uniforms, glass_changed or not
    if (mask_tex) glDeleteTextures(1, &mask_tex);   // the grille
    mask_tex = 0;
    mask_w = mask_h = 0;
    // validate_all_programs() put the real tile (if any) back into mask_tile; it is uploaded
    // on the next frame that draws. Without one, composite() refuses until mask_changed.
    mask_pending = mask_tile.w > 0 && mask_tile.h > 0 && !mask_tile.rgb.empty();
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    for (Target& h : history)   // validation content must not glow on as afterglow
        if (h.made()) {
            target(h.fbo, 0, 0, h.w, h.h);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    history_index = 0;
    abandon_mean();
    mean_requested = false;
    current_plan.reset();
    prepared_ok = false;
    marker_clean = false;
    write_marker("");   // every program validated is drawn; nothing is left to hang in
}

// ---- TubeRenderer ---------------------------------------------------------------------------

void TubeRenderer::request_mean() { impl_->mean_requested = true; }

void TubeRenderer::set_mean_sink(std::function<void(double)> sink) { impl_->mean_sink = std::move(sink); }

bool TubeRenderer::composite(const GlEnv& env, video::IRect picture, const tube::DriverFrame& frame) {
    Impl& m = *impl_;
    const tube::SceneDecode decode = scene_decode_for(caps());
    // The driver's one-shot inputs first: a refusal below must not lose them.
    m.take_one_shots(frame);
    // THE CALLER'S ERRORS, checked BEFORE prepare(): a refusal never runs prepare()'s full
    // path and never arms the marker. Each refusal is logged once and stands down
    // (refuse_frame: the marker a previous frame wrote is removed once; after that a
    // refusal that persists does no file operation, no allocation and no GL state work).
    const auto refuse = [&m](bool& logged, auto&& log) {
        if (!logged) log();
        logged = true;
        m.refuse_frame();
        return false;
    };
    if (env.scene_tex == 0 || env.fb_w <= 0 || env.fb_h <= 0 || env.scene_w <= 0 || env.scene_h <= 0)
        return refuse(m.bad_env_logged, [&] {
            LOG_WARN("tube: no scene texture or framebuffer size (scene {} {}x{}, framebuffer {}x{}); drawing "
                     "Enhanced (logged once)",
                     env.scene_tex, env.scene_w, env.scene_h, env.fb_w, env.fb_h);
        });
    if (picture.w != frame.sizes.pic_w || picture.h != frame.sizes.pic_h || picture.w <= 0 || picture.h <= 0)
        return refuse(m.size_mismatch_logged, [&] {
            LOG_WARN("tube: picture {}x{} is not the driver's {}x{}; drawing Enhanced (logged once)", picture.w,
                     picture.h, frame.sizes.pic_w, frame.sizes.pic_h);
        });
    if (!m.mask_tex && !m.mask_pending)   // no texture, no tile this frame, none kept
        return refuse(m.missing_mask_logged, [] {
            LOG_WARN("tube: no mask texture and no mask tile has ever been handed over (FrameDriver::gpu_lost() "
                     "not called after validate_all_programs() on a first frame?); drawing Enhanced (logged once)");
        });
    // begin_scene() already logged the real ERROR, and prepare() refuses meanwhile.
    if (m.scene_srgb_failed) {
        m.refuse_frame();
        return false;
    }
    if (decode == tube::SceneDecode::Hardware &&
        !(m.scene_tex == env.scene_tex && m.scene_format == GL_SRGB8_ALPHA8))
        return refuse(m.scene_not_srgb_logged, [&] {
            LOG_WARN("tube: the scene texture {} is not the SRGB8_ALPHA8 one begin_scene() made (begin_scene not "
                     "called this frame?); drawing Enhanced rather than a washed-out picture (logged once)",
                     env.scene_tex);
        });
    if (!prepare(frame.plan, decode)) return false;
    // Destroyed in reverse: the marker settles, then TubeGlState restores what it found,
    // then the exit state the 2D renderer expects is applied over it.
    CompositeExitState exit_state(env, m.caps.srgb_write_control);
    TubeGlState state(m.caps.srgb_write_control);
    struct Settle {
        Impl& m;
        ~Settle() { m.settle_marker(); }
    } settle{m};
    if (m.current_decode == tube::SceneDecode::Hardware) state.decode_scene(env.scene_tex);
    return m.draw_frame(env, picture, frame);
}

bool TubeRenderer::validate_all_programs(const GlEnv& env, video::IRect picture, tube::SceneDecode decode) {
    Impl& m = *impl_;
    const Caps& c = caps();
    if (decode == tube::SceneDecode::Hardware && scene_decode_for(c) != tube::SceneDecode::Hardware) {
        LOG_ERROR("tube validate: hardware scene decode is not available on this context; not validated");
        return false;
    }
    if (!m.builds_allowed || m.guard.tripped()) {
        LOG_ERROR("tube validate: builds not allowed or the guard tripped ({}); not validated", unavailable_reason());
        return false;
    }
    // Every program afresh, so each one's first draw happens here, through its real pass.
    {
        TubeGlState state(c.srgb_write_control);
        for (std::size_t i = 0; i < tube::kProgramCount; ++i) m.free_program(static_cast<tube::Program>(i));
    }
    m.prepared_ok = false;
    m.current_plan.reset();
    // The real tile (the current mask) survives the synthetic grille: restored at the end.
    tube::MaskTile real_mask = std::move(m.mask_tile);
    m.mask_tile = tube::MaskTile{};
    m.mask_pending = false;

    const tube::Picture pic{static_cast<double>(picture.w), static_cast<double>(picture.h), false};
    const tube::Resolved resolved = tube::resolve(tube::TubeSetting{});
    std::array<bool, tube::kProgramCount> ok{};
    bool all_plans = true;
    LOG_INFO("tube validate: {} plans, scene decode {}", tube::validation_plans().size(),
             decode == tube::SceneDecode::Hardware ? "hardware" : "shader");
    std::size_t n = 0;
    for (const tube::PassPlan& plan : tube::validation_plans()) {
        ++n;
        tube::DriverFrame f;
        f.plan = plan;
        f.resolved = resolved;
        f.u = tube::tube_uniforms_for(resolved, pic);
        f.sizes = tube::target_sizes(plan, pic, f.u);
        f.glow_sigma = plan.glow ? tube::glow_sigma_px(resolved, pic) : 0.0f;
        f.mask = tube::mask_tile(tube::Mask::grille, 3, 0.5f);
        f.still_changed = f.glass_changed = f.mask_changed = true;
        m.take_one_shots(f);
        m.validate_failed_in.reset();
        m.validating = true;
        m.mean_requested = plan.mean_probe;
        retry_builds();   // a failed plan before this one must not block it
        bool drew = false;
        if (prepare(plan, decode)) {
            CompositeExitState exit_state(env, c.srgb_write_control);
            TubeGlState state(c.srgb_write_control);
            struct Settle {
                Impl& m;
                ~Settle() { m.settle_marker(); }
            } settle{m};
            if (decode == tube::SceneDecode::Hardware) state.decode_scene(env.scene_tex);
            drew = m.draw_frame(env, picture, f);
        }
        m.validating = false;
        if (!drew) {
            all_plans = false;
            LOG_ERROR("tube validate: plan {} (output {}) FAILED{}{}", n, program_name(plan.output),
                      m.validate_failed_in ? " while drawing " : "",
                      m.validate_failed_in ? program_name(*m.validate_failed_in) : "");
            continue;
        }
        for (const tube::Program p : tube::programs_of(plan)) {
            const std::size_t i = static_cast<std::size_t>(p);
            if (ok[i]) continue;
            ok[i] = m.slot(p).drawn;
            if (!ok[i]) continue;
            if (m.first_draw_ms[i] >= 0.0)
                LOG_INFO("tube validate: {} first real draw {:.1f} ms", program_name(p), m.first_draw_ms[i]);
            else   // the glass: its first draw is the bake, timed by the bake's own lines
                LOG_INFO("tube validate: {} first real draw in the glass bake (its log line has the time)",
                         program_name(p));
        }
    }
    m.mask_tile = std::move(real_mask);
    m.forget_synthetic_state(c.srgb_write_control);
    const auto good = static_cast<std::size_t>(std::count(ok.begin(), ok.end(), true));
    LOG_INFO("tube validate: {} of {} programs OK", good, tube::kProgramCount);
    for (std::size_t i = 0; i < tube::kProgramCount; ++i)
        if (!ok[i]) LOG_ERROR("tube validate: {} was never drawn cleanly", program_name(static_cast<tube::Program>(i)));
    return all_plans && good == tube::kProgramCount;
}

}  // namespace ui
