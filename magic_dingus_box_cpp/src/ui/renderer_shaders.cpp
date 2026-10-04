// ui::Renderer — Every GLSL source the Renderer owns and the functions that compile
// them: the 2D UI program, the legacy CRT overlay, the enhanced-CRT
// composite and the bloom downsample.
//
// One of several translation units implementing ui::Renderer (split
// out of renderer.cpp by responsibility; the class API in renderer.h
// is unchanged). Shared private bits live in renderer_internal.h.

// Declaration-only include — the implementation TU is
// utils/stb_image_impl.cpp, so renderer edits stop recompiling stb.
#include "../utils/stb_image.h"

#include "renderer.h"
#include "renderer_internal.h"

#include "theme.h"
#include "crt_time.h"
#include "font_manager.h"
#include "settings_menu.h"
#include "controller_wizard.h"
#include "pairing_screen.h"
#include "pairing_screen_renderer.h"
#include "virtual_keyboard.h" // Added for virtual keyboard rendering
#include "qrcodegen.hpp" // QR code generation
#include "text_utf8.h"
#include "../app/app_state.h"
#include "../app/playlist_loader.h"
#include "../app/settings_persistence.h" // For getting config if needed
#include "../utils/config.h"

#ifdef MEDIA_BROWSER_ENABLED
#include "../media_browser/artwork/artwork_cache.h"
#include "../platform/platform_profile.h"
#endif

#include <GLES3/gl3.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <cstring> // strlen — was reaching us transitively through
                   // stb_image's implementation until that moved to its
                   // own TU (utils/stb_image_impl.cpp)
#include <iomanip>
#include <vector>
#include <algorithm> // Added for std::min/max

namespace ui {

// Simple vertex shader for 2D rendering
static const char* vertex_shader_source = R"(
#version 300 es
precision mediump float;
in vec2 position;
in vec2 texCoord;
out vec2 vTexCoord;
uniform vec2 screenSize;

void main() {
    vec2 normalizedPos = (position / screenSize) * 2.0 - 1.0;
    normalizedPos.y = -normalizedPos.y;  // Flip Y
    gl_Position = vec4(normalizedPos, 0.0, 1.0);
    vTexCoord = texCoord;
}
)";

// Simple fragment shader
static const char* fragment_shader_source = R"(
#version 300 es
precision highp float;
in vec2 vTexCoord;
out vec4 fragColor;
uniform vec4 color;
uniform sampler2D tex;
uniform bool useTexture;

void main() {
    if (useTexture) {
        vec4 texColor = texture(tex, vTexCoord);
        // Standard alpha blending: multiply texture RGB by color RGB, multiply alphas
        // This ensures text looks the same whether over solid background or video
        fragColor = texColor * color;
    } else {
        fragColor = color;
    }
}
)";

// CRT Fragment Shader
static const char* crt_fragment_shader_source = R"(
#version 300 es
precision mediump float;
in vec2 vTexCoord;
out vec4 fragColor;

uniform float time;
uniform float scanlineIntensity;
uniform float warmthIntensity;
uniform float glowIntensity;
uniform float rgbMaskIntensity;
uniform float bloomIntensity;
uniform float interlacingIntensity;
uniform float flickerIntensity;
uniform vec2 screenSize;

