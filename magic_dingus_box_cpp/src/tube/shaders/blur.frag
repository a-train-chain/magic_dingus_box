// blur.frag - one direction of the separable Gaussian GLOW blur on the quarter
// texture (H then V), standing in for Sound Factory's MPSImageGaussianBlur.
//
// Source and target are both sRGB textures: reads decode and writes encode in
// hardware, so this sums linear light. Taps are whole texels (no bilinear pair
// trick), 2r+1 of them, r = ceil(3 sigma) capped at MAX_RADIUS. The weights depend
// on sigma alone, so they are worked out once a frame (tube_derived.h
// blur_weights): exp(k * k * -0.5 / sigma^2) for |k| <= r, and 1 / their sum.

#define MAX_RADIUS 24

uniform sampler2D src;      // the quarter picture, or the H pass's result
uniform vec2 uDir;          // (1,0) horizontal, (0,1) vertical
uniform int uRadius;        // ceil(3 sigma), <= MAX_RADIUS (the CPU clamps it too)
uniform float uWeights[MAX_RADIUS + 1];   // weight[|k|]
uniform float uInvWeightSum;              // 1 / the sum of the 2r + 1 weights

out vec4 fragColor;

void main()
{
    ivec2 size = textureSize(src, 0);
    ivec2 at = ivec2(gl_FragCoord.xy);
    int r = min(uRadius, MAX_RADIUS);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 2 * MAX_RADIUS + 1; ++i) {
        int k = i - r;
        if (k > r) break;
        ivec2 q = clamp(at + ivec2(uDir) * k, ivec2(0), size - 1);
        sum += uWeights[abs(k)] * texelFetch(src, q, 0).rgb;
    }
    fragColor = vec4(sum * uInvWeightSum, 1.0);
}
