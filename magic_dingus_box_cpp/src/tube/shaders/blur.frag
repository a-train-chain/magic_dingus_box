// blur.frag - one direction of the separable Gaussian GLOW blur on the quarter
// texture (H then V), standing in for Sound Factory's MPSImageGaussianBlur.
//
// Source and target are both sRGB textures: reads decode and writes encode in
// hardware, so this sums linear light. Taps are whole texels (no bilinear pair
// trick), 2r+1 of them, r = ceil(3 sigma) capped at MAX_RADIUS.

#define MAX_RADIUS 24

uniform sampler2D src;      // the quarter picture, or the H pass's result
uniform vec2 uDir;          // (1,0) horizontal, (0,1) vertical
uniform float uSigma;       // quarter-texture px
uniform int uRadius;        // ceil(3 sigma), <= MAX_RADIUS (the CPU clamps it too)

out vec4 fragColor;

void main()
{
    ivec2 size = textureSize(src, 0);
    ivec2 at = ivec2(gl_FragCoord.xy);
    int r = min(uRadius, MAX_RADIUS);
    // A floor on sigma: at 0 the factor is -inf and tap 0's 0 * -inf is NaN. Any real
    // glow sigma is far above it, so its weights are unchanged.
    float sigma = max(uSigma, 0.001);
    float k2 = -0.5 / (sigma * sigma);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 2 * MAX_RADIUS + 1; ++i) {
        int k = i - r;
        if (k > r) break;
        ivec2 q = clamp(at + ivec2(uDir) * k, ivec2(0), size - 1);
        float w = exp(float(k * k) * k2);
        sum += w * texelFetch(src, q, 0).rgb;
        wsum += w;
    }
    fragColor = vec4(sum / wsum, 1.0);
}
