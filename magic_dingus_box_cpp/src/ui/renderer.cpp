#define STB_IMAGE_IMPLEMENTATION
#include "../utils/stb_image.h"

#include "renderer.h"

#include "theme.h"
#include "font_manager.h"
#include "settings_menu.h"
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
#endif

#include <GLES3/gl3.h>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <cmath>
#include <chrono>
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

Renderer::Renderer(uint32_t width, uint32_t height)
    : width_(width)
    , height_(height)
    , original_width_(width)
    , original_height_(height)
    , ui_alpha_(1.0f)
    , shader_program_(0)
    , crt_shader_program_(0)
    , crt_composite_shader_program_(0)
    , vao_(0)
    , vbo_(0)
    , logo_texture_id_(0)
    , logo_width_(0)
    , logo_height_(0)
{
    theme_ = std::make_unique<Theme>();
    title_font_manager_ = std::make_unique<FontManager>();
    body_font_manager_ = std::make_unique<FontManager>();
}

Renderer::~Renderer() {
    cleanup();
}

void Renderer::reset_gl() {
    // After an external app (like RetroArch) takes over the EGL context,
    // our GL resources are invalid. We need to delete and re-create them.
    std::cout << "UI Renderer: Resetting GL resources after external context takeover" << std::endl;
    
    // Delete old resources (they may be invalid but try anyway for cleanliness)
    if (shader_program_ != 0) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (crt_shader_program_ != 0) {
        glDeleteProgram(crt_shader_program_);
        crt_shader_program_ = 0;
    }
    if (crt_composite_shader_program_ != 0) {
        glDeleteProgram(crt_composite_shader_program_);
        crt_composite_shader_program_ = 0;
    }
    if (bloom_downsample_shader_program_ != 0) {
        glDeleteProgram(bloom_downsample_shader_program_);
        bloom_downsample_shader_program_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (logo_texture_id_ != 0) {
        glDeleteTextures(1, &logo_texture_id_);
        logo_texture_id_ = 0;
    }
    if (bezel_texture_id_ != 0) {
        glDeleteTextures(1, &bezel_texture_id_);
        bezel_texture_id_ = 0;
        current_bezel_path_.clear();
    }
    if (marquee_frame_texture_id_ != 0) {
        glDeleteTextures(1, &marquee_frame_texture_id_);
        marquee_frame_texture_id_ = 0;
        marquee_frame_loaded_path_.clear();
    }
    if (thumbnail_texture_id_ != 0) {
        glDeleteTextures(1, &thumbnail_texture_id_);
        thumbnail_texture_id_ = 0;
        current_thumbnail_path_.clear();
    }
    // Tear down the scene FBO and bloom chain; they'll be lazily
    // re-created on next begin_scene_fbo() / first bloom-active frame
    // respectively. RetroArch (or any external GL takeover) may have
    // invalidated framebuffer object handles along with shader
    // programs, so we don't try to reuse them.
    destroy_scene_fbo();
    destroy_bloom_fbos();
    for (auto& pair : system_logo_cache_) {
        if (pair.second.texture_id != 0) {
            glDeleteTextures(1, &pair.second.texture_id);
        }
    }
    system_logo_cache_.clear();

    // Reset font manager GL resources (keep font data for re-rasterization)
    if (title_font_manager_) {
        title_font_manager_->reset_textures();
    }
    if (body_font_manager_) {
        body_font_manager_->reset_textures();
    }
    
    // Re-compile shaders
    if (!compile_shaders()) {
        std::cerr << "UI Renderer: Failed to re-compile shaders after reset" << std::endl;
    } else {
        std::cout << "UI Renderer: Shaders recompiled, program_id=" << shader_program_ << std::endl;
    }
    if (!compile_crt_shader()) {
        std::cerr << "UI Renderer: Failed to re-compile CRT shader after reset" << std::endl;
    } else {
        std::cout << "UI Renderer: CRT shader recompiled, program_id=" << crt_shader_program_ << std::endl;
    }
    if (!compile_crt_composite_shader()) {
        std::cerr << "UI Renderer: Failed to re-compile CRT composite shader after reset" << std::endl;
    } else {
        std::cout << "UI Renderer: CRT composite shader recompiled, program_id="
                  << crt_composite_shader_program_ << std::endl;
    }
    if (!compile_bloom_downsample_shader()) {
        std::cerr << "UI Renderer: Failed to re-compile bloom downsample shader after reset" << std::endl;
    } else {
        std::cout << "UI Renderer: Bloom downsample shader recompiled, program_id="
                  << bloom_downsample_shader_program_ << std::endl;
    }

    // Re-create VAO/VBO
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
    
    // Re-load logo texture using config paths
    unsigned char* data = nullptr;
    int channels;
    std::vector<std::string> logo_paths = config::get_logo_search_paths();
    for (const auto& logo_path : logo_paths) {
        data = stbi_load(logo_path.c_str(), &logo_width_, &logo_height_, &channels, 4);
        if (data) break;
    }
    
    if (data) {
        glGenTextures(1, &logo_texture_id_);
        glBindTexture(GL_TEXTURE_2D, logo_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo_width_, logo_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        stbi_image_free(data);
    }
    
    // CRITICAL: Re-enable blending - RetroArch may have disabled it
    // Without this, all UI elements become invisible!
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    std::cout << "UI Renderer: GL resources reset complete (blending enabled)" << std::endl;
}

bool Renderer::load_bezel(const std::string& path) {
    // Don't reload if already loaded
    if (path == current_bezel_path_ && bezel_texture_id_ != 0) {
        return true;
    }
    
    // Delete old texture if exists
    if (bezel_texture_id_ != 0) {
        glDeleteTextures(1, &bezel_texture_id_);
        bezel_texture_id_ = 0;
    }
    
    current_bezel_path_ = path;
    
    if (path.empty()) {
        // No bezel requested
        return true;
    }
    
    // Try multiple paths using config
    std::vector<std::string> bezel_paths = {
        "../assets/bezels/" + path,
        "assets/bezels/" + path,
        config::get_bezels_dir() + "/" + path
    };
    
    unsigned char* data = nullptr;
    int channels;
    
    for (const auto& bezel_path : bezel_paths) {
        data = stbi_load(bezel_path.c_str(), &bezel_width_, &bezel_height_, &channels, 4);
        if (data) {
            std::cout << "Loaded bezel from: " << bezel_path << " (" << bezel_width_ << "x" << bezel_height_ << ")" << std::endl;
            break;
        }
    }
    
    if (!data) {
        std::cerr << "Failed to load bezel: " << path << std::endl;
        return false;
    }
    
    glGenTextures(1, &bezel_texture_id_);
    glBindTexture(GL_TEXTURE_2D, bezel_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bezel_width_, bezel_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    stbi_image_free(data);
    return true;
}

void Renderer::render_bezel() {
    if (bezel_texture_id_ == 0) return;
    
    // Bind our shader program and set up projection
    glUseProgram(shader_program_);
    
    // Use ORIGINAL screen dimensions for bezel (fullscreen overlay)
    // Not width_/height_ which may be reduced for 4:3 content viewport
    float bezel_w = static_cast<float>(original_width_);
    float bezel_h = static_cast<float>(original_height_);
    
    // Set screenSize uniform for the shader (uses screen coords divider)
    glUniform2f(u_screen_size_loc_, bezel_w, bezel_h);
    
    // Render bezel as fullscreen textured quad
    float x = 0.0f;
    float y = 0.0f;
    
    float vertices[] = {
        x, y,             0.0f, 0.0f,
        x + bezel_w, y,   1.0f, 0.0f,
        x, y + bezel_h,   0.0f, 1.0f,
        x + bezel_w, y + bezel_h, 1.0f, 1.0f
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    // Use white color with full alpha to render texture as-is
    glUniform4f(u_color_loc_, 1.0f, 1.0f, 1.0f, 1.0f);
    glUniform1i(u_use_texture_loc_, 1);
    
    // Ensure we are using Texture Unit 0 and tell the shader
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(shader_program_, "tex"), 0);
    
    // Enable blending for transparent areas of the bezel
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glBindTexture(GL_TEXTURE_2D, bezel_texture_id_);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
}

// =====================================================================
// Marquee "TV cabinet" frame
// =====================================================================
// Renders the wood-frame overlay used ONLY on Marquee (Media Browser)
// screens. Contract:
//
//   - Asset is a single 1280×720 PNG with TRANSPARENT CENTER and an
//     opaque ~40 px wood frame painted along all four edges (mitered
//     corners, beveled inner highlight). Identical render path to the
//     existing bezel: one full-screen textured quad with alpha blending.
//   - The 40 px is baked into the PNG art — the renderer doesn't need
//     to know the thickness; it just lays the whole image at 1280×720.
//   - Layout code in later Marquee screens treats `kFrameInsetPx = 40`
//     as the safe-area inset (plus an extra ~20 px breathing room
//     between the frame's inner edge and content).
//   - Drawn on top of all Marquee content (after MB screen render,
//     before toast) so the frame visually "covers" the outer pixels
//     of the CRT-processed output, producing the design's inset-CRT
//     effect without needing a real CRT-region change.
//
// Why GL_CLAMP_TO_EDGE (not GL_REPEAT): the asset is a complete frame
// at native canvas size; we draw it 1:1 across the full screen, never
// outside [0,1] UV. Repeat mode would still work but clamping is the
// correct semantic for a non-tiled overlay.
bool Renderer::load_marquee_frame(const std::string& path) {
    if (path == marquee_frame_loaded_path_ && marquee_frame_texture_id_ != 0) {
        return true;
    }
    if (marquee_frame_texture_id_ != 0) {
        glDeleteTextures(1, &marquee_frame_texture_id_);
        marquee_frame_texture_id_ = 0;
    }
    marquee_frame_loaded_path_ = path;
    if (path.empty()) {
        return true;
    }

    std::vector<std::string> candidates = {
        "../assets/marquee/" + path,
        "assets/marquee/" + path,
        config::get_assets_path() + "/marquee/" + path,
    };

    unsigned char* data = nullptr;
    int channels = 0;
    for (const auto& p : candidates) {
        data = stbi_load(p.c_str(), &marquee_frame_tile_w_, &marquee_frame_tile_h_, &channels, 4);
        if (data) {
            std::cout << "Loaded Marquee frame overlay: " << p
                      << " (" << marquee_frame_tile_w_ << "x" << marquee_frame_tile_h_ << ")" << std::endl;
            break;
        }
    }
    if (!data) {
        std::cerr << "Failed to load Marquee frame overlay: " << path << std::endl;
        return false;
    }

    glGenTextures(1, &marquee_frame_texture_id_);
    glBindTexture(GL_TEXTURE_2D, marquee_frame_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 marquee_frame_tile_w_, marquee_frame_tile_h_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    return true;
}

void Renderer::render_marquee_frame() {
    if (marquee_frame_texture_id_ == 0) return;

    glUseProgram(shader_program_);

    // Full-screen overlay using ORIGINAL dimensions, not the (possibly
    // letterboxed) content viewport — the frame is a fixed cabinet that
    // owns the whole HDMI canvas.
    const float screen_w = static_cast<float>(original_width_);
    const float screen_h = static_cast<float>(original_height_);
    glUniform2f(u_screen_size_loc_, screen_w, screen_h);
    glUniform4f(u_color_loc_, 1.0f, 1.0f, 1.0f, 1.0f);
    glUniform1i(u_use_texture_loc_, 1);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(shader_program_, "tex"), 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, marquee_frame_texture_id_);

    float vertices[] = {
        0.0f,     0.0f,     0.0f, 0.0f,
        screen_w, 0.0f,     1.0f, 0.0f,
        0.0f,     screen_h, 0.0f, 1.0f,
        screen_w, screen_h, 1.0f, 1.0f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::set_content_viewport(int width, int height) {
    // Temporarily override width and height for 4:3 rendering
    // This affects the projection matrix used by all render methods
    static bool logged = false;
    if (!logged) {
        std::cout << "UI Renderer: set_content_viewport(" << width << ", " << height << ") - was " << width_ << "x" << height_ << std::endl;
        logged = true;
    }
    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);
}

void Renderer::reset_content_viewport() {
    // Restore original screen dimensions
    width_ = original_width_;
    height_ = original_height_;
}


void Renderer::resize_screen(uint32_t width, uint32_t height) {
    original_width_ = width;
    original_height_ = height;
    reset_content_viewport();
}

void Renderer::set_framebuffer_size(uint32_t width, uint32_t height) {
    framebuffer_width_ = width;
    framebuffer_height_ = height;
}

bool Renderer::initialize(const std::string& title_font_path, const std::string& body_font_path) {
    std::cout << "  Initializing UI renderer..." << std::endl;
    std::cout << "    Title font path: " << title_font_path << std::endl;
    std::cout << "    Body font path: " << body_font_path << std::endl;
    if (!compile_shaders()) {
        return false;
    }
    
    if (!compile_crt_shader()) {
        std::cerr << "Warning: Failed to compile CRT shader, effects will be disabled" << std::endl;
    }
    if (!compile_crt_composite_shader()) {
        // Non-fatal — falling back means enhanced_crt_enabled requests
        // will silently behave like the legacy path. The legacy CRT
        // shader still works for procedural overlays.
        std::cerr << "Warning: Failed to compile CRT composite shader; "
                     "enhanced CRT pipeline disabled" << std::endl;
    }
    if (!compile_bloom_downsample_shader()) {
        // Also non-fatal — main composite will skip the bloom-sample
        // path when bloomIntensity > 0 but the chain isn't available.
        // (The composite shader itself doesn't crash on a missing
        // bloomTexture; the bind happens only when intensity > 0
        // AND the FBOs were successfully built.)
        std::cerr << "Warning: Failed to compile bloom downsample shader; "
                     "halation will be disabled" << std::endl;
    }

    // Load title font (Zen Dots) for title and heading
    if (!title_font_manager_->load_font(title_font_path, theme_->font_title_size)) {
        std::cerr << "Warning: Failed to load title font, falling back to body font" << std::endl;
        // Fallback to body font if title font fails
        if (!title_font_manager_->load_font(body_font_path, theme_->font_title_size)) {
            std::cerr << "ERROR: Failed to load any font" << std::endl;
            return false;
        }
    }
    
    // Load body font (mono) for playlist items and footer
    if (!body_font_manager_->load_font(body_font_path, theme_->font_medium_size)) {
        std::cerr << "Warning: Failed to load body font, text rendering may not work" << std::endl;
        return false;
    }
    
    // Set up VAO/VBO for quad rendering
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    
    // Vertex attributes: position (2), texCoord (2)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
    
    // Enable blending for transparency
    // Use standard alpha blending for consistent text rendering (same whether over video or not)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Ensure smooth text rendering - disable dithering which can cause blockiness
    glDisable(GL_DITHER);
    
    // Load Logo using config paths
    std::vector<std::string> logo_paths = config::get_logo_search_paths();

    unsigned char* data = nullptr;
    int channels;
    std::string loaded_logo_path;

    for (const auto& path : logo_paths) {
        data = stbi_load(path.c_str(), &logo_width_, &logo_height_, &channels, 4); // Force RGBA
        if (data) {
            loaded_logo_path = path;
            break;
        }
    }
    
    if (data) {
        glGenTextures(1, &logo_texture_id_);
        glBindTexture(GL_TEXTURE_2D, logo_texture_id_);
        
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo_width_, logo_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        
        stbi_image_free(data);
        std::cout << "Loaded logo from: " << loaded_logo_path << " (" << logo_width_ << "x" << logo_height_ << ")" << std::endl;
    } else {
        std::cerr << "Failed to load logo from any location" << std::endl;
    }

    return true;
}

void Renderer::render(app::AppState& state) {
    // Debug logging removed for performance - only log errors
    
    // CRITICAL: Don't render UI at all when intro video is showing (even if not ready yet)
    // This prevents UI from briefly appearing before intro video starts
    // Also don't render if intro is loading but not ready yet
    // BUT render if we are fading out (transition to UI)
    if (state.showing_intro_video && !state.intro_fading_out) {
        return;  // Don't render UI during intro video playback (unless fading out)
    }
    
    // NOTE: glViewport is set by the caller (main.cpp) for proper 4:3 centering in Modern TV mode
    // Do NOT set glViewport here as it would override the centered position
    
    // Conditional clearing based on video state
    // NOTE: When video is active, mpv has already rendered to the framebuffer,
    // so we should NOT clear - we want to preserve the video frame
    // Also, if ui_visible_when_playing is true, a video was just started, so don't clear
    // Also, if we have a valid playlist index, we're transitioning between videos, so don't clear
    // IMPORTANT: After intro video completes, we want to clear the screen to show clean UI
    bool is_transitioning = (state.current_playlist_index >= 0 && state.current_item_index >= 0);
    bool should_clear = (!state.video_active && !state.ui_visible_when_playing && !is_transitioning) || 
                        (state.intro_complete && !state.video_active);  // Clear after intro completes
    if (should_clear) {
        // No video and no video loading: clear with background color (UI should always show when no video)
        // Also clear after intro video completes to remove any lingering video frames
        glClearColor(
            theme_->bg.r / 255.0f,
            theme_->bg.g / 255.0f,
            theme_->bg.b / 255.0f,
            1.0f
        );
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // When video is active or loading, we don't clear - mpv already rendered (or will render) the video frame
    
    // Calculate UI overlay alpha based on fade state
    float ui_overlay_alpha = 1.0f;
    
    // If we're transitioning between videos, don't show UI at all
    if (is_transitioning && !state.video_active) {
        return;  // Don't render UI during video transitions
    }
    
    // Handle fade animation (works for both video active and intro fade-in cases)
    if (state.is_fading) {
        // Calculate fade progress
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.fade_start_time);
        if (elapsed < state.fade_duration) {
            float fade_progress = static_cast<float>(elapsed.count()) / static_cast<float>(state.fade_duration.count());
            fade_progress = std::min(1.0f, std::max(0.0f, fade_progress));  // Clamp to [0, 1]
            
            if (state.fade_target_ui_visible) {
                // Fading in: alpha from 0 to 1
                ui_overlay_alpha = fade_progress;
            } else {
                // Fading out: alpha from 1 to 0
                ui_overlay_alpha = 1.0f - fade_progress;
            }
        } else {
            // Fade complete
            ui_overlay_alpha = state.fade_target_ui_visible ? 1.0f : 0.0f;
        }
    } else if (state.video_active) {
        // Video active but not fading - use current visibility state
        ui_overlay_alpha = state.ui_visible_when_playing ? 1.0f : 0.0f;
    }

    // Fullscreen-playback fast path: skip the entire UI render when nothing
    // would be visible anyway. The previously commented-out early-return at
    // ui_overlay_alpha == 0 was disabled because CRT effects need the
    // pipeline to run; we now correctly gate on whether ANY CRT effect is
    // active AND whether other always-visible UI elements (scrub bar,
    // settings menu) are showing. Saves ~3-5% CPU + reduces GPU contention
    // with v4l2h264dec on the Pi 4 during fullscreen 1080p playback.
    if (ui_overlay_alpha <= 0.0f) {
        const auto& s = state.display_settings;
        const bool any_crt_effect = (s.scanline_intensity   > 0.0f ||
                                     s.warmth_intensity     > 0.0f ||
                                     s.glow_intensity       > 0.0f ||
                                     s.rgb_mask_intensity   > 0.0f ||
                                     s.bloom_intensity      > 0.0f ||
                                     s.interlacing_intensity > 0.0f ||
                                     s.flicker_intensity    > 0.0f);
        const bool settings_open = state.settings_menu &&
            (state.settings_menu->is_active()  ||
             state.settings_menu->is_opening() ||
             state.settings_menu->is_closing());
        const bool seek_visible = state.show_seek_bar;
        if (!any_crt_effect && !settings_open && !seek_visible) {
            // Nothing would draw; skip the rest of the UI render entirely.
            return;
        }
    }
    // Otherwise (no video active), render UI normally (ui_overlay_alpha = 1.0f from initial value)
    
    // CRITICAL: Reset OpenGL state after mpv renders to ensure consistent text rendering
    // mpv may change blending, texture state, etc. that affects UI rendering
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DITHER);
    glActiveTexture(GL_TEXTURE0);  // Ensure we're using texture unit 0
    
    glUseProgram(shader_program_);
    if (shader_program_ == 0) {
        std::cerr << "ERROR: Shader program is 0!" << std::endl;
        return;
    }
    
    // Set screen size uniform
    GLint screenSizeLoc = u_screen_size_loc_;
    if (screenSizeLoc < 0) {
        std::cerr << "Warning: screenSize uniform not found" << std::endl;
    } else {
        static bool logged_screensize = false;
        if (!logged_screensize) {
            std::cout << "UI Renderer: render() screenSize=" << width_ << "x" << height_ << std::endl;
            logged_screensize = true;
        }
        glUniform2f(screenSizeLoc, static_cast<float>(width_), static_cast<float>(height_));
    }
    
    // Determine alpha multipliers based on video state and fade
    // When video is active and UI is visible: text fully opaque (1.0), backgrounds 50% transparent (0.5)
    // When video is not active: everything fully opaque (1.0)
    // Text alpha is controlled by fade animation (fades in/out with overlay)
    float text_alpha = ui_overlay_alpha;  // Text fades in/out with overlay
    
    // Handle intro video fade-out: draw black overlay that fades in over the video
    if (state.intro_fading_out && state.video_active) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.intro_fade_out_start_time);
        std::chrono::milliseconds fade_out_duration(300);
        
        float fade_out_progress = 1.0f;
        if (elapsed < fade_out_duration) {
            fade_out_progress = static_cast<float>(elapsed.count()) / static_cast<float>(fade_out_duration.count());
            fade_out_progress = std::min(1.0f, std::max(0.0f, fade_out_progress));  // Clamp to [0, 1]
        }
        
        // Draw black overlay that fades in (from transparent to opaque) over the video
        ui::Color black_overlay = {0, 0, 0, static_cast<uint8_t>(255 * fade_out_progress)};  // Fade from 0 to 100% opacity
        draw_quad(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), black_overlay, 1.0f);
    }
    
    // Only render UI components if they are visible
    if (ui_overlay_alpha > 0.0f) {
        // When UI overlay should be visible, draw dark overlay behind text
        // Draw overlay first so it's behind all text elements
        if (state.video_active && !state.intro_fading_out) {
            // Draw a semi-transparent black overlay over the entire screen
            // This darkens the video background while still allowing it to show through
            // The overlay is drawn first so text renders on top of it
            // Alpha is controlled by fade animation
            ui::Color dark_overlay = {0, 0, 0, static_cast<uint8_t>(128 * ui_overlay_alpha)};  // 50% opacity black, scaled by fade
            draw_quad(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), dark_overlay, 1.0f);
        }
        float background_alpha = (state.video_active && state.ui_visible_when_playing) ? 0.5f : 1.0f;
        
        // Render UI components.
        render_title(text_alpha, state.video_active, state.ui_visible_when_playing);

        // Publish the actual on-screen row count to state so
        // main.cpp's input handler scrolls only when the selection
        // really goes off-screen for the current viewport.
        // Mirrors the math inside render_playlist_list — see the
        // detailed comment block there for the derivation.
        {
            const int title_baseline_offset =
                title_font_manager_->get_baseline_at_size(theme_->font_title_size);
            const float title_baseline = 8.0f + title_baseline_offset;
            const int title_line_height =
                static_cast<int>(theme_->font_title_size * 1.2f);
            const float header_baseline = title_baseline + title_line_height + 24.0f;
            const int header_line_height =
                static_cast<int>(theme_->font_heading_size * 1.2f);
            const float start_y =
                header_baseline + header_line_height + 4.0f + 40.0f;
            const float bottom_reserve = 25.0f;
            const float available_h =
                static_cast<float>(height_) - start_y - bottom_reserve;
            const int row_h = std::max(1, theme_->playlist_item_height);
            state.playlist_max_visible =
                std::max(8, static_cast<int>(available_h / row_h));
        }

        render_playlist_list(state.playlists, state.selected_index, state.playlist_scroll_offset, state.video_active, state.ui_visible_when_playing, state.current_playlist_index);
        render_footer(state, text_alpha, state.video_active, state.ui_visible_when_playing);
        
        // Render loading overlay if needed
        if (state.is_loading_game) {
            render_loading_overlay(state);
        }
        
        // background_alpha is calculated but currently not used in individual render functions
        // It's available for future use if needed for background transparency
        (void)background_alpha;  // Suppress unused variable warning
    }
    
    // Render volume overlay (always on top if active)
    render_volume_overlay(state);

    // Render seek progress bar (only during active seeking via rotary encoder)
    render_seek_bar(state);

    // Render error overlay banner (if any error message is set)
    render_error_overlay(state);

    // Render settings menu if active (on top of everything, before scanlines)
    if (state.settings_menu && state.settings_menu->is_active()) {

        if (state.settings_menu->is_pairing_screen_active()) {
            // Phone Remote pairing screen replaces the regular settings panel.
            // Use the shared mtime-based cache so this and main.cpp's input
            // handler always see the same device list (no divergence after
            // forget).
            const auto& cached_devices = ui::paired_devices_cached(
                config::get_data_path() + "/paired_remotes.json");
            ui::PairingScreen* ps = state.settings_menu->pairing_screen();
            if (ps) {
                render_pairing_screen(*ps, cached_devices,
                                      state.lan_ip, state.hostname);
            }
        } else {
        render_settings_menu(state.settings_menu, state.game_playlists, state.video_active, state.ui_visible_when_playing);

        // Render QR code when Info submenu is active
        if (state.settings_menu->get_current_submenu() == ui::MenuSection::INFO) {
            // Compute qr_url + qr_label from the gated usb_url/wifi_url
            // fields directly, NOT from content_manager_url. Both are
            // updated by build_info_submenu in the same pass, but if a
            // stale content_manager_url is ever left over from a prior
            // state (e.g., the user was previously plugged in via USB,
            // unplugged, and the auto-refresh rebuild hadn't yet run
            // when this frame rendered), reading that field could
            // disagree with state.usb_url and produce the bug where
            // the QR encodes one network but the label below it
            // claims the other.
            //
            // build_info_submenu now sets state.usb_url to "" when no
            // USB carrier is present, so a non-empty state.usb_url
            // means USB is genuinely active and preferred. Otherwise
            // fall back to state.wifi_url.
            std::string qr_url;
            std::string qr_label;
            if (!state.usb_url.empty()) {
                qr_url = state.usb_url;
                qr_label = "USB Connection (Preferred)";
            } else if (!state.wifi_url.empty()) {
                qr_url = state.wifi_url;
                qr_label = "Wi-Fi Connection";
            }
            
            if (!qr_url.empty()) {
                // Position QR code centered in the menu panel
                uint32_t menu_width = width_ / 2;
                float menu_x = static_cast<float>(width_) - menu_width;

                // Calculate Y position: simplified menu (Header + Status + Back) takes space
                // Start below the menu items
                float menu_items_end = 280.0f;  // Safe distance below menu items
                float footer_start = static_cast<float>(height_) - 70.0f;
                float available_space = footer_start - menu_items_end;

                // Center QR code + hint text in available space
                float qr_size = 130.0f;  // Slightly optimized size
                // Layout below the QR:
                //   25px padding
                //   line 1: connection-type label (accent color)
                //   20px line gap
                //   line 2: "Scan with phone camera"
                //   20px line gap
                //   line 3: "or visit magicpi.local on any device" (highlight2)
                // Total below the QR: ~85px, so qr_with_hint_height = qr + 85.
                float qr_with_hint_height = qr_size + 85.0f;

                float qr_y = menu_items_end + (available_space - qr_with_hint_height) / 2.0f;
                float qr_x = menu_x + (static_cast<float>(menu_width) - qr_size) / 2.0f;

                render_qr_code(qr_url, qr_x, qr_y, qr_size, 1.0f);

                // Draw connection type label above hint
                int label_width = body_font_manager_->get_text_width(qr_label, theme_->font_small_size);
                float label_x = menu_x + (static_cast<float>(menu_width) - label_width) / 2.0f;
                float label_y = qr_y + qr_size + 25.0f + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
                draw_text(qr_label, label_x, label_y, theme_->font_small_size, theme_->accent, false, 1.0f);

                // Draw helper text below label
                std::string qr_hint = "Scan with phone camera";
                int hint_width = body_font_manager_->get_text_width(qr_hint, theme_->font_small_size);
                float hint_x = menu_x + (static_cast<float>(menu_width) - hint_width) / 2.0f;
                float hint_y = label_y + 20.0f;
                draw_text(qr_hint, hint_x, hint_y, theme_->font_small_size, theme_->fg, false, 1.0f);

                // Draw typed-URL fallback below the QR hint. The hostname
                // magicpi.local resolves via mDNS/Bonjour from any device on
                // the same network (Wi-Fi OR USB-Gadget) — works on macOS,
                // iOS, Linux, and Windows 10+ natively without configuring
                // anything. This line gives the operator a fallback for the
                // case where:
                //   - phone is on the same Wi-Fi as the Pi but the QR is
                //     showing a USB-only URL (would otherwise be unreachable)
                //   - the user prefers typing to scanning
                //   - they want to bookmark a stable URL that works
                //     regardless of which interface is active
                // Distinct color (highlight2) makes the URL pop visually
                // since it's the actionable bit — eye is drawn to it.
                std::string fallback_text = "or visit magicpi.local on any device";
                int fallback_width = body_font_manager_->get_text_width(fallback_text, theme_->font_small_size);
                float fallback_x = menu_x + (static_cast<float>(menu_width) - fallback_width) / 2.0f;
                float fallback_y = hint_y + 20.0f;
                draw_text(fallback_text, fallback_x, fallback_y, theme_->font_small_size, theme_->highlight2, false, 1.0f);
            }
        }
        } // end else (not pairing screen active)
    }

    // Virtual Keyboard overlay
    if (state.keyboard && state.keyboard->is_active()) {
        render_virtual_keyboard(*state.keyboard);
    }
        
    // Apply CRT effects (scanlines, warmth, glow, etc.)
    // These are rendered as an overlay on top of everything in the
    // legacy path. In the enhanced path the call is a no-op
    // (scene_fbo_active_) and the effects are deferred to
    // end_scene_fbo_and_composite, which reads last_scanlines_enabled_
    // because it runs OUTSIDE this function and doesn't see
    // ui_overlay_alpha directly.
    last_scanlines_enabled_ = (ui_overlay_alpha > 0.0f);
    render_crt_effects(state, last_scanlines_enabled_);
    
    // Check for errors after rendering
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "OpenGL error after render: " << err << std::endl;
    }
}

