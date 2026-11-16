// common.glsl - put before EVERY Tube fragment program by tube_shader_assembly.h,
// after the version line, the program's defines and SCENE_HW_DECODE when the
// texture unit decodes the scene. It has no version line of its own.
//
// TUBE_GLSL_TABLE_V1
// (The binary's marker: this comment is in every embedded string the kiosk links,
// and CI's `strings` check on the kiosk binary looks for it. Nothing else names it.)
//
// THE GAMMA METHOD, in one place. The scene texture holds gamma-encoded bytes
// (the kiosk's UI and video draw into it unchanged), so every read of it decodes,
// in the texture unit (SCENE_HW_DECODE) or here, and every write to a target that
// is not sRGB (the default framebuffer) encodes. The sRGB intermediates (quarter,
// glow, lines, history) decode on read and encode on write in hardware, so the
// shaders read and write linear light there and call neither function. Exact
// piecewise sRGB: the cheaper curves the spike priced are not used.

precision highp float;
precision highp int;
precision highp sampler2D;

vec3 decodeGamma(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 encodeGamma(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// THE SCENE AND THE Y FLIP. The kiosk's 2D vertex shader flips Y, so what it
// draws at the top of the screen lands at the top of the scene texture, which in
// GL texture space is HIGH v (see the composite shader's comment in
// renderer_shaders.cpp). Every coordinate in the tube is TOP-DOWN, as in
// crt.metal (`p` and `sp` grow downward). sceneUv maps a top-down 0..1 picture
// coordinate to the scene texture's own uv and clamps it to the picture's
// rectangle inside it (half a texel in), so a tap past the picture's edge reads
// the edge, as Metal's clamp_to_edge on a picture-sized texture does, not the
// pillarbox bars beside it.
uniform sampler2D ui;          // the scene FBO's colour texture
uniform vec4 uSceneMap;        // x: u of the picture's left, y: v of its TOP, z: width in u, w: height in v
uniform vec4 uSceneClamp;      // min u, min v, max u, max v

vec2 sceneUv(vec2 uvTop)
{
    vec2 uv = vec2(uSceneMap.x + uvTop.x * uSceneMap.z, uSceneMap.y - uvTop.y * uSceneMap.w);
    return clamp(uv, uSceneClamp.xy, uSceneClamp.zw);
}

// The scene at a top-down picture uv, in linear light.
// SCENE_HW_DECODE: the scene texture is SRGB8_ALPHA8 (its bytes written
// unencoded under GL_EXT_sRGB_write_control) and sampled with GL_DECODE_EXT, so
// the texture unit decodes BEFORE it filters (linear light, as Metal) and no pow
// runs here. Otherwise the bilinear filter has averaged gamma-encoded texels
// before this decodes: exact at texel centres, slightly dark on sub-pixel edges.
vec3 uiTex(vec2 uvTop)
{
#ifdef SCENE_HW_DECODE
    return texture(ui, sceneUv(uvTop)).rgb;
#else
    return decodeGamma(texture(ui, sceneUv(uvTop)).rgb);
#endif
}

// The intermediates are stored the GL way up: row 0 is the BOTTOM of the
// picture. A top-down uv reads them through this.
vec2 flipV(vec2 uvTop) { return vec2(uvTop.x, 1.0 - uvTop.y); }
