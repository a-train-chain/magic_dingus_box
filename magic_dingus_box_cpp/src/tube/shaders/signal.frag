// signal.frag - THE SIGNAL, once a frame, into a texture in source (picture)
// space: what the tube is DRIVEN with, before the beam, the mask or the glass see
// it (crt.metal's sampleConverged and toned, and the horizontal displacements of
// tubeLight). Linear light out (RGBA16F, or an sRGB target that encodes).
//
// Always compiled, each behind its own uniform test: TONE (contrast,
// saturation) and the three horizontal displacements (jitter, tracking slip,
// flagging). By define, as the fixed table builds them:
//   SIG_SHARP   the left/right taps, switched off inside by `sharpness != 1.0`
//   SIG_GUNS    convergence + colour drift: each gun read at its own offset
//   SIG_CHROMA  chroma bleed: six quarter-picture taps + YIQ
//
// DIFFERENCES FROM crt.metal (each is in source space, before the bend):
//   - convergence's offset is the SOURCE position times convergence, not the
//     output pixel's (they differ by the bend: a few % at the edges);
//   - tracking and flagging key on the source row (sp.y), not the output row
//     (p.y): the band sits where the bend puts that row, a few px off;
//   - at half width the texture is half as wide: each texel samples the scene
//     at the boundary of two source pixels and the output pass interpolates it
//     back.
//
// The d* uniforms are values this file used to work out per pixel from uniforms
// alone, now worked out once a frame (tube/tube_derived.h). The caps (jitter's,
// and the tracking and flagging clamps) stay here, on the raw uniforms.

// DRIVER HAZARD (measured: docs/engineering/measurements/2026-11-15-tube-hang-isolation.txt).
// The Pi 5's shader compiler (Mesa V3D) hung, 4 runs of 4, on this program built
// with chroma bleed and convergence but WITHOUT the sharpness path. Every signal
// program in the table carries SIG_SHARP; these refuse to build one that does not.
#if defined(SIG_CHROMA) && !defined(SIG_SHARP)
#error SIG_CHROMA without SIG_SHARP is a known compiler hang on the Pi 5
#endif
#if defined(SIG_GUNS) && !defined(SIG_SHARP)
#error SIG_GUNS without SIG_SHARP is a known compiler hang on the Pi 5 when chroma joins it
#endif

uniform float outW, outH, pxPerMm;
uniform float sharpness, contrast, saturation;
uniform float jitter, jitterSeed, jitterBias;
uniform float trackingOffset, trackingPosition, flagOffset;
// tube_derived.h
uniform float dInvW, dInvH, dLinesPerPx, dTrackWidthInv, dFlagExtentInv;
uniform float dInvSigW, dInvSigH;   // 1 / the signal texture's size, texels
#ifdef SIG_GUNS
uniform float convergence, driftOffset;
uniform float dInvHalfW, dInvHalfH;
#endif
#ifdef SIG_CHROMA
uniform float chromaBleed;
uniform float dChromaStep;      // chromaBleed / 2.5
uniform sampler2D quarter;      // the quarter picture (sRGB, linear on read)
#endif

out vec4 fragColor;

vec2 srcSize() { return vec2(outW, outH); }