void Renderer::draw_quad(float x, float y, float w, float h, const ui::Color& color, float alpha_multiplier) {
    float vertices[] = {
        x, y,         0.0f, 0.0f,  // Top-left
        x + w, y,     1.0f, 0.0f,  // Top-right
        x, y + h,     0.0f, 1.0f,  // Bottom-left
        x + w, y + h, 1.0f, 1.0f   // Bottom-right
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glUniform4f(u_color_loc_,
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 0);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void Renderer::draw_text(const std::string& text, float x, float y, int font_size, const ui::Color& color, bool use_title_font, float alpha_multiplier) {
    FontManager* font_manager = use_title_font ? title_font_manager_.get() : body_font_manager_.get();
    if (!font_manager || text.empty()) {
        return;
    }
    
    // Use the requested font size
    float current_x = x;
    // Y is the baseline position in screen coordinates (Y increases downward)
    // All glyphs will be aligned to this common baseline
    float baseline_y = y;
    
    std::size_t pos = 0;
    while (pos < text.size()) {
        char32_t c = ::ui::decode_utf8(text, pos);
        if (c == 0) break;

        if (c == U'\n') {
            // Calculate line height for this font size
            int line_height = static_cast<int>(font_size * 1.2f);  // Approximate line height
            baseline_y += line_height;
            current_x = x;
            continue;
        }

        // Skip spaces - they don't need to be rendered, just advance
        if (c == U' ') {
            // Get the space glyph just for its advance width
            ui::Glyph space_glyph = font_manager->get_glyph_at_size(U' ', font_size);
            current_x += space_glyph.advance;
            continue;
        }

        ui::Glyph glyph = font_manager->get_glyph_at_size(c, font_size);
        if (glyph.texture_id == 0) {
            // Glyph not available, just advance
            current_x += glyph.advance;
            continue;
        }
        
        // Draw glyph as a textured quad
        // bearing_y is the distance from baseline to top of bitmap (positive = above baseline)
        // In screen coords (Y down): top_of_bitmap = baseline_y - bearing_y
        float glyph_x = current_x + glyph.bearing_x;
        float glyph_y = baseline_y - glyph.bearing_y;  // Top of glyph bitmap
        
        glBindTexture(GL_TEXTURE_2D, glyph.texture_id);

        // Texture params (LINEAR filtering, CLAMP wrap) are per-texture-
        // object state set once at glyph creation (font_manager.cpp
        // rasterize path) and re-applied automatically when glyphs are
        // re-rasterized after reset_textures() (RetroArch return). The
        // per-glyph re-set + glGetTexParameteriv verification that used to
        // live here cost 2 driver READBACKS per glyph per frame (~36k
        // pipeline syncs/sec on a full menu) for a condition that could
        // never be false — removed.

        // Set color uniform
        // IMPORTANT: For text, we want colors to stay vibrant, so we DON'T multiply RGB by ui_alpha_
        // ui_alpha_ is only for background transparency, not text dimming
        // alpha_multiplier controls fade in/out animation, which we do want
        GLint colorLoc = u_color_loc_;
        if (colorLoc >= 0) {
            glUniform4f(colorLoc, color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * alpha_multiplier);
        }
        
        GLint useTextureLoc = u_use_texture_loc_;
        if (useTextureLoc >= 0) {
            glUniform1i(useTextureLoc, 1);
        }
        
        // Draw quad for glyph
        // The shader flips Y coordinate (normalizedPos.y = -normalizedPos.y)
        // So in screen space (Y increases downward), we position glyphs normally
        // glyph_y is the top of the glyph bitmap
        // Texture coordinates: (0,0) is top-left of texture, (1,1) is bottom-right
        float vertices[] = {
            glyph_x, glyph_y,                     0.0f, 0.0f,  // Top-left (screen and texture)
            glyph_x + glyph.width, glyph_y,       1.0f, 0.0f,  // Top-right
            glyph_x, glyph_y + glyph.height,      0.0f, 1.0f,  // Bottom-left
            glyph_x + glyph.width, glyph_y + glyph.height, 1.0f, 1.0f  // Bottom-right
        };
        
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        
        glBindTexture(GL_TEXTURE_2D, 0);
        
        current_x += glyph.advance;
    }
}

void Renderer::draw_glyph(char32_t codepoint, float x, float baseline_y, int font_size, const ui::Color& color, float alpha_multiplier) {
    ui::Glyph glyph = body_font_manager_->get_glyph_at_size(codepoint, font_size);
    if (glyph.texture_id == 0) return;

    float glyph_x = x + glyph.bearing_x;
    float glyph_y = baseline_y - glyph.bearing_y;

    glBindTexture(GL_TEXTURE_2D, glyph.texture_id);
    // Filtering params live on the texture object (set at glyph creation);
    // no per-draw re-set needed — see the matching note in draw_text().

    GLint colorLoc = u_color_loc_;
    if (colorLoc >= 0) {
        glUniform4f(colorLoc, color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * alpha_multiplier);
    }
    GLint useTextureLoc = u_use_texture_loc_;
    if (useTextureLoc >= 0) glUniform1i(useTextureLoc, 1);

    float vertices[] = {
        glyph_x, glyph_y,                                     0.0f, 0.0f,
        glyph_x + glyph.width, glyph_y,                       1.0f, 0.0f,
        glyph_x, glyph_y + glyph.height,                      0.0f, 1.0f,
        glyph_x + glyph.width, glyph_y + glyph.height,        1.0f, 1.0f
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::draw_line(float x1, float y1, float x2, float y2, float width, const ui::Color& color, float alpha_multiplier) {
    // Draw line as a thin quad
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len == 0.0f) return;
    
    float perp_x = -dy / len * width / 2.0f;
    float perp_y = dx / len * width / 2.0f;
    
    float vertices[] = {
        x1 + perp_x, y1 + perp_y,  0.0f, 0.0f,
        x2 + perp_x, y2 + perp_y,  1.0f, 0.0f,
        x1 - perp_x, y1 - perp_y,  0.0f, 1.0f,
        x2 - perp_x, y2 - perp_y,  1.0f, 1.0f
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glUniform4f(u_color_loc_,
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 0);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void Renderer::render_virtual_keyboard(const VirtualKeyboard& keyboard) {
    // Darken background
    draw_quad(0, 0, width_, height_, theme_->bg, 0.8f);
    
    float kb_width = width_ * 0.8f;
    float kb_height = height_ * 0.5f;
    float start_x = (width_ - kb_width) / 2.0f;
    float start_y = (height_ - kb_height) / 2.0f + 50.0f;
    
    // Background panel
    draw_quad(start_x - 20, start_y - 80, kb_width + 40, kb_height + 100, theme_->bg, 1.0f);
    draw_line(start_x - 20, start_y - 80, start_x + kb_width + 20, start_y - 80, 2.0f, theme_->accent2);
    draw_line(start_x - 20, start_y + kb_height + 20, start_x + kb_width + 20, start_y + kb_height + 20, 2.0f, theme_->accent2);
    
    // Title
    draw_text(keyboard.get_title(), start_x, start_y - 50, 24, theme_->fg, true);
    
    // Text buffer (Input box)
    float input_box_height = 40.0f;
    draw_quad(start_x, start_y - 20, kb_width, input_box_height, theme_->bg, 1.0f);
    draw_line(start_x, start_y - 20 + input_box_height, start_x + kb_width, start_y - 20 + input_box_height, 2.0f, theme_->accent);
    
    // Text cursor blinking
    std::string display_text = keyboard.get_text();
    // Simple cursor visualization
    if (static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 500) % 2 == 0) {
        display_text += "_";
    }
    draw_text(display_text, start_x + 10, start_y - 12, 20, theme_->fg);
    
    // Keys
    const auto& layout = keyboard.get_layout();
    float key_margin = 5.0f;
    float keys_area_height = kb_height - 20; // approximate
    float row_height = keys_area_height / layout.size();
    
    for (int r = 0; r < (int)layout.size(); ++r) {
        const auto& row = layout[r];
        float row_width = kb_width;
        float key_width = row_width / row.size();
        
        for (int c = 0; c < (int)row.size(); ++c) {
            float kx = start_x + c * key_width;
            float ky = start_y + 40 + r * row_height;
            float kw = key_width - key_margin;
            float kh = row_height - key_margin;
            
            bool selected = (r == keyboard.get_selected_row() && c == keyboard.get_selected_col());
            
            ui::Color bg_color = selected ? theme_->accent : theme_->action;
            ui::Color text_color = selected ? theme_->bg : theme_->fg;
            
            draw_quad(kx, ky, kw, kh, bg_color);
            
            // Center text
            std::string label = row[c];
            // Adjust special labels if needed
            int font_size = 20;
            if (label.length() > 1) font_size = 16;
            
            // Proper font-based centering
            float text_width = static_cast<float>(body_font_manager_->get_text_width(label, font_size));
            float tx = kx + (kw - text_width) / 2.0f + (font_size * 0.2f); // minor adjustment
            // Center text vertically
            // draw_text uses y as baseline. To center vertically, baseline should be lower.
            // approx baseline = top + (height + font_size/2) / 2
            float ty = ky + (kh + font_size * 0.6f) / 2.0f;
            
            draw_text(label, tx, ty, font_size, text_color);
        }
    }
}


// ... (existing includes)



// ...

void Renderer::render_title(float text_alpha, bool /* video_active */, bool /* ui_visible_when_playing */) {
    // Render Logo instead of text
    if (logo_texture_id_ != 0) {
        // Calculate centered position
        // User requested "low enough... not being chopped off"
        float logo_y = 20.0f; // Top margin
        float logo_x = (width_ - logo_width_) / 2.0f;
        
        // Draw logo quad
        // We need to bind the texture and draw a quad
        // We can reuse draw_quad but it takes a color, we need a textured quad method or modify draw_quad
        // Actually draw_text does textured quads.
        // Let's manually draw it here to be safe and simple
        
        float x = logo_x;
        float y = logo_y;
        float w = static_cast<float>(logo_width_);
        float h = static_cast<float>(logo_height_);
        
        float vertices[] = {
            x, y,         0.0f, 0.0f,
            x + w, y,     1.0f, 0.0f,
            x, y + h,     0.0f, 1.0f,
            x + w, y + h, 1.0f, 1.0f
        };
        
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        
        // Use white color to render texture as-is (multiplied by alpha)
        glUniform4f(u_color_loc_,
                    1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha);
        glUniform1i(u_use_texture_loc_, 1); // Enable texture
        
        glBindTexture(GL_TEXTURE_2D, logo_texture_id_);
        
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        
        glBindTexture(GL_TEXTURE_2D, 0);
        
    } else {
        // Fallback to text if logo failed to load
        std::string product_title = "Magic Dingus Box";
        int title_width = title_font_manager_->get_text_width(product_title);
        float title_x = (static_cast<float>(width_) - title_width) / 2.0f;
        int title_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_title_size);
        float title_baseline = 8.0f + title_baseline_offset;
        
        draw_text(product_title, title_x, title_baseline, theme_->font_title_size, theme_->accent, true, text_alpha);
        
        float underline_y = title_baseline + 10.0f;
        draw_line(title_x, underline_y, title_x + title_width, underline_y, 2.0f, theme_->accent2, text_alpha);
    }
    
    // PLAYLISTS section header (use title font)
    // Calculate proper spacing based on line heights
    // If logo is used, base it on logo height
    float header_baseline;
    if (logo_texture_id_ != 0) {
        header_baseline = 20.0f + logo_height_ + 40.0f; // Logo Y + Height + Spacing
    } else {
        int title_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_title_size);
        float title_baseline = 8.0f + title_baseline_offset;
        int title_line_height = static_cast<int>(theme_->font_title_size * 1.2f);
        header_baseline = title_baseline + title_line_height + 24.0f;
    }

    std::string header = "Playlists";
    // Use the correct font size for width calculation to match rendering
    int header_width = title_font_manager_->get_text_width(header, theme_->font_heading_size);
    
    // Align the header with the playlist titles (offset by 36.0f to skip numbers)
    float header_x = static_cast<float>(theme_->margin_x) + 36.0f;
    
    draw_text(header, header_x, header_baseline, theme_->font_heading_size, theme_->accent2, true, text_alpha);
    
    // Underline for playlists header - matching text width
    float header_line_y = header_baseline + 10.0f;
    draw_line(header_x, header_line_y,
              header_x + header_width, header_line_y, 2.0f, theme_->accent2, text_alpha);
}

void Renderer::render_playlist_list(const std::vector<app::Playlist>& playlists, int selected_index, int scroll_offset, bool video_active, bool ui_visible_when_playing, int current_playlist_index) {
    // Debug: Log playlist rendering
    static int playlist_render_count = 0;
    if (playlist_render_count < 2) {
        std::cout << "    Rendering playlist list: " << playlists.size() << " playlists, selected=" << selected_index << ", scroll=" << scroll_offset << std::endl;
        playlist_render_count++;
    }
    
    // Start after title and playlists header
    // Calculate proper positions using font-size-specific baselines
    int title_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_title_size);
    float title_baseline = 8.0f + title_baseline_offset;
    int title_line_height = static_cast<int>(theme_->font_title_size * 1.2f);
    
    float header_baseline = title_baseline + title_line_height + 24.0f;
    int header_line_height = static_cast<int>(theme_->font_heading_size * 1.2f);
    
    // Increased spacing below header line (was 20.0f, now 40.0f)
    float start_y = header_baseline + header_line_height + 4.0f + 40.0f;
    
    // Compute how many rows fit between start_y and the bottom of
    // the screen.
    //
    // Was hardcoded `max_visible = 8`, sized for CRT native (640x480).
    // In Modern TV the UI viewport is 4:3 within 16:9 (~960x720), so
    // there's room for many more rows. With the old cap, the playlist
    // visibly stopped ~2/3 down the screen even with 20 items.
    //
    // The playlist column lives on the LEFT side of the screen; the
    // footer (status text, time, video info) is right-aligned and
    // does not overlap, so we don't need to clear it. The only thing
    // below the last visible row is the down-scroll arrow:
    //
    //   arrow_y      = last_row_bottom + arrow_margin/2  (≈ +6)
    //   arrow_height = arrow_size * 1.2                  (≈ 7)
    //   total tail   ≈ 13 px below last row
    //
    // Plus a small breathing-room margin so the arrow isn't flush
    // against the screen edge. bottom_reserve = 13 (arrow) + 12
    // (margin) = 25.
    //
    // Modern TV (720): (720 - 166 - 25) / 36 = 14 rows  → last bottom 670, 37px free.
    // CRT (480):       (480 - 166 - 25) / 36 = 8 rows   → last bottom 454, 13px free.
    //
    // Floor at 8 stays as a safety net (CRT is already 8 by formula
    // but a future tweak to start_y or font sizes could push it to 7
    // and silently regress the established CRT layout — the floor
    // protects against that).
    const float bottom_reserve = 25.0f;  // down-arrow tail + breathing room
    const float available_h = static_cast<float>(height_) - start_y - bottom_reserve;
    const int row_h = std::max(1, theme_->playlist_item_height);
    int max_visible = std::max(8, static_cast<int>(available_h / row_h));
    if (max_visible > static_cast<int>(playlists.size())) {
        max_visible = static_cast<int>(playlists.size());
    }
    
    // Determine alpha multipliers based on video state
    // When video is active and UI is visible: text fully opaque (1.0), backgrounds 50% transparent (0.5)
    // When video is not active: everything fully opaque (1.0)
    float text_alpha = (video_active && ui_visible_when_playing) ? 1.0f : 1.0f;
    // background_alpha is no longer used in this function (highlight bar uses fixed alpha)
    // Removed to eliminate unused variable warning
    
    // Get current time for blinking indicator (time-based, matching Python: 500ms)
    static auto last_blink_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_blink_time).count();
    bool indicator_visible = (elapsed_ms / 500) % 2 == 0;  // Blink every 500ms (matching Python)
    
    // Calculate if we need to show scroll arrows
    int total_playlists = static_cast<int>(playlists.size());
    bool show_up_arrow = scroll_offset > 0;
    bool show_down_arrow = scroll_offset + max_visible < total_playlists;
    
    // Reserve space for arrows if needed
    float arrow_size = 6.0f;  // Smaller arrows for better spacing
    float arrow_margin = 12.0f;
    // Position arrows aligned with center of "Playlists" header text
    // Header starts at margin_x + 36.0f, "Playlists" is ~100px wide, so center is ~50px in
    float header_x = static_cast<float>(theme_->margin_x) + 36.0f;
    float header_text_width = static_cast<float>(title_font_manager_->get_text_width("Playlists", theme_->font_heading_size));
    float arrow_center_x = header_x + (header_text_width / 2.0f);  // Center of "Playlists" text
    float y = start_y;
    
    // If showing up arrow, draw it just above the first visible playlist item
    // Position it symmetrically with how the down arrow is positioned below the last item
    if (show_up_arrow) {
        float arrow_y = start_y - arrow_margin;  // Position above start of playlist area
        
        // Draw triangle pointing UP
        float triangle_vertices[] = {
            arrow_center_x - arrow_size, arrow_y + arrow_size * 1.2f, 0.0f, 0.0f,  // Bottom-left
            arrow_center_x + arrow_size, arrow_y + arrow_size * 1.2f, 1.0f, 0.0f,  // Bottom-right  
            arrow_center_x, arrow_y,                                  0.5f, 1.0f   // Top (pointing up)
        };
        
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
        
        GLint colorLoc = u_color_loc_;
        if (colorLoc >= 0) {
            glUniform4f(colorLoc, theme_->accent2.r / 255.0f, theme_->accent2.g / 255.0f, 
                       theme_->accent2.b / 255.0f, (theme_->accent2.a / 255.0f) * ui_alpha_ * text_alpha);
        }
        GLint useTextureLoc = u_use_texture_loc_;
        if (useTextureLoc >= 0) {
            glUniform1i(useTextureLoc, 0);
        }
        
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }
    
    // Render visible playlists starting from scroll_offset
    for (int i = 0; i < max_visible && (scroll_offset + i) < total_playlists; i++) {
        int playlist_idx = scroll_offset + i;
        const auto& pl = playlists[playlist_idx];
        bool selected = (playlist_idx == selected_index);
        
        // Selection highlight bar (subtle background - very transparent so it doesn't block text)
        if (selected) {
            float highlight_x = static_cast<float>(theme_->margin_x) + 32.0f;
            float highlight_y = y - 2.0f;
            float highlight_w = static_cast<float>(width_) - highlight_x - static_cast<float>(theme_->margin_x);
            float highlight_h = static_cast<float>(theme_->playlist_item_height) + 4.0f;
            
            ui::Color highlight_color = theme_->accent2;
            highlight_color.a = 20;  // Very subtle alpha (reduced from 40) so text is clearly visible
            // Don't apply background_alpha multiplier to highlight - keep it consistently subtle
            draw_quad(highlight_x, highlight_y, highlight_w, highlight_h, highlight_color, 1.0f);
        }
        
        // Channel number - regular playlists start from 01 (Master Shuffle has no number)
        char channel_buf[16];
        snprintf(channel_buf, sizeof(channel_buf), "%02d.", playlist_idx);
        std::string channel_num = channel_buf;

        // Playlist title only (curator removed per user request)
        std::string text = pl.title;

        int font_size = selected ? theme_->font_large_size : theme_->font_medium_size;
        bool is_now_playing = (playlist_idx == current_playlist_index && current_playlist_index >= 0);
        ui::Color text_color = selected ? theme_->accent2 : (is_now_playing ? theme_->highlight1 : theme_->fg);
        ui::Color channel_color = is_now_playing ? theme_->highlight1 : theme_->dim;

        // Use a common baseline for all text on this line (based on the larger font)
        int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
        float item_baseline = y + item_baseline_offset;

        float text_x = static_cast<float>(theme_->margin_x) + 36.0f;

        if (playlist_idx == 0) {
            // Draw shuffle icon (crossed arrows) instead of channel number for Master Shuffle
            float line_height = static_cast<float>(font_size) * 1.2f;
            float icon_cy = y + line_height * 0.45f;
            float icon_x = static_cast<float>(theme_->margin_x) + 2.0f;
            float icon_w = 16.0f;
            float icon_h = 10.0f;
            float half_h = icon_h / 2.0f;
            float arrow_w = 4.0f;
            float line_body = icon_w - arrow_w;
            float t = 1.4f;  // line half-thickness

            // Perpendicular offset for diagonal lines
            float diag_len = sqrtf(line_body * line_body + half_h * half_h);
            float px = (half_h / diag_len) * t;
            float py = (line_body / diag_len) * t;

            // Line endpoints
            float x1 = icon_x,             y1 = icon_cy + half_h;   // bottom-left
            float x2 = icon_x + line_body, y2 = icon_cy - half_h;   // top-right
            float x3 = icon_x,             y3 = icon_cy - half_h;   // top-left
            float x4 = icon_x + line_body, y4 = icon_cy + half_h;   // bottom-right
            float ah = 2.8f;  // arrowhead half-height

            float verts[] = {
                // Line 1: bottom-left → top-right
                x1 - px, y1 - py, 0, 0,   x1 + px, y1 + py, 0, 0,   x2 + px, y2 + py, 0, 0,
                x1 - px, y1 - py, 0, 0,   x2 + px, y2 + py, 0, 0,   x2 - px, y2 - py, 0, 0,
                // Line 2: top-left → bottom-right
                x3 + px, y3 - py, 0, 0,   x3 - px, y3 + py, 0, 0,   x4 - px, y4 + py, 0, 0,
                x3 + px, y3 - py, 0, 0,   x4 - px, y4 + py, 0, 0,   x4 + px, y4 - py, 0, 0,
                // Arrowhead at top-right
                x2, y2 - ah, 0, 0,   x2, y2 + ah, 0, 0,   x2 + arrow_w, y2, 0, 0,
                // Arrowhead at bottom-right
                x4, y4 - ah, 0, 0,   x4, y4 + ah, 0, 0,   x4 + arrow_w, y4, 0, 0,
            };

            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
            GLint colorLoc = u_color_loc_;
            if (colorLoc >= 0) {
                glUniform4f(colorLoc, channel_color.r / 255.0f, channel_color.g / 255.0f,
                           channel_color.b / 255.0f, (channel_color.a / 255.0f) * ui_alpha_ * text_alpha);
            }
            GLint useTextureLoc = u_use_texture_loc_;
            if (useTextureLoc >= 0) glUniform1i(useTextureLoc, 0);
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLES, 0, 18);
            glBindVertexArray(0);
        } else {
            // Draw channel number for regular playlists
            draw_text(channel_num, static_cast<float>(theme_->margin_x), item_baseline, theme_->font_small_size, channel_color, false, text_alpha);
        }

        // Draw playlist text on the same baseline - use body font
        draw_text(text, text_x, item_baseline, font_size, text_color, false, text_alpha);

        // Blinking selection indicator (triangle pointing LEFT toward text, at end of text)
        if (selected && indicator_visible) {
            // Position triangle at the end of the text with spacing
            // IMPORTANT: Use the same font size as the text being rendered
            float text_width = body_font_manager_->get_text_width(text, font_size);
            float indicator_x = text_x + text_width + 16.0f;
            // Center triangle on the visual center of the text line (middle of line height)
            // Line height is approximately font_size * 1.2, so center is at y + (line_height / 2)
            float line_height = static_cast<float>(font_size) * 1.2f;
            float indicator_y = y + (line_height / 2.0f);  // Visual center of line, not baseline
            float size = 6.0f;
            
            // Draw filled triangle pointing LEFT (toward text) - matching Python version
            // Python: points = [(indicator_x, cy - size), (indicator_x, cy + size), (indicator_x - int(size * 1.2), cy)]
            // So: top point, bottom point, left point (pointing left)
            float top_y = indicator_y - size;
            float bottom_y = indicator_y + size;
            float left_x = indicator_x - size * 1.2f;  // Point to the left
            
            // Draw triangle as 3 vertices
            float triangle_vertices[] = {
                indicator_x, top_y,       0.0f, 0.0f,  // Top point (right side)
                indicator_x, bottom_y,    1.0f, 0.0f,  // Bottom point (right side)
                left_x, indicator_y,      1.0f, 1.0f   // Left point (pointing left)
            };
            
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
            
            GLint colorLoc = u_color_loc_;
            if (colorLoc >= 0) {
                glUniform4f(colorLoc, theme_->accent2.r / 255.0f, theme_->accent2.g / 255.0f, 
                           theme_->accent2.b / 255.0f, (theme_->accent2.a / 255.0f) * ui_alpha_ * text_alpha);
            }
            GLint useTextureLoc = u_use_texture_loc_;
            if (useTextureLoc >= 0) {
                glUniform1i(useTextureLoc, 0);  // No texture, solid color
            }
            
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);  // Draw as triangle
            glBindVertexArray(0);
        }
        
        y += static_cast<float>(theme_->playlist_item_height);
    }
    
    // If showing down arrow, draw it after the list items
    if (show_down_arrow) {
        float arrow_y = y + arrow_margin / 2.0f;
        
        // Draw triangle pointing DOWN (arrow_center_x already defined above)
        float triangle_vertices[] = {
            arrow_center_x - arrow_size, arrow_y,            0.0f, 0.0f,  // Top-left
            arrow_center_x + arrow_size, arrow_y,            1.0f, 0.0f,  // Top-right
            arrow_center_x, arrow_y + arrow_size * 1.2f,     0.5f, 1.0f   // Bottom (pointing down)
        };
        
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
        
        GLint colorLoc = u_color_loc_;
        if (colorLoc >= 0) {
            glUniform4f(colorLoc, theme_->accent2.r / 255.0f, theme_->accent2.g / 255.0f, 
                       theme_->accent2.b / 255.0f, (theme_->accent2.a / 255.0f) * ui_alpha_ * text_alpha);
        }
        GLint useTextureLoc = u_use_texture_loc_;
        if (useTextureLoc >= 0) {
            glUniform1i(useTextureLoc, 0);
        }
        
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }
}