void main() {
    vec4 color = vec4(0.0, 0.0, 0.0, 0.0); // Start transparent
    
    // 1. Scanlines (Horizontal)
    // Use gl_FragCoord.y for pixel-perfect lines, or vTexCoord.y for resolution-independent
    if (scanlineIntensity > 0.0) {
        float scanline = sin(vTexCoord.y * screenSize.y * 3.14159); // Simple sine wave
        // Map [-1, 1] to [1-intensity, 1]
        // We want dark lines, so we subtract alpha
        float line = 0.5 + 0.5 * scanline;
        color = vec4(0.0, 0.0, 0.0, scanlineIntensity * (1.0 - line));
    }
    
    // 2. Interlacing (Vertical jitter or alternate lines)
    if (interlacingIntensity > 0.0) {
        // Simple odd/even line darkening based on time
        float odd = mod(gl_FragCoord.y, 2.0);
        float flicker = mod(time * 30.0, 2.0); // 30Hz flicker
        if (abs(odd - flicker) < 0.5) {
            color.a = max(color.a, interlacingIntensity * 0.5);
        }
    }
    
    // 3. RGB Mask - handled below after finalRGB/finalAlpha declaration
    
    // 4. Phosphor Glow (Vignette)
    if (glowIntensity > 0.0) {
        vec2 uv = vTexCoord * 2.0 - 1.0; // [-1, 1]
        float dist = length(uv);
        // Darken corners
        float vignette = smoothstep(0.5, 1.5, dist);
        color.a = max(color.a, glowIntensity * vignette);
    }
    
    // 5. Warmth (Orange/Red tint)
    vec4 warmthColor = vec4(1.0, 0.9, 0.8, 0.0); // Warm tint
    if (warmthIntensity > 0.0) {
        // We can't easily "tint" the underlying video with a black overlay
        // We need to draw a colored overlay with alpha
        // But 'color' variable so far is black overlay.
        // We need to mix in the warmth.
        // This shader is getting complicated because we are mixing "darkening" (scanlines) and "tinting" (warmth).
        // Let's output the warmth as a separate component or just mix it into fragColor.
        // For now, let's just add a warm overlay.
        // We'll handle this by outputting a color that isn't just black.
    }
    
    // 6. Flicker (Global brightness modulation)
    if (flickerIntensity > 0.0) {
        float f = sin(time * 10.0) * sin(time * 23.0);
        // Random-ish flicker
        color.a = max(color.a, flickerIntensity * 0.1 * (f + 1.0));
    }

    // Final composition
    // We are drawing a "filter" layer. 
    // Most effects are "darkening" (scanlines, vignette, mask).
    // Warmth is "tinting".
    // Bloom is "brightening".
    
    // If we want to support all, we might need to change blend mode or do multiple passes.
    // For "Low Taxing", we stick to one pass.
    // Standard alpha blending: src * alpha + dst * (1-alpha)
    // If src is black (0,0,0), we darken.
    // If src is orange, we tint.
    
    vec3 finalRGB = vec3(0.0, 0.0, 0.0); // Default to black (darkening)
    float finalAlpha = color.a;
    
    if (warmthIntensity > 0.0) {
        // Add warmth: reddish color, low alpha
        finalRGB += vec3(1.0, 0.6, 0.2) * warmthIntensity; // Orange-ish
        finalAlpha = max(finalAlpha, warmthIntensity * 0.2);
    }
    
    if (bloomIntensity > 0.0) {
        // Bloom: simplified as a center glow?
        // Or just a general brightness boost?
        // We can't boost brightness with standard alpha blending over opaque background easily without additive blending.
        // But we are in standard alpha blending mode.
        // Let's skip bloom for now or make it a white overlay (foggy).
        // Let's make it a subtle white glow in the center
        vec2 uv = vTexCoord * 2.0 - 1.0;
        float dist = length(uv);
        float centerGlow = 1.0 - smoothstep(0.0, 1.0, dist);
        finalRGB += vec3(1.0, 1.0, 1.0) * bloomIntensity * centerGlow * 0.2;
        finalAlpha = max(finalAlpha, bloomIntensity * centerGlow * 0.1);
    }

    // RGB Mask (subpixel column darkening)
    if (rgbMaskIntensity > 0.0) {
        float m = mod(gl_FragCoord.x, 3.0);
        float r = smoothstep(1.0, 0.0, abs(m - 0.0));
        float g = smoothstep(1.0, 0.0, abs(m - 1.0));
        float b = smoothstep(1.0, 0.0, abs(m - 2.0));
        vec3 mask = vec3(r, g, b) * 0.5 + 0.5;
        vec3 darkening = vec3(1.0) - mask;
        finalRGB = mix(finalRGB, vec3(0.0), darkening * rgbMaskIntensity * 0.5);
        finalAlpha = max(finalAlpha, rgbMaskIntensity * length(darkening) * 0.3);
    }

    fragColor = vec4(finalRGB, finalAlpha);
}
)";

