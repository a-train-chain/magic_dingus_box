// tube_renderer.h - the Tube CRT engine's GL side (design: 2026-11-15-tube-crt-engine-design.md,
// section 3). It draws what tube::FrameDriver decides (tube_driver.h) with the programs of
// the fixed table (tube_programs.h), assembled by tube_shader_assembly.h and built under
// the build guard (app/tube_build_guard.h).
//
// KIOSK-ONLY (GL). The decisions it acts on are pure and Mac-tested elsewhere; this file is
// verified by the Pi-userspace container compile, CI's shader validation and the hardware.
//
// CALLING RULES
//   - Every call is on the render thread with the kiosk's GL context current, and with the
//     UI batch already FLUSHED: the caller (the Renderer files) runs flush_ui_batch() first.
//   - The destructor touches no GL: release_gl() runs from Renderer::cleanup() / reset_gl().
//   - After release_gl() the CALLER MUST ALSO CALL FrameDriver::gpu_lost() on its driver,
//     so the next frame carries glass_changed, mask_changed (with the pixels) and
//     still_changed and everything is made again. TubeRenderer does not own the driver.
//   - Every GL state the tube changes is restored before any public call returns, on every
//     return path (one RAII helper, tube_renderer_internal.h's TubeGlState), except the one
//     begin_scene() changes on purpose for the scene draw (FRAMEBUFFER_SRGB off).
//
// BUILDS AND THE GUARD
//   prepare() writes the guard's marker naming the plan's programs that have not yet been
//   DRAWN, then compiles and links what the plan lacks. It never draws: Mesa (V3D) compiles
//   a program's real variant at its first draw with the real target and texture formats,
//   so the marker stays until composite() has drawn every one of them through its real
//   pass and returned. A kiosk killed inside that window finds the marker at its next
//   start, and Tube stays off ("build_hung") for as long as the marker survives (/dev/shm:
//   until a reboot). A compile or link failure, or an incomplete render target, removes the
//   marker (a failure is not a hang) and records the plan as failed ("build_failed"): it is
//   retried when prepare() is given a different plan, or after retry_builds(). A hang is
//   never retried within a process. The marker is also removed when Tube stops drawing
//   (end_tube_scene), by release_gl() and by the destructor: programs that are not being
//   drawn cannot hang, and a stale marker would read as a hang at the next restart.
//
// THE CALLING PATTERN (Task 12), every frame the engine is Tube:
//     if (tube.composite(env, picture, frame)) ...   (composite() calls prepare() itself)
//     else  draw the Enhanced composite;  status reason = tube.unavailable_reason()
//   ALWAYS call composite() (it calls prepare() first), even while unavailable_reason() is
//   "build_failed": prepare() is what clears a failure once the plan has changed, so
//   gating it on available() would keep Tube off forever. prepare() is cheap when nothing
//   changed (no allocation, no GL query). available() / unavailable_reason() are for
//   status only. Calling prepare() on its own beforehand is harmless (the fast path).
//   On an engine change call retry_builds() (beside FrameDriver::start); on leaving Tube,
//   end_tube_scene().
#pragma once

#include <GLES3/gl3.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "../app/tube_build_guard.h"
#include "../tube/tube_driver.h"
#include "../tube/tube_programs.h"
#include "../tube/tube_shader_assembly.h"
#include "../video/picture_placement.h"

namespace ui {

class TubeRenderer {
public:
    struct Caps { bool srgb_write_control = false, srgb_decode = false, half_float_targets = false, timer_query = false; };
    struct GlEnv { GLuint scene_tex = 0; int scene_w = 0, scene_h = 0, fb_w = 0, fb_h = 0; GLuint vao = 0, vbo = 0, ui_program = 0; };

    explicit TubeRenderer(std::string guard_path = app::TubeBuildGuard::kDefaultPath);
    ~TubeRenderer();                                   // no GL: release_gl() runs from Renderer::cleanup()
    TubeRenderer(const TubeRenderer&) = delete;
    TubeRenderer& operator=(const TubeRenderer&) = delete;

    // Status only, never a gate on prepare(): false when the guard tripped at startup
    // ("build_hung", for the life of the process) or a build failed and no prepare() with a
    // different plan nor retry_builds() has cleared it since ("build_failed").
    bool available() const;
    std::string_view unavailable_reason() const;       // "" | "build_hung" | "build_failed"
    // What the marker named when the guard tripped at startup (log-safe; see
    // app::TubeBuildGuard::tripped_program()); "" when it did not trip.
    std::string_view hung_program() const;
    const Caps& caps();                                // probed once per context (again after release_gl)
    void allow_builds();                               // main calls it right after READY=1; prepare() does nothing before

    // From begin_scene_fbo, the scene FBO bound, before its clear. With hardware decode
    // (scene_decode_for(caps())) the scene texture is re-specified as SRGB8_ALPHA8 with
    // SKIP_DECODE, only when it is a new or resized texture or after release_gl() - never
    // per frame. FRAMEBUFFER_SRGB is turned off (when the context can) and LEFT off: the
    // UI's already-encoded bytes are stored unchanged. A texture recreated under the same
    // name and size is noticed by its decode parameter (hardware path only). If the scene
    // FBO is incomplete as SRGB8_ALPHA8 the texture goes back to RGBA8 and the failure is
    // recorded ("build_failed") until retry_builds() or release_gl() (a new context tries
    // again; see release_gl()).
    void begin_scene(GLuint scene_tex, int w, int h);
    // Back to RGBA8 (and FRAMEBUFFER_SRGB to its default, on): the engine left Tube. Also
    // removes the guard's marker and forgets the plan (the next prepare() starts afresh).
    void end_tube_scene(GLuint scene_tex, int w, int h);

