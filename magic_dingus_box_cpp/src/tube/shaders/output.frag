// output.frag - THE OUTPUT pass, one per output pixel: map the pixel through the
// bend, read the picture (the scene itself, ONE signal tap, and/or TWO line taps),
// the beam, the mask (a texture), glow, ABL, the motion block, tint, power, then
// the BAKED glass (one texelFetch) and encode. crt.metal's tubeLight; its
// coefficients and flash-safety caps unchanged.
//
// By define, as the fixed table builds them:
//   OUT_SCENE        reads the scene itself (uiTex); with
//     OUT_TONE       contrast and saturation, and
//     OUT_DISPLACE   jitter, tracking slip and flagging, before the beam
//   OUT_SIGNAL       reads one tap of the signal pass instead
//   OUT_RASTER       two line taps, mixed in by `raster`
//   OUT_RASTER_ONLY  the picture is the lines alone (TRUE RASTER at 100)
//   OUT_BEND         the per-pixel bend (else the flat fit)
//   OUT_SCANLINES    the beam profile; OUT_BEAM_CHEAP its one-sigma form
//   OUT_ABL          ABL and breathing (the mean probe's level)
//   OUT_MOTION       noise, flicker, hum, the rolling band, purity
//   OUT_GLOW         the glow tap
//   OUT_HISTORY      afterglow: linear light into the history, max'd with the
//                    previous frame's (else gamma-encoded to the screen)
// In EVERY output, each behind its own uniform test: the highlight roll-off,
// COLOUR TEMP's tint and the power block (power-off collapse, degauss, the
// power-off spot, warm-up). Inside OUT_MOTION each effect has its own test too.
//
// The glass texture's A is inside * vignette * rim and its RGB the glass light
// times inside, times 32 (glass.frag). crt.metal multiplies the picture by
// vignette * rim * tint and later by flicker, hum, band, purity, power and
// inside: all products, so multiplying by A at the end is the same light. The
// glass is added after, as there; the power-off spot is added unmasked (it only
// exists at the centre, where inside is 1).
//
// DIFFERENCES FROM crt.metal: the opening's edges are straight; the flat fit is
// one number for both axes; the mask is read from a texture the renderer bakes
// from maskType, maskStrength and maskPitch (so the shader reads none of those
// three, and maskStrength and maskPitch are declared only because the renderer
// uploads every uniform field by name); the glass black does not dim with
// `power` (glass.frag). With OUT_SCENE the displacements are crt.metal's own
// (tracking and flagging keyed on the OUTPUT row p.y); with OUT_SIGNAL they were
// done in the signal pass, keyed on the source row (signal.frag).
//
// THE d* UNIFORMS are what this file used to work out per pixel from uniforms
// alone (reciprocals, products, smoothsteps of uniforms), worked out once a frame
// on the CPU: tube/tube_derived.h, where each names the expression it replaces.
// The flash-safety caps are NOT among them: each stays here, on the raw uniform.

#if defined(OUT_SCENE) && defined(OUT_SIGNAL)
#error OUT_SCENE and OUT_SIGNAL are two sources for one picture
#endif
#if !defined(OUT_SCENE) && !defined(OUT_SIGNAL) && !defined(OUT_RASTER_ONLY)
#error an output needs OUT_SCENE, OUT_SIGNAL or OUT_RASTER_ONLY
#endif
#if defined(OUT_RASTER_ONLY) && !defined(OUT_RASTER)
#error OUT_RASTER_ONLY needs OUT_RASTER
#endif
#if (defined(OUT_TONE) || defined(OUT_DISPLACE)) && !defined(OUT_SCENE)
#error OUT_TONE and OUT_DISPLACE belong to OUT_SCENE (the signal pass does them otherwise)
#endif