std::string Renderer::format_time(double seconds) {
    if (seconds < 0) seconds = 0;
    int total = static_cast<int>(seconds);
    int m = total / 60;
    int s = total % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

void Renderer::render_error_overlay(const app::AppState& state) {
    if (!state.has_error_message()) return;

    std::string msg = state.error_message;
    float banner_h = 40.0f;
    float banner_y = static_cast<float>(height_) - banner_h - 20.0f;
    float banner_w = static_cast<float>(width_) * 0.6f;
    float banner_x = (static_cast<float>(width_) - banner_w) / 2.0f;

    // Semi-transparent dark background
    ui::Color bg = {0, 0, 0, 180};
    draw_quad(banner_x, banner_y, banner_w, banner_h, bg);

    // Error text in red, centered
    int font_size = 18;
    float text_w = static_cast<float>(body_font_manager_->get_text_width(msg, font_size));
    float text_x = banner_x + (banner_w - text_w) / 2.0f;
    float text_y = banner_y + banner_h / 2.0f + font_size * 0.3f;
    draw_text(msg, text_x, text_y, font_size, {255, 100, 100, 255});
}

void Renderer::render_loading_overlay(const app::AppState& state) {
    if (!state.is_loading_game) return;
    
    // Draw semi-transparent background
    draw_quad(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 
              ui::Color(0, 0, 0, 200));
              
    // Calculate pulsing alpha for text
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    float pulse = (sin(duration * 0.005f) + 1.0f) * 0.5f; // 0.0 to 1.0
    float text_alpha = 0.5f + (pulse * 0.5f); // 0.5 to 1.0
    
    // Draw "Loading..." text
    std::string text = "Loading...";
    int font_size = theme_->font_large_size;
    
    // Get text metrics for proper centering
    float text_width = body_font_manager_->get_text_width(text, font_size);
    float baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
    
    // Center text horizontally
    float x = (width_ - text_width) / 2.0f;
    
    // Center text vertically (accounting for baseline)
    // Visual center of text should be at screen center
    float y = (height_ / 2.0f) - (font_size / 2.0f) + baseline_offset;
    
    // Use body font (false) to match rest of UI
    draw_text(text, x, y, font_size, theme_->accent, false, text_alpha);
    
    // Draw animated spinner (rotating square)
    float spinner_size = 40.0f;
    float spinner_x = width_ / 2.0f;
    // Position spinner below centered text
    float spinner_y = y + (font_size / 2.0f) + 30.0f; // 30px below text center
    
    float rotation = duration * 0.005f; // Radians
    
    // Calculate rotated vertices
    float half_size = spinner_size / 2.0f;
    float cos_r = cos(rotation);
    float sin_r = sin(rotation);
    
    // 4 corners relative to center
    struct Point { float x, y; };
    Point corners[4] = {
        { -half_size, -half_size },
        { half_size, -half_size },
        { half_size, half_size },
        { -half_size, half_size }
    };
    
    // Rotate and translate
    // We'll use 2 triangles: 0-1-2 and 0-2-3
    
    Point rotated[4];
    for (int i = 0; i < 4; ++i) {
        rotated[i].x = spinner_x + (corners[i].x * cos_r - corners[i].y * sin_r);
        rotated[i].y = spinner_y + (corners[i].x * sin_r + corners[i].y * cos_r);
    }
    
    float triangle_vertices[] = {
        rotated[0].x, rotated[0].y, 0.0f, 0.0f,
        rotated[1].x, rotated[1].y, 1.0f, 0.0f,
        rotated[2].x, rotated[2].y, 1.0f, 1.0f,
        
        rotated[0].x, rotated[0].y, 0.0f, 0.0f,
        rotated[2].x, rotated[2].y, 1.0f, 1.0f,
        rotated[3].x, rotated[3].y, 0.0f, 1.0f
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);

    GLint colorLoc = u_color_loc_;
    if (colorLoc >= 0) {
        glUniform4f(colorLoc, theme_->accent.r / 255.0f, theme_->accent.g / 255.0f,
                   theme_->accent.b / 255.0f, 1.0f);
    }
    GLint useTextureLoc = u_use_texture_loc_;
    if (useTextureLoc >= 0) {
        glUniform1i(useTextureLoc, 0);
    }

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

#ifdef MEDIA_BROWSER_ENABLED
void Renderer::render_media_browser_placeholder() {
    // Solid near-black background covering the full screen.
    draw_quad(0.0f, 0.0f,
              static_cast<float>(width_), static_cast<float>(height_),
              ui::Color(0, 0, 0, 240));

    if (!body_font_manager_) return;

    // Title line
    const std::string title_text = "Movies - Media Browser";
    int title_size = theme_->font_large_size;
    float title_w = body_font_manager_->get_text_width(title_text, title_size);
    float title_baseline = body_font_manager_->get_baseline_at_size(title_size);
    float title_x = (static_cast<float>(width_) - title_w) / 2.0f;
    float title_y = (static_cast<float>(height_) / 2.0f) - (title_size / 2.0f) + title_baseline;
    draw_text(title_text, title_x, title_y, title_size, theme_->accent, false, 1.0f);

    // Hint line below
    const std::string hint_text = "[Menu to return]";
    int hint_size = theme_->font_small_size;
    float hint_w = body_font_manager_->get_text_width(hint_text, hint_size);
    float hint_baseline = body_font_manager_->get_baseline_at_size(hint_size);
    float hint_x = (static_cast<float>(width_) - hint_w) / 2.0f;
    float hint_y = title_y + static_cast<float>(title_size) + hint_baseline;
    draw_text(hint_text, hint_x, hint_y, hint_size, theme_->fg, false, 0.8f);
}

void Renderer::render_media_browser_screen_stub(const std::string& label) {
    // Solid near-black background covering the full screen. Matches the
    // placeholder's look so navigating between stub screens feels consistent.
    draw_quad(0.0f, 0.0f,
              static_cast<float>(width_), static_cast<float>(height_),
              ui::Color(0, 0, 0, 240));

    if (!body_font_manager_) return;

    // Centered screen label (large, accent color).
    int title_size = theme_->font_title_size;
    float title_w = body_font_manager_->get_text_width(label, title_size);
    float title_baseline = body_font_manager_->get_baseline_at_size(title_size);
    float title_x = (static_cast<float>(width_) - title_w) / 2.0f;
    float title_y = (static_cast<float>(height_) / 2.0f) - (title_size / 2.0f) + title_baseline;
    draw_text(label, title_x, title_y, title_size, theme_->accent, false, 1.0f);

    // "[Menu to return]" hint below.
    const std::string hint_text = "[Menu to return]";
    int hint_size = theme_->font_small_size;
    float hint_w = body_font_manager_->get_text_width(hint_text, hint_size);
    float hint_baseline = body_font_manager_->get_baseline_at_size(hint_size);
    float hint_x = (static_cast<float>(width_) - hint_w) / 2.0f;
    float hint_y = title_y + static_cast<float>(title_size) + hint_baseline;
    draw_text(hint_text, hint_x, hint_y, hint_size, theme_->fg, false, 0.8f);
}

// ---- Task 18 public drawing primitives for Media Browser screens ----

void Renderer::mb_fill_background() {
    // Fully opaque so the Media Browser overlay always fully covers any
    // video frame that may still be rendering underneath (e.g., during
    // the brief transition window before the controller's stop()
    // actually flushes the pipeline). Without this, a 6% alpha gap let
    // the previous main-UI playlist video bleed through.
    //
    // Uses bg (#1F191F) — the darker page background from the Marquee
    // palette. Cards and overlay panels use bg_lift (#2A232A) so they
    // read as elevated surfaces against this darker page. v1.6.4 operator
    // direction: page = darker bg, cards = lighter bg_lift.
    draw_quad(0.0f, 0.0f,
              static_cast<float>(width_), static_cast<float>(height_),
              theme_->bg);
}

void Renderer::mb_render_seek_bar(const app::AppState& state) {
    render_seek_bar(state);
}

void Renderer::mb_begin_2d_state() {
    // Mirror the "CRITICAL: Reset OpenGL state after mpv renders" block
    // from render(state). The Media Browser dispatcher skips render(state)
    // entirely, so without this call gst_renderer.render()'s YUV shader
    // stays bound and every subsequent mb_* draw silently writes to the
    // wrong shader's uniforms. Required before any MB screen draws.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DITHER);
    glActiveTexture(GL_TEXTURE0);

    if (shader_program_ != 0) {
        glUseProgram(shader_program_);

        // Set the screenSize uniform so the shader's NDC conversion
        // uses the actual framebuffer dimensions.
        //
        // Use original_width_ / original_height_ (the framebuffer size
        // from construction), NOT width_ / height_. The latter pair
        // gets temporarily overwritten by set_content_viewport() during
        // the main-menu's 4:3 pillarbox path; if we used width_ here
        // and a future bezel/letterbox feature were added to the MB
        // dispatcher, the screenSize uniform would silently shrink to
        // the pillarbox area and every MB UI element would render at
        // the wrong scale. The MB dispatcher in main.cpp always sets a
        // fullscreen glViewport, so original_* is what matches.
        GLint loc = u_screen_size_loc_;
        if (loc >= 0) {
            glUniform2f(loc,
                        static_cast<float>(original_width_),
                        static_cast<float>(original_height_));
        }
    }
}

void Renderer::mb_fill_rect(float x, float y, float w, float h,
                            const ui::Color& color, float alpha_multiplier) {
    draw_quad(x, y, w, h, color, alpha_multiplier);
}

void Renderer::mb_stroke_rect(float x, float y, float w, float h, float thickness,
                              const ui::Color& color, float alpha_multiplier) {
    // Top
    draw_quad(x, y, w, thickness, color, alpha_multiplier);
    // Bottom
    draw_quad(x, y + h - thickness, w, thickness, color, alpha_multiplier);
    // Left
    draw_quad(x, y, thickness, h, color, alpha_multiplier);
    // Right
    draw_quad(x + w - thickness, y, thickness, h, color, alpha_multiplier);
}

void Renderer::mb_fill_star(float cx, float cy, float outer_r,
                            const ui::Color& color, float alpha_multiplier) {
    // Rendered as GL_TRIANGLE_FAN: center + 10 alternating outer/inner radius
    // points + one repeat of the first ring point to close the shape. The
    // inner radius ratio (~0.382 = sin(18°)/sin(54°)) gives the classic
    // 5-point star silhouette; changing it makes the star fatter/skinnier.
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int   kN  = 10;                   // outer+inner points around ring
    constexpr int   kVerts = 1 + kN + 1;        // center + ring + closing point
    const float inner_r = outer_r * 0.382f;

    // Each vertex = (x, y, u, v) matching the VAO layout used by draw_quad.
    // UVs are unused in untextured mode but must still be present for the
    // attribute stride to stay consistent with the textured path.
    float vertices[kVerts * 4];
    vertices[0] = cx;
    vertices[1] = cy;
    vertices[2] = 0.0f;
    vertices[3] = 0.0f;
    for (int i = 0; i <= kN; ++i) {
        // Start angle -pi/2 puts the first outer point straight up, so the
        // star reads as pointing at the ceiling rather than sideways.
        float angle = -kPi / 2.0f
                    + static_cast<float>(i) * (2.0f * kPi / static_cast<float>(kN));
        float r = (i % 2 == 0) ? outer_r : inner_r;
        int o = (i + 1) * 4;
        vertices[o + 0] = cx + r * std::cos(angle);
        vertices[o + 1] = cy + r * std::sin(angle);
        vertices[o + 2] = 0.0f;
        vertices[o + 3] = 0.0f;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glUniform4f(u_color_loc_,
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_FAN, 0, kVerts);
    glBindVertexArray(0);
}

void Renderer::mb_draw_text(const std::string& text, float x, float baseline_y,
                            int font_size, const ui::Color& color,
                            float alpha_multiplier) {
    if (!body_font_manager_) return;
    draw_text(text, x, baseline_y, font_size, color, false, alpha_multiplier);
}

int Renderer::mb_text_width(const std::string& text, int font_size) {
    if (!body_font_manager_) return 0;
    return body_font_manager_->get_text_width(text, font_size);
}

int Renderer::mb_text_baseline(int font_size) {
    if (!body_font_manager_) return font_size;
    return body_font_manager_->get_baseline_at_size(font_size);
}

void Renderer::mb_draw_title_text(const std::string& text, float x, float baseline_y,
                                  int font_size, const ui::Color& color,
                                  float alpha_multiplier) {
    if (!title_font_manager_) return;
    // use_title_font=true selects the Zen Dots family — same one render_title()
    // and the "Playlists" header use, which is the look we're matching.
    draw_text(text, x, baseline_y, font_size, color, true, alpha_multiplier);
}

int Renderer::mb_title_text_width(const std::string& text, int font_size) {
    if (!title_font_manager_) return 0;
    return title_font_manager_->get_text_width(text, font_size);
}

int Renderer::mb_title_text_baseline(int font_size) {
    if (!title_font_manager_) return font_size;
    return title_font_manager_->get_baseline_at_size(font_size);
}

void Renderer::mb_draw_line(float x1, float y1, float x2, float y2,
                            float thickness, const ui::Color& color,
                            float alpha_multiplier) {
    draw_line(x1, y1, x2, y2, thickness, color, alpha_multiplier);
}

void Renderer::mb_fill_triangle(float x1, float y1, float x2, float y2,
                                float x3, float y3, const ui::Color& color,
                                float alpha_multiplier) {
    // Same VAO layout as draw_quad (4 floats per vertex: x, y, u, v).
    // UVs unused in untextured mode but stride must match.
    float vertices[] = {
        x1, y1, 0.0f, 0.0f,
        x2, y2, 0.0f, 0.0f,
        x3, y3, 0.0f, 0.0f,
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glUniform4f(u_color_loc_,
                color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                (color.a / 255.0f) * ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

media_browser::ArtworkCache& Renderer::artwork_cache() {
    if (!artwork_cache_) {
        // 256MB budget matches the default from artwork_cache.h. Could
        // be tuned per-hardware later; on Pi 4B this leaves plenty of
        // headroom below our 2GB system RAM.
        //
        // Disk-cache directory: /mnt/ssd/cache/posters when the USB SSD
        // is mounted (it always is when the Media Browser is in use —
        // it's where the library files live). The cache constructor
        // tolerates a missing/unwritable directory by silently falling
        // back to network-only mode, so kiosks without the SSD still
        // boot fine.
        const char* env_cache = std::getenv("MDB_ARTWORK_CACHE_DIR");
        std::string cache_dir = env_cache && *env_cache
            ? std::string(env_cache)
            : std::string("/mnt/ssd/cache/posters");
        artwork_cache_ = std::make_unique<media_browser::ArtworkCache>(
            256u * 1024u * 1024u, std::move(cache_dir));
    }
    return *artwork_cache_;
}

void Renderer::draw_textured_quad(uint32_t tex_id, float x, float y,
                                  float w, float h, float alpha_multiplier) {
    // Same vertex layout as draw_quad, but switch the shader into
    // textured mode and bind the supplied texture. Mirrors the
    // render_title() logo-drawing idiom.
    float vertices[] = {
        x, y,         0.0f, 0.0f,
        x + w, y,     1.0f, 0.0f,
        x, y + h,     0.0f, 1.0f,
        x + w, y + h, 1.0f, 1.0f
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glUniform4f(u_color_loc_,
                1.0f, 1.0f, 1.0f, ui_alpha_ * alpha_multiplier);
    glUniform1i(u_use_texture_loc_, 1);

    glBindTexture(GL_TEXTURE_2D, tex_id);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    // Restore solid-color mode so the next draw_quad doesn't sample
    // whatever texture was bound.
    glUniform1i(u_use_texture_loc_, 0);
}

void Renderer::mb_draw_poster_or_tint(const std::string& url,
                                      float x, float y, float w, float h,
                                      const ui::Color& fallback_tint,
                                      float alpha_multiplier) {
    // Empty URL: just draw the fallback tint. Don't pester the cache.
    if (url.empty()) {
        draw_quad(x, y, w, h, fallback_tint, alpha_multiplier);
        return;
    }

    uint32_t tex_id = artwork_cache().get_or_fetch(url);
    if (tex_id == 0) {
        // Not yet loaded — draw placeholder tint, the fetch is already
        // enqueued by get_or_fetch.
        draw_quad(x, y, w, h, fallback_tint, alpha_multiplier);
        return;
    }
    draw_textured_quad(tex_id, x, y, w, h, alpha_multiplier);
}

void Renderer::mb_draw_poster_fit(const std::string& url,
                                  float x, float y, float w, float h,
                                  const ui::Color& fallback_tint,
                                  float alpha_multiplier) {
    // Empty URL: just fill the slot with the tint, no fetch.
    if (url.empty()) {
        draw_quad(x, y, w, h, fallback_tint, alpha_multiplier);
        return;
    }

    uint32_t tex_id = artwork_cache().get_or_fetch(url);
    if (tex_id == 0) {
        // Texture still loading — fill the whole slot so the layout doesn't
        // jump on arrival.
        draw_quad(x, y, w, h, fallback_tint, alpha_multiplier);
        return;
    }

    auto dims = artwork_cache().get_dims(url);
    if (!dims || dims->w <= 0 || dims->h <= 0) {
        // Defensive — entry exists but dims unknown. Stretch like the
        // legacy variant rather than skipping the draw entirely.
        draw_textured_quad(tex_id, x, y, w, h, alpha_multiplier);
        return;
    }

    float img_aspect  = static_cast<float>(dims->w) / static_cast<float>(dims->h);
    float slot_aspect = (h > 0.0f) ? (w / h) : img_aspect;

    float out_w = w, out_h = h, out_x = x, out_y = y;
    if (img_aspect > slot_aspect) {
        // Image is wider than slot → fit width, letterbox top/bottom.
        out_h = w / img_aspect;
        out_y = y + (h - out_h) * 0.5f;
    } else {
        // Image is taller than slot → fit height, pillarbox left/right.
        out_w = h * img_aspect;
        out_x = x + (w - out_w) * 0.5f;
    }

    // Fill the full slot with a dim version of the fallback tint so the
    // letterbox/pillarbox bars are visible (not transparent). 0.35 keeps
    // them subtle without making the bars look like part of the image.
    draw_quad(x, y, w, h, fallback_tint, alpha_multiplier * 0.35f);
    draw_textured_quad(tex_id, out_x, out_y, out_w, out_h, alpha_multiplier);
}

void Renderer::pump_artwork() {
    // Only pump if the cache exists. Avoids lazy-init forcing a
    // background thread to start on frames where Media Browser isn't
    // being used.
    if (artwork_cache_) {
        artwork_cache_->pump();
    }
}
#endif

void Renderer::render_footer(const app::AppState& state, float text_alpha, bool video_active, bool ui_visible_when_playing) {
    // Footer baseline position (bottom-right, MTV-style)
    // Position baseline so text appears at height - 60
    float footer_baseline = static_cast<float>(height_ - 60) + body_font_manager_->get_baseline_at_size(theme_->font_small_size);
    
    // When video is active and UI overlay is visible, show title, artist, and duration
    if (video_active && ui_visible_when_playing && state.current_playlist_index >= 0 && state.current_item_index >= 0) {
        // Get current playing item
        if (state.current_playlist_index < static_cast<int>(state.playlists.size())) {
            const auto& pl = state.playlists[state.current_playlist_index];
            if (state.current_item_index < static_cast<int>(pl.items.size())) {
                const auto& item = pl.items[state.current_item_index];
                
                // Line 1: Title
                std::string title = item.title.empty() ? "Untitled" : item.title;
                int title_width = body_font_manager_->get_text_width(title, theme_->font_small_size);
                float title_x = static_cast<float>(width_) - title_width - 80.0f;
                draw_text(title, title_x, footer_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
                
                // Line 2: Artist (if available)
                if (!item.artist.empty()) {
                    float artist_baseline = footer_baseline + 18.0f;
                    int artist_width = body_font_manager_->get_text_width(item.artist, theme_->font_small_size);
                    float artist_x = static_cast<float>(width_) - artist_width - 80.0f;
                    draw_text(item.artist, artist_x, artist_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
                }
                
                // Line 3: Video progress (elapsed / duration)
                const double footer_duration = state.get_duration();
                if (footer_duration > 0) {
                    const double footer_position = state.get_position();
                    std::string progress_text = format_time(footer_position) + " / " + format_time(footer_duration);
                    float progress_baseline = footer_baseline + (item.artist.empty() ? 18.0f : 36.0f);
                    int progress_width = body_font_manager_->get_text_width(progress_text, theme_->font_small_size);
                    float progress_x = static_cast<float>(width_) - progress_width - 80.0f;
                    draw_text(progress_text, progress_x, progress_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
                }
                
                return;  // Don't show status text when showing video info
            }
        }
    }
    
    // Default footer: Status text and time (when not showing video info)
    std::string status = state.status_text;
    if (!status.empty()) {
        int status_width = body_font_manager_->get_text_width(status);
        float status_x = static_cast<float>(width_) - status_width - 80.0f;
        draw_text(status, status_x, footer_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
    }
    
    // Time display below status (elapsed / duration)
    const double time_position = state.get_position();
    const double time_duration = state.get_duration();
    if (time_position > 0 || time_duration > 0) {
        std::string time_text = format_time(time_position);
        if (time_duration > 0) {
            time_text += " / " + format_time(time_duration);
        }
        int time_width = body_font_manager_->get_text_width(time_text);
        float time_x = static_cast<float>(width_) - time_width - 80.0f;
        float time_baseline = footer_baseline + 18.0f;
        draw_text(time_text, time_x, time_baseline, theme_->font_small_size, theme_->fg, false, text_alpha);
    }
}



void Renderer::render_settings_menu(ui::SettingsMenuManager* menu, const std::vector<app::Playlist>& game_playlists, bool video_active, bool ui_visible_when_playing) {
    (void)ui_visible_when_playing; // Suppress unused parameter warning
    (void)video_active; // Suppress unused parameter warning
    if (!menu || (!menu->is_active() && !menu->is_closing())) {
        return;
    }
    
    // Determine alpha multipliers based on video state
    // Settings menu text is always fully opaque
    // Settings menu background is always fully opaque (not transparent) so video doesn't show through
    float text_alpha = 1.0f;
    float background_alpha = 1.0f;  // Always fully opaque for settings menu
    
    float progress = menu->get_animation_progress();
    uint32_t menu_width = width_ / 2;  // Half screen width
    float slide_offset = menu_width * (1.0f - progress);
    float menu_x = static_cast<float>(width_) - menu_width + slide_offset;
    
    // Get section color
    // Top-level "Settings" menu uses green, sub-sections use their original colors
    ui::MenuSection current_submenu = menu->get_current_submenu();
    ui::Color section_color;
    if (current_submenu == ui::MenuSection::BACK) {
        // Top-level Settings menu: green
        section_color = theme_->highlight1;
    } else if (current_submenu == ui::MenuSection::VIDEO_GAMES) {
        section_color = theme_->highlight1;  // Green
    } else if (current_submenu == ui::MenuSection::DISPLAY) {
        section_color = theme_->highlight3;  // Gold
    } else if (current_submenu == ui::MenuSection::AUDIO) {
        section_color = theme_->action;  // Blue
    } else if (current_submenu == ui::MenuSection::SYSTEM) {
        section_color = theme_->highlight2;  // Red
    } else {
        section_color = theme_->highlight1;  // Default to green
    }
    
    // Draw menu background panel
    draw_quad(menu_x, 0.0f, static_cast<float>(menu_width), static_cast<float>(height_), theme_->bg, background_alpha);
    
    // Draw left border accent
    draw_quad(menu_x, 0.0f, 4.0f, static_cast<float>(height_), section_color, background_alpha);
    
    // Check if we're in game browser mode
    if (menu->is_game_browser_active()) {
        render_game_browser(menu, game_playlists, menu_x, menu_width, section_color, text_alpha, background_alpha);
        return;
    }
    
    // Header
    std::string header_text = "Settings";
    if (current_submenu != ui::MenuSection::BACK) {
        if (current_submenu == ui::MenuSection::VIDEO_GAMES) {
            header_text = "Video games";
        } else if (current_submenu == ui::MenuSection::DISPLAY) {
            header_text = "Display";
        } else if (current_submenu == ui::MenuSection::AUDIO) {
            header_text = "Audio";
        } else if (current_submenu == ui::MenuSection::SYSTEM) {
            header_text = "System info";
        } else if (current_submenu == ui::MenuSection::WIFI) {
            header_text = "Wi-Fi";
        } else if (current_submenu == ui::MenuSection::WIFI_NETWORKS) {
            header_text = "Wi-Fi Networks";
        } else if (current_submenu == ui::MenuSection::INFO) {
            header_text = "Content Manager";
        }
    }
    
    int header_width = title_font_manager_->get_text_width(header_text, theme_->font_heading_size);
    float header_x = menu_x + (static_cast<float>(menu_width) - header_width) / 2.0f;
    int header_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
    float header_baseline = 8.0f + header_baseline_offset;
    draw_text(header_text, header_x, header_baseline, theme_->font_heading_size, section_color, true, text_alpha);
    
    // Underline - position below the text with enough space for descenders (like 'g', 'y', 'p')
    // Match the spacing used in render_title and render_playlist_list (10.0f instead of 4.0f)
    float underline_y = header_baseline + 10.0f;
    // Underline only spans the word - use exact text width
    draw_line(header_x, underline_y, header_x + header_width, underline_y, 2.0f, section_color, text_alpha);
    
    // Menu items
    const std::vector<ui::MenuItem>& items = current_submenu == ui::MenuSection::BACK ? 
        menu->get_menu_items() : menu->get_submenu_items();
    
    // Safety check: ensure items vector is not empty
    if (items.empty()) {
        return;  // Don't render if no items
    }
    
    float start_y = underline_y + 30.0f;
    int item_height = 60;
    // Compute visible row count from the actual viewport height.
    // Was hardcoded to 7 (sized for CRT 640x480). In Modern TV mode
    // the menu's 4:3 viewport is taller (720), so more items fit.
    // Floor at 7 to preserve CRT's established layout.
    //
    // bottom_reserve = 30: just a small breathing-room margin from the
    // bottom of the screen. We don't need to clear the controls-banner
    // footer because the settings menu draws its own opaque bg quad
    // over the right half of the screen, masking the banner there.
    // With this value, Modern TV (720) fits 10 rows — enough to show
    // every item in the Display submenu (Mode + Bezel + 7 CRT effects
    // + Back) without scrolling. CRT (480) stays floored at 7.
    const float settings_bottom_reserve = 30.0f;
    const float settings_available_h =
        static_cast<float>(height_) - start_y - settings_bottom_reserve;
    int max_visible = std::max(
        7, static_cast<int>(settings_available_h / static_cast<float>(item_height)));
    // Publish back to the menu so move_selection() knows when it
    // actually needs to scroll. Without this, the input handler used
    // a hardcoded 7 and forced scroll_offset to 1 on row index 7+,
    // chopping the top item off-screen even when all items fit.
    menu->set_max_visible_items(max_visible);
    int scroll_offset = menu->get_scroll_offset();
    int selected_index = menu->get_selected_index();
    
    // Clamp scroll_offset and selected_index to valid range
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset >= static_cast<int>(items.size())) scroll_offset = std::max(0, static_cast<int>(items.size()) - 1);
    if (selected_index < 0) selected_index = 0;
    if (selected_index >= static_cast<int>(items.size())) selected_index = static_cast<int>(items.size()) - 1;
    
    // Render visible items
    for (int i = 0; i < max_visible && (scroll_offset + i) < static_cast<int>(items.size()); i++) {
        int idx = scroll_offset + i;
        if (idx < 0 || idx >= static_cast<int>(items.size())) {
            continue;  // Safety check
        }
        const ui::MenuItem& item = items[idx];
        bool is_selected = (idx == selected_index);
        float y = start_y + i * item_height;
        
        // Selection highlight
        if (is_selected) {
            ui::Color highlight = section_color;
            highlight.a = 40;
            draw_quad(menu_x + 10.0f, y - 5.0f, static_cast<float>(menu_width) - 20.0f, 
                     static_cast<float>(item_height), highlight, background_alpha);
            
            // Selection indicator (triangle pointing RIGHT, at beginning of text - matching Python)
            // Python: points = [(indicator_x, cy - size), (indicator_x, cy + size), (indicator_x + int(size * 1.2), cy)]
            float indicator_x = menu_x + 15.0f;
            float indicator_y = y + item_height / 2.0f - 5.0f;
            float size = 8.0f;
            float right_x = indicator_x + size * 1.2f;  // Point to the right
            float triangle_vertices[] = {
                indicator_x, indicator_y - size,   0.0f, 0.0f,  // Top point (left side)
                indicator_x, indicator_y + size,   1.0f, 0.0f,  // Bottom point (left side)
                right_x, indicator_y,               1.0f, 1.0f   // Right point (pointing right)
            };
            
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);
            
            GLint colorLoc = u_color_loc_;
            if (colorLoc >= 0) {
                glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                           section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
            }
            GLint useTextureLoc = u_use_texture_loc_;
            if (useTextureLoc >= 0) {
                glUniform1i(useTextureLoc, 0);
            }
            
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
        }
        
        // Label
        int font_size = is_selected ? theme_->font_medium_size : theme_->font_small_size;
        ui::Color text_color = is_selected ? section_color : theme_->fg;
        float text_x = menu_x + 35.0f;
        int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
        float item_baseline = y + item_baseline_offset;
        draw_text(item.label, text_x, item_baseline, font_size, text_color, false, text_alpha);
        
        // Sublabel
        if (!item.sublabel.empty()) {
            float sublabel_baseline = item_baseline + font_size + 4.0f;
            draw_text(item.sublabel, text_x, sublabel_baseline, theme_->font_small_size, theme_->dim, false, text_alpha);
        }
    }
    // Footer hint removed - interface is self-explanatory
}

void Renderer::render_volume_overlay(const app::AppState& state) {
    if (!state.show_volume_slider) return;

    // Overlay background
    float overlay_width = 400.0f;
    float overlay_height = 80.0f;
    float x = (width_ - overlay_width) / 2.0f;
    float y = height_ - 120.0f; // Bottom center
    
    ui::Color bg_color = theme_->bg;
    bg_color.a = 230; // Mostly opaque
    draw_quad(x, y, overlay_width, overlay_height, bg_color);
    
    // Border
    draw_line(x, y, x + overlay_width, y, 2.0f, theme_->accent);
    draw_line(x, y + overlay_height, x + overlay_width, y + overlay_height, 2.0f, theme_->accent);
    draw_line(x, y, x, y + overlay_height, 2.0f, theme_->accent);
    draw_line(x + overlay_width, y, x + overlay_width, y + overlay_height, 2.0f, theme_->accent);
    
    // Label
    std::string label = "MASTER VOLUME: " + std::to_string(state.master_volume) + "%";
    int font_size = theme_->font_medium_size;
    int label_width = body_font_manager_->get_text_width(label, font_size);
    float label_x = x + (overlay_width - label_width) / 2.0f;
    float label_y = y + 25.0f;
    draw_text(label, label_x, label_y, font_size, theme_->fg, false);
    
    // Slider bar background
    float bar_width = 300.0f;
    float bar_height = 10.0f;
    float bar_x = x + (overlay_width - bar_width) / 2.0f;
    float bar_y = y + 50.0f;
    ui::Color bar_bg = theme_->dim;
    draw_quad(bar_x, bar_y, bar_width, bar_height, bar_bg);
    
    // Slider fill
    float fill_width = (state.master_volume / 100.0f) * bar_width;
    if (fill_width > 0) {
        draw_quad(bar_x, bar_y, fill_width, bar_height, theme_->accent);
    }
}

void Renderer::render_seek_bar(const app::AppState& state) {
    // Gate on show_seek_bar only. video_active was previously checked here
    // as a defensive guard, but show_seek_bar is only flipped true by seek
    // input handlers that run during active playback in the first place,
    // so the extra gate was redundant — and it broke the Media Browser's
    // PlaybackScreen path where state.video_active can briefly drop during
    // a FLUSH seek transition while the user is actively scrubbing.
    if (!state.show_seek_bar) return;

    // Fade behavior: full opacity when timer > 0.5, linear fade 0.5 -> 0.0
    float alpha = 1.0f;
    if (state.seek_bar_timer <= 0.5) {
        alpha = static_cast<float>(state.seek_bar_timer / 0.5);
    }
    if (alpha <= 0.0f) return;

    double position = state.get_position();
    double duration = state.get_duration();
    if (duration <= 0.0) return;

    float progress = static_cast<float>(position / duration);
    progress = std::max(0.0f, std::min(1.0f, progress));

    // Layout: centered, 80% screen width.
    // Bar sits at ~110px from screen bottom so the chrome footer hints
    // (rendered ~40px above the bezel bottom = screen_h - 40 - 30 ≈ -70px
    // from bottom) have clear breathing room below the scrub bar.
    float bar_total_width = static_cast<float>(width_) * 0.8f;
    float bar_x = (static_cast<float>(width_) - bar_total_width) / 2.0f;
    float bar_y = static_cast<float>(height_) - 110.0f;
    float bar_height = 4.0f;

    // Track background (dim gray)
    ui::Color track_color = theme_->dim;
    track_color.a = static_cast<uint8_t>(track_color.a * alpha);
    draw_quad(bar_x, bar_y, bar_total_width, bar_height, track_color);

    // Progress fill (accent/gold)
    float fill_width = bar_total_width * progress;
    if (fill_width > 0.0f) {
        ui::Color fill_color = theme_->accent;
        fill_color.a = static_cast<uint8_t>(fill_color.a * alpha);
        draw_quad(bar_x, bar_y, fill_width, bar_height, fill_color);
    }

    // Playhead indicator (bright square at current position)
    float head_size = 10.0f;
    float head_x = bar_x + fill_width - head_size / 2.0f;
    float head_y = bar_y - (head_size - bar_height) / 2.0f;
    ui::Color head_color = theme_->fg;
    head_color.a = static_cast<uint8_t>(head_color.a * alpha);
    draw_quad(head_x, head_y, head_size, head_size, head_color);

    // Time labels above the bar
    std::string time_current = format_time(position);
    std::string time_total = format_time(duration);
    int font_size = theme_->font_small_size;
    float label_y = bar_y - 8.0f;  // Just above the bar

    // Current time (left-aligned)
    ui::Color text_color = theme_->fg;
    text_color.a = static_cast<uint8_t>(text_color.a * alpha);
    draw_text(time_current, bar_x, label_y, font_size, text_color, false, alpha);

    // Total duration (right-aligned)
    int total_width = body_font_manager_->get_text_width(time_total, font_size);
    float total_x = bar_x + bar_total_width - static_cast<float>(total_width);
    draw_text(time_total, total_x, label_y, font_size, text_color, false, alpha);
}

bool Renderer::load_thumbnail(const std::string& rom_path) {
    // Derive thumbnail path from ROM path: data/roms/ps1/Game.chd -> data/thumbnails/ps1/Game.png
    // Find the system directory component
    std::string thumb_path;
    size_t roms_pos = rom_path.find("data/roms/");
    if (roms_pos != std::string::npos) {
        thumb_path = rom_path.substr(0, roms_pos) + "data/thumbnails/" + rom_path.substr(roms_pos + 10); // skip "data/roms/"
    } else {
        // Fallback: just try replacing extension
        thumb_path = rom_path;
    }

    // Replace ROM extension (.chd, .m3u, .z64, .nes, etc.) with .png
    size_t dot_pos = thumb_path.rfind('.');
    if (dot_pos != std::string::npos) {
        thumb_path = thumb_path.substr(0, dot_pos) + ".png";
    }

    if (thumb_path == current_thumbnail_path_) {
        return thumbnail_texture_id_ != 0; // Already loaded
    }

    // Free old texture
    if (thumbnail_texture_id_ != 0) {
        glDeleteTextures(1, &thumbnail_texture_id_);
        thumbnail_texture_id_ = 0;
    }
    current_thumbnail_path_ = thumb_path;

    // Try to resolve to absolute path
    std::string abs_path = thumb_path;
    // Also try relative to working directory with ../
    std::vector<std::string> search_paths = {
        thumb_path,
        "../" + thumb_path,
    };

    // Try progressively simpler filename variants
    for (const auto& sp : std::vector<std::string>{thumb_path, "../" + thumb_path}) {
        std::string base = sp;

        // Strip " (Disc N)" patterns
        size_t disc_pos = base.find(" (Disc ");
        if (disc_pos != std::string::npos) {
            search_paths.push_back(base.substr(0, disc_pos) + ".png");
        }

        // Strip version info: " (v1.1)" etc.
        size_t ver_pos = base.find(" (v");
        if (ver_pos != std::string::npos) {
            // Strip everything from (vN.N) onward, keep region code before it
            search_paths.push_back(base.substr(0, ver_pos) + ".png");
        }

        // Strip everything after region code: "Game (USA) (anything).png" -> "Game (USA).png"
        // Look for common region codes
        for (const char* region : {"(USA)", "(Europe)", "(Japan)", "(World)"}) {
            size_t region_pos = base.find(region);
            if (region_pos != std::string::npos) {
                std::string stripped = base.substr(0, region_pos + strlen(region)) + ".png";
                search_paths.push_back(stripped);
                break;
            }
        }
    }

    int channels;
    unsigned char* data = nullptr;
    for (const auto& path : search_paths) {
        data = stbi_load(path.c_str(), &thumbnail_width_, &thumbnail_height_, &channels, 4);
        if (data) break;
    }

    if (!data) {
        return false;
    }

    glGenTextures(1, &thumbnail_texture_id_);
    glBindTexture(GL_TEXTURE_2D, thumbnail_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumbnail_width_, thumbnail_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(data);

    return true;
}

std::string Renderer::get_system_key(const app::Playlist& playlist) const {
    // Try emulator_system from the first item
    for (const auto& item : playlist.items) {
        if (!item.emulator_system.empty()) {
            std::string sys = item.emulator_system;
            // Lowercase
            for (auto& c : sys) c = tolower(c);

            if (sys == "ps1" || sys == "playstation") return "ps1";
            if (sys == "nes") return "nes";
            if (sys == "snes" || sys == "super nintendo") return "snes";
            if (sys == "genesis" || sys == "md" || sys == "mega drive") return "genesis";
            if (sys == "atari7800" || sys == "7800" || sys == "atari 7800") return "atari7800";
            if (sys == "pcengine" || sys == "tg16" || sys == "turbografx") return "pcengine";
            if (sys == "arcade" || sys == "mame" || sys == "fba") return "arcade";
            return sys; // Use as-is if no mapping found
        }
    }

    // Try emulator_core from the first item
    for (const auto& item : playlist.items) {
        if (!item.emulator_core.empty()) {
            const std::string& core = item.emulator_core;
            if (core.find("nestopia") != std::string::npos || core.find("fceumm") != std::string::npos) return "nes";
            if (core.find("snes9x") != std::string::npos || core.find("bsnes") != std::string::npos) return "snes";
            if (core.find("pcsx") != std::string::npos) return "ps1";
            if (core.find("genesis_plus") != std::string::npos || core.find("picodrive") != std::string::npos) return "genesis";
            if (core.find("prosystem") != std::string::npos) return "atari7800";
            if (core.find("mednafen_pce") != std::string::npos || core.find("supergrafx") != std::string::npos) return "pcengine";
            if (core.find("mame") != std::string::npos || core.find("fbneo") != std::string::npos || core.find("fbalpha") != std::string::npos) return "arcade";
            if (core.find("mupen64") != std::string::npos) return "n64";
        }
    }

    // Fallback: derive from playlist title
    std::string title = playlist.title;
    for (auto& c : title) c = tolower(c);

    if (title.find("playstation") != std::string::npos || title.find("ps1") != std::string::npos) return "ps1";
    if (title.find("super nintendo") != std::string::npos || title.find("snes") != std::string::npos) return "snes";
    if (title.find("nes") != std::string::npos) return "nes"; // After SNES check
    if (title.find("genesis") != std::string::npos || title.find("sega") != std::string::npos || title.find("mega drive") != std::string::npos) return "genesis";
    if (title.find("atari") != std::string::npos) return "atari7800";
    if (title.find("pc engine") != std::string::npos || title.find("turbografx") != std::string::npos) return "pcengine";
    if (title.find("arcade") != std::string::npos) return "arcade";

    return "";
}

const Renderer::CachedLogo* Renderer::get_system_logo(const std::string& system_key) {
    if (system_key.empty()) return nullptr;

    // Check cache first
    auto it = system_logo_cache_.find(system_key);
    if (it != system_logo_cache_.end()) {
        return it->second.texture_id != 0 ? &it->second : nullptr;
    }

    // Load from disk
    std::vector<std::string> search_paths = {
        "data/thumbnails/systems/" + system_key + ".png",
        "../data/thumbnails/systems/" + system_key + ".png",
    };

    CachedLogo logo;
    int channels;
    unsigned char* data = nullptr;
    for (const auto& path : search_paths) {
        data = stbi_load(path.c_str(), &logo.width, &logo.height, &channels, 4);
        if (data) break;
    }

    if (!data) {
        // Cache the miss so we don't retry every frame
        system_logo_cache_[system_key] = logo;
        return nullptr;
    }

    glGenTextures(1, &logo.texture_id);
    glBindTexture(GL_TEXTURE_2D, logo.texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo.width, logo.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(data);

    system_logo_cache_[system_key] = logo;
    return &system_logo_cache_[system_key];
}

void Renderer::render_game_browser(ui::SettingsMenuManager* menu, const std::vector<app::Playlist>& game_playlists, float menu_x, uint32_t menu_width, const ui::Color& section_color, float text_alpha, float background_alpha) {
    // Render game browser header
    std::string header_text = "Select Game Library";
    if (menu->is_viewing_games_in_playlist()) {
        int playlist_idx = menu->get_current_game_playlist_index();
        if (playlist_idx >= 0 && playlist_idx < static_cast<int>(game_playlists.size())) {
            header_text = game_playlists[playlist_idx].title;
        }
    }

    int header_width = title_font_manager_->get_text_width(header_text, theme_->font_heading_size);
    float header_x = menu_x + (static_cast<float>(menu_width) - header_width) / 2.0f;
    int header_baseline_offset = title_font_manager_->get_baseline_at_size(theme_->font_heading_size);
    float header_baseline = 8.0f + header_baseline_offset;
    draw_text(header_text, header_x, header_baseline, theme_->font_heading_size, section_color, true, text_alpha);

    // Underline
    float underline_y = header_baseline + 10.0f;
    draw_line(header_x, underline_y, header_x + header_width, underline_y, 2.0f, section_color, text_alpha);

    float content_start_y = underline_y + 15.0f;

    if (menu->is_viewing_games_in_playlist()) {
        // Show games in current playlist with thumbnail above the list
        int playlist_idx = menu->get_current_game_playlist_index();
        if (playlist_idx >= 0 && playlist_idx < static_cast<int>(game_playlists.size())) {
            const auto& playlist = game_playlists[playlist_idx];
            int selected_game = menu->get_selected_game_in_playlist();

            // --- Thumbnail or placeholder rendering ---
            float thumb_display_h = 160.0f; // Fixed height for thumbnail area
            float thumbnail_area_height = thumb_display_h + 10.0f; // Always reserve space

            if (selected_game >= 0 && selected_game < static_cast<int>(playlist.items.size())) {
                const auto& selected_item = playlist.items[selected_game];
                bool has_thumbnail = load_thumbnail(selected_item.path);

                if (has_thumbnail && thumbnail_texture_id_ != 0) {
                    // Scale thumbnail to fit, preserving aspect ratio
                    float max_thumb_w = static_cast<float>(menu_width) - 40.0f;
                    float scale = std::min(max_thumb_w / thumbnail_width_, thumb_display_h / thumbnail_height_);
                    float draw_w = thumbnail_width_ * scale;
                    float draw_h = thumbnail_height_ * scale;

                    // Center in the reserved area
                    float thumb_x = menu_x + (static_cast<float>(menu_width) - draw_w) / 2.0f;
                    float thumb_y = content_start_y + (thumb_display_h - draw_h) / 2.0f;

                    float vertices[] = {
                        thumb_x, thumb_y,                 0.0f, 0.0f,
                        thumb_x + draw_w, thumb_y,        1.0f, 0.0f,
                        thumb_x, thumb_y + draw_h,        0.0f, 1.0f,
                        thumb_x + draw_w, thumb_y + draw_h, 1.0f, 1.0f
                    };

                    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

                    glUniform4f(u_color_loc_,
                                1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha);
                    glUniform1i(u_use_texture_loc_, 1);

                    glBindTexture(GL_TEXTURE_2D, thumbnail_texture_id_);
                    glBindVertexArray(vao_);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glBindVertexArray(0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                } else {
                    // Placeholder: dark box with system console logo
                    float placeholder_size = thumb_display_h * 0.8f;
                    float ph_x = menu_x + (static_cast<float>(menu_width) - placeholder_size) / 2.0f;
                    float ph_y = content_start_y + (thumb_display_h - placeholder_size) / 2.0f;

                    // Dark background
                    ui::Color ph_bg = {40, 35, 40, 180};
                    draw_quad(ph_x, ph_y, placeholder_size, placeholder_size, ph_bg, text_alpha);

                    // Border
                    ui::Color border_color = section_color;
                    border_color.a = 80;
                    draw_line(ph_x, ph_y, ph_x + placeholder_size, ph_y, 1.0f, border_color, text_alpha);
                    draw_line(ph_x, ph_y + placeholder_size, ph_x + placeholder_size, ph_y + placeholder_size, 1.0f, border_color, text_alpha);
                    draw_line(ph_x, ph_y, ph_x, ph_y + placeholder_size, 1.0f, border_color, text_alpha);
                    draw_line(ph_x + placeholder_size, ph_y, ph_x + placeholder_size, ph_y + placeholder_size, 1.0f, border_color, text_alpha);

                    // System logo centered in placeholder
                    std::string sys_key = get_system_key(playlist);
                    const CachedLogo* logo = get_system_logo(sys_key);
                    if (logo) {
                        // Scale logo to fit inside placeholder with padding
                        float logo_max_w = placeholder_size * 0.7f;
                        float logo_max_h = placeholder_size * 0.7f;
                        float logo_scale = std::min(logo_max_w / logo->width, logo_max_h / logo->height);
                        float logo_w = logo->width * logo_scale;
                        float logo_h = logo->height * logo_scale;

                        float logo_x = ph_x + (placeholder_size - logo_w) / 2.0f;
                        float logo_y = ph_y + (placeholder_size - logo_h) / 2.0f;

                        float vertices[] = {
                            logo_x, logo_y,                 0.0f, 0.0f,
                            logo_x + logo_w, logo_y,        1.0f, 0.0f,
                            logo_x, logo_y + logo_h,        0.0f, 1.0f,
                            logo_x + logo_w, logo_y + logo_h, 1.0f, 1.0f
                        };

                        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

                        // Render logo in white to match text color
                        glUniform4f(u_color_loc_,
                                    1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha * 0.5f);
                        glUniform1i(u_use_texture_loc_, 1);

                        glBindTexture(GL_TEXTURE_2D, logo->texture_id);
                        glBindVertexArray(vao_);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        glBindVertexArray(0);
                        glBindTexture(GL_TEXTURE_2D, 0);
                    }
                }
            } else {
                // "Back" selected — show dimmed system logo placeholder
                float placeholder_size = thumb_display_h * 0.8f;
                float ph_x = menu_x + (static_cast<float>(menu_width) - placeholder_size) / 2.0f;
                float ph_y = content_start_y + (thumb_display_h - placeholder_size) / 2.0f;
                ui::Color ph_bg = {40, 35, 40, 100};
                draw_quad(ph_x, ph_y, placeholder_size, placeholder_size, ph_bg, text_alpha);

                // Show system logo dimmed
                std::string sys_key = get_system_key(playlist);
                const CachedLogo* logo = get_system_logo(sys_key);
                if (logo) {
                    float logo_max_w = placeholder_size * 0.7f;
                    float logo_max_h = placeholder_size * 0.7f;
                    float logo_scale = std::min(logo_max_w / logo->width, logo_max_h / logo->height);
                    float logo_w = logo->width * logo_scale;
                    float logo_h = logo->height * logo_scale;

                    float logo_x = ph_x + (placeholder_size - logo_w) / 2.0f;
                    float logo_y = ph_y + (placeholder_size - logo_h) / 2.0f;

                    float vertices[] = {
                        logo_x, logo_y,                 0.0f, 0.0f,
                        logo_x + logo_w, logo_y,        1.0f, 0.0f,
                        logo_x, logo_y + logo_h,        0.0f, 1.0f,
                        logo_x + logo_w, logo_y + logo_h, 1.0f, 1.0f
                    };

                    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

                    glUniform4f(u_color_loc_,
                                1.0f, 1.0f, 1.0f, ui_alpha_ * text_alpha * 0.3f);
                    glUniform1i(u_use_texture_loc_, 1);

                    glBindTexture(GL_TEXTURE_2D, logo->texture_id);
                    glBindVertexArray(vao_);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glBindVertexArray(0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }

                // Clear cached thumbnail
                if (thumbnail_texture_id_ != 0) {
                    glDeleteTextures(1, &thumbnail_texture_id_);
                    thumbnail_texture_id_ = 0;
                    current_thumbnail_path_.clear();
                }
            }

            // --- Game list (tighter spacing, no subtitle) ---
            int item_height = 30;
            float start_y = content_start_y + thumbnail_area_height;
            float available_height = static_cast<float>(height_) - start_y - 10.0f;
            int max_visible = std::max(3, static_cast<int>(available_height / item_height));

            int total_items = static_cast<int>(playlist.items.size()) + 1; // +1 for Back

            // Compute scroll offset from selection (resets across playlists since not static)
            int game_scroll_offset = 0;
            if (selected_game >= max_visible) {
                game_scroll_offset = selected_game - max_visible + 1;
            }
            if (game_scroll_offset < 0) game_scroll_offset = 0;
            if (game_scroll_offset > total_items - max_visible) game_scroll_offset = std::max(0, total_items - max_visible);

            for (int i = 0; i < max_visible; ++i) {
                int idx = game_scroll_offset + i;
                if (idx >= total_items) break;

                bool is_selected = (idx == selected_game);
                float y = start_y + static_cast<float>(i) * item_height;

                // Selection highlight
                if (is_selected) {
                    ui::Color highlight = section_color;
                    highlight.a = 40;
                    draw_quad(menu_x + 10.0f, y - 2.0f, static_cast<float>(menu_width) - 20.0f,
                             static_cast<float>(item_height), highlight, background_alpha);

                    // Selection indicator triangle
                    float indicator_x = menu_x + 15.0f;
                    float indicator_y = y + item_height / 2.0f;
                    float size = 6.0f;
                    float right_x = indicator_x + size * 1.2f;
                    float triangle_vertices[] = {
                        indicator_x, indicator_y - size,   0.0f, 0.0f,
                        indicator_x, indicator_y + size,   1.0f, 0.0f,
                        right_x, indicator_y,               1.0f, 1.0f
                    };

                    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);

                    GLint colorLoc = u_color_loc_;
                    if (colorLoc >= 0) {
                        glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                                   section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
                    }
                    GLint useTextureLoc = u_use_texture_loc_;
                    if (useTextureLoc >= 0) {
                        glUniform1i(useTextureLoc, 0);
                    }

                    glBindVertexArray(vao_);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    glBindVertexArray(0);
                }

                // Game title only (no subtitle)
                int font_size = is_selected ? theme_->font_medium_size : theme_->font_small_size;
                ui::Color text_color = is_selected ? section_color : theme_->fg;
                float text_x = menu_x + 35.0f;
                int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
                float item_baseline = y + (item_height - font_size) / 2.0f + item_baseline_offset;

                if (idx < static_cast<int>(playlist.items.size())) {
                    draw_text(playlist.items[idx].title, text_x, item_baseline, font_size, text_color, false, text_alpha);
                } else {
                    draw_text("Back", text_x, item_baseline, font_size, text_color, false, text_alpha);
                }
            }
        }
    } else {
        // Show game playlists (keep subtitle with game count)
        int item_height = 50;
        float start_y = content_start_y + 15.0f;
        // Compute visible row count from the actual viewport height.
        // Was hardcoded to 8 (CRT 640x480). Modern TV's UI viewport is
        // ~960x720 — room for ~11 rows. Floor at 8 preserves CRT.
        const float gp_bottom_reserve = 80.0f;
        const float gp_available_h =
            static_cast<float>(height_) - start_y - gp_bottom_reserve;
        int max_visible = std::max(
            8, static_cast<int>(gp_available_h / static_cast<float>(item_height)));
        int selected_playlist = menu->get_game_browser_selected();

        int total_items = static_cast<int>(game_playlists.size()) + 1; // +1 for Back

        static int playlist_scroll_offset = 0;
        if (selected_playlist < playlist_scroll_offset) {
            playlist_scroll_offset = selected_playlist;
        } else if (selected_playlist >= playlist_scroll_offset + max_visible) {
            playlist_scroll_offset = selected_playlist - max_visible + 1;
        }
        if (playlist_scroll_offset < 0) playlist_scroll_offset = 0;
        if (playlist_scroll_offset > total_items - max_visible) playlist_scroll_offset = std::max(0, total_items - max_visible);

        // Clear thumbnail when not viewing games
        if (thumbnail_texture_id_ != 0) {
            glDeleteTextures(1, &thumbnail_texture_id_);
            thumbnail_texture_id_ = 0;
            current_thumbnail_path_.clear();
        }

        for (int i = 0; i < max_visible; ++i) {
            int idx = playlist_scroll_offset + i;
            if (idx >= total_items) break;

            bool is_selected = (idx == selected_playlist);
            float y = start_y + static_cast<float>(i) * item_height;

            // Selection highlight
            if (is_selected) {
                ui::Color highlight = section_color;
                highlight.a = 40;
                draw_quad(menu_x + 10.0f, y - 5.0f, static_cast<float>(menu_width) - 20.0f,
                         static_cast<float>(item_height), highlight, background_alpha);

                // Selection indicator
                float indicator_x = menu_x + 15.0f;
                float indicator_y = y + item_height / 2.0f - 5.0f;
                float size = 8.0f;
                float right_x = indicator_x + size * 1.2f;
                float triangle_vertices[] = {
                    indicator_x, indicator_y - size,   0.0f, 0.0f,
                    indicator_x, indicator_y + size,   1.0f, 0.0f,
                    right_x, indicator_y,               1.0f, 1.0f
                };

                glBindBuffer(GL_ARRAY_BUFFER, vbo_);
                glBufferData(GL_ARRAY_BUFFER, sizeof(triangle_vertices), triangle_vertices, GL_DYNAMIC_DRAW);

                GLint colorLoc = u_color_loc_;
                if (colorLoc >= 0) {
                    glUniform4f(colorLoc, section_color.r / 255.0f, section_color.g / 255.0f,
                               section_color.b / 255.0f, (section_color.a / 255.0f) * ui_alpha_ * text_alpha);
                }
                GLint useTextureLoc = u_use_texture_loc_;
                if (useTextureLoc >= 0) {
                    glUniform1i(useTextureLoc, 0);
                }

                glBindVertexArray(vao_);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
            }

            // Render item text
            int font_size = is_selected ? theme_->font_medium_size : theme_->font_small_size;
            ui::Color text_color = is_selected ? section_color : theme_->fg;
            float text_x = menu_x + 35.0f;
            int item_baseline_offset = body_font_manager_->get_baseline_at_size(font_size);
            float item_baseline = y + item_baseline_offset;

            if (idx < static_cast<int>(game_playlists.size())) {
                draw_text(game_playlists[idx].title, text_x, item_baseline, font_size, text_color, false, text_alpha);

                // Game count as sublabel
                std::string count_text = std::to_string(game_playlists[idx].items.size()) + " games";
                float sublabel_baseline = item_baseline + font_size + 4.0f;
                draw_text(count_text, text_x, sublabel_baseline, theme_->font_small_size, theme_->dim, false, text_alpha);
            } else {
                draw_text("Back", text_x, item_baseline, font_size, text_color, false, text_alpha);
            }
        }
    }
}

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

void Renderer::destroy_scene_fbo() {
    if (scene_fbo_ != 0) {
        glDeleteFramebuffers(1, &scene_fbo_);
        scene_fbo_ = 0;
    }
    if (scene_color_tex_ != 0) {
        glDeleteTextures(1, &scene_color_tex_);
        scene_color_tex_ = 0;
    }
    scene_fbo_width_ = 0;
    scene_fbo_height_ = 0;
}

void Renderer::ensure_scene_fbo(uint32_t fb_width, uint32_t fb_height) {
    // No-op if already at the right size.
    if (scene_fbo_ != 0 &&
        scene_fbo_width_ == fb_width &&
        scene_fbo_height_ == fb_height) {
        return;
    }

    // Size mismatch (display reconnect, mode change, first call) —
    // tear down and rebuild. This is rare; the screen rarely resizes.
    destroy_scene_fbo();

    if (fb_width == 0 || fb_height == 0) {
        return;  // Nothing sensible we can build at zero size.
    }

    glGenFramebuffers(1, &scene_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);

    glGenTextures(1, &scene_color_tex_);
    glBindTexture(GL_TEXTURE_2D, scene_color_tex_);
    // RGBA8 is the safe-everywhere format. The kiosk doesn't need HDR
    // headroom — all source content is sRGB-encoded 8-bit. Linear
    // filtering matters because the composite's sub-1px effects
    // (Phase 4 convergence offsets) sample at non-integer coords.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 static_cast<GLsizei>(fb_width),
                 static_cast<GLsizei>(fb_height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, scene_color_tex_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "UI Renderer: scene FBO incomplete (status=0x"
                  << std::hex << status << std::dec << "); enhanced CRT "
                  << "pipeline disabled" << std::endl;
        // Restore default framebuffer so subsequent draws still show.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        destroy_scene_fbo();
        return;
    }

    scene_fbo_width_ = fb_width;
    scene_fbo_height_ = fb_height;

    // Leave the FBO bound — the typical caller (begin_scene_fbo) wants
    // to draw into it next. Caller still has to set viewport. Keep
    // the texture unbound so Sampler unit 0 isn't accidentally pinned
    // to it during the scene render (we only want it bound during the
    // composite pass).
    glBindTexture(GL_TEXTURE_2D, 0);

    std::cout << "UI Renderer: scene FBO created " << fb_width << "x" << fb_height
              << " (fbo=" << scene_fbo_ << ", tex=" << scene_color_tex_ << ")"
              << std::endl;
}

bool Renderer::begin_scene_fbo(const app::AppState& state) {
    // Gate 1: feature flag must be on.
    if (!state.display_settings.enhanced_crt_enabled) return false;

    // Gate 2: composite shader must have compiled at startup.
    if (crt_composite_shader_program_ == 0) return false;

    // Gate 3: skip the Media Browser screen entirely. Its content is
    // 16:9 (movie posters / movie playback) and intentionally outside
    // the CRT-effects scope — same gating idea as the bezel skip in
    // main.cpp.
#ifdef MEDIA_BROWSER_ENABLED
    if (state.current_screen == app::AppScreen::MediaBrowser) return false;
#endif

    // Gate 4: skip when no effects are active. There's no point paying
    // the FBO indirection cost (~1ms on Pi 4) just to copy pixels
    // straight through. This matches the early-out in
    // render_crt_effects() and keeps the "all sliders off = direct
    // draw" property of the legacy path.
    const auto& s = state.display_settings;
    bool any_effect_active = (s.scanline_intensity > 0.0f ||
                              s.warmth_intensity > 0.0f ||
                              s.glow_intensity > 0.0f ||
                              s.rgb_mask_intensity > 0.0f ||
                              s.bloom_intensity > 0.0f ||
                              s.interlacing_intensity > 0.0f ||
                              s.flicker_intensity > 0.0f);
    if (!any_effect_active) return false;

    // Gate 5: lazily create the FBO (or recreate at new size).
    //
    // Size the FBO to match the actual HDMI framebuffer, not the
    // logical UI canvas. They're equal in Modern TV (so nothing
    // changes), but in CRT_NATIVE the logical canvas is 640×480
    // while the framebuffer is whatever HDMI negotiated (typically
    // 1280×720). Pre-fix, the FBO was 640×480 and the composite
    // viewport at the end of this pipeline rendered into the
    // bottom-left 640×480 of a 1280×720 framebuffer — visible on a
    // CRT as the entire UI scrunched into one corner with everything
    // else black. With the FBO at framebuffer size, UI projection
    // upscales 640×480 into the FBO the same way it upscales into
    // the default framebuffer on the legacy path, and the composite
    // is a 1:1 copy that fills the screen.
    //
    // Fallback: if main.cpp never called set_framebuffer_size() (or
    // we're in a state where it hasn't been called yet), framebuffer_*
    // is 0 — use original_* to preserve pre-fix Modern TV behavior.
    const uint32_t fbo_w = framebuffer_width_  ? framebuffer_width_  : original_width_;
    const uint32_t fbo_h = framebuffer_height_ ? framebuffer_height_ : original_height_;
    ensure_scene_fbo(fbo_w, fbo_h);
    if (scene_fbo_ == 0) return false;  // Creation failed; fall back to legacy.

    // Bind the FBO and clear it. Caller is expected to set its own
    // viewport for any letterboxed UI/video draw — those calls now
    // target our offscreen texture.
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glViewport(0, 0,
               static_cast<GLsizei>(scene_fbo_width_),
               static_cast<GLsizei>(scene_fbo_height_));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    scene_fbo_active_ = true;
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

void Renderer::destroy_bloom_fbos() {
    if (bloom_a_fbo_ != 0) {
        glDeleteFramebuffers(1, &bloom_a_fbo_);
        bloom_a_fbo_ = 0;
    }
    if (bloom_a_tex_ != 0) {
        glDeleteTextures(1, &bloom_a_tex_);
        bloom_a_tex_ = 0;
    }
    if (bloom_b_fbo_ != 0) {
        glDeleteFramebuffers(1, &bloom_b_fbo_);
        bloom_b_fbo_ = 0;
    }
    if (bloom_b_tex_ != 0) {
        glDeleteTextures(1, &bloom_b_tex_);
        bloom_b_tex_ = 0;
    }
    bloom_a_width_ = bloom_a_height_ = 0;
    bloom_b_width_ = bloom_b_height_ = 0;
}

void Renderer::ensure_bloom_fbos(uint32_t base_w, uint32_t base_h) {
    // Target sizes: 1/2 and 1/4 of the scene FBO. Round down so we
    // don't wind up sampling past the source texture edges. Min 1px
    // to avoid GL errors at degenerate sizes.
    uint32_t a_w = (base_w >= 2) ? (base_w / 2) : 1;
    uint32_t a_h = (base_h >= 2) ? (base_h / 2) : 1;
    uint32_t b_w = (a_w >= 2) ? (a_w / 2) : 1;
    uint32_t b_h = (a_h >= 2) ? (a_h / 2) : 1;

    // Skip rebuild if dimensions unchanged and FBOs are alive.
    if (bloom_a_fbo_ != 0 && bloom_b_fbo_ != 0 &&
        bloom_a_width_ == a_w && bloom_a_height_ == a_h &&
        bloom_b_width_ == b_w && bloom_b_height_ == b_h) {
        return;
    }

    destroy_bloom_fbos();

    auto build_fbo = [](uint32_t w, uint32_t h, uint32_t& out_fbo, uint32_t& out_tex) -> bool {
        glGenFramebuffers(1, &out_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);

        glGenTextures(1, &out_tex);
        glBindTexture(GL_TEXTURE_2D, out_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // Bilinear filter is what gives the bloom its smooth upsample
        // when the main composite samples the quarter-res texture at
        // arbitrary screen-space coords. Clamp-to-edge avoids halo
        // smearing across the edge during corner sampling.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, out_tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindTexture(GL_TEXTURE_2D, 0);
        return status == GL_FRAMEBUFFER_COMPLETE;
    };

    if (!build_fbo(a_w, a_h, bloom_a_fbo_, bloom_a_tex_)) {
        std::cerr << "UI Renderer: bloom_a FBO incomplete; halation disabled" << std::endl;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy_bloom_fbos();
        return;
    }
    if (!build_fbo(b_w, b_h, bloom_b_fbo_, bloom_b_tex_)) {
        std::cerr << "UI Renderer: bloom_b FBO incomplete; halation disabled" << std::endl;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy_bloom_fbos();
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bloom_a_width_ = a_w; bloom_a_height_ = a_h;
    bloom_b_width_ = b_w; bloom_b_height_ = b_h;

    std::cout << "UI Renderer: bloom FBOs created — a=" << a_w << "x" << a_h
              << " (fbo=" << bloom_a_fbo_ << ", tex=" << bloom_a_tex_ << "), b="
              << b_w << "x" << b_h << " (fbo=" << bloom_b_fbo_
              << ", tex=" << bloom_b_tex_ << ")" << std::endl;
}

// Helper to render a fullscreen quad pass through the bloom downsample
// shader. Caller has already bound the destination FBO, set viewport,
// and bound the source texture on unit 0. We only set the per-pass
// uniforms, upload the quad geometry, and draw.
//
// File-scope static so it's invisible outside this translation unit
// and doesn't pollute the Renderer class header.
namespace {
void bloom_pass_draw(uint32_t shader_program,
                     uint32_t vbo,
                     uint32_t vao,
                     uint32_t dst_w,
                     uint32_t dst_h,
                     uint32_t src_w,
                     uint32_t src_h,
                     float luma_threshold) {
    glUseProgram(shader_program);

    // Vertex shader screenSize maps screen-space coords to NDC; we set
    // it to the destination FBO dims so position 0..dst_w × 0..dst_h
    // covers the full target.
    glUniform2f(glGetUniformLocation(shader_program, "screenSize"),
                static_cast<float>(dst_w), static_cast<float>(dst_h));
    glUniform2f(glGetUniformLocation(shader_program, "srcTexelSize"),
                1.0f / static_cast<float>(src_w),
                1.0f / static_cast<float>(src_h));
    glUniform1f(glGetUniformLocation(shader_program, "lumaThreshold"),
                luma_threshold);
    glUniform1i(glGetUniformLocation(shader_program, "srcTexture"), 0);

    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(dst_w), 0.0f, 1.0f, 0.0f,
        static_cast<float>(dst_w), static_cast<float>(dst_h), 1.0f, 1.0f,

        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(dst_w), static_cast<float>(dst_h), 1.0f, 1.0f,
        0.0f, static_cast<float>(dst_h), 0.0f, 1.0f
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}
}  // namespace

void Renderer::end_scene_fbo_and_composite(const app::AppState& state) {
    if (!scene_fbo_active_) return;
    scene_fbo_active_ = false;

    const auto& s = state.display_settings;

    // ===== Phase 5: build the halation chain (bloom_a → bloom_b) =====
    //
    // Run iff bloomIntensity > 0 AND the bloom shader compiled.
    // Each pass renders a fullscreen quad through the bloom downsample
    // shader: scene→bloom_a (with luma threshold), bloom_a→bloom_b
    // (no threshold). The composite below reads bloom_b as a softly-
    // blurred bright-only halation map.
    bool bloom_built = false;
    if (s.bloom_intensity > 0.0f && bloom_downsample_shader_program_ != 0) {
        ensure_bloom_fbos(scene_fbo_width_, scene_fbo_height_);
        if (bloom_a_fbo_ != 0 && bloom_b_fbo_ != 0) {
            // Pass 1: scene → bloom_a, with soft luma threshold (~0.7)
            //         so only bright pixels feed the bloom.
            glBindFramebuffer(GL_FRAMEBUFFER, bloom_a_fbo_);
            glViewport(0, 0,
                       static_cast<GLsizei>(bloom_a_width_),
                       static_cast<GLsizei>(bloom_a_height_));
            glDisable(GL_BLEND);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, scene_color_tex_);
            bloom_pass_draw(bloom_downsample_shader_program_, vbo_, vao_,
                            bloom_a_width_, bloom_a_height_,
                            scene_fbo_width_, scene_fbo_height_,
                            /*luma_threshold=*/0.70f);

            // Pass 2: bloom_a → bloom_b, no threshold. Just a second
            //         Kawase 5-tap that further softens / widens the
            //         bloom radius.
            glBindFramebuffer(GL_FRAMEBUFFER, bloom_b_fbo_);
            glViewport(0, 0,
                       static_cast<GLsizei>(bloom_b_width_),
                       static_cast<GLsizei>(bloom_b_height_));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, bloom_a_tex_);
            bloom_pass_draw(bloom_downsample_shader_program_, vbo_, vao_,
                            bloom_b_width_, bloom_b_height_,
                            bloom_a_width_, bloom_a_height_,
                            /*luma_threshold=*/0.0f);

            bloom_built = true;
        }
    }

    // ===== Main composite: scene_fbo (+ bloom_b) → default FB =====
    //
    // Re-target the default framebuffer at full HDMI mode size. The
    // composite covers everything; bezel/toast follow on top.
    //
    // Use framebuffer_* (the actual HDMI mode dims), not original_*
    // (the logical UI canvas). In Modern TV these are equal and
    // behavior is unchanged. In CRT_NATIVE the logical canvas is
    // 640×480 but the framebuffer is 1280×720 — using original_*
    // here was the bug: it caused the composite to render only into
    // the bottom-left 640×480 of the screen. See begin_scene_fbo()
    // above for the matching fbo-size fix.
    //
    // Fallback path preserved for callers that haven't wired
    // set_framebuffer_size() yet (treat 0 as "unset" → original_*).
    const uint32_t fb_w = framebuffer_width_  ? framebuffer_width_  : original_width_;
    const uint32_t fb_h = framebuffer_height_ ? framebuffer_height_ : original_height_;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0,
               static_cast<GLsizei>(fb_w),
               static_cast<GLsizei>(fb_h));

    glUseProgram(crt_composite_shader_program_);

    // Uniform setup mirrors render_crt_effects, plus the scene + bloom textures.
    glUniform2f(glGetUniformLocation(crt_composite_shader_program_, "screenSize"),
                static_cast<float>(original_width_),
                static_cast<float>(original_height_));

    auto now = std::chrono::steady_clock::now();
    float time = std::chrono::duration<float>(now.time_since_epoch()).count();
    glUniform1f(glGetUniformLocation(crt_composite_shader_program_, "time"), time);

    float effective_scanline_intensity =
        last_scanlines_enabled_ ? s.scanline_intensity : 0.0f;
    glUniform1f(glGetUniformLocation(crt_composite_shader_program_, "scanlineIntensity"),
                effective_scanline_intensity);
    glUniform1f(glGetUniformLocation(crt_composite_shader_program_, "warmthIntensity"),
                s.warmth_intensity);
    glUniform1f(glGetUniformLocation(crt_composite_shader_program_, "glowIntensity"),
                s.glow_intensity);
    glUniform1f(glGetUniformLocation(crt_composite_shader_program_, "rgbMaskIntensity"),
                s.rgb_mask_intensity);
    // Bloom intensity gates BOTH the bloom build above and the
    // composite-side screen-blend. If the build failed (bloom_built
    // false) we force the composite-side intensity to 0 so the
    // shader's `if (bloomIntensity > 0.0)` branch doesn't try to
    // sample a stale or unbound bloom texture.
    glUniform1f(glGetUniformLocation(crt_composite_shader_program_, "bloomIntensity"),
                bloom_built ? s.bloom_intensity : 0.0f);
    glUniform1f(glGetUniformLocation(crt_composite_shader_program_, "interlacingIntensity"),
                s.interlacing_intensity);
    glUniform1f(glGetUniformLocation(crt_composite_shader_program_, "flickerIntensity"),
                s.flicker_intensity);

    // Bind the scene texture on unit 0 for sampling.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_color_tex_);
    glUniform1i(glGetUniformLocation(crt_composite_shader_program_, "sceneTexture"), 0);

    // Bind the bloom (halation) texture on unit 1 if available; the
    // shader only reads this when its bloomIntensity uniform > 0,
    // which we've gated on bloom_built. When bloom is off we still
    // bind unit 1 to texture 0 (no-op default) to keep the sampler
    // valid on drivers that warn about unbound samplers.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloom_built ? bloom_b_tex_ : 0u);
    glUniform1i(glGetUniformLocation(crt_composite_shader_program_, "bloomTexture"), 1);

    // Composite is opaque (the shader inlines the OVER blend against
    // sceneRGB). Disabling blend avoids accidental further compositing
    // against the default-FB clear color.
    glDisable(GL_BLEND);

    // Fullscreen quad over the default-FB extent. The vertex shader
    // turns position/screenSize into NDC, so we use HDMI mode coords.
    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(original_width_), 0.0f, 1.0f, 0.0f,
        static_cast<float>(original_width_), static_cast<float>(original_height_), 1.0f, 1.0f,

        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(original_width_), static_cast<float>(original_height_), 1.0f, 1.0f,
        0.0f, static_cast<float>(original_height_), 0.0f, 1.0f
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore the rest of the GL state subsequent overlay code expects:
    //   - default 2D shader bound (bezel/toast use it)
    //   - blending re-enabled with the standard alpha-blend func
    //   - no texture pinned to either active unit
    //   - active texture unit returned to 0 (most overlay code assumes
    //     unit 0 is current)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(shader_program_);
}

void Renderer::render_crt_effects(const app::AppState& state, bool scanlines_enabled) {
    if (crt_shader_program_ == 0) return;

    // Enhanced pipeline: skip the legacy procedural overlay. The same
    // effects will be applied (with extra fidelity in Phases 2+) by
    // end_scene_fbo_and_composite, which has access to the scene
    // texture. Drawing the overlay here on top of the FBO would
    // double-apply darkening/tinting once the composite runs.
    if (scene_fbo_active_) return;

    // Check if any effects are active
    const auto& s = state.display_settings;
    if (s.scanline_intensity <= 0.0f && s.warmth_intensity <= 0.0f && 
        s.glow_intensity <= 0.0f && s.rgb_mask_intensity <= 0.0f && 
        s.bloom_intensity <= 0.0f && s.interlacing_intensity <= 0.0f && 
        s.flicker_intensity <= 0.0f) {
        return;
    }
    
    glUseProgram(crt_shader_program_);
    
    // Set uniforms
    glUniform2f(glGetUniformLocation(crt_shader_program_, "screenSize"), static_cast<float>(width_), static_cast<float>(height_));
    
    auto now = std::chrono::steady_clock::now();
    float time = std::chrono::duration<float>(now.time_since_epoch()).count();
    glUniform1f(glGetUniformLocation(crt_shader_program_, "time"), time);
    
    // Scanlines are only enabled if the UI is visible (scanlines_enabled flag)
    // OR if scanline intensity is set to a value > 0 and we want to force them?
    // User request: "except for the scan lines. Make these only present during the video UI."
    // So if scanlines_enabled is false, we force intensity to 0.
    float effective_scanline_intensity = scanlines_enabled ? s.scanline_intensity : 0.0f;
    glUniform1f(glGetUniformLocation(crt_shader_program_, "scanlineIntensity"), effective_scanline_intensity);
    
    glUniform1f(glGetUniformLocation(crt_shader_program_, "warmthIntensity"), s.warmth_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "glowIntensity"), s.glow_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "rgbMaskIntensity"), s.rgb_mask_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "bloomIntensity"), s.bloom_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "interlacingIntensity"), s.interlacing_intensity);
    glUniform1f(glGetUniformLocation(crt_shader_program_, "flickerIntensity"), s.flicker_intensity);
    
    // Draw full screen quad
    // We reuse the existing VBO which has a quad from (-1,-1) to (1,1) in clip space?
    // Wait, the vertex shader expects 'position' and 'texCoord'.
    // The VBO setup in initialize() creates a quad for the whole screen.
    // Vertex shader:
    // in vec2 position;
    // in vec2 texCoord;
    // uniform vec2 screenSize;
    // normalizedPos = (position / screenSize) * 2.0 - 1.0;
    
    // So we need to pass position as screen coordinates (0..width, 0..height)
    // The VBO setup in initialize() creates a quad:
    // 0, 0, 0, 0
    // width, 0, 1, 0
    // width, height, 1, 1
    // 0, height, 0, 1
    // Wait, I need to check initialize() again to be sure about VBO content.
    // But draw_quad uses it.
    
    // Let's just use draw_quad? No, draw_quad uses shader_program_.
    // We need to manually draw using crt_shader_program_.
    
    // Re-upload quad data to VBO if needed?
    // draw_quad uploads data every time.
    // Let's do the same here for simplicity.
    
    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(width_), 0.0f, 1.0f, 0.0f,
        static_cast<float>(width_), static_cast<float>(height_), 1.0f, 1.0f,
        
        0.0f, 0.0f, 0.0f, 0.0f,
        static_cast<float>(width_), static_cast<float>(height_), 1.0f, 1.0f,
        0.0f, static_cast<float>(height_), 0.0f, 1.0f
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    
    // Restore standard shader
    glUseProgram(shader_program_);
}

void Renderer::cleanup() {
    if (shader_program_ != 0) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (crt_shader_program_ != 0) {
        glDeleteProgram(crt_shader_program_);
        crt_shader_program_ = 0;
    }
    if (crt_composite_shader_program_ != 0) {
        glDeleteProgram(crt_composite_shader_program_);
        crt_composite_shader_program_ = 0;
    }
    if (bloom_downsample_shader_program_ != 0) {
        glDeleteProgram(bloom_downsample_shader_program_);
        bloom_downsample_shader_program_ = 0;
    }
    destroy_scene_fbo();
    destroy_bloom_fbos();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (logo_texture_id_ != 0) {
        glDeleteTextures(1, &logo_texture_id_);
        logo_texture_id_ = 0;
    }
    if (bezel_texture_id_ != 0) {
        glDeleteTextures(1, &bezel_texture_id_);
        bezel_texture_id_ = 0;
    }
    if (marquee_frame_texture_id_ != 0) {
        glDeleteTextures(1, &marquee_frame_texture_id_);
        marquee_frame_texture_id_ = 0;
    }
    if (thumbnail_texture_id_ != 0) {
        glDeleteTextures(1, &thumbnail_texture_id_);
        thumbnail_texture_id_ = 0;
    }
    for (auto& pair : system_logo_cache_) {
        if (pair.second.texture_id != 0) {
            glDeleteTextures(1, &pair.second.texture_id);
        }
    }
    system_logo_cache_.clear();
    if (title_font_manager_) {
        title_font_manager_->cleanup();
    }
    if (body_font_manager_) {
        body_font_manager_->cleanup();
    }
}

void Renderer::render_qr_code(const std::string& url, float x, float y, float size, float alpha_multiplier) {
    if (url.empty()) return;
    
    try {
        // Generate QR code from URL
        qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        int qr_size = qr.getSize();
        if (qr_size <= 0) return;
        
        // Calculate module (cell) size
        float module_size = size / static_cast<float>(qr_size);
        
        // Add quiet zone (border) around QR code - standard is 4 modules
        int quiet_zone = 2;  // Use 2 for compact display
        float total_size = size + (quiet_zone * 2 * module_size);
        
        // Draw white background for QR code (including quiet zone)
        ui::Color white(255, 255, 255, 255);
        ui::Color black(0, 0, 0, 255);
        draw_quad(x - quiet_zone * module_size, y - quiet_zone * module_size, 
                  total_size, total_size, white, alpha_multiplier);
        
        // Draw black modules
        for (int row = 0; row < qr_size; row++) {
            for (int col = 0; col < qr_size; col++) {
                if (qr.getModule(col, row)) {  // True = black module
                    float px = x + col * module_size;
                    float py = y + row * module_size;
                    draw_quad(px, py, module_size, module_size, black, alpha_multiplier);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to generate QR code: " << e.what() << std::endl;
    }
}

} // namespace ui

