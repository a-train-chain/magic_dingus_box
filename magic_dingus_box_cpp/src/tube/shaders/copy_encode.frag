// copy_encode.frag - AFTERGLOW's last step: the history (an sRGB texture, so the
// fetch is already linear light) to the screen, gamma-encoded. One texel fetch a
// pixel.

uniform sampler2D src;      // the history just written
uniform vec2 uOrigin;       // the target's viewport origin (gl_FragCoord px)
uniform ivec2 uSrcOffset;   // where the picture starts in `src`, texels (bottom-left)

out vec4 fragColor;

void main()
{
    vec3 c = texelFetch(src, ivec2(gl_FragCoord.xy - uOrigin) + uSrcOffset, 0).rgb;
    fragColor = vec4(encodeGamma(c), 1.0);
}