uniform float outW, outH, pxPerMm;
uniform float scanlines, raster, beamMin;
uniform float maskStrength, maskGain, maskPitch;
uniform float glow, brightness;
uniform vec3 tint;
uniform float power, degauss, degaussPhaseA, degaussPhaseB, spot, warmupGain;
uniform float noise, noiseSeed, flicker, flickerWave, hum;                           // OUT_MOTION
uniform float rollAmount, rollPosition, rollTint;
uniform float purityAmount, purityR, purityG, purityB;
uniform float contrast, saturation;                                                  // OUT_TONE
uniform float jitter, jitterSeed, jitterBias;                                        // OUT_DISPLACE
uniform float trackingOffset, trackingPosition, flagOffset;
// tube_derived.h
uniform float dInvW, dInvH, dInvHalfW, dInvHalfH, dAspect;
uniform float dOpenInvH, dOpenInvV, dDegaussAmp, dPowerGain;
uniform float dBendInvX, dBendInvY, dBendB, dFlatFitInv;                             // OUT_BEND (else dFlatFitInv)
uniform float dLinesPerPx, dInvRows, dBeamRange;                                     // OUT_SCANLINES / OUT_RASTER
uniform float dRollGain, dRollStart, dRollRoom, dRollRoomInv;
uniform float dInvMaskW, dInvMaskH;
uniform float dBreatheInv, dAblGain;                                                 // OUT_ABL
uniform float dGrainInvX, dGrainInvY, dHumScale, dHumPhase, dRollWidthInv;          // OUT_MOTION
uniform float dPurityCx, dPurityCy, dInvMm, dPurityRadiusInv;
uniform float dDecayR, dDecayG, dDecayB;                                             // OUT_HISTORY
uniform float dTrackWidthInv, dFlagExtentInv;                                        // OUT_DISPLACE
uniform vec2 uOrigin;            // the target's viewport origin (gl_FragCoord px)
uniform sampler2D signalTex;     // OUT_SIGNAL
uniform sampler2D glowTex;       // OUT_GLOW
uniform sampler2D prev;          // OUT_HISTORY: the previous frame's history
uniform sampler2D linesTex;      // OUT_RASTER
uniform sampler2D glassTex;      // the baked glass (RGBA8, RGB x32)
uniform sampler2D maskTex;       // the mask, one period (NEAREST, REPEAT)

out vec4 fragColor;

