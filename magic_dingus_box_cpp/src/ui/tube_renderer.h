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
//   never retried within a process.
#pragma once

#include <GLES3/gl3.h>

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

    bool available() const;                            // not hang-tripped, and no failure for the current plan
    std::string_view unavailable_reason() const;       // "" | "build_hung" | "build_failed"
    const Caps& caps();                                // probed once per context (again after release_gl)
    void allow_builds();                               // main calls it right after READY=1; prepare() does nothing before

    // From begin_scene_fbo, the scene FBO bound, before its clear. With hardware decode
    // (scene_decode_for(caps())) the scene texture is re-specified as SRGB8_ALPHA8 with
    // SKIP_DECODE, only when it is a new or resized texture or after release_gl() - never
    // per frame. FRAMEBUFFER_SRGB is turned off (when the context can) and LEFT off: the
    // UI's already-encoded bytes are stored unchanged.
    void begin_scene(GLuint scene_tex, int w, int h);
    // Back to RGBA8 (and FRAMEBUFFER_SRGB to its default, on): the engine left Tube.
    void end_tube_scene(GLuint scene_tex, int w, int h);

    // Compiles and links what the plan lacks, the marker written first. False: not
    // allowed yet, hang-tripped, or a build failed (now or before, for this plan).
    bool prepare(const tube::PassPlan&, tube::SceneDecode);
    // Clears a recorded build failure so the next prepare() tries again. Task 12 calls it
    // on an engine change (where it calls FrameDriver::start); a changed plan clears the
    // failure by itself. Never clears a hang.
    void retry_builds();

    bool composite(const GlEnv&, video::IRect picture, const tube::DriverFrame&);  // Task 11
    bool validate_all_programs(const GlEnv&, video::IRect picture, tube::SceneDecode);  // Task 14
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
