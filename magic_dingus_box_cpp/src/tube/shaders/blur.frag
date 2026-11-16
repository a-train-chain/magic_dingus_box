// blur.frag - one direction of the separable Gaussian GLOW blur (H then V), standing
// in for Sound Factory's MPSImageGaussianBlur.
//
// Source and target are both sRGB textures: reads decode and writes encode in
// hardware, so this sums linear light. 2r+1 taps, r = ceil(3 sigma) capped at
// MAX_RADIUS.
//
// TWO SOURCES (uDown):
//   0  the source is this target's own size (the quarter picture for a narrow glow, or
//      the H pass's result): each tap is one whole texel (texelFetch).
//   1  a WIDE glow's horizontal pass: the source is the quarter picture, twice this
//      target's size in each direction (tube_programs.h glow_half), and each tap is the
//      box average of the 2x2 quarter texels block (2bx, 2by)..(2bx+1, 2by+1) its
//      eighth-resolution texel covers: ONE bilinear tap at the block's shared corner,
//      uv = (2 * b + 1) / quarterSize, which weighs the four texels 1/4 each (the
//      source is LINEAR-filtered, CLAMP_TO_EDGE: a last block of an odd-sized source
//      averages its edge texels with themselves). sigma and r are then in
//      eighth-resolution texels (half the quarter's sigma).

#define MAX_RADIUS 24

uniform sampler2D src;      // the quarter picture, or the H pass's result
uniform vec2 uDir;          // (1,0) horizontal, (0,1) vertical
uniform float uSigma;       // this target's px
uniform int uRadius;        // ceil(3 sigma), <= MAX_RADIUS (the CPU clamps it too)
uniform int uDown;          // 1: src is twice this target's size; each tap a 2x2 box

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
    if (uDown != 0) {
        ivec2 blocks = (size + 1) / 2;          // this target's size
        vec2 invSize = 1.0 / vec2(size);
        for (int i = 0; i < 2 * MAX_RADIUS + 1; ++i) {
            int k = i - r;
            if (k > r) break;
            ivec2 b = clamp(at + ivec2(uDir) * k, ivec2(0), blocks - 1);
            float w = exp(float(k * k) * k2);
            sum += w * texture(src, vec2(2 * b + 1) * invSize).rgb;
            wsum += w;
        }
    } else {
        for (int i = 0; i < 2 * MAX_RADIUS + 1; ++i) {
            int k = i - r;
            if (k > r) break;
            ivec2 q = clamp(at + ivec2(uDir) * k, ivec2(0), size - 1);
            float w = exp(float(k * k) * k2);
            sum += w * texelFetch(src, q, 0).rgb;
            wsum += w;
        }
    }
    fragColor = vec4(sum / wsum, 1.0);
}