// Bloom Downsample Fragment Shader (Phase 5 — halation chain)
//
// Used by the two downsample passes that build the half-res and
// quarter-res halation textures sampled by the main composite. A
// Kawase 5-tap pattern (center weight 4, four diagonals at 1.0 src
// texels offset, sum / 8) gives a soft Gaussian-equivalent kernel at
// minimum cost. The first pass (scene → bloom_a) gates contributions
// by luma so only bright pixels feed the halation; the second pass
// (bloom_a → bloom_b) runs ungated to broaden the spread.
//
// Why Kawase over a separable Gaussian: at this kernel size, Kawase
// produces a near-identical result with 5 samples instead of the
// 9-13 you'd want for a Gaussian of equivalent radius, and the
// downsample-chain pattern naturally widens the effective radius
// without needing a large per-pass kernel.
static const char* bloom_downsample_fragment_shader_source = R"(
#version 300 es
precision mediump float;
in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D srcTexture;
uniform vec2 srcTexelSize;     // 1.0 / src_dimensions, for Kawase offsets
uniform float lumaThreshold;   // 0 = pass everything; >0 = soft luma gate

const vec3 LUMA_NTSC = vec3(0.299, 0.587, 0.114);

void main() {
    // Same Y-flip rule as the main composite: srcTexture was rendered
    // by the kiosk's screen-space (top-down) vertex shader, so screen
    // top is at high texel-y. Sampling with vTexCoord directly would
    // pull the bottom when displaying the top.
    vec2 uv = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    vec2 d = srcTexelSize;

    // Kawase 5-tap: center * 4 + four diagonals * 1, divide by 8.
    // Diagonals at 1 src-texel offset = 0.5 dst-pixel offset for a
    // 2:1 downsample, the canonical Bjørge dual-filter pattern.
    vec3 c = texture(srcTexture, uv).rgb * 4.0;
    c += texture(srcTexture, uv + vec2(-d.x, -d.y)).rgb;
    c += texture(srcTexture, uv + vec2( d.x, -d.y)).rgb;
    c += texture(srcTexture, uv + vec2(-d.x,  d.y)).rgb;
    c += texture(srcTexture, uv + vec2( d.x,  d.y)).rgb;
    c /= 8.0;

    // Soft-knee luma threshold. smoothstep gives a gentle ramp from
    // (threshold) to (threshold + 0.2) so we don't get hard edges
    // around the bright/dark boundary in the bloom map.
    if (lumaThreshold > 0.0) {
        float luma = dot(c, LUMA_NTSC);
        float w = smoothstep(lumaThreshold, lumaThreshold + 0.2, luma);
        c *= w;
    }

    fragColor = vec4(c, 1.0);
}
)";

