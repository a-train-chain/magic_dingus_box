// tube_renderer_internal.h - private to TubeRenderer's implementation files
// (tube_renderer.cpp: capabilities, builds, targets, mask, glass; tube_renderer_passes.cpp:
// the frame's passes, Task 11). Not an API: nothing else includes it. KIOSK-ONLY (GL).
#pragma once

#include <GLES3/gl3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tube_renderer.h"
#include "../tube/tube_derived.h"
#include "../tube/tube_gpu_timing.h"

// Extension tokens (GL_EXT_sRGB_write_control, GL_EXT_texture_sRGB_decode,
// GL_EXT_disjoint_timer_query), in case the GLES3 headers lack gl2ext's copies.
#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif
#ifndef GL_TEXTURE_SRGB_DECODE_EXT
#define GL_TEXTURE_SRGB_DECODE_EXT 0x8A48
#endif
#ifndef GL_DECODE_EXT
#define GL_DECODE_EXT 0x8A49
#endif
#ifndef GL_SKIP_DECODE_EXT
#define GL_SKIP_DECODE_EXT 0x8A4A
#endif
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

namespace ui {

// ---- Uniform tables --------------------------------------------------------------
//
// FOR TASK 11 - uploading a frame's uniforms with no string lookup per frame:
//   kTubeFields lists every float field of tube::TubeUniforms with its GLSL name (and
//   `tint`, the one vec3, is separate). When a program is built, each name uniforms_of(p)
//   returns is resolved ONCE into its slot:
//     - a TubeUniforms float field -> ProgramSlot::uploads gets {field index, location}
//       (only when the location is >= 0; three names are declared but unread, -1 is legal);
//     - a tube::kDerivedFields name -> ProgramSlot::derived_uploads, the same way;
//     - "tint"                      -> ProgramSlot::tint;
//     - one of kOwnUniformNames     -> ProgramSlot::own[OwnUniform] (-1 when absent);
//     - one of kSamplerUnits         -> glUniform1i(location, unit) at build, never again.
//   Each frame: upload_tube_uniforms(slot, frame.u, derived) sends every TubeUniforms and
//   TubeDerived field the program lists (the derived values are worked out once a frame,
//   on the stack, by tube::tube_derived); the pass then sets its own with slot.own[...]
//   and binds its textures to kSamplerUnits' units.

struct TubeField {
    const char* name;
    float tube::TubeUniforms::*member;
};
namespace tube_fields {
using TU = tube::TubeUniforms;   // scoped: only this table's lines use the short name
inline constexpr TubeField kTable[] = {
    {"outW", &TU::outW}, {"outH", &TU::outH}, {"pxPerMm", &TU::pxPerMm},
    {"curvature", &TU::curvature}, {"cornerPx", &TU::cornerPx}, {"marginPx", &TU::marginPx}, {"flatFit", &TU::flatFit},
    {"scanlines", &TU::scanlines}, {"lines", &TU::lines}, {"raster", &TU::raster},
    {"beamMin", &TU::beamMin}, {"beamMax", &TU::beamMax},
    {"maskType", &TU::maskType}, {"maskStrength", &TU::maskStrength}, {"maskGain", &TU::maskGain},
    {"maskPitch", &TU::maskPitch},
    {"glow", &TU::glow}, {"sharpness", &TU::sharpness}, {"brightness", &TU::brightness},
    {"contrast", &TU::contrast}, {"saturation", &TU::saturation}, {"convergence", &TU::convergence},
    {"persistence", &TU::persistence}, {"flicker", &TU::flicker}, {"hum", &TU::hum},
    {"vignette", &TU::vignette}, {"rim", &TU::rim},
    {"noise", &TU::noise}, {"jitter", &TU::jitter}, {"glassBlack", &TU::glassBlack},
    {"reflection", &TU::reflection}, {"abl", &TU::abl}, {"breathe", &TU::breathe},
    {"chromaBleed", &TU::chromaBleed},
    {"humPhase", &TU::humPhase}, {"flickerWave", &TU::flickerWave}, {"noiseSeed", &TU::noiseSeed},
    {"jitterSeed", &TU::jitterSeed}, {"degaussPhaseA", &TU::degaussPhaseA},
    {"degaussPhaseB", &TU::degaussPhaseB}, {"jitterBias", &TU::jitterBias},
    {"ablLevel", &TU::ablLevel}, {"power", &TU::power}, {"degauss", &TU::degauss}, {"spot", &TU::spot},
    {"warmupGain", &TU::warmupGain},
    {"rollAmount", &TU::rollAmount}, {"rollPosition", &TU::rollPosition},
    {"rollHalfWidth", &TU::rollHalfWidth}, {"rollTint", &TU::rollTint},
    {"trackingOffset", &TU::trackingOffset}, {"trackingPosition", &TU::trackingPosition},
    {"trackingHalfWidth", &TU::trackingHalfWidth}, {"driftOffset", &TU::driftOffset},
    {"flagOffset", &TU::flagOffset}, {"flagExtent", &TU::flagExtent},
    {"purityAmount", &TU::purityAmount}, {"purityX", &TU::purityX}, {"purityY", &TU::purityY},
    {"purityRadius", &TU::purityRadius}, {"purityR", &TU::purityR}, {"purityG", &TU::purityG},
    {"purityB", &TU::purityB},
};
}  // namespace tube_fields
inline constexpr const auto& kTubeFields = tube_fields::kTable;
inline constexpr std::size_t kTubeFieldCount = sizeof(kTubeFields) / sizeof(kTubeFields[0]);
// + tint (three floats): every TubeUniforms member is in the table.
static_assert(sizeof(tube::TubeUniforms) == (kTubeFieldCount + 3) * sizeof(float), "a TubeUniforms field is missing from kTubeFields");

// The programs' own uniforms (tube_programs.h's uniforms_for, the `u` names).
enum class OwnUniform : std::uint8_t {
    uSceneMap, uSceneClamp, uPicSize, uDstSize, uDir, uSigma, uRadius, uOrigin, uSrcOffset, count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(OwnUniform::count)> kOwnUniformNames{
    "uSceneMap", "uSceneClamp", "uPicSize", "uDstSize", "uDir", "uSigma",
    "uRadius",   "uOrigin",     "uSrcOffset"};

// Every sampler and the texture unit it reads, fixed for the life of a program (the
// spike's assignment). `ui` (the scene) and `src` share unit 0: no program has both.
struct SamplerUnit {
    std::string_view name;
    int unit;
};
inline constexpr std::array<SamplerUnit, 9> kSamplerUnits{{{"ui", 0},
                                                          {"src", 0},
                                                          {"glowTex", 1},
                                                          {"prev", 2},
                                                          {"linesTex", 3},
                                                          {"quarter", 4},
                                                          {"signalTex", 5},
                                                          {"glassTex", 6},
                                                          {"maskTex", 7}}};
inline constexpr int kTubeTextureUnits = 8;   // units 0..7, all of which TubeGlState restores

// ---- One program of the table -------------------------------------------------------

struct ProgramSlot {
    GLuint id = 0;
    tube::SceneDecode built_for = tube::SceneDecode::Shader;   // matters only for programs that read the scene
    // Built but not yet drawn through its real pass: Mesa's real compile is still to
    // come, so the guard's marker must name it (see Impl's bookkeeping below).
    bool drawn = false;
    struct Upload {
        std::uint8_t field;   // index into kTubeFields
        GLint location;
    };
    std::vector<Upload> uploads;
    std::vector<Upload> derived_uploads;   // field: index into tube::kDerivedFields
    GLint tint = -1;
    std::array<GLint, static_cast<std::size_t>(OwnUniform::count)> own{};