// GLSL's smoothstep(0.0, edge, x) with 1.0 / edge handed in (tube_derived.h).
float rampTo(float x, float invEdge)
{
    float t = clamp(x * invEdge, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

vec3 sampleUI(vec2 sp)
{
    vec2 uv = sp * vec2(dInvW, dInvH);   // sp / srcSize()
    vec3 c = uiTex(uv);
#ifdef SIG_SHARP
    // SHARPNESS: the signal's horizontal bandwidth. Kept compiled in every signal
    // program (the DRIVER HAZARD above) and switched off here when neutral.
    if (sharpness != 1.0) {
        vec2 dx = vec2(dInvW, 0.0);        // 1.0 / srcSize().x
        vec3 l = uiTex(uv - dx), r = uiTex(uv + dx);
        c = max(mix((l + 2.0 * c + r) * 0.25, c, sharpness), 0.0);   // > 1 overshoots: crisper
    }
#endif
    return c;
}

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

#ifdef SIG_CHROMA
vec3 rgbToYiq(vec3 c) { return vec3(dot(c, vec3(0.299, 0.587, 0.114)), dot(c, vec3(0.596, -0.274, -0.322)), dot(c, vec3(0.211, -0.523, 0.312))); }
vec3 yiqToRgb(vec3 y) { return vec3(y.x + 0.956 * y.y + 0.621 * y.z, y.x - 0.272 * y.y - 0.647 * y.z, y.x - 1.106 * y.y + 1.703 * y.z); }
vec3 withChroma(vec3 c, vec2 iq) { return max(yiqToRgb(vec3(dot(c, vec3(0.299, 0.587, 0.114)), iq)), 0.0); }
#endif

// THE GUNS COME AFTER THE SIGNAL (crt.metal): each gun reads the bled signal at
// its own place, its own luma and the one smear of colour.
vec3 gunsRead(vec3 atR, vec3 atG, vec3 atB, vec2 iq, bool bleeds)
{
#ifdef SIG_CHROMA
    return bleeds ? vec3(withChroma(atR, iq).r, withChroma(atG, iq).g, withChroma(atB, iq).b) : vec3(atR.r, atG.g, atB.b);
#else
    return vec3(atR.r, atG.g, atB.b);
#endif
}

void main()
{
    // this texel's top-down source position, picture px
    vec2 uvTop = vec2(gl_FragCoord.x * dInvSigW, 1.0 - gl_FragCoord.y * dInvSigH);
    vec2 sp = uvTop * srcSize();
    float yN = sp.y * dInvH;            // stands in for crt.metal's p.y / outSize.y
    float mm = pxPerMm;                 // uiPxPerMm: the source is the picture
    // JITTER: each line's horizontal sync lands a hair late or early.
    // THE CAP (crt.metal): +-0.25 mm a line at 1.
    if (jitter > 0.0) {
        float lineNo = floor(sp.y * dLinesPerPx);   // sp.y / (srcSize().y / lines)
        sp.x += min(jitter, 1.0) * 0.50 * mm * (hash(vec2(lineNo, jitterSeed)) + jitterBias);
    }
    // TRACKING: a brief local sync slip, with soft boundaries.
    if (trackingOffset != 0.0) {
        float dist = abs(yN - trackingPosition);
        float strip = 1.0 - rampTo(dist, dTrackWidthInv);
        sp.x += clamp(trackingOffset, -3.6 * mm, 3.6 * mm) * strip;
    }
    // FLAGGING: the top of the picture bends sideways.
    if (flagOffset != 0.0) {
        float top = 1.0 - rampTo(yN, dFlagExtentInv);
        sp.x += clamp(flagOffset, -4.0 * mm, 4.0 * mm) * top * top;
    }
    // Chroma bleed and the guns below: never in a build without SIG_SHARP (the
    // DRIVER HAZARD at the top). The guards there refuse it; keep them.
    vec2 iq = vec2(0.0);
    bool bleeds = false;
#ifdef SIG_CHROMA
    // CHROMA BLEED: composite colour carries a third of luma's bandwidth, so colour
    // smears sideways and luma stays where it was. From the quarter picture, so a
    // reach of a few mm does not alias on what it steps over.
    bleeds = chromaBleed > 0.0;
    if (bleeds) {
        vec3 w = vec3(0.0);
        for (int k = 0; k < 6; ++k)
            w += rgbToYiq(texture(quarter, flipV((sp + vec2((float(k) - 2.5) * dChromaStep, 0.0)) * vec2(dInvW, dInvH))).rgb);
        iq = w.yz / 6.0;
    }
#endif
#ifdef SIG_GUNS
    // CONVERGENCE (misregistration grows toward the edges) + COLOUR DRIFT.
    vec2 off = (sp * vec2(dInvHalfW, dInvHalfH) - 1.0) * convergence + vec2(driftOffset, 0.0);
    vec3 c = gunsRead(sampleUI(sp + off), sampleUI(sp), sampleUI(sp - off), iq, bleeds);
#else
    vec3 g = sampleUI(sp);
    vec3 c = gunsRead(g, g, g, iq, bleeds);
#endif
    // TONE: CONTRAST in linear light, then SATURATION about luma. Black stays black.
    if (contrast != 1.0)
        c = pow(max(c, 0.0), vec3(contrast));
    if (saturation != 1.0)
        c = max(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, saturation), 0.0);
    fragColor = vec4(c, 1.0);
}