// GLSL's smoothstep(0.0, edge, x) with 1.0 / edge handed in (tube_derived.h).
float rampTo(float x, float invEdge)
{
    float t = clamp(x * invEdge, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// The raster's deflection: POWER-OFF collapses it to a line, then a dot (startup
// keeps it full-sized); BREATHING swells it with a bright picture; DEGAUSS wobbles
// it, decaying.
vec2 ndOf(vec2 n)
{
    vec2 nd = n;
    if (power < 1.0)
        nd = n * vec2(dOpenInvH, dOpenInvV);           // n / vec2(hOpen, vOpen)
#ifdef OUT_ABL
    nd *= dBreatheInv;                                   // nd /= 1.0 + breathe * 0.008 * ablLevel
#endif
    if (degauss > 0.0)
        nd += dDegaussAmp * vec2(sin(degaussPhaseA + n.y * 9.0), sin(degaussPhaseB + n.x * 7.0));
    return nd;
}

// crt.metal's pictureCoord: the out pixel -> the picture pixel it shows.
vec2 bend(vec2 nd)
{
    vec2 outSize = vec2(outW, outH);
#ifdef OUT_BEND
    vec2 s = nd * vec2(dBendInvX, dBendInvY);            // nd / mix(flatFit, inset, curvature): straight edges
    float b = dBendB;                                    // 0.045 * curvature: a touch of tube, edges fixed
    s = s * (1.0 - b * (1.0 - s * s));
#else
    vec2 s = nd * dFlatFitInv;
#endif
    return (s * 0.5 + 0.5) * outSize;
}

#if defined(OUT_SCANLINES) || defined(OUT_RASTER)
vec3 beamWeight(float d, vec3 c)    // d in line units
{
#ifdef OUT_BEAM_CHEAP
    // ONE sigma from the luma: one sqrt, one exp, one divide (not three each).
    float s = beamMin + dBeamRange * sqrt(clamp(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0));
    return vec3(exp(-0.5 * d * d / (s * s)) / (s * 2.5066));
#else
    vec3 sigma = beamMin + dBeamRange * sqrt(clamp(c, 0.0, 1.0));   // mix(beamMin, beamMax, sqrt(c))
    return exp(-0.5 * d * d / (sigma * sigma)) / (sigma * 2.5066);
#endif
}
#endif

#if defined(OUT_DISPLACE) || defined(OUT_MOTION)
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
#endif

// HIGHLIGHTS ROLL OFF on the brightest channel, before the mask, and all three
// scale by what it lost. The gain test is NOT an optimisation: below a gain of 0
// `room` is negative and the curve grows instead of settling (crt.metal).
// gain = maskGain * brightness - 1, start = 1 - 0.2 * min(gain, 1), room = 1 - start.
vec3 rollOff(vec3 x)
{
    if (dRollGain <= 0.0)
        return x;
    float start = dRollStart, room = dRollRoom;
    float m = max(x.r, max(x.g, x.b));
    if (m <= start)
        return x;
    return x * (start + room * (1.0 - exp(-(m - start) * dRollRoomInv))) / m;
}

void main()
{
    vec2 outSize = vec2(outW, outH);
    vec2 p = vec2(gl_FragCoord.x - uOrigin.x, outH - (gl_FragCoord.y - uOrigin.y));
    vec2 n = p * vec2(dInvHalfW, dInvHalfH) - 1.0;      // p / (outSize * 0.5) - 1.0
    vec2 nd = ndOf(n);
    vec2 sp = bend(nd);
    // |s| <= 1 is sp inside the picture; the squashed raster ends where it ends.
    bool lit = all(greaterThanEqual(sp, vec2(0.0))) && all(lessThanEqual(sp, outSize))
            && max(abs(nd.x), abs(nd.y)) <= 1.0;
#ifdef OUT_DISPLACE
    float mm = pxPerMm;
    // JITTER: each line's horizontal sync lands a hair late or early.
    // THE CAP (crt.metal): +-0.25 mm a line at 1.
    if (jitter > 0.0) {
        float lineNo = floor(sp.y * dLinesPerPx);       // sp.y / (outH / lines)
        sp.x += min(jitter, 1.0) * 0.50 * mm * (hash(vec2(lineNo, jitterSeed)) + jitterBias);
    }
    if (trackingOffset != 0.0) {
        float dist = abs(p.y * dInvH - trackingPosition);
        float strip = 1.0 - rampTo(dist, dTrackWidthInv);
        sp.x += clamp(trackingOffset, -3.6 * mm, 3.6 * mm) * strip;
    }
    if (flagOffset != 0.0) {
        float top = 1.0 - rampTo(p.y * dInvH, dFlagExtentInv);
        sp.x += clamp(flagOffset, -4.0 * mm, 4.0 * mm) * top * top;
    }
#endif
    vec4 glass = texelFetch(glassTex, ivec2(gl_FragCoord.xy - uOrigin), 0);
    vec3 col = vec3(0.0);
    if (lit && glass.a > 0.0) {
        vec2 uv = flipV(sp * vec2(dInvW, dInvH));
#ifdef OUT_RASTER
        // RASTER: the picture rebuilt from `lines` true scanlines, the two nearest line centres.
        float i0 = floor(sp.y * dLinesPerPx - 0.5);
        float d0 = sp.y * dLinesPerPx - 0.5 - i0;
        vec3 c0 = texture(linesTex, flipV(vec2(sp.x * dInvW, (i0 + 0.5) * dInvRows))).rgb;
        vec3 c1 = texture(linesTex, flipV(vec2(sp.x * dInvW, (i0 + 1.5) * dInvRows))).rgb;
        vec3 traced = c0 * beamWeight(d0, c0) + c1 * beamWeight(d0 - 1.0, c1);
        vec3 blended = mix(c0, c1, smoothstep(0.0, 1.0, d0));
        vec3 rastered = mix(blended, traced, scanlines);
#endif
#ifdef OUT_RASTER_ONLY
        vec3 beamed = rastered;                       // raster 1: mix(native, rastered, 1)
#else
#ifdef OUT_SCENE
        vec3 c = uiTex(sp * vec2(dInvW, dInvH));
#ifdef OUT_TONE
        // TONE: CONTRAST in linear light, then SATURATION about luma. Black stays black.
        if (contrast != 1.0)
            c = pow(max(c, 0.0), vec3(contrast));
        if (saturation != 1.0)
            c = max(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, saturation), 0.0);
#endif
#else
        vec3 c = texture(signalTex, uv).rgb;
#endif
#ifdef OUT_SCANLINES
        // NATIVE: every picture row kept; the beam profile carves the gaps, wider where it is bright.
        float f = fract(sp.y * dLinesPerPx) - 0.5;
        vec3 beamed = c * mix(vec3(1.0), beamWeight(f, c), scanlines);
#else
        vec3 beamed = c;
#endif
#ifdef OUT_RASTER
        beamed = mix(beamed, rastered, raster);
#endif
#endif
        col = rollOff(beamed * brightness);
        col *= texture(maskTex, (floor(p) + 0.5) * vec2(dInvMaskW, dInvMaskH)).rgb * maskGain;
#ifdef OUT_GLOW
        if (glow > 0.0)   // halation, built in linear light; a row at 0 reads nothing
            col += glow * texture(glowTex, uv).rgb;
#endif
#ifdef OUT_ABL
        // ABL: the beam-current limiter pulls a bright screen down.
        col *= dAblGain;                                  // 1.0 - abl * 0.22 * ablLevel
#endif
#ifdef OUT_MOTION
        // NOISE: snow in the signal, strongest where the picture is dark, in a grain of
        // about 0.5 x 0.6 mm. THE CAP (crt.metal): whatever it is handed, +-1 % of the
        // light, plus a grain a quarter of a percent of white at most.
        if (noise > 0.0) {
            float nz = min(noise, 0.2);
            vec2 grain = floor(p * vec2(dGrainInvX, dGrainInvY));   // p / max(vec2(0.5, 0.6) * pxPerMm, 1.0)
            float r = hash(grain + noiseSeed) - 0.5;
            col = max(col * (1.0 + nz * 0.10 * r) + nz * 0.012 * (r + 0.5), 0.0);
        }
#endif
        // COLOUR TEMP: phosphor white.
        if (tint != vec3(1.0))
            col *= tint;
#ifdef OUT_MOTION
        // FLICKER: the light breathing. THE CAP (crt.metal): +-2.5 % at 1. crt.metal
        // computes the wave here (|wave| <= 1 by construction); it arrives as a uniform,
        // so it is clamped here too, and the cap holds whatever the CPU uploads.
        if (flicker > 0.0)
            col *= 1.0 + min(flicker, 1.0) * 0.025 * clamp(flickerWave, -1.0, 1.0);
        // HUM: one soft band rolling up the tube. THE CAP (crt.metal): at most 25 % darker at 1.
        if (hum > 0.0)
            col *= 1.0 - min(hum, 1.0) * 0.25 * pow(max(0.5 + 0.5 * sin(p.y * dHumScale + dHumPhase), 0.0), 2.32);
        // ROLLING BAND: a downward rolling discoloration, separate from HUM.
        if (rollAmount > 0.0) {
            float dist = abs(p.y * dInvH - rollPosition);
            dist = min(dist, 1.0 - dist);
            float band = 1.0 - rampTo(dist, dRollWidthInv);
            float rtint = clamp(rollTint, -1.0, 1.0);
            col *= 1.0 - clamp(rollAmount, 0.0, 2.0) * band * vec3(0.22 + 0.06 * rtint, 0.22, 0.22 - 0.06 * rtint);
        }
        // PURITY: a patch of the tube tinted by a magnetised mask.
        if (purityAmount > 0.0) {
            vec2 distanceMm = (p - vec2(dPurityCx, dPurityCy)) * dInvMm;
            float spotPatch = 1.0 - rampTo(length(distanceMm), dPurityRadiusInv);
            vec3 hue = clamp(vec3(purityR, purityG, purityB), 0.0, 1.0);
            col *= 1.0 - 0.45 * clamp(purityAmount, 0.0, 1.0) * spotPatch * (1.0 - hue);
        }
#endif
        // POWER: the shrinking raster concentrates the beam.
        if (power < 1.0)
            col *= dPowerGain;                             // 1.0 + 2.5 * (1.0 - vOpen)
        col *= glass.a;
    }
    col += glass.rgb / 32.0;
    if (spot > 0.0) {   // POWER-OFF: the beam parks in the centre, a white-hot dot and its halo, fading
        vec2 q = n * vec2(dAspect, 1.0);
        float r2 = dot(q, q);
        col += spot * (6.0 * exp(-r2 * 9000.0) + 0.30 * exp(-r2 * 260.0)) * vec3(0.92, 1.0, 0.96);
    }
    col *= warmupGain;
#ifdef OUT_HISTORY
    // AFTERGLOW: P22's green tail and red linger; blue goes first.
    vec3 decay = vec3(dDecayR, dDecayG, dDecayB);     // persistence * vec3(0.68, 0.75, 0.45)
    col = max(col, texelFetch(prev, ivec2(gl_FragCoord.xy - uOrigin), 0).rgb * decay);
    fragColor = vec4(col, 1.0);                 // linear: the sRGB history encodes
#else
    fragColor = vec4(encodeGamma(col), 1.0);
#endif
}