    GLint loc(OwnUniform u) const { return own[static_cast<std::size_t>(u)]; }
};

// ---- Render targets ------------------------------------------------------------------

struct Target {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0, levels = 1;
    GLenum fmt = 0;
    bool made() const { return tex != 0; }
};

// ---- GL state: one save/restore for every return path -----------------------------------
//
// Constructed at the top of every public call that touches GL state; the destructor puts
// back everything listed below, whatever path the call returns by. Impl methods that touch
// GL state assume one is alive in their caller (nesting is fine: an inner one restores to
// the outer's moment). Saved: draw and read framebuffer, viewport, scissor test and box,
// blend enable/funcs/equations, clear colour, FRAMEBUFFER_SRGB (when the context has the
// switch), the active unit and TEXTURE_2D on units 0..7, the program, the VAO, the array
// buffer, the pixel pack/unpack buffers, and the pack and unpack alignment, row length
// and skips (Task 11's mean-probe read-back changes the pack side).
// decode_scene() turns the scene texture's hardware decode on; the destructor turns it
// back to SKIP_DECODE first, so anyone else reading the scene sees today's raw bytes.
// glGet* of this state is answered from Mesa's client-side copy: no GPU sync.
class TubeGlState {
public:
    explicit TubeGlState(bool have_srgb_switch);
    ~TubeGlState();
    TubeGlState(const TubeGlState&) = delete;
    TubeGlState& operator=(const TubeGlState&) = delete;