// CRT Composite Fragment Shader (Phases 1-5 of enhanced pipeline)
//
// This shader runs ONLY in the enhanced CRT pipeline. It samples the
// scene texture (the offscreen FBO containing the kiosk video + UI
// composite) and produces final pixels for the default framebuffer.
//
// Effects implemented so far:
//   Phase 1: structural — sample-then-blend (substrate for the rest).
//   Phase 2: brightness-modulated Gaussian scanlines (replaces the
//            naive sine-wave) AND Lottes-style aperture-grille
//            subpixel mask (replaces the simple column-darkening).
//   Phase 3: Color Warmth replaced with gamma boost (2.2→2.4 at full
//            intensity) + a per-channel D65→warm-white temperature
//            shift. Both modulate sceneRGB rather than overlaying an
//            orange film.
//   Phase 4: Phosphor Glow upgraded — the existing vignette is now
//            paired with RGB convergence error. Same slider drives
//            both phenomena (radial-falloff edge imperfections).
//   Phase 5: Screen Bloom replaced with luma-driven halation. Two
//            offscreen downsample passes build a bright-only blurred
//            texture (1/4 res); we screen-blend it back into the
//            output here, modeling the way phosphor light bleeds
//            into surrounding pixels around bright regions of a
//            real CRT.
//
// Physical apply order:
//   1. Sample scene from FBO (with optional convergence-offset RGB
//      sub-samples, gated by glowIntensity)
//   2. Color warmth (input signal coloration / phosphor aging)
//   3. Scanlines (electron beam modulates display)
//   4. Mask (phosphor structure modulates display)
//   5. Alpha-blend overlay (post-display surface effects: vignette
//      darkening, interlacing, flicker)
//   6. Screen-blend halation/bloom (luma-driven phosphor light bleed)
static const char* crt_composite_fragment_shader_source = R"(
#version 300 es
precision mediump float;
in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D sceneTexture;
uniform sampler2D bloomTexture;     // Phase 5 — quarter-res halation map.
                                    // Sampled only when bloomIntensity > 0;
                                    // C++ side ensures a valid texture is
                                    // bound on unit 1 in that case.
uniform float time;
uniform float scanlineIntensity;
uniform float warmthIntensity;
uniform float glowIntensity;
uniform float rgbMaskIntensity;
uniform float bloomIntensity;
uniform float interlacingIntensity;
uniform float flickerIntensity;
uniform vec2 screenSize;

// NTSC luma weights — used to brightness-modulate scanline beam width.
// Bright pixels get a wider beam (line gaps less visible), dark pixels
// get a narrow beam (gaps clearly visible). This is what gives real
// CRTs that "highlight bloom into adjacent lines" character.
const vec3 LUMA_NTSC = vec3(0.299, 0.587, 0.114);

// Approximate NTSC active-scanline count. 240 lines is the historical
// NTSC interlaced active region; on a 720p HDMI output that gives each
// scanline ~3 vertical pixels (1.5 bright + 1.5 gap), which reads as
// "scanlines" without aliasing on the operator's display. Hard-coded
// rather than derived from screenSize so the look is mode-stable
// (CRT_NATIVE 1280x720 vs. Modern TV 1280x720 letterboxed produce the
// same scanline density).
const float SCANLINE_COUNT = 240.0;

