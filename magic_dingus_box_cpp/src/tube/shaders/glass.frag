// glass.frag - THE GLASS, baked: drawn only when the setting or the picture's size
// changes. Everything here depends on position and settings alone (crt.metal's
// openingDistance, inside, vignette, rim, glass black and reflection):
//   A   = inside * vignette * rim          (output.frag multiplies the picture's light by it)
//   RGB = (glass black + reflection) * inside * 32   (output.frag divides by 32, then adds it)
// THE x32: the target is RGBA8 holding LINEAR light, and glass black at 1 is about
// 0.004, one step of 8 bits. Times 32 it spans about 33 steps; the most the
// glass reaches (glass black and reflection both at 1, at the highlight and an
// edge) is about 0.019, 0.61 after the scale, so nothing clips.
// DIFFERENCES FROM crt.metal: the opening's edges are straight (arcX = arcY = 1);
// glass black is not multiplied by `power` (the power-off animation is not
// baked: the glass stays lit while the raster collapses).

uniform float outW, outH, cornerPx, rim, vignette, glassBlack, reflection;

out vec4 fragColor;

void main()
{
    vec2 outSize = vec2(outW, outH);
    vec2 p = vec2(gl_FragCoord.x, outH - gl_FragCoord.y);
    vec2 n = p / (outSize * 0.5) - 1.0;
    // openingDistance with straight edges (openingEdge == 1)
    vec2 q = (abs(n) - 1.0) * (outSize * 0.5) + cornerPx;
    float sdf = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - cornerPx;
    float inside = clamp(0.5 - sdf, 0.0, 1.0);
    float edge = max(abs(n.x), abs(n.y)), edge2 = edge * edge;
    float vig = max(1.0 - vignette * (0.35 * dot(n, n) + 0.65 * edge2 * edge2 * edge2), 0.0);
    float rimK = 1.0 - rim * (1.0 - clamp(-sdf / 60.0, 0.0, 1.0));
    vec3 add = glassBlack * vec3(0.0031, 0.0040, 0.0037);
    if (reflection > 0.0) {
        vec2 g = n - vec2(-0.55, -0.62);
        add += reflection * 0.012 * exp(-dot(g, g) * 2.2) * vec3(1.0, 0.97, 0.92);    // a soft window highlight
        add += reflection * 0.003 * edge2 * edge2;                                       // fresnel at the edges
    }
    fragColor = vec4(add * inside * 32.0, vig * rimK * inside);
}