    void decode_scene(GLuint scene_tex);

private:
    bool srgb_switch_, framebuffer_srgb_ = false, scissor_ = false, blend_ = false;
    GLint draw_fbo_ = 0, read_fbo_ = 0, viewport_[4]{}, scissor_box_[4]{};
    GLint blend_src_rgb_ = 0, blend_dst_rgb_ = 0, blend_src_a_ = 0, blend_dst_a_ = 0, blend_eq_rgb_ = 0,
          blend_eq_a_ = 0;
    GLfloat clear_[4]{};
    GLint active_unit_ = 0, tex2d_[kTubeTextureUnits]{};
    GLint program_ = 0, vao_ = 0, array_buffer_ = 0, pack_buffer_ = 0, unpack_buffer_ = 0;
    GLint unpack_alignment_ = 4, unpack_row_length_ = 0, unpack_skip_rows_ = 0, unpack_skip_pixels_ = 0;
    GLint pack_alignment_ = 4, pack_row_length_ = 0, pack_skip_rows_ = 0, pack_skip_pixels_ = 0;
    GLuint decoded_scene_ = 0;
};

// ---- The renderer's state ----------------------------------------------------------------

struct TubeRenderer::Impl {
    explicit Impl(std::string guard_path);

    // Capabilities (probed once per context).
    Caps caps;
    bool caps_probed = false;
    using PfnBeginQuery = void (*)(GLenum, GLuint);
    using PfnEndQuery = void (*)(GLenum);
    using PfnGetQueryObjectui64 = void (*)(GLuint, GLenum, GLuint64*);
    PfnBeginQuery begin_query = nullptr;          // GL_EXT_disjoint_timer_query, for Task 11
    PfnEndQuery end_query = nullptr;
    PfnGetQueryObjectui64 get_query_ui64 = nullptr;
    void probe_caps();

    // The guard (read once, at construction) and the build permission.
    app::TubeBuildGuard guard;
    bool builds_allowed = false;
#ifdef MDB_TUBE_PREVIEW
    std::string force_hang;   // MDB_TUBE_FORCE_HANG, read once (Task 14 Step 4)
#endif

    // The program cache: one slot per tube::Program, in enum order.
    std::array<ProgramSlot, tube::kProgramCount> programs{};
    ProgramSlot& slot(tube::Program p) { return programs[static_cast<std::size_t>(p)]; }
    bool build(tube::Program p, tube::SceneDecode decode);   // compile, link, locations; false logs the info log
    void free_program(tube::Program p);