void main() {
    // Sample the underlying scene (video + UI composite from the FBO).
    //
    // Y-flip the sample UV: the kiosk's 2D vertex shader applies
    // `normalizedPos.y = -normalizedPos.y` so screen-space (0,0) is at
    // top-left. When the kiosk renders into the scene FBO, geometry
    // drawn at screen-top ends up at the TOP of the FBO texture in
    // sample space. But OpenGL texture coordinates place (0,0) at the
    // BOTTOM-left. The composite quad's vTexCoord goes 0→1 top→bottom
    // (matching screen-space), so without the flip we'd sample the
    // bottom of the FBO when displaying the top of the screen — the
    // operator-visible symptom is "everything on the screen is upside
    // down". One-line fix: invert the V coordinate at sample time.
    // vTexCoord.y itself is left untouched because the per-pixel effect
    // math (scanlines, glow vignette, bloom) wants screen-space Y, not
    // texture-space Y.
    vec2 sample_uv_base = vec2(vTexCoord.x, 1.0 - vTexCoord.y);

    // ===== Phase 4: RGB convergence error (paired with Phosphor Glow) =====
    //
    // In a real CRT the three electron guns (R/G/B) must converge on
    // the same target pixel. Edge-of-screen geometry is the hardest
    // for the deflection coils to keep aligned, so the further from
    // center a pixel is, the more the channels can drift apart. The
    // visible result is subtle color fringing toward the corners.
    //
    // Implementation:
    //   - Compute a centered radial direction in vTexCoord space.
    //   - Magnitude grows quadratically with radial distance — so the
    //     center is sharp and only the corners visibly misconverge.
    //   - Sample R outward, B inward, G at the base UV. This pattern
    //     matches the typical magnetic-misconvergence character of an
    //     aged consumer CRT (R drifts out, B drifts in).
    //   - Y component of the offset is flipped because radial_dir is
    //     in vTexCoord space (top-down) but the sample UV is in
    //     OpenGL texture space (bottom-up).
    //
    // Hijacks the Phosphor Glow slider because both phenomena are
    // "phosphor / beam imperfections that get worse near the edges"
    // — same mental model for the operator. At glowIntensity=0 we
    // skip the extra samples entirely and pay only one texture fetch.
    //
    // Cost: 2 extra texture samples (only on the active branch).
    // Pi 4 V3D handles ~6.7 GTexel/s; at 1280x720 = ~922k frags this
    // adds ~0.3ms per frame. Cheap.
    vec3 sceneRGB;
    if (glowIntensity > 0.0) {
        vec2 center = vTexCoord * 2.0 - 1.0;            // -1..+1
        float radial = length(center);
        vec2 radial_dir = (radial > 1e-4) ? center / radial : vec2(0.0);

        // Quadratic falloff. At corners (radial≈√2) and full slider,
        // max offset is ~0.005 in UV ≈ 6.4 px on a 1280-wide FBO —
        // visible but not garish. Slider scales the strength linearly.
        float conv_amt = radial * radial * glowIntensity * 0.005;

        // Y flip in offset because radial_dir is top-down but sample
        // space is bottom-up.
        vec2 r_offset =  radial_dir * conv_amt * vec2(1.0, -1.0);
        vec2 b_offset = -radial_dir * conv_amt * vec2(1.0, -1.0);

        float r = texture(sceneTexture, sample_uv_base + r_offset).r;
        float g = texture(sceneTexture, sample_uv_base).g;
        float b = texture(sceneTexture, sample_uv_base + b_offset).b;
        sceneRGB = vec3(r, g, b);
    } else {
        sceneRGB = texture(sceneTexture, sample_uv_base).rgb;
    }

    // ===== Phase 3: Color Warmth — gamma boost + D65→warm temp shift =====
    //
    // Two physically-motivated adjustments are applied jointly under
    // the single "Color Warmth" intensity slider, replacing the v1.4.3
    // orange-tint alpha overlay (which crushed saturation and looked
    // artificial against bright scene content).
    //
    // (a) Gamma boost. sRGB content is encoded for a 2.2-gamma display.
    //     Real CRTs naturally produce light at ~2.4-2.5 gamma — the
    //     mismatch is what gives a CRT its punchy, rich-blacks
    //     character. To emulate this on a modern 2.2 display, we
    //     re-encode the scene with the higher target gamma:
    //         out = pow(in, target_gamma / 2.2)
    //     At target 2.4 the exponent is 1.091, which crushes near-black
    //     midtones meaningfully without clipping highlights. We lerp
    //     the exponent toward 1.0 (identity, no boost) at intensity 0,
    //     so the slider remains a clean OFF/ON control.
    //
    // (b) Temperature shift. D65 (~6500K) is the modern display white
    //     point; aged consumer-grade CRTs drift warmer (5000K-5500K),
    //     subtly attenuating the green and blue channels. Modeled as
    //     a 3-component per-channel multiplier (cheaper than a full
    //     3x3 matrix and avoids the hue-rotation artifacts a generic
    //     matrix would introduce; warmth is a saturation-preserving
    //     channel-balance shift, not a hue rotation).
    //
    // Cost: 1 pow + 1 mix per channel, ~6 ALU ops total; negligible.
    if (warmthIntensity > 0.0) {
        // (a) Gamma boost. mix(1.0, 1.091, intensity) = 1.0 at 0,
        //     1.091 at 1.0. CRT target gamma 2.4 / sRGB source 2.2.
        float gamma_exp = mix(1.0, 2.4 / 2.2, warmthIntensity);
        sceneRGB = pow(sceneRGB, vec3(gamma_exp));

        // (b) D65 → ~5000K. The classic warm-white attenuation
        //     pattern for an aged tube. Slider lerps from neutral
        //     (1, 1, 1) to warm (1.00, 0.92, 0.82).
        vec3 warm_white = vec3(1.00, 0.92, 0.82);
        vec3 warm_factor = mix(vec3(1.0), warm_white, warmthIntensity);
        sceneRGB *= warm_factor;
    }

    // ===== Phase 2a: Brightness-modulated Gaussian scanlines =====
    //
    // Real CRT scanlines are not uniform: the electron beam paints a
    // line whose intensity falls off in a Gaussian envelope, and the
    // beam *widens* as signal voltage (brightness) increases. So:
    //   - Dark areas: narrow beam → wide black gaps → lines very visible
    //   - Bright areas: wide beam → gaps fill in → lines fade
    //
    // Implementation:
    //   1. Compute a per-fragment distance from the nearest scanline
    //      center, in scanline-row units (range -0.5..+0.5).
    //   2. Compute beam width modulated by luma. We use a baseline of
    //      ~0.30 row units widening to ~0.45 in pure white (cubic
    //      ramp keeps the fade subtle in midtones, dramatic in
    //      highlights — matches real-beam I-V curves).
    //   3. Gaussian weight = exp(-d²/(2σ²)). Centered = 1.0, falls off.
    //   4. Modulate scene by weight → scanline-darkened scene.
    //
    // Intensity slider behavior:
    //   intensity 0.0 → modulation = 1.0 everywhere (identity, no-op)
    //   intensity 0.5 → gap brightness clamps to 0.5 (subtle)
    //   intensity 1.0 → gap brightness goes to 0 (full black gaps)
    //
    // Cost: ~5 ALU ops per pixel; negligible on Pi 4 V3D at 720p.
    if (scanlineIntensity > 0.0) {
        float row_pos = vTexCoord.y * SCANLINE_COUNT;
        float dist = fract(row_pos) - 0.5;            // -0.5..+0.5
        float luma = dot(sceneRGB, LUMA_NTSC);        // 0..1
        float beam_width = 0.30 + 0.15 * luma * luma * luma;
        float gauss = exp(-(dist * dist) / (2.0 * beam_width * beam_width));
        // Mix between (1 - intensity) at gap centers and 1.0 at line
        // centers. At intensity 0.0, mix(1,1,gauss) = 1.0 → no effect.
        float gap_brightness = 1.0 - scanlineIntensity;
        float scanline_mod = mix(gap_brightness, 1.0, gauss);
        sceneRGB *= scanline_mod;
    }

    // ===== Phase 2b: Lottes-style aperture-grille subpixel mask =====
    //
    // A Trinitron-style aperture-grille mask: every column of HDMI
    // pixels emits one of R/G/B at full strength while the other two
    // channels are attenuated. This produces the characteristic
    // vertical-stripe color-pattern of a real CRT, AND because we
    // multiply the scene rather than darken it with an overlay, the
    // colors at the subpixel are physically tied to what the scene
    // contains (a red object actually paints the R subpixels, etc.).
    //
    // Implementation: take gl_FragCoord.x mod 3 to get a phase
    // 0/1/2 → R/G/B subpixel column, attenuate the other two channels
    // by `mask_dark`. The legacy shader did something similar but
    // applied via overlay alpha and used smoothstep blending; this
    // version is per-channel multiply, which is what a CRT's phosphor
    // dots actually do.
    //
    // Intensity slider behavior:
    //   intensity 0.0 → mask = (1,1,1) → no-op
    //   intensity 0.5 → off-channel atten = 0.75 (subtle stripe)
    //   intensity 1.0 → off-channel atten = 0.50 (classic Lottes)
    //
    // Cost: 1 mod, 1 if-else chain, 3 mults; trivial.
    if (rgbMaskIntensity > 0.0) {
        // 0.5 = Lottes default attenuation at full intensity. Slider
        // scales linearly between identity (1.0) and full (0.5).
        float mask_dark = 1.0 - 0.5 * rgbMaskIntensity;
        float phase = mod(gl_FragCoord.x, 3.0);
        vec3 mask;
        if (phase < 1.0) {
            mask = vec3(1.0, mask_dark, mask_dark);   // Red column
        } else if (phase < 2.0) {
            mask = vec3(mask_dark, 1.0, mask_dark);   // Green column
        } else {
            mask = vec3(mask_dark, mask_dark, 1.0);   // Blue column
        }
        sceneRGB *= mask;
    }

    // ===== Phase 1 effects (legacy alpha-blend overlay path) =====
    //
    // Warmth, glow vignette, bloom, interlacing, flicker still use the
    // procedural-overlay computation: a (color, alpha) tuple is built
    // up and then alpha-blended over the (now scanline+mask-modulated)
    // sceneRGB. Phases 3-5 will replace these one at a time. Until
    // then, the visual character of these effects matches v1.4.3 so
    // operators can A/B compare with the classic path.
    vec4 color = vec4(0.0);

    // Interlacing (alternate-line darkening, time-flickered)
    if (interlacingIntensity > 0.0) {
        float odd = mod(gl_FragCoord.y, 2.0);
        float interlace_phase = mod(time * 30.0, 2.0);
        if (abs(odd - interlace_phase) < 0.5) {
            color.a = max(color.a, interlacingIntensity * 0.5);
        }
    }

    // Phosphor Glow (vignette — edge darkening)
    if (glowIntensity > 0.0) {
        vec2 uv = vTexCoord * 2.0 - 1.0;
        float dist = length(uv);
        float vignette = smoothstep(0.5, 1.5, dist);
        color.a = max(color.a, glowIntensity * vignette);
    }

    // Flicker (low-amplitude global wobble)
    if (flickerIntensity > 0.0) {
        float f = sin(time * 10.0) * sin(time * 23.0);
        color.a = max(color.a, flickerIntensity * 0.1 * (f + 1.0));
    }

    vec3 finalRGB = vec3(0.0);
    float finalAlpha = color.a;

    // (Warmth was here in Phase 1/2; promoted to a scene-multiplying
    // gamma + temperature pass at the top of main() in Phase 3 because
    // an alpha overlay can't model the steeper electron-beam gamma
    // curve and ends up just dyeing the screen orange. See "Phase 3"
    // block above.)

    // (Center-white-glow "fake bloom" was here in Phase 1/2; replaced
    // by luma-driven halation sampled from the bloomTexture below in
    // Phase 5. The fake bloom couldn't react to scene content — it
    // just lit the center of the screen white regardless of what was
    // playing. Real CRT halation glows around bright regions wherever
    // they are, which is what the bloom downsample chain produces.)

    // Inline the legacy alpha blend: src OVER scanline+mask-modulated scene.
    vec3 outRGB = finalRGB * finalAlpha + sceneRGB * (1.0 - finalAlpha);

    // ===== Phase 5: Halation/bloom screen-blend =====
    //
    // The bloomTexture (quarter-res, prebuilt by two downsample passes
    // before this composite runs) holds a soft Gaussian-equivalent
    // blur of the scene's bright regions only. Screen-blend it into
    // outRGB so highlights radiate phosphor light into surrounding
    // pixels — the characteristic "halo around bright text on dark
    // background" of an old TV.
    //
    // Screen blend formula: out = 1 - (1 - a)·(1 - b). Acts additive
    // for dark a but saturates near 1.0, so highlights brighten and
    // darks barely change without anyone going past 1.0 and clipping
    // hard. Slider scales the bloom contribution linearly.
    //
    // Same Y-flip rule as scene sampling.
    if (bloomIntensity > 0.0) {
        vec3 bloomRGB = texture(bloomTexture, vec2(vTexCoord.x, 1.0 - vTexCoord.y)).rgb;
        bloomRGB *= bloomIntensity;
        outRGB = vec3(1.0) - (vec3(1.0) - outRGB) * (vec3(1.0) - bloomRGB);
    }

    fragColor = vec4(outRGB, 1.0);
}
)";


