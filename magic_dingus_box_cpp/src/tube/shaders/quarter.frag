// quarter.frag - the picture to the QUARTER texture (1/4 x 1/4), linear light,
// for GLOW (blurred next), CHROMA BLEED (read directly by the signal pass) and
// the mean probe (mipped down for ABL and breathing). Stands in for Sound
// Factory's MPSImageBilinearScale.
//
// Four bilinear taps, each centred on a 2x2 block of the 4x4 the quarter texel
// covers: a 16-texel box. Without SCENE_HW_DECODE each bilinear tap averages
// four gamma-encoded texels before uiTex decodes it: exact on flat areas, a
// little dark on edges.

uniform vec2 uPicTexel;     // one picture pixel in uv: 1 / the picture's size (tube_derived.h texel_size)
uniform vec2 uDstTexel;     // one target texel in uv: 1 / the quarter texture's size

out vec4 fragColor;

void main()
{
    // top-down uv of this quarter texel's centre
    vec2 uvTop = vec2(gl_FragCoord.x * uDstTexel.x, 1.0 - gl_FragCoord.y * uDstTexel.y);
    vec2 px = uPicTexel;        // one picture pixel in uv
    vec3 c = uiTex(uvTop + vec2(-px.x, -px.y))
           + uiTex(uvTop + vec2( px.x, -px.y))
           + uiTex(uvTop + vec2(-px.x,  px.y))
           + uiTex(uvTop + vec2( px.x,  px.y));
    fragColor = vec4(c * 0.25, 1.0);   // linear: the sRGB target encodes
}