    // THE DRAWN-PROGRAM BOOKKEEPING (for Task 11). A slot's `drawn` is false from its build
    // until its first draw through its real pass. The marker names the CURRENT plan's
    // programs that are built-or-about-to-be but not drawn; it is written (or rewritten)
    // whenever that set changes and removed when it is empty. Task 11's composite():
    //   - calls mark_drawn(p) right after each program's first real draw (the glass bake
    //     below already does it for the glass);
    //   - calls settle_marker() as it returns (on every path - the passes it did draw are
    //     marked; an undrawn one keeps the marker, correctly, because its compile is still
    //     ahead). settle_marker() removes the marker once the plan has nothing undrawn.
    // A built-but-undrawn program that leaves the plan stays built; the marker stops
    // naming it, and names it again when a plan that draws it is prepared.
    //
    // WHERE THE MARKER IS REMOVED: settle_marker() once the plan has nothing undrawn; a
    // prepare() whose plan has nothing undrawn; record_failure(); end_tube_scene() (Tube
    // stopped drawing, so nothing undrawn can hang - it also forgets current_plan, so the
    // next prepare() writes the marker again); release_gl(); ~TubeRenderer(). The guard's
    // end() only ever removes a marker this process armed.
    std::optional<tube::PassPlan> current_plan;
    tube::SceneDecode current_decode = tube::SceneDecode::Shader;
    // THE FAST PATH: true after a prepare() that returned true for current_plan and
    // current_decode, until anything that could change its answer - release_gl(),
    // end_tube_scene(), a recorded failure. prepare() with the same plan and decode then
    // returns true before any allocation or GL query. The drawn state need not be compared:
    // drawing only shrinks the undrawn set, and settle_marker() keeps the marker in step.
    bool prepared_ok = false;
    // settle_marker() found nothing undrawn for the prepared plan: drawing only shrinks
    // that set, so until prepare() takes its full path again (which clears this) the marker
    // is known empty and settle_marker() returns before undrawn_of() allocates.
    bool marker_clean = false;
    std::string marker_text;   // what the marker on disk says; "" = no marker written by us
    void mark_drawn(tube::Program p) { slot(p).drawn = true; }
    void settle_marker();
    std::string undrawn_of(const tube::PassPlan& plan) const;   // space-separated names
    void write_marker(const std::string& names);

    // The build-failure record: cleared by a different plan or retry_builds().
    bool failed = false;
    // The scene texture would not make a complete framebuffer as SRGB8_ALPHA8: sticky (a
    // different plan does not clear it, or every frame would try again) until
    // retry_builds() or release_gl(); prepare() refuses meanwhile.
    bool scene_srgb_failed = false;
    std::optional<tube::PassPlan> failed_plan;
    tube::SceneDecode failed_decode = tube::SceneDecode::Shader;
    void record_failure(const char* what);

    // Render targets by TargetSizes (ensure_targets), formats by caps.
    Target quarter, glow_tmp, glow_out, history[2], signal, lines, glass, mean;
    int history_index = 0;    // the history written this frame; the other is `prev`
    bool glass_valid = false; // the glass target holds a bake: bake when !glass_valid OR glass_changed
    GLenum signal_format() const;   // RGBA16F with half-float targets, else SRGB8_ALPHA8
    // Makes what the plan needs at these sizes, frees what it no longer needs. Called by
    // composite's draw_frame on EVERY frame: in the steady state it is size and format
    // comparisons only, and it remakes whatever release_targets() or release_gl() freed.
    // False: an FBO was incomplete; the failure is recorded (retried as a build failure).
    bool ensure_targets(const tube::PassPlan& plan, const tube::TargetSizes& sizes);
    bool make_target(Target& t, const char* name, int w, int h, GLenum fmt, int levels);
    static void free_target(Target& t);
    void free_targets();
    void release_targets();   // TubeRenderer::release_targets(): the targets, the probe's read FBO