bool Renderer::compile_shaders() {
    uint32_t vertex_shader = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    uint32_t fragment_shader = compile_shader(fragment_shader_source, GL_FRAGMENT_SHADER);
    
    if (vertex_shader == 0 || fragment_shader == 0) {
        return false;
    }
    
    shader_program_ = glCreateProgram();
    glAttachShader(shader_program_, vertex_shader);
    glAttachShader(shader_program_, fragment_shader);
    glLinkProgram(shader_program_);
    
    GLint success;
    glGetProgramiv(shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(shader_program_, 512, nullptr, info_log);
        std::cerr << "Shader program linking failed: " << info_log << std::endl;
        return false;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    // Cache the hot uniform locations (see header note). Must run on every
    // (re)link — locations are per-program-object, so the RetroArch-return
    // recompile would silently invalidate previously cached values.
    u_color_loc_ = glGetUniformLocation(shader_program_, "color");
    u_use_texture_loc_ = glGetUniformLocation(shader_program_, "useTexture");
    u_screen_size_loc_ = glGetUniformLocation(shader_program_, "screenSize");

    return true;
}

uint32_t Renderer::compile_shader(const std::string& source, uint32_t type) {
    uint32_t shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(shader, 512, nullptr, info_log);
        std::cerr << "Shader compilation failed: " << info_log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}


bool Renderer::compile_crt_shader() {
    uint32_t vertex_shader = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    uint32_t fragment_shader = compile_shader(crt_fragment_shader_source, GL_FRAGMENT_SHADER);

    if (vertex_shader == 0 || fragment_shader == 0) {
        return false;
    }

    crt_shader_program_ = glCreateProgram();
    glAttachShader(crt_shader_program_, vertex_shader);
    glAttachShader(crt_shader_program_, fragment_shader);
    glLinkProgram(crt_shader_program_);

    GLint success;
    glGetProgramiv(crt_shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(crt_shader_program_, 512, nullptr, info_log);
        std::cerr << "CRT Shader program linking failed: " << info_log << std::endl;
        return false;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    return true;
}

bool Renderer::compile_crt_composite_shader() {
    // Reuse the existing 2D vertex shader — the composite is a
    // fullscreen quad with the same screen-space position layout.
    uint32_t vertex_shader = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    uint32_t fragment_shader = compile_shader(crt_composite_fragment_shader_source, GL_FRAGMENT_SHADER);

    if (vertex_shader == 0 || fragment_shader == 0) {
        return false;
    }

    crt_composite_shader_program_ = glCreateProgram();
    glAttachShader(crt_composite_shader_program_, vertex_shader);
    glAttachShader(crt_composite_shader_program_, fragment_shader);
    glLinkProgram(crt_composite_shader_program_);

    GLint success;
    glGetProgramiv(crt_composite_shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(crt_composite_shader_program_, 512, nullptr, info_log);
        std::cerr << "CRT composite shader program linking failed: " << info_log << std::endl;
        glDeleteProgram(crt_composite_shader_program_);
        crt_composite_shader_program_ = 0;
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    return true;
}

bool Renderer::compile_bloom_downsample_shader() {
    uint32_t vertex_shader = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    uint32_t fragment_shader =
        compile_shader(bloom_downsample_fragment_shader_source, GL_FRAGMENT_SHADER);

    if (vertex_shader == 0 || fragment_shader == 0) {
        return false;
    }

    bloom_downsample_shader_program_ = glCreateProgram();
    glAttachShader(bloom_downsample_shader_program_, vertex_shader);
    glAttachShader(bloom_downsample_shader_program_, fragment_shader);
    glLinkProgram(bloom_downsample_shader_program_);

    GLint success;
    glGetProgramiv(bloom_downsample_shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(bloom_downsample_shader_program_, 512, nullptr, info_log);
        std::cerr << "Bloom downsample shader linking failed: " << info_log << std::endl;
        glDeleteProgram(bloom_downsample_shader_program_);
        bloom_downsample_shader_program_ = 0;
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    return true;
}

} // namespace ui
