// lines.frag - TRUE RASTER's line pass: crt.metal's linePrepass, averaging the
// SIGNAL texture (already sharpened, converged, displaced and toned) over each
// raster line's height with the 5-tap tent. A LINE IS AN INTEGRAL, NOT A POINT:
// the beam sweeping one line lights everything drawn across its height.
// Output: signal width x ceil(lines), linear light.
// DIFFERENCES FROM crt.metal: tone is applied before the line average here (in
// the signal pass), after it there; and a line inherits the signal's vertical
// convergence offset, where crt.metal's lineAt offsets x only.

uniform float outW, outH, lines;
uniform sampler2D signalTex;

out vec4 fragColor;

void main()
{
    float rows = ceil(lines);
    float Ls = outH / lines;
    vec2 size = vec2(textureSize(signalTex, 0));
    float x = gl_FragCoord.x / size.x;
    float spy = (floor((1.0 - gl_FragCoord.y / rows) * rows) + 0.5) * Ls;
    float h = Ls * 0.5;
    vec3 c = vec3(0.0);
    const float w[5] = float[5](0.5, 1.0, 1.0, 1.0, 0.5);
    for (int k = 0; k < 5; ++k)
        c += w[k] * texture(signalTex, flipV(vec2(x, (spy + (float(k) - 2.0) * h * 0.5) / outH))).rgb;
    fragColor = vec4(c / 4.0, 1.0);
}