    // The mask (one period, NEAREST + REPEAT, RGBA8), from DriverFrame::mask on mask_changed.
    GLuint mask_tex = 0;
    int mask_w = 0, mask_h = 0;
    // THE ONE-SHOT INPUTS. FrameDriver hands the mask's pixels (mask_changed) and the
    // glass's change (glass_changed) on ONE frame only. composite() takes them BEFORE any
    // check that can refuse the frame (take_one_shots), so a refused or failed frame
    // cannot lose them:
    //   - the tile is copied into `mask_tile` (one allocation, on a mask_changed frame only;
    //     a newer tile replaces it) and `mask_pending` set; the next frame that reaches the
    //     upload uploads it and clears the flag. The copy is kept after the upload: it IS
    //     the current mask, so release() re-arms mask_pending from it (CPU data outlives a
    //     context), and validation restores it after its synthetic grille.
    //   - glass_changed clears glass_valid, so the next frame that draws re-bakes the glass
    //     from its own uniforms (the glass inputs are still uniforms: the same on every
    //     frame until the next glass_changed).
    tube::MaskTile mask_tile;
    bool mask_pending = false;
    void take_one_shots(const tube::DriverFrame& f);
    void upload_mask(const tube::MaskTile& tile);

    // COLOUR SPACE FOR TASK 11's PASSES: draw every pass into a target with FRAMEBUFFER_SRGB
    // ENABLED (when caps.srgb_write_control; without the switch ES 3.0 always encodes into an
    // sRGB attachment), so quarter / glow / history / mean - and signal / lines on the
    // SRGB8_ALPHA8 fallback - store linear light encoded. It changes nothing for RGBA16F,
    // the RGBA8 glass or the default framebuffer (not sRGB; output.frag encodes itself).
    // Read the scene through TubeGlState::decode_scene() when the decode is Hardware.
    //
    // The glass bake (DriverFrame::glass_changed), into `glass` at the picture's size. The
    // glass program's first draw: it is marked drawn here. False: no glass program or target.
    // Never waits on the GPU: with caps.timer_query the bake is timed by a GL_TIME_ELAPSED_EXT
    // query that poll_glass_timing() reads on a LATER frame, once its result is available;
    // without it (or while an earlier bake's query is still pending) only the CPU submit time
    // is logged. A first bake's CPU submit time includes Mesa's first-draw compile of the
    // program (its line says so; the GPU time does not include it). The first bake after a
    // build logs at INFO, later ones at DEBUG.
    bool bake_glass(const GlEnv& env, const tube::TubeUniforms& u, const tube::TubeDerived& d);
    // Task 11's composite calls this once a frame (cheap when nothing is pending): logs a
    // finished bake query's time, never waiting for one that is not.
    void poll_glass_timing();
    GLuint glass_query = 0;
    bool glass_query_pending = false, glass_query_first_draw = false;

    // Every TubeUniforms field the program lists, tint, and every TubeDerived field it lists.
    static void upload_tube_uniforms(const ProgramSlot& s, const tube::TubeUniforms& u, const tube::TubeDerived& d);
    // The renderer's VAO with a clip-space full-target quad in its VBO (attr 0 position,
    // attr 1 texCoord, four floats a vertex: renderer.cpp's layout).
    static void bind_quad(const GlEnv& env);

    // The scene texture's storage as begin_scene() last specified it (0: never / forgotten).
    GLuint scene_tex = 0;
    int scene_w = 0, scene_h = 0;
    GLenum scene_format = 0;