    // Compiles and links what the plan lacks, the marker written first. False: not
    // allowed yet, hang-tripped, or a build failed (now or before, for this plan).
    bool prepare(const tube::PassPlan&, tube::SceneDecode);
    // Clears a recorded build failure so the next prepare() tries again. Task 12 calls it
    // on an engine change (where it calls FrameDriver::start); a changed plan clears the
    // failure by itself. Never clears a hang.
    void retry_builds();

    // ONE FRAME (Task 11), from end_scene_fbo_and_composite with the UI batch flushed, in
    // this order:
    //   1. the frame's one-shot inputs are kept (the mask tile on mask_changed, the glass's
    //      invalidation on glass_changed), so a refused or failed frame never loses them -
    //      the next frame that draws uploads / re-bakes them;
    //   2. the caller's inputs are checked, WITHOUT prepare(): env (scene texture and sizes),
    //      `picture` size == frame.sizes, a mask (texture, or a tile handed over and kept),
    //      a scene-texture failure recorded by begin_scene(), and on the hardware path the
    //      scene texture being the SRGB8_ALPHA8 one begin_scene() made. A failed check
    //      refuses the frame (logged once per reason; the marker removed once; nothing more
    //      while it persists);
    //   3. prepare(frame.plan, scene_decode_for(caps())) (cheap when nothing changed);
    //   4. the plan's passes - the glass bake and mask upload when the frame says so, the
    // mean probe when request_mean() asked for one, quarter -> blur H -> blur V, signal,
    // lines - and the output into `picture` (top-left-origin framebuffer px; the bars
    // around it cleared black), through the afterglow history when the plan has one.
    // CALL ORDER: composite() calls prepare() itself (step 3), then draws only the plan prepare()
    // succeeded with; it refuses (returns false, draws nothing, marks nothing drawn) when
    // there is no current plan or it is not frame.plan - e.g. after end_tube_scene(). A
    // caller that also calls prepare() must do it in the same frame, before composite(),
    // with nothing (end_tube_scene(), release_gl()) in between.
    // `picture` must be the rectangle the driver's Picture was made from (its size equal
    // to frame.sizes.pic_w x pic_h). Leaves GL as the Enhanced composite does: VAO 0,
    // FBO 0, the whole-framebuffer viewport, units 7..0 unbound with unit 0 active, blend
    // on (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), env.ui_program, FRAMEBUFFER_SRGB off, the scene
    // SKIP_DECODE; scissor, clear colour and pixel-store state as they were (a frame refused
    // at step 2 touches no GL at all). False: Tube drew nothing usable this frame - a
    // caller-input refusal (step 2), not allowed or not built yet, a build, target, mask or
    // glass failure, or a GL error during the frame (those recorded as a failure for this
    // plan); the caller draws the Enhanced composite instead. Never waits on the GPU.
    bool composite(const GlEnv&, video::IRect picture, const tube::DriverFrame&);
    // Ask the next composite() to run the mean probe (call it when FrameDriver::mean_due()
    // says so, with note_mean_requested()). Ignored by a plan without the probe; while a
    // reading is still in flight the request waits for it. The value reaches the sink a few
    // frames later, never by waiting.
    void request_mean();
    // Where a finished mean reading goes: linear luma (0.2126 R + 0.7152 G + 0.0722 B) of
    // the picture, 0..1. The caller passes it to FrameDriver::on_mean(). Called from inside
    // composite(), on the render thread.
    void set_mean_sink(std::function<void(double)> sink);
    // PREVIEW / HARDWARE VALIDATION (Task 14): every program of the table built afresh
    // with `decode` and drawn once through its real pass, plan by plan
    // (tube::validation_plans()), into the REAL default framebuffer and targets with
    // synthetic frames, each plan under the guard like any build. Logs
    // "tube validate: <program> first real draw <ms> ms" per program and
    // "tube validate: N of 19 programs OK"; a failure names the program being drawn.
    // Draws visible frames: not for production. True when every program drew cleanly.
    // Leaves no synthetic state behind (the programs stay built and drawn): the glass is
    // re-baked on the next frame, the mask texture is deleted, the afterglow history
    // cleared, any probe in flight dropped and the plan forgotten; the real mask tile held
    // before validation (if any) is kept and uploaded again on the next frame that draws.
    // THE CALLER THEN CALLS FrameDriver::gpu_lost() (fresh mask pixels and targets on the
    // next frame); if no real tile was ever handed over, composite() refuses (once-logged)
    // until a frame carries mask_changed.
    bool validate_all_programs(const GlEnv&, video::IRect picture, tube::SceneDecode);
    // Every GL object deleted (a game took the display, or the Renderer is going). The next
    // composite() makes everything again. Clears a failure whose cause was the scene
    // texture (a new context gets one more try). Does NOT clear the other recorded failures
    // - a program that failed to compile or link, an incomplete render target, a malformed
    // mask tile, a GL error during a frame: those stay until a prepare() with a different
    // plan or retry_builds() - and never clears a hang (the guard's marker).
    void release_gl();

private:
    struct Impl;   // tube_renderer_internal.h (shared with tube_renderer_passes.cpp)
    std::unique_ptr<Impl> impl_;
};

// How the scene is decoded on this context: the texture unit decodes it only when the
// UI's bytes can be written unencoded (GL_EXT_sRGB_write_control) AND the decode can be
// switched per texture (GL_EXT_texture_sRGB_decode); otherwise the shader decodes.
inline tube::SceneDecode scene_decode_for(const TubeRenderer::Caps& c) {
    return c.srgb_write_control && c.srgb_decode ? tube::SceneDecode::Hardware : tube::SceneDecode::Shader;
}

}  // namespace ui