    // ---- The frame (tube_renderer_passes.cpp) -------------------------------------------
    // One frame's passes into the real targets and the default framebuffer, for the plan
    // and decode prepare() last succeeded with (current_plan, current_decode). False: nothing usable was drawn (the caller draws Enhanced).
    bool draw_frame(const GlEnv& env, video::IRect pic, const tube::DriverFrame& f);
    // One pass's draw call: records the program for this frame's drawn list (committed
    // by draw_frame() only once the frame ends without a GL error) and, on its first real
    // draw, the CPU time across the call (Mesa's synchronous variant compile is in it).
    void draw_pass(tube::Program p);
    std::array<tube::Program, tube::kProgramCount> drawn_now{};
    std::size_t drawn_now_count = 0;
    std::array<double, tube::kProgramCount> first_draw_ms{};   // < 0: not first-drawn since build
    // validate_all_programs(): glGetError after every draw, so a failure names its program.
    bool validating = false;
    // The end of validate_all_programs(): glass invalid, mask texture deleted, history
    // cleared, probe abandoned, the plan forgotten. Keeps every program and its drawn flag.
    void forget_synthetic_state(bool have_srgb_switch);
    std::optional<tube::Program> validate_failed_in;
    // The plan whose GL error was last logged (log once per distinct plan).
    std::optional<tube::PassPlan> gl_error_logged_plan;
    bool foreign_error_logged = false;   // a GL error pending BEFORE composite (not ours)
    // composite()'s caller-error refusals, each logged once (refuse_frame()).
    bool size_mismatch_logged = false, missing_mask_logged = false, bad_env_logged = false,
         scene_not_srgb_logged = false;
    // Refuses a frame BEFORE prepare(): stands down (prepared_ok cleared, so prepare()'s full
    // path rewrites the marker when the caller's error is gone) and removes a marker this
    // process wrote - a file operation on the first refused frame only (write_marker("")
    // is a no-op once the marker is gone). No GL, no allocation.
    void refuse_frame();

    // A 1x1 black SRGB8_ALPHA8 texture for every sampler a program declares but this plan
    // does not feed (the glow when the plan dropped it): no sampler reads an incomplete
    // texture, and the format matches what the sampler reads when the pass exists.
    GLuint black_tex = 0;
    GLuint black();

    // THE MEAN PROBE. Never waits, one read-back in flight:
    //   idle    -> (requested, plan has the probe) the picture into `mean` (quarter.frag),
    //              glGenerateMipmap, fence A                                   -> mipped
    //   mipped  -> (fence A signalled: the GPU has finished the mips) glReadPixels of
    //              level 6 into the PBO, fence B                                -> reading
    //   reading -> (fence B signalled) map 4 bytes, decode, the sink           -> idle
    // Every check is glClientWaitSync(fence, 0, 0). The read-back is issued only once the
    // mips are finished, so even a driver that serves a PBO read on the CPU (mapping the
    // texture) finds it idle and does not wait - the likely cause of the spike's stall,
    // which read back in the same frame it drew the probe.
    enum class MeanStage : std::uint8_t { idle, mipped, reading };
    MeanStage mean_stage = MeanStage::idle;
    bool mean_requested = false;
    std::function<void(double)> mean_sink;
    GLuint mean_read_fbo = 0, mean_pbo = 0;
    GLsync mean_fence = nullptr;
    void poll_mean();
    void abandon_mean();
    void run_mean_probe(const GlEnv& env, video::IRect pic);

    // THE DEBUG GPU TIMERS (MDB_TUBE_GPU_TIMING=1, read once at construction). A ring of
    // four sets of GL_TIME_ELAPSED_EXT queries, one per PassTimingAverager pass, made the
    // first time a set is used (none exist with the variable unset). A set's results are
    // read only once all are available; a set still pending when its turn comes round is
    // dropped. Queries never nest: one pass query open at a time (timer_open), and the
    // glass bake's own query is begun and ended inside bake_glass(), which draw_frame()
    // calls before any pass query is opened.
    static constexpr std::size_t kTimedPasses = 7;
    static constexpr std::size_t kTimerSets = 4;
    struct TimerSet {
        std::array<GLuint, kTimedPasses> q{};
        std::array<bool, kTimedPasses> pending{};
        bool any_pending() const {
            for (bool p : pending)
                if (p) return true;
            return false;
        }
    };
    bool gpu_timing = false;
    std::array<TimerSet, kTimerSets> timer_ring{};
    std::size_t timer_slot = 0;
    bool timer_open = false;
    unsigned long timer_dropped = 0;
    tube::PassTimingAverager timing;
    void collect_timers();
    void timer_begin(tube::PassTimingAverager::Pass pass);
    void timer_end();
    void free_timers();

    void release();   // every GL object, ids zeroed, scene storage forgotten
};

}  // namespace ui
